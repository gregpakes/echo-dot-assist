/* Wake word clips (experimental, setting "wake_clips", off by default; fork gregpakes/echo-dot-assist): each request
 * that a local wake word started, from just before the wake word to the end of the request, sent as a WAV to a
 * collector on the network for voice isolation experiments (who said the wake word, and focusing on them).  What
 * Home Assistant gets does not change.  The audio is the front end's micAsr as the wake word engine hears it, before
 * the pipeline's gain and noise reduction; the headers say where the wake word is and where Home Assistant's stream
 * began.  Capture thread only, except wakeclip_enable. */
#ifndef WAKECLIP_H
#define WAKECLIP_H
#include <stddef.h>
#include <stdint.h>

/* Collector, host:port.  HASSMIC_WAKE_CLIP_TARGET overrides it (PC tests) */
#ifndef WAKECLIP_TARGET
#define WAKECLIP_TARGET "172.16.51.78:10301"
#endif

int  wakeclip_enable(int on);           /* -1 reads */

/* A request starts streaming.  pre: the ring's samples from before the wake word up to now, n of them; word_begin,
 * word_end, stream_from: offsets into pre (samples) of the wake word as Pryon heard it and of the first sample Home
 * Assistant got.  keyword_db: the wake word's level. */
void wakeclip_begin(const int16_t *pre, size_t n, size_t word_begin, size_t word_end, size_t stream_from, float keyword_db,
                    const char *keyword);
void wakeclip_add(const int16_t *pcm, size_t n);  /* the request's audio as it streams */
void wakeclip_end(void);                          /* the stream ended: sends it (in the background) */
int  wakeclip_active(void);
#endif
