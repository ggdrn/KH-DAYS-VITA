/* Sound output on the Vita: the audio thread that runs the ARM7 sound driver (snd7.c) and
 * feeds sceAudioOut, 48 kHz stereo. */
#ifndef KH_AUDIO_OUT_H
#define KH_AUDIO_OUT_H

#include <stdint.h>

/* Open the output and start the thread; without it the driver acknowledges commands at once
 * and the game runs silent. */
void audio_out_init(void);
/* since the last call: the audio thread's run time inside sceAudioOutOutput and the wall
 * time there (us) */
void audio_out_take_stats(uint32_t *output_run_us, uint32_t *output_wall_us);

#endif
