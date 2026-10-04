// Host test for netpolicy.c: private-host detection, cookie scope, build date.
#include "netpolicy.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); fails++; } } while (0)

int main(void) {
    // private / LAN hosts: verification exempt
    CHECK(netpol_host_is_private("192.168.1.20"));
    CHECK(netpol_host_is_private("10.0.0.5"));
    CHECK(netpol_host_is_private("172.16.0.1"));
    CHECK(netpol_host_is_private("172.31.255.255"));
    CHECK(netpol_host_is_private("127.0.0.1"));
    CHECK(netpol_host_is_private("169.254.3.4"));
    CHECK(netpol_host_is_private("localhost"));
    CHECK(netpol_host_is_private("nas"));
    CHECK(netpol_host_is_private("jellyfin.local"));
    CHECK(netpol_host_is_private("Media.LAN"));
    CHECK(netpol_host_is_private("box.home.arpa"));
    // public hosts: verified
    CHECK(!netpol_host_is_private("172.32.0.1"));
    CHECK(!netpol_host_is_private("8.8.8.8"));
    CHECK(!netpol_host_is_private("100.64.1.1"));
    CHECK(!netpol_host_is_private("example.com"));
    CHECK(!netpol_host_is_private("local.example.com"));
    CHECK(netpol_host_is_private("lan"));    // single-label names are LAN names
    CHECK(!netpol_host_is_private(""));
    CHECK(!netpol_host_is_private("256.1.1.1"));   // not an IPv4 literal: a (public) name
    CHECK(!netpol_is_ipv4_literal("256.1.1.1"));
    CHECK(!netpol_is_ipv4_literal("1.2.3"));
    CHECK(!netpol_is_ipv4_literal("1.2.3.4.5"));
    CHECK(netpol_is_ipv4_literal("1.2.3.4"));

    // cookie scope across redirects / segment hosts
    CHECK(netpol_cookie_host_ok("site.com", "site.com"));
    CHECK(netpol_cookie_host_ok("www.site.com", "cdn.site.com"));
    CHECK(netpol_cookie_host_ok("www.site.com", "site.com"));
    CHECK(netpol_cookie_host_ok("site.com", "a.b.site.com"));
    CHECK(netpol_cookie_host_ok("WWW.Site.com", "www.site.COM"));
    CHECK(!netpol_cookie_host_ok("site.com", "evil.com"));
    CHECK(!netpol_cookie_host_ok("site.com", "notsite.com"));
    CHECK(!netpol_cookie_host_ok("www.site.com", "site.com.evil.com"));
    CHECK(!netpol_cookie_host_ok("1.2.3.4", "1.2.3.5"));
    CHECK(netpol_cookie_host_ok("1.2.3.4", "1.2.3.4"));
    // Public suffixes: a sibling under a shared suffix is another site.
    CHECK(!netpol_cookie_host_ok("site.co.uk", "evil.co.uk"));
    CHECK(!netpol_cookie_host_ok("site.com.au", "evil.com.au"));
    CHECK(!netpol_cookie_host_ok("d1.cloudfront.net", "d2.cloudfront.net"));
    CHECK(!netpol_cookie_host_ok("user.github.io", "other.github.io"));
    CHECK(!netpol_cookie_host_ok("www.co.uk", "evil.co.uk"));
    CHECK(!netpol_cookie_host_ok("www.com.tn", "evil.com.tn"));
    CHECK(!netpol_cookie_host_ok("site.co.uk", "co.uk"));
    // ...while a site under one works as before.
    CHECK(netpol_cookie_host_ok("www.site.co.uk", "cdn.site.co.uk"));
    CHECK(netpol_cookie_host_ok("www.site.com.tn", "live.site.com.tn"));
    CHECK(netpol_cookie_host_ok("site.co.uk", "cdn.site.co.uk"));
    // A plain sibling is not trusted any more (no www. to say whose domain it is).
    CHECK(!netpol_cookie_host_ok("cdn1.provider.com", "cdn2.provider.com"));
    CHECK(netpol_cookie_host_ok("site.com", "www.site.com"));
    CHECK(!netpol_cookie_host_ok("", "site.com"));

    // __DATE__ parsing
    CHECK(netpol_date_to_unix("Jan  1 1970") == 0);          // year < 2000 rejected -> 0
    CHECK(netpol_date_to_unix("Jan  1 2000") == 946684800LL);
    CHECK(netpol_date_to_unix("Sep 27 2026") == 1790467200LL);
    CHECK(netpol_date_to_unix("Feb 29 2024") == 1709164800LL);
    CHECK(netpol_date_to_unix("Foo 1 2026") == 0);
    CHECK(netpol_date_to_unix(__DATE__) > 0);

    if (fails) { printf("test_netpolicy: %d failure(s)\n", fails); return 1; }
    printf("test_netpolicy: all ok\n");
    return 0;
}
