#include "extension_tests.h"
#include "dict.h"
#include "results.h"
#include "timer.h"
#include "workload.h"

#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *id, *inputs, *remove, *shape, *rotations;
} ExtensionAvlFixture;

static const ExtensionAvlFixture avl_cases[] = {
    {"extension:0.2:T05-D01", "aad aab aae aaa aac", "aae", "aab(aaa,aad(aac,-))", "R:0"},
    {"extension:0.2:T05-D02", "aab aaa aad aac aae", "aaa", "aad(aab(-,aac),aae)", "L:0"},
    {"extension:0.2:T05-D03", "aad aab aae aaa", "aae", "aab(aaa,aad)", "R:1"},
    {"extension:0.2:T05-D04", "aab aaa aad aae", "aaa", "aad(aab,aae)", "L:-1"},
    {"extension:0.2:T05-D05", "aad aab aae aac", "aae", "aac(aab,aad)", "LR:-1"},
    {"extension:0.2:T05-D06", "aab aaa aad aac", "aaa", "aac(aab,aad)", "RL:1"},
    {"extension:0.2:T05-D07", "aab aaa aac aad", "aab", "aac(aaa,aad)", "none"},
    {"extension:0.2:T05-D08", "aad aab aaf aaa aae aag aah", "aaa",
        "aaf(aad(aab,aae),aag(-,aah))", "L:-1"},
    {"extension:0.2:T05-D09", "aab aad aae aak aal aah aaj aac aaf aaa aai aag", "aad",
        "aah(aae(aab(aaa,aac),aaf(-,aag)),aaj(aai,aak(-,aal)))", "R:1,RL:1"}
};

static void check(FILE *log, TestSummary *summary, const char *id, const char *step,
    const char *expected, const char *actual, int pass)
{
    ++summary->checks;
    if (!pass) ++summary->failed;
    fprintf(log, "%s\tEXTENSION\t%s\t%s\t%s\t%s\n", id, step, expected, actual,
        pass ? "PASS" : "FAIL");
}

static void test_avl(FILE *log, TestSummary *summary, bool diagnostics)
{
    static const char *expected_events[][2] = {
        {"node=aad,bf=2,child_bf=0,rotation=R,height=3>3", NULL},
        {"node=aab,bf=-2,child_bf=0,rotation=L,height=3>3", NULL},
        {"node=aad,bf=2,child_bf=1,rotation=R,height=3>2", NULL},
        {"node=aab,bf=-2,child_bf=-1,rotation=L,height=3>2", NULL},
        {"node=aad,bf=2,child_bf=-1,rotation=LR,height=3>2", NULL},
        {"node=aab,bf=-2,child_bf=1,rotation=RL,height=3>2", NULL},
        {NULL, NULL},
        {"node=aad,bf=-2,child_bf=-1,rotation=L,height=4>3", NULL},
        {"node=aac,bf=2,child_bf=1,rotation=R,height=3>2",
         "node=aae,bf=-2,child_bf=1,rotation=RL,height=5>4"}
    };
    size_t i;
    for (i = 0; i < sizeof avl_cases / sizeof avl_cases[0]; ++i) {
        const ExtensionAvlFixture *f = &avl_cases[i];
        DictConfig cfg = {16};
        Dict *dict = NULL;
        char input[160], *context = NULL, *token;
        char shape[1024] = {0};
        int inserts_ok = 1;
        DictStatus removed;
        DictStatus absent;
        ++summary->cases;
        if (dict_create(DICT_AVL, &cfg, &dict) != DICT_OK) {
            check(log, summary, f->id, "create", "OK", "ERROR", 0);
            continue;
        }
        strcpy_s(input, sizeof input, f->inputs);
        token = strtok_s(input, " ", &context);
        while (token) {
            if (dict_insert(dict, token) != DICT_OK) inserts_ok = 0;
            token = strtok_s(NULL, " ", &context);
        }
        check(log, summary, f->id, "insert_sequence", "OK", inserts_ok ? "OK" : "ERROR", inserts_ok);
#ifdef DICT_TEST_HOOKS
        if (diagnostics) dict_test_clear_avl_events(dict);
#else
        (void)diagnostics;
#endif
        removed = dict_remove(dict, f->remove);
        absent = dict_search(dict, f->remove);
        check(log, summary, f->id, "delete", "OK",
            removed == DICT_OK && absent == DICT_NOT_FOUND ? "OK" : "ERROR",
            removed == DICT_OK && absent == DICT_NOT_FOUND);
        if (!dict_test_avl_shape(dict, shape, sizeof shape)) strcpy_s(shape, sizeof shape, "INVALID");
        check(log, summary, f->id, "final_shape", f->shape, shape, !strcmp(f->shape, shape));
        check(log, summary, f->id, "all_nodes_height_bf", "valid",
            dict_test_validate(dict, NULL, 0) ? "valid" : "invalid",
            dict_test_validate(dict, NULL, 0));
#ifdef DICT_TEST_HOOKS
        if (diagnostics) {
            char events[128] = {0};
            size_t j, used = 0;
            for (j = 0; j < dict_test_avl_event_count(dict); ++j) {
                DictAvlEvent event;
                int written;
                char detail[128];
                if (!dict_test_avl_event(dict, j, &event)) continue;
                written = snprintf(events + used, sizeof events - used, "%s%s:%d",
                    used ? "," : "", event.rotation, event.child_balance_before);
                if (written <= 0 || (size_t)written >= sizeof events - used) break;
                used += (size_t)written;
                snprintf(detail, sizeof detail, "node=%s,bf=%d,child_bf=%d,rotation=%s,height=%d>%d",
                    event.node, event.balance_before, event.child_balance_before,
                    event.rotation, event.before_height, event.after_height);
                check(log, summary, f->id, "delete_event_fields",
                    j < 2 && expected_events[i][j] ? expected_events[i][j] : "UNEXPECTED_EVENT",
                    detail, j < 2 && expected_events[i][j] && !strcmp(expected_events[i][j], detail));
                ++summary->diagnostic_checks;
            }
            if (!used) strcpy_s(events, sizeof events, "none");
            check(log, summary, f->id, "delete_rotation_events", f->rotations, events,
                !strcmp(f->rotations, events));
            ++summary->diagnostic_checks;
            {
                size_t expected_count = (expected_events[i][0] != NULL) + (expected_events[i][1] != NULL);
                char expected_count_text[8], actual_count_text[8];
                snprintf(expected_count_text, sizeof expected_count_text, "%zu", expected_count);
                snprintf(actual_count_text, sizeof actual_count_text, "%zu", dict_test_avl_event_count(dict));
                check(log, summary, f->id, "delete_event_count", expected_count_text,
                    actual_count_text, expected_count == dict_test_avl_event_count(dict));
                ++summary->diagnostic_checks;
            }
        }
#endif
        dict_destroy(&dict);
    }
}

static void fake_words(WordFile *out)
{
    size_t i;
    out->items = calloc(WORD_COUNT_REQUIRED, sizeof *out->items);
    out->count = out->items ? WORD_COUNT_REQUIRED : 0;
    if (!out->items) return;
    for (i = 0; i < out->count; ++i) {
        size_t value = i, j;
        out->items[i][0] = 'w';
        for (j = 4; j > 0; --j) {
            out->items[i][j] = (char)('a' + value % 26);
            value /= 26;
        }
        out->items[i][5] = '\0';
    }
}

static void test_workload(FILE *log, TestSummary *summary)
{
    static const char *words[] = {"waaty#", "wabmm", "waaqq", "waavz#", "waaoy",
        "wabcn", "wabmn", "waarh", "waada#", "wabbr"};
    static const OperationKind kinds[] = {OP_SEARCH, OP_INSERT, OP_SEARCH, OP_SEARCH,
        OP_SEARCH, OP_SEARCH, OP_INSERT, OP_SEARCH, OP_SEARCH, OP_DELETE};
    static const DictStatus statuses[] = {DICT_NOT_FOUND, DICT_OK, DICT_FOUND, DICT_NOT_FOUND,
        DICT_FOUND, DICT_FOUND, DICT_OK, DICT_FOUND, DICT_NOT_FOUND, DICT_OK};
    WordFile file = {0};
    Workload work = {0};
    size_t i;
    char error[128] = {0};
    const char *id = "T09:REFERENCE_A5_N1000";
    ++summary->cases;
    fake_words(&file);
    if (!file.items || workload_make(&file, 1000, EXP_A5, &work, error, sizeof error)) {
        check(log, summary, id, "create", "OK", error, 0);
        words_free(&file); return;
    }
    for (i = 0; i < 10; ++i) {
        const Operation *op = &work.items[i];
        int match = op->kind == kinds[i] && op->expected == statuses[i] &&
            !strcmp(op->word, words[i]);
        check(log, summary, id, "fixed_vector", words[i], op->word, match);
    }
    check(log, summary, id, "counts_and_rng", "699/210/91/485/1119/2489",
        work.searches == 699 && work.inserts == 210 && work.deletes == 91 &&
            work.expected_hits == 485 && work.final_count == 1119 && work.random_calls == 2489 ?
            "699/210/91/485/1119/2489" : "DIFFERENT",
        work.searches == 699 && work.inserts == 210 && work.deletes == 91 &&
            work.expected_hits == 485 && work.final_count == 1119 && work.random_calls == 2489);
    for (i = DICT_LIST; i <= DICT_HASH; ++i) {
        size_t hits = 0, count = 0, failed = 0;
        int result = workload_replay(&file, 1000, &work, (DictKind)i, &hits, &count, &failed);
        check(log, summary, id, "independent_replay", "485/1119/PASS",
            result == 0 && hits == 485 && count == 1119 ? "485/1119/PASS" : "FAIL",
            result == 0 && hits == 485 && count == 1119);
    }
    workload_free(&work);
    words_free(&file);
}

/* Separate index model: it never reads a produced operation to choose its next step. */
static uint32_t reference_random(uint32_t *state, uint64_t *calls)
{
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    ++*calls;
    return value;
}

static int reference_compare(const WordFile *words, size_t n, Experiment experiment,
    const Workload *produced, size_t *difference, uint32_t *rng_at_difference,
    size_t *live_at_difference, char *diagnostic, size_t diagnostic_size)
{
    size_t *live = malloc(2 * n * sizeof *live);
    size_t count = n, next = n, i, searches = 0, inserts = 0, deletes = 0, hits = 0;
    uint32_t rng = WORD_SEED;
    uint64_t calls = 0;
    int match = 1;
    if (!live || !produced || !produced->items) { free(live); return -1; }
    for (i = 0; i < n; ++i) live[i] = i;
    *difference = produced->count;
    for (i = 0; i < (experiment == EXP_A5 ? n : n / 2); ++i) {
        OperationKind kind;
        DictStatus expected;
        const char *word;
        char absent[17];
        size_t index;
        if (experiment == EXP_A5) {
            uint32_t choice = reference_random(&rng, &calls) % 100;
            if (choice < 70) kind = OP_SEARCH;
            else if (choice < 90) kind = OP_INSERT;
            else kind = OP_DELETE;
        } else if (experiment == EXP_A6_SEARCH) kind = OP_SEARCH;
        else if (experiment == EXP_A6_INSERT) kind = OP_INSERT;
        else kind = OP_DELETE;
        if (count == 0 && kind != OP_INSERT) kind = OP_INSERT;
        if (kind == OP_INSERT) {
            if (next >= words->count || count >= 2 * n) { match = -1; break; }
            word = words->items[next];
            live[count++] = next++;
            expected = DICT_OK;
            ++inserts;
        } else if (kind == OP_DELETE) {
            index = reference_random(&rng, &calls) % count;
            word = words->items[live[index]];
            live[index] = live[count - 1];
            --count;
            expected = DICT_OK;
            ++deletes;
        } else {
            int found = reference_random(&rng, &calls) % 100 < 70;
            index = reference_random(&rng, &calls) % count;
            word = words->items[live[index]];
            if (!found) {
                snprintf(absent, sizeof absent, "%s#", word);
                word = absent;
            } else ++hits;
            expected = found ? DICT_FOUND : DICT_NOT_FOUND;
            ++searches;
        }
        if (i >= produced->count || produced->items[i].kind != kind ||
            produced->items[i].expected != expected || strcmp(produced->items[i].word, word) ||
            produced->items[i].rng_after != rng || produced->items[i].live_after != count) {
            *difference = i;
            *rng_at_difference = rng;
            *live_at_difference = count;
            snprintf(diagnostic, diagnostic_size,
                "index=%zu kind=%d/%d word=%s/%s status=%s/%s rng=%u/%u live=%zu/%zu",
                i, kind, i < produced->count ? produced->items[i].kind : -1,
                word, i < produced->count ? produced->items[i].word : "MISSING",
                dict_status_name(expected), i < produced->count ?
                    dict_status_name(produced->items[i].expected) : "MISSING",
                rng, i < produced->count ? produced->items[i].rng_after : 0,
                count, i < produced->count ? produced->items[i].live_after : 0);
            match = 0;
            break;
        }
    }
    if (match == 1 && (produced->count != (experiment == EXP_A5 ? n : n / 2) ||
        produced->searches != searches || produced->inserts != inserts ||
        produced->deletes != deletes || produced->expected_hits != hits ||
        produced->final_count != count || produced->random_calls != calls)) {
        *rng_at_difference = rng;
        *live_at_difference = count;
        snprintf(diagnostic, diagnostic_size,
            "aggregate search=%zu/%zu insert=%zu/%zu delete=%zu/%zu hits=%zu/%zu live=%zu/%zu calls=%llu/%llu",
            searches, produced->searches, inserts, produced->inserts,
            deletes, produced->deletes, hits, produced->expected_hits,
            count, produced->final_count, (unsigned long long)calls,
            (unsigned long long)produced->random_calls);
        match = 0;
    }
    free(live);
    return match;
}

static void test_full_reference(FILE *log, TestSummary *summary)
{
    static const size_t sizes[] = {1000, 10000, 100000};
    WordFile words = {0};
    size_t a, b;
    fake_words(&words);
    ++summary->cases;
    if (!words.items) {
        check(log, summary, "T09:FULL_REFERENCE", "word_fixture", "OK", "OOM", 0);
        return;
    }
    for (a = 0; a < sizeof sizes / sizeof sizes[0]; ++a) {
        for (b = EXP_A5; b <= EXP_A6_DELETE; ++b) {
            Workload work = {0};
            char error[128] = {0}, id[64], actual[256], diagnostic[200] = {0};
            size_t difference = 0, live = 0;
            uint32_t rng = 0;
            int result;
            snprintf(id, sizeof id, "T09:FULL_REFERENCE:%s:%zu", experiment_name((Experiment)b), sizes[a]);
            ++summary->cases;
            if (workload_make(&words, sizes[a], (Experiment)b, &work, error, sizeof error)) {
                check(log, summary, id, "generate", "OK", error, 0);
                continue;
            }
            result = reference_compare(&words, sizes[a], (Experiment)b, &work,
                &difference, &rng, &live, diagnostic, sizeof diagnostic);
            snprintf(actual, sizeof actual, "match=%d/records=%zu/first=%zu/rng=%u/live=%zu/%s",
                result, work.count, difference, rng, live, diagnostic);
            check(log, summary, id, "all_records_and_counts", "match=1", actual, result == 1);
            if (a == 0 && b == EXP_A5) {
                uint64_t saved_calls = work.random_calls;
                work.random_calls++;
                result = reference_compare(&words, sizes[a], (Experiment)b, &work,
                    &difference, &rng, &live, diagnostic, sizeof diagnostic);
                check(log, summary, id, "wrong_rng_call_detected", "mismatch", result == 0 ? "mismatch" : "missed", result == 0);
                work.random_calls = saved_calls;
                work.items[0].rng_after++;
                result = reference_compare(&words, sizes[a], (Experiment)b, &work,
                    &difference, &rng, &live, diagnostic, sizeof diagnostic);
                check(log, summary, id, "wrong_intermediate_rng_detected", "first=0",
                    difference == 0 && result == 0 ? "first=0" : "missed",
                    difference == 0 && result == 0);
                work.items[0].rng_after--;
                work.items[0].live_after++;
                result = reference_compare(&words, sizes[a], (Experiment)b, &work,
                    &difference, &rng, &live, diagnostic, sizeof diagnostic);
                check(log, summary, id, "wrong_intermediate_live_detected", "first=0",
                    difference == 0 && result == 0 ? "first=0" : "missed",
                    difference == 0 && result == 0);
                work.items[0].live_after--;
                work.items[0].kind = OP_DELETE;
                result = reference_compare(&words, sizes[a], (Experiment)b, &work,
                    &difference, &rng, &live, diagnostic, sizeof diagnostic);
                check(log, summary, id, "changed_record_detected", "first=0", difference == 0 && result == 0 ? "first=0" : "missed",
                    difference == 0 && result == 0);
            }
            workload_free(&work);
        }
    }
    words_free(&words);
}

static void test_full_hash_collision(FILE *log, TestSummary *summary)
{
    const char *left = "skvxieetw", *right = "hxftijesa";
    const char *id = "extension:0.2:T06-FULL";
    DictConfig config = {2};
    Dict *dict = NULL;
    size_t count = 0;
    uint32_t left_hash = dict_fnv1a32(left), right_hash = dict_fnv1a32(right);
    int pass;
    ++summary->cases;
    pass = left_hash == 2172590711u && right_hash == left_hash &&
        strlen(left) == strlen(right) && strcmp(left, right) != 0;
    check(log, summary, id, "natural_collision", "2172590711/both_length9",
        pass ? "2172590711/both_length9" : "MISMATCH", pass);
    if (dict_create(DICT_HASH, &config, &dict) != DICT_OK) {
        check(log, summary, id, "create", "OK", "ERROR", 0); return;
    }
    pass = dict_capacity(dict) == 4;
    check(log, summary, id, "fixed_m_initial", "4", pass ? "4" : "OTHER", pass);
    pass = dict_insert(dict, left) == DICT_OK && dict_insert(dict, right) == DICT_OK &&
        dict_size(dict, &count) == DICT_OK && count == 2;
    check(log, summary, id, "insert_both", "OK/count2", pass ? "OK/count2" : "ERROR", pass);
    pass = dict_search(dict, left) == DICT_FOUND && dict_search(dict, right) == DICT_FOUND;
    check(log, summary, id, "search_both", "FOUND/FOUND", pass ? "FOUND/FOUND" : "ERROR", pass);
    pass = dict_insert(dict, left) == DICT_ALREADY_EXISTS &&
        dict_insert(dict, right) == DICT_ALREADY_EXISTS &&
        dict_size(dict, &count) == DICT_OK && count == 2;
    check(log, summary, id, "duplicate_both", "ALREADY_EXISTS/count2",
        pass ? "ALREADY_EXISTS/count2" : "ERROR", pass);
    pass = dict_remove(dict, left) == DICT_OK && dict_search(dict, left) == DICT_NOT_FOUND &&
        dict_search(dict, right) == DICT_FOUND && dict_size(dict, &count) == DICT_OK && count == 1;
    check(log, summary, id, "remove_first", "OK/NOT_FOUND/FOUND/count1",
        pass ? "OK/NOT_FOUND/FOUND/count1" : "ERROR", pass);
    pass = dict_remove(dict, right) == DICT_OK && dict_remove(dict, right) == DICT_NOT_FOUND &&
        dict_size(dict, &count) == DICT_OK && count == 0 && dict_capacity(dict) == 4;
    check(log, summary, id, "remove_second", "OK/NOT_FOUND/count0/m4",
        pass ? "OK/NOT_FOUND/count0/m4" : "ERROR", pass);
    dict_destroy(&dict);
    check(log, summary, id, "destroy", "NULL", dict ? "NON_NULL" : "NULL", dict == NULL);
#ifdef DICT_TEST_HOOKS
    {
        int mode;
        for (mode = 1; mode <= 2; ++mode) {
            Dict *mutant = NULL;
            DictStatus first, second;
            if (dict_create(DICT_HASH, &config, &mutant) != DICT_OK) {
                check(log, summary, id, "mutant_create", "OK", "ERROR", 0);
                continue;
            }
            dict_test_set_hash_equality_mutant(mode);
            first = dict_insert(mutant, left);
            second = dict_insert(mutant, right);
            dict_test_set_hash_equality_mutant(0);
            pass = first == DICT_OK && second == DICT_ALREADY_EXISTS;
            check(log, summary, id, mode == 1 ? "hash_only_actual_operation_detected" :
                "hash_length_actual_operation_detected", "FALSE_DUPLICATE_DETECTED",
                pass ? "FALSE_DUPLICATE_DETECTED" : "MISSED", pass);
            dict_destroy(&mutant);
        }
    }
#endif
}

static RawRow sample_row(void)
{
    RawRow row = {0};
    strcpy_s(row.run_id, sizeof row.run_id, "fixture-run");
    strcpy_s(row.condition_id, sizeof row.condition_id, "fixture-condition");
    strcpy_s(row.build_id, sizeof row.build_id, "fixture-build");
    strcpy_s(row.experiment, sizeof row.experiment, "A5");
    strcpy_s(row.structure, sizeof row.structure, "AVL");
    row.n = row.initial_count = 1000;
    row.final_count = 1119;
    row.repeat = 1;
    row.operations = 1000;
    row.expected_hits = row.actual_hits = 485;
    strcpy_s(row.timer, sizeof row.timer, "clock");
    strcpy_s(row.time_unit, sizeof row.time_unit, "ms");
    row.has_elapsed = 1;
    row.elapsed_ms = 0;
    strcpy_s(row.time_status, sizeof row.time_status, "VALID");
    strcpy_s(row.validation_status, sizeof row.validation_status, "PASS");
    strcpy_s(row.resolution_status, sizeof row.resolution_status, "BELOW_RESOLUTION");
    return row;
}

static void test_results(FILE *log, TestSummary *summary)
{
    RawRow rows[5], readback = {0};
    SummaryRow aggregate = {0};
    OfficialCondition official = {1000, 1000, 485, 1119};
    FILE *temp = tmpfile();
    size_t i;
    int rc;
    int pass;
    RawRow negative;
    FILE *bad;
    const char *id = "T12:SCHEMA2_FIXTURE";
    ++summary->cases;
    for (i = 0; i < 5; ++i) {
        rows[i] = sample_row();
        rows[i].repeat = i + 1;
        rows[i].elapsed_ms = (double)i;
        if (i) strcpy_s(rows[i].resolution_status, sizeof rows[i].resolution_status, "OK");
    }
    if (!temp) { check(log, summary, id, "tmpfile", "OK", "ERROR", 0); return; }
    rc = results_write_header(temp);
    pass = rc == 0;
    check(log, summary, id, "header_write", "OK", pass ? "OK" : "ERROR", pass);
    rc = results_write_raw(temp, &rows[0]);
    pass = rc == 0 && !ferror(temp);
    check(log, summary, id, "zero_write", "OK", pass ? "OK" : "ERROR", pass);
    rewind(temp);
    rc = results_read_header(temp);
    check(log, summary, id, "header_read", "OK", rc ? "ERROR" : "OK", rc == 0);
    rc = results_read_raw(temp, &readback);
    pass = rc == 0 && readback.has_elapsed && readback.elapsed_ms == 0 &&
        !strcmp(readback.time_status, "VALID") &&
        !strcmp(readback.resolution_status, "BELOW_RESOLUTION");
    check(log, summary, id, "zero_roundtrip", "0/VALID/BELOW_RESOLUTION",
        pass ? "0/VALID/BELOW_RESOLUTION" : "FAIL", pass);
    fclose(temp);
    rc = results_aggregate(rows, 5, &aggregate);
    pass = rc == 0 && aggregate.has_median && aggregate.median_ms == 2 &&
        !strcmp(aggregate.comparison_status, "BELOW_RESOLUTION");
    check(log, summary, id, "five_median", "2/BELOW_RESOLUTION",
        pass ? "2/BELOW_RESOLUTION" : "FAIL", pass);
    rc = results_aggregate(rows, 4, &aggregate);
    pass = rc == 0 && !aggregate.has_median && !strcmp(aggregate.comparison_status, "INVALID");
    check(log, summary, id, "four_invalid", "INVALID/no_median",
        pass ? "INVALID/no_median" : "FAIL", pass);
    rc = results_aggregate_official(rows, 5, &official, &aggregate);
    check(log, summary, id, "official_five_accepted", "median2", rc == 0 && aggregate.median_ms == 2 ? "median2" : "FAIL",
        rc == 0 && aggregate.median_ms == 2);
    rows[2].final_count++;
    rc = results_aggregate_official(rows, 5, &official, &aggregate);
    check(log, summary, id, "official_wrong_final_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    rows[2].final_count--;
    rows[2].expected_hits--;
    rows[2].actual_hits--;
    rc = results_aggregate_official(rows, 5, &official, &aggregate);
    check(log, summary, id, "official_wrong_hits_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    rows[2].expected_hits++;
    rows[2].actual_hits++;
    rows[2].initial_count--;
    rc = results_aggregate_official(rows, 5, &official, &aggregate);
    check(log, summary, id, "official_wrong_initial_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    rows[2].initial_count++;
    rows[2].operations--;
    rc = results_aggregate_official(rows, 5, &official, &aggregate);
    check(log, summary, id, "official_wrong_operations_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    rows[2].operations++;
    {
        RawRow semantic = sample_row();
        OfficialCondition a6 = {1000, 500, 350, 1000};
        strcpy_s(semantic.experiment, sizeof semantic.experiment, "A6_SEARCH");
        semantic.operations = 500;
        semantic.expected_hits = semantic.actual_hits = 350;
        semantic.final_count = 1000;
        rc = results_validate_official(&semantic, &a6);
        check(log, summary, id, "a6_search_semantics", "ACCEPTED",
            rc ? "REJECTED" : "ACCEPTED", rc == 0);
        semantic.final_count++;
        rc = results_validate_official(&semantic, &a6);
        check(log, summary, id, "a6_wrong_final_rejected", "ERROR",
            rc ? "ERROR" : "ACCEPTED", rc != 0);
        semantic = sample_row();
        strcpy_s(semantic.structure, sizeof semantic.structure, "HASH");
        semantic.bucket_count = 2000;
        semantic.initial_alpha = 0.5;
        semantic.final_alpha = 1119.0 / 2000.0;
        rc = results_validate_official(&semantic, &official);
        check(log, summary, id, "hash_m_2n", "ACCEPTED",
            rc ? "REJECTED" : "ACCEPTED", rc == 0);
        semantic.bucket_count = 1999;
        semantic.initial_alpha = 1000.0 / 1999.0;
        semantic.final_alpha = 1119.0 / 1999.0;
        rc = results_validate_official(&semantic, &official);
        check(log, summary, id, "hash_wrong_m_rejected", "ERROR",
            rc ? "ERROR" : "ACCEPTED", rc != 0);
    }
    {
        static const char *invalid_numbers[] = {
            "18446744073709551616", "-1", "+1", " 1", "1 ", ""
        };
        for (i = 0; i < sizeof invalid_numbers / sizeof invalid_numbers[0]; ++i) {
            if (tmpfile_s(&bad)) bad = NULL;
            if (!bad) { check(log, summary, id, "numeric_tmpfile", "OK", "ERROR", 0); break; }
            fprintf(bad, "fixture-run,fixture-condition,fixture-build,A5,AVL,%s,1,1000,clock,ms,0,VALID,485,485,1000,1119,,,,PASS,OK,\n",
                invalid_numbers[i]);
            rewind(bad);
            rc = results_read_raw(bad, &readback);
            check(log, summary, id, "invalid_integer_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
            fclose(bad);
        }
    }
    rows[4].repeat = 4;
    rc = results_aggregate(rows, 5, &aggregate);
    check(log, summary, id, "duplicate_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    negative = sample_row();
    negative.elapsed_ms = -1;
    if (tmpfile_s(&bad)) bad = NULL;
    if (!bad) { check(log, summary, id, "negative_tmpfile", "OK", "ERROR", 0); return; }
    rc = results_write_raw(bad, &negative);
    check(log, summary, id, "negative_time_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    negative.elapsed_ms = NAN;
    rc = results_write_raw(bad, &negative);
    check(log, summary, id, "nan_time_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    negative = sample_row();
    strcpy_s(negative.time_status, sizeof negative.time_status, "NOT_RUN");
    strcpy_s(negative.resolution_status, sizeof negative.resolution_status, "NOT_EVALUATED");
    negative.has_elapsed = 0;
    rc = results_write_raw(bad, &negative);
    rewind(bad);
    pass = rc == 0 && results_read_raw(bad, &readback) == 0 && !readback.has_elapsed &&
        !strcmp(readback.time_status, "NOT_RUN");
    check(log, summary, id, "not_run_blank_time", "NOT_RUN/blank",
        pass ? "NOT_RUN/blank" : "FAIL", pass);
    fclose(bad);
    if (tmpfile_s(&bad)) bad = NULL;
    if (!bad) { check(log, summary, id, "quoted_tmpfile", "OK", "ERROR", 0); return; }
    negative = sample_row();
    strcpy_s(negative.error, sizeof negative.error, "comma, quote\" and\nnewline");
    rc = results_write_raw(bad, &negative);
    rewind(bad);
    pass = rc == 0 && results_read_raw(bad, &readback) == 0 &&
        !strcmp(readback.error, negative.error);
    check(log, summary, id, "quoted_roundtrip", "PASS", pass ? "PASS" : "FAIL", pass);
    fclose(bad);
    if (tmpfile_s(&bad)) bad = NULL;
    if (!bad) { check(log, summary, id, "malformed_tmpfile", "OK", "ERROR", 0); return; }
    fputs("wrong,header\n", bad);
    rewind(bad);
    rc = results_read_header(bad);
    check(log, summary, id, "wrong_header_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    fclose(bad);
    if (tmpfile_s(&bad)) bad = NULL;
    if (!bad) { check(log, summary, id, "old_tmpfile", "OK", "ERROR", 0); return; }
    fputs("a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u\n", bad);
    rewind(bad);
    rc = results_read_raw(bad, &readback);
    check(log, summary, id, "old_21_columns_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    fclose(bad);
    negative = sample_row();
    strcpy_s(negative.time_status, sizeof negative.time_status, "UNKNOWN");
    if (tmpfile_s(&bad)) bad = NULL;
    if (!bad) { check(log, summary, id, "state_tmpfile", "OK", "ERROR", 0); return; }
    rc = results_write_raw(bad, &negative);
    check(log, summary, id, "unknown_status_rejected", "ERROR", rc ? "ERROR" : "ACCEPTED", rc != 0);
    fclose(bad);
    negative = sample_row();
    rc = results_write_raw(stdin, &negative);
    check(log, summary, id, "read_only_stream_write_rejected", "STORAGE_ERROR",
        rc == 6 ? "STORAGE_ERROR" : "ACCEPTED", rc == 6);
    clearerr(stdin);
}

static void test_timer(FILE *log, TestSummary *summary)
{
    TimerProbe probe = {0};
    const char *id = "G4:TIMER_FIXTURE";
    ++summary->cases;
    check(log, summary, id, "sample_0", "INSUFFICIENT_SAMPLES",
        probe_status_name(timer_probe_evaluate(0, 0, 0)),
        timer_probe_evaluate(0, 0, 0) == PROBE_INSUFFICIENT_SAMPLES);
    check(log, summary, id, "sample_29", "INSUFFICIENT_SAMPLES",
        probe_status_name(timer_probe_evaluate(29, 0, 0)),
        timer_probe_evaluate(29, 0, 0) == PROBE_INSUFFICIENT_SAMPLES);
    check(log, summary, id, "sample_30", "OK", probe_status_name(timer_probe_evaluate(30, 0, 0)),
        timer_probe_evaluate(30, 0, 0) == PROBE_OK);
    check(log, summary, id, "clock_error", "TIMER_ERROR",
        probe_status_name(timer_probe_evaluate(30, 1, 0)),
        timer_probe_evaluate(30, 1, 0) == PROBE_TIMER_ERROR);
    check(log, summary, id, "wall_error", "OBSERVATION_INTERRUPTED",
        probe_status_name(timer_probe_evaluate(30, 0, 1)),
        timer_probe_evaluate(30, 0, 1) == PROBE_OBSERVATION_INTERRUPTED);
    probe.status = PROBE_OK;
    probe.q_ms = 1;
    check(log, summary, id, "zero_and_10q", "BELOW_RESOLUTION",
        resolution_status_name(timer_resolution(10, &probe)),
        timer_resolution(0, &probe) == RESOLUTION_BELOW_RESOLUTION &&
        timer_resolution(10, &probe) == RESOLUTION_BELOW_RESOLUTION);
    check(log, summary, id, "above_10q", "OK", resolution_status_name(timer_resolution(11, &probe)),
        timer_resolution(11, &probe) == RESOLUTION_OK);
    probe.status = PROBE_INSUFFICIENT_SAMPLES;
    check(log, summary, id, "q_unconfirmed", "RESOLUTION_UNCONFIRMED",
        resolution_status_name(timer_resolution(11, &probe)),
        timer_resolution(11, &probe) == RESOLUTION_UNCONFIRMED);
}

int extension_tests_run(FILE *log, bool diagnostics, TestSummary *summary)
{
    if (!log || !summary) return 2;
    memset(summary, 0, sizeof *summary);
    if (fprintf(log, "case_id\tkind\tstep\texpected\tactual\tresult\n") < 0) return 6;
    test_avl(log, summary, diagnostics);
    test_full_hash_collision(log, summary);
    test_workload(log, summary);
    test_full_reference(log, summary);
    test_results(log, summary);
    test_timer(log, summary);
    if (fflush(log)) return 6;
    return summary->failed ? 3 : 0;
}
