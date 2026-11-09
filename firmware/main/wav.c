#include <string.h>

#include "wav.h"

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (v >> (8 * i)) & 0xFF;
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void wav_write_header(uint8_t *out, uint32_t sample_rate, uint16_t channels, uint32_t data_bytes)
{
    memcpy(out, "RIFF", 4);
    put_u32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);
    put_u32(out + 16, 16);
    put_u16(out + 20, 1);                               /* PCM */
    put_u16(out + 22, channels);
    put_u32(out + 24, sample_rate);
    put_u32(out + 28, sample_rate * channels * 2);      /* byte rate */
    put_u16(out + 32, channels * 2);                    /* block align */
    put_u16(out + 34, 16);
    memcpy(out + 36, "data", 4);
    put_u32(out + 40, data_bytes);
}

int wav_parse(const uint8_t *buf, size_t len, wav_info_t *out)
{
    size_t pos = 12;
    int have_fmt = 0;

    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return -1;
    }
    while (pos + 8 <= len) {
        uint32_t size = get_u32(buf + pos + 4);
        const uint8_t *body = buf + pos + 8;
        size_t avail = len - pos - 8;

        if (memcmp(buf + pos, "fmt ", 4) == 0) {
            if (size < 16 || avail < 16 || get_u16(body) != 1) {
                return -2;                                  /* not PCM */
            }
            out->channels = get_u16(body + 2);
            out->sample_rate = get_u32(body + 4);
            out->bits = get_u16(body + 14);
            have_fmt = 1;
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            if (!have_fmt) {
                return -3;
            }
            out->data = body;
            out->data_bytes = size < avail ? size : avail;
            return 0;
        }
        if (size > avail) {
            break;
        }
        pos += 8 + size + (size & 1);
    }
    return -4;
}
