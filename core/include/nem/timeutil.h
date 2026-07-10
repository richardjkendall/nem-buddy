#ifndef NEM_TIMEUTIL_H
#define NEM_TIMEUTIL_H

#include <stdbool.h>

bool nem_parse_iso8601(const char *s, long long *epoch_out);
int  nem_minute_of_day(long long epoch);
int  nem_day_index(long long epoch);

#endif
