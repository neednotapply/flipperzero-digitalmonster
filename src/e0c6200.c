/*
 * Epson E0C6200 core. See e0c6200.h.
 *
 * This is a faithful port of BrickEmuPy's cores/E0C6200.py (azya52).
 * Instruction semantics, cycle counts and register quirks intentionally
 * match the reference so that ROMs that work there work here.
 */
#include "e0c6200.h"

#include <string.h>

#define VRAM_PART1_OFFSET 0xE00
#define VRAM_PART2_OFFSET 0xE80
#define VRAM_PART_SIZE    0x050
#define IORAM_OFFSET      0xF00
#define IORAM_SIZE        0x07F

#define TIMER_DIV     128 /* OSC1 / 256 Hz */
#define STOPWATCH_DIV 32768 /* OSC1 / 100 Hz, in 1/100 ticks */

#define IO_IT1  8
#define IO_IT2  4
#define IO_IT8  2
#define IO_IT32 1
#define IO_ISW0 2
#define IO_ISW1 1
#define IO_IPT  1
#define IO_IK0  1
#define IO_IK1  1

#define IO_TM2 4
#define IO_TM4 1
#define IO_TM6 4
#define IO_TM7 8

#define IO_R33 8
#define IO_R43 8

#define IO_CLKCHG 8
#define IO_ALOFF  8
#define IO_ALON   4
#define IO_SVDDT  8

#define IO_SHOTPW 8
#define IO_BZFQ   7
#define IO_BZSHOT 8
#define IO_ENVRST 4
#define IO_ENVRT  2
#define IO_ENVON  1
#define IO_TMRST  2
#define IO_SWRST  2
#define IO_SWRUN  1
#define IO_PTRST  2
#define IO_PTRUN  1
#define IO_PTCOUT 8
#define IO_PTC    7

static const int32_t ptimer_div[8] = {0, 0, 128, 64, 32, 16, 8, 4};

/* OSC1 / {8, 10, 12, 14, 16, 20, 24, 28} */
const uint16_t e0c6200_buzzer_hz[8] = {4096, 3277, 2731, 2341, 2048, 1638, 1365, 1170};

#define SOUND_CLOCK_DIV 128
static const int32_t one_shot_div[2] = {8 * SOUND_CLOCK_DIV, 16 * SOUND_CLOCK_DIV};
static const int32_t envelope_cycle_div[2] = {16 * SOUND_CLOCK_DIV, 32 * SOUND_CLOCK_DIV};

/* ------------------------------------------------------------------ */
/* Buzzer                                                              */
/* ------------------------------------------------------------------ */

static void snd_emit_tone(E0C6200State* s) {
    s->audio_on = 1;
    s->audio_freq = s->snd_freq;
    s->audio_level = s->snd_env_step;
}

static void snd_emit_off(E0C6200State* s) {
    s->audio_on = 0;
}

static void snd_reset(E0C6200State* s) {
    s->snd_one_shot = 0;
    s->snd_env_counter = 0;
    s->snd_env_cycle = envelope_cycle_div[0];
    s->snd_freq = 0;
    s->snd_env_step = 7;
    s->snd_env_on = 0;
    s->snd_on = 0;
    s->audio_on = 0;
    s->audio_freq = 0;
    s->audio_level = 7;
}

static inline void snd_clock(E0C6200State* s) {
    if(s->snd_one_shot > 0) {
        if(--s->snd_one_shot <= 0) snd_emit_off(s);
    }
    if(s->snd_env_counter > 0) {
        if(--s->snd_env_counter <= 0) {
            if(s->snd_env_step > 0) {
                snd_emit_tone(s);
                s->snd_env_step--;
            }
            s->snd_env_counter = s->snd_env_cycle;
        }
    }
}

static void snd_set_freq(E0C6200State* s, uint8_t value) {
    s->snd_freq = value & 7;
    if(s->snd_on) snd_emit_tone(s);
}

static void snd_set_envelope_on(E0C6200State* s) {
    s->snd_env_on = 1;
    s->snd_env_step = 7;
}

static void snd_set_envelope_off(E0C6200State* s) {
    s->snd_env_on = 0;
    s->snd_env_step = 7;
    s->snd_env_counter = 0;
    snd_emit_off(s);
}

static void snd_one_shot(E0C6200State* s, int duration) {
    if(s->snd_one_shot == 0) {
        s->snd_one_shot = one_shot_div[duration];
        if(!s->snd_on) snd_emit_tone(s);
    }
}

static void snd_set_buzzer_on(E0C6200State* s) {
    s->snd_on = 1;
    s->snd_one_shot = 0;
    snd_emit_tone(s);
    if(s->snd_env_on) s->snd_env_counter = s->snd_env_cycle;
}

static void snd_set_buzzer_off(E0C6200State* s) {
    s->snd_on = 0;
    s->snd_env_counter = 0;
    s->snd_one_shot = 0;
    snd_emit_off(s);
}

/* ------------------------------------------------------------------ */
/* Ports                                                               */
/* ------------------------------------------------------------------ */

static inline uint8_t port_read(const E0C6200State* s, int port) {
    uint8_t pull = s->pullup_ext[port] | s->pullup[port];
    uint8_t dir = s->pdir[port];
    uint8_t in = s->in_high[port] | (pull & (uint8_t)~s->in_low[port]);
    uint8_t out = s->platch[port] & (pull | s->pushpull[port]);
    return (uint8_t)(((~dir & in) | (dir & out)) & 0xF);
}

uint8_t e0c6200_port_read(const E0C6200* cpu, E0CPort port) {
    return port_read(&cpu->s, port);
}

/* ------------------------------------------------------------------ */
/* Init / reset                                                        */
/* ------------------------------------------------------------------ */

static void init_registers(E0C6200State* s) {
    s->a = s->b = 0;
    s->ix = s->iy = 0;
    s->sp = 0;
    s->pc = 0x100;
    s->npc = 0x100;
    s->cf = s->zf = s->df = s->iflag = 0;
    memset(s->ram, 0, sizeof(s->ram));
    memset(s->vram, 0, sizeof(s->vram));
    s->halt = 0;

    for(int p = 0; p < E0C_PORT_COUNT; p++) {
        bool is_r = p >= E0CPortR0 && p <= E0CPortR4;
        bool is_p = p >= E0CPortP0;
        s->pdir[p] = is_r ? 0xF : 0;
        s->pushpull[p] = is_p ? 0xF : 0;
        s->platch[p] = 0;
        s->pullup[p] = is_p ? 0xF : 0;
    }

    s->it = s->isw = s->ipt = s->isio = s->ik0 = s->ik1 = 0;
    s->eit = s->eisw = s->eipt = s->eisio = s->eik0 = s->eik1 = 0;
    s->tm = 0;
    s->swl = s->swh = 0;
    s->pt = s->rd = s->sd = 0;
    s->dfk0 = 0xF;
    s->ctrl_osc = 0;
    s->ctrl_lcd = IO_ALOFF;
    s->lc = 0;
    s->ctrl_svd = IO_SVDDT;
    s->ctrl_bz1 = s->ctrl_bz2 = 0;
    s->ctrl_sw = s->ctrl_pt = 0;
    s->ptc = s->sc = s->hzr = s->ioc = s->pup = 0;
}

static void do_reset(E0C6200State* s) {
    init_registers(s);
    s->osc1_counter = 0;
    s->timer_counter = 0;
    s->stopwatch_counter = 0;
    snd_set_buzzer_off(s);
    snd_set_envelope_off(s);
}

void e0c6200_init(E0C6200* cpu, const uint16_t* rom, uint32_t osc3_hz, const E0C6200Mask* mask) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->rom = rom;
    cpu->osc3_hz = osc3_hz;
    E0C6200State* s = &cpu->s;
    if(mask) memcpy(s->pullup_ext, mask->pullup_ext, sizeof(s->pullup_ext));
    init_registers(s);
    snd_reset(s);
    s->osc1_counter = 0;
    s->timer_counter = 0;
    s->stopwatch_counter = 0;
    s->ptimer_counter = 0;
    s->if_delay = 0;
    s->reset_held = 0;
}

void e0c6200_reset(E0C6200* cpu) {
    do_reset(&cpu->s);
    cpu->s.reset_held = 0;
}

void e0c6200_set_reset_pin(E0C6200* cpu, bool held) {
    if(held) {
        do_reset(&cpu->s);
        cpu->s.reset_held = 1;
    } else {
        cpu->s.reset_held = 0;
    }
}

E0CLcdMode e0c6200_lcd_mode(const E0C6200* cpu) {
    const E0C6200State* s = &cpu->s;
    if((s->ctrl_lcd & IO_ALOFF) || s->reset_held) return E0CLcdBlank;
    if(s->ctrl_lcd & IO_ALON) return E0CLcdAllOn;
    return E0CLcdNormal;
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

static void process_ptimer(E0C6200State* s) {
    s->pt = (uint8_t)(s->pt - 1);
    if(s->pt == 0) {
        s->pt = s->rd;
        s->ipt |= IO_IPT;
    }
    if(s->ptc & IO_PTCOUT) s->platch[E0CPortR3] ^= IO_R33;
}

static void process_stopwatch(E0C6200State* s) {
    if(s->ctrl_sw & IO_SWRUN) {
        s->swl = (s->swl + 1) % 10;
        if(s->swl == 0) {
            s->swh = (s->swh + 1) % 10;
            s->isw |= IO_ISW1;
            if(s->swh == 0) s->isw |= IO_ISW0;
        }
    }
}

static void process_timer(E0C6200State* s) {
    uint8_t old_tm = s->tm;
    uint8_t new_tm = (uint8_t)(old_tm + 1);
    if((new_tm & IO_TM2) < (old_tm & IO_TM2)) s->it |= IO_IT32;
    if(((new_tm >> 4) & IO_TM4) < ((old_tm >> 4) & IO_TM4)) s->it |= IO_IT8;
    if(((new_tm >> 4) & IO_TM6) < ((old_tm >> 4) & IO_TM6)) s->it |= IO_IT2;
    if(((new_tm >> 4) & IO_TM7) < ((old_tm >> 4) & IO_TM7)) s->it |= IO_IT1;
    s->tm = new_tm;
}

static inline void clock_osc1(E0C6200State* s) {
    s->osc1_ticks++;
    snd_clock(s);

    if((s->ptc & IO_PTC) > 1) {
        if(--s->ptimer_counter <= 0) {
            s->ptimer_counter += ptimer_div[s->ptc & IO_PTC];
            process_ptimer(s);
        }
    }

    s->stopwatch_counter -= 100;
    if(s->stopwatch_counter <= 0) {
        s->stopwatch_counter += STOPWATCH_DIV;
        process_stopwatch(s);
    }

    if(--s->timer_counter <= 0) {
        s->timer_counter += TIMER_DIV;
        process_timer(s);
    }
}

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */

static uint8_t io_read(E0C6200State* s, uint16_t addr) {
    uint8_t ret;
    switch(addr) {
    case 0xF00:
        ret = s->it;
        s->it = 0;
        return ret;
    case 0xF01:
        ret = s->isw;
        s->isw = 0;
        return ret;
    case 0xF02:
        ret = s->ipt;
        s->ipt = 0;
        return ret;
    case 0xF03:
        ret = s->isio;
        s->isio = 0;
        return ret;
    case 0xF04:
        ret = s->ik0;
        s->ik0 = 0;
        return ret;
    case 0xF05:
        ret = s->ik1;
        s->ik1 = 0;
        return ret;
    case 0xF10:
        return s->eit;
    case 0xF11:
        return s->eisw;
    case 0xF12:
        return s->eipt;
    case 0xF13:
        return s->eisio;
    case 0xF14:
        return s->eik0;
    case 0xF15:
        return s->eik1;
    case 0xF20:
        return s->tm & 0xF;
    case 0xF21:
        return (s->tm >> 4) & 0xF;
    case 0xF22:
        return s->swl & 0xF;
    case 0xF23:
        return s->swh & 0xF;
    case 0xF24:
        return s->pt & 0xF;
    case 0xF25:
        return (s->pt >> 4) & 0xF;
    case 0xF26:
        return s->rd & 0xF;
    case 0xF27:
        return (s->rd >> 4) & 0xF;
    case 0xF30:
        return s->sd & 0xF;
    case 0xF31:
        return (s->sd >> 4) & 0xF;
    case 0xF40:
        return port_read(s, E0CPortK0);
    case 0xF41:
        return s->dfk0;
    case 0xF42:
        return port_read(s, E0CPortK1);
    case 0xF50:
    case 0xF51:
    case 0xF52:
    case 0xF53:
    case 0xF54:
        return s->platch[E0CPortR0 + (addr - 0xF50)];
    case 0xF60:
    case 0xF61:
    case 0xF62:
    case 0xF63:
        return port_read(s, E0CPortP0 + (addr - 0xF60));
    case 0xF70:
        return s->ctrl_osc;
    case 0xF71:
        return s->ctrl_lcd;
    case 0xF72:
        return s->lc;
    case 0xF73:
        return 0;
    case 0xF74:
        return s->ctrl_bz1;
    case 0xF75:
        return (s->ctrl_bz2 & (IO_ENVRT | IO_ENVON)) | (s->snd_one_shot > 0 ? IO_BZSHOT : 0);
    case 0xF77:
        return s->ctrl_sw & IO_SWRUN;
    case 0xF78:
        return s->ctrl_pt & IO_PTRUN;
    case 0xF79:
        return s->ptc;
    case 0xF7D:
        return s->ioc;
    case 0xF7E:
        return s->pup;
    default:
        return 0;
    }
}

static void io_write(E0C6200State* s, uint16_t addr, uint8_t value) {
    switch(addr) {
    case 0xF10:
        s->eit = value;
        break;
    case 0xF11:
        s->eisw = value & 0x3;
        break;
    case 0xF12:
        s->eipt = value & 0x1;
        break;
    case 0xF13:
        s->eisio = value & 0x1;
        break;
    case 0xF14:
        s->eik0 = value;
        break;
    case 0xF15:
        s->eik1 = value;
        break;
    case 0xF26:
        s->rd = (s->rd & 0xF0) | (value & 0x0F);
        break;
    case 0xF27:
        s->rd = (s->rd & 0x0F) | ((value << 4) & 0xF0);
        break;
    case 0xF30:
        s->sd = (s->sd & 0xF0) | (value & 0x0F);
        break;
    case 0xF31:
        s->sd = (s->sd & 0x0F) | ((value << 4) & 0xF0);
        break;
    case 0xF41:
        s->dfk0 = value;
        break;
    case 0xF50:
    case 0xF51:
    case 0xF52:
    case 0xF53:
        s->platch[E0CPortR0 + (addr - 0xF50)] = value;
        break;
    case 0xF54:
        s->platch[E0CPortR4] = value;
        if(value & IO_R43)
            snd_set_buzzer_off(s);
        else
            snd_set_buzzer_on(s);
        break;
    case 0xF60:
    case 0xF61:
    case 0xF62:
    case 0xF63:
        s->platch[E0CPortP0 + (addr - 0xF60)] = value;
        break;
    case 0xF70:
        s->ctrl_osc = value;
        break;
    case 0xF71:
        s->ctrl_lcd = value;
        break;
    case 0xF72:
        s->lc = value;
        break;
    case 0xF74:
        s->ctrl_bz1 = value;
        snd_set_freq(s, s->ctrl_bz1 & IO_BZFQ);
        break;
    case 0xF75:
        s->ctrl_bz2 = value & (IO_ENVRT | IO_ENVON);
        s->snd_env_cycle = envelope_cycle_div[(value & IO_ENVRT) ? 1 : 0];
        if(value & IO_BZSHOT) snd_one_shot(s, (s->ctrl_bz1 & IO_SHOTPW) ? 1 : 0);
        if(value & IO_ENVON)
            snd_set_envelope_on(s);
        else
            snd_set_envelope_off(s);
        if(value & IO_ENVRST) s->snd_env_step = 7;
        break;
    case 0xF76:
        if(value & IO_TMRST) s->tm = 0;
        break;
    case 0xF77:
        if(value & IO_SWRST) s->swl = s->swh = 0;
        s->ctrl_sw = value & IO_SWRUN;
        break;
    case 0xF78:
        if(value & IO_PTRST) s->pt = s->rd;
        s->ctrl_pt = value & IO_PTRUN;
        break;
    case 0xF79:
        s->ptc = value;
        break;
    case 0xF7D:
        s->ioc = value;
        for(int i = 0; i < 4; i++)
            s->pdir[E0CPortP0 + i] = (value & (1 << i)) ? 0xF : 0;
        break;
    case 0xF7E:
        s->pup = value;
        for(int i = 0; i < 4; i++)
            s->pullup[E0CPortP0 + i] = (value & (1 << i)) ? 0 : 0xF;
        break;
    default:
        break;
    }
}

static inline uint8_t get_mem(E0C6200State* s, uint16_t addr) {
    if(addr < E0C_RAM_SIZE) return s->ram[addr];
    if(addr >= VRAM_PART1_OFFSET && addr < VRAM_PART1_OFFSET + VRAM_PART_SIZE)
        return s->vram[addr - VRAM_PART1_OFFSET];
    if(addr >= VRAM_PART2_OFFSET && addr < VRAM_PART2_OFFSET + VRAM_PART_SIZE)
        return s->vram[addr - VRAM_PART2_OFFSET + VRAM_PART_SIZE];
    if(addr >= IORAM_OFFSET && addr < IORAM_OFFSET + IORAM_SIZE) return io_read(s, addr);
    return 0;
}

static inline void set_mem(E0C6200State* s, uint16_t addr, uint8_t value) {
    if(addr < E0C_RAM_SIZE)
        s->ram[addr] = value & 0xF;
    else if(addr >= VRAM_PART1_OFFSET && addr < VRAM_PART1_OFFSET + VRAM_PART_SIZE)
        s->vram[addr - VRAM_PART1_OFFSET] = value & 0xF;
    else if(addr >= VRAM_PART2_OFFSET && addr < VRAM_PART2_OFFSET + VRAM_PART_SIZE)
        s->vram[addr - VRAM_PART2_OFFSET + VRAM_PART_SIZE] = value & 0xF;
    else if(addr >= IORAM_OFFSET && addr < IORAM_OFFSET + IORAM_SIZE)
        io_write(s, addr, value);
}

/* r/q operand: 0 = A, 1 = B, 2 = M(X), 3 = M(Y) */
static inline uint8_t get_r(E0C6200State* s, unsigned r) {
    switch(r & 3) {
    case 0:
        return s->a;
    case 1:
        return s->b;
    case 2:
        return get_mem(s, s->ix);
    default:
        return get_mem(s, s->iy);
    }
}

static inline void set_r(E0C6200State* s, unsigned r, uint8_t value) {
    switch(r & 3) {
    case 0:
        s->a = value & 0xF;
        break;
    case 1:
        s->b = value & 0xF;
        break;
    case 2:
        set_mem(s, s->ix, value);
        break;
    default:
        set_mem(s, s->iy, value);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Pins                                                                */
/* ------------------------------------------------------------------ */

void e0c6200_set_pin(E0C6200* cpu, E0CPort port, uint8_t mask, int level) {
    E0C6200State* s = &cpu->s;
    uint8_t prev_state = port_read(s, port) & mask;
    s->in_low[port] &= (uint8_t)~mask;
    s->in_high[port] &= (uint8_t)~mask;
    if(level == 0)
        s->in_low[port] |= mask;
    else if(level > 0)
        s->in_high[port] |= mask;

    uint8_t new_state = port_read(s, port) & mask;
    if(port == E0CPortK0) {
        if((prev_state & s->eik0) != (new_state & s->eik0)) {
            if((s->dfk0 & mask) != new_state) {
                s->ik0 |= IO_IK0;
                if(mask & 0x8) process_ptimer(s);
            }
        }
    } else if(port == E0CPortK1) {
        if((prev_state & s->eik1) != (new_state & s->eik1)) {
            if(mask != new_state) s->ik1 |= IO_IK1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Instruction execution                                               */
/* ------------------------------------------------------------------ */

#define NEXT_PC() (s->pc = s->npc = (s->pc & 0x1000) | ((s->pc + 1) & 0xFFF))

static uint32_t execute(E0C6200State* s, uint16_t op) {
    unsigned r, q;
    int res;
    uint8_t v;

    switch(op >> 8) {
    case 0x0: /* JP s */
        s->pc = (s->npc & 0x1F00) | (op & 0xFF);
        return 5;

    case 0x1: /* RETD l */
        s->pc = s->npc = (s->pc & 0x1000) | (s->ram[s->sp + 2] << 8) | (s->ram[s->sp + 1] << 4) |
                         s->ram[s->sp];
        s->sp = (uint8_t)(s->sp + 3);
        set_mem(s, s->ix, op & 0xF);
        set_mem(s, (s->ix & 0xF00) | ((s->ix + 1) & 0xFF), (op >> 4) & 0xF);
        s->ix = (s->ix & 0xF00) | ((s->ix + 2) & 0xFF);
        return 12;

    case 0x2: /* JP C,s */
        if(s->cf)
            s->pc = (s->npc & 0x1F00) | (op & 0xFF);
        else
            NEXT_PC();
        return 5;

    case 0x3: /* JP NC,s */
        if(!s->cf)
            s->pc = (s->npc & 0x1F00) | (op & 0xFF);
        else
            NEXT_PC();
        return 5;

    case 0x4: /* CALL s */
        set_mem(s, (uint8_t)(s->sp - 1), ((s->pc + 1) >> 8) & 0xF);
        set_mem(s, (uint8_t)(s->sp - 2), ((s->pc + 1) >> 4) & 0xF);
        s->sp = (uint8_t)(s->sp - 3);
        set_mem(s, s->sp, (s->pc + 1) & 0xF);
        s->pc = (s->npc & 0x1F00) | (op & 0xFF);
        return 7;

    case 0x5: /* CALZ s */
        set_mem(s, (uint8_t)(s->sp - 1), ((s->pc + 1) >> 8) & 0xF);
        set_mem(s, (uint8_t)(s->sp - 2), ((s->pc + 1) >> 4) & 0xF);
        s->sp = (uint8_t)(s->sp - 3);
        set_mem(s, s->sp, (s->pc + 1) & 0xF);
        s->pc = s->npc = (s->npc & 0x1000) | (op & 0xFF);
        return 7;

    case 0x6: /* JP Z,s */
        if(s->zf)
            s->pc = (s->npc & 0x1F00) | (op & 0xFF);
        else
            NEXT_PC();
        return 5;

    case 0x7: /* JP NZ,s */
        if(!s->zf)
            s->pc = (s->npc & 0x1F00) | (op & 0xFF);
        else
            NEXT_PC();
        return 5;

    case 0x8: /* LD Y,y */
        s->iy = (s->iy & 0xF00) | (op & 0xFF);
        NEXT_PC();
        return 5;

    case 0x9: /* LBPX MX,l */
        set_mem(s, s->ix, op & 0xF);
        set_mem(s, (s->ix & 0xF00) | ((s->ix + 1) & 0xFF), (op >> 4) & 0xF);
        s->ix = (s->ix & 0xF00) | ((s->ix + 2) & 0xFF);
        NEXT_PC();
        return 5;

    case 0xA:
        switch((op >> 4) & 0xF) {
        case 0x0: /* ADC XH,i */
            res = ((s->ix >> 4) & 0xF) + (op & 0xF) + s->cf;
            s->zf = (res & 0xF) == 0;
            s->cf = res > 15;
            s->ix = (s->ix & 0xF0F) | ((res << 4) & 0x0F0);
            break;
        case 0x1: /* ADC XL,i */
            res = (s->ix & 0xF) + (op & 0xF) + s->cf;
            s->zf = (res & 0xF) == 0;
            s->cf = res > 15;
            s->ix = (s->ix & 0xFF0) | (res & 0xF);
            break;
        case 0x2: /* ADC YH,i */
            res = ((s->iy >> 4) & 0xF) + (op & 0xF) + s->cf;
            s->zf = (res & 0xF) == 0;
            s->cf = res > 15;
            s->iy = (s->iy & 0xF0F) | ((res << 4) & 0x0F0);
            break;
        case 0x3: /* ADC YL,i */
            res = (s->iy & 0xF) + (op & 0xF) + s->cf;
            s->zf = (res & 0xF) == 0;
            s->cf = res > 15;
            s->iy = (s->iy & 0xFF0) | (res & 0xF);
            break;
        case 0x4: /* CP XH,i */
            res = ((s->ix >> 4) & 0xF) - (op & 0xF);
            s->zf = res == 0;
            s->cf = res < 0;
            break;
        case 0x5: /* CP XL,i */
            res = (s->ix & 0xF) - (op & 0xF);
            s->zf = res == 0;
            s->cf = res < 0;
            break;
        case 0x6: /* CP YH,i */
            res = ((s->iy >> 4) & 0xF) - (op & 0xF);
            s->zf = res == 0;
            s->cf = res < 0;
            break;
        case 0x7: /* CP YL,i */
            res = (s->iy & 0xF) - (op & 0xF);
            s->zf = res == 0;
            s->cf = res < 0;
            break;
        case 0x8: /* ADD r,q */
        case 0x9: /* ADC r,q */
            r = (op >> 2) & 3;
            q = op & 3;
            res = get_r(s, r);
            res += get_r(s, q);
            if(((op >> 4) & 0xF) == 0x9) res += s->cf;
            s->cf = res > 15;
            if(s->df && res > 9) {
                res += 6;
                s->cf = 1;
            }
            s->zf = (res & 0xF) == 0;
            set_r(s, r, res & 0xF);
            break;
        case 0xA: /* SUB r,q */
        case 0xB: /* SBC r,q */
            r = (op >> 2) & 3;
            q = op & 3;
            res = get_r(s, r);
            res -= get_r(s, q);
            if(((op >> 4) & 0xF) == 0xB) res -= s->cf;
            s->cf = res < 0;
            if(s->df && res < 0) res += 10;
            s->zf = (res & 0xF) == 0;
            set_r(s, r, res & 0xF);
            break;
        case 0xC: /* AND r,q */
            r = (op >> 2) & 3;
            q = op & 3;
            v = get_r(s, r);
            v &= get_r(s, q);
            s->zf = v == 0;
            set_r(s, r, v);
            break;
        case 0xD: /* OR r,q */
            r = (op >> 2) & 3;
            q = op & 3;
            v = get_r(s, r);
            v |= get_r(s, q);
            s->zf = v == 0;
            set_r(s, r, v);
            break;
        case 0xE: /* XOR r,q */
            r = (op >> 2) & 3;
            q = op & 3;
            v = get_r(s, r);
            v ^= get_r(s, q);
            s->zf = v == 0;
            set_r(s, r, v);
            break;
        default: /* RLC r */
            r = op & 3;
            res = (get_r(s, r) << 1) + s->cf;
            s->cf = res > 15;
            set_r(s, r, res & 0xF);
            break;
        }
        NEXT_PC();
        return 7;

    case 0xB: /* LD X,x */
        s->ix = (s->ix & 0xF00) | (op & 0xFF);
        NEXT_PC();
        return 5;

    case 0xC:
    case 0xD:
        r = (op >> 4) & 3;
        switch((op >> 6) & 7) {
        case 0: /* ADD r,i */
        case 1: /* ADC r,i */
            res = get_r(s, r) + (op & 0xF);
            if((op >> 6) & 1) res += s->cf;
            s->cf = res > 15;
            if(s->df && res > 9) {
                res += 6;
                s->cf = 1;
            }
            s->zf = (res & 0xF) == 0;
            set_r(s, r, res & 0xF);
            break;
        case 2: /* AND r,i */
            v = get_r(s, r) & op & 0xF;
            s->zf = v == 0;
            set_r(s, r, v);
            break;
        case 3: /* OR r,i */
            v = (get_r(s, r) | op) & 0xF;
            s->zf = v == 0;
            set_r(s, r, v);
            break;
        case 4: /* XOR r,i */
            v = (get_r(s, r) ^ op) & 0xF;
            s->zf = v == 0;
            set_r(s, r, v);
            break;
        case 5: /* SBC r,i */
            res = get_r(s, r) - (op & 0xF) - s->cf;
            s->cf = res < 0;
            if(s->df && s->cf) res += 10;
            s->zf = (res & 0xF) == 0;
            set_r(s, r, res & 0xF);
            break;
        case 6: /* FAN r,i */
            s->zf = (get_r(s, r) & op & 0xF) == 0;
            break;
        default: /* CP r,i */
            res = get_r(s, r) - (op & 0xF);
            s->zf = res == 0;
            s->cf = res < 0;
            break;
        }
        NEXT_PC();
        return 7;

    case 0xE:
        if(op < 0xE40) { /* LD r,i */
            set_r(s, (op >> 4) & 3, op & 0xF);
            NEXT_PC();
            return 5;
        }
        if(op < 0xE60) { /* PSET p */
            s->if_delay = 1;
            s->npc = (op << 8) & 0x1F00;
            s->pc = (s->pc & 0x1000) | ((s->pc + 1) & 0xFFF);
            return 5;
        }
        if(op < 0xE70) { /* LDPX MX,i */
            set_mem(s, s->ix, op & 0xF);
            s->ix = (s->ix & 0xF00) | ((s->ix + 1) & 0xFF);
            NEXT_PC();
            return 5;
        }
        if(op < 0xE80) { /* LDPY MY,i */
            set_mem(s, s->iy, op & 0xF);
            s->iy = (s->iy & 0xF00) | ((s->iy + 1) & 0xFF);
            NEXT_PC();
            return 5;
        }
        r = op & 3;
        switch((op >> 2) & 0x3F) {
        case 0x20: /* E80 LD XP,r */
            s->ix = (get_r(s, r) << 8) | (s->ix & 0x0FF);
            break;
        case 0x21: /* E84 LD XH,r */
            s->ix = (get_r(s, r) << 4) | (s->ix & 0xF0F);
            break;
        case 0x22: /* E88 LD XL,r */
            s->ix = get_r(s, r) | (s->ix & 0xFF0);
            break;
        case 0x23: /* E8C RRC r */
            res = get_r(s, r) + (s->cf << 4);
            s->cf = res & 1;
            set_r(s, r, res >> 1);
            break;
        case 0x24: /* E90 LD YP,r */
            s->iy = (get_r(s, r) << 8) | (s->iy & 0x0FF);
            break;
        case 0x25: /* E94 LD YH,r */
            s->iy = (get_r(s, r) << 4) | (s->iy & 0xF0F);
            break;
        case 0x26: /* E98 LD YL,r */
            s->iy = get_r(s, r) | (s->iy & 0xFF0);
            break;
        case 0x28: /* EA0 LD r,XP */
            set_r(s, r, s->ix >> 8);
            break;
        case 0x29: /* EA4 LD r,XH */
            set_r(s, r, (s->ix >> 4) & 0xF);
            break;
        case 0x2A: /* EA8 LD r,XL */
            set_r(s, r, s->ix & 0xF);
            break;
        case 0x2C: /* EB0 LD r,YP */
            set_r(s, r, s->iy >> 8);
            break;
        case 0x2D: /* EB4 LD r,YH */
            set_r(s, r, (s->iy >> 4) & 0xF);
            break;
        case 0x2E: /* EB8 LD r,YL */
            set_r(s, r, s->iy & 0xF);
            break;
        case 0x30: /* EC0..ECF LD r,q */
        case 0x31:
        case 0x32:
        case 0x33:
            set_r(s, (op >> 2) & 3, get_r(s, op & 3));
            break;
        case 0x38: /* EE0..EEF LDPX r,q */
        case 0x39:
        case 0x3A:
        case 0x3B:
            set_r(s, (op >> 2) & 3, get_r(s, op & 3));
            s->ix = (s->ix & 0xF00) | ((s->ix + 1) & 0xFF);
            break;
        case 0x3C: /* EF0..EFF LDPY r,q */
        case 0x3D:
        case 0x3E:
        case 0x3F:
            set_r(s, (op >> 2) & 3, get_r(s, op & 3));
            s->iy = (s->iy & 0xF00) | ((s->iy + 1) & 0xFF);
            break;
        default: /* undefined: PC does not advance */
            return 5;
        }
        NEXT_PC();
        return 5;

    default: /* 0xF */
        break;
    }

    /* 0xF00 - 0xFFF */
    switch((op >> 4) & 0xF) {
    case 0x0: /* CP r,q */
        r = (op >> 2) & 3;
        q = op & 3;
        res = get_r(s, r);
        res -= get_r(s, q);
        s->zf = res == 0;
        s->cf = res < 0;
        NEXT_PC();
        return 7;
    case 0x1: /* FAN r,q */
        r = (op >> 2) & 3;
        q = op & 3;
        v = get_r(s, r);
        v &= get_r(s, q);
        s->zf = v == 0;
        NEXT_PC();
        return 7;
    case 0x2:
    case 0x3: {
        bool sub = (op & 0x10) != 0;
        bool use_y = (op & 0x4) != 0;
        if(!(op & 0x8)) return 5; /* undefined */
        r = op & 3;
        uint16_t addr = use_y ? s->iy : s->ix;
        res = get_mem(s, addr);
        if(sub) {
            /* SCPX / SCPY: M(X) <- M(X) - r - C */
            res -= get_r(s, r);
            res -= s->cf;
            s->cf = res < 0;
            if(s->df && res < 0) res += 10;
        } else {
            /* ACPX / ACPY: M(X) <- M(X) + r + C */
            res += get_r(s, r);
            res += s->cf;
            s->cf = res > 15;
            if(s->df && res > 9) {
                res += 6;
                s->cf = 1;
            }
        }
        s->zf = (res & 0xF) == 0;
        set_mem(s, addr, res & 0xF);
        if(use_y)
            s->iy = (s->iy & 0xF00) | ((s->iy + 1) & 0xFF);
        else
            s->ix = (s->ix & 0xF00) | ((s->ix + 1) & 0xFF);
        NEXT_PC();
        return 7;
    }
    case 0x4: { /* SET F,i */
        s->cf |= op & 1;
        s->zf |= (op >> 1) & 1;
        s->df |= (op >> 2) & 1;
        uint8_t new_if = (op >> 3) & 1;
        s->if_delay = new_if && !s->iflag;
        s->iflag |= new_if;
        NEXT_PC();
        return 7;
    }
    case 0x5: /* RST F,i */
        s->cf &= op & 1;
        s->zf &= (op >> 1) & 1;
        s->df &= (op >> 2) & 1;
        s->iflag &= (op >> 3) & 1;
        NEXT_PC();
        return 7;
    case 0x6: /* INC Mn */
        res = get_mem(s, op & 0xF) + 1;
        s->zf = res == 16;
        s->cf = res > 15;
        set_mem(s, op & 0xF, res & 0xF);
        NEXT_PC();
        return 7;
    case 0x7: /* DEC Mn */
        res = get_mem(s, op & 0xF) - 1;
        s->zf = res == 0;
        s->cf = res < 0;
        set_mem(s, op & 0xF, res & 0xF);
        NEXT_PC();
        return 7;
    case 0x8: /* LD Mn,A */
        set_mem(s, op & 0xF, s->a);
        NEXT_PC();
        return 5;
    case 0x9: /* LD Mn,B */
        set_mem(s, op & 0xF, s->b);
        NEXT_PC();
        return 5;
    case 0xA: /* LD A,Mn */
        s->a = get_mem(s, op & 0xF);
        NEXT_PC();
        return 5;
    case 0xB: /* LD B,Mn */
        s->b = get_mem(s, op & 0xF);
        NEXT_PC();
        return 5;
    case 0xC: /* PUSH */
        if(op <= 0xFCB) {
            s->sp = (uint8_t)(s->sp - 1);
            switch(op & 0xF) {
            case 0x0:
            case 0x1:
            case 0x2:
            case 0x3:
                set_mem(s, s->sp, get_r(s, op & 3));
                break;
            case 0x4:
                set_mem(s, s->sp, s->ix >> 8);
                break;
            case 0x5:
                set_mem(s, s->sp, (s->ix >> 4) & 0xF);
                break;
            case 0x6:
                set_mem(s, s->sp, s->ix & 0xF);
                break;
            case 0x7:
                set_mem(s, s->sp, s->iy >> 8);
                break;
            case 0x8:
                set_mem(s, s->sp, (s->iy >> 4) & 0xF);
                break;
            case 0x9:
                set_mem(s, s->sp, s->iy & 0xF);
                break;
            case 0xA:
                set_mem(s, s->sp, (s->iflag << 3) | (s->df << 2) | (s->zf << 1) | s->cf);
                break;
            default: /* 0xB: DEC SP */
                break;
            }
            NEXT_PC();
        }
        return 5;
    case 0xD: /* POP / RET */
        switch(op & 0xF) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x3:
            set_r(s, op & 3, get_mem(s, s->sp));
            break;
        case 0x4:
            s->ix = (get_mem(s, s->sp) << 8) | (s->ix & 0x0FF);
            break;
        case 0x5:
            s->ix = (get_mem(s, s->sp) << 4) | (s->ix & 0xF0F);
            break;
        case 0x6:
            s->ix = get_mem(s, s->sp) | (s->ix & 0xFF0);
            break;
        case 0x7:
            s->iy = (get_mem(s, s->sp) << 8) | (s->iy & 0x0FF);
            break;
        case 0x8:
            s->iy = (get_mem(s, s->sp) << 4) | (s->iy & 0xF0F);
            break;
        case 0x9:
            s->iy = get_mem(s, s->sp) | (s->iy & 0xFF0);
            break;
        case 0xA: { /* POP F */
            uint8_t f = get_mem(s, s->sp);
            s->cf = f & 1;
            s->zf = (f >> 1) & 1;
            s->df = (f >> 2) & 1;
            uint8_t new_if = (f >> 3) & 1;
            s->if_delay = new_if && !s->iflag;
            s->iflag = new_if;
            break;
        }
        case 0xB: /* INC SP */
            break;
        case 0xE: { /* RETS */
            uint16_t lo = get_mem(s, s->sp);
            uint16_t mid = get_mem(s, s->sp + 1);
            uint16_t hi = get_mem(s, s->sp + 2);
            s->pc = (s->pc & 0x1000) | lo | (mid << 4) | (hi << 8);
            s->sp = (uint8_t)(s->sp + 3);
            NEXT_PC();
            return 12;
        }
        case 0xF: { /* RET */
            uint16_t lo = get_mem(s, s->sp);
            uint16_t mid = get_mem(s, s->sp + 1);
            uint16_t hi = get_mem(s, s->sp + 2);
            s->pc = s->npc = (s->pc & 0x1000) | lo | (mid << 4) | (hi << 8);
            s->sp = (uint8_t)(s->sp + 3);
            return 7;
        }
        default: /* undefined */
            return 5;
        }
        s->sp = (uint8_t)(s->sp + 1);
        NEXT_PC();
        return 5;
    case 0xE:
        if(op <= 0xFE3) { /* LD SPH,r */
            s->sp = (get_r(s, op & 3) << 4) | (s->sp & 0x0F);
        } else if(op <= 0xFE7) { /* LD r,SPH */
            set_r(s, op & 3, s->sp >> 4);
        } else if(op == 0xFE8) { /* JPBA */
            s->pc = (s->npc & 0x1F00) | (s->b << 4) | s->a;
            return 5;
        } else {
            return 5; /* undefined */
        }
        NEXT_PC();
        return 5;
    default: /* 0xF */
        if(op <= 0xFF3) { /* LD SPL,r */
            s->sp = get_r(s, op & 3) | (s->sp & 0xF0);
        } else if(op <= 0xFF7) { /* LD r,SPL */
            set_r(s, op & 3, s->sp & 0x0F);
        } else if(op == 0xFF8) { /* HALT */
            s->halt = 1;
        } else if(op == 0xFFB) { /* NOP5 */
        } else if(op == 0xFFF) { /* NOP7 */
            NEXT_PC();
            return 7;
        } else {
            return 5; /* undefined */
        }
        NEXT_PC();
        return 5;
    }
}

static uint32_t interrupt(E0C6200State* s, uint8_t vector) {
    set_mem(s, (uint8_t)(s->sp - 1), (s->pc >> 8) & 0xF);
    set_mem(s, (uint8_t)(s->sp - 2), (s->pc >> 4) & 0xF);
    s->sp = (uint8_t)(s->sp - 3);
    set_mem(s, s->sp, s->pc & 0xF);
    s->iflag = 0;
    s->halt = 0;
    s->pc = s->npc = (s->npc & 0x1000) | 0x0100 | vector;
    return 13;
}

uint32_t e0c6200_step(E0C6200* cpu) {
    E0C6200State* s = &cpu->s;
    uint32_t cycles = 7;
    if(s->reset_held) return cycles;

    if(!s->halt) {
        s->if_delay = 0;
        cycles = execute(s, cpu->rom[s->pc & (E0C_ROM_WORDS - 1)]);
    }

    if(s->iflag && !s->if_delay) {
        if(s->ipt & s->eipt)
            cycles += interrupt(s, 0xC);
        else if(s->isio & s->eisio)
            cycles += interrupt(s, 0xA);
        else if(s->ik1)
            cycles += interrupt(s, 0x8);
        else if(s->ik0)
            cycles += interrupt(s, 0x6);
        else if(s->isw & s->eisw)
            cycles += interrupt(s, 0x4);
        else if(s->it & s->eit)
            cycles += interrupt(s, 0x2);
    }

    int64_t units = (int64_t)cycles * ((s->ctrl_osc & IO_CLKCHG) ? E0C_OSC1_HZ : cpu->osc3_hz);
    s->osc1_counter -= units;
    while(s->osc1_counter <= 0) {
        s->osc1_counter += cpu->osc3_hz;
        clock_osc1(s);
    }
    return cycles;
}

static bool interrupt_pending(const E0C6200State* s) {
    if(!s->iflag) return false;
    return (s->ipt & s->eipt) || (s->isio & s->eisio) || s->ik1 || s->ik0 || (s->isw & s->eisw) ||
           (s->it & s->eit);
}

/* While halted nothing happens until an OSC1 event changes an interrupt
 * flag. Skip straight to just before the next such event. */
static uint32_t idle_skip(E0C6200State* s, uint32_t max_ticks) {
    int32_t n = s->timer_counter;
    if(s->ctrl_sw & IO_SWRUN) {
        int32_t sw = (s->stopwatch_counter + 99) / 100;
        if(sw < n) n = sw;
    }
    if((s->ptc & IO_PTC) > 1 && s->ptimer_counter < n) n = s->ptimer_counter;
    if(s->snd_one_shot > 0 && s->snd_one_shot < n) n = s->snd_one_shot;
    if(s->snd_env_counter > 0 && s->snd_env_counter < n) n = s->snd_env_counter;
    n -= 1; /* leave the event tick itself to the normal path */
    if(n <= 0) return 0;
    if((uint32_t)n > max_ticks) n = (int32_t)max_ticks;

    s->timer_counter -= n;
    s->stopwatch_counter -= 100 * n;
    if(s->stopwatch_counter <= 0) {
        /* stopwatch not running: keep the divider phase, no event */
        s->stopwatch_counter %= STOPWATCH_DIV;
        if(s->stopwatch_counter <= 0) s->stopwatch_counter += STOPWATCH_DIV;
    }
    if((s->ptc & IO_PTC) > 1) s->ptimer_counter -= n;
    if(s->snd_one_shot > 0) s->snd_one_shot -= n;
    if(s->snd_env_counter > 0) s->snd_env_counter -= n;
    s->osc1_ticks += n;
    return (uint32_t)n;
}

void e0c6200_run(E0C6200* cpu, uint32_t osc1_ticks, uint32_t max_steps) {
    E0C6200State* s = &cpu->s;
    if(s->reset_held) {
        s->osc1_ticks += osc1_ticks;
        return;
    }
    uint64_t target = s->osc1_ticks + osc1_ticks;
    uint32_t steps = 0;
    while(s->osc1_ticks < target) {
        if(cpu->fast_halt && s->halt && !interrupt_pending(s)) {
            idle_skip(s, (uint32_t)(target - s->osc1_ticks));
            if(s->osc1_ticks >= target) break;
        }
        e0c6200_step(cpu);
        if(max_steps && ++steps >= max_steps) break;
    }
}
