#ifndef VISUAL_GRID_H
#define VISUAL_GRID_H

#include <stdint.h>
#define VISUAL_FRAME_BYTES 288
#define VISUAL_PAYLOAD_BYTES 264
#define VISUAL_CRC_OFFSET 284

void visual_grid_begin(void);
void visual_grid_show(const uint8_t *frame);
void visual_grid_end(void);

#endif
