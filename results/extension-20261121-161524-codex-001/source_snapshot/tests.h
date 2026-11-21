#ifndef WORD_DICTIONARY_TESTS_H
#define WORD_DICTIONARY_TESTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    size_t checks;
    size_t failed;
    size_t cases;
    size_t diagnostic_checks;
} TestSummary;

int tests_run(FILE *accuracy_log, bool diagnostics, TestSummary *summary);
#ifdef DICT_TEST_HOOKS
int tests_run_checker_negative(FILE *accuracy_log, TestSummary *summary);
#endif

#endif
