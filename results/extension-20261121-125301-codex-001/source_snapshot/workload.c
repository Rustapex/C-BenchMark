#include "workload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WORD_HASH_SLOTS = 524288 };

static void set_error(char *out, size_t capacity, const char *message)
{
    if (out && capacity) snprintf(out, capacity, "%s", message);
}

static bool valid_file_word(const char *word)
{
    size_t i, length = strlen(word);
    if (length < 3 || length > 15) return false;
    for (i = 0; i < length; ++i) if (word[i] < 'a' || word[i] > 'z') return false;
    return true;
}

int words_load(const char *path, WordFile *out, size_t *error_line, char *error, size_t error_size)
{
    FILE *input = NULL;
    char line[128];
    size_t *slots = NULL, count = 0, line_number = 0;
    char (*words)[16] = NULL;
    int result = 2;
    if (!path || !out || out->items || out->count) { set_error(error, error_size, "invalid arguments"); return 2; }
    if (error_line) *error_line = 0;
    if (fopen_s(&input, path, "rb") || !input) { set_error(error, error_size, "cannot open input"); return 2; }
    words = calloc(WORD_COUNT_REQUIRED, sizeof *words);
    slots = calloc(WORD_HASH_SLOTS, sizeof *slots);
    if (!words || !slots) { set_error(error, error_size, "out of memory"); result = 4; goto done; }
    while (fgets(line, sizeof line, input)) {
        size_t length, slot;
        ++line_number;
        if (error_line) *error_line = line_number;
        length = strlen(line);
        if (length && line[length - 1] == '\n') line[--length] = '\0';
        else if (!feof(input)) {
            int ch;
            while ((ch = fgetc(input)) != '\n' && ch != EOF) { }
            set_error(error, error_size, "line too long"); goto done;
        }
        if (length && line[length - 1] == '\r') line[--length] = '\0';
        if (!valid_file_word(line)) { set_error(error, error_size, "invalid word or empty line"); goto done; }
        if (count == WORD_COUNT_REQUIRED) { set_error(error, error_size, "more than 160000 words"); goto done; }
        slot = dict_fnv1a32(line) & (WORD_HASH_SLOTS - 1);
        while (slots[slot]) {
            if (!strcmp(words[slots[slot] - 1], line)) { set_error(error, error_size, "duplicate word"); goto done; }
            slot = (slot + 1) & (WORD_HASH_SLOTS - 1);
        }
        strcpy_s(words[count], sizeof words[count], line);
        slots[slot] = ++count;
    }
    if (ferror(input)) { set_error(error, error_size, "read failure"); goto done; }
    if (count != WORD_COUNT_REQUIRED) { set_error(error, error_size, "not 160000 words"); goto done; }
    out->items = words;
    out->count = count;
    words = NULL;
    result = 0;
    if (error_line) *error_line = 0;
    set_error(error, error_size, "OK");
done:
    free(words);
    free(slots);
    if (fclose(input) != 0 && result == 0) {
        words_free(out);
        result = 2;
        set_error(error, error_size, "close failure");
    }
    return result;
}

void words_free(WordFile *file)
{
    if (!file) return;
    free(file->items);
    file->items = NULL;
    file->count = 0;
}

static uint32_t next_random(uint32_t *state, uint64_t *calls)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    ++*calls;
    return *state;
}

const char *experiment_name(Experiment experiment)
{
    static const char *names[] = {"A5", "A6_SEARCH", "A6_INSERT", "A6_DELETE"};
    return (unsigned)experiment < 4 ? names[experiment] : "INVALID";
}

int workload_make(const WordFile *words, size_t n, Experiment experiment, Workload *out,
    char *error, size_t error_size)
{
    size_t *live = NULL, live_count, next, i, length;
    uint32_t state = WORD_SEED;
    Workload candidate = {0};
    if (!words || !words->items || words->count != WORD_COUNT_REQUIRED || !out || out->items ||
        (n != 1000 && n != 10000 && n != 100000) || (unsigned)experiment > EXP_A6_DELETE ||
        n > SIZE_MAX / 2) {
        set_error(error, error_size, "invalid workload arguments"); return 2;
    }
    length = experiment == EXP_A5 ? n : n / 2;
    live = malloc(2 * n * sizeof *live);
    candidate.items = calloc(length, sizeof *candidate.items);
    if (!live || !candidate.items) { set_error(error, error_size, "out of memory"); goto fail; }
    for (i = 0; i < n; ++i) live[i] = i;
    live_count = next = n;
    for (i = 0; i < length; ++i) {
        Operation *op = &candidate.items[i];
        OperationKind kind;
        size_t index;
        if (experiment == EXP_A5) {
            uint32_t r = next_random(&state, &candidate.random_calls) % 100;
            kind = r < 70 ? OP_SEARCH : r < 90 ? OP_INSERT : OP_DELETE;
        } else kind = experiment == EXP_A6_SEARCH ? OP_SEARCH :
            experiment == EXP_A6_INSERT ? OP_INSERT : OP_DELETE;
        if (!live_count && kind != OP_INSERT) kind = OP_INSERT;
        op->kind = kind;
        if (kind == OP_INSERT) {
            if (next >= words->count || live_count >= 2 * n) { set_error(error, error_size, "word capacity exhausted"); goto fail; }
            strcpy_s(op->word, sizeof op->word, words->items[next]);
            live[live_count++] = next++;
            op->expected = DICT_OK;
            ++candidate.inserts;
        } else if (kind == OP_DELETE) {
            index = next_random(&state, &candidate.random_calls) % live_count;
            strcpy_s(op->word, sizeof op->word, words->items[live[index]]);
            live[index] = live[--live_count];
            op->expected = DICT_OK;
            ++candidate.deletes;
        } else {
            bool found = next_random(&state, &candidate.random_calls) % 100 < 70;
            index = next_random(&state, &candidate.random_calls) % live_count;
            strcpy_s(op->word, sizeof op->word, words->items[live[index]]);
            if (!found) strcat_s(op->word, sizeof op->word, "#");
            op->expected = found ? DICT_FOUND : DICT_NOT_FOUND;
            candidate.expected_hits += found ? 1 : 0;
            ++candidate.searches;
        }
    }
    candidate.count = length;
    candidate.final_count = live_count;
    *out = candidate;
    free(live);
    set_error(error, error_size, "OK");
    return 0;
fail:
    free(live);
    free(candidate.items);
    return 4;
}

void workload_free(Workload *workload)
{
    if (!workload) return;
    free(workload->items);
    memset(workload, 0, sizeof *workload);
}

int workload_replay(const WordFile *words, size_t n, const Workload *workload,
    DictKind kind, size_t *actual_hits, size_t *final_count, size_t *failed_index)
{
    Dict *dict = NULL;
    DictConfig config = {n};
    size_t i, hits = 0, count = 0;
    int result = 3;
    if (!words || !words->items || !workload || !workload->items || !actual_hits || !final_count ||
        n > words->count || (unsigned)kind > DICT_HASH) return 2;
    *actual_hits = *final_count = 0;
    if (failed_index) *failed_index = 0;
    if (dict_create(kind, &config, &dict) != DICT_OK) return 4;
    for (i = 0; i < n; ++i) {
        if (dict_insert(dict, words->items[i]) != DICT_OK) goto done;
    }
    for (i = 0; i < workload->count; ++i) {
        const Operation *op = &workload->items[i];
        DictStatus got = op->kind == OP_SEARCH ? dict_search(dict, op->word) :
            op->kind == OP_INSERT ? dict_insert(dict, op->word) : dict_remove(dict, op->word);
        if (got != op->expected) goto done;
        if (op->kind == OP_SEARCH && got == DICT_FOUND) ++hits;
    }
    if (dict_size(dict, &count) != DICT_OK || hits != workload->expected_hits ||
        count != workload->final_count) goto done;
    result = 0;
done:
    *actual_hits = hits;
    *final_count = count;
    if (failed_index) *failed_index = i;
    dict_destroy(&dict);
    return result;
}

uint64_t workload_fingerprint(const Workload *workload)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i, j;
    if (!workload) return 0;
    for (i = 0; i < workload->count; ++i) {
        const Operation *op = &workload->items[i];
        hash = (hash ^ (unsigned char)op->kind) * UINT64_C(1099511628211);
        hash = (hash ^ (unsigned char)op->expected) * UINT64_C(1099511628211);
        for (j = 0; op->word[j]; ++j)
            hash = (hash ^ (unsigned char)op->word[j]) * UINT64_C(1099511628211);
        hash = (hash ^ 0) * UINT64_C(1099511628211);
    }
    return hash;
}
