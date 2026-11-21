#include "timer.h"

#include <stdlib.h>
#include <string.h>

static double seconds_between(struct timespec newer, struct timespec older)
{
    return (double)(newer.tv_sec - older.tv_sec) +
        (double)(newer.tv_nsec - older.tv_nsec) / 1000000000.0;
}

static int compare_ticks(const void *left, const void *right)
{
    clock_t a = *(const clock_t *)left, b = *(const clock_t *)right;
    return (a > b) - (a < b);
}

ProbeStatus timer_probe_evaluate(size_t samples, int clock_error, int wall_error)
{
    if (wall_error) return PROBE_OBSERVATION_INTERRUPTED;
    if (clock_error) return PROBE_TIMER_ERROR;
    return samples >= 30 ? PROBE_OK : PROBE_INSUFFICIENT_SAMPLES;
}

ProbeStatus timer_probe_run(TimerProbe *out)
{
    struct timespec begin, previous_wall, now;
    clock_t previous_clock, current_clock, *changes = NULL;
    size_t capacity = 0;
    if (!out) return PROBE_NOT_RUN;
    memset(out, 0, sizeof *out);
    out->status = PROBE_NOT_RUN;
    if (timespec_get(&begin, TIME_UTC) != TIME_UTC) {
        out->status = PROBE_OBSERVATION_INTERRUPTED; return out->status;
    }
    previous_wall = begin;
    previous_clock = clock();
    if (previous_clock == (clock_t)-1) { out->status = PROBE_TIMER_ERROR; return out->status; }
    for (;;) {
        double step, total;
        if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
            out->status = PROBE_OBSERVATION_INTERRUPTED; break;
        }
        step = seconds_between(now, previous_wall);
        total = seconds_between(now, begin);
        if (step < 0 || step > 1.0 || total < 0) {
            out->status = PROBE_OBSERVATION_INTERRUPTED; break;
        }
        out->elapsed_seconds = total;
        if (total >= 5.0) {
            out->status = timer_probe_evaluate(out->positive_samples, 0, 0);
            break;
        }
        current_clock = clock();
        ++out->calls;
        if (current_clock == (clock_t)-1 || current_clock < previous_clock) {
            out->status = PROBE_TIMER_ERROR; break;
        }
        if (current_clock > previous_clock) {
            clock_t delta = current_clock - previous_clock;
            if (out->positive_samples == capacity) {
                size_t new_capacity = capacity ? capacity * 2 : 1024;
                clock_t *next = realloc(changes, new_capacity * sizeof *next);
                if (!next) { out->status = PROBE_TIMER_ERROR; break; }
                changes = next;
                capacity = new_capacity;
            }
            changes[out->positive_samples++] = delta;
        }
        previous_clock = current_clock;
        previous_wall = now;
    }
    if (out->positive_samples) {
        qsort(changes, out->positive_samples, sizeof *changes, compare_ticks);
        out->min_ticks = changes[0];
        out->median_ticks = changes[out->positive_samples / 2];
        out->max_ticks = changes[out->positive_samples - 1];
        if (out->status == PROBE_OK)
            out->q_ms = 1000.0 * (double)out->min_ticks / CLOCKS_PER_SEC;
    }
    free(changes);
    return out->status;
}

int timer_probe_overhead(TimerOverhead *out)
{
    clock_t pairs[32], before, after, first, last;
    volatile uint64_t checksum = 0;
    size_t i;
    if (!out) return 5;
    memset(out, 0, sizeof *out);
    for (i = 0; i < 32; ++i) {
        before = clock();
        after = clock();
        if (before == (clock_t)-1 || after == (clock_t)-1 || after < before) return 5;
        pairs[i] = after - before;
    }
    qsort(pairs, 32, sizeof pairs[0], compare_ticks);
    out->pair_min = pairs[0];
    out->pair_median = pairs[16];
    out->pair_max = pairs[31];
    first = clock();
    if (first == (clock_t)-1) return 5;
    for (i = 0; i < 1000000; ++i) {
        clock_t current = clock();
        if (current == (clock_t)-1) return 5;
        checksum += (uint64_t)current;
    }
    last = clock();
    if (last == (clock_t)-1 || last < first) return 5;
    out->million_call_ms = 1000.0 * (double)(last - first) / CLOCKS_PER_SEC;
    out->call_checksum = checksum;
    return 0;
}

ResolutionStatus timer_resolution(double elapsed_ms, const TimerProbe *probe)
{
    if (!probe || elapsed_ms < 0) return RESOLUTION_NOT_EVALUATED;
    if (probe->status != PROBE_OK || probe->q_ms <= 0) return RESOLUTION_UNCONFIRMED;
    return elapsed_ms <= 10.0 * probe->q_ms ? RESOLUTION_BELOW_RESOLUTION : RESOLUTION_OK;
}

const char *probe_status_name(ProbeStatus status)
{
    static const char *names[] = {"NOT_RUN", "OK", "INSUFFICIENT_SAMPLES",
        "OBSERVATION_INTERRUPTED", "TIMER_ERROR"};
    return (unsigned)status < 5 ? names[status] : "INVALID";
}

const char *resolution_status_name(ResolutionStatus status)
{
    static const char *names[] = {"OK", "BELOW_RESOLUTION", "RESOLUTION_UNCONFIRMED", "NOT_EVALUATED"};
    return (unsigned)status < 4 ? names[status] : "INVALID";
}
