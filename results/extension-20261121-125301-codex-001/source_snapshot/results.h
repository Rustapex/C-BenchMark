#ifndef WORD_DICTIONARY_RESULTS_H
#define WORD_DICTIONARY_RESULTS_H

#include <stddef.h>
#include <stdio.h>

#define RAW_SCHEMA_VERSION 2
#define RAW_COLUMN_COUNT 22

typedef struct {
    char run_id[96], condition_id[96], build_id[96], experiment[24], structure[8];
    size_t n, repeat, operations;
    char timer[16], time_unit[8];
    double elapsed_ms;
    int has_elapsed;
    char time_status[24];
    size_t expected_hits, actual_hits, initial_count, final_count;
    size_t bucket_count;
    double initial_alpha, final_alpha;
    char validation_status[16], resolution_status[32], error[128];
} RawRow;

typedef struct {
    size_t valid_repeats;
    double median_ms, min_ms, max_ms;
    int has_median;
    char comparison_status[32];
} SummaryRow;

typedef struct {
    size_t n, operations, expected_hits, final_count;
} OfficialCondition;

const char *results_raw_header(void);
int results_write_header(FILE *file);
int results_read_header(FILE *file);
int results_write_raw(FILE *file, const RawRow *row);
int results_read_raw(FILE *file, RawRow *row);
int results_aggregate(const RawRow *rows, size_t count, SummaryRow *out);
int results_validate_official(const RawRow *row, const OfficialCondition *condition);
int results_aggregate_official(const RawRow *rows, size_t count,
    const OfficialCondition *condition, SummaryRow *out);

#endif
