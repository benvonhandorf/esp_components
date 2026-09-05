#ifndef RX8130CE_CALC_H
#define RX8130CE_CALC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * BCD conversion and the register/struct-tm mapping, separated so they can be
 * tested off-target.
 *
 * Two off-by-one conventions meet here -- the RTC counts months from 1 while
 * struct tm counts from 0, and the RTC's year is an offset from 2000 while tm's
 * is from 1900 -- and getting either wrong yields a date that is plausible and
 * wrong, which no amount of staring at a register dump reveals.
 */

uint8_t rx8130ce_bin2bcd(uint8_t value);
uint8_t rx8130ce_bcd2bin(uint8_t value);

/* Seconds, minutes, hours, weekday, day, month, year -- the seven registers as
 * the part lays them out. */
#define RX8130CE_TIME_REGS 7

/* Decode a register block into a broken-down time. Returns false if the values
 * are not a valid date, which is what an unset or backup-drained part returns. */
bool rx8130ce_decode_time(const uint8_t regs[RX8130CE_TIME_REGS], struct tm *out);

/* Encode a broken-down time into a register block. Returns false if the year is
 * outside the 2000-2099 the part can represent. */
bool rx8130ce_encode_time(const struct tm *in, uint8_t regs[RX8130CE_TIME_REGS]);

#endif /* RX8130CE_CALC_H */
