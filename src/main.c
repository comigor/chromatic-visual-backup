#include "checksum.h"
#include "pff.h"
#include "x7_io.h"
#include <gb/cgb.h>
#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include <string.h>

#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)
#define SNAP_PATH STRINGIFY(SNAP_DIRECTORY)

static FATFS filesystem;
static DIR directory;
static FILINFO entry;
static uint8_t buffer[256];
static char path[sizeof(SNAP_PATH) + sizeof(entry.fname)];

static void wait_button(uint8_t button) {
  waitpadup();
  waitpad(button);
  waitpadup();
}

static void hex32(uint32_t value) {
  int8_t shift;
  for (shift = 28; shift >= 0; shift -= 4) {
    putchar("0123456789ABCDEF"[(uint8_t)(value >> shift) & 15]);
  }
}

static void stop(const char *stage, uint8_t error) {
  cls();
  printf("%s FAILED\nFS ERROR %u\nSD ERROR %u\n\nPOWER OFF TO EXIT", stage,
         error, x7_error);
  for (;;)
    vsync();
}

static void hash_file(const char *name) {
  FRESULT result;
  uint16_t got;
  uint32_t crc = 0xFFFFFFFFUL, total = 0;
  gotoxy(0, 3);
  printf("OPENING...");
  result = pf_open(name);
  if (result != FR_OK)
    stop("OPEN", result);
  gotoxy(0, 5);
  printf("BYTES 00000000/\n      ");
  hex32(filesystem.fsize);
  do {
    gotoxy(0, 3);
    printf("READING...");
    result = pf_read(buffer, sizeof(buffer), &got);
    if (result != FR_OK)
      stop("READ", result);
    gotoxy(0, 3);
    printf("HASHING...");
    crc = checksum_update(crc, buffer, got);
    total += got;
    gotoxy(6, 5);
    hex32(total);
  } while (got);
  if (total != filesystem.fsize)
    stop("SIZE", 255);
  gotoxy(0, 3);
  printf("DONE      ");
  gotoxy(0, 8);
  printf("CRC32 ");
  hex32(crc ^ 0xFFFFFFFFUL);
  printf("\n\nA: NEXT");
  wait_button(J_A);
}

void main(void) {
  FRESULT result;
  if (_cpu == CGB_TYPE)
    set_default_palette();
  printf("X7 READ DIAGNOSTIC\n\nX3/X5/X7 ONLY\nSD V2 / FAT32\n\nREADS EXISTING "
         "FILES\nCONTROL REG WRITES\nNO SD DATA WRITES\nNO FIRMWARE "
         "LOAD\n\nSTART: RUN\nPOWER OFF: CANCEL");
  wait_button(J_START);
  cls();
  printf("MOUNTING SD...");
  result = pf_mount(&filesystem);
  if (result != FR_OK)
    stop("MOUNT", result);
  cls();
  printf("GBCSYS/GBCOS.BIN");
  hash_file("GBCSYS/GBCOS.BIN");
  result = pf_opendir(&directory, SNAP_PATH);
  if (result != FR_OK)
    stop("SNAP DIRECTORY", result);
  for (;;) {
    result = pf_readdir(&directory, &entry);
    if (result != FR_OK)
      stop("DIRECTORY", result);
    if (!entry.fname[0])
      break;
    if (entry.fattrib & (AM_DIR | AM_HID | AM_SYS))
      continue;
    strcpy(path, SNAP_PATH "/");
    strcat(path, entry.fname);
    cls();
    printf("SNAPSHOT\n%s", entry.fname);
    hash_file(path);
  }
  cls();
  printf("READS COMPLETE\n\nNO SD DATA WRITTEN\n\nPOWER OFF TO EXIT");
  for (;;)
    vsync();
}
