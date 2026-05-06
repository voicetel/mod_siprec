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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_count = 0;
static int fail_count = 0;

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

/* ──────────────────────────────────────────────────────────── *
 * SDP tests                                                   *
 * ──────────────────────────────────────────────────────────── */

static void test_sdp_two_track_pcmu(void) {
    const siprec_sdp_track_t tracks[] = {
        { .label = "1", .port = 12240, .pt = 0, .codec_name = "PCMU",
          .clock_rate = 8000, .channels = 1, .ptime_ms = 20 },
        { .label = "2", .port = 12242, .pt = 0, .codec_name = "PCMU",
          .clock_rate = 8000, .channels = 1, .ptime_ms = 20 },
    };
    const char *group_labels[] = { "1", "2" };

    siprec_sdp_options_t opts = {
        .src_ip = "src.example.com",
        .session_id = 1234,
        .session_version = 5678,
        .tracks = tracks,
        .track_count = 2,
        .group_labels = group_labels,
        .group_label_count = 2,
    };

    char *sdp = siprec_sdp_build(&opts);

    check_contains(sdp, "v=0\r\n",                       "sdp:version line");
    check_contains(sdp, "o=- 1234 5678 IN IP4 src.example.com\r\n",
                                                          "sdp:origin line");
    check_contains(sdp, "s=-\r\n",                       "sdp:session-name line");
    check_contains(sdp, "c=IN IP4 src.example.com\r\n",  "sdp:connection line");
    check_contains(sdp, "t=0 0\r\n",                     "sdp:time line");
    check_contains(sdp, "a=group:DUP 1 2\r\n",           "sdp:group:DUP");
    check_contains(sdp, "m=audio 12240 RTP/AVP 0\r\n",   "sdp:m=audio track 1");
    check_contains(sdp, "m=audio 12242 RTP/AVP 0\r\n",   "sdp:m=audio track 2");
    check_contains(sdp, "a=rtpmap:0 PCMU/8000\r\n",      "sdp:rtpmap PCMU/8000");
    check_contains(sdp, "a=ptime:20\r\n",                "sdp:ptime");
    check_contains(sdp, "a=label:1\r\n",                 "sdp:label 1");
    check_contains(sdp, "a=label:2\r\n",                 "sdp:label 2");
    check_contains(sdp, "a=sendonly\r\n",                "sdp:sendonly");

    /* Channels should NOT be in the rtpmap for mono. */
    check_not_contains(sdp, "PCMU/8000/1",                "sdp:no /1 channel suffix");

    siprec_sdp_free(sdp);
}

static void test_sdp_stereo_opus(void) {
    /* Validate that the channels suffix appears for non-mono. */
    const siprec_sdp_track_t tracks[] = {
        { .label = "stereo-a", .port = 30000, .pt = 96, .codec_name = "opus",
          .clock_rate = 48000, .channels = 2, .ptime_ms = 20 },
    };

    siprec_sdp_options_t opts = {
        .src_ip = "192.0.2.10",
        .session_id = 1, .session_version = 1,
        .tracks = tracks, .track_count = 1,
    };

    char *sdp = siprec_sdp_build(&opts);
    check_contains(sdp, "a=rtpmap:96 opus/48000/2\r\n",  "sdp:opus stereo rtpmap");
    check_contains(sdp, "m=audio 30000 RTP/AVP 96\r\n",  "sdp:m=audio dynamic PT");
    check_contains(sdp, "a=label:stereo-a\r\n",          "sdp:string label");
    siprec_sdp_free(sdp);
}

static void test_sdp_invalid_returns_null(void) {
    /* Empty src_ip → NULL */
    {
        const siprec_sdp_track_t t = { .label = "1", .port = 1, .pt = 0,
            .codec_name = "PCMU", .clock_rate = 8000 };
        siprec_sdp_options_t opts = { .src_ip = "", .tracks = &t, .track_count = 1 };
        test_count++;
        if (siprec_sdp_build(&opts) == NULL) printf("PASS sdp:reject empty src_ip\n");
        else { fprintf(stderr, "FAIL sdp:empty src_ip should reject\n"); fail_count++; }
    }
    /* Zero tracks → NULL */
    {
        siprec_sdp_options_t opts = { .src_ip = "1.2.3.4", .track_count = 0 };
        test_count++;
        if (siprec_sdp_build(&opts) == NULL) printf("PASS sdp:reject zero tracks\n");
        else { fprintf(stderr, "FAIL sdp:zero tracks should reject\n"); fail_count++; }
    }
    /* Track with port=0 → NULL */
    {
        const siprec_sdp_track_t t = { .label = "x", .port = 0, .pt = 0,
            .codec_name = "PCMU", .clock_rate = 8000 };
        siprec_sdp_options_t opts = { .src_ip = "1.2.3.4", .tracks = &t, .track_count = 1 };
        test_count++;
        if (siprec_sdp_build(&opts) == NULL) printf("PASS sdp:reject port=0\n");
        else { fprintf(stderr, "FAIL sdp:port=0 should reject\n"); fail_count++; }
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
        { .stream_id = "urn:uuid:s-1", .mode = SIPREC_STREAM_SEND, .participant_idx = 0 },
        { .stream_id = "urn:uuid:s-2", .mode = SIPREC_STREAM_SEND, .participant_idx = 1 },
    };

    siprec_metadata_options_t opts = {
        .session_id = "urn:uuid:sess-abc",
        .group_id   = "urn:uuid:grp-xyz",
        .associate_time_utc = "2026-05-06T03:00:00Z",
        .participants = parts, .participant_count = 2,
        .streams = streams, .stream_count = 2,
    };

    char *xml = siprec_metadata_build(&opts);

    check_contains(xml, "<?xml version=\"1.0\"",                    "meta:xml decl");
    check_contains(xml, "xmlns=\"urn:ietf:params:xml:ns:recording:1\"", "meta:xmlns");
    check_contains(xml, "<datamode>complete</datamode>",            "meta:datamode complete");
    check_contains(xml, "<group group_id=\"urn:uuid:grp-xyz\"/>",   "meta:group");
    check_contains(xml, "<session session_id=\"urn:uuid:sess-abc\">",
                                                                     "meta:session open");
    check_contains(xml, "<associate-time>2026-05-06T03:00:00Z</associate-time>",
                                                                     "meta:associate-time");
    check_contains(xml, "</session>",                                "meta:session close");
    check_contains(xml, "<participant participant_id=\"urn:uuid:p-caller\"",
                                                                     "meta:participant alice");
    check_contains(xml, "<nameID aor=\"sip:alice@example.com\">",   "meta:nameID alice");
    check_contains(xml, "<name>Alice</name>",                        "meta:display name");
    check_contains(xml, "<send>urn:uuid:s-1</send>",                 "meta:send xref");
    check_contains(xml, "<participant participant_id=\"urn:uuid:p-callee\"",
                                                                     "meta:participant bob");
    /* Bob has no display name → self-closing nameID */
    check_contains(xml, "<nameID aor=\"sip:bob@example.com\"/>",     "meta:nameID self-close");
    check_contains(xml, "<stream stream_id=\"urn:uuid:s-1\"",        "meta:stream 1");
    check_contains(xml, "<stream stream_id=\"urn:uuid:s-2\"",        "meta:stream 2");

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

int main(void) {
    test_sdp_two_track_pcmu();
    test_sdp_stereo_opus();
    test_sdp_invalid_returns_null();

    test_metadata_two_participants();
    test_metadata_xml_escaping();
    test_metadata_invalid_returns_null();

    printf("\n%d/%d passed\n", test_count - fail_count, test_count);
    return fail_count == 0 ? 0 : 1;
}
