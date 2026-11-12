/* JSRF sounds (m7): the WAV files inside the game's .dat containers.
 *
 * A sound is an item of a NORM (formats learned from JSRF ModTool by
 * neodoso, no code taken): a whole RIFF WAV, either 16-bit PCM or Xbox
 * ADPCM (format 0x69: blocks of 36 bytes a channel, a 4-byte header, the
 * first sample and the step index, then 64 four-bit steps: 65 samples a
 * block, as the recomp's own mixer plays them). Replacing one rewrites the
 * item and moves everything after it, as the game's containers expect: the
 * NORM's child (end, size) and the later children, the MULT's entries. */
#ifndef JSND_H
#define JSND_H
#include <stddef.h>
#include <stdint.h>

enum { JS_PCM = 1, JS_XADPCM = 0x69, JS_ADX = 0x10ADC };   /* m14: JS_ADX: a whole .adx file (music) */

typedef struct {
    size_t item, len;           /* the item: the RIFF, its bytes */
    size_t ent;                 /* its 16-byte entry in the NORM */
    size_t norm, norm_end;      /* the NORM it is in */
    int fmt, ch, rate, bits;    /* fmt: JS_PCM, JS_XADPCM, or another tag (shown, not decoded) */
    size_t data, dlen;          /* the samples */
    int frames;                 /* samples a channel */
    int loop, loop_start, loop_end;   /* m14 (ADX): looped, where (samples) */
} JSnd;

typedef struct {
    JSnd *s;
    int n;
} JSounds;

/* a .dat's sounds, or (m14) an .adx: one sound, the whole file */
int js_scan(const uint8_t *buf, size_t size, JSounds *out);
void js_free(JSounds *ss);
const char *js_fmt_name(int fmt);

/* the sound as 16-bit samples, channels interleaved (malloc'd); 0 done */
int js_decode(const uint8_t *buf, const JSnd *s, int16_t **pcm, int *frames);

/* a WAV file read: any PCM (8, 16, 24, 32 bits, or 32-bit float), as 16-bit;
 * 0 done, else err says why */
int js_read_wav(const char *path, int16_t **pcm, int *frames, int *ch, int *rate, char *err, size_t errn);
int js_write_wav(const char *path, const int16_t *pcm, int frames, int ch, int rate);

/* the item made again from pcm (ch, rate as given) in the format of s,
 * resampled and mixed to its rate and channels; the file rebuilt with it
 * (nb, nsize malloc'd); 0 done */
int js_replace(const uint8_t *buf, size_t size, const JSnd *s, const int16_t *pcm, int frames, int ch, int rate,
               uint8_t **nb, size_t *nsize);

#endif
