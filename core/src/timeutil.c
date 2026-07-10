#include "nem/timeutil.h"
#include <stdio.h>

/* Days from 1970-01-01 to civil date y-m-d. Howard Hinnant's algorithm. */
static long long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

bool nem_parse_iso8601(const char *s, long long *epoch_out) {
    if (!s) return false;
    int y, mo, d, h, mi, se;
    char t;
    /* Require the literal 'T' separator between date and time. */
    if (sscanf(s, "%4d-%2d-%2d%c%2d:%2d:%2d", &y, &mo, &d, &t, &h, &mi, &se) != 7)
        return false;
    if (t != 'T') return false;
    if (mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    if (h > 23 || mi > 59 || se > 60) return false;
    long long days = days_from_civil(y, (unsigned)mo, (unsigned)d);
    *epoch_out = days * 86400LL + h * 3600LL + mi * 60LL + se;
    return true;
}

int nem_minute_of_day(long long epoch) {
    long long secs = epoch % 86400;
    if (secs < 0) secs += 86400;
    return (int)(secs / 60);
}

int nem_day_index(long long epoch) {
    long long d = epoch / 86400;
    if (epoch < 0 && epoch % 86400 != 0) d -= 1;
    return (int)d;
}
