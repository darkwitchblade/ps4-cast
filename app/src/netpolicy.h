// netpolicy.h — pure host-name policy shared by TLS and the HTTP clients
// (host-testable: no PS4 dependencies).
#ifndef PS4CAST_NETPOLICY_H
#define PS4CAST_NETPOLICY_H

// 1 if `host` is a dotted-quad IPv4 literal.
int netpol_is_ipv4_literal(const char *host);

// 1 if `host` names a private/LAN target: an RFC 1918, loopback or link-local
// IPv4 literal, "localhost", a single-label name, or a *.local / *.lan /
// *.home / *.internal / *.home.arpa name. HTTPS certificate verification skips
// these by default -- LAN media servers routinely use self-signed certificates
// and a public CA cannot vouch for a private address anyway.
int netpol_host_is_private(const char *host);

// 1 if a Cookie supplied for `origin` may also be sent to `target`: the same
// host or one under it, and for a www. host the rest of its domain
// (www.site.com -> cdn.site.com, but never www.co.uk -> x.co.uk). Anything
// else (a redirect to another host, even a sibling) must not receive it.
int netpol_cookie_host_ok(const char *origin, const char *target);

// Unix time (UTC midnight) of a compiler __DATE__ string ("Sep 27 2026"),
// or 0 if it cannot be parsed. Used as a floor for an obviously wrong clock.
long long netpol_date_to_unix(const char *date);

#endif
