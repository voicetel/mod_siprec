/*
 * siprec_srtp.h — SRTP encryption for the recording RTP fork.
 *
 * Wraps libsrtp2 (the same library FreeSWITCH itself uses) so
 * mod_siprec can encrypt outbound RTP packets per RFC 3711 +
 * negotiate the keys via SDP a=crypto (RFC 4568).
 *
 * Crypto suite supported in v1:
 *   AES_CM_128_HMAC_SHA1_80
 *     - AES-128 in counter mode for the payload
 *     - HMAC-SHA1 80-bit (10-byte) auth tag
 *     - 16-byte master key + 14-byte master salt = 30 bytes
 *
 * AES_CM_128_HMAC_SHA1_32 (32-bit auth tag) and the 256-bit
 * variants are reserved for v2.
 */
#ifndef SIPREC_SRTP_H
#define SIPREC_SRTP_H

#include <stddef.h>
#include <stdint.h>

/* Crypto-suite enum kept narrow on purpose. The string form
 * lives in siprec_sdp_track_t.srtp_crypto_suite for SDP
 * emission; this enum is the runtime selector. */
typedef enum {
    SIPREC_SRTP_NONE = 0,
    SIPREC_SRTP_AES_CM_128_HMAC_SHA1_80,
} siprec_srtp_suite_t;

/* Total master-key+salt bytes for AES_CM_128_HMAC_SHA1_80. */
#define SIPREC_SRTP_AES128_KEY_SALT_LEN 30

/* Worst-case trailer libsrtp may append to a packet during
 * srtp_protect (auth tag + MKI etc). libsrtp2's own
 * SRTP_MAX_TRAILER_LEN is 144 across the suites it supports;
 * we mirror it here so siprec_media's packet buffer can size
 * itself without pulling srtp2/srtp.h into every includer.
 * The compile-time check inside siprec_srtp.c keeps the two
 * definitions in lockstep. */
#define SIPREC_SRTP_MAX_TRAILER_LEN 144

/* siprec_srtp_keymat_random: fill buf with cryptographically-
 * strong random key material for a fresh SRTP context. Returns
 * 0 on success, non-zero on failure (no /dev/urandom etc.).
 *
 * For AES_CM_128_HMAC_SHA1_80 the buffer MUST be exactly 30
 * bytes (16 master key + 14 master salt).
 */
int siprec_srtp_keymat_random(uint8_t *buf, size_t len);

/* siprec_srtp_keymat_to_b64: base64-encode keymat for the
 * SDP a=crypto inline= parameter. Output buffer should be at
 * least ((len * 4 / 3) + 4) bytes; for the 30-byte suite the
 * encoded length is 40 (no '=' padding when len % 3 == 0). */
int siprec_srtp_keymat_to_b64(
    const uint8_t *keymat, size_t keymat_len,
    char *out_b64, size_t out_b64_len);

/* Opaque SRTP session state. One per outbound stream — keys
 * MUST NOT be reused across streams (RFC 3711 §9.1). */
typedef struct siprec_srtp_session siprec_srtp_session_t;

/* siprec_srtp_session_create: open a fresh SRTP context for an
 * outbound (sendonly) stream. The keymat is consumed (copied
 * into libsrtp's internal state); the caller may free their
 * copy after this returns.
 *
 *   suite     — currently only AES_CM_128_HMAC_SHA1_80.
 *   ssrc      — RTP SSRC for this stream (host byte order).
 *   keymat    — master key + salt; length must match suite.
 *
 * Returns NULL on failure (suite unknown, libsrtp init error).
 */
siprec_srtp_session_t *siprec_srtp_session_create(
    siprec_srtp_suite_t suite,
    uint32_t ssrc,
    const uint8_t *keymat, size_t keymat_len);

/* siprec_srtp_protect: encrypt + auth a plain-RTP packet in
 * place. The buffer MUST be sized to hold the original RTP
 * header + payload PLUS room for the auth tag (10 bytes for
 * SHA1_80). On success *len_io is updated to the post-protect
 * length; on failure the buffer contents are undefined and
 * the caller MUST drop the packet.
 *
 * Returns 0 on success, non-zero on libsrtp error.
 */
int siprec_srtp_protect(
    siprec_srtp_session_t *sess,
    uint8_t *pkt, size_t buf_cap, size_t *len_io);

/* siprec_srtp_session_destroy: free the SRTP context. NULL-safe. */
void siprec_srtp_session_destroy(siprec_srtp_session_t *sess);

/* siprec_srtp_init: one-shot module-load init for libsrtp.
 * Idempotent — calling twice is safe. Returns 0 on success. */
int siprec_srtp_init(void);

/* siprec_srtp_shutdown: paired with siprec_srtp_init. */
void siprec_srtp_shutdown(void);

#endif /* SIPREC_SRTP_H */
