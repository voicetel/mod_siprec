/*
 * siprec_uri.h — validation of per-call (ad-hoc) SRS URIs.
 *
 * The ad-hoc URI from `siprec <handle> <uri>` is concatenated into a
 * switch_ivr_originate dial string ("{...}sofia/<profile>/<uri>"). The
 * originate grammar treats ',' and '|' as parallel/serial target
 * separators, ":_:" as the enterprise-originate separator, '{' '}' '['
 * ']' as variable blocks and '<' '>' / whitespace specially, so an
 * untrusted value could inject an extra leg that receives the RFC 7865
 * metadata (participant AORs). Validation is therefore an allowlist,
 * not a denylist.
 *
 * Pure C, no FreeSWITCH dependency. Unit-tested in siprec_test.c.
 */
#ifndef SIPREC_URI_H
#define SIPREC_URI_H

/* Longest accepted ad-hoc URI, in bytes. */
#define SIPREC_URI_MAX_LEN 255

/* siprec_uri_check: validate an ad-hoc SRS URI.
 *
 * Accepts a "sip:" or "sips:" URI (scheme case-insensitive) with a
 * non-empty remainder, at most SIPREC_URI_MAX_LEN bytes, made only of
 * ASCII letters, digits and "-._~%!*+;=:@$", and not containing ":_:".
 * That covers user@host:port;params. URI headers ('?', '&'), quotes,
 * parentheses and IPv6 brackets are rejected.
 *
 * Returns NULL if the URI is acceptable, otherwise a short static
 * string naming the reason, suitable for a log line. */
const char *siprec_uri_check(const char *uri);

#endif /* SIPREC_URI_H */
