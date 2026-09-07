#ifndef X7_IO_H
#define X7_IO_H

#include <stdint.h>

#ifdef X7_HOST_TEST
uint8_t x7_reg_read(uint16_t address);
void x7_reg_write(uint16_t address, uint8_t value);
void x7_delay(void);
#else
#include <gb/gb.h>
#define x7_reg_read(address) (*(volatile uint8_t *)(address))
#define x7_reg_write(address, value) (*(volatile uint8_t *)(address) = (value))
#define x7_delay() delay(1)
#endif

#define X7_DATA 0xBD00u
#define X7_CTRL 0xBD01u
#define X7_KEY 0xBD0Au

extern uint8_t x7_error;
#endif
