#ifndef WORD_DICTIONARY_WORKLOAD_H
#define WORD_DICTIONARY_WORKLOAD_H

#include "dict.h"

#include <stddef.h>
#include <stdint.h>

enum { WORD_COUNT_REQUIRED = 160000, WORD_SEED = 2022156023 };
typedef struct { char (*items)[16]; size_t count; } WordFile;
typedef enum { EXP_A5, EXP_A6_SEARCH, EXP_A6_INSERT, EXP_A6_DELETE } Experiment;
typedef enum { OP_SEARCH, OP_INSERT, OP_DELETE } OperationKind;
typedef struct { OperationKind kind; char word[17]; DictStatus expected; } Operation;
typedef struct {
    Operation *items;
    size_t count, searches, inserts, deletes, expected_hits, final_count;
    uint64_t random_calls;
} Workload;

int words_load(const char *path, WordFile *out, size_t *error_line, char *error, size_t error_size);
void words_free(WordFile *file);
int workload_make(const WordFile *words, size_t n, Experiment experiment, Workload *out,
    char *error, size_t error_size);
void workload_free(Workload *workload);
int workload_replay(const WordFile *words, size_t n, const Workload *workload,
    DictKind kind, size_t *actual_hits, size_t *final_count, size_t *failed_index);
uint64_t workload_fingerprint(const Workload *workload);
const char *experiment_name(Experiment experiment);

#endif
