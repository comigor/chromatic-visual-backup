#include "visual_grid.h"
#include <gb/cgb.h>
#include <gb/gb.h>
#include <gbdk/console.h>
#include <gbdk/font.h>

#define FRAME_TICKS 4
static uint8_t cells[20 * 18];
static uint8_t map_b_shown;
static uint8_t last_flip;
static const palette_color_t colors[4] = {RGB(31, 31, 31), RGB(0, 0, 0),
                                          RGB(31, 0, 0), RGB(0, 31, 31)};

static uint8_t ring_color(uint8_t x, uint8_t y) {
  return (x == 0 || x == 39 || y == 0 || y == 35) ? (x + y) & 3 : 0;
}

void visual_grid_begin(void) {
  uint16_t tile, address;
  uint8_t row, x, y;
  uint8_t data[16];
  vsync();
  HIDE_WIN;
  HIDE_SPRITES;
  LCDC_REG &= (uint8_t)~(LCDCF_BG8000 | LCDCF_BG9C00);
  VBK_REG = 1;
  for (address = 0x9800; address < 0xA000; address++)
    set_vram_byte((uint8_t *)address, 0);
  VBK_REG = 0;
  set_bkg_palette(0, 1, colors);
  for (tile = 0; tile < 256; tile++) {
    for (row = 0; row < 8; row++) {
      uint8_t pair = row < 4 ? tile >> 4 : tile & 15;
      uint8_t left = pair >> 2, right = pair & 3;
      data[row * 2] = ((left & 1) ? 0xF0 : 0) | ((right & 1) ? 0x0F : 0);
      data[row * 2 + 1] = ((left & 2) ? 0xF0 : 0) | ((right & 2) ? 0x0F : 0);
    }
    set_bkg_data((uint8_t)tile, 1, data);
  }
  for (y = 0; y < 18; y++)
    for (x = 0; x < 20; x++)
      cells[y * 20 + x] = (ring_color(x * 2, y * 2) << 6) |
                          (ring_color(x * 2 + 1, y * 2) << 4) |
                          (ring_color(x * 2, y * 2 + 1) << 2) |
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
    for (group = 0; group < 9; group++) {
      uint8_t top = frame[group], bottom = frame[group + 9];
      *dest++ = (top & 0xF0) | (bottom >> 4);
      *dest++ = (top << 4) | (bottom & 15);
    }
    dest += 2;
    frame += 18;
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
  set_default_palette();
  font_init();
  font_load(font_ibm);
  cls();
  SHOW_BKG;
}
