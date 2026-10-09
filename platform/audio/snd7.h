/* The ARM7's sound driver, high-level: what the NitroSDK's ARM7 SND library does with the
 * command lists the ARM9 sends over PXI tag 7. Sequences (SSEQ) are played with their banks
 * (SBNK) and wave archives (SWAR) where the game loaded them, on 16 channels (PCM8, PCM16,
 * IMA-ADPCM, PSG, noise) mixed to stereo 48 kHz; channels the ARM9 sets up itself (NNS sound
 * streams) and alarms work as on the DS.
 *
 * Plain C: the Vita side (platform/audio/audio_out.c) runs snd7_render on an audio thread and
 * forwards PXI words to snd7_pxi; tools/snd_test drives it on the host. */
#ifndef KH_AUDIO_SND7_H
#define KH_AUDIO_SND7_H

#include <stdint.h>

#define SND7_RATE 48000

/* A word the ARM9 sent on PXI tag 7: the head of a command list (0: "process now"). The list
 * is copied at once; its finished tag advances once snd7_render has processed it. */
void snd7_pxi(uint32_t word);

/* The sound output, interleaved stereo, `frames` frames; runs the driver's 192 Hz frames in
 * step. From one thread only. */
void snd7_render(int16_t *out, int frames);

/* Whether snd7_render is being called (the audio thread runs). Without it, command lists are
 * acknowledged at once so that the game never waits on them. */
void snd7_set_running(int running);

/* Where DS-side 32-bit pointers point: 0 on the Vita (they are addresses); the base of an
 * arena on a 64-bit host (tools/snd_test). */
extern uintptr_t snd7_ptr_base;

/* The port's own volume over the game's, 0-1 (the port menu). */
extern volatile float snd7_port_volume;

/* Hooks the host provides: deliver a word to the ARM9 on PXI tag 7 (alarms). */
extern void (*snd7_send_to_arm9)(uint32_t word);

/* A microsecond clock for the render's own timing (the log), NULL for none (tools/snd_test). */
extern uint64_t (*snd7_clock_us)(void);

typedef struct {
    uint32_t lists, commands, notes, seq_starts, alarms, voices_max;
    uint32_t unknown_cmd, unknown_seq;
    /* snd7_render by step (us): the driver's frames (sequencer, envelopes, commands), the
     * channels' mixing, the limiter and the conversion; samples rendered and channels mixed
     * (one channel over one chunk counts its samples) */
    uint32_t frame_us, mix_us, out_us, samples, channel_samples, renders;
} Snd7Stats;
void snd7_take_stats(Snd7Stats *out);

#endif
