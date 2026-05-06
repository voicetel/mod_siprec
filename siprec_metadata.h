/*
 * siprec_metadata.h — RFC 7865 recording metadata XML builder.
 *
 * The XML half of the multipart MIME body sent on the SIP INVITE
 * from the SRC to the SRS. Spec: RFC 7865 §5.
 *
 * Pure C, no FreeSWITCH dependency. Heap-allocated string output.
 */
#ifndef SIPREC_METADATA_H
#define SIPREC_METADATA_H

#include <stddef.h>
#include <stdint.h>

/* siprec_metadata_stream: one media stream entry. RFC 7865
 * defines stream as the unit of recordable media — for a 2-leg
 * audio call we typically have 2 streams (one per direction). */
typedef struct {
    /* stream_id is the unique identifier referenced from the
     * <participant>'s <send>/<recv> elements and from the SDP's
     * a=label. RFC 7865 §5 — a UUID URN is the canonical form,
     * e.g. "urn:uuid:55c93cc4-...". */
    const char *stream_id;

    /* Mode determines the XML element under <participant> that
     * cross-references this stream — <send> for SRC-to-SRS,
     * <recv> for SRS-to-SRC (currently never; SRC is sendonly).
     * For our SRC-only implementation every stream is "send". */
    enum {
        SIPREC_STREAM_SEND,
        SIPREC_STREAM_RECV,
    } mode;

    /* The participant this stream belongs to. Index into the
     * participants array. */
    size_t participant_idx;
} siprec_metadata_stream_t;

typedef struct {
    /* participant_id — UUID URN. RFC 7865 §5. */
    const char *participant_id;

    /* nameID/aor — the SIP AOR ("Address of Record") of the
     * participant. RFC 7865 §5: "sip:caller@example.com". */
    const char *aor;

    /* Optional display name (RFC 7865 §5 nameID/name).
     * NULL omits the element. */
    const char *display_name;
} siprec_metadata_participant_t;

typedef struct {
    /* session_id — UUID URN identifying the recording session.
     * Stays stable across re-INVITEs. RFC 7865 §5. */
    const char *session_id;

    /* group_id — RFC 7865 §5 — groups participants of the same
     * logical conversation. For a 1-call recording, all
     * participants share one group. NULL omits the <group>. */
    const char *group_id;

    /* ISO-8601 timestamp, e.g. "2026-05-06T03:00:00Z", placed
     * inside <session><associate-time>. NULL emits no element. */
    const char *associate_time_utc;

    /* Participants array. */
    const siprec_metadata_participant_t *participants;
    size_t participant_count;

    /* Streams array. Cross-references participants by index. */
    const siprec_metadata_stream_t *streams;
    size_t stream_count;
} siprec_metadata_options_t;

/* siprec_metadata_build: render the XML to a heap buffer.
 * Returns NULL on allocation failure or invalid input.
 *
 * Caller frees with siprec_metadata_free().
 */
char *siprec_metadata_build(const siprec_metadata_options_t *opts);

/* Free a buffer returned by siprec_metadata_build. NULL-safe. */
void siprec_metadata_free(char *buf);

#endif /* SIPREC_METADATA_H */
