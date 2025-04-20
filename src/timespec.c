#include "timespec.h"

void timespec_add(struct timespec* ts, double increment)
{
    double integral;
    double frac = modf(increment, &integral);
    frac *= 1'000'000'000;
    ts->tv_sec += integral;
    ts->tv_nsec += frac;
    if (ts->tv_nsec >= 1'000'000'000) {
        ts->tv_nsec -= 1'000'000'000;
        ts->tv_sec++;
    }
}

void timespec_sub(struct timespec* a, struct timespec* b)
{
    a->tv_sec -= b->tv_sec;
    if (a->tv_nsec < b->tv_nsec) {
        a->tv_sec--;
        a->tv_nsec += 1000'000'000UL;
    }
    a->tv_nsec -= b->tv_nsec;
}
