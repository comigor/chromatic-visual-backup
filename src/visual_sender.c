#include "checksum.h"
#include "pff.h"
#include "visual_grid.h"
#include "x7_io.h"
#include <gb/cgb.h>
#include <gb/gb.h>
#include <gbdk/console.h>
#include <stdio.h>
#include <string.h>

#define PAYLOAD_BYTES VISUAL_PAYLOAD_BYTES
#define MAX_FILE_SIZE 0x1000000UL /* 16 MiB */
#define MANIFEST_INTERVAL 32

#define FRAME_TYPE_MANIFEST 0
#define FRAME_TYPE_DATA 1

static FATFS filesystem;
static DIR directory;
static FILINFO page_entries[10];
static uint8_t page_count, page_more, selection;
static uint16_t page_start;
static char browser_path[256];
static char directory_title[256];
static FILINFO entry;
static uint8_t block[PAYLOAD_BYTES];
static char path[sizeof(browser_path) + 13];
/* Latched file CRC from the prepass; identifies every data frame. */
static uint32_t transfer_crc;
/* Set when B is pressed while visual frames are on screen. */
static uint8_t transmit_aborted;

static void wait_button(uint8_t button) {
  waitpadup();
  waitpad(button);
  waitpadup();
}

static void hex32(uint32_t value) {
  int8_t shift;
  for (shift = 28; shift >= 0; shift -= 4)
    putchar("0123456789ABCDEF"[(uint8_t)(value >> shift) & 15]);
}

static void text_mode(void) {
  visual_grid_end();
  DISPLAY_ON;
}

static void halt_screen(void) {
  printf("\n\nPOWER OFF TO EXIT");
  for (;;)
    vsync();
}

/* Fail loudly: every error path ends here. */
static void fail(const char *stage, uint8_t error) {
  text_mode();
  printf("%s FAILED\nFS ERROR %u\nSD ERROR %u", stage, error, x7_error);
  halt_screen();
}

static void fail_msg(const char *stage, const char *reason) {
  text_mode();
  printf("%s FAILED\n%s", stage, reason);
  halt_screen();
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

static void print_name(const char *name, uint8_t width) {
  while (width--) {
    putchar(*name ? *name++ : ' ');
  }
}

static void show_code(const uint8_t *frame) {
  visual_grid_show(frame);
  if (joypad() & J_B)
    transmit_aborted = 1;
}

static void send_manifest(void) {
  uint8_t frame[VISUAL_FRAME_BYTES];
  uint32_t name_len = 0;
  memset(frame, 0, sizeof(frame));
  frame[0] = 'X';
  frame[1] = '7';
  frame[2] = 'V';
  frame[3] = '2';
  frame[4] = FRAME_TYPE_MANIFEST;
  frame[5] = 0;
  store32(&frame[8], transfer_crc);
  store32(&frame[12], entry.fsize);
  frame[16] = 0xFF;
  frame[17] = 0xFF;
  frame[18] = 0xFF;
  frame[19] = 0xFF;
  while (entry.fname[name_len] && name_len < 12)
    name_len++;
  frame[5] = (uint8_t)name_len;
  memcpy(&frame[20], entry.fname, name_len);
  store32(&frame[VISUAL_CRC_OFFSET], frame_crc(frame) ^ 0xFFFFFFFFUL);
  show_code(frame);
}

static void send_block_data(const uint8_t *payload, uint16_t payload_len,
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
  store32(&frame[8], transfer_crc);
  store32(&frame[12], entry.fsize);
  store32(&frame[16], block_number);
  if (payload_len)
    memcpy(&frame[20], payload, payload_len);
  store32(&frame[VISUAL_CRC_OFFSET], frame_crc(frame) ^ 0xFFFFFFFFUL);
  show_code(frame);
}

/* One transmission pass over the open file. Returns the CRC computed
 * over the bytes actually read so the finish can be checked. */
static uint32_t transmit_pass(void) {
  uint32_t block_number = 0;
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t total = 0;
  transmit_aborted = 0;
  send_manifest();
  while (total < entry.fsize && !transmit_aborted) {
    uint16_t got;
    uint16_t wanted = entry.fsize - total > PAYLOAD_BYTES
                          ? PAYLOAD_BYTES
                          : (uint16_t)(entry.fsize - total);
    FRESULT result = pf_read(block, wanted, &got);
    if (result != FR_OK)
      fail("READ", result);
    if (got != wanted)
      fail_msg("READ", "SHORT FILE");
    crc = checksum_update(crc, block, got);
    if (block_number && block_number % MANIFEST_INTERVAL == 0)
      send_manifest();
    if (transmit_aborted)
      break;
    send_block_data(block, got, block_number);
    total += got;
    block_number++;
  }
  return crc ^ 0xFFFFFFFFUL;
}

/* CRC prepass over the open file; latches transfer_crc. */
static void crc_prepass(void) {
  uint32_t crc = 0xFFFFFFFFUL;
  uint32_t total = 0;
  text_mode();
  printf("CRC ");
  print_name(entry.lfname, 16);
  gotoxy(0, 1);
  if (strlen(entry.lfname) > 16)
    print_name(entry.lfname + 16, 20);
  for (;;) {
    uint16_t got = 0;
    FRESULT result = pf_read(block, PAYLOAD_BYTES, &got);
    if (result != FR_OK)
      fail("READ", result);
    if ((uint32_t)got > filesystem.fsize - total)
      fail_msg("READ", "FILE GREW");
    crc = checksum_update(crc, block, got);
    total += got;
    gotoxy(0, 3);
    printf("BYTES ");
    hex32(total);
    if (!got)
      break;
  }
  if (total != filesystem.fsize)
    fail_msg("CRC", "SHORT FILE");
  transfer_crc = crc ^ 0xFFFFFFFFUL;
  gotoxy(0, 3);
  printf("CRC   ");
  hex32(transfer_crc);
  gotoxy(0, 5);
  printf("A: SEND  B: SKIP");
}

static uint8_t browser_key(void) {
  uint8_t keys;
  waitpadup();
  do {
    vsync();
    keys = joypad();
  } while (!keys);
  return keys;
}

static void load_page(void) {
  const char *leaf;
  uint16_t split = strlen(browser_path);
  strcpy(directory_title, "/");
  if (split) {
    strcpy(path, browser_path);
    while (split && path[split - 1] != '/')
      split--;
    leaf = browser_path + split;
    path[split ? split - 1 : 0] = 0;
    if (pf_opendir(&directory, path) != FR_OK)
      fail_msg("DIRECTORY", "PARENT UNREADABLE");
    for (;;) {
      FRESULT title_result = pf_readdir(&directory, &entry);
      if (title_result != FR_OK)
        fail("DIRECTORY", title_result);
      if (!entry.fname[0])
        fail_msg("DIRECTORY", "NAME NOT FOUND");
      if (!strcmp(entry.fname, leaf)) {
        strcpy(directory_title, entry.lfname);
        break;
      }
    }
  }
  FRESULT result = pf_opendir(&directory, browser_path);
  uint16_t index = 0;
  if (result != FR_OK)
    fail("DIRECTORY", result);
  page_count = 0;
  page_more = 0;
  for (;;) {
    result = pf_readdir(&directory, &entry);
    if (result != FR_OK)
      fail("DIRECTORY", result);
    if (!entry.fname[0])
      break;
    if (entry.fattrib & AM_VOL || !strcmp(entry.fname, ".") ||
        !strcmp(entry.fname, ".."))
      continue;
    if (index++ < page_start)
      continue;
    if (page_count == 10) {
      page_more = 1;
      break;
    }
    page_entries[page_count++] = entry;
  }
}

static void draw_browser(void) {
  uint8_t i;
  cls();
  printf("SD FILES  PAGE %u\n", page_start / 10 + 1);
  print_name(directory_title, 20);
  for (i = 0; i < page_count; i++) {
    gotoxy(0, i + 3);
    putchar(i == selection ? '>' : ' ');
    putchar(page_entries[i].fattrib & AM_DIR ? '+' : ' ');
    putchar(' ');
    print_name(page_entries[i].lfname, 17);
  }
  if (!page_count) {
    gotoxy(0, 3);
    printf("(EMPTY DIRECTORY)");
  }
  gotoxy(0, 14);
  printf("UP/DOWN: SELECT\nLEFT/RIGHT: PAGE\nA: OPEN/SEND B: UP");
}

static uint8_t browser_input(void) {
  uint16_t offset = 0;
  uint16_t length = page_count ? strlen(page_entries[selection].lfname) : 0;
  uint8_t last = (uint8_t)sys_time;
  uint8_t keys;
  waitpadup();
  do {
    vsync();
    keys = joypad();
    if (length > 17 && (uint8_t)((uint8_t)sys_time - last) >= 30) {
      last = (uint8_t)sys_time;
      offset = offset + 17 >= length ? 0 : offset + 1;
      gotoxy(3, selection + 3);
      print_name(page_entries[selection].lfname + offset, 17);
    }
  } while (!keys);
  return keys;
}

static void browser_notice(const char *message) {
  cls();
  printf("%s\n\nB: BACK", message);
  wait_button(J_B);
}

static void send_selected(void) {
  FRESULT result;
  if (entry.fsize > MAX_FILE_SIZE) {
    browser_notice("FILE OVER 16 MIB");
    return;
  }
  result = pf_open(path);
  if (result != FR_OK)
    fail("OPEN", result);
  if (filesystem.fsize != entry.fsize)
    fail_msg("OPEN", "SIZE CHANGED");
  crc_prepass();
  while (1) {
    uint8_t keys = browser_key();
    if (keys & J_B)
      return;
    if (keys & J_A)
      break;
  }
  waitpadup();
  visual_grid_begin();
  for (;;) {
    uint32_t pass_crc;
    result = pf_open(path);
    if (result != FR_OK)
      fail("OPEN", result);
    if (filesystem.fsize != entry.fsize)
      fail_msg("OPEN", "SIZE CHANGED");
    pass_crc = transmit_pass();
    if (transmit_aborted)
      break;
    if (pass_crc != transfer_crc)
      fail_msg("SEND", "CRC MISMATCH");
  }
  waitpadup();
  text_mode();
}

void main(void) {
  FRESULT result;
  if (_cpu != CGB_TYPE) {
    printf("GAME BOY COLOR\nREQUIRED");
    for (;;)
      vsync();
  }
  set_default_palette();
  printf("X7 VISUAL SENDER\n\nSD FILE BROWSER\nSD V2 / FAT32\nREADABLE FILE "
         "NAMES\n"
         "NO SD DATA WRITES\n\nSTART: MOUNT SD\nPOWER OFF: EXIT");
  wait_button(J_START);
  cls();
  printf("MOUNTING SD...");
  result = pf_mount(&filesystem);
  if (result != FR_OK)
    fail("MOUNT", result);
  load_page();
  for (;;) {
    uint8_t keys;
    draw_browser();
    keys = browser_input();
    if (keys & J_B) {
      uint16_t length = strlen(browser_path);
      while (length && browser_path[length - 1] != '/')
        length--;
      browser_path[length ? length - 1 : 0] = 0;
      page_start = 0;
      selection = 0;
      load_page();
    } else if ((keys & J_LEFT) || ((keys & J_UP) && !selection)) {
      if (page_start) {
        page_start -= 10;
        load_page();
        selection = (keys & J_UP) && page_count ? page_count - 1 : 0;
      }
    } else if ((keys & J_RIGHT) ||
               ((keys & J_DOWN) && selection + 1 >= page_count)) {
      if (page_more) {
        page_start += 10;
        selection = 0;
        load_page();
      }
    } else if ((keys & J_UP) && selection) {
      selection--;
    } else if ((keys & J_DOWN) && selection + 1 < page_count) {
      selection++;
    } else if ((keys & J_A) && page_count) {
      entry = page_entries[selection];
      strcpy(path, browser_path);
      strcat(path, "/");
      strcat(path, entry.fname);
      if (entry.fattrib & AM_DIR) {
        if (strlen(path) >= sizeof(browser_path)) {
          browser_notice("PATH OVER 255 BYTES");
          continue;
        }
        strcpy(browser_path, path);
        page_start = 0;
        selection = 0;
        load_page();
      } else {
        send_selected();
      }
    }
  }
}
