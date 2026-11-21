#include "results.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char header[] =
    "run_id,condition_id,build_id,experiment,structure,n,repeat,operations,timer,time_unit,elapsed_ms,time_status,expected_hits,actual_hits,initial_count,final_count,bucket_count,initial_alpha,final_alpha,validation_status,resolution_status,error";

const char *results_raw_header(void) { return header; }

static int safe_text(const char *text)
{
    return text != NULL;
}

int results_write_header(FILE *file)
{
    return file && fprintf(file, "%s\n", header) >= 0 ? 0 : 6;
}

int results_read_header(FILE *file)
{
    char line[512];
    if (!file || !fgets(line, sizeof line, file)) return 2;
    return strlen(line) == strlen(header) + 1 &&
        !memcmp(line, header, strlen(header)) && line[strlen(header)] == '\n' ? 0 : 2;
}

static int write_text(FILE *file, const char *value)
{
    const char *it;
    int quoted = strpbrk(value, ",\"\r\n") != NULL;
    if (quoted && fputc('"', file) == EOF) return 6;
    for (it = value; *it; ++it) {
        if (*it == '"' && fputc('"', file) == EOF) return 6;
        if (fputc(*it, file) == EOF) return 6;
    }
    return quoted && fputc('"', file) == EOF ? 6 : 0;
}

static int valid_status(const RawRow *row)
{
    if (!row->run_id[0] || !row->condition_id[0] || !row->build_id[0] ||
        (strcmp(row->experiment, "A5") && strcmp(row->experiment, "A6_SEARCH") &&
         strcmp(row->experiment, "A6_INSERT") && strcmp(row->experiment, "A6_DELETE")) ||
        (strcmp(row->structure, "DLL") && strcmp(row->structure, "AVL") && strcmp(row->structure, "HASH")) ||
        strcmp(row->timer, "clock") || strcmp(row->time_unit, "ms") || !row->n || !row->repeat ||
        !row->operations ||
        (strcmp(row->validation_status, "PASS") && strcmp(row->validation_status, "FAIL") &&
         strcmp(row->validation_status, "NOT_RUN")) ||
        (strcmp(row->resolution_status, "OK") && strcmp(row->resolution_status, "BELOW_RESOLUTION") &&
         strcmp(row->resolution_status, "RESOLUTION_UNCONFIRMED") &&
         strcmp(row->resolution_status, "NOT_EVALUATED"))) return 0;
    if (!strcmp(row->time_status, "VALID")) {
        if (!row->has_elapsed || !isfinite(row->elapsed_ms) || row->elapsed_ms < 0 ||
            !strcmp(row->resolution_status, "NOT_EVALUATED")) return 0;
    } else if (!strcmp(row->time_status, "TIMER_ERROR") || !strcmp(row->time_status, "NOT_RUN")) {
        if (row->has_elapsed || strcmp(row->resolution_status, "NOT_EVALUATED")) return 0;
    } else return 0;
    if (!strcmp(row->validation_status, "PASS") &&
        row->actual_hits != row->expected_hits) return 0;
    if (!strcmp(row->structure, "HASH")) {
        if (!row->bucket_count || !isfinite(row->initial_alpha) || !isfinite(row->final_alpha) ||
            row->initial_alpha < 0 || row->final_alpha < 0 ||
            fabs(row->initial_alpha - (double)row->initial_count / row->bucket_count) > 1e-8 ||
            fabs(row->final_alpha - (double)row->final_count / row->bucket_count) > 1e-8) return 0;
    } else if (row->bucket_count || row->initial_alpha || row->final_alpha) return 0;
    return safe_text(row->run_id) && safe_text(row->condition_id) && safe_text(row->build_id) &&
        safe_text(row->error);
}

int results_write_raw(FILE *file, const RawRow *row)
{
    if (!file || !row || !valid_status(row)) return 2;
    if (write_text(file, row->run_id) || fputc(',', file) == EOF ||
        write_text(file, row->condition_id) || fputc(',', file) == EOF ||
        write_text(file, row->build_id) ||
        fprintf(file, ",%s,%s,%zu,%zu,%zu,%s,%s,", row->experiment, row->structure,
            row->n, row->repeat, row->operations, row->timer, row->time_unit) < 0) return 6;
    if (row->has_elapsed && fprintf(file, "%.17g", row->elapsed_ms) < 0) return 6;
    if (fprintf(file, ",%s,%zu,%zu,%zu,%zu,", row->time_status,
            row->expected_hits, row->actual_hits, row->initial_count, row->final_count) < 0) return 6;
    if (!strcmp(row->structure, "HASH")) {
        if (fprintf(file, "%zu,%.17g,%.17g", row->bucket_count,
                row->initial_alpha, row->final_alpha) < 0) return 6;
    } else if (fputs(",,", file) == EOF) return 6;
    if (fprintf(file, ",%s,%s,", row->validation_status,
            row->resolution_status) < 0 || write_text(file, row->error) ||
        fputc('\n', file) == EOF) return 6;
    return 0;
}

static int parse_size(const char *text, size_t *out)
{
    unsigned long long number;
    char *end;
    const unsigned char *digit = (const unsigned char *)text;
    if (!*digit) return 0;
    for (; *digit; ++digit) if (*digit < '0' || *digit > '9') return 0;
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno == ERANGE || *end || number > SIZE_MAX) return 0;
    *out = (size_t)number;
    return 1;
}

static int parse_double(const char *text, double *out)
{
    char *end;
    if (!text[0] || strchr(text, ',')) return 0;
    *out = strtod(text, &end);
    return !*end && isfinite(*out) && *out >= 0;
}

static int copy_field(char *target, size_t capacity, const char *field)
{
    size_t size = strlen(field);
    if (size >= capacity) return 0;
    memcpy(target, field, size + 1);
    return 1;
}

int results_read_raw(FILE *file, RawRow *row)
{
    char buffer[2048], *fields[RAW_COLUMN_COUNT];
    size_t count = 1, used = 0;
    int c, state = 0, ended = 0;
    RawRow temp = {0};
    if (!file || !row) return 2;
    fields[0] = buffer;
    while ((c = fgetc(file)) != EOF) {
        if (c == '\0' || used + 2 >= sizeof buffer) return 2;
        if (state == 2) {
            if (c == '"') state = 3;
            else buffer[used++] = (char)c;
            continue;
        }
        if (state == 3 && c == '"') { buffer[used++] = '"'; state = 2; continue; }
        if (c == ',') {
            if (count == RAW_COLUMN_COUNT) return 2;
            buffer[used++] = '\0';
            fields[count++] = buffer + used;
            state = 0;
            continue;
        }
        if (c == '\n') {
            buffer[used] = '\0';
            ended = 1;
            break;
        }
        if (c == '\r') return 2;
        if (state == 3) return 2;
        if (c == '"') {
            if (state != 0) return 2;
            state = 2;
        } else {
            buffer[used++] = (char)c;
            state = 1;
        }
    }
    if (!ended || state == 2 || count != RAW_COLUMN_COUNT ||
        !copy_field(temp.run_id, sizeof temp.run_id, fields[0]) ||
        !copy_field(temp.condition_id, sizeof temp.condition_id, fields[1]) ||
        !copy_field(temp.build_id, sizeof temp.build_id, fields[2]) ||
        !copy_field(temp.experiment, sizeof temp.experiment, fields[3]) ||
        !copy_field(temp.structure, sizeof temp.structure, fields[4]) ||
        !parse_size(fields[5], &temp.n) || !parse_size(fields[6], &temp.repeat) ||
        !parse_size(fields[7], &temp.operations) ||
        !copy_field(temp.timer, sizeof temp.timer, fields[8]) ||
        !copy_field(temp.time_unit, sizeof temp.time_unit, fields[9]) ||
        !copy_field(temp.time_status, sizeof temp.time_status, fields[11]) ||
        !parse_size(fields[12], &temp.expected_hits) ||
        !parse_size(fields[13], &temp.actual_hits) ||
        !parse_size(fields[14], &temp.initial_count) ||
        !parse_size(fields[15], &temp.final_count) ||
        !copy_field(temp.validation_status, sizeof temp.validation_status, fields[19]) ||
        !copy_field(temp.resolution_status, sizeof temp.resolution_status, fields[20]) ||
        !copy_field(temp.error, sizeof temp.error, fields[21])) return 2;
    if (fields[10][0]) {
        if (!parse_double(fields[10], &temp.elapsed_ms)) return 2;
        temp.has_elapsed = 1;
    }
    if (fields[16][0] || fields[17][0] || fields[18][0]) {
        if (!parse_size(fields[16], &temp.bucket_count) ||
            !parse_double(fields[17], &temp.initial_alpha) ||
            !parse_double(fields[18], &temp.final_alpha)) return 2;
    }
    if (!valid_status(&temp)) return 2;
    *row = temp;
    return 0;
}

static int compare_double(const void *a, const void *b)
{
    double left = *(const double *)a, right = *(const double *)b;
    return (left > right) - (left < right);
}

int results_aggregate(const RawRow *rows, size_t count, SummaryRow *out)
{
    double *values;
    size_t i, j, valid = 0;
    int below = 0, unconfirmed = 0;
    if (!rows || !count || !out) return 2;
    memset(out, 0, sizeof *out);
    strcpy_s(out->comparison_status, sizeof out->comparison_status, "INVALID");
    values = malloc(count * sizeof *values);
    if (!values) return 4;
    for (i = 0; i < count; ++i) {
        const RawRow *row = &rows[i];
        if (!valid_status(row) || strcmp(row->run_id, rows[0].run_id) ||
            strcmp(row->condition_id, rows[0].condition_id) ||
            strcmp(row->build_id, rows[0].build_id) ||
            strcmp(row->experiment, rows[0].experiment) ||
            strcmp(row->structure, rows[0].structure) || row->n != rows[0].n ||
            strcmp(row->timer, rows[0].timer) || strcmp(row->time_unit, rows[0].time_unit) ||
            row->operations != rows[0].operations ||
            strcmp(row->validation_status, "PASS") || strcmp(row->time_status, "VALID") ||
            row->expected_hits != row->actual_hits) { free(values); return 2; }
        for (j = 0; j < i; ++j) if (row->repeat == rows[j].repeat) { free(values); return 2; }
        values[valid++] = row->elapsed_ms;
        if (!strcmp(row->resolution_status, "BELOW_RESOLUTION")) below = 1;
        if (!strcmp(row->resolution_status, "RESOLUTION_UNCONFIRMED")) unconfirmed = 1;
    }
    out->valid_repeats = valid;
    if (valid >= 5) {
        qsort(values, valid, sizeof *values, compare_double);
        out->min_ms = values[0];
        out->max_ms = values[valid - 1];
        out->median_ms = valid % 2 ? values[valid / 2] : (values[valid / 2 - 1] + values[valid / 2]) / 2;
        out->has_median = 1;
        strcpy_s(out->comparison_status, sizeof out->comparison_status,
            unconfirmed ? "RESOLUTION_UNCONFIRMED" : below ? "BELOW_RESOLUTION" : "COMPARABLE");
    }
    free(values);
    return 0;
}

int results_validate_official(const RawRow *row, const OfficialCondition *condition)
{
    size_t expected_operations;
    if (!row || !condition || !valid_status(row) ||
        (condition->n != 1000 && condition->n != 10000 && condition->n != 100000) ||
        row->n != condition->n || row->initial_count != condition->n ||
        condition->n > SIZE_MAX / 2) return 2;
    expected_operations = !strcmp(row->experiment, "A5") ? condition->n : condition->n / 2;
    if (condition->operations != expected_operations || row->operations != expected_operations) return 2;
    if (strcmp(row->validation_status, "PASS") || strcmp(row->time_status, "VALID")) return 2;
    if (row->expected_hits != condition->expected_hits ||
        row->actual_hits != condition->expected_hits ||
        row->final_count != condition->final_count) return 2;
    if (!strcmp(row->experiment, "A6_SEARCH") &&
        (condition->final_count != condition->n || condition->expected_hits > condition->operations)) return 2;
    if (!strcmp(row->experiment, "A6_INSERT") &&
        (condition->final_count != condition->n + condition->operations || condition->expected_hits)) return 2;
    if (!strcmp(row->experiment, "A6_DELETE") &&
        (condition->final_count != condition->n - condition->operations || condition->expected_hits)) return 2;
    if (!strcmp(row->structure, "HASH")) {
        if (row->bucket_count != 2 * condition->n) return 2;
    } else if (row->bucket_count) return 2;
    return 0;
}

int results_aggregate_official(const RawRow *rows, size_t count,
    const OfficialCondition *condition, SummaryRow *out)
{
    size_t i;
    if (!rows || !count || !condition || !out) return 2;
    for (i = 0; i < count; ++i)
        if (results_validate_official(&rows[i], condition) ||
            rows[i].expected_hits != rows[0].expected_hits ||
            rows[i].initial_count != rows[0].initial_count ||
            rows[i].final_count != rows[0].final_count ||
            rows[i].bucket_count != rows[0].bucket_count ||
            rows[i].initial_alpha != rows[0].initial_alpha) return 2;
    return results_aggregate(rows, count, out);
}
