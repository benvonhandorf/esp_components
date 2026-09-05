#include "rx8130ce_calc.h"

#include <string.h>

uint8_t rx8130ce_bin2bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

uint8_t rx8130ce_bcd2bin(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

bool rx8130ce_decode_time(const uint8_t regs[RX8130CE_TIME_REGS], struct tm *out)
{
    if (!regs || !out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    /* The masks drop bits the part does not use for the value; leaving them in
     * turns a flag into decades. */
    out->tm_sec  = rx8130ce_bcd2bin(regs[0] & 0x7F);
    out->tm_min  = rx8130ce_bcd2bin(regs[1] & 0x7F);
    out->tm_hour = rx8130ce_bcd2bin(regs[2] & 0x3F);
    out->tm_wday = rx8130ce_bcd2bin(regs[3] & 0x07);
    out->tm_mday = rx8130ce_bcd2bin(regs[4] & 0x3F);
    out->tm_mon  = rx8130ce_bcd2bin(regs[5] & 0x1F) - 1;   /* RTC 1-12, tm 0-11 */
    out->tm_year = rx8130ce_bcd2bin(regs[6]) + 100;        /* RTC 00-99, tm from 1900 */
    out->tm_isdst = -1;

    /*
     * A part that has lost its backup returns values that decode to nonsense.
     * Rejecting them here means a caller cannot mistake a dead RTC for a device
     * that genuinely believes it is the year 2000, month 0.
     */
    if (out->tm_sec > 59 || out->tm_min > 59 || out->tm_hour > 23 ||
        out->tm_mday < 1 || out->tm_mday > 31 ||
        out->tm_mon < 0 || out->tm_mon > 11 || out->tm_wday > 6) {
        return false;
    }
    return true;
}

bool rx8130ce_encode_time(const struct tm *in, uint8_t regs[RX8130CE_TIME_REGS])
{
    if (!in || !regs) {
        return false;
    }
    /* tm_year is an offset from 1900; the part holds two digits from 2000. */
    if (in->tm_year < 100 || in->tm_year > 199) {
        return false;
    }

    regs[0] = rx8130ce_bin2bcd((uint8_t)in->tm_sec);
    regs[1] = rx8130ce_bin2bcd((uint8_t)in->tm_min);
    regs[2] = rx8130ce_bin2bcd((uint8_t)in->tm_hour);
    regs[3] = rx8130ce_bin2bcd((uint8_t)in->tm_wday);
    regs[4] = rx8130ce_bin2bcd((uint8_t)in->tm_mday);
    regs[5] = rx8130ce_bin2bcd((uint8_t)(in->tm_mon + 1));
    regs[6] = rx8130ce_bin2bcd((uint8_t)(in->tm_year - 100));
    return true;
}
