/*
 * siprec_g711.h — G.711 (PCMU/PCMA) encoders for the media fork.
 *
 * Two layers:
 *   - Reference encoders (siprec_l16_to_{ulaw,alaw}_ref): the
 *     canonical branch-based G.711 quantisers. They are the source
 *     of truth and the equivalence oracle for the unit test.
 *   - Table encoders (siprec_l16_to_{ulaw,alaw}): O(1) branch-free
 *     lookups into 65536-entry tables built from the reference
 *     encoders at module load.
 *
 * Why tables: the media-bug callback (siprec_media.c) encodes every
 * sample of every 20 ms frame on the FreeSWITCH media thread. The
 * reference encoder's data-dependent normalisation loop mispredicts
 * on real audio — measured ~6.2 ns/sample vs ~0.9 ns/sample for the
 * table (~7×), shaving ~24% off the whole per-tick cost on an
 * i7-1280P. The win is per-stream CPU on the media threads, which
 * matters once the box runs thousands of concurrent recordings.
 * Cost: 128 KB of shared static tables, built once.
 *
 * This translation unit is FreeSWITCH-free (no <switch.h>) so the
 * table-vs-reference equivalence is a standalone unit test
 * (Makefile.test), mirroring siprec_sdp.c.
 */
#ifndef SIPREC_G711_H
#define SIPREC_G711_H

#include <stdint.h>

/* Reference encoders — canonical G.711. Exposed so the unit test
 * can use them as the oracle the tables must match. */
uint8_t siprec_l16_to_ulaw_ref(int16_t pcm);
uint8_t siprec_l16_to_alaw_ref(int16_t pcm);

/* Build the lookup tables from the reference encoders. Idempotent
 * (guarded), so calling it more than once is cheap. MUST be called
 * before the first table lookup — wired into mod_siprec_load. */
void siprec_g711_init(void);

/* Tables are defined in siprec_g711.c and filled by
 * siprec_g711_init(). Indexed by the raw 16-bit sample pattern. */
extern uint8_t siprec_g711_ulaw_tab[65536];
extern uint8_t siprec_g711_alaw_tab[65536];

/* Hot-path encoders: a single indexed load, no branches. Inlined
 * at the call site in the media-bug callback. Before init() runs
 * the tables are all-zero; the load hook guarantees init precedes
 * any recording. */
static inline uint8_t siprec_l16_to_ulaw(int16_t pcm)
{
    return siprec_g711_ulaw_tab[(uint16_t)pcm];
}

static inline uint8_t siprec_l16_to_alaw(int16_t pcm)
{
    return siprec_g711_alaw_tab[(uint16_t)pcm];
}

#endif /* SIPREC_G711_H */
