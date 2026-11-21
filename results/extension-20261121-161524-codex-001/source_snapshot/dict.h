#ifndef WORD_DICTIONARY_DICT_H
#define WORD_DICTIONARY_DICT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Dict Dict;

typedef enum { DICT_LIST, DICT_AVL, DICT_HASH } DictKind;
typedef enum {
    DICT_OK = 0, DICT_FOUND, DICT_NOT_FOUND, DICT_ALREADY_EXISTS,
    DICT_INVALID_INPUT, DICT_OUT_OF_MEMORY, DICT_IO_ERROR, DICT_INTERNAL_ERROR
} DictStatus;
typedef struct { size_t initial_n; } DictConfig;
typedef bool (*DictVisitor)(const char *word, void *context);

DictStatus dict_create(DictKind kind, const DictConfig *config, Dict **out);
DictStatus dict_insert(Dict *dict, const char *word);
DictStatus dict_search(const Dict *dict, const char *word);
DictStatus dict_remove(Dict *dict, const char *word);
DictStatus dict_size(const Dict *dict, size_t *out_count);
DictStatus dict_visit_sorted(const Dict *dict, DictVisitor visitor, void *context);
void dict_destroy(Dict **dict);
size_t dict_capacity(const Dict *dict);
uint32_t dict_fnv1a32(const char *word);
const char *dict_status_name(DictStatus status);
const char *dict_kind_name(DictKind kind);
bool dict_test_validate(const Dict *dict, char *reason, size_t reason_size);
bool dict_test_list_order(const Dict *dict, char *out, size_t out_size);
bool dict_test_hash_chain(const Dict *dict, size_t bucket, char *out, size_t out_size);
bool dict_test_avl_shape(const Dict *dict, char *out, size_t out_size);

#ifdef DICT_TEST_HOOKS
typedef struct {
    size_t live_count;
    size_t live_bytes;
    uint64_t ownership_fingerprint;
    size_t allocation_attempts;
} DictAllocStats;

typedef struct {
    char node[16];
    char rotation[3];
    int before_height;
    int after_height;
    int balance_before;
    int child_balance_before;
} DictAvlEvent;

void dict_test_fail_allocation(size_t ordinal);
void dict_test_clear_allocation_failure(void);
DictAllocStats dict_test_alloc_stats(void);
size_t dict_test_avl_event_count(const Dict *dict);
bool dict_test_avl_event(const Dict *dict, size_t index, DictAvlEvent *out);
void dict_test_clear_avl_events(Dict *dict);
bool dict_test_fault_detected(Dict *dict, int fault_kind);
void dict_test_set_hash_equality_mutant(int mode);
#endif

#endif
