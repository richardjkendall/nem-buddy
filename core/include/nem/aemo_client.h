#ifndef NEM_AEMO_CLIENT_H
#define NEM_AEMO_CLIENT_H

#include <stdbool.h>
#include "nem/snapshot.h"

bool nem_aemo_parse_summary(const char *json, nem_snapshot_t *out);

#endif
