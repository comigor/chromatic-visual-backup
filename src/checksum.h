#ifndef CHECKSUM_H
#define CHECKSUM_H
#include <stdint.h>
uint32_t checksum_update(uint32_t crc, const uint8_t *data, uint16_t size);
#endif
