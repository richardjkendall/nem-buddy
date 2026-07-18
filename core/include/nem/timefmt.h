#ifndef NEM_TIMEFMT_H
#define NEM_TIMEFMT_H

#include <stddef.h>

/* Render an elapsed duration as a short human string: "12s ago", "3m ago",
 * "5h ago", "2d ago". A negative `secs` means "it has not happened yet" and
 * renders as "never" — that is the caller's signal for "no successful fetch",
 * distinct from a genuine zero. Always NUL-terminates within `cap`. */
void nem_fmt_ago(long long secs, char *buf, size_t cap);

#endif
