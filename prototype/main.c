#include "dict.h"
#include "tests.h"
#include "extension_tests.h"
#include "workload.h"
#include "timer.h"
#include "runner.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_line(char *buffer, size_t capacity)
{
    size_t length;
    int ch;
    if (!fgets(buffer, (int)capacity, stdin)) return feof(stdin) ? -1 : -3;
    length = strlen(buffer);
    if (length && buffer[length - 1] == '\n') {
        buffer[length - 1] = '\0';
        if (length > 1 && buffer[length - 2] == '\r') buffer[length - 2] = '\0';
        return 0;
    }
    if (feof(stdin)) return 0;
    while ((ch = getchar()) != '\n' && ch != EOF) { }
    return -2;
}

static bool parse_number(const char *text, size_t *out)
{
    unsigned long long number;
    char *end;
    if (!text || !text[0]) return false;
    for (end = (char *)text; *end; ++end) if (*end < '0' || *end > '9') return false;
    errno = 0;
    number = strtoull(text, &end, 10);
    if (errno || *end || number > SIZE_MAX) return false;
    *out = (size_t)number;
    return true;
}

static int prompt_number(const char *label, size_t minimum, size_t maximum, size_t *out)
{
    char input[80];
    int read_status;
    for (;;) {
        fputs(label, stdout);
        fflush(stdout);
        read_status = read_line(input, sizeof input);
        if (read_status == -1) return -1;
        if (read_status == -3) return -3;
        if (read_status == 0 && parse_number(input, out) && *out >= minimum && *out <= maximum) return 0;
        puts("Invalid number. Try again.");
    }
}

static bool print_word(const char *word, void *context)
{
    FILE *out = context;
    return fprintf(out, "%s\n", word) >= 0;
}

static int interactive_dictionary(DictKind kind, size_t initial_n)
{
    DictConfig config = {initial_n};
    Dict *dict = NULL;
    DictStatus status = dict_create(kind, &config, &dict);
    size_t choice;
    if (status != DICT_OK) {
        fprintf(stderr, "Create failed: %s\n", dict_status_name(status));
        return status == DICT_OUT_OF_MEMORY ? 4 : 2;
    }
    printf("Created empty %s dictionary. initial_n=%zu capacity=%zu count=0\n",
        dict_kind_name(kind), initial_n, dict_capacity(dict));
    for (;;) {
        char word[128];
        size_t count = 0;
        int read_status;
        puts("\n1 insert | 2 search | 3 remove | 4 sorted words | 5 count/settings | 0 back");
        read_status = prompt_number("Operation: ", 0, 5, &choice);
        if (read_status == -1) { puts("EOF: leaving dictionary."); break; }
        if (read_status != 0) { dict_destroy(&dict); return 2; }
        if (choice == 0) break;
        if (choice == 4) {
            puts("Sorted words:");
            status = dict_visit_sorted(dict, print_word, stdout);
            if (status != DICT_OK) { dict_destroy(&dict); return 6; }
            continue;
        }
        if (choice == 5) {
            dict_size(dict, &count);
            printf("kind=%s initial_n=%zu capacity=%zu count=%zu",
                dict_kind_name(kind), initial_n, dict_capacity(dict), count);
            if (kind == DICT_HASH) printf(" alpha=%.3f", (double)count / dict_capacity(dict));
            putchar('\n');
            continue;
        }
        fputs("Word: ", stdout);
        fflush(stdout);
        read_status = read_line(word, sizeof word);
        if (read_status == -1) { puts("EOF: leaving dictionary."); break; }
        if (read_status == -3) { dict_destroy(&dict); return 2; }
        if (read_status == -2) { puts("Input line too long; discarded."); continue; }
        status = choice == 1 ? dict_insert(dict, word) : choice == 2 ?
            dict_search(dict, word) : dict_remove(dict, word);
        if (dict_size(dict, &count) != DICT_OK) { dict_destroy(&dict); return 8; }
        printf("%s %s => %s; count=%zu\n", choice == 1 ? "insert" : choice == 2 ? "search" : "remove",
            word, dict_status_name(status), count);
        if (status == DICT_OUT_OF_MEMORY) { dict_destroy(&dict); return 4; }
        if (status == DICT_INTERNAL_ERROR) { dict_destroy(&dict); return 8; }
    }
    dict_destroy(&dict);
    return 0;
}

static int run_test_mode(const char *mode, const char *log_path)
{
    FILE *log = stdout;
    TestSummary summary;
    bool diagnostics = !strcmp(mode, "all") || !strcmp(mode, "diagnostic") ||
        !strcmp(mode, "checker-negative");
    int result;
#ifndef DICT_TEST_HOOKS
    if (diagnostics) {
        fputs("Diagnostic hooks are unavailable in Release. Use --test core.\n", stderr);
        return 2;
    }
#endif
    if (log_path) {
        FILE *existing = NULL;
        if (fopen_s(&existing, log_path, "rb") == 0) {
            fclose(existing);
            fprintf(stderr, "Refusing to overwrite accuracy log: %s\n", log_path);
            return 6;
        }
        if (fopen_s(&log, log_path, "wb") != 0 || !log) {
            fprintf(stderr, "Cannot create accuracy log: %s\n", log_path);
            return 6;
        }
    }
#ifdef DICT_TEST_HOOKS
    if (!strcmp(mode, "checker-negative")) result = tests_run_checker_negative(log, &summary);
    else
#endif
    if (!strcmp(mode, "extension")) {
#ifdef DICT_TEST_HOOKS
        diagnostics = true;
#endif
        result = extension_tests_run(log, diagnostics, &summary);
    } else result = tests_run(log, diagnostics, &summary);
    if (log_path && fclose(log) != 0) result = 6;
    printf("TEST_SUMMARY mode=%s checks=%zu failed=%zu cases=%zu diagnostic_checks=%zu exit=%d\n",
        mode, summary.checks, summary.failed, summary.cases, summary.diagnostic_checks, result);
    return result;
}

static int run_probe_mode(void)
{
    TimerProbe probe;
    ProbeStatus status = timer_probe_run(&probe);
    TimerOverhead overhead;
    printf("PROBE policy=%s status=%s elapsed_s=%.9g samples=%zu calls=%llu "
        "min_ticks=%lld median_ticks=%lld max_ticks=%lld q_ms=%.17g CLOCKS_PER_SEC=%ld\n",
        TIMER_POLICY_ID, probe_status_name(status), probe.elapsed_seconds,
        probe.positive_samples, (unsigned long long)probe.calls,
        (long long)probe.min_ticks, (long long)probe.median_ticks,
        (long long)probe.max_ticks, probe.q_ms, (long)CLOCKS_PER_SEC);
    if (status != PROBE_OK) return status == PROBE_TIMER_ERROR ? 5 : 7;
    if (timer_probe_overhead(&overhead)) return 5;
    printf("PROBE_OVERHEAD empty_pair_min_ticks=%lld empty_pair_median_ticks=%lld "
        "empty_pair_max_ticks=%lld million_call_ms=%.17g calls=1000000 checksum=%llu "
        "official_subtraction=NO\n", (long long)overhead.pair_min,
        (long long)overhead.pair_median, (long long)overhead.pair_max,
        overhead.million_call_ms, (unsigned long long)overhead.call_checksum);
    return 0;
}

static int parse_experiment(const char *name, Experiment *out)
{
    int i;
    for (i = 0; i < 4; ++i) {
        if (!strcmp(name, experiment_name((Experiment)i))) { *out = (Experiment)i; return 1; }
    }
    return 0;
}

static int parse_kind(const char *name, DictKind *out)
{
    if (!strcmp(name, "LIST")) *out = DICT_LIST;
    else if (!strcmp(name, "AVL")) *out = DICT_AVL;
    else if (!strcmp(name, "HASH")) *out = DICT_HASH;
    else return 0;
    return 1;
}

static int run_workload_mode(const char *path, const char *number, const char *experiment_text,
    const char *kind_text)
{
    WordFile words = {0};
    Workload workload = {0};
    Experiment experiment;
    DictKind kind = DICT_LIST;
    char error[128];
    size_t n, line = 0, hits = 0, final_count = 0, failed = 0;
    int result;
    if (!parse_number(number, &n) || !parse_experiment(experiment_text, &experiment) ||
        (kind_text && !parse_kind(kind_text, &kind))) return 2;
    result = words_load(path, &words, &line, error, sizeof error);
    if (result) { fprintf(stderr, "T01 line=%zu error=%s path=%s\n", line, error, path); return result; }
    result = workload_make(&words, n, experiment, &workload, error, sizeof error);
    if (result) { fprintf(stderr, "T09 error=%s\n", error); words_free(&words); return result; }
    printf("WORKLOAD experiment=%s n=%zu operations=%zu search=%zu insert=%zu delete=%zu "
        "expected_hits=%zu final_count=%zu random_calls=%llu fingerprint=%016llx\n",
        experiment_name(experiment), n, workload.count, workload.searches,
        workload.inserts, workload.deletes, workload.expected_hits, workload.final_count,
        (unsigned long long)workload.random_calls,
        (unsigned long long)workload_fingerprint(&workload));
    if (kind_text) {
        result = workload_replay(&words, n, &workload, kind, &hits, &final_count, &failed);
        printf("REPLAY kind=%s actual_hits=%zu final_count=%zu failed_index=%zu result=%s\n",
            dict_kind_name(kind), hits, final_count, failed, result ? "FAIL" : "PASS");
    }
    workload_free(&workload);
    words_free(&words);
    return result;
}

static int run_init_mode(const char *path, const char *number)
{
    WordFile words = {0};
    DictConfig config;
    Dict *dict = NULL;
    char error[128];
    size_t n, i, line = 0;
    clock_t start, finish;
    int result;
    if (!parse_number(number, &n) || (n != 1000 && n != 10000 && n != 100000)) return 2;
    result = words_load(path, &words, &line, error, sizeof error);
    if (result) { fprintf(stderr, "T01 line=%zu error=%s\n", line, error); return result; }
    config.initial_n = n;
    if (dict_create(DICT_LIST, &config, &dict) != DICT_OK) { words_free(&words); return 4; }
    start = clock();
    if (start == (clock_t)-1) { dict_destroy(&dict); words_free(&words); return 5; }
    for (i = 0; i < n; ++i) {
        if (dict_insert(dict, words.items[i]) != DICT_OK) {
            fprintf(stderr, "INIT_FAIL n=%zu inserted=%zu\n", n, i);
            dict_destroy(&dict); words_free(&words); return 3;
        }
        if ((i + 1) % 1000 == 0) { printf("INIT_PROGRESS n=%zu inserted=%zu\n", n, i + 1); fflush(stdout); }
    }
    finish = clock();
    if (finish == (clock_t)-1 || finish < start) {
        dict_destroy(&dict);
        words_free(&words);
        return 5;
    }
    printf("INIT_COMPLETE n=%zu inserted=%zu elapsed_ms=%.17g timer=clock\n",
        n, n, 1000.0 * (double)(finish - start) / CLOCKS_PER_SEC);
    dict_destroy(&dict);
    words_free(&words);
    return 0;
}

static int interactive_menu(void)
{
    size_t choice;
    for (;;) {
        int input;
        puts("\nWordDictionaryPrototype | 1 interactive | 2 accuracy | 3 official measurement | 0 exit");
        input = prompt_number("Main menu: ", 0, 3, &choice);
        if (input == -1) return 0;
        if (input != 0) return 2;
        if (choice == 0) return 0;
        if (choice == 3) { puts("NOT_IMPLEMENTED: official measurement belongs to the next stage."); continue; }
        if (choice == 2) {
#ifdef DICT_TEST_HOOKS
            puts("Debug core and diagnostic checks:");
            if (run_test_mode("all", NULL) != 0) return 3;
#else
            puts("Release core checks (diagnostic hooks unavailable):");
            if (run_test_mode("core", NULL) != 0) return 3;
#endif
            continue;
        }
        {
            size_t kind, initial_n;
            int result;
            puts("Dictionary: 1 LIST | 2 AVL | 3 HASH");
            input = prompt_number("Kind: ", 1, 3, &kind);
            if (input == -1) return 0;
            if (input != 0) return 2;
            input = prompt_number("initial_n (1..100000; enter 16 for a small test): ", 1, 100000, &initial_n);
            if (input == -1) return 0;
            if (input != 0) return 2;
            result = interactive_dictionary((DictKind)(kind - 1), initial_n);
            if (result) return result;
        }
    }
}

int main(int argc, char **argv)
{
    const char *mode = NULL;
    const char *log_path = NULL;
    int i;
    if (argc == 1) return interactive_menu();
    if (argc > 1 && (!strcmp(argv[1], "--official") || !strcmp(argv[1], "--official-fixture")))
        return runner_main(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: WordDictionaryPrototype [--interactive | --test core|all|extension|checker-negative [--log PATH]]");
        puts("       WordDictionaryPrototype --workload WORDS 1000|10000|100000 A5|A6_SEARCH|A6_INSERT|A6_DELETE [LIST|AVL|HASH]");
        puts("       WordDictionaryPrototype --probe | --init WORDS 1000|10000|100000");
        puts("Exit: 0 success, 2 input, 3 accuracy, 4 memory, 5 timer, 6 storage, 7 interrupted, 8 internal.");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--probe")) return run_probe_mode();
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "--workload"))
        return run_workload_mode(argv[2], argv[3], argv[4], argc == 6 ? argv[5] : NULL);
    if (argc == 4 && !strcmp(argv[1], "--init")) return run_init_mode(argv[2], argv[3]);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--interactive")) {
            if (argc != 2) return 2;
            return interactive_menu();
        }
        if (!strcmp(argv[i], "--test") && i + 1 < argc && !mode) mode = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc && !log_path) log_path = argv[++i];
        else { fputs("Invalid arguments. Use --help.\n", stderr); return 2; }
    }
    if (!mode || (strcmp(mode, "core") && strcmp(mode, "all") && strcmp(mode, "extension") && strcmp(mode, "diagnostic") &&
        strcmp(mode, "checker-negative"))) {
        fputs("Invalid test mode. Use --help.\n", stderr);
        return 2;
    }
    return run_test_mode(mode, log_path);
}
