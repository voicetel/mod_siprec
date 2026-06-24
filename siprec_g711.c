/*
 * siprec_g711.c — G.711 reference encoders + lookup-table build.
 *
 * The reference encoders here were the original inline hot-path
 * encoders in siprec_media.c. They are now the source of truth used
 * to populate the lookup tables the media-bug callback actually
 * uses (see siprec_g711.h for the why / measurements). Keeping them
 * here, FreeSWITCH-free, lets the unit test verify table == ref for
 * all 65536 inputs.
 */
#include "siprec_g711.h"

uint8_t siprec_g711_ulaw_tab[65536];
uint8_t siprec_g711_alaw_tab[65536];

uint8_t siprec_l16_to_ulaw_ref(int16_t pcm)
{
    /* Standard µ-law encoder (G.711). Bias 0x84, exponent cap 7.
     * Matches libsndfile's lookup-free version.
     *
     * Promote to int before negation so INT16_MIN (-32768) doesn't
     * overflow: -INT16_MIN = 32768, which fits int but not int16_t
     * (max 32767). Storing the result back into an int16_t and
     * truncating is implementation-defined per C11 §6.3.1.3 —
     * gcc/clang both wrap to -32768 again, producing the wrong
     * PCMU code for one sample value. */
    const int BIAS = 0x84;
    const int CLIP = 32635;

    int p    = pcm;
    int sign = (p < 0) ? 0x80 : 0;
    int exponent = 7;
    int mantissa;
    if (sign) p = -p;
    if (p > CLIP) p = CLIP;
    p += BIAS;

    for (int mask = 0x4000; (p & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    mantissa = (p >> ((exponent == 0) ? 4 : (exponent + 3))) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

uint8_t siprec_l16_to_alaw_ref(int16_t pcm)
{
    /* Same INT16_MIN consideration as the µ-law encoder — promote
     * before negation. The A-law negative-side adjustment is
     * `-p - 1` (rather than just `-p`) per G.711's symmetric-
     * around-zero quantisation. */
    int p    = pcm;
    int sign = (p < 0) ? 0x80 : 0;
    int exponent = 7;
    int mantissa;
    uint8_t alaw;
    if (sign) p = -p - 1;
    if (p > 32767) p = 32767;

    for (int mask = 0x4000; (p & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    mantissa = (exponent < 1)
        ? (p >> 4) & 0x0F
        : (p >> (exponent + 3)) & 0x0F;
    alaw = (uint8_t)((exponent << 4) | mantissa);
    if (sign) alaw |= 0x80;
    return alaw ^ 0x55; /* per G.711 spec */
}

void siprec_g711_init(void)
{
    static int built = 0;
    int v;
    if (built) {
        return;
    }
    for (v = -32768; v <= 32767; v++) {
        uint16_t idx = (uint16_t)(int16_t)v;
        siprec_g711_ulaw_tab[idx] = siprec_l16_to_ulaw_ref((int16_t)v);
        siprec_g711_alaw_tab[idx] = siprec_l16_to_alaw_ref((int16_t)v);
    }
    built = 1;
}
