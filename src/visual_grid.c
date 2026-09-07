#include "visual_grid.h"
#include <gb/cgb.h>
#include <gb/gb.h>
#include <gbdk/console.h>
#include <gbdk/font.h>

#define TILE_BASE 128
#define FRAME_TICKS 4
static uint8_t cells[20 * 18];
static uint8_t map_b_shown;
static uint8_t last_flip;

static uint8_t ring_color(uint8_t x, uint8_t y) {
  return (x == 0 || x == 39 || y == 0 || y == 35) && !((x + y) & 1);
}

void visual_grid_begin(void) {
  uint8_t tile, row, x, y;
  uint8_t data[16];
  vsync();
  HIDE_WIN;
  HIDE_SPRITES;
  LCDC_REG &= (uint8_t)~(LCDCF_BG8000 | LCDCF_BG9C00);
  if (_cpu == CGB_TYPE) {
    uint16_t address;
    VBK_REG = 1;
    for (address = 0x9800; address < 0xA000; address++)
      set_vram_byte((uint8_t *)address, 0);
    VBK_REG = 0;
    set_default_palette();
  }
  BGP_REG = 0xE4;
  for (tile = 0; tile < 16; tile++) {
    for (row = 0; row < 8; row++) {
      uint8_t left = row < 4 ? 8 : 2;
      uint8_t right = row < 4 ? 4 : 1;
      uint8_t bits = ((tile & left) ? 0xF0 : 0) | ((tile & right) ? 0x0F : 0);
      data[row * 2] = bits;
      data[row * 2 + 1] = bits;
    }
    set_bkg_data(TILE_BASE + tile, 1, data);
  }
  for (y = 0; y < 18; y++)
    for (x = 0; x < 20; x++)
      cells[y * 20 + x] = TILE_BASE | (ring_color(x * 2, y * 2) << 3) |
                          (ring_color(x * 2 + 1, y * 2) << 2) |
                          (ring_color(x * 2, y * 2 + 1) << 1) |
                          ring_color(x * 2 + 1, y * 2 + 1);
  move_bkg(0, 0);
  map_b_shown = 0;
  last_flip = (uint8_t)sys_time;
  SHOW_BKG;
}

void visual_grid_show(const uint8_t *frame) {
  uint8_t row, group;
  uint8_t *dest = cells + 21;
  for (row = 0; row < 16; row++) {
    for (group = 0; group < 4; group++) {
      uint8_t top = frame[group];
      uint8_t bottom = (frame[group + 4] << 4) | (frame[group + 5] >> 4);
      *dest++ = TILE_BASE | ((top >> 4) & 12) | (bottom >> 6);
      *dest++ = TILE_BASE | ((top >> 2) & 12) | ((bottom >> 4) & 3);
      *dest++ = TILE_BASE | (top & 12) | ((bottom >> 2) & 3);
      *dest++ = TILE_BASE | ((top << 2) & 12) | (bottom & 3);
    }
    *dest++ = TILE_BASE | ((frame[4] >> 4) & 12) | ((frame[8] >> 2) & 3);
    *dest++ = TILE_BASE | ((frame[4] >> 2) & 12) | (frame[8] & 3);
    dest += 2;
    frame += 9;
  }
  set_tiles(0, 0, 20, 18, (uint8_t *)(map_b_shown ? 0x9800 : 0x9C00), cells);
  do {
    vsync();
  } while ((uint8_t)((uint8_t)sys_time - last_flip) < FRAME_TICKS);
  LCDC_REG ^= LCDCF_BG9C00;
  map_b_shown ^= 1;
  last_flip = (uint8_t)sys_time;
}

void visual_grid_end(void) {
  vsync();
  LCDC_REG &= (uint8_t)~LCDCF_BG9C00;
  map_b_shown = 0;
  font_init();
  font_load(font_ibm);
  cls();
  SHOW_BKG;
}
