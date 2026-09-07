#include "checksum.h"
#include "visual_grid.h"
#include <gb/cgb.h>
#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_BYTES VISUAL_PAYLOAD_BYTES
#define DEMO_SIZE 1021

#define FRAME_TYPE_MANIFEST 0
#define FRAME_TYPE_DATA 1

static uint8_t block[PAYLOAD_BYTES];

/* Deterministic demo payload: byte(i) = (i*37 + (i >> 8)) & 0xFF. */
static uint8_t demo_byte(uint16_t index) {
  return (uint8_t)((uint32_t)index * 37UL + (index >> 8));
}

static uint32_t frame_crc(const uint8_t *frame) {
  return checksum_update(0xFFFFFFFFUL, frame, VISUAL_CRC_OFFSET);
}

static void store32(uint8_t *dst, uint32_t value) {
  dst[0] = (uint8_t)value;
  dst[1] = (uint8_t)(value >> 8);
  dst[2] = (uint8_t)(value >> 16);
  dst[3] = (uint8_t)(value >> 24);
}

static void show_code(const uint8_t *frame) { visual_grid_show(frame); }

static void send_manifest(uint32_t file_crc, uint32_t file_size) {
  static const char name[] = "DEMO.BIN";
  uint8_t frame[VISUAL_FRAME_BYTES];
  memset(frame, 0, sizeof(frame));
  frame[0] = 'X';
  frame[1] = '7';
  frame[2] = 'V';
  frame[3] = '2';
  frame[4] = FRAME_TYPE_MANIFEST;
  frame[5] = sizeof(name) - 1;
  store32(&frame[8], file_crc);
  store32(&frame[12], file_size);
  frame[16] = 0xFF;
  frame[17] = 0xFF;
  frame[18] = 0xFF;
  frame[19] = 0xFF;
  memcpy(&frame[20], name, sizeof(name) - 1);
  store32(&frame[VISUAL_CRC_OFFSET], frame_crc(frame) ^ 0xFFFFFFFFUL);
  show_code(frame);
}

static void send_block_data(const uint8_t *payload, uint16_t payload_len,
                            uint32_t file_crc, uint32_t file_size,
                            uint32_t block_number) {
  uint8_t frame[VISUAL_FRAME_BYTES];
  memset(frame, 0, sizeof(frame));
  frame[0] = 'X';
  frame[1] = '7';
  frame[2] = 'V';
  frame[3] = '2';
  frame[4] = FRAME_TYPE_DATA;
  frame[5] = (uint8_t)payload_len;
  frame[6] = (uint8_t)(payload_len >> 8);
  store32(&frame[8], file_crc);
  store32(&frame[12], file_size);
  store32(&frame[16], block_number);
  if (payload_len)
    memcpy(&frame[20], payload, payload_len);
  store32(&frame[VISUAL_CRC_OFFSET], frame_crc(frame) ^ 0xFFFFFFFFUL);
  show_code(frame);
}

void main(void) {
  uint32_t file_crc;
  if (_cpu != CGB_TYPE) {
    printf("GAME BOY COLOR\nREQUIRED");
    for (;;)
      vsync();
  }
  set_default_palette();
  printf("VISUAL DEMO\n\nNO SD ACCESS\nNO CART WRITES\n\nDEMO.BIN 1021B\n"
         "15 FPS TARGET\n\nA: TRANSMIT\nPOWER OFF: EXIT");
  waitpadup();
  waitpad(J_A);
  waitpadup();
  /* CRC over the deterministic payload. */
  {
    uint16_t offset = 0;
    file_crc = 0xFFFFFFFFUL;
    while (offset < DEMO_SIZE) {
      uint16_t payload_len = DEMO_SIZE - offset > PAYLOAD_BYTES
                                 ? PAYLOAD_BYTES
                                 : DEMO_SIZE - offset;
      uint16_t index;
      memset(block, 0, PAYLOAD_BYTES);
      for (index = 0; index < payload_len; index++)
        block[index] = demo_byte((uint16_t)(offset + index));
      file_crc = checksum_update(file_crc, block, payload_len);
      offset += payload_len;
    }
    file_crc ^= 0xFFFFFFFFUL;
  }
  visual_grid_begin();
  /* Repeat transmission passes; each pass renumbers blocks from zero so
   * the receiver treats it as a fresh identical file. */
  for (;;) {
    uint32_t block_number = 0;
    uint32_t offset = 0;
    send_manifest(file_crc, DEMO_SIZE);
    while (offset < DEMO_SIZE) {
      uint16_t payload_len = DEMO_SIZE - offset > PAYLOAD_BYTES
                                 ? PAYLOAD_BYTES
                                 : DEMO_SIZE - offset;
      uint16_t index;
      memset(block, 0, PAYLOAD_BYTES);
      for (index = 0; index < payload_len; index++)
        block[index] = demo_byte((uint16_t)(offset + index));
      send_block_data(block, payload_len, file_crc, DEMO_SIZE, block_number);
      block_number++;
      offset += payload_len;
    }
  }
}
