#include "digimon_clock.h"

#define CLK_FLAGS  0x018
#define CLK_SEC_LO 0x01A
#define CLK_SEC_HI 0x01B
#define CLK_MIN_LO 0x01C
#define CLK_MIN_HI 0x01D
#define CLK_HOUR   0x01E
#define CLK_PM     0x01F

#define CLK_FLAG_SETTING 0x4
#define CLK_FLAG_UNSET   0x8

/* Emulated seconds before / after each hour boundary when stepping over it */
#define HOP_BEFORE_S 2u
#define HOP_AFTER_S  2u

bool digimon_clock_get(const E0C6200* cpu, uint32_t* sec_of_day) {
    const uint8_t* r = cpu->s.ram;
    if(r[CLK_FLAGS] & (CLK_FLAG_SETTING | CLK_FLAG_UNSET)) return false;
    if(r[CLK_SEC_LO] > 9 || r[CLK_SEC_HI] > 5 || r[CLK_MIN_LO] > 9 || r[CLK_MIN_HI] > 5 ||
       r[CLK_HOUR] < 1 || r[CLK_HOUR] > 12 || r[CLK_PM] > 1)
        return false;
    uint32_t hour = (r[CLK_HOUR] % 12u) + (r[CLK_PM] ? 12u : 0u);
    uint32_t min = r[CLK_MIN_HI] * 10u + r[CLK_MIN_LO];
    uint32_t sec = r[CLK_SEC_HI] * 10u + r[CLK_SEC_LO];
    *sec_of_day = hour * 3600u + min * 60u + sec;
    return true;
}

bool digimon_clock_set(E0C6200* cpu, uint32_t sec_of_day) {
    e0c6200_settle(cpu, DIGIMON_SETTLE_STEPS);
    uint32_t old;
    if(!digimon_clock_get(cpu, &old)) return false;

    sec_of_day %= DIGIMON_DAY_S;
    uint32_t hour = sec_of_day / 3600u;
    uint32_t min = (sec_of_day / 60u) % 60u;
    uint32_t sec = sec_of_day % 60u;
    uint8_t* r = cpu->s.ram;
    r[CLK_SEC_LO] = (uint8_t)(sec % 10u);
    r[CLK_SEC_HI] = (uint8_t)(sec / 10u);
    r[CLK_MIN_LO] = (uint8_t)(min % 10u);
    r[CLK_MIN_HI] = (uint8_t)(min / 10u);
    r[CLK_HOUR] = (uint8_t)(hour % 12u ? hour % 12u : 12u);
    r[CLK_PM] = hour >= 12u ? 1 : 0;
    return true;
}

bool digimon_clock_advance(E0C6200* cpu, uint32_t seconds) {
    e0c6200_settle(cpu, DIGIMON_SETTLE_STEPS);
    uint32_t start;
    if(!digimon_clock_get(cpu, &start)) return false;

    /* Bedtimes repeat daily: only the last day of the gap matters. `pos`
     * is how far along the gap the clock currently is. */
    uint32_t pos = seconds > DIGIMON_DAY_S ? seconds - DIGIMON_DAY_S : 0;
    if(pos) {
        start = (start + pos) % DIGIMON_DAY_S;
        seconds -= pos;
        pos = 0;
        digimon_clock_set(cpu, start);
    }

    /* first hour boundary after the start, as an offset into the gap */
    uint32_t boundary = 3600u - start % 3600u;
    for(; boundary <= seconds; boundary += 3600u) {
        if(boundary > pos + HOP_BEFORE_S) {
            pos = boundary - HOP_BEFORE_S;
            digimon_clock_set(cpu, start + pos);
        }
        uint32_t end = boundary + HOP_AFTER_S;
        if(end > seconds) end = seconds;
        e0c6200_run(cpu, (end - pos) * E0C_OSC1_HZ, 0);
        pos = end;
    }
    if(pos < seconds) digimon_clock_set(cpu, start + seconds);
    return true;
}
