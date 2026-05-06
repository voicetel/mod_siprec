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

static void test_sdp_srtp_emission(void) {
    /* RFC 4568: when srtp_crypto_suite is set the m= profile
     * flips to RTP/SAVP and an a=crypto line appears. */
    const siprec_sdp_track_t tracks[] = {
        { .label = "1", .port = 12340, .pt = 0,
          .codec_name = "PCMU", .clock_rate = 8000,
          .channels = 1, .ptime_ms = 20,
          .srtp_crypto_suite = "AES_CM_128_HMAC_SHA1_80",
          .srtp_inline_key_b64 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" },
    };
    siprec_sdp_options_t opts = {
        .src_ip = "192.0.2.10",
        .session_id = 1, .session_version = 1,
        .tracks = tracks, .track_count = 1,
    };
    char *sdp = siprec_sdp_build(&opts);
    check_contains(sdp, "m=audio 12340 RTP/SAVP 0",     "sdp:srtp m=line uses SAVP");
    check_contains(sdp, "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                                                         "sdp:srtp a=crypto");
    check_not_contains(sdp, "RTP/AVP",                   "sdp:srtp no plain AVP");
    siprec_sdp_free(sdp);
}

static void test_sdp_flip_direction(void) {
    /* RFC 7866 §6.4: pause/resume re-INVITE flips direction
     * while preserving the negotiated session — same ports,
     * same crypto, same session-id; only o=session-version
     * is bumped per RFC 4566 §5.2. */
    const char *src =
        "v=0\r\n"
        "o=- 12345 7 IN IP4 192.0.2.10\r\n"
        "s=-\r\n"
        "c=IN IP4 192.0.2.10\r\n"
        "t=0 0\r\n"
        "m=audio 30000 RTP/SAVP 0\r\n"
        "a=rtpmap:0 PCMU/8000\r\n"
        "a=ptime:20\r\n"
        "a=label:1\r\n"
        "a=sendonly\r\n"
        "a=crypto:1 AES_CM_128_HMAC_SHA1_80 inline:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n";

    char *paused = siprec_sdp_flip_direction(src, 1);
    check_contains(paused, "o=- 12345 8 IN IP4 192.0.2.10\r\n",
        "flip:o= version bumped on pause");
    check_contains(paused, "a=inactive\r\n",   "flip:a=inactive on pause");
    check_not_contains(paused, "a=sendonly",   "flip:no a=sendonly on pause");
    check_contains(paused, "m=audio 30000 RTP/SAVP 0\r\n",
        "flip:m= preserved on pause");
    check_contains(paused, "a=crypto:1 AES_CM_128_HMAC_SHA1_80",
        "flip:a=crypto preserved on pause");
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
        { .stream_id = "urn:uuid:s-1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0, .label = "1", .media_type = "audio" },
        { .stream_id = "urn:uuid:s-2", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 1, .label = "2", .media_type = "audio" },
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
    /* session carries group_ref binding to its group */
    check_contains(xml, "<session session_id=\"urn:uuid:sess-abc\" group_ref=\"urn:uuid:grp-xyz\">",
                                                                     "meta:session open with group_ref");
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

    /* RFC 7865 §5: <stream> body MUST include <label> + <media-type> */
    check_contains(xml, "<stream stream_id=\"urn:uuid:s-1\"",        "meta:stream 1");
    check_contains(xml, "<stream stream_id=\"urn:uuid:s-2\"",        "meta:stream 2");
    check_contains(xml, "<label>1</label>",                          "meta:stream label 1");
    check_contains(xml, "<label>2</label>",                          "meta:stream label 2");
    check_contains(xml, "<media-type>audio</media-type>",            "meta:stream media-type");
    check_not_contains(xml, "<stream stream_id=\"urn:uuid:s-1\" session_id=\"urn:uuid:sess-abc\"/>",
                                                                     "meta:stream not self-closed");

    /* RFC 7865 §5: <session> binds to <group> via group_ref */
    check_contains(xml, "group_ref=\"urn:uuid:grp-xyz\"",            "meta:session group_ref");

    /* <group> with associate-time gets a body */
    check_contains(xml, "<group group_id=\"urn:uuid:grp-xyz\">",     "meta:group with body open");
    check_contains(xml, "</group>",                                   "meta:group close");

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

static void test_metadata_reason_elements(void) {
    /* RFC 7865 §5: <reason> is allowed inside <group>,
     * <session>, and <participant>. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x", .display_name = NULL,
          .reason = "transferred" },
    };
    siprec_metadata_options_t opts = {
        .session_id    = "sess",
        .group_id      = "grp",
        .associate_time_utc = "2026-05-06T03:00:00Z",
        .session_reason = "paused",
        .group_reason  = "merged",
        .participants  = parts, .participant_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);
    check_contains(xml, "<reason>paused</reason>",       "meta:session reason");
    check_contains(xml, "<reason>merged</reason>",       "meta:group reason");
    check_contains(xml, "<reason>transferred</reason>",  "meta:participant reason");
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

static void test_metadata_stream_default_media_type(void) {
    /* When .media_type is NULL the builder defaults to audio. */
    const siprec_metadata_participant_t parts[] = {
        { .participant_id = "p1", .aor = "sip:a@x", .display_name = NULL },
    };
    const siprec_metadata_stream_t streams[] = {
        { .stream_id = "s1", .mode = SIPREC_STREAM_SEND,
          .participant_idx = 0, .label = "1", .media_type = NULL },
    };
    siprec_metadata_options_t opts = {
        .session_id = "sess",
        .participants = parts, .participant_count = 1,
        .streams = streams, .stream_count = 1,
    };
    char *xml = siprec_metadata_build(&opts);
    check_contains(xml, "<media-type>audio</media-type>",
        "meta:default media-type is audio");
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
    test_sdp_srtp_emission();
    test_sdp_flip_direction();
    test_sdp_invalid_returns_null();

    test_metadata_two_participants();
    test_metadata_xml_escaping();
    test_metadata_invalid_returns_null();
    test_metadata_partial_datamode();
    test_metadata_stream_default_media_type();
    test_metadata_reason_elements();
    test_metadata_assoc_elements();

    printf("\n%d/%d passed\n", test_count - fail_count, test_count);
    return fail_count == 0 ? 0 : 1;
}
