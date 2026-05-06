/*
 * siprec_srtp.c — SRTP encryption wrapper.
 *
 * libsrtp2 does the heavy lifting (AES-CTR + HMAC-SHA1 +
 * SRTP-specific key derivation per RFC 3711 §4.3.1). We
 * provide a small task-scoped surface that hides the library's
 * srtp_policy_t / srtp_t types from the rest of the module.
 *
 * libsrtp2 is the same library FreeSWITCH itself links
 * against — the in-tree FS build at libs/srtp populates its
 * libs/srtp/include/ tree. The Makefile.am entry adds
 * `-lsrtp2` so the runtime library resolves to the system
 * libsrtp2-1 (or whatever FS configured).
 */
#include "siprec_srtp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <srtp2/srtp.h>

/* ──────────────────────────────────────────────────────────── *
 * Random key material                                         *
 * ──────────────────────────────────────────────────────────── */

int siprec_srtp_keymat_random(uint8_t *buf, size_t len)
{
    if (!buf || !len) return -1;

    /* /dev/urandom — kernel CSPRNG, never blocks once seeded.
     * For server-side SRTP (no realtime constraint at start
     * of session) this is the right primitive. RFC 4086 §6.2
     * endorses /dev/urandom for cryptographic key material. */
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return -1;
        }
        if (n == 0) {
            /* /dev/urandom shouldn't EOF, but treat it as a
             * hard failure rather than spinning forever. */
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);
    return 0;
}

/* ──────────────────────────────────────────────────────────── *
 * base64                                                      *
 * Self-contained encoder so we don't pull in libcrypto purely *
 * for one EVP_EncodeBlock call. Standard alphabet, no line    *
 * wrap, padding optional (we don't pad — the 30-byte suite    *
 * encodes to 40 bytes with no '=').                           *
 * ──────────────────────────────────────────────────────────── */

static const char b64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int siprec_srtp_keymat_to_b64(
    const uint8_t *keymat, size_t keymat_len,
    char *out_b64, size_t out_b64_len)
{
    if (!keymat || !out_b64) return -1;

    /* Required output: ceil(len/3)*4 chars + 1 NUL.
     * For len=30: 40 + 1 = 41 bytes. */
    size_t need = ((keymat_len + 2) / 3) * 4 + 1;
    if (out_b64_len < need) return -1;

    size_t i = 0, j = 0;
    while (i + 3 <= keymat_len) {
        uint32_t triple =
            ((uint32_t)keymat[i]   << 16) |
            ((uint32_t)keymat[i+1] <<  8) |
             (uint32_t)keymat[i+2];
        out_b64[j++] = b64_alphabet[(triple >> 18) & 0x3F];
        out_b64[j++] = b64_alphabet[(triple >> 12) & 0x3F];
        out_b64[j++] = b64_alphabet[(triple >>  6) & 0x3F];
        out_b64[j++] = b64_alphabet[ triple        & 0x3F];
        i += 3;
    }

    /* Trailing 1- or 2-byte tail. RFC 4648 padding with '='. */
    size_t rem = keymat_len - i;
    if (rem == 1) {
        uint32_t triple = (uint32_t)keymat[i] << 16;
        out_b64[j++] = b64_alphabet[(triple >> 18) & 0x3F];
        out_b64[j++] = b64_alphabet[(triple >> 12) & 0x3F];
        out_b64[j++] = '=';
        out_b64[j++] = '=';
    } else if (rem == 2) {
        uint32_t triple =
            ((uint32_t)keymat[i]   << 16) |
            ((uint32_t)keymat[i+1] <<  8);
        out_b64[j++] = b64_alphabet[(triple >> 18) & 0x3F];
        out_b64[j++] = b64_alphabet[(triple >> 12) & 0x3F];
        out_b64[j++] = b64_alphabet[(triple >>  6) & 0x3F];
        out_b64[j++] = '=';
    }
    out_b64[j] = '\0';
    return 0;
}

/* ──────────────────────────────────────────────────────────── *
 * libsrtp2 wrapper                                             *
 * ──────────────────────────────────────────────────────────── */

struct siprec_srtp_session {
    srtp_t              srtp;
    siprec_srtp_suite_t suite;
};

static int srtp_initialised = 0;

int siprec_srtp_init(void)
{
    if (srtp_initialised) return 0;
    if (srtp_init() != srtp_err_status_ok) {
        return -1;
    }
    srtp_initialised = 1;
    return 0;
}

void siprec_srtp_shutdown(void)
{
    if (!srtp_initialised) return;
    srtp_shutdown();
    srtp_initialised = 0;
}

siprec_srtp_session_t *siprec_srtp_session_create(
    siprec_srtp_suite_t suite,
    uint32_t ssrc,
    const uint8_t *keymat, size_t keymat_len)
{
    if (!srtp_initialised || !keymat) return NULL;
    if (suite != SIPREC_SRTP_AES_CM_128_HMAC_SHA1_80) return NULL;
    if (keymat_len != SIPREC_SRTP_AES128_KEY_SALT_LEN) return NULL;

    siprec_srtp_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->suite = suite;

    /* Configure libsrtp policy. v1 fixes:
     *   ssrc_type      = ssrc_specific (one stream per session)
     *   ssrc_value     = caller-provided
     *   key            = caller-provided 30-byte master key+salt
     *   rtp.cipher_type    = SRTP_AES_ICM_128
     *   rtp.cipher_key_len = 30
     *   rtp.auth_type      = SRTP_HMAC_SHA1
     *   rtp.auth_key_len   = 20
     *   rtp.auth_tag_len   = 10  (SHA1_80 = 10-byte tag)
     *   rtp.sec_serv       = sec_serv_conf_and_auth
     */
    srtp_policy_t policy;
    memset(&policy, 0, sizeof(policy));

    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&policy.rtcp);

    policy.ssrc.type    = ssrc_specific;
    policy.ssrc.value   = ssrc;
    policy.key          = (uint8_t *)keymat; /* libsrtp copies */
    policy.window_size  = 128;
    policy.allow_repeat_tx = 0;
    policy.next         = NULL;

    if (srtp_create(&s->srtp, &policy) != srtp_err_status_ok) {
        free(s);
        return NULL;
    }
    return s;
}

int siprec_srtp_protect(
    siprec_srtp_session_t *sess,
    uint8_t *pkt, size_t buf_cap, size_t *len_io)
{
    if (!sess || !pkt || !len_io) return -1;

    /* libsrtp expects the buffer to have SRTP_MAX_TRAILER_LEN
     * bytes of headroom past the current packet length so
     * srtp_protect can append the auth tag in place. The
     * caller (siprec_media) sizes its buffer to RTP_HEADER_LEN
     * + payload + 16 bytes — enough for the SHA1_80 10-byte
     * tag with margin. We re-check here as a guard.
     */
    int  len = (int)*len_io;
    if (buf_cap < (size_t)len + SRTP_MAX_TRAILER_LEN) return -1;

    if (srtp_protect(sess->srtp, pkt, &len) != srtp_err_status_ok) {
        return -1;
    }
    *len_io = (size_t)len;
    return 0;
}

void siprec_srtp_session_destroy(siprec_srtp_session_t *sess)
{
    if (!sess) return;
    if (sess->srtp) srtp_dealloc(sess->srtp);
    free(sess);
}
