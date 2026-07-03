/*
 * siprec_metadata.c — RFC 7865 recording metadata XML builder.
 *
 * Pure C. The output document conforms to the schema in
 * RFC 7865 §5 (urn:ietf:params:xml:ns:recording:1).
 *
 * Element order matches the IANA-registered schema's
 * complexType <recording>. Attribute names are case-sensitive.
 *
 * XML content is escaped via xml_escape_into() to keep
 * caller-provided strings (display names, AORs, etc.) from
 * breaking the parse on the SRS side. The escaping covers the
 * RFC 4627-style five entities (& < > " ').
 */
#include "siprec_metadata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "siprec_sb.h"   /* shared growable string buffer (sb_t) */

/* xml_escape_into: append `s` to `sb`, replacing the five XML
 * entities with their numeric/named references. Used wherever
 * caller-supplied strings (AORs, display names, IDs) land in
 * element content or attribute values. */
static void xml_escape_into(sb_t *sb, const char *s) {
    if (!s) return;
    while (*s) {
        switch (*s) {
            case '&':  sb_append(sb, "&amp;",  5); break;
            case '<':  sb_append(sb, "&lt;",   4); break;
            case '>':  sb_append(sb, "&gt;",   4); break;
            case '"':  sb_append(sb, "&quot;", 6); break;
            case '\'': sb_append(sb, "&apos;", 6); break;
            default:   sb_append(sb, s, 1);        break;
        }
        s++;
    }
}

/* ──────────────────────────────────────────────────────────── *
 * Validation                                                  *
 * ──────────────────────────────────────────────────────────── */

static int validate_options(const siprec_metadata_options_t *opts) {
    if (!opts) return 0;
    if (!opts->session_id || !*opts->session_id) return 0;
    if (opts->participant_count == 0 || !opts->participants) return 0;

    for (size_t i = 0; i < opts->participant_count; i++) {
        const siprec_metadata_participant_t *p = &opts->participants[i];
        if (!p->participant_id || !*p->participant_id) return 0;
        if (!p->aor || !*p->aor) return 0;
    }

    /* Streams may legitimately be zero (a session with declared
     * participants but no media yet — re-INVITE will add them
     * later). When present, each stream's participant_idx must
     * be in range. */
    for (size_t i = 0; i < opts->stream_count; i++) {
        const siprec_metadata_stream_t *s = &opts->streams[i];
        if (!s->stream_id || !*s->stream_id) return 0;
        if (s->participant_idx >= opts->participant_count) return 0;
    }

    return 1;
}

/* ──────────────────────────────────────────────────────────── *
 * Build                                                       *
 * ──────────────────────────────────────────────────────────── */

char *siprec_metadata_build(const siprec_metadata_options_t *opts) {
    sb_t sb;
    const char *dm_str;
    int group_has_body;
    int session_has_body;

    if (!validate_options(opts)) {
        return NULL;
    }

    sb_init(&sb);

    /* XML prolog. RFC 7865 examples use UTF-8. */
    sb_appendf(&sb,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<recording xmlns=\"urn:ietf:params:xml:ns:recording:1\">\r\n");

    /* <datamode> — RFC 7865 §5.1. "complete" means this body is
     * a full snapshot; "partial" is a delta update typically
     * carried on a re-INVITE so the SRS merges rather than
     * replaces existing state. */
    dm_str =
        (opts->datamode == SIPREC_DATAMODE_PARTIAL)
            ? "partial" : "complete";
    sb_appendf(&sb, "  <datamode>%s</datamode>\r\n", dm_str);

    /* <group group_id="..."> — RFC 7865 Appendix A grouptype.
     *
     *   <xs:complexType name="group">
     *     <xs:sequence>
     *       <xs:element name="associate-time" type="dateTime"/>
     *       <xs:element name="disassociate-time" type="dateTime"/>
     *       <xs:any namespace='##other' .../>
     *     </xs:sequence>
     *     <xs:attribute name="group_id" use="required"/>
     *   </xs:complexType>
     *
     * No <reason> child in the schema; only the timestamps
     * and extension elements. */
    group_has_body =
        opts->group_id && *opts->group_id &&
        opts->associate_time_utc && *opts->associate_time_utc;

    if (opts->group_id && *opts->group_id) {
        sb_appendf(&sb, "  <group group_id=\"");
        xml_escape_into(&sb, opts->group_id);
        sb_appendf(&sb, "\"");

        if (group_has_body) {
            sb_appendf(&sb, ">\r\n");
            sb_appendf(&sb, "    <associate-time>");
            xml_escape_into(&sb, opts->associate_time_utc);
            sb_appendf(&sb, "</associate-time>\r\n");
            sb_appendf(&sb, "  </group>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }
    }

    /* <session session_id="..."> — only attribute is
     * session_id (RFC 7865 Appendix A sessiontype).
     *
     * Children appear in schema order:
     *   <sipSessionID>     — omitted (we don't track it).
     *   <reason>           — opts->session_reason.
     *   <group-ref>        — opts->group_id (NB: child element
     *                        with hyphen, NOT a group_ref
     *                        attribute).
     *   <start-time>       — opts->associate_time_utc.
     *
     * The session has no <associate-time> element in the
     * schema; that name belongs to <group> and the assoc
     * elements. <session> uses <start-time> / <stop-time>
     * instead. */
    sb_appendf(&sb, "  <session session_id=\"");
    xml_escape_into(&sb, opts->session_id);
    sb_appendf(&sb, "\"");

    session_has_body =
        (opts->session_reason && *opts->session_reason) ||
        (opts->group_id && *opts->group_id) ||
        (opts->associate_time_utc && *opts->associate_time_utc);

    if (!session_has_body) {
        sb_appendf(&sb, "/>\r\n");
    } else {
        sb_appendf(&sb, ">\r\n");
        if (opts->session_reason && *opts->session_reason) {
            sb_appendf(&sb, "    <reason>");
            xml_escape_into(&sb, opts->session_reason);
            sb_appendf(&sb, "</reason>\r\n");
        }
        if (opts->group_id && *opts->group_id) {
            sb_appendf(&sb, "    <group-ref>");
            xml_escape_into(&sb, opts->group_id);
            sb_appendf(&sb, "</group-ref>\r\n");
        }
        if (opts->associate_time_utc && *opts->associate_time_utc) {
            sb_appendf(&sb, "    <start-time>");
            xml_escape_into(&sb, opts->associate_time_utc);
            sb_appendf(&sb, "</start-time>\r\n");
        }
        sb_appendf(&sb, "  </session>\r\n");
    }

    /* <participant> entries. Per RFC 7865 Appendix A
     * (participanttype):
     *
     *   <xs:complexType name="participant">
     *     <xs:sequence>
     *       <xs:element name="nameID" .../>
     *       <xs:any namespace='##other' .../>
     *     </xs:sequence>
     *     <xs:attribute name="participant_id" use="required"/>
     *   </xs:complexType>
     *
     * Only `participant_id` is a valid attribute, and the only
     * in-namespace child is <nameID>. The participant→stream
     * mapping lives exclusively in <participantstreamassoc>;
     * <reason> is a session-level element. */
    for (size_t i = 0; i < opts->participant_count; i++) {
        const siprec_metadata_participant_t *p = &opts->participants[i];

        sb_appendf(&sb, "  <participant participant_id=\"");
        xml_escape_into(&sb, p->participant_id);
        sb_appendf(&sb, "\">\r\n");

        sb_appendf(&sb, "    <nameID aor=\"");
        xml_escape_into(&sb, p->aor);
        sb_appendf(&sb, "\"");
        if (p->display_name && *p->display_name) {
            sb_appendf(&sb, ">\r\n      <name>");
            xml_escape_into(&sb, p->display_name);
            sb_appendf(&sb, "</name>\r\n    </nameID>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }

        sb_appendf(&sb, "  </participant>\r\n");
    }

    /* <stream> entries — RFC 7865 Appendix A streamtype:
     *
     *   <xs:complexType name="stream">
     *     <xs:sequence>
     *       <xs:element name="label" minOccurs="0" maxOccurs="1"
     *                   type="xs:string"/>
     *       <xs:any namespace='##other' .../>
     *     </xs:sequence>
     *     <xs:attribute name="stream_id" use="required"/>
     *     <xs:attribute name="session_id"/>
     *   </xs:complexType>
     *
     * Only <label> is a typed in-namespace child; media type
     * is conveyed by the SDP, not by a recording: element. A
     * stream with no label collapses to a self-closing tag.
     *
     * Schema sequence: <stream> sits between <participant>
     * and the assoc elements. */
    for (size_t i = 0; i < opts->stream_count; i++) {
        const siprec_metadata_stream_t *s = &opts->streams[i];

        sb_appendf(&sb, "  <stream stream_id=\"");
        xml_escape_into(&sb, s->stream_id);
        sb_appendf(&sb, "\" session_id=\"");
        xml_escape_into(&sb, opts->session_id);
        sb_appendf(&sb, "\"");

        if (s->label && *s->label) {
            sb_appendf(&sb, ">\r\n    <label>");
            xml_escape_into(&sb, s->label);
            sb_appendf(&sb, "</label>\r\n  </stream>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }
    }

    /* <participantsessionassoc> — RFC 7865 Appendix A
     * participantsessionassoc complexType. Both participant_id
     * and session_id are required. Each declared participant
     * gets one binding so an SRS that prefers explicit
     * associations over schema implication has a definitive
     * source. Schema sequence: comes after <stream>. */
    for (size_t i = 0; i < opts->participant_count; i++) {
        const siprec_metadata_participant_t *p = &opts->participants[i];

        sb_appendf(&sb, "  <participantsessionassoc participant_id=\"");
        xml_escape_into(&sb, p->participant_id);
        sb_appendf(&sb, "\" session_id=\"");
        xml_escape_into(&sb, opts->session_id);
        sb_appendf(&sb, "\"");

        if (opts->associate_time_utc && *opts->associate_time_utc) {
            sb_appendf(&sb, ">\r\n    <associate-time>");
            xml_escape_into(&sb, opts->associate_time_utc);
            sb_appendf(&sb,
                "</associate-time>\r\n  </participantsessionassoc>\r\n");
        } else {
            sb_appendf(&sb, "/>\r\n");
        }
    }

    /* <participantstreamassoc> — RFC 7865 Appendix A
     * participantstreamassoc complexType. Carries the
     * participant→stream mapping (this is the ONLY place
     * <send>/<recv> appear; they are not allowed inside
     * <participant>). Schema declares only participant_id
     * as a required attribute. */
    for (size_t i = 0; i < opts->stream_count; i++) {
        const siprec_metadata_stream_t *s = &opts->streams[i];
        const siprec_metadata_participant_t *p;
        const char *tag;
        if (s->participant_idx >= opts->participant_count) continue;
        p = &opts->participants[s->participant_idx];

        sb_appendf(&sb, "  <participantstreamassoc participant_id=\"");
        xml_escape_into(&sb, p->participant_id);
        sb_appendf(&sb, "\">\r\n");

        tag =
            (s->mode == SIPREC_STREAM_RECV) ? "recv" : "send";
        sb_appendf(&sb, "    <%s>", tag);
        xml_escape_into(&sb, s->stream_id);
        sb_appendf(&sb, "</%s>\r\n", tag);

        sb_appendf(&sb, "  </participantstreamassoc>\r\n");
    }

    sb_appendf(&sb, "</recording>\r\n");

    return sb_take(&sb);
}

void siprec_metadata_free(char *buf) {
    free(buf);
}
