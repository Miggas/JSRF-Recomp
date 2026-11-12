/* JSRF music (m14): CRI ADX files (Media/Z_ADX: BGM, Env, DJ), written
 * from the format's public description (no code taken from anywhere).
 *
 * Big-endian header: 0x8000, the copyright offset (the samples start 4
 * after it, "(c)CRI" just before), the encoding (3: ADX), the block size
 * (18), 4 bits a sample, the channels, the rate, the total samples, the
 * high-pass cutoff (it sets the predictor), the version (3 or 4), flags
 * (encryption), then the loop: version 3 at 0x18 (enabled, start sample,
 * start byte, end sample, end byte), version 4 the same at 0x24. The
 * samples: blocks of 18 bytes a channel, interleaved: a 16-bit scale, then
 * 32 four-bit steps, each predicted from the two before it. */
#ifndef JADX_H
#define JADX_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int ch, rate, frames, version, flags, cutoff;
    size_t data;                /* where the samples start */
    int loop, loop_at;          /* looped; where its loop words are (0: none) */
    int loop_start, loop_end;   /* samples */
} JAdx;

/* 0 when buf is an ADX the tool can read (standard ADX, not encrypted) */
int adx_parse(const uint8_t *buf, size_t size, JAdx *a);
/* the first bytes of a file (n of them) look like an ADX the tool can read */
int adx_head_ok(const uint8_t *head, size_t n);
/* its samples, 16-bit, channels interleaved (malloc'd); 0 done */
int adx_decode(const uint8_t *buf, size_t size, const JAdx *a, int16_t **pcm);
/* a new file: the original's header (kept: its rate, channels, cutoff,
 * version), pcm (ch, rate as given, made the original's) encoded; a looped
 * one loops the whole new piece. nb, nsize malloc'd; 0 done */
int adx_build(const uint8_t *buf, size_t size, const JAdx *a, const int16_t *pcm, int frames, int ch, int rate,
              uint8_t **nb, size_t *nsize);

#endif
