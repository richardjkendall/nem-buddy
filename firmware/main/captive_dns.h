#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

/* Start a UDP:53 task that resolves every A query to 192.168.4.1 (captive
 * portal DNS hijack). Call after the SoftAP is up. */
void captive_dns_start(void);

#endif
