/* Host test for the RX8130CE's BCD and calendar conversions. Two off-by-one
 * conventions meet here, and getting either wrong yields a date that is
 * plausible and wrong. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "rx8130ce_calc.h"

static int failures;
static void expect(bool c, const char *w)
{ if (!c) { failures++; printf("FAIL: %s\n", w); } else printf("ok  : %s\n", w); }

static void test_bcd(void)
{
    expect(rx8130ce_bin2bcd(0) == 0x00, "0 -> 0x00");
    expect(rx8130ce_bin2bcd(9) == 0x09, "9 -> 0x09");
    expect(rx8130ce_bin2bcd(10) == 0x10, "10 -> 0x10, not 0x0A");
    expect(rx8130ce_bin2bcd(59) == 0x59, "59 -> 0x59");
    expect(rx8130ce_bcd2bin(0x59) == 59, "0x59 -> 59");
    expect(rx8130ce_bcd2bin(0x10) == 10, "0x10 -> 10");
    for (uint8_t i = 0; i <= 99; i++) {
        if (rx8130ce_bcd2bin(rx8130ce_bin2bcd(i)) != i) {
            expect(false, "every value 0-99 survives a BCD round trip");
            return;
        }
    }
    expect(true, "every value 0-99 survives a BCD round trip");
}

static void test_round_trip(void)
{
    struct tm t = {
        .tm_sec = 45, .tm_min = 30, .tm_hour = 13, .tm_wday = 3,
        .tm_mday = 24, .tm_mon = 8 /* September */, .tm_year = 126 /* 2026 */,
    };

    uint8_t regs[RX8130CE_TIME_REGS];
    expect(rx8130ce_encode_time(&t, regs), "a valid time encodes");

    /* The month register holds 1-12, not tm's 0-11. */
    expect(regs[5] == 0x09, "September is written as 9, not 8");
    /* The year register holds two digits from 2000. */
    expect(regs[6] == 0x26, "2026 is written as 0x26");

    struct tm back;
    expect(rx8130ce_decode_time(regs, &back), "and decodes");
    expect(back.tm_sec == t.tm_sec && back.tm_min == t.tm_min &&
           back.tm_hour == t.tm_hour && back.tm_mday == t.tm_mday &&
           back.tm_mon == t.tm_mon && back.tm_year == t.tm_year &&
           back.tm_wday == t.tm_wday, "every field survives the round trip");
}

static void test_masks(void)
{
    /* The top bits of several registers are flags, not part of the value.
     * Leaving them in turns a flag into decades. */
    uint8_t regs[RX8130CE_TIME_REGS] = {0x80 | 0x45, 0x80 | 0x30, 0xC0 | 0x13,
                                        0xF8 | 0x03, 0xC0 | 0x24, 0xE0 | 0x09, 0x26};
    struct tm t;
    expect(rx8130ce_decode_time(regs, &t), "a register block with flags set decodes");
    expect(t.tm_sec == 45 && t.tm_min == 30 && t.tm_hour == 13 &&
           t.tm_mday == 24 && t.tm_mon == 8, "the flag bits are masked off");
}

static void test_rejects_nonsense(void)
{
    /* What a part with a drained backup returns. Accepting it would let a dead
     * RTC pass for a device that believes it is the year 2000. */
    uint8_t all_ones[RX8130CE_TIME_REGS] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    struct tm t;
    expect(!rx8130ce_decode_time(all_ones, &t), "an all-ones register block is rejected");

    uint8_t zero_month[RX8130CE_TIME_REGS] = {0, 0, 0, 0, 0x01, 0x00, 0x26};
    expect(!rx8130ce_decode_time(zero_month, &t), "month 0 is rejected");

    uint8_t zero_day[RX8130CE_TIME_REGS] = {0, 0, 0, 0, 0x00, 0x01, 0x26};
    expect(!rx8130ce_decode_time(zero_day, &t), "day 0 is rejected");
}

static void test_year_range(void)
{
    uint8_t regs[RX8130CE_TIME_REGS];
    struct tm t = {.tm_mday = 1, .tm_mon = 0, .tm_year = 99};   /* 1999 */
    expect(!rx8130ce_encode_time(&t, regs), "a year before 2000 is refused");
    t.tm_year = 200;                                            /* 2100 */
    expect(!rx8130ce_encode_time(&t, regs), "a year after 2099 is refused");
    t.tm_year = 100;                                            /* 2000 */
    expect(rx8130ce_encode_time(&t, regs), "2000 is accepted");
}

int main(void)
{
    test_bcd();
    test_round_trip();
    test_masks();
    test_rejects_nonsense();
    test_year_range();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
