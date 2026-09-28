/*
 * siprec_uri.c — validation of per-call (ad-hoc) SRS URIs.
 * See siprec_uri.h for why this is an allowlist.
 */
#include "siprec_uri.h"

#include <string.h>
#include <strings.h>

static int uri_char_ok(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return 1;
    }
    return c != '\0' && strchr("-._~%!*+;=:@$", c) != NULL;
}

const char *siprec_uri_check(const char *uri)
{
    size_t len, scheme_len;
    const char *p;

    if (!uri || !*uri) {
        return "empty URI";
    }

    len = strlen(uri);
    if (len > SIPREC_URI_MAX_LEN) {
        return "URI too long";
    }

    if (!strncasecmp(uri, "sip:", 4)) {
        scheme_len = 4;
    } else if (!strncasecmp(uri, "sips:", 5)) {
        scheme_len = 5;
    } else {
        return "not a sip:/sips: URI";
    }
    if (len == scheme_len) {
        return "URI has no host";
    }

    for (p = uri; *p; p++) {
        if (!uri_char_ok((unsigned char)*p)) {
            return "URI contains a character outside the allowed set";
        }
    }

    if (strstr(uri, ":_:")) {
        return "URI contains the enterprise-originate separator ':_:'";
    }

    return NULL;
}
