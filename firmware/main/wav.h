#pragma once

#include <stddef.h>
#include <stdint.h>

/* Portable WAV helpers (no IDF dependencies; candidate for onurb_core host tests). */

#define WAV_HEADER_SIZE 44

/* Write a canonical 44-byte PCM s16le header for `data_bytes` of audio. */
void wav_write_header(uint8_t *out, uint32_t sample_rate, uint16_t channels, uint32_t data_bytes);

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits;
    const uint8_t *data;
    size_t data_bytes;
} wav_info_t;

/* Parse a RIFF/WAVE PCM buffer, skipping unknown chunks. Returns 0 on success,
 * negative on malformed or non-PCM input. data_bytes is clamped to the buffer. */
int wav_parse(const uint8_t *buf, size_t len, wav_info_t *out);
