#ifndef WORD_DICTIONARY_TIMER_H
#define WORD_DICTIONARY_TIMER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define TIMER_POLICY_ID "CLOCK-STD-C-01"

typedef enum {
    PROBE_NOT_RUN, PROBE_OK, PROBE_INSUFFICIENT_SAMPLES,
    PROBE_OBSERVATION_INTERRUPTED, PROBE_TIMER_ERROR
} ProbeStatus;
typedef enum {
    RESOLUTION_OK, RESOLUTION_BELOW_RESOLUTION,
    RESOLUTION_UNCONFIRMED, RESOLUTION_NOT_EVALUATED
} ResolutionStatus;
typedef struct {
    ProbeStatus status;
    double elapsed_seconds;
    size_t positive_samples;
    uint64_t calls;
    clock_t min_ticks, median_ticks, max_ticks;
    double q_ms;
} TimerProbe;
typedef struct {
    clock_t pair_min, pair_median, pair_max;
    double million_call_ms;
    uint64_t call_checksum;
} TimerOverhead;

ProbeStatus timer_probe_run(TimerProbe *out);
int timer_probe_overhead(TimerOverhead *out);
ProbeStatus timer_probe_evaluate(size_t positive_samples, int clock_error, int wall_error);
ResolutionStatus timer_resolution(double elapsed_ms, const TimerProbe *probe);
const char *probe_status_name(ProbeStatus status);
const char *resolution_status_name(ResolutionStatus status);

#endif
