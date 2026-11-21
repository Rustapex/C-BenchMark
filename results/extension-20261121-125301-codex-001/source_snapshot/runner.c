#include "runner.h"
#include "dict.h"
#include "results.h"
#include "timer.h"
#include "workload.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int parse_size(const char *text, size_t *out)
{
    unsigned long long value;
    char *end;
    const unsigned char *it = (const unsigned char *)text;
    if (!*it) return 0;
    for (; *it; ++it) if (*it < '0' || *it > '9') return 0;
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno || *end || value > SIZE_MAX) return 0;
    *out = (size_t)value;
    return 1;
}

static int parse_positive(const char *text, double *out)
{
    char *end;
    errno = 0;
    *out = strtod(text, &end);
    return *text && !errno && !*end && isfinite(*out) && *out > 0;
}

static int safe_id(const char *text)
{
    const unsigned char *it = (const unsigned char *)text;
    if (!*it) return 0;
    for (; *it; ++it)
        if (!(*it >= 'a' && *it <= 'z') && !(*it >= 'A' && *it <= 'Z') &&
            !(*it >= '0' && *it <= '9') && *it != '-' && *it != '_') return 0;
    return 1;
}

static int parse_experiment(const char *name, Experiment *out)
{
    int i;
    for (i = EXP_A5; i <= EXP_A6_DELETE; ++i)
        if (!strcmp(name, experiment_name((Experiment)i))) { *out = (Experiment)i; return 1; }
    return 0;
}

static int parse_kind(const char *name, DictKind *out)
{
    if (!strcmp(name, "DLL")) *out = DICT_LIST;
    else if (!strcmp(name, "AVL")) *out = DICT_AVL;
    else if (!strcmp(name, "HASH")) *out = DICT_HASH;
    else return 0;
    return 1;
}

static void fixture_words(WordFile *words)
{
    size_t i, j;
    words->items = calloc(WORD_COUNT_REQUIRED, sizeof *words->items);
    if (!words->items) return;
    words->count = WORD_COUNT_REQUIRED;
    for (i = 0; i < words->count; ++i) {
        size_t value = i;
        words->items[i][0] = 'w';
        for (j = 4; j > 0; --j) {
            words->items[i][j] = (char)('a' + value % 26);
            value /= 26;
        }
    }
}

static int create_file(FILE **file, const char *dir, const char *name)
{
    char path[1024];
    int length = snprintf(path, sizeof path, "%s\\%s", dir, name);
    return length > 0 && (size_t)length < sizeof path && !fopen_s(file, path, "wx") && *file;
}

static int close_file(FILE **file)
{
    int flushed = fflush(*file);
    int closed = fclose(*file);
    *file = NULL;
    return flushed || closed;
}

static int measure_repeat(const WordFile *words, size_t n, const Workload *work,
    Experiment exp, DictKind kind, size_t repeat, const char *run_id,
    const char *build_id, const char *condition_id, const TimerProbe *probe, RawRow *row)
{
    Dict *dict = NULL;
    DictConfig config = {n};
    DictStatus *actual = NULL;
    clock_t start, finish;
    size_t i, executed = 0, hits = 0, count = 0;
    int correct = 1, result = 0;
    memset(row, 0, sizeof *row);
    strcpy_s(row->run_id, sizeof row->run_id, run_id);
    strcpy_s(row->condition_id, sizeof row->condition_id, condition_id);
    strcpy_s(row->build_id, sizeof row->build_id, build_id);
    strcpy_s(row->experiment, sizeof row->experiment, experiment_name(exp));
    strcpy_s(row->structure, sizeof row->structure,
        kind == DICT_LIST ? "DLL" : dict_kind_name(kind));
    row->n = row->initial_count = n;
    row->repeat = repeat;
    row->operations = work->count;
    row->expected_hits = work->expected_hits;
    strcpy_s(row->timer, sizeof row->timer, "clock");
    strcpy_s(row->time_unit, sizeof row->time_unit, "ms");
    strcpy_s(row->time_status, sizeof row->time_status, "NOT_RUN");
    strcpy_s(row->resolution_status, sizeof row->resolution_status, "NOT_EVALUATED");
    strcpy_s(row->validation_status, sizeof row->validation_status, "FAIL");
    if (kind == DICT_HASH) {
        row->bucket_count = 2 * n;
        row->initial_alpha = (double)n / row->bucket_count;
    }
    actual = calloc(work->count, sizeof *actual);
    if (!actual || dict_create(kind, &config, &dict) != DICT_OK) {
        strcpy_s(row->error, sizeof row->error, "initial allocation failed");
        result = 4;
        goto done;
    }
    for (i = 0; i < n; ++i)
        if (dict_insert(dict, words->items[i]) != DICT_OK) {
            strcpy_s(row->error, sizeof row->error, "initial insertion failed");
            result = 3;
            goto done;
        }
    start = clock();
    if (start == (clock_t)-1) {
        strcpy_s(row->time_status, sizeof row->time_status, "TIMER_ERROR");
        strcpy_s(row->error, sizeof row->error, "clock start failed");
        result = 5;
        goto done;
    }
    for (i = 0; i < work->count; ++i) {
        const Operation *op = &work->items[i];
        actual[i] = op->kind == OP_SEARCH ? dict_search(dict, op->word) :
            op->kind == OP_INSERT ? dict_insert(dict, op->word) : dict_remove(dict, op->word);
        ++executed;
        if (actual[i] == DICT_OUT_OF_MEMORY || actual[i] == DICT_INTERNAL_ERROR) break;
    }
    finish = clock();
    if (finish == (clock_t)-1 || finish < start) {
        strcpy_s(row->time_status, sizeof row->time_status, "TIMER_ERROR");
        strcpy_s(row->error, sizeof row->error, "clock finish failed or reversed");
        result = 5;
    } else {
        row->has_elapsed = 1;
        row->elapsed_ms = 1000.0 * (double)(finish - start) / CLOCKS_PER_SEC;
        strcpy_s(row->time_status, sizeof row->time_status, "VALID");
        strcpy_s(row->resolution_status, sizeof row->resolution_status,
            resolution_status_name(timer_resolution(row->elapsed_ms, probe)));
    }
    for (i = 0; i < executed; ++i) {
        if (actual[i] != work->items[i].expected) correct = 0;
        if (work->items[i].kind == OP_SEARCH && actual[i] == DICT_FOUND) ++hits;
    }
    if (dict_size(dict, &count) != DICT_OK) correct = 0;
    if (executed != work->count || hits != work->expected_hits ||
        count != work->final_count || !dict_test_validate(dict, NULL, 0)) correct = 0;
    if (!correct && !result) result = 3;
    if (!correct && !row->error[0])
        strcpy_s(row->error, sizeof row->error, "operation or invariant mismatch");
    row->actual_hits = hits;
    row->final_count = count;
done:
    if (kind == DICT_HASH) row->final_alpha = (double)row->final_count / row->bucket_count;
    if (!result) strcpy_s(row->validation_status, sizeof row->validation_status, "PASS");
    free(actual);
    dict_destroy(&dict);
    return result;
}

int runner_main(int argc, char **argv)
{
    WordFile words = {0};
    Workload work = {0};
    RawRow rows[5];
    SummaryRow summary = {0};
    OfficialCondition official;
    TimerProbe probe = {0};
    Experiment exp;
    DictKind kind;
    FILE *raw = NULL, *summary_file = NULL, *manifest = NULL;
    char condition_id[96], error[128] = {0};
    size_t n, repeat, count = 0, line = 0;
    int fixture, result = 0, written;
    uint64_t fingerprint;
    if (argc != 11) return 2;
    fixture = !strcmp(argv[1], "--official-fixture");
    if (!fixture && strcmp(argv[1], "--official")) return 2;
    if (!parse_size(argv[3], &n) || !parse_experiment(argv[4], &exp) ||
        !parse_kind(argv[5], &kind) || !safe_id(argv[7]) || !safe_id(argv[8]) ||
        strlen(argv[7]) >= sizeof rows[0].run_id ||
        strlen(argv[8]) >= sizeof rows[0].build_id ||
        !parse_positive(argv[9], &probe.q_ms) || !argv[10][0]) return 2;
    if (fixture) fixture_words(&words);
    else result = words_load(argv[2], &words, &line, error, sizeof error);
    if (result || !words.items) {
        fprintf(stderr, "RUNNER_INPUT line=%zu error=%s\n", line, result ? error : "out of memory");
        return result ? result : 4;
    }
    result = workload_make(&words, n, exp, &work, error, sizeof error);
    if (result) { fprintf(stderr, "RUNNER_WORKLOAD error=%s\n", error); words_free(&words); return result; }
    fingerprint = workload_fingerprint(&work);
    probe.status = PROBE_OK;
    snprintf(condition_id, sizeof condition_id, "%s-%s-n%zu", experiment_name(exp),
        kind == DICT_LIST ? "DLL" : dict_kind_name(kind), n);
    if (!create_file(&raw, argv[6], "raw.csv")) {
        fprintf(stderr, "RUNNER_STORAGE raw.csv already exists or path unavailable\n");
        result = 6;
        goto done;
    }
    if (results_write_header(raw)) { result = 6; goto done; }
    for (repeat = 1; repeat <= 5; ++repeat) {
        int repeat_result = measure_repeat(&words, n, &work, exp, kind, repeat,
            argv[7], argv[8], condition_id, &probe, &rows[count]);
        if (results_write_raw(raw, &rows[count])) { result = 6; break; }
        ++count;
        if (repeat_result) { result = repeat_result; break; }
    }
    if (close_file(&raw)) result = 6;
    official.n = n;
    official.operations = work.count;
    official.expected_hits = work.expected_hits;
    official.final_count = work.final_count;
    if (!result && results_aggregate_official(rows, count, &official, &summary)) result = 3;
    if (!create_file(&summary_file, argv[6], "summary.csv")) { result = 6; goto done; }
    if (fprintf(summary_file,
        "run_id,condition_id,build_id,valid_repeats,raw_reference,median_ms,min_ms,max_ms,comparison_status\n"
        "%s,%s,%s,%zu,raw.csv,", argv[7], condition_id, argv[8],
        result ? 0 : summary.valid_repeats) < 0) { result = 6; goto done; }
    if (!result && fprintf(summary_file, "%.17g,%.17g,%.17g,", summary.median_ms,
        summary.min_ms, summary.max_ms) < 0) { result = 6; goto done; }
    if (fprintf(summary_file, "%s\n", result ? "INVALID" : summary.comparison_status) < 0 ||
        close_file(&summary_file)) result = 6;
    if (!create_file(&manifest, argv[6], "manifest.json")) { result = 6; goto done; }
    written = fprintf(manifest,
        "{\n  \"schema_version\": 2,\n  \"run_id\": \"%s\",\n  \"build_id\": \"%s\",\n"
        "  \"condition_id\": \"%s\",\n  \"input_kind\": \"%s\",\n"
        "  \"timer_policy_id\": \"%s\",\n  \"probe_status\": \"OK\",\n"
        "  \"q_ms\": %.17g,\n  \"policy_frozen_at\": \"%s\",\n"
        "  \"policy_decision_reason\": \"validated probe before pilot\",\n"
        "  \"seed\": %u,\n  \"n\": %zu,\n  \"operations_hash\": \"%016llx\",\n"
        "  \"raw_reference\": \"raw.csv\",\n  \"summary_reference\": \"summary.csv\",\n"
        "  \"raw_rows\": %zu,\n  \"status\": \"%s\"\n}\n",
        argv[7], argv[8], condition_id, fixture ? "SYNTHETIC_FIXTURE" : "ORIGINAL_FILE",
        TIMER_POLICY_ID, probe.q_ms, argv[10], (unsigned)WORD_SEED, n,
        (unsigned long long)fingerprint, count,
        result ? "FAILED" : fixture ? "FIXTURE_COMPLETED" : "COMPLETED");
    if (written < 0 || close_file(&manifest)) result = 6;
done:
    if (raw) fclose(raw);
    if (summary_file) fclose(summary_file);
    if (manifest) fclose(manifest);
    workload_free(&work);
    words_free(&words);
    printf("RUNNER condition=%s rows=%zu status=%s exit=%d\n", condition_id, count,
        result ? "FAILED" : fixture ? "FIXTURE_COMPLETED" : "COMPLETED", result);
    return result;
}
