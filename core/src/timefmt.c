#include "nem/timefmt.h"
#include <stdio.h>

void nem_fmt_ago(long long secs, char *buf, size_t cap)
{
    if (!buf || cap == 0) return;

    if (secs < 0)            snprintf(buf, cap, "never");
    else if (secs < 60)      snprintf(buf, cap, "%llds ago", secs);
    else if (secs < 3600)    snprintf(buf, cap, "%lldm ago", secs / 60);
    else if (secs < 86400)   snprintf(buf, cap, "%lldh ago", secs / 3600);
    else                     snprintf(buf, cap, "%lldd ago", secs / 86400);
}
