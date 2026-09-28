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
#include "siprec_uri.h"

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

static void test_metadata_uuid_to_id(void) {
    /* RFC 7865 §6.9: IDs are the UUID's 16 bytes in standard base64. */
    char id[SIPREC_METADATA_ID_LEN + 1];

    check_int(siprec_metadata_uuid_to_id("00112233-4455-6677-8899-aabbccddeeff", id),
        0, "id:canonical uuid accepted");
    check_str(id, "ABEiM0RVZneImaq7zN3u/w==", "id:canonical uuid encodes");
    check_int(siprec_metadata_uuid_to_id("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF", id),
        0, "id:bare uppercase hex accepted");
    check_str(id, "/////////////////////w==", "id:bare hex encodes");
    check_int((long)strlen(id), SIPREC_METADATA_ID_LEN, "id:24 chars");

    check_int(siprec_metadata_uuid_to_id(NULL, id), -1, "id:reject NULL");
    check_int(siprec_metadata_uuid_to_id("0011", id), -1, "id:reject short");
    check_str(id, "", "id:out empty on failure");
    check_int(siprec_metadata_uuid_to_id("00112233-4455-6677-8899-aabbccddeefg", id),
        -1, "id:reject non-hex");
    check_int(siprec_metadata_uuid_to_id("001122334-455-6677-8899-aabbccddeeff", id),
        -1, "id:reject misplaced hyphen");
    check_int(siprec_metadata_uuid_to_id("00112233-4455-6677-8899+aabbccddeeff", id),
        -1, "id:reject bad separator");
}

static void test_metadata_aor(void) {
    char *a;

    a = siprec_metadata_aor("sip:alice@example.com", "ignored");
    check_str(a, "sip:alice@example.com", "aor:sip uri kept");
    siprec_metadata_free(a);
    a = siprec_metadata_aor("SIPS:bob@example.com", NULL);
    check_str(a, "SIPS:bob@example.com", "aor:sips uri kept (case-insensitive)");
    siprec_metadata_free(a);
    a = siprec_metadata_aor("tel:+15551234567", NULL);
    check_str(a, "tel:+15551234567", "aor:tel uri kept");
    siprec_metadata_free(a);
    a = siprec_metadata_aor("1000@pbx.example.com", "other");
    check_str(a, "sip:1000@pbx.example.com", "aor:user@host gets sip:");
    siprec_metadata_free(a);
    a = siprec_metadata_aor("15551234567", "carrier.example.net");
    check_str(a, "sip:15551234567@carrier.example.net", "aor:bare number uses host");
    siprec_metadata_free(a);
    a = siprec_metadata_aor("15551234567", "");
    check_str(a, "sip:15551234567@invalid", "aor:bare number, no host");
    siprec_metadata_free(a);
    expect_null(siprec_metadata_aor(NULL, "h"), "aor:NULL rejected");
    expect_null(siprec_metadata_aor("", "h"), "aor:empty rejected");
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

/* ──────────────────────────────────────────────────────────── *
 * Ad-hoc SRS URI validation (siprec_uri_check)                *
 * ──────────────────────────────────────────────────────────── */

static void test_uri_check(void) {
    static const char *ok[] = {
        "sip:198.51.100.5:5070",
        "sip:srs@example.com:5070;transport=tcp",
        "sips:srs.example.com;transport=tls",
        "SIP:Rec%40er@host",
        "sip:a-b_c.d~e!f*g+h=i$j@host",
    };
    static const struct { const char *uri; const char *what; } bad[] = {
        { NULL,                               "uri:reject NULL" },
        { "",                                 "uri:reject empty" },
        { "sip:",                             "uri:reject scheme only" },
        { "tel:+15551234",                    "uri:reject non-sip scheme" },
        { "sip:srs,loopback/9999",            "uri:reject ',' parallel leg" },
        { "sip:srs|sofia/x/y",                "uri:reject '|' serial leg" },
        { "sip:srs:_:evil@host",              "uri:reject ':_:' enterprise leg" },
        { "sip:{x=y}srs",                     "uri:reject '{' variables" },
        { "sip:[x=y]srs",                     "uri:reject '[' variables" },
        { "sip:srs <x>",                      "uri:reject whitespace/angle" },
        { "sip:srs\ttab",                     "uri:reject tab" },
        { "sip:srs?X-Evil=1",                 "uri:reject '?' URI headers" },
        { "sip:o'brien@host",                 "uri:reject quote" },
    };
    char longuri[SIPREC_URI_MAX_LEN + 2];
    size_t i;

    for (i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        const char *why = siprec_uri_check(ok[i]);
        test_count++;
        if (!why) {
            printf("PASS uri:accept %s\n", ok[i]);
        } else {
            fprintf(stderr, "FAIL uri:accept %s: %s\n", ok[i], why);
            fail_count++;
        }
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        expect_true(siprec_uri_check(bad[i].uri) != NULL, bad[i].what);
    }

    memset(longuri, 'a', sizeof(longuri) - 1);
    memcpy(longuri, "sip:", 4);
    longuri[sizeof(longuri) - 1] = '\0';
    expect_true(siprec_uri_check(longuri) != NULL, "uri:reject over-length");
    longuri[SIPREC_URI_MAX_LEN] = '\0';
    expect_true(siprec_uri_check(longuri) == NULL, "uri:accept max-length");
}

int main(void) {
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
    test_metadata_uuid_to_id();
    test_metadata_aor();

    test_g711_tables_match_reference();

    test_sb_defensive_paths();

    test_uri_check();

    printf("\n%d/%d passed\n", test_count - fail_count, test_count);
    return fail_count == 0 ? 0 : 1;
}
