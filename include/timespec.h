#pragma once

#include <math.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void timespec_add(struct timespec* ts, double increment);
void timespec_sub(struct timespec* a, struct timespec* b);

#ifdef __cplusplus
}
#endif
