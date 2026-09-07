/* Derived from untoxa/VGM_player, Copyright (c) 2024 Toxa, MIT.
 * See vendor/petitfatfs/DRIVER-LICENSE for the original license. */
#include "diskio.h"
#include "x7_io.h"
#include <string.h>

uint8_t x7_error;
static uint8_t card_ready;
static uint8_t block_addressed;
static uint8_t control;
static uint32_t cached_sector = 0xFFFFFFFFUL;
static uint8_t sector_cache[512];

static void set_control(uint8_t value) {
  control = value;
  x7_reg_write(X7_CTRL, value);
}

static uint8_t transfer(uint8_t value) {
  uint16_t attempts = 4096;
  if (x7_error)
    return 0xFF;
  x7_reg_write(X7_DATA, value);
  while (x7_reg_read(X7_CTRL) & 0x80) {
    if (--attempts == 0) {
      x7_error = 1;
      return 0xFF;
    }
  }
  return x7_reg_read(X7_DATA);
}

static void access_start(void) {
  x7_reg_write(X7_KEY, 0xA5);
  control = x7_reg_read(X7_CTRL) & 0x03;
  set_control(control);
}

static void access_finish(void) {
  set_control(control | 0x01);
  transfer(0xFF);
  x7_reg_write(X7_KEY, 0x00);
}

static uint8_t command(uint8_t cmd, uint32_t arg) {
  uint8_t response = 0xFF;
  uint8_t attempts = 16;
  if (x7_error)
    return response;
  switch (cmd) {
  case 0:
  case 8:
  case 16:
  case 17:
  case 41:
  case 55:
  case 58:
    break;
  default:
    x7_error = 2;
    return response;
  }
  set_control(control | 0x01);
  transfer(0xFF);
  set_control(control & 0xFE);
  transfer(0xFF);
  transfer(0x40 | cmd);
  transfer((uint8_t)(arg >> 24));
  transfer((uint8_t)(arg >> 16));
  transfer((uint8_t)(arg >> 8));
  transfer((uint8_t)arg);
  transfer(cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01);
  do {
    response = transfer(0xFF);
  } while ((response & 0x80) && --attempts && !x7_error);
  return response;
}

DSTATUS disk_initialize(void) {
  uint16_t attempts;
  uint8_t i, response, ocr[4];
  card_ready = block_addressed = x7_error = 0;
  cached_sector = 0xFFFFFFFFUL;
  access_start();
  set_control(0x03);
  for (i = 0; i < 10; i++)
    transfer(0xFF);
  if (command(0, 0) != 1) {
    x7_error = x7_error ? x7_error : 3;
    goto finish;
  }
  if (command(8, 0x1AA) != 1) {
    x7_error = x7_error ? x7_error : 4;
    goto finish;
  }
  for (i = 0; i < 4; i++)
    ocr[i] = transfer(0xFF);
  if (x7_error || ocr[2] != 1 || ocr[3] != 0xAA) {
    x7_error = x7_error ? x7_error : 4;
    goto finish;
  }
  for (attempts = 0; attempts < 1000; attempts++) {
    response = command(55, 0);
    if (response > 1)
      break;
    response = command(41, 0x40000000UL);
    if (response != 1)
      break;
    x7_delay();
  }
  if (response != 0 || attempts == 1000) {
    x7_error = x7_error ? x7_error : 5;
    goto finish;
  }
  if (command(58, 0) != 0) {
    x7_error = x7_error ? x7_error : 6;
    goto finish;
  }
  for (i = 0; i < 4; i++)
    ocr[i] = transfer(0xFF);
  if (x7_error || !(ocr[0] & 0x80)) {
    x7_error = x7_error ? x7_error : 6;
    goto finish;
  }
  block_addressed = (ocr[0] & 0x40) != 0;
  if (!block_addressed && command(16, 512) != 0) {
    x7_error = x7_error ? x7_error : 7;
    goto finish;
  }
  set_control(control & 0xFD);
  card_ready = !x7_error;
finish:
  access_finish();
  if (x7_error)
    card_ready = 0;
  return card_ready ? 0 : STA_NOINIT;
}

DRESULT disk_readp(uint8_t *buffer, uint32_t sector, uint16_t offset,
                   uint16_t count) {
  uint16_t i, attempts;
  uint8_t token = 0xFF;
  uint32_t argument;
  if (!card_ready || x7_error)
    return RES_NOTRDY;
  if (!buffer || offset > 512 || count > 512 - offset)
    return RES_PARERR;
  if (cached_sector != sector) {
    cached_sector = 0xFFFFFFFFUL;
    if (!block_addressed && sector > 0x7FFFFFUL)
      return RES_PARERR;
    argument = block_addressed ? sector : sector * 512UL;
    access_start();
    if (command(17, argument) != 0) {
      if (!x7_error)
        x7_error = 8;
      goto finish;
    }
    for (attempts = 0; attempts < 40000; attempts++) {
      token = transfer(0xFF);
      if (token != 0xFF || x7_error)
        break;
    }
    if (token != 0xFE || x7_error) {
      if (!x7_error)
        x7_error = 9;
      goto finish;
    }
    for (i = 0; i < 512 && !x7_error; i++)
      sector_cache[i] = transfer(0xFF);
    transfer(0xFF);
    transfer(0xFF);
    if (!x7_error)
      cached_sector = sector;
  finish:
    access_finish();
    if (x7_error) {
      cached_sector = 0xFFFFFFFFUL;
      return RES_ERROR;
    }
  }
  memcpy(buffer, sector_cache + offset, count);
  return RES_OK;
}
