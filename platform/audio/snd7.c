/* The ARM7's sound driver (see snd7.h), after the NitroSDK's ARM7 SND library: command
 * processing (snd_command.c), the sequencer (snd_seq.c), extended channels with their
 * envelopes, LFO and sweep (snd_exchannel.c), instruments (snd_bank.c), alarms
 * (snd_alarm.c) and the shared work the ARM9 reads (snd_work.c); and in place of the sound
 * hardware a software mixer.
 *
 * Units as on the DS: volumes in decibels x10 (-723 is silence), envelope levels x128, pitch
 * in 1/64 semitone, sequencer frames at the ARM7 driver's rate (~192 Hz), ticks of 1/48 of a
 * quarter note. */
#include "snd7.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void (*snd7_send_to_arm9)(uint32_t word);
uint64_t (*snd7_clock_us)(void);
uintptr_t snd7_ptr_base;

/* a 32-bit DS-side pointer (command arguments, the links inside banks): the address itself on
 * the Vita, an offset into the test's arena on a 64-bit host */
#define P(v) ((const uint8_t *)(snd7_ptr_base + (uintptr_t)(uint32_t)(v)))

/* ---- constants ------------------------------------------------------------------------- */

#define CHANNELS 16
#define PLAYERS 16
#define TRACKS 32
#define ALARMS 8
#define TRACKS_PER_PLAYER 16

#define ARM7_CLOCK 33513982.0
#define CHANNEL_CLOCK (ARM7_CLOCK / 2.0)        /* sound timers count at this rate */
#define FRAME_HZ (ARM7_CLOCK / 64.0 / 2728.0)   /* the driver's frame: ~191.97 Hz */
#define ALARM_HZ (ARM7_CLOCK / 64.0)            /* SND alarm ticks */

#define DB_MIN (-723)
#define ENV_MIN (DB_MIN * 128)

enum {
    CMD_START_SEQ, CMD_STOP_SEQ, CMD_PREPARE_SEQ, CMD_START_PREPARED_SEQ, CMD_PAUSE_SEQ,
    CMD_SKIP_SEQ, CMD_PLAYER_PARAM, CMD_TRACK_PARAM, CMD_MUTE_TRACK, CMD_ALLOCATABLE_CHANNEL,
    CMD_PLAYER_LOCAL_VAR, CMD_PLAYER_GLOBAL_VAR, CMD_START_TIMER, CMD_STOP_TIMER,
    CMD_SETUP_CHANNEL_PCM, CMD_SETUP_CHANNEL_PSG, CMD_SETUP_CHANNEL_NOISE, CMD_SETUP_CAPTURE,
    CMD_SETUP_ALARM, CMD_CHANNEL_TIMER, CMD_CHANNEL_VOLUME, CMD_CHANNEL_PAN,
    CMD_SURROUND_DECAY, CMD_MASTER_VOLUME, CMD_MASTER_PAN, CMD_OUTPUT_SELECTOR,
    CMD_LOCK_CHANNEL, CMD_UNLOCK_CHANNEL, CMD_STOP_UNLOCKED_CHANNEL, CMD_SHARED_WORK,
    CMD_INVALIDATE_SEQ, CMD_INVALIDATE_BANK, CMD_INVALIDATE_WAVE, CMD_READ_DRIVER_INFO,
};

enum { FMT_PCM8, FMT_PCM16, FMT_ADPCM, FMT_PSG };
enum { CH_PCM, CH_PSG, CH_NOISE };
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

/* ---- the ARM9's structures ------------------------------------------------------------ */

typedef struct {
    uint32_t next; /* SNDCommand*, on the DS a 32-bit pointer */
    uint32_t id;
    uint32_t arg[4];
} DsCommand;

typedef struct {
    uint8_t type, pad;
    uint16_t wave[2];
    uint8_t original_key, attack, decay, sustain, release, pan;
} InstData;

/* ---- the driver's state ----------------------------------------------------------------- */

/* SNDPlayer and SNDTrack keep the SDK's layout up to 0x20: PLAYER_PARAM and TRACK_PARAM write
 * into them by offset (SND_SetPlayerVolume: offset 6; SND_SetTrackPan: 9; ...). */
typedef struct {
    uint8_t flags;        /* 0x00: 1 active, 2 prepared, 4 paused */
    uint8_t my_no;        /* 0x01 */
    uint8_t pad02[2];
    uint8_t prio;         /* 0x04 */
    uint8_t volume;       /* 0x05 */
    int16_t ext_fader;    /* 0x06 */
    uint8_t tracks[16];   /* 0x08: track pool indices, 0xff for none */
    uint16_t tempo;       /* 0x18 */
    uint16_t tempo_ratio; /* 0x1a */
    uint16_t tempo_counter; /* 0x1c */
    uint8_t pad1e[2];
    const uint8_t *bank;  /* SNDBankData */
} Player;

typedef struct {
    uint8_t flags;        /* 0x00 */
    uint8_t pan_range;    /* 0x01 */
    uint16_t prg_no;      /* 0x02 */
    uint8_t volume;       /* 0x04 */
    uint8_t volume2;      /* 0x05 */
    int8_t pitch_bend;    /* 0x06 */
    uint8_t bend_range;   /* 0x07 */
    int8_t pan;           /* 0x08 */
    int8_t ext_pan;       /* 0x09 */
    int16_t ext_fader;    /* 0x0a */
    int16_t ext_pitch;    /* 0x0c */
    uint8_t attack, decay, sustain, release; /* 0x0e */
    uint8_t prio;         /* 0x12 */
    int8_t transpose;     /* 0x13 */
    uint8_t porta_key;    /* 0x14 */
    uint8_t porta_time;   /* 0x15 */
    int16_t sweep_pitch;  /* 0x16 */
    uint8_t mod_target, mod_speed, mod_depth, mod_range; /* 0x18 */
    uint16_t mod_delay;   /* 0x1c */
    uint16_t channel_mask; /* 0x1e */
    int32_t wait;
    const uint8_t *base, *cur;
    const uint8_t *call_stack[3];
    uint8_t loop_count[3];
    uint8_t depth;
    int player;
} Track;

enum { TF_ACTIVE = 1, TF_NOTE_WAIT = 2, TF_MUTE = 4, TF_TIE = 8, TF_NOTE_FINISH_WAIT = 16,
       TF_PORTA = 32, TF_CMP = 64, TF_CHANNEL_MASK = 128 };
enum { PF_ACTIVE = 1, PF_PREPARED = 2, PF_PAUSE = 4 };

_Static_assert(offsetof(Player, ext_fader) == 6, "Player layout");
_Static_assert(offsetof(Player, tempo) == 0x18, "Player layout");
_Static_assert(offsetof(Track, ext_pan) == 9, "Track layout");
_Static_assert(offsetof(Track, ext_fader) == 0xa, "Track layout");
_Static_assert(offsetof(Track, channel_mask) == 0x1e, "Track layout");

typedef struct {
    /* the hardware channel */
    int hw_on;
    int format;              /* FMT_* */
    const uint8_t *data;
    uint32_t loop_start;     /* bytes */
    uint32_t end;            /* bytes: loop start + loop length */
    int repeat;              /* 1 loop, 2 one shot */
    uint64_t pos;            /* samples (nibbles for ADPCM) from the start, 32.32 fixed point */
    double rate;             /* samples per second */
    int duty;                /* PSG */
    uint16_t lfsr;
    int adpcm_pred, adpcm_index, adpcm_pos;       /* the decoder, at sample adpcm_pos */
    int adpcm_loop_pred, adpcm_loop_index, adpcm_loop_saved;
    int adpcm_next;          /* sample adpcm_next_at + 1, decoded ahead */
    uint32_t adpcm_next_at;
    struct WaveCache *wc;    /* a bank's ADPCM wave decoded (wave_cache_get), NULL none */
    float gain_l, gain_r;    /* linear, from volume, shift and pan */
    int16_t hold;            /* the last sample, for format PSG/noise steps */
    /* SETUP_CHANNEL_* registers, kept for CHANNEL_* commands */
    int reg_volume, reg_shift, reg_pan;
    uint32_t reg_timer;

    /* the extended channel (sequencer voices) */
    int ex_active;           /* owned by the sequencer */
    int type;                /* CH_* */
    int start;
    int track;               /* owner track, -1 none */
    int prio;
    uint8_t key, original_key, velocity;
    int init_pan;
    int env_status;
    int32_t env_decay;
    int attack_rate, decay_rate, release_rate;
    int32_t sustain_level;
    int32_t length;          /* ticks, -1 forever */
    int user_decay, user_decay2, user_pitch, user_pan, pan_range;
    int sweep_pitch, sweep_counter, sweep_length, auto_sweep;
    int lfo_target, lfo_speed, lfo_depth, lfo_range, lfo_delay, lfo_delay_counter;
    uint32_t lfo_counter;
    double base_rate;        /* samples per second at original_key */
    uint32_t serial;         /* allocation order, for stealing */
} Channel;

static Player s_player[PLAYERS];
static Track s_track[TRACKS];
static Channel s_ch[CHANNELS];
static uint16_t s_locked;            /* channels the ARM9 keeps for itself */
static int s_master_volume = 127;
static int16_t s_local_vars[PLAYERS][16], s_global_vars[16]; /* without a shared work */
static uint8_t *s_work;              /* SNDSharedWork */
static uint32_t s_rand = 0x12345678;
static uint32_t s_serial;

typedef struct {
    int active;
    uint32_t id;
    double next, period;             /* in seconds of audio */
} Alarm;
static Alarm s_alarm[ALARMS];
static struct { uint32_t tick, period, id; } s_alarm_setup[ALARMS];

static double s_time;                /* seconds of audio rendered */
static double s_frame_acc;
static Snd7Stats s_stats;

/* ---- command intake: PXI words from the game thread, processed by the audio thread ---- */

#define QUEUE_LISTS 32
#define LIST_MAX 256
typedef struct {
    int count;
    DsCommand cmd[LIST_MAX];
} List;
static List s_queue[QUEUE_LISTS];
static volatile uint32_t s_q_head, s_q_tail; /* written by intake / by the audio thread */
static volatile int s_running;

static void process_list(const List *l);
static void work_finish_list(void);

void snd7_set_running(int running)
{
    s_running = running;
}

void snd7_pxi(uint32_t word)
{
    const DsCommand *c;
    List *l;
    uint32_t head;
    if (!word)
        return;
    head = s_q_head;
    if (head - __atomic_load_n(&s_q_tail, __ATOMIC_ACQUIRE) >= QUEUE_LISTS) {
        /* the audio thread is far behind (or gone): process here */
        static List tmp;
        tmp.count = 0;
        for (c = (const DsCommand *)P(word); c && tmp.count < LIST_MAX;
             c = (c->next ? (const DsCommand *)P(c->next) : NULL))
            tmp.cmd[tmp.count++] = *c;
        process_list(&tmp);
        work_finish_list();
        return;
    }
    l = &s_queue[head % QUEUE_LISTS];
    l->count = 0;
    for (c = (const DsCommand *)P(word); c && l->count < LIST_MAX;
         c = (c->next ? (const DsCommand *)P(c->next) : NULL))
        l->cmd[l->count++] = *c;
    if (!s_running) {
        /* no audio thread: the commands still take effect on the driver state, so that a
         * later start finds it consistent, and the ARM9 sees them finished at once */
        process_list(l);
        work_finish_list();
        return;
    }
    __atomic_store_n(&s_q_head, head + 1, __ATOMIC_RELEASE);
}

static void drain_queue(void)
{
    uint32_t tail = s_q_tail;
    while (tail != __atomic_load_n(&s_q_head, __ATOMIC_ACQUIRE)) {
        process_list(&s_queue[tail % QUEUE_LISTS]);
        work_finish_list();
        __atomic_store_n(&s_q_tail, ++tail, __ATOMIC_RELEASE);
    }
}

/* ---- shared work ---------------------------------------------------------------------- */

static void work_finish_list(void)
{
    s_stats.lists++;
    if (s_work)
        __atomic_add_fetch((uint32_t *)s_work, 1, __ATOMIC_RELEASE);
}

static int16_t *var_ptr(int player, int no)
{
    if (no < 16) {
        if (s_work)
            return (int16_t *)(s_work + 0x20 + player * 0x24 + no * 2);
        return &s_local_vars[player][no];
    }
    if (s_work)
        return (int16_t *)(s_work + 0x260 + (no - 16) * 2);
    return &s_global_vars[no - 16];
}

static void work_update_status(void)
{
    uint32_t ps = 0;
    uint16_t cs = 0;
    int i;
    if (!s_work)
        return;
    for (i = 0; i < PLAYERS; i++)
        if (s_player[i].flags & PF_ACTIVE)
            ps |= 1u << i;
    for (i = 0; i < CHANNELS; i++)
        if (s_ch[i].hw_on)
            cs |= (uint16_t)(1u << i);
    *(volatile uint32_t *)(s_work + 4) = ps;
    *(volatile uint16_t *)(s_work + 8) = cs;
}

/* ---- tables ------------------------------------------------------------------------------ */

static int16_t s_db_square[128]; /* 0-127 as a squared amplitude, in dB x10 */
static int8_t s_sine[33];        /* a quarter sine, 0-127 */
static int s_tables_ready;

static const uint8_t s_attack_table[19] = { 0, 1, 5, 14, 26, 38, 51, 63, 73, 84,
                                           92, 100, 109, 116, 123, 127, 132, 137, 143 };

/* IMA ADPCM */
static const int16_t s_adpcm_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
    449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767 };
static const int8_t s_adpcm_index[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

static void tables_init(void)
{
    int i;
    if (s_tables_ready)
        return;
    s_db_square[0] = DB_MIN;
    for (i = 1; i < 128; i++) {
        double db = 400.0 * log10((double)i / 127.0);
        s_db_square[i] = (int16_t)(db < DB_MIN ? DB_MIN : lrint(db));
    }
    for (i = 0; i <= 32; i++)
        s_sine[i] = (int8_t)lrint(127.0 * sin(i * M_PI / 64.0));
    s_tables_ready = 1;
}

static int sine_at(int idx) /* idx 0..127 over a period */
{
    idx &= 127;
    if (idx < 32) return s_sine[idx];
    if (idx < 64) return s_sine[64 - idx];
    if (idx < 96) return -s_sine[idx - 64];
    return -s_sine[128 - idx];
}

static int attack_rate(int attack)
{
    return attack >= 109 ? s_attack_table[127 - attack] : 255 - attack;
}

static int decay_rate(int v) /* also release */
{
    if (v == 127) return 0xffff;
    if (v == 126) return 0x3c00;
    if (v < 50) return v * 2 + 1;
    return 0x1e00 / (126 - v);
}

static uint16_t rand16(void)
{
    s_rand = s_rand * 1664525u + 1013904223u;
    return (uint16_t)(s_rand >> 16);
}

/* ---- channels ------------------------------------------------------------------------- */

/* The SDK's channel timers (SND_SetupChannelPcm, SNDWaveParam.timer) are the sample period in
 * channel clocks; the ARM7 writes 0x10000 - timer to the hardware register. */
static double timer_rate(uint32_t timer)
{
    timer &= 0xffff;
    return CHANNEL_CLOCK / (double)(timer ? timer : 1);
}

static void hw_set_gain(Channel *c, double amp, int pan /* 0-127 */)
{
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    c->gain_l = (float)(amp * (127 - pan) / 127.0);
    c->gain_r = (float)(amp * pan / 127.0);
}

static void hw_reg_gain(Channel *c)
{
    static const int shift_div[4] = { 1, 2, 4, 16 };
    hw_set_gain(c, (double)c->reg_volume / 127.0 / shift_div[c->reg_shift & 3], c->reg_pan);
}

static void hw_start(Channel *c)
{
    c->hw_on = 1;
    c->pos = 0;
    c->adpcm_pos = -1;
    c->adpcm_loop_saved = 0;
    c->adpcm_next_at = UINT32_MAX;
    c->lfsr = 0x7fff;
    c->hold = 0;
    if (c->format == FMT_ADPCM && c->data) {
        c->adpcm_pred = (int16_t)(c->data[0] | c->data[1] << 8);
        c->adpcm_index = c->data[2] > 88 ? 88 : c->data[2];
        c->adpcm_pos = 0;
    }
}

static void hw_stop(Channel *c)
{
    c->hw_on = 0;
}

/* one IMA-ADPCM nibble into the decoder state */
static inline void adpcm_step(int *pred, int *index, int nib)
{
    int st = s_adpcm_step[*index], diff = st >> 3;
    if (nib & 1) diff += st >> 2;
    if (nib & 2) diff += st >> 1;
    if (nib & 4) diff += st;
    if (nib & 8) {
        *pred -= diff;
        if (*pred < -0x7fff) *pred = -0x7fff;
    } else {
        *pred += diff;
        if (*pred > 0x7fff) *pred = 0x7fff;
    }
    *index += s_adpcm_index[nib & 7];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
}

#define POS_ONE 4294967296.0 /* 1.0 in the channels' 32.32 positions */
#define POS_INT(p) ((uint32_t)((p) >> 32))

/* The ADPCM decoder brought to the position's sample (adpcm_pred holds it); false when the
 * wave ended. Wraps the position at the loop as the hardware does. */
static inline int adpcm_seek(Channel *c, uint32_t n, uint32_t ls)
{
    uint32_t want = POS_INT(c->pos);
    if (want >= n) {
        if (c->repeat == 1 && n > ls) {
            while (POS_INT(c->pos) >= n)
                c->pos -= (uint64_t)(n - ls) << 32;
            want = POS_INT(c->pos);
            if (c->adpcm_loop_saved) {
                c->adpcm_pred = c->adpcm_loop_pred;
                c->adpcm_index = c->adpcm_loop_index;
                c->adpcm_pos = (int)ls;
            }
            c->adpcm_next_at = UINT32_MAX;
        } else {
            c->hw_on = 0;
            return 0;
        }
    }
    if ((int)want + 1 < c->adpcm_pos) { /* went back without a saved state: restart */
        c->adpcm_pred = (int16_t)(c->data[0] | c->data[1] << 8);
        c->adpcm_index = c->data[2] > 88 ? 88 : c->data[2];
        c->adpcm_pos = 0;
        c->adpcm_next_at = UINT32_MAX;
    }
    while ((uint32_t)c->adpcm_pos <= want) {
        adpcm_step(&c->adpcm_pred, &c->adpcm_index,
                   (c->data[4 + c->adpcm_pos / 2] >> ((c->adpcm_pos & 1) * 4)) & 15);
        c->adpcm_pos++;
        if ((uint32_t)c->adpcm_pos == ls && !c->adpcm_loop_saved) {
            c->adpcm_loop_pred = c->adpcm_pred;
            c->adpcm_loop_index = c->adpcm_index;
            c->adpcm_loop_saved = 1;
        }
    }
    return 1;
}

/* the interpolation between two samples (16-bit scale) at the position's fraction, 15 bits */
static inline int lerp15(int s0, int s1, uint64_t pos)
{
    return s0 + (((s1 - s0) * (int)((uint32_t)pos >> 17)) >> 15);
}

/* ---- decoded ADPCM waves ----------------------------------------------------------------
 * A bank's ADPCM wave (a sequencer note's, never written while it plays) decoded once into
 * 16-bit samples, kept for the next notes that play it, and mixed as plain samples. Decoding
 * each channel's nibbles inside the mixing loop, the state of two decoders and the position
 * in registers, was most of the audio thread's time on the Vita: 2.4 of its 2.6 s every 10 s
 * with 15 channels (0.4.12's log). Decoded just ahead of where the channels play, a long wave
 * is never decoded at once on its first note. Sample k is the decoder after nibble k, the
 * order the hardware plays them in, a loop's start included (its saved state is the one the
 * linear decoding reaches there). */
typedef struct WaveCache {
    const uint8_t *src;      /* the wave's data: the 4-byte header, then the nibbles */
    uint32_t n, ls;          /* samples, loop start (samples) */
    int16_t *pcm;            /* n + 1 samples, the last one repeated (the interpolation's) */
    uint32_t done;           /* samples decoded */
    int pred, index;         /* the decoder after them */
    uint32_t used;           /* the last note on it (s_wave_clock) */
} WaveCache;

#define WAVE_CACHE_MAX 384
#define WAVE_CACHE_BYTES (24u * 1024 * 1024)
static WaveCache s_wave[WAVE_CACHE_MAX];
static int s_nwave;
static size_t s_wave_bytes;
static uint32_t s_wave_clock;

/* decoded up to sample upto (exclusive, at most n) */
static void wave_decode(WaveCache *w, uint32_t upto)
{
    const uint8_t *d = w->src + 4;
    int pred = w->pred, index = w->index;
    uint32_t k = w->done;
    if (upto > w->n)
        upto = w->n;
    for (; k < upto; k++) {
        adpcm_step(&pred, &index, (d[k >> 1] >> ((k & 1) * 4)) & 15);
        w->pcm[k] = (int16_t)pred;
    }
    if (k > w->done) {
        w->done = k;
        w->pred = pred, w->index = index;
        if (k == w->n)
            w->pcm[k] = w->pcm[k - 1];
    }
}

static int wave_in_use(const WaveCache *w)
{
    int i;
    for (i = 0; i < CHANNELS; i++)
        if (s_ch[i].wc == w && s_ch[i].hw_on)
            return 1;
    return 0;
}

/* The decoded wave for an ADPCM wave of end bytes (loop start ls_bytes), NULL when there is
 * no room (the channel then decodes as it plays) */
static WaveCache *wave_cache_get(const uint8_t *src, uint32_t end, uint32_t ls_bytes)
{
    const uint32_t n = (end - 4) * 2, ls = (ls_bytes > 4 ? ls_bytes - 4 : 0) * 2;
    const size_t bytes = ((size_t)n + 1) * sizeof(int16_t);
    WaveCache *w = NULL;
    int i;
    s_wave_clock++;
    for (i = 0; i < s_nwave; i++)
        if (s_wave[i].src == src && s_wave[i].n == n && s_wave[i].ls == ls) {
            s_wave[i].used = s_wave_clock;
            return &s_wave[i];
        }
    if (bytes > WAVE_CACHE_BYTES / 4)
        return NULL;
    /* room: the waves no note has played for longest go first */
    while (s_nwave >= WAVE_CACHE_MAX || s_wave_bytes + bytes > WAVE_CACHE_BYTES) {
        int old = -1;
        for (i = 0; i < s_nwave; i++)
            if (!wave_in_use(&s_wave[i]) && (old < 0 || s_wave[i].used < s_wave[old].used))
                old = i;
        if (old < 0)
            return NULL;
        s_wave_bytes -= ((size_t)s_wave[old].n + 1) * sizeof(int16_t);
        free(s_wave[old].pcm);
        for (i = 0; i < CHANNELS; i++)
            if (s_ch[i].wc == &s_wave[old])
                s_ch[i].wc = NULL; /* stopped ones only (wave_in_use) */
        s_wave[old] = s_wave[--s_nwave];
        /* the moved entry's channels follow it */
        for (i = 0; i < CHANNELS; i++)
            if (s_ch[i].wc == &s_wave[s_nwave])
                s_ch[i].wc = &s_wave[old];
    }
    w = &s_wave[s_nwave];
    w->pcm = malloc(bytes);
    if (!w->pcm)
        return NULL;
    s_nwave++;
    s_wave_bytes += bytes;
    w->src = src, w->n = n, w->ls = ls;
    w->done = 0;
    w->pred = (int16_t)(src[0] | src[1] << 8);
    w->index = src[2] > 88 ? 88 : src[2];
    w->used = s_wave_clock;
    return w;
}

/* A channel's next count output samples added into the mix (al, ar: 16-bit samples times
 * 2^24-scaled gains, >> 16: full scale is 2^23, 16 channels fit), each format in its own
 * loop. Stops where a one-shot wave
 * ends. Was one sample of every channel at a time, the format chosen again for each, the
 * position in doubles and the ADPCM's next sample decoded again for every output sample:
 * about a quarter of a core on the Vita (0.4.0's log). A channel at zero volume (a note's
 * release run out, a muted track) only moves on. */
#define MIX_MUL(s, g) ((int32_t)(((int64_t)(s) * (g)) >> 16))
/* the same with the gain in 15 bits (g15 = g >> 9): a 32-bit multiply, 16 x 15 bits */
#define MIX_MUL15(s, g15) (((s) * (g15)) >> 7)

/* How many of the next left output samples, from pos by step, have both samples they
 * interpolate between before sample limit (i + 1 < limit): those need no end or loop test,
 * and are mixed in a loop of their own (0.6.x: the loop with the tests, and the position kept
 * in the channel, was most of the mixing time) */
static inline int span_before(uint64_t pos, uint64_t step, uint32_t limit, int left)
{
    uint64_t room, m;
    if (limit < 2 || POS_INT(pos) + 1 >= limit || !step)
        return 0;
    room = ((uint64_t)(limit - 1) << 32) - pos; /* positions below sample limit - 1 */
    m = (room - 1) / step + 1;
    return m < (uint64_t)left ? (int)m : left;
}

typedef int16_t __attribute__((aligned(1), may_alias)) s16u;

static void mix_channel(Channel *c, int32_t *al, int32_t *ar, int count, uint64_t step)
{
    const int32_t gl = (int32_t)(c->gain_l * 16777216.0f + 0.5f);
    const int32_t gr = (int32_t)(c->gain_r * 16777216.0f + 0.5f);
    const int32_t gl15 = (gl + 256) >> 9, gr15 = (gr + 256) >> 9;
    const int silent = gl == 0 && gr == 0;
    const uint8_t *d = c->data;
    int k;
    switch (c->format) {
    case FMT_PCM8:
    case FMT_PCM16: {
        /* linear interpolation between the two samples around the position: the DS plays the
         * nearest one at its own 32 kHz, which resampled to 48 kHz turns into grain */
        const int b16 = c->format == FMT_PCM16;
        const uint32_t n = b16 ? c->end / 2 : c->end, ls = b16 ? c->loop_start / 2 : c->loop_start;
        const int loops = c->repeat == 1 && n > ls;
        uint64_t pos = c->pos;
        for (k = 0; k < count;) {
            uint32_t i, j;
            int s0, s1, s;
            const int span = silent ? 0 : span_before(pos, step, n, count - k);
            if (span > 0) {
                /* away from the end: no tests */
                const int e = k + span;
                if (b16) {
                    const s16u *p16 = (const s16u *)d;
                    for (; k < e; k++) {
                        const uint32_t ii = POS_INT(pos);
                        const int v = lerp15(p16[ii], p16[ii + 1], pos);
                        al[k] += MIX_MUL15(v, gl15);
                        ar[k] += MIX_MUL15(v, gr15);
                        pos += step;
                    }
                } else {
                    const int8_t *p8 = (const int8_t *)d;
                    for (; k < e; k++) {
                        const uint32_t ii = POS_INT(pos);
                        const int v = lerp15(p8[ii] * 256, p8[ii + 1] * 256, pos);
                        al[k] += MIX_MUL15(v, gl15);
                        ar[k] += MIX_MUL15(v, gr15);
                        pos += step;
                    }
                }
                continue;
            }
            i = POS_INT(pos);
            if (i >= n) {
                if (loops) {
                    while (POS_INT(pos) >= n)
                        pos -= (uint64_t)(n - ls) << 32;
                    i = POS_INT(pos);
                } else {
                    c->pos = pos;
                    c->hw_on = 0;
                    return;
                }
            }
            if (silent) {
                pos += step;
                k++;
                continue;
            }
            j = i + 1;
            if (j >= n)
                j = loops ? ls : i;
            if (b16) {
                s0 = (int16_t)(d[i * 2] | d[i * 2 + 1] << 8);
                s1 = (int16_t)(d[j * 2] | d[j * 2 + 1] << 8);
            } else {
                s0 = (int8_t)d[i] * 256;
                s1 = (int8_t)d[j] * 256;
            }
            s = lerp15(s0, s1, pos);
            al[k] += MIX_MUL(s, gl);
            ar[k] += MIX_MUL(s, gr);
            pos += step;
            k++;
        }
        c->pos = pos;
        break;
    }
    case FMT_ADPCM:
        if (c->wc) {
            /* decoded (wave_cache_get): the samples this chunk reaches first, then the same
             * loop as the nibbles' below, plain samples in place of the decoder */
            WaveCache *const w = c->wc;
            const int16_t *const pcm = w->pcm;
            const uint32_t n = w->n, ls = w->ls;
            const int loops = c->repeat == 1 && n > ls;
            uint64_t pos = c->pos;
            {
                const uint64_t last = pos + step * (uint64_t)count;
                const uint32_t need = POS_INT(last) + 2;
                if (w->done < n)
                    wave_decode(w, loops && POS_INT(last) >= n ? n : need);
            }
            for (k = 0; k < count;) {
                uint32_t i;
                /* pcm holds n + 1 samples: away from sample n, no tests */
                const int span = silent ? 0 : span_before(pos, step, n + 1, count - k);
                if (span > 0) {
                    const int e = k + span;
                    for (; k < e; k++) {
                        const uint32_t ii = POS_INT(pos);
                        const int s = lerp15(pcm[ii], pcm[ii + 1], pos);
                        al[k] += MIX_MUL15(s, gl15);
                        ar[k] += MIX_MUL15(s, gr15);
                        pos += step;
                    }
                    continue;
                }
                i = POS_INT(pos);
                if (i >= n) {
                    if (!loops) {
                        c->pos = pos;
                        c->hw_on = 0;
                        return;
                    }
                    while (POS_INT(pos) >= n)
                        pos -= (uint64_t)(n - ls) << 32;
                    i = POS_INT(pos);
                }
                if (!silent) {
                    const int s = lerp15(pcm[i], pcm[i + 1], pos);
                    al[k] += MIX_MUL(s, gl);
                    ar[k] += MIX_MUL(s, gr);
                }
                pos += step;
                k++;
            }
            c->pos = pos;
            break;
        }
        {
        /* nibbles after the 4-byte header, 2 a byte; adpcm_pos nibbles decoded leave
         * adpcm_pred holding sample adpcm_pos - 1 (the header's value before the first) */
        const uint32_t n = (c->end - 4) * 2, ls = (c->loop_start > 4 ? c->loop_start - 4 : 0) * 2;
        uint32_t cur = UINT32_MAX; /* the source sample s0 and s1 are around */
        int s0 = 0, s1 = 0;
        for (k = 0; k < count; k++) {
            /* the decoder only moved when the source sample changes: at 48 kHz from 32, two
             * output samples in three; the third one is between the same two */
            if (POS_INT(c->pos) != cur) {
                uint32_t want;
                if (!adpcm_seek(c, n, ls))
                    return;
                want = POS_INT(c->pos);
                s0 = c->adpcm_pred;
                if (want + 1 < n) {
                    /* the next sample, decoded on a copy of the state */
                    if (c->adpcm_next_at != want) {
                        int pred = c->adpcm_pred, index = c->adpcm_index;
                        adpcm_step(&pred, &index,
                                   (d[4 + (want + 1) / 2] >> (((want + 1) & 1) * 4)) & 15);
                        c->adpcm_next = pred;
                        c->adpcm_next_at = want;
                    }
                    s1 = c->adpcm_next;
                } else {
                    s1 = s0;
                }
                cur = want;
            }
            if (!silent) {
                const int s = lerp15(s0, s1, c->pos);
                al[k] += MIX_MUL(s, gl);
                ar[k] += MIX_MUL(s, gr);
            }
            c->pos += step;
        }
        break;
        }
    default: /* PSG (channels 8-13) or noise (14-15): half scale */
        for (k = 0; k < count; k++) {
            int s;
            if (c->type == CH_NOISE) {
                uint32_t steps = POS_INT(c->pos + step) - POS_INT(c->pos);
                while (steps--) {
                    if (c->lfsr & 1) {
                        c->lfsr = (uint16_t)((c->lfsr >> 1) ^ 0x6000);
                        c->hold = -1;
                    } else {
                        c->lfsr >>= 1;
                        c->hold = 1;
                    }
                }
                s = c->hold * 16384;
            } else {
                s = (int)(POS_INT(c->pos) & 7) <= c->duty ? 16384 : -16384;
            }
            al[k] += MIX_MUL(s, gl);
            ar[k] += MIX_MUL(s, gr);
            c->pos += step;
        }
        break;
    }
}

/* ---- extended channels -------------------------------------------------------------- */

static void ex_release(Channel *c, int prio)
{
    if (c->ex_active) {
        if (prio >= 0)
            c->prio = prio;
        c->env_status = ENV_RELEASE;
    }
}

static void ex_free(Channel *c)
{
    c->ex_active = 0;
    c->track = -1;
    hw_stop(c);
}

/* A channel for a sequencer voice: free first, else the lowest priority (and oldest) one
 * below `prio`; the mask names the allowed channels. */
static Channel *ex_alloc(uint16_t mask, int prio, int type)
{
    static const uint16_t type_mask[3] = { 0xffff, 0x3f00, 0xc000 };
    Channel *best = NULL;
    int i;
    mask &= type_mask[type] & (uint16_t)~s_locked;
    for (i = 0; i < CHANNELS; i++) {
        Channel *c = &s_ch[i];
        if (!(mask & (1u << i)))
            continue;
        if (!c->ex_active && !c->hw_on)
            return c;
        if (!c->ex_active)
            continue; /* a hardware-only channel (stream): not ours */
        if (!best || c->prio < best->prio ||
            (c->prio == best->prio && (c->env_status == ENV_RELEASE) > (best->env_status == ENV_RELEASE)) ||
            (c->prio == best->prio && c->serial < best->serial))
            best = c;
    }
    if (best && best->prio <= prio)
        return best;
    return NULL;
}

static void ex_update(Channel *c, int do_env)
{
    int32_t db;
    int pitch, pan;
    if (!c->ex_active)
        return;
    if (do_env) {
        switch (c->env_status) {
        case ENV_ATTACK:
            c->env_decay = -((-c->env_decay * c->attack_rate) >> 8);
            if (c->env_decay >= 0) {
                c->env_decay = 0;
                c->env_status = ENV_DECAY;
            }
            break;
        case ENV_DECAY:
            c->env_decay -= c->decay_rate;
            if (c->env_decay <= c->sustain_level) {
                c->env_decay = c->sustain_level;
                c->env_status = ENV_SUSTAIN;
            }
            break;
        case ENV_SUSTAIN:
            break;
        default:
            c->env_decay -= c->release_rate;
            break;
        }
        if (c->env_decay <= ENV_MIN && c->env_status == ENV_RELEASE) {
            ex_free(c);
            return;
        }
    }
    if (!c->hw_on && !c->start) { /* a one-shot wave ran out */
        ex_free(c);
        return;
    }

    /* LFO */
    {
        int lfo = 0;
        if (c->lfo_depth) {
            if (c->lfo_delay_counter < c->lfo_delay) {
                if (do_env)
                    c->lfo_delay_counter++;
            } else {
                lfo = sine_at((int)(c->lfo_counter >> 8)) * c->lfo_depth * c->lfo_range;
                if (do_env)
                    c->lfo_counter += (uint32_t)c->lfo_speed << 6;
            }
        }
        db = s_db_square[c->velocity] + (c->env_decay >> 7) + c->user_decay + c->user_decay2;
        pitch = ((int)c->key - (int)c->original_key) * 64 + c->user_pitch;
        pan = c->init_pan + c->user_pan;
        if (lfo) {
            if (c->lfo_target == 1) /* volume: dB x10 */
                db += (lfo * 60) >> 14;
            else if (c->lfo_target == 2) /* pan */
                pan += lfo >> 8;
            else /* pitch */
                pitch += lfo >> 8;
        }
    }
    /* sweep (portamento) */
    if (c->sweep_pitch && c->sweep_counter < c->sweep_length) {
        pitch += (int)((int64_t)c->sweep_pitch * (c->sweep_length - c->sweep_counter) / c->sweep_length);
        if (do_env && c->auto_sweep)
            c->sweep_counter++;
    }
    if (db < DB_MIN)
        db = DB_MIN;
    if (db > 0)
        db = 0;
    pan += 64;
    hw_set_gain(c, db <= DB_MIN ? 0.0 : pow(10.0, db / 200.0), pan);
    c->rate = c->base_rate * pow(2.0, pitch / 768.0);
    if (c->start) {
        c->start = 0;
        hw_start(c);
    }
}

/* ---- banks and instruments ------------------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int read_inst(const uint8_t *bank, int prg, int key, InstData *out)
{
    uint32_t count, off, type;
    const uint8_t *d;
    if (!bank || prg < 0)
        return 0;
    count = rd32(bank + 0x38);
    if ((uint32_t)prg >= count)
        return 0;
    off = rd32(bank + 0x3c + prg * 4);
    type = off & 0xff;
    d = bank + (off >> 8);
    switch (type) {
    case 1: case 2: case 3: case 4: case 5:
        out->type = (uint8_t)type;
        memcpy(&out->wave, d, 10);
        return 1;
    case 0x10: { /* drum set: min, max, then an entry per key */
        int min = d[0], max = d[1];
        if (key < min || key > max)
            return 0;
        d += 2 + (key - min) * 12;
        out->type = d[0];
        memcpy(&out->wave, d + 2, 10);
        return out->type != 0;
    }
    case 0x11: { /* key split: up to 8 upper keys, then an entry per region */
        int i;
        for (i = 0; i < 8 && d[i]; i++)
            if (key <= d[i])
                break;
        if (i == 8 || !d[i])
            return 0;
        d += 8 + i * 12;
        out->type = d[0];
        memcpy(&out->wave, d + 2, 10);
        return out->type != 0;
    }
    default:
        return 0;
    }
}

/* The wave of a PCM instrument: SNDWaveData {format, loop, rate, timer, loopstart, looplen}
 * then the samples. */
static const uint8_t *inst_wave(const uint8_t *bank, const InstData *inst)
{
    const uint8_t *arc;
    uint32_t count, off;
    if (inst->wave[1] >= 4)
        return NULL;
    if (!rd32(bank + 0x18 + inst->wave[1] * 8))
        return NULL;
    arc = P(rd32(bank + 0x18 + inst->wave[1] * 8));
    count = rd32(arc + 0x38);
    if (inst->wave[0] >= count)
        return NULL;
    off = rd32(arc + 0x3c + inst->wave[0] * 4);
    return off ? arc + off : NULL;
}

/* ---- sequencer ------------------------------------------------------------------------- */

static Player *track_player(const Track *t) { return &s_player[t->player]; }

static void track_release_channels(int ti, int prio)
{
    int i;
    for (i = 0; i < CHANNELS; i++)
        if (s_ch[i].ex_active && s_ch[i].track == ti)
            ex_release(&s_ch[i], prio);
}

static void track_free_channels(int ti)
{
    int i;
    for (i = 0; i < CHANNELS; i++)
        if (s_ch[i].ex_active && s_ch[i].track == ti)
            ex_free(&s_ch[i]);
}

static int track_has_channels(int ti)
{
    int i;
    for (i = 0; i < CHANNELS; i++)
        if (s_ch[i].ex_active && s_ch[i].track == ti)
            return 1;
    return 0;
}

static void track_init(Track *t, int player)
{
    memset(t, 0, sizeof(*t));
    t->flags = TF_ACTIVE | TF_NOTE_WAIT;
    t->player = player;
    t->volume = 127;
    t->volume2 = 127;
    t->pan = 0;
    t->bend_range = 2;
    t->porta_key = 60;
    t->attack = t->decay = t->sustain = t->release = 0xff;
    t->prio = 64;
    t->mod_target = 0;
    t->mod_speed = 16;
    t->mod_range = 1;
    t->mod_delay = 0;
    t->channel_mask = 0xffff;
}

static int track_alloc(void)
{
    int i;
    for (i = 0; i < TRACKS; i++)
        if (!(s_track[i].flags & TF_ACTIVE))
            return i;
    return -1;
}

static void player_stop(Player *p)
{
    int i;
    for (i = 0; i < TRACKS_PER_PLAYER; i++) {
        if (p->tracks[i] != 0xff) {
            track_release_channels(p->tracks[i], 127);
            /* the voices end with their release */
            {
                int k;
                for (k = 0; k < CHANNELS; k++)
                    if (s_ch[k].ex_active && s_ch[k].track == p->tracks[i])
                        s_ch[k].track = -1;
            }
            s_track[p->tracks[i]].flags = 0;
            p->tracks[i] = 0xff;
        }
    }
    p->flags = 0;
}

static void player_prepare(int no, const uint8_t *seq, uint32_t offset, const uint8_t *bank)
{
    Player *p = &s_player[no];
    int ti, i;
    player_stop(p);
    p->my_no = (uint8_t)no;
    p->prio = 64;
    p->volume = 127;
    p->ext_fader = 0;
    p->tempo = 120;
    p->tempo_ratio = 256;
    p->tempo_counter = 240;
    p->bank = bank;
    for (i = 0; i < TRACKS_PER_PLAYER; i++)
        p->tracks[i] = 0xff;
    for (i = 0; i < 16; i++)
        *var_ptr(no, i) = -1;
    if (s_work)
        *(volatile uint32_t *)(s_work + 0x20 + no * 0x24 + 0x20) = 0; /* tick counter */
    ti = track_alloc();
    if (ti < 0)
        return;
    track_init(&s_track[ti], no);
    s_track[ti].base = seq;
    s_track[ti].cur = seq + offset;
    p->tracks[0] = (uint8_t)ti;
    /* a sequence begins by naming its tracks (0xfe mask) and opening them (0x93) */
    {
        const uint8_t *c = s_track[ti].cur;
        if (c[0] == 0xfe) {
            c += 3;
            while (c[0] == 0x93) {
                int no2 = c[1];
                uint32_t off = c[2] | c[3] << 8 | (uint32_t)c[4] << 16;
                int tj = track_alloc();
                c += 5;
                if (tj < 0 || no2 >= TRACKS_PER_PLAYER)
                    continue;
                track_init(&s_track[tj], no);
                s_track[tj].base = seq;
                s_track[tj].cur = seq + off;
                p->tracks[no2] = (uint8_t)tj;
            }
            s_track[ti].cur = c;
        }
    }
    p->flags = PF_PREPARED;
}

static uint32_t read_varlen(const uint8_t **pc)
{
    uint32_t v = 0;
    uint8_t b;
    do {
        b = *(*pc)++;
        v = v << 7 | (b & 0x7f);
    } while (b & 0x80);
    return v;
}

enum { ARG_U8, ARG_S16, ARG_VAR, ARG_RAND, ARG_VARIABLE };

/* An argument of `type`, or the value the prefix (0xa0 random, 0xa1 variable) gives. */
static int32_t read_arg(Track *t, int type, int prefix)
{
    const uint8_t *c = t->cur;
    int32_t v;
    if (prefix == ARG_RAND) {
        int16_t lo = (int16_t)rd16(c), hi = (int16_t)rd16(c + 2);
        t->cur = c + 4;
        return lo + (int32_t)(((int32_t)(hi - lo + 1) * rand16()) >> 16);
    }
    if (prefix == ARG_VARIABLE) {
        int no = *c;
        t->cur = c + 1;
        return *var_ptr(t->player, no);
    }
    switch (type) {
    case ARG_U8: v = *c; t->cur = c + 1; break;
    case ARG_S16: v = (int16_t)rd16(c); t->cur = c + 2; break;
    default: t->cur = c; v = (int32_t)read_varlen(&t->cur); break;
    }
    return v;
}

static void note_on(int ti, int key, int velocity, int32_t length)
{
    Track *t = &s_track[ti];
    Player *p = track_player(t);
    InstData inst;
    Channel *c = NULL;
    int type, prio = p->prio + t->prio;
    const uint8_t *wave = NULL;

    if (key < 0) key = 0;
    if (key > 127) key = 127;
    if (prio > 127) prio = 127;
    s_stats.notes++;

    if (t->flags & TF_TIE) {
        int i;
        for (i = 0; i < CHANNELS; i++)
            if (s_ch[i].ex_active && s_ch[i].track == ti && s_ch[i].env_status != ENV_RELEASE)
                c = &s_ch[i];
        if (c) { /* glide the voice to the new key */
            c->key = (uint8_t)key;
            c->velocity = (uint8_t)velocity;
            c->length = length;
            return;
        }
    }
    if (!read_inst(p->bank, t->prg_no, key, &inst))
        return;
    switch (inst.type) {
    case 1: case 4: type = CH_PCM; wave = inst_wave(p->bank, &inst); if (!wave) return; break;
    case 2: type = CH_PSG; break;
    case 3: type = CH_NOISE; break;
    default: return;
    }
    c = ex_alloc((t->flags & TF_CHANNEL_MASK) ? t->channel_mask : 0xffff, prio, type);
    if (!c)
        return;
    if (c->ex_active || c->hw_on)
        ex_free(c);
    memset(&c->ex_active, 0, sizeof(Channel) - offsetof(Channel, ex_active));
    c->ex_active = 1;
    c->type = type;
    c->track = ti;
    c->prio = prio;
    c->serial = ++s_serial;
    c->key = (uint8_t)key;
    c->original_key = inst.original_key;
    c->velocity = (uint8_t)velocity;
    c->init_pan = inst.pan - 64;
    c->length = length;
    c->attack_rate = attack_rate(t->attack != 0xff ? t->attack : inst.attack);
    c->decay_rate = decay_rate(t->decay != 0xff ? t->decay : inst.decay);
    c->sustain_level = (int32_t)s_db_square[t->sustain != 0xff ? t->sustain : inst.sustain] << 7;
    c->release_rate = decay_rate(t->release != 0xff ? t->release : inst.release);
    c->env_decay = ENV_MIN;
    c->env_status = ENV_ATTACK;
    c->lfo_target = t->mod_target;
    c->lfo_speed = t->mod_speed;
    c->lfo_depth = t->mod_depth;
    c->lfo_range = t->mod_range;
    c->lfo_delay = t->mod_delay;
    c->lfo_delay_counter = 0;
    c->lfo_counter = 0;
    /* sweep: portamento from the last key, plus the track's own sweep pitch */
    c->sweep_pitch = t->sweep_pitch;
    if (t->flags & TF_PORTA)
        c->sweep_pitch += (t->porta_key - key) * 64;
    if (t->porta_time == 0) {
        c->sweep_length = length > 0 ? length : 1;
        c->auto_sweep = 0;
    } else {
        int sp = c->sweep_pitch < 0 ? -c->sweep_pitch : c->sweep_pitch;
        c->sweep_length = (int)(((uint32_t)t->porta_time * t->porta_time * (uint32_t)sp) >> 11);
        if (c->sweep_length < 1)
            c->sweep_length = 1;
        c->auto_sweep = 1;
    }
    c->sweep_counter = 0;
    t->porta_key = (uint8_t)key;
    if (type == CH_PCM) {
        c->format = wave[0];
        c->repeat = wave[1] ? 1 : 2;
        c->data = wave + 12;
        c->loop_start = (uint32_t)rd16(wave + 6) * 4;
        c->end = c->loop_start + rd32(wave + 8) * 4;
        c->base_rate = timer_rate(rd16(wave + 4));
        if (c->format > FMT_ADPCM)
            c->format = FMT_PCM8;
        c->wc = c->format == FMT_ADPCM && c->end > 4 && c->loop_start <= c->end
                    ? wave_cache_get(c->data, c->end, c->loop_start) : NULL;
    } else {
        c->wc = NULL;
        c->format = FMT_PSG;
        c->duty = type == CH_PSG ? inst.wave[0] : 0;
        /* PSG and noise: the key's frequency, 8 steps a period for PSG */
        c->base_rate = 440.0 * pow(2.0, (inst.original_key - 69) / 12.0) * (type == CH_PSG ? 8.0 : 64.0);
    }
    c->start = 1;
    if (s_stats.voices_max < (uint32_t)(c - s_ch) + 1)
        s_stats.voices_max = (uint32_t)(c - s_ch) + 1;
}

/* the track's parameters into its voices, once a frame */
static void track_update_channels(int ti)
{
    Track *t = &s_track[ti];
    Player *p = track_player(t);
    int i;
    int32_t decay = s_db_square[t->volume] + s_db_square[t->volume2] + s_db_square[p->volume] +
                    t->ext_fader + p->ext_fader;
    int pitch = t->pitch_bend * t->bend_range * 64 / 128 + t->ext_pitch;
    int pan = t->pan + t->ext_pan;
    if (decay < DB_MIN)
        decay = DB_MIN;
    for (i = 0; i < CHANNELS; i++) {
        Channel *c = &s_ch[i];
        if (!c->ex_active || c->track != ti)
            continue;
        c->user_decay = decay;
        c->user_pitch = pitch;
        c->user_pan = pan;
        if (t->flags & TF_MUTE)
            c->user_decay2 = DB_MIN;
        else
            c->user_decay2 = 0;
    }
}

/* One tick of a track; returns 0 when it ended. */
static int track_tick(int ti)
{
    Track *t = &s_track[ti];
    Player *p = track_player(t);
    int i;

    for (i = 0; i < CHANNELS; i++) {
        Channel *c = &s_ch[i];
        if (!c->ex_active || c->track != ti)
            continue;
        if (c->length > 0 && --c->length == 0)
            ex_release(c, -1);
        if (!c->auto_sweep && c->sweep_counter < c->sweep_length)
            c->sweep_counter++;
    }
    if (t->flags & TF_NOTE_FINISH_WAIT) {
        if (track_has_channels(ti))
            return 1;
        t->flags &= ~TF_NOTE_FINISH_WAIT;
    }
    if (t->wait > 0 && --t->wait > 0)
        return 1;

    while (t->wait == 0 && !(t->flags & TF_NOTE_FINISH_WAIT)) {
        int prefix = -1, cond = 1;
        uint8_t cmd = *t->cur++;
        if (cmd == 0xa2) {
            cmd = *t->cur++;
            cond = (t->flags & TF_CMP) != 0;
        }
        if (cmd == 0xa0) {
            cmd = *t->cur++;
            prefix = ARG_RAND;
        } else if (cmd == 0xa1) {
            cmd = *t->cur++;
            prefix = ARG_VARIABLE;
        }
        if (cmd < 0x80) { /* note: key, velocity, length */
            int velocity = *t->cur++;
            int32_t length = read_arg(t, ARG_VAR, prefix);
            int key = cmd + t->transpose;
            if (!cond)
                continue;
            if (!(t->flags & TF_MUTE))
                note_on(ti, key, velocity, length > 0 ? length : -1);
            if (t->flags & TF_NOTE_WAIT) {
                t->wait = length;
                if (length == 0)
                    t->flags |= TF_NOTE_FINISH_WAIT;
            }
            continue;
        }
        switch (cmd) {
        case 0x80: { int32_t v = read_arg(t, ARG_VAR, prefix); if (cond) t->wait = v; break; }
        case 0x81: { int32_t v = read_arg(t, ARG_VAR, prefix); if (cond) t->prg_no = (uint16_t)v; break; }
        case 0x93: t->cur += 4; break; /* open track: done at start */
        case 0x94: {
            uint32_t off = t->cur[0] | t->cur[1] << 8 | (uint32_t)t->cur[2] << 16;
            t->cur += 3;
            if (cond)
                t->cur = t->base + off;
            break;
        }
        case 0x95: { uint32_t off = t->cur[0] | t->cur[1] << 8 | (uint32_t)t->cur[2] << 16; t->cur += 3;
                     if (cond && t->depth < 3) { t->call_stack[t->depth++] = t->cur; t->cur = t->base + off; }
                     break; }
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6:
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: {
            int no = *t->cur++;
            int32_t v = read_arg(t, ARG_S16, prefix);
            int16_t *var = var_ptr(t->player, no);
            if (!cond)
                break;
            switch (cmd) {
            case 0xb0: *var = (int16_t)v; break;
            case 0xb1: *var = (int16_t)(*var + v); break;
            case 0xb2: *var = (int16_t)(*var - v); break;
            case 0xb3: *var = (int16_t)(*var * v); break;
            case 0xb4: if (v) *var = (int16_t)(*var / v); break;
            case 0xb5: *var = (int16_t)(v >= 0 ? *var << v : *var >> -v); break;
            case 0xb6: { int neg = v < 0; int32_t r = (int32_t)((rand16() * ((neg ? -v : v) + 1)) >> 16);
                         *var = (int16_t)(neg ? -r : r); break; }
            case 0xb8: t->flags = (t->flags & ~TF_CMP) | (*var == v ? TF_CMP : 0); break;
            case 0xb9: t->flags = (t->flags & ~TF_CMP) | (*var >= v ? TF_CMP : 0); break;
            case 0xba: t->flags = (t->flags & ~TF_CMP) | (*var > v ? TF_CMP : 0); break;
            case 0xbb: t->flags = (t->flags & ~TF_CMP) | (*var <= v ? TF_CMP : 0); break;
            case 0xbc: t->flags = (t->flags & ~TF_CMP) | (*var < v ? TF_CMP : 0); break;
            case 0xbd: t->flags = (t->flags & ~TF_CMP) | (*var != v ? TF_CMP : 0); break;
            }
            break;
        }
        case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: case 0xc5: case 0xc6: case 0xc7:
        case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
        case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: {
            int32_t v = read_arg(t, ARG_U8, prefix);
            if (!cond)
                break;
            switch (cmd) {
            case 0xc0: t->pan = (int8_t)(v - 64); break;
            case 0xc1: t->volume = (uint8_t)v; break;
            case 0xc2: p->volume = (uint8_t)v; break;
            case 0xc3: t->transpose = (int8_t)v; break;
            case 0xc4: t->pitch_bend = (int8_t)v; break;
            case 0xc5: t->bend_range = (uint8_t)v; break;
            case 0xc6: t->prio = (uint8_t)v; break;
            case 0xc7: t->flags = (t->flags & ~TF_NOTE_WAIT) | (v ? TF_NOTE_WAIT : 0); break;
            case 0xc8: t->flags = (t->flags & ~TF_TIE) | (v ? TF_TIE : 0); track_release_channels(ti, -1); break;
            case 0xc9: t->porta_key = (uint8_t)(v + t->transpose); t->flags |= TF_PORTA; break;
            case 0xca: t->mod_depth = (uint8_t)v; break;
            case 0xcb: t->mod_speed = (uint8_t)v; break;
            case 0xcc: t->mod_target = (uint8_t)v; break;
            case 0xcd: t->mod_range = (uint8_t)v; break;
            case 0xce: t->flags = (t->flags & ~TF_PORTA) | (v ? TF_PORTA : 0); break;
            case 0xcf: t->porta_time = (uint8_t)v; break;
            case 0xd0: t->attack = (uint8_t)v; break;
            case 0xd1: t->decay = (uint8_t)v; break;
            case 0xd2: t->sustain = (uint8_t)v; break;
            case 0xd3: t->release = (uint8_t)v; break;
            case 0xd4: if (t->depth < 3) { t->call_stack[t->depth] = t->cur; t->loop_count[t->depth] = (uint8_t)v; t->depth++; } break;
            case 0xd5: t->volume2 = (uint8_t)v; break;
            default: break; /* 0xd6 print variable */
            }
            break;
        }
        case 0xe0: case 0xe1: case 0xe3: {
            int32_t v = read_arg(t, ARG_S16, prefix);
            if (!cond)
                break;
            if (cmd == 0xe0) t->mod_delay = (uint16_t)v;
            else if (cmd == 0xe1) p->tempo = (uint16_t)v;
            else t->sweep_pitch = (int16_t)v;
            break;
        }
        case 0xfc: /* loop end */
            if (cond && t->depth > 0) {
                uint8_t *n = &t->loop_count[t->depth - 1];
                if (*n == 1) {
                    t->depth--;
                } else {
                    if (*n)
                        (*n)--;
                    t->cur = t->call_stack[t->depth - 1];
                }
            }
            break;
        case 0xfd: /* return */
            if (cond && t->depth > 0)
                t->cur = t->call_stack[--t->depth];
            break;
        case 0xfe: t->cur += 2; break;
        case 0xff:
            return 0;
        default:
            s_stats.unknown_seq++;
            return 0;
        }
    }
    return 1;
}

static void player_tick(int no)
{
    Player *p = &s_player[no];
    int i, alive = 0;
    for (i = 0; i < TRACKS_PER_PLAYER; i++) {
        int ti = p->tracks[i];
        if (ti == 0xff)
            continue;
        if (track_tick(ti)) {
            alive = 1;
        } else {
            /* an ended track lets its notes play out */
            int k;
            for (k = 0; k < CHANNELS; k++)
                if (s_ch[k].ex_active && s_ch[k].track == ti) {
                    if (s_ch[k].length < 0)
                        ex_release(&s_ch[k], -1);
                    s_ch[k].track = -1;
                }
            s_track[ti].flags = 0;
            p->tracks[i] = 0xff;
        }
    }
    if (s_work)
        (*(volatile uint32_t *)(s_work + 0x20 + no * 0x24 + 0x20))++;
    if (!alive)
        player_stop(p);
}

/* one driver frame: sequencer ticks by tempo, then every voice's envelope and parameters */
static void frame(void)
{
    int i, j;
    for (i = 0; i < PLAYERS; i++) {
        Player *p = &s_player[i];
        if (!(p->flags & PF_ACTIVE) || (p->flags & PF_PAUSE))
            continue;
        while (p->tempo_counter >= 240) {
            p->tempo_counter -= 240;
            player_tick(i);
            if (!(p->flags & PF_ACTIVE))
                break;
        }
        p->tempo_counter = (uint16_t)(p->tempo_counter + ((uint32_t)p->tempo * p->tempo_ratio >> 8));
    }
    for (j = 0; j < TRACKS; j++)
        if (s_track[j].flags & TF_ACTIVE)
            track_update_channels(j);
    for (i = 0; i < CHANNELS; i++)
        ex_update(&s_ch[i], 1);
    work_update_status();
}

/* ---- commands --------------------------------------------------------------------------- */

static void set_param(void *base, uint32_t offset, uint32_t value, uint32_t size)
{
    if (offset + size > 0x20)
        return;
    if (size == 1) *((uint8_t *)base + offset) = (uint8_t)value;
    else if (size == 2) { uint16_t v = (uint16_t)value; memcpy((uint8_t *)base + offset, &v, 2); }
    else if (size == 4) memcpy((uint8_t *)base + offset, &value, 4);
}

static void setup_pcm(const DsCommand *c)
{
    Channel *ch = &s_ch[c->arg[0] & 15];
    uint32_t loop_start = c->arg[3] & 0xffff, loop_len = c->arg[2] & 0x3fffff;
    if (ch->ex_active)
        ex_free(ch);
    hw_stop(ch);
    ch->type = CH_PCM;
    ch->wc = NULL; /* the ARM9's own wave (a stream's ring, perhaps): decoded as it plays */
    ch->format = (c->arg[3] >> 24) & 3;
    ch->repeat = (c->arg[3] >> 26) & 3;
    ch->data = P(c->arg[1]);
    ch->loop_start = loop_start * 4;
    ch->end = (loop_start + loop_len) * 4;
    ch->reg_volume = (c->arg[2] >> 24) & 0x7f;
    ch->reg_shift = (c->arg[2] >> 22) & 3;
    ch->reg_pan = (c->arg[3] >> 16) & 0x7f;
    ch->reg_timer = c->arg[0] >> 16;
    ch->rate = timer_rate(ch->reg_timer);
    hw_reg_gain(ch);
}

static void process_command(const DsCommand *c)
{
    int i;
    s_stats.commands++;
    switch (c->id) {
    case CMD_START_SEQ:
    case CMD_PREPARE_SEQ:
        if (c->arg[0] < PLAYERS && c->arg[1]) {
            player_prepare((int)c->arg[0], P(c->arg[1]), c->arg[2], c->arg[3] ? P(c->arg[3]) : NULL);
            if (c->id == CMD_START_SEQ)
                s_player[c->arg[0]].flags |= PF_ACTIVE;
            s_stats.seq_starts++;
        }
        break;
    case CMD_START_PREPARED_SEQ:
        if (c->arg[0] < PLAYERS && (s_player[c->arg[0]].flags & PF_PREPARED))
            s_player[c->arg[0]].flags |= PF_ACTIVE;
        break;
    case CMD_STOP_SEQ:
        if (c->arg[0] < PLAYERS)
            player_stop(&s_player[c->arg[0]]);
        break;
    case CMD_PAUSE_SEQ:
        if (c->arg[0] < PLAYERS) {
            Player *p = &s_player[c->arg[0]];
            if (c->arg[1]) {
                p->flags |= PF_PAUSE;
                for (i = 0; i < TRACKS_PER_PLAYER; i++)
                    if (p->tracks[i] != 0xff)
                        track_release_channels(p->tracks[i], 127);
            } else {
                p->flags &= ~PF_PAUSE;
            }
        }
        break;
    case CMD_SKIP_SEQ:
        if (c->arg[0] < PLAYERS) {
            uint32_t n;
            for (n = 0; n < c->arg[1] && (s_player[c->arg[0]].flags & PF_ACTIVE); n++)
                player_tick((int)c->arg[0]);
        }
        break;
    case CMD_PLAYER_PARAM:
        if (c->arg[0] < PLAYERS)
            set_param(&s_player[c->arg[0]], c->arg[1], c->arg[2], c->arg[3]);
        break;
    case CMD_TRACK_PARAM: {
        int pl = c->arg[0] & 0xff, size = c->arg[0] >> 24;
        if (pl >= PLAYERS)
            break;
        for (i = 0; i < TRACKS_PER_PLAYER; i++)
            if ((c->arg[1] & (1u << i)) && s_player[pl].tracks[i] != 0xff)
                set_param(&s_track[s_player[pl].tracks[i]], c->arg[2], c->arg[3], (uint32_t)size);
        break;
    }
    case CMD_MUTE_TRACK: {
        int pl = (int)c->arg[0];
        if (pl >= PLAYERS)
            break;
        for (i = 0; i < TRACKS_PER_PLAYER; i++) {
            int ti = s_player[pl].tracks[i];
            if (ti == 0xff || !(c->arg[1] & (1u << i)))
                continue;
            if (c->arg[2]) {
                s_track[ti].flags |= TF_MUTE;
                if (c->arg[2] == 2) track_release_channels(ti, 127);
                if (c->arg[2] == 3) track_free_channels(ti);
            } else {
                s_track[ti].flags &= ~TF_MUTE;
            }
        }
        break;
    }
    case CMD_ALLOCATABLE_CHANNEL: {
        int pl = (int)c->arg[0];
        if (pl >= PLAYERS)
            break;
        for (i = 0; i < TRACKS_PER_PLAYER; i++) {
            int ti = s_player[pl].tracks[i];
            if (ti != 0xff && (c->arg[1] & (1u << i))) {
                s_track[ti].channel_mask = (uint16_t)c->arg[2];
                s_track[ti].flags |= TF_CHANNEL_MASK;
            }
        }
        break;
    }
    case CMD_PLAYER_LOCAL_VAR:
        if (c->arg[0] < PLAYERS && c->arg[1] < 16)
            *var_ptr((int)c->arg[0], (int)c->arg[1]) = (int16_t)c->arg[2];
        break;
    case CMD_PLAYER_GLOBAL_VAR:
        if (c->arg[0] < 16)
            *var_ptr(0, 16 + (int)c->arg[0]) = (int16_t)c->arg[1];
        break;
    case CMD_START_TIMER:
        for (i = 0; i < CHANNELS; i++)
            if (c->arg[0] & (1u << i))
                hw_start(&s_ch[i]);
        for (i = 0; i < ALARMS; i++)
            if (c->arg[2] & (1u << i)) {
                Alarm *a = &s_alarm[i];
                a->active = 1;
                a->id = s_alarm_setup[i].id;
                a->next = s_time + s_alarm_setup[i].tick / ALARM_HZ;
                a->period = s_alarm_setup[i].period / ALARM_HZ;
            }
        break;
    case CMD_STOP_TIMER:
        for (i = 0; i < CHANNELS; i++)
            if (c->arg[0] & (1u << i))
                hw_stop(&s_ch[i]);
        for (i = 0; i < ALARMS; i++)
            if (c->arg[2] & (1u << i))
                s_alarm[i].active = 0;
        break;
    case CMD_SETUP_CHANNEL_PCM:
        setup_pcm(c);
        break;
    case CMD_SETUP_CHANNEL_PSG:
    case CMD_SETUP_CHANNEL_NOISE: {
        Channel *ch = &s_ch[c->arg[0] & 15];
        if (ch->ex_active)
            ex_free(ch);
        ch->type = c->id == CMD_SETUP_CHANNEL_PSG ? CH_PSG : CH_NOISE;
        ch->wc = NULL;
        ch->format = FMT_PSG;
        ch->duty = (int)c->arg[1] & 7;
        ch->reg_volume = (int)(c->arg[2] & 0x7f);
        ch->reg_shift = (int)((c->arg[2] >> 8) & 3);
        ch->reg_timer = c->arg[3] & 0xffff;
        ch->reg_pan = (int)((c->arg[3] >> 16) & 0x7f);
        ch->rate = timer_rate(ch->reg_timer);
        hw_reg_gain(ch);
        break;
    }
    case CMD_SETUP_ALARM:
        if (c->arg[0] < ALARMS) {
            s_alarm_setup[c->arg[0]].tick = c->arg[1];
            s_alarm_setup[c->arg[0]].period = c->arg[2];
            s_alarm_setup[c->arg[0]].id = c->arg[3];
        }
        break;
    case CMD_CHANNEL_TIMER:
        for (i = 0; i < CHANNELS; i++)
            if (c->arg[0] & (1u << i)) {
                s_ch[i].reg_timer = c->arg[1];
                s_ch[i].rate = timer_rate(c->arg[1]);
            }
        break;
    case CMD_CHANNEL_VOLUME:
        for (i = 0; i < CHANNELS; i++)
            if (c->arg[0] & (1u << i)) {
                s_ch[i].reg_volume = (int)(c->arg[1] & 0x7f);
                s_ch[i].reg_shift = (int)(c->arg[2] & 3);
                hw_reg_gain(&s_ch[i]);
            }
        break;
    case CMD_CHANNEL_PAN:
        for (i = 0; i < CHANNELS; i++)
            if (c->arg[0] & (1u << i)) {
                s_ch[i].reg_pan = (int)(c->arg[1] & 0x7f);
                hw_reg_gain(&s_ch[i]);
            }
        break;
    case CMD_MASTER_VOLUME:
        s_master_volume = (int)(c->arg[0] & 0x7f);
        break;
    case CMD_LOCK_CHANNEL:
        for (i = 0; i < CHANNELS; i++)
            if ((c->arg[0] & (1u << i)) && s_ch[i].ex_active)
                ex_free(&s_ch[i]);
        s_locked |= (uint16_t)c->arg[0];
        break;
    case CMD_UNLOCK_CHANNEL:
        s_locked &= (uint16_t)~c->arg[0];
        break;
    case CMD_STOP_UNLOCKED_CHANNEL:
        for (i = 0; i < CHANNELS; i++)
            if ((c->arg[0] & (1u << i)) && !(s_locked & (1u << i))) {
                if (s_ch[i].ex_active)
                    ex_free(&s_ch[i]);
                hw_stop(&s_ch[i]);
            }
        break;
    case CMD_SHARED_WORK:
        s_work = c->arg[0] ? (uint8_t *)P(c->arg[0]) : NULL;
        break;
    case CMD_INVALIDATE_SEQ:
    case CMD_INVALIDATE_BANK:
    case CMD_INVALIDATE_WAVE: {
        /* stop what reads from the range the ARM9 is about to reuse */
        const uintptr_t lo = (uintptr_t)P(c->arg[0]), hi = (uintptr_t)P(c->arg[1]);
        for (i = 0; i < PLAYERS; i++) {
            Player *p = &s_player[i];
            int k, hit = 0;
            if (!p->flags)
                continue;
            if (c->id == CMD_INVALIDATE_BANK)
                hit = (uintptr_t)p->bank >= lo && (uintptr_t)p->bank < hi;
            else if (c->id == CMD_INVALIDATE_SEQ)
                for (k = 0; k < TRACKS_PER_PLAYER; k++)
                    if (p->tracks[k] != 0xff && (uintptr_t)s_track[p->tracks[k]].cur >= lo &&
                        (uintptr_t)s_track[p->tracks[k]].cur < hi)
                        hit = 1;
            if (hit)
                player_stop(p);
        }
        for (i = 0; i < CHANNELS; i++)
            if (s_ch[i].hw_on && (uintptr_t)s_ch[i].data >= lo && (uintptr_t)s_ch[i].data < hi) {
                if (s_ch[i].ex_active)
                    ex_free(&s_ch[i]);
                hw_stop(&s_ch[i]);
            }
        break;
    }
    case CMD_SETUP_CAPTURE: case CMD_SURROUND_DECAY: case CMD_MASTER_PAN:
    case CMD_OUTPUT_SELECTOR: case CMD_READ_DRIVER_INFO:
        break;
    default:
        s_stats.unknown_cmd++;
        break;
    }
}

static void process_list(const List *l)
{
    int i;
    tables_init();
    for (i = 0; i < l->count; i++)
        process_command(&l->cmd[i]);
    work_update_status();
}

/* ---- output ------------------------------------------------------------------------------ */

static void alarms_run(void)
{
    int i;
    for (i = 0; i < ALARMS; i++) {
        Alarm *a = &s_alarm[i];
        if (!a->active || s_time < a->next)
            continue;
        if (snd7_send_to_arm9)
            snd7_send_to_arm9((uint32_t)i | (a->id & 0xff) << 8);
        s_stats.alarms++;
        if (a->period > 0)
            a->next += a->period;
        else
            a->active = 0;
    }
}

/* Mixer level: a full-volume channel panned centre gives half scale on each side, as on the
 * DS; past 0.75 the sum is bent smoothly towards full scale instead of clipped, so that busy
 * scenes neither crackle nor have to be mixed quieter. */
#define MIX_GAIN 1.0f
volatile float snd7_port_volume = 1.0f;
int snd7_rate = SND7_RATE;
#define KNEE 0.75f
/* the most samples mixed at once (a driver frame is about 250 at 48 kHz) */
#define MIX_MAX 512

static inline float soft_limit(float x)
{
    const float a = x < 0 ? -x : x;
    float y;
    if (a <= KNEE)
        return x;
    y = KNEE + (1.0f - KNEE) * tanhf((a - KNEE) / (1.0f - KNEE));
    return x < 0 ? -y : y;
}

void snd7_render(int16_t *out, int frames)
{
    const double dt = 1.0 / snd7_rate;
    uint64_t (*const clock)(void) = snd7_clock_us;
    uint64_t t = clock ? clock() : 0, t1;
    int n = 0;
    tables_init();
    drain_queue();
    s_stats.renders++;
    s_stats.samples += (uint32_t)frames;
    while (n < frames) {
        /* run driver frames due, then mix until the next one */
        int chunk, i, k;
        while (s_frame_acc <= 0) {
            frame();
            s_frame_acc += 1.0 / FRAME_HZ;
        }
        alarms_run();
        if (clock) {
            t1 = clock();
            s_stats.frame_us += (uint32_t)(t1 - t);
            t = t1;
        }
        chunk = (int)ceil(s_frame_acc / dt);
        if (chunk < 1)
            chunk = 1;
        if (chunk > frames - n)
            chunk = frames - n;
        if (chunk > MIX_MAX)
            chunk = MIX_MAX;
        {
            static int32_t al[MIX_MAX], ar[MIX_MAX];
            /* full scale at 2^23 (mix_channel) */
            const float mv = (float)s_master_volume / 127.0f * MIX_GAIN * snd7_port_volume *
                             (1.0f / 8388608.0f);
            memset(al, 0, sizeof(al[0]) * (size_t)chunk);
            memset(ar, 0, sizeof(ar[0]) * (size_t)chunk);
            for (i = 0; i < CHANNELS; i++) {
                Channel *c = &s_ch[i];
                if (!c->hw_on || (!c->data && c->format != FMT_PSG))
                    continue;
                mix_channel(c, al, ar, chunk, (uint64_t)(c->rate * dt * POS_ONE + 0.5));
                s_stats.channel_samples += (uint32_t)chunk;
            }
            if (clock) {
                t1 = clock();
                s_stats.mix_us += (uint32_t)(t1 - t);
                t = t1;
            }
            {
                /* the chunk's peak: below the knee (nearly always), a plain integer scaling;
                 * the limiter's float maths on every sample was a third of the render */
                int32_t peak = 0;
                for (k = 0; k < chunk; k++) {
                    const int32_t a = al[k] < 0 ? -al[k] : al[k], b = ar[k] < 0 ? -ar[k] : ar[k];
                    peak = a > peak ? a : peak;
                    peak = b > peak ? b : peak;
                }
                if ((float)peak * mv <= KNEE) {
                    /* mv is about 2^-23: the factor in 32 fractional bits keeps 24 of it */
                    const int64_t m = (int64_t)((double)mv * 32767.0 * 4294967296.0);
                    for (k = 0; k < chunk; k++) {
                        out[(n + k) * 2] = (int16_t)((al[k] * m) / 4294967296LL);
                        out[(n + k) * 2 + 1] = (int16_t)((ar[k] * m) / 4294967296LL);
                    }
                } else {
                    for (k = 0; k < chunk; k++) {
                        int sl, sr;
                        const float l = soft_limit((float)al[k] * mv);
                        const float r = soft_limit((float)ar[k] * mv);
                        sl = (int)(l * 32767.0f);
                        sr = (int)(r * 32767.0f);
                        out[(n + k) * 2] = (int16_t)sl;
                        out[(n + k) * 2 + 1] = (int16_t)sr;
                    }
                }
            }
        }
        n += chunk;
        s_time += chunk * dt;
        s_frame_acc -= chunk * dt;
        if (clock) {
            t1 = clock();
            s_stats.out_us += (uint32_t)(t1 - t);
            t = t1;
        }
    }
}

void snd7_take_stats(Snd7Stats *out)
{
    *out = s_stats;
    memset(&s_stats, 0, sizeof(s_stats));
}
