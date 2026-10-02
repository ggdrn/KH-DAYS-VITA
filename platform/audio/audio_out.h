/* Sound output on the Vita: the audio thread that runs the ARM7 sound driver (snd7.c) and
 * feeds sceAudioOut, 48 kHz stereo. */
#ifndef KH_AUDIO_OUT_H
#define KH_AUDIO_OUT_H

/* Open the output and start the thread; without it the driver acknowledges commands at once
 * and the game runs silent. */
void audio_out_init(void);

#endif
