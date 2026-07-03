/*
 * siprec_test.c — unit tests for the SDP and metadata builders.
 *
 * Compile + run standalone (no FreeSWITCH needed):
 *
 *   gcc -Wall -Wextra -O2 \
 *       siprec_test.c siprec_sdp.c siprec_metadata.c \
 *       -o siprec_test && ./siprec_test
 *
 * Asserts the output matches RFC 7866 §7 / RFC 7865 §5 shape.
 * Failures exit non-zero with a diagnostic pointing at the
 * offending substring.
 */
#include "siprec_sdp.h"
#include "siprec_metadata.h"
#include "siprec_g711.h"
#include "siprec_sb.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

/* All fprintf(stderr, ...) calls below this point are test
 * diagnostics with literal format strings. They are not a
 * buffer-bound surface — fprintf to a FILE* writes to the
 * stream, not into a caller-supplied buffer. Annex K's
 * fprintf_s would satisfy clang-analyzer's check but glibc
 * doesn't ship it, and replacing fprintf with a vsnprintf_s
 * + fputs helper trades one analyzer complaint for another.
 * The Annex K checker is disabled project-wide in .clang-tidy;
 * this comment records why these calls are bounds-safe. */

static void check_contains(const char *got, const char *want, const char *what) {
    test_count++;
    if (!got) {
        fprintf(stderr, "FAIL %s: builder returned NULL\n", what);
        fail_count++;
        return;
    }
    if (strstr(got, want)) {
        printf("PASS %s\n", what);
    } else {
        fprintf(stderr, "FAIL %s: missing substring '%s'\n  got:\n%s\n",
            what, want, got);
        fail_count++;
    }
}

static void check_not_contains(const char *got, const char *want, const char *what) {
    test_count++;
    if (got && !strstr(got, want)) {
        printf("PASS %s\n", what);
    } else {
        fprintf(stderr, "FAIL %s: unexpected substring '%s'\n", what, want);
        fail_count++;
    }
}

static void check_int(long got, long want, const char *what) {
    test_count++;
    if (got == want) {
        printf("PASS %s\n", what);
    } else {
        fprintf(stderr, "FAIL %s: got %ld want %ld\n", what, got, want);
        fail_count++;
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    test_count++;
    if (got && strcmp(got, want) == 0) {
        printf("PASS %s\n", what);
    } else {
        fprintf(stderr, "FAIL %s: got '%s' want '%s'\n",
            what, got ? got : "(null)", want);
        fail_count++;
    }
}

/* ──────────────────────────────────────────────────────────── *
 * SDP tests                                                   *
 * ──────────────────────────────────────────────────────────── */

static void test_sdp_flip_direction(void) {
    /* RFC 7866 §6.4: pause/resume re-INVITE flips direction
     * while preserving the negotiated session — same ports,
     * same codec, same session-id; only o=session-version
     * is bumped per RFC 4566 §5.2. */
    const char *src =
        "v=0\r\n"
        "o=- 12345 7 IN IP4 192.0.2.10\r\n"
        "s=-\r\n"
        "c=IN IP4 192.0.2.10\r\n"
        "t=0 0\r\n"
        "m=audio 30000 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=ptime:20\r\n"
        "a=label:1\r\n"
        "a=sendonly\r\n";

    char *paused = siprec_sdp_flip_direction(src, 1);
    check_contains(paused, "o=- 12345 8 IN IP4 192.0.2.10\r\n",
        "flip:o= version bumped on pause");
    check_contains(paused, "a=inactive\r\n",   "flip:a=inactive on pause");
    check_not_contains(paused, "a=sendonly",   "flip:no a=sendonly on pause");
    check_contains(paused, "m=audio 30000 RTP/AVP 0\r\n",
        "flip:m= preserved on pause");
    check_contains(paused, "a=label:1\r\n",    "flip:a=label preserved");
    siprec_sdp_free(paused);

    char *resumed = siprec_sdp_flip_direction(src, 0);
    check_contains(resumed, "o=- 12345 8 IN IP4 192.0.2.10\r\n",
        "flip:o= version bumped on resume");
    check_contains(resumed, "a=sendonly\r\n", "flip:a=sendonly on resume");
    check_not_contains(resumed, "a=inactive", "flip:no a=inactive on resume");
    siprec_sdp_free(resumed);

    /* Round-trip a SDP that already has a=inactive; resume
     * should rewrite it back to a=sendonly. */
    const char *paused_src =
        "v=0\r\n"
        "o=- 99 1 IN IP4 1.2.3.4\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=inactive\r\n";
    char *back = siprec_sdp_flip_direction(paused_src, 0);
    check_contains(back, "a=sendonly\r\n",   "flip:inactive→sendonly");
    check_contains(back, "o=- 99 2 IN IP4 1.2.3.4\r\n",
        "flip:inactive→sendonly bumps version");
    siprec_sdp_free(back);

    /* Malformed o= line (sscanf < 6 fields): flip passes it
     * through verbatim (no version bump) while still flipping the
     * direction attribute. */
    const char *bad_o =
        "v=0\r\n"
        "o=malformed\r\n"
        "s=-\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=sendonly\r\n";
    char *bad_o_out = siprec_sdp_flip_direction(bad_o, 1);
    check_contains(bad_o_out, "o=malformed\r\n",
        "flip:malformed o= passed through verbatim");
    check_contains(bad_o_out, "a=inactive\r\n",
        "flip:direction still flipped with malformed o=");
    siprec_sdp_free(bad_o_out);

    /* LF-only line endings (bare \n, no CR): flip handles a \n
     * terminator — o= still bumps, direction still flips. */
    const char *lf_only =
        "v=0\n"
        "o=- 5 1 IN IP4 1.2.3.4\n"
        "s=-\n"
        "m=audio 9 RTP/AVP 0\n"
        "a=sendonly\n";
    char *lf_out = siprec_sdp_flip_direction(lf_only, 1);
    check_contains(lf_out, "o=- 5 2 IN IP4 1.2.3.4",
        "flip:LF-only o= version bumped");
    check_contains(lf_out, "a=inactive",
        "flip:LF-only direction flipped");
    siprec_sdp_free(lf_out);

    /* Empty / NULL input must reject — no point producing an
     * empty body for a re-INVITE that would be malformed. */
    test_count++;
    if (siprec_sdp_flip_direction(NULL, 0) == NULL) printf("PASS flip:reject NULL\n");
    else { fprintf(stderr, "FAIL flip:NULL should reject\n"); fail_count++; }
    test_count++;
    if (siprec_sdp_flip_direction("", 0) == NULL) printf("PASS flip:reject empty\n");
    else { fprintf(stderr, "FAIL flip:empty should reject\n"); fail_count++; }
}

static void test_sdp_inject_labels(void) {
    /* RFC 7866 §8.5: every SRC stream must carry a=label:N.
     * mod_sofia auto-gens an SDP without labels; we inject
     * via post-originate re-INVITE. The injector auto-numbers
     * per m= block (1st → label:1, 2nd → label:2, …). */

    /* Single m= block: label:1 emitted before a=sendonly. */
    const char *unlabelled =
        "v=0\r\n"
        "o=- 555 1 IN IP4 192.0.2.10\r\n"
        "s=-\r\n"
        "c=IN IP4 192.0.2.10\r\n"
        "t=0 0\r\n"
        "m=audio 30000 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=ptime:20\r\n"
        "a=sendonly\r\n";

    char *labelled = siprec_sdp_inject_labels(unlabelled);
    check_contains(labelled, "a=label:1\r\n",
        "inject:single-block label:1 appears");
    check_contains(labelled, "o=- 555 2 IN IP4 192.0.2.10\r\n",
        "inject:o= version bumped");
    /* Label should appear BEFORE a=sendonly (the conventional
     * direction-attribute position, RFC 7866 §8.5). */
    test_count++;
    {
        const char *lpos = labelled ? strstr(labelled, "a=label:1") : NULL;
        const char *spos = labelled ? strstr(labelled, "a=sendonly") : NULL;
        if (lpos && spos && lpos < spos) {
            printf("PASS inject:label precedes a=sendonly\n");
        } else {
            fprintf(stderr, "FAIL inject:label should appear before a=sendonly\n");
            fail_count++;
        }
    }
    check_contains(labelled, "m=audio 30000 RTP/AVP 0\r\n",
        "inject:m= preserved");
    check_contains(labelled, "c=IN IP4 192.0.2.10\r\n",
        "inject:c= preserved");
    siprec_sdp_free(labelled);

    /* Two m= blocks: 1st gets label:1, 2nd gets label:2.
     * RFC 7866 §7 / §8.5 multi-track signature — what the
     * call site emits the day mod_sofia produces multi-track
     * offers (or the SDP-override hook lands). */
    const char *two_blocks =
        "v=0\r\n"
        "o=- 100 1 IN IP4 192.0.2.10\r\n"
        "s=-\r\n"
        "c=IN IP4 192.0.2.10\r\n"
        "t=0 0\r\n"
        "m=audio 30000 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=sendonly\r\n"
        "m=audio 30002 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=sendonly\r\n";
    char *two = siprec_sdp_inject_labels(two_blocks);
    check_contains(two, "a=label:1\r\n",
        "inject:two-block label:1 on first stream");
    check_contains(two, "a=label:2\r\n",
        "inject:two-block label:2 on second stream");
    /* Ordering check: label:1 must precede label:2 in the
     * output, AND each label must precede the corresponding
     * a=sendonly within its block. Two-pass scan from the top. */
    test_count++;
    {
        const char *l1 = two ? strstr(two, "a=label:1") : NULL;
        const char *l2 = two ? strstr(two, "a=label:2") : NULL;
        const char *m2 = two ? strstr(two, "m=audio 30002") : NULL;
        if (l1 && l2 && m2 && l1 < m2 && m2 < l2) {
            printf("PASS inject:two-block label ordering (label:1 in 1st block, label:2 in 2nd)\n");
        } else {
            fprintf(stderr, "FAIL inject:two-block label ordering wrong\n");
            fail_count++;
        }
    }
    siprec_sdp_free(two);

    /* Idempotent: SDP that already carries a=label:1 must not
     * be double-labelled. Version still bumps because that's
     * what re-INVITE semantics demand. */
    const char *already_labelled =
        "v=0\r\n"
        "o=- 555 1 IN IP4 1.2.3.4\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 30000 RTP/AVP 0\r\n"
        "a=label:1\r\n"
        "a=sendonly\r\n";
    char *re_inject = siprec_sdp_inject_labels(already_labelled);
    test_count++;
    {
        const char *p = re_inject;
        int count = 0;
        while (p && (p = strstr(p, "a=label:1")) != NULL) { count++; p++; }
        if (count == 1) {
            printf("PASS inject:idempotent (single a=label:1)\n");
        } else {
            fprintf(stderr, "FAIL inject:idempotent — extra a=label:1 was emitted\n");
            fail_count++;
        }
    }
    check_contains(re_inject, "o=- 555 2 IN IP4 1.2.3.4\r\n",
        "inject:idempotent still bumps version");
    siprec_sdp_free(re_inject);

    /* Mixed: 1st block already labelled (label:1), 2nd block
     * unlabelled. Counter advances on the labelled block, so
     * the 2nd block correctly gets label:2 — not label:1. */
    const char *mixed =
        "v=0\r\n"
        "o=- 200 1 IN IP4 1.2.3.4\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 30000 RTP/AVP 0\r\n"
        "a=label:1\r\n"
        "a=sendonly\r\n"
        "m=audio 30002 RTP/AVP 0\r\n"
        "a=sendonly\r\n";
    char *mixed_out = siprec_sdp_inject_labels(mixed);
    test_count++;
    {
        const char *p = mixed_out;
        int l1 = 0, l2 = 0;
        while (p && (p = strstr(p, "a=label:1")) != NULL) { l1++; p++; }
        p = mixed_out;
        while (p && (p = strstr(p, "a=label:2")) != NULL) { l2++; p++; }
        if (l1 == 1 && l2 == 1) {
            printf("PASS inject:mixed (existing label:1 kept, new label:2 added)\n");
        } else {
            fprintf(stderr, "FAIL inject:mixed — l1=%d l2=%d, want 1/1\n", l1, l2);
            fail_count++;
        }
    }
    siprec_sdp_free(mixed_out);

    /* m= block with no direction attribute — label appended
     * at end of block / SDP. */
    const char *no_direction =
        "v=0\r\n"
        "o=- 1 1 IN IP4 1.2.3.4\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 RTP/AVP 0\r\n";
    char *trail = siprec_sdp_inject_labels(no_direction);
    check_contains(trail, "a=label:1\r\n", "inject:trailing-block label");
    siprec_sdp_free(trail);

    /* Malformed o= line (sscanf < 6 fields): passed through
     * verbatim — the injector doesn't guess at a fix — and the
     * label is still injected. */
    const char *bad_o =
        "v=0\r\n"
        "o=garbage\r\n"
        "s=-\r\n"
        "m=audio 9 RTP/AVP 0\r\n"
        "a=sendonly\r\n";
    char *bad_o_out = siprec_sdp_inject_labels(bad_o);
    check_contains(bad_o_out, "o=garbage\r\n",
        "inject:malformed o= passed through verbatim");
    check_contains(bad_o_out, "a=label:1\r\n",
        "inject:label still injected with malformed o=");
    siprec_sdp_free(bad_o_out);

    /* First m= block unlabelled AND with no direction attribute,
     * followed by a second m= block: the first block's label is
     * emitted at the m= boundary (not deferred to end-of-SDP). */
    const char *first_unlabelled =
        "v=0\r\n"
        "o=- 1 1 IN IP4 1.2.3.4\r\n"
        "s=-\r\n"
        "m=audio 30000 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "m=audio 30002 RTP/AVP 0\r\n"
        "a=sendonly\r\n";
    char *fu = siprec_sdp_inject_labels(first_unlabelled);
    check_contains(fu, "a=label:1\r\n",
        "inject:first-block label emitted at m= boundary");
    check_contains(fu, "a=label:2\r\n",
        "inject:second-block label emitted");
    test_count++;
    {
        const char *l1 = fu ? strstr(fu, "a=label:1") : NULL;
        const char *m2 = fu ? strstr(fu, "m=audio 30002") : NULL;
        if (l1 && m2 && l1 < m2) {
            printf("PASS inject:first-block label precedes second m=\n");
        } else {
            fprintf(stderr, "FAIL inject:first-block label should precede second m=\n");
            fail_count++;
        }
    }
    siprec_sdp_free(fu);

    /* LF-only line endings (bare \n, no CR): the injector handles
     * a \n terminator, not just \r\n. */
    const char *lf_only =
        "v=0\n"
        "o=- 1 1 IN IP4 1.2.3.4\n"
        "s=-\n"
        "m=audio 9 RTP/AVP 0\n"
        "a=sendonly\n";
    char *lf_out = siprec_sdp_inject_labels(lf_only);
    check_contains(lf_out, "a=label:1", "inject:LF-only line endings handled");
    siprec_sdp_free(lf_out);

    /* Reject NULL / empty inputs. */
    test_count++;
    if (siprec_sdp_inject_labels(NULL) == NULL) printf("PASS inject:reject NULL sdp\n");
    else { fprintf(stderr, "FAIL inject:NULL sdp should reject\n"); fail_count++; }
    test_count++;
    if (siprec_sdp_inject_labels("") == NULL) printf("PASS inject:reject empty sdp\n");
    else { fprintf(stderr, "FAIL inject:empty sdp should reject\n"); fail_count++; }
}

/* ──────────────────────────────────────────────────────────── *
 * SDP answer parser tests (siprec_sdp_parse_remote_streams)   *
 * ──────────────────────────────────────────────────────────── */

static void test_parse_remote_streams(void) {
    siprec_negotiated_t out[SIPREC_MAX_STREAMS];

    /* The regression case: a single-stream PCMA (PT 8) answer.
     * The parser MUST surface pt=8 so the media fork encodes
     * a-law instead of guessing from the original call leg. */
    {
        const char *sdp =
            "v=0\r\n"
            "o=- 1 1 IN IP4 203.0.113.5\r\n"
            "s=-\r\n"
            "c=IN IP4 203.0.113.5\r\n"
            "t=0 0\r\n"
            "m=audio 5004 RTP/AVP 8\r\n"
            "a=rtpmap:8 PCMA/8000\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:pcma single-stream count");
        check_str(out[0].remote_ip, "203.0.113.5", "parse:pcma ip");
        check_int(out[0].remote_port, 5004, "parse:pcma port");
        check_int(out[0].pt, 8, "parse:pcma pt=8");
    }

    /* Single-stream PCMU (PT 0). */
    {
        const char *sdp =
            "c=IN IP4 198.51.100.7\r\n"
            "m=audio 6000 RTP/AVP 0\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:pcmu count");
        check_int(out[0].pt, 0, "parse:pcmu pt=0");
    }

    /* Two streams, session-level c= applies to both; each m=
     * line carries a different codec. */
    {
        const char *sdp =
            "v=0\r\n"
            "c=IN IP4 192.0.2.10\r\n"
            "m=audio 40000 RTP/AVP 0\r\n"
            "a=rtpmap:0 PCMU/8000\r\n"
            "m=audio 40002 RTP/AVP 8\r\n"
            "a=rtpmap:8 PCMA/8000\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 2, "parse:two-stream count");
        check_str(out[0].remote_ip, "192.0.2.10", "parse:two-stream ip0");
        check_int(out[0].remote_port, 40000, "parse:two-stream port0");
        check_int(out[0].pt, 0, "parse:two-stream pt0");
        check_str(out[1].remote_ip, "192.0.2.10", "parse:two-stream ip1");
        check_int(out[1].remote_port, 40002, "parse:two-stream port1");
        check_int(out[1].pt, 8, "parse:two-stream pt1");
    }

    /* Multiple PTs on the m= line: the FIRST is the answerer's
     * selected codec (RFC 3264 §6). */
    {
        const char *sdp =
            "c=IN IP4 198.51.100.7\r\n"
            "m=audio 6000 RTP/AVP 8 0 101\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:multi-pt count");
        check_int(out[0].pt, 8, "parse:multi-pt first wins");
    }

    /* Secured transport token (RTP/SAVP) must not block PT
     * parsing — the %*s skips whatever the transport spells. */
    {
        const char *sdp =
            "c=IN IP4 198.51.100.7\r\n"
            "m=audio 6000 RTP/SAVP 0\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:savp count");
        check_int(out[0].pt, 0, "parse:savp pt parsed");
    }

    /* m= line with no payload type → pt = SIPREC_PT_UNSET (the
     * media fork then falls back to the read-codec default). */
    {
        const char *sdp =
            "c=IN IP4 198.51.100.7\r\n"
            "m=audio 6000 RTP/AVP\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:no-pt count");
        check_int(out[0].pt, SIPREC_PT_UNSET, "parse:no-pt is UNSET");
    }

    /* port=0 stream is rejected (RFC 3264 §5.1) and consumes no
     * output slot; the following valid stream still lands at
     * index 0. */
    {
        const char *sdp =
            "c=IN IP4 198.51.100.7\r\n"
            "m=audio 0 RTP/AVP 0\r\n"
            "m=audio 7000 RTP/AVP 8\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:port0 skipped count");
        check_int(out[0].remote_port, 7000, "parse:port0 skipped survivor");
        check_int(out[0].pt, 8, "parse:port0 skipped survivor pt");
    }

    /* Per-media c= after an m= line overrides the session c= for
     * that stream only; the next stream falls back to session c=. */
    {
        const char *sdp =
            "v=0\r\n"
            "c=IN IP4 10.0.0.1\r\n"
            "m=audio 8000 RTP/AVP 0\r\n"
            "c=IN IP4 10.0.0.9\r\n"
            "m=audio 8002 RTP/AVP 8\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 2, "parse:per-media-c count");
        check_str(out[0].remote_ip, "10.0.0.9", "parse:per-media-c override");
        check_str(out[1].remote_ip, "10.0.0.1", "parse:per-media-c session fallback");
    }

    /* Regression: a per-media c= belonging to a DECLINED (port-0)
     * audio block must NOT clobber the previously committed
     * stream's remote IP. The old code attributed any media c= to
     * out[n-1] and silently redirected out[0]'s RTP. */
    {
        const char *sdp =
            "v=0\r\n"
            "c=IN IP4 10.0.0.1\r\n"
            "m=audio 40000 RTP/AVP 0\r\n"
            "m=audio 0 RTP/AVP 0\r\n"
            "c=IN IP4 203.0.113.9\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:declined-block c= count");
        check_str(out[0].remote_ip, "10.0.0.1",
            "parse:declined-block c= does not clobber prior stream");
    }

    /* Regression: a per-media c= belonging to a NON-AUDIO block
     * (m=video / m=image) must likewise not touch a prior audio
     * stream. The old parser didn't recognise non-audio m= lines
     * at all, so their c= fell onto out[n-1]. */
    {
        const char *sdp =
            "v=0\r\n"
            "c=IN IP4 10.0.0.1\r\n"
            "m=audio 40000 RTP/AVP 0\r\n"
            "m=video 50000 RTP/AVP 96\r\n"
            "c=IN IP4 203.0.113.9\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:non-audio-block c= count");
        check_str(out[0].remote_ip, "10.0.0.1",
            "parse:non-audio-block c= does not clobber audio stream");
    }

    /* A non-audio m= block appearing BEFORE the audio block must
     * not consume the session c=; the audio stream still inherits
     * the session-level address. */
    {
        const char *sdp =
            "v=0\r\n"
            "c=IN IP4 10.0.0.1\r\n"
            "m=video 50000 RTP/AVP 96\r\n"
            "m=audio 40000 RTP/AVP 0\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:leading-video count");
        check_str(out[0].remote_ip, "10.0.0.1",
            "parse:audio after video inherits session c=");
    }

    /* out_max caps the number of streams written — a third m=
     * block must NOT overflow the 2-slot array (ASan build
     * exercises the bound). */
    {
        const char *sdp =
            "c=IN IP4 1.1.1.1\r\n"
            "m=audio 1000 RTP/AVP 0\r\n"
            "m=audio 1002 RTP/AVP 0\r\n"
            "m=audio 1004 RTP/AVP 0\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, SIPREC_MAX_STREAMS, "parse:out_max cap");
    }

    /* IPv6 c= is ignored (v1 fork is IPv4-only): the stream is
     * still committed but its remote_ip stays empty so the
     * downstream inet_pton fails loudly rather than silently. */
    {
        const char *sdp =
            "c=IN IP6 2001:db8::1\r\n"
            "m=audio 9000 RTP/AVP 0\r\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:ipv6 count");
        check_int(out[0].remote_ip[0], '\0', "parse:ipv6 ip left empty");
    }

    /* Bare LF line endings (no CR) parse identically to CRLF. */
    {
        const char *sdp =
            "c=IN IP4 5.5.5.5\n"
            "m=audio 1234 RTP/AVP 8\n";
        memset(out, 0, sizeof(out));
        int n = siprec_sdp_parse_remote_streams(sdp, out, SIPREC_MAX_STREAMS);
        check_int(n, 1, "parse:lf-only count");
        check_str(out[0].remote_ip, "5.5.5.5", "parse:lf-only ip");
        check_int(out[0].pt, 8, "parse:lf-only pt");
    }

    /* Defensive: NULL sdp, NULL out, and out_max==0 all return 0
     * without dereferencing. */
    {
        check_int(siprec_sdp_parse_remote_streams(NULL, out, SIPREC_MAX_STREAMS),
            0, "parse:null sdp");
        check_int(siprec_sdp_parse_remote_streams("m=audio 1 RTP/AVP 0\r\n", NULL, 2),
            0, "parse:null out");
        check_int(siprec_sdp_parse_remote_streams("m=audio 1 RTP/AVP 0\r\n", out, 0),
            0, "parse:zero out_max");
        check_int(siprec_sdp_parse_remote_streams("", out, SIPREC_MAX_STREAMS),
            0, "parse:empty sdp");
    }
}

/* ──────────────────────────────────────────────────────────── *
 * Metadata tests                                              *
 * ──────────────────────────────────────────────────────────── */

static void test_metadata_two_participants(void) {
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "urn:uuid:p-caller", .aor = "sip:alice@example.com",
          .display_name = "Alice" },
        { .participant_id = "urn:uuid:p-callee", .aor = "sip:bob@example.com",
          .display_name = NULL },
    };
    const siprec_metadata_stream_t streams[] = {
        { .stream_id = "urn:uuid:s-1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0, .label = "1" },
        { .stream_id = "urn:uuid:s-2", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 1, .label = "2" },
    };

    siprec_metadata_options_t opts = {
        .session_id = "urn:uuid:sess-abc",
        .group_id   = "urn:uuid:grp-xyz",
        .associate_time_utc = "2026-05-06T03:00:00Z",
        .datamode   = SIPREC_DATAMODE_COMPLETE,
        .participants = parts, .participant_count = 2,
        .streams = streams, .stream_count = 2,
    };

    char *xml = siprec_metadata_build(&opts);

    check_contains(xml, "<?xml version=\"1.0\"",                    "meta:xml decl");
    check_contains(xml, "xmlns=\"urn:ietf:params:xml:ns:recording:1\"", "meta:xmlns");
    check_contains(xml, "<datamode>complete</datamode>",            "meta:datamode complete");
    /* group has body because associate_time_utc is set */
    check_contains(xml, "<group group_id=\"urn:uuid:grp-xyz\">",
                                                                     "meta:group with body");
    /* RFC 7865 Appendix A sessiontype: session_id is the only
     * attribute, group binding is via <group-ref> child, the
     * timestamp is <start-time>. The historic group_ref attr
     * and <associate-time> child were schema-non-conformant. */
    check_contains(xml, "<session session_id=\"urn:uuid:sess-abc\">",
                                                                     "meta:session open (session_id only)");
    check_not_contains(xml, "<session session_id=\"urn:uuid:sess-abc\" group_ref=",
                                                                     "meta:session no group_ref attr");
    check_contains(xml, "<group-ref>urn:uuid:grp-xyz</group-ref>",
                                                                     "meta:session has <group-ref> child");
    check_contains(xml, "<start-time>2026-05-06T03:00:00Z</start-time>",
                                                                     "meta:session has <start-time>");
    check_contains(xml, "</session>",                                "meta:session close");
    check_contains(xml, "<participant participant_id=\"urn:uuid:p-caller\">",
                                                                     "meta:participant alice");
    /* <participant> only carries participant_id per RFC 7865
     * Appendix A — session_id MUST NOT appear here. */
    check_not_contains(xml,
        "<participant participant_id=\"urn:uuid:p-caller\" session_id=",
                                                                     "meta:participant no session_id attr");
    check_contains(xml, "<nameID aor=\"sip:alice@example.com\">",   "meta:nameID alice");
    check_contains(xml, "<name>Alice</name>",                        "meta:display name");
    /* <send>/<recv> moved to <participantstreamassoc>; the
     * participant body must not carry them. */
    check_not_contains(xml, "<send>urn:uuid:s-1</send>\r\n  </participant>",
                                                                     "meta:participant has no send xref");
    check_contains(xml, "<participant participant_id=\"urn:uuid:p-callee\">",
                                                                     "meta:participant bob");
    /* Bob has no display name → self-closing nameID */
    check_contains(xml, "<nameID aor=\"sip:bob@example.com\"/>",     "meta:nameID self-close");

    /* RFC 7865 Appendix A streamtype: <label> is the only
     * typed in-namespace child. <media-type> is not in the
     * schema and was historically a non-conformant emission. */
    check_contains(xml, "<stream stream_id=\"urn:uuid:s-1\"",        "meta:stream 1");
    check_contains(xml, "<stream stream_id=\"urn:uuid:s-2\"",        "meta:stream 2");
    check_contains(xml, "<label>1</label>",                          "meta:stream label 1");
    check_contains(xml, "<label>2</label>",                          "meta:stream label 2");
    check_not_contains(xml, "<media-type>",                          "meta:no <media-type>");

    /* <group> with associate-time gets a body */
    check_contains(xml, "<group group_id=\"urn:uuid:grp-xyz\">",     "meta:group with body open");
    check_contains(xml, "</group>",                                   "meta:group close");
    /* <group> uses <associate-time>, distinct from <session>'s
     * <start-time>; both are present but in different parents. */
    check_contains(xml, "<associate-time>2026-05-06T03:00:00Z</associate-time>",
                                                                     "meta:group has <associate-time>");

    siprec_metadata_free(xml);
}

static void test_metadata_partial_datamode(void) {
    /* RFC 7865 §5.1: re-INVITE updates use datamode=partial. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x", .display_name = NULL },
    };
    siprec_metadata_options_t opts = {
        .session_id = "s",
        .datamode   = SIPREC_DATAMODE_PARTIAL,
        .participants = parts, .participant_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);
    check_contains(xml, "<datamode>partial</datamode>",     "meta:datamode partial");
    check_not_contains(xml, "<datamode>complete</datamode>", "meta:no complete when partial");
    siprec_metadata_free(xml);
}

static void test_metadata_group_self_close(void) {
    /* A <group> with a group_id but no associate-time has no
     * body, so it must emit a self-closing <group .../> rather
     * than an open/close pair. Covers the group_has_body == false
     * branch. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x", .display_name = NULL },
    };
    siprec_metadata_options_t opts = {
        .session_id   = "sess",
        .group_id     = "grp-nobody",
        /* associate_time_utc deliberately unset → no group body. */
        .datamode     = SIPREC_DATAMODE_COMPLETE,
        .participants = parts, .participant_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);
    check_contains(xml, "<group group_id=\"grp-nobody\"/>",
        "meta:group self-closes without associate-time");
    check_not_contains(xml, "</group>",
        "meta:no group close tag when self-closed");
    siprec_metadata_free(xml);
}

static void test_metadata_reason_elements(void) {
    /* RFC 7865 Appendix A: <reason> is a child of <session>
     * only. <participant> and <group> do not allow it in the
     * recording: namespace. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x", .display_name = NULL },
    };
    siprec_metadata_options_t opts = {
        .session_id    = "sess",
        .group_id      = "grp",
        .associate_time_utc = "2026-05-06T03:00:00Z",
        .session_reason = "paused",
        .participants  = parts, .participant_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);
    check_contains(xml, "<reason>paused</reason>",       "meta:session reason");
    /* <group> has no <reason> in the schema. */
    check_not_contains(xml, "<reason>merged</reason>",   "meta:no group reason");
    siprec_metadata_free(xml);
}

static void test_metadata_assoc_elements(void) {
    /* RFC 7865 §5 explicit associations. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p-alice", .aor = "sip:alice@x" },
        { .participant_id = "p-bob",   .aor = "sip:bob@x" },
    };
    const siprec_metadata_stream_t streams[] = {
        { .stream_id = "s1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0, .label = "1" },
        { .stream_id = "s2", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 1, .label = "2" },
    };
    siprec_metadata_options_t opts = {
        .session_id   = "sess",
        .associate_time_utc = "2026-05-06T03:00:00Z",
        .participants = parts, .participant_count = 2,
        .streams      = streams, .stream_count = 2,
    };
    char *xml = siprec_metadata_build(&opts);

    check_contains(xml,
        "<participantsessionassoc participant_id=\"p-alice\" session_id=\"sess\"",
        "meta:participantsessionassoc alice");
    check_contains(xml,
        "<participantsessionassoc participant_id=\"p-bob\" session_id=\"sess\"",
        "meta:participantsessionassoc bob");
    check_contains(xml,
        "<participantstreamassoc participant_id=\"p-alice\">",
        "meta:participantstreamassoc alice");
    check_contains(xml, "<send>s1</send>", "meta:stream-assoc send-1");
    check_contains(xml, "<send>s2</send>", "meta:stream-assoc send-2");

    siprec_metadata_free(xml);
}

static void test_metadata_element_ordering(void) {
    /* RFC 7865 Appendix A's <recording> sequence is:
     * datamode → group → session → participant → stream →
     * sessionrecordingassoc → participantsessionassoc →
     * participantstreamassoc.
     *
     * A strict XSD parser will reject an out-of-sequence
     * document. Verify <stream> appears BEFORE the assoc
     * elements. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x" },
    };
    const siprec_metadata_stream_t streams[] = {
        { .stream_id = "s1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0, .label = "1" },
    };
    siprec_metadata_options_t opts = {
        .session_id = "sess",
        .participants = parts, .participant_count = 1,
        .streams      = streams, .stream_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);

    test_count++;
    const char *stream_pos = strstr(xml, "<stream stream_id=");
    const char *psa_pos    = strstr(xml, "<participantsessionassoc");
    const char *psma_pos   = strstr(xml, "<participantstreamassoc");
    if (stream_pos && psa_pos && psma_pos
        && stream_pos < psa_pos && stream_pos < psma_pos) {
        printf("PASS meta:<stream> before assocs (schema order)\n");
    } else {
        fprintf(stderr,
            "FAIL meta:element order — <stream> must precede "
            "<participantsessionassoc>/<participantstreamassoc>\n");
        fail_count++;
    }

    siprec_metadata_free(xml);
}

static void test_metadata_stream_self_closes_when_unlabelled(void) {
    /* No label → <stream …/> self-closing per RFC 7865
     * Appendix A streamtype (label is the only typed
     * in-namespace child and it's optional). */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x", .display_name = NULL },
    };
    const siprec_metadata_stream_t streams[] = {
        { .stream_id = "s1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0, .label = NULL },
    };
    siprec_metadata_options_t opts = {
        .session_id = "sess",
        .participants = parts, .participant_count = 1,
        .streams = streams, .stream_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);
    check_contains(xml, "<stream stream_id=\"s1\" session_id=\"sess\"/>",
        "meta:unlabelled stream self-closes");
    siprec_metadata_free(xml);
}

static void test_metadata_xml_escaping(void) {
    /* Hostile-looking AOR with reserved XML chars must be
     * escaped, not passed through. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1",
          .aor = "sip:user&pass<\"'>x@example.com",
          .display_name = "<script>alert(1)</script>" },
    };
    siprec_metadata_options_t opts = {
        .session_id = "s1",
        .participants = parts, .participant_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);

    check_contains(xml, "&amp;", "meta:escape ampersand");
    check_contains(xml, "&lt;",  "meta:escape less-than");
    check_contains(xml, "&gt;",  "meta:escape greater-than");
    check_contains(xml, "&quot;", "meta:escape quote");
    check_contains(xml, "&apos;", "meta:escape apostrophe");
    /* The literal hostile string MUST NOT appear unescaped. */
    check_not_contains(xml, "<script>alert(1)</script>", "meta:no raw script tag");

    siprec_metadata_free(xml);
}

static void test_metadata_invalid_returns_null(void) {
    /* No session_id → NULL */
    {
        const siprec_metadata_participant_t p = { .participant_id = "p", .aor = "sip:x@y" };
        siprec_metadata_options_t opts = { .participants = &p, .participant_count = 1 };
        test_count++;
        if (siprec_metadata_build(&opts) == NULL) printf("PASS meta:reject no session_id\n");
        else { fprintf(stderr, "FAIL meta:no session_id should reject\n"); fail_count++; }
    }
    /* Stream pointing at out-of-range participant → NULL */
    {
        const siprec_metadata_participant_t p = { .participant_id = "p", .aor = "sip:x@y" };
        const siprec_metadata_stream_t s = { .stream_id = "s",
            .mode = SIPREC_STREAM_SEND, .participant_idx = 99 };
        siprec_metadata_options_t opts = { .session_id = "sess",
            .participants = &p, .participant_count = 1,
            .streams = &s, .stream_count = 1 };
        test_count++;
        if (siprec_metadata_build(&opts) == NULL) printf("PASS meta:reject stream OOB idx\n");
        else { fprintf(stderr, "FAIL meta:OOB stream idx should reject\n"); fail_count++; }
    }
}

/* ──────────────────────────────────────────────────────────── *
 * Main                                                        *
 * ──────────────────────────────────────────────────────────── */

/* The media-bug hot path encodes via the G.711 lookup tables
 * (siprec_l16_to_ulaw / _alaw). Those tables MUST be bit-identical
 * to the reference encoders for every one of the 65536 possible
 * int16 samples — a faster encoder that changes even one sample's
 * output corrupts the recording. This sweep is the equivalence gate
 * that lets the hot path trust the tables. */
static void test_g711_tables_match_reference(void) {
    int v;
    int ulaw_mismatch = 0;
    int alaw_mismatch = 0;

    siprec_g711_init();

    for (v = -32768; v <= 32767; v++) {
        int16_t s = (int16_t)v;
        if (siprec_l16_to_ulaw(s) != siprec_l16_to_ulaw_ref(s)) ulaw_mismatch++;
        if (siprec_l16_to_alaw(s) != siprec_l16_to_alaw_ref(s)) alaw_mismatch++;
    }
    check_int(ulaw_mismatch, 0, "g711 ulaw table == reference for all 65536 inputs");
    check_int(alaw_mismatch, 0, "g711 alaw table == reference for all 65536 inputs");

    /* init() is idempotent — a second call must not corrupt the
     * tables (guards against an accidental re-zero on reload). */
    siprec_g711_init();
    check_int(siprec_l16_to_ulaw(0), siprec_l16_to_ulaw_ref(0),
        "g711 init is idempotent (ulaw[0] stable after re-init)");
}

/* ──────────────────────────────────────────────────────────── *
 * String-builder (siprec_sb) defensive-path tests             *
 *                                                              *
 * The error-latching paths in siprec_sb are unreachable by any *
 * ordinary input: they fire only on allocation failure or at   *
 * the 64 MB growth ceiling. We exercise them directly — the    *
 * allocator failures via the siprec_sb_realloc test seam, the  *
 * ceiling via a body that crosses SB_MAX_CAP.                  *
 * ──────────────────────────────────────────────────────────── */

/* Failing allocator: pass the first `sb_realloc_ok` calls through to
 * real realloc, then return NULL for the rest. */
static int sb_realloc_ok = 0;
static void *failing_realloc(void *ptr, size_t size) {
    if (sb_realloc_ok > 0) {
        sb_realloc_ok--;
        return realloc(ptr, size);
    }
    return NULL;
}

static void expect_null(const void *p, const char *what) {
    test_count++;
    if (p == NULL) {
        printf("PASS %s\n", what);
    } else {
        fprintf(stderr, "FAIL %s: expected NULL\n", what);
        fail_count++;
    }
}

static void expect_true(int cond, const char *what) {
    test_count++;
    if (cond) {
        printf("PASS %s\n", what);
    } else {
        fprintf(stderr, "FAIL %s\n", what);
        fail_count++;
    }
}

static void test_sb_defensive_paths(void) {
    /* sb_take on a never-appended buffer: err clear but data NULL,
     * so it frees nothing and returns NULL. */
    {
        sb_t sb;
        sb_init(&sb);
        expect_null(sb_take(&sb), "sb:empty take returns NULL");
    }

    /* sb_append n == 0 is a safe no-op (adjacent line terminators). */
    {
        sb_t sb;
        sb_init(&sb);
        sb_append(&sb, "ignored", 0);
        sb_append(&sb, "hi", 2);
        char *r = sb_take(&sb);
        expect_true(r && strcmp(r, "hi") == 0, "sb:n==0 append is a no-op");
        free(r);
    }

    /* First allocation fails: sb_append's reserve fails and latches
     * err; a following sb_appendf sees err and no-ops; sb_take frees
     * the (NULL) buffer and returns NULL. */
    {
        sb_t sb;
        sb_init(&sb);
        siprec_sb_realloc = failing_realloc;
        sb_realloc_ok = 0;                 /* fail immediately */
        sb_append(&sb, "hello", 5);        /* reserve realloc -> NULL -> err */
        sb_appendf(&sb, "%d", 42);         /* err already set -> early return */
        expect_null(sb_take(&sb), "sb:realloc-fail latches err, take returns NULL");
        siprec_sb_realloc = realloc;
    }

    /* sb_appendf as the first op, allocation fails: covers the
     * appendf reserve-failure branch (distinct from the append one). */
    {
        sb_t sb;
        sb_init(&sb);
        siprec_sb_realloc = failing_realloc;
        sb_realloc_ok = 0;
        sb_appendf(&sb, "%s", "x");        /* reserve realloc -> NULL -> err */
        expect_null(sb_take(&sb), "sb:appendf realloc-fail latches err");
        siprec_sb_realloc = realloc;
    }

    /* sb_take's shrink-to-fit realloc fails: the original (oversized)
     * buffer is kept and returned (no leak, no NULL). */
    {
        sb_t sb;
        sb_init(&sb);
        siprec_sb_realloc = failing_realloc;
        sb_realloc_ok = 1;                 /* the append grows once, then take's shrink fails */
        sb_append(&sb, "keep-me", 7);
        char *r = sb_take(&sb);            /* shrink realloc -> NULL -> returns original */
        siprec_sb_realloc = realloc;
        expect_true(r && strcmp(r, "keep-me") == 0,
            "sb:take shrink-fail keeps original buffer");
        free(r);
    }

    /* Growth ceiling: a body larger than SB_MAX_CAP (64 MB) is
     * refused — sb_can_grow_by rejects it and err latches, before any
     * buffer allocation. Exercised for both append and appendf. */
    {
        size_t huge = (size_t)65 * 1024 * 1024;
        char *big = malloc(huge + 1);
        if (big) {
            memset(big, 'a', huge);
            big[huge] = '\0';
            {
                sb_t sb;
                sb_init(&sb);
                sb_append(&sb, big, huge);           /* > cap -> reject */
                expect_null(sb_take(&sb), "sb:append over SB_MAX_CAP is refused");
            }
            {
                sb_t sb;
                sb_init(&sb);
                sb_appendf(&sb, "%s", big);          /* > cap -> reject */
                expect_null(sb_take(&sb), "sb:appendf over SB_MAX_CAP is refused");
            }
            free(big);
        }
    }
}

int main(void) {
    test_sdp_flip_direction();
    test_sdp_inject_labels();
    test_parse_remote_streams();

    test_metadata_two_participants();
    test_metadata_xml_escaping();
    test_metadata_invalid_returns_null();
    test_metadata_partial_datamode();
    test_metadata_stream_self_closes_when_unlabelled();
    test_metadata_group_self_close();
    test_metadata_reason_elements();
    test_metadata_assoc_elements();
    test_metadata_element_ordering();

    test_g711_tables_match_reference();

    test_sb_defensive_paths();

    printf("\n%d/%d passed\n", test_count - fail_count, test_count);
    return fail_count == 0 ? 0 : 1;
}
