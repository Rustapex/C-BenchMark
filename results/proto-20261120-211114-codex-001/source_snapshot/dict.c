#include "dict.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ListNode {
    struct ListNode *prev;
    struct ListNode *next;
    char word[16];
} ListNode;

typedef struct AvlNode {
    struct AvlNode *left;
    struct AvlNode *right;
    char word[16];
    int height;
} AvlNode;

typedef struct HashNode {
    struct HashNode *next;
    char word[16];
} HashNode;

struct Dict {
    DictKind kind;
    size_t count;
    size_t capacity;
    union {
        ListNode *head;
        AvlNode *root;
        HashNode **buckets;
    } data;
#ifdef DICT_TEST_HOOKS
    DictAvlEvent events[64];
    size_t event_count;
#endif
};

#ifdef DICT_TEST_HOOKS
typedef struct { void *pointer; size_t bytes; uint64_t id; } Allocation;
static Allocation allocations[32768];
static size_t allocation_count;
static size_t allocation_bytes;
static size_t allocation_attempts;
static size_t fail_ordinal;
static uint64_t next_allocation_id = 1;

static void *core_calloc(size_t count, size_t bytes)
{
    size_t total;
    void *pointer;
    size_t i;
    if (bytes && count > SIZE_MAX / bytes) return NULL;
    total = count * bytes;
    ++allocation_attempts;
    if (fail_ordinal && allocation_attempts == fail_ordinal) return NULL;
    if (allocation_count == sizeof allocations / sizeof allocations[0]) return NULL;
    pointer = calloc(count, bytes);
    if (!pointer) return NULL;
    for (i = 0; i < sizeof allocations / sizeof allocations[0]; ++i) {
        if (!allocations[i].pointer) {
            allocations[i].pointer = pointer;
            allocations[i].bytes = total;
            allocations[i].id = next_allocation_id++;
            ++allocation_count;
            allocation_bytes += total;
            return pointer;
        }
    }
    free(pointer);
    return NULL;
}

static void core_free(void *pointer)
{
    size_t i;
    if (!pointer) return;
    for (i = 0; i < sizeof allocations / sizeof allocations[0]; ++i) {
        if (allocations[i].pointer == pointer) {
            allocation_bytes -= allocations[i].bytes;
            --allocation_count;
            allocations[i].pointer = NULL;
            allocations[i].bytes = 0;
            allocations[i].id = 0;
            break;
        }
    }
    free(pointer);
}

void dict_test_fail_allocation(size_t ordinal)
{
    allocation_attempts = 0;
    fail_ordinal = ordinal;
}

void dict_test_clear_allocation_failure(void)
{
    allocation_attempts = 0;
    fail_ordinal = 0;
}

DictAllocStats dict_test_alloc_stats(void)
{
    DictAllocStats result = {0};
    size_t i;
    result.live_count = allocation_count;
    result.live_bytes = allocation_bytes;
    result.allocation_attempts = allocation_attempts;
    for (i = 0; i < sizeof allocations / sizeof allocations[0]; ++i) {
        if (allocations[i].pointer) result.ownership_fingerprint ^= allocations[i].id;
    }
    return result;
}
#else
static void *core_calloc(size_t count, size_t bytes) { return calloc(count, bytes); }
static void core_free(void *pointer) { free(pointer); }
#endif

static bool valid_word(const char *word, bool allow_hash)
{
    size_t length = 0;
    size_t i;
    if (!word) return false;
    while (length <= 16 && word[length]) ++length;
    if (length > 16) return false;
    if (allow_hash && length >= 4 && length <= 16 && word[length - 1] == '#') {
        --length;
    } else if (length < 3 || length > 15) {
        return false;
    }
    for (i = 0; i < length; ++i) {
        if (word[i] < 'a' || word[i] > 'z') return false;
    }
    return true;
}

uint32_t dict_fnv1a32(const char *word)
{
    uint32_t value = UINT32_C(2166136261);
    const unsigned char *p = (const unsigned char *)word;
    if (!p) return value;
    while (*p) {
        value ^= *p++;
        value *= UINT32_C(16777619);
    }
    return value;
}

const char *dict_status_name(DictStatus status)
{
    static const char *names[] = {"OK", "FOUND", "NOT_FOUND", "ALREADY_EXISTS",
        "INVALID_INPUT", "OUT_OF_MEMORY", "IO_ERROR", "INTERNAL_ERROR"};
    return (unsigned)status < sizeof names / sizeof names[0] ? names[status] : "UNKNOWN";
}

const char *dict_kind_name(DictKind kind)
{
    static const char *names[] = {"LIST", "AVL", "HASH"};
    return (unsigned)kind < sizeof names / sizeof names[0] ? names[kind] : "UNKNOWN";
}

static int avl_height(const AvlNode *node) { return node ? node->height : 0; }
static int avl_balance(const AvlNode *node)
{
    return node ? avl_height(node->left) - avl_height(node->right) : 0;
}
static void avl_update(AvlNode *node)
{
    int left = avl_height(node->left);
    int right = avl_height(node->right);
    node->height = 1 + (left > right ? left : right);
}
static AvlNode *avl_rotate_right(AvlNode *node)
{
    AvlNode *left = node->left;
    node->left = left->right;
    left->right = node;
    avl_update(node);
    avl_update(left);
    return left;
}
static AvlNode *avl_rotate_left(AvlNode *node)
{
    AvlNode *right = node->right;
    node->right = right->left;
    right->left = node;
    avl_update(node);
    avl_update(right);
    return right;
}

static AvlNode *avl_rebalance(Dict *dict, AvlNode *node, bool deleting)
{
    int before_height = node->height;
    int balance;
    int child_balance = 0;
    const char *rotation = NULL;
    char old_word[16];
    AvlNode *result = node;
    avl_update(node);
    balance = avl_balance(node);
    memcpy(old_word, node->word, sizeof old_word);
    if (balance > 1) {
        child_balance = avl_balance(node->left);
        if (child_balance < 0) {
            node->left = avl_rotate_left(node->left);
            rotation = "LR";
        } else {
            rotation = "R";
        }
        result = avl_rotate_right(node);
    } else if (balance < -1) {
        child_balance = avl_balance(node->right);
        if (child_balance > 0) {
            node->right = avl_rotate_right(node->right);
            rotation = "RL";
        } else {
            rotation = "L";
        }
        result = avl_rotate_left(node);
    }
#ifdef DICT_TEST_HOOKS
    if (deleting && rotation && dict->event_count < sizeof dict->events / sizeof dict->events[0]) {
        DictAvlEvent *event = &dict->events[dict->event_count++];
        memcpy(event->node, old_word, sizeof event->node);
        strcpy_s(event->rotation, sizeof event->rotation, rotation);
        event->before_height = before_height;
        event->after_height = result->height;
        event->balance_before = balance;
        event->child_balance_before = child_balance;
    }
#else
    (void)dict;
    (void)deleting;
    (void)before_height;
    (void)child_balance;
    (void)rotation;
    (void)old_word;
#endif
    return result;
}

static AvlNode *avl_insert_node(Dict *dict, AvlNode *node, const char *word, DictStatus *status)
{
    int compare;
    if (!node) {
        node = core_calloc(1, sizeof *node);
        if (!node) { *status = DICT_OUT_OF_MEMORY; return NULL; }
        memcpy(node->word, word, strlen(word) + 1);
        node->height = 1;
        *status = DICT_OK;
        return node;
    }
    compare = strcmp(word, node->word);
    if (!compare) { *status = DICT_ALREADY_EXISTS; return node; }
    if (compare < 0) node->left = avl_insert_node(dict, node->left, word, status);
    else node->right = avl_insert_node(dict, node->right, word, status);
    return *status == DICT_OK ? avl_rebalance(dict, node, false) : node;
}

static AvlNode *avl_remove_node(Dict *dict, AvlNode *node, const char *word, bool *removed)
{
    int compare;
    if (!node) return NULL;
    compare = strcmp(word, node->word);
    if (compare < 0) node->left = avl_remove_node(dict, node->left, word, removed);
    else if (compare > 0) node->right = avl_remove_node(dict, node->right, word, removed);
    else {
        *removed = true;
        if (!node->left || !node->right) {
            AvlNode *child = node->left ? node->left : node->right;
            core_free(node);
            return child;
        }
        {
            AvlNode *successor = node->right;
            bool successor_removed = false;
            while (successor->left) successor = successor->left;
            memcpy(node->word, successor->word, sizeof node->word);
            node->right = avl_remove_node(dict, node->right, node->word, &successor_removed);
        }
    }
    return *removed ? avl_rebalance(dict, node, true) : node;
}

static void avl_destroy(AvlNode *node)
{
    if (!node) return;
    avl_destroy(node->left);
    avl_destroy(node->right);
    core_free(node);
}

DictStatus dict_create(DictKind kind, const DictConfig *config, Dict **out)
{
    Dict *dict;
    if (!out || *out || !config || !config->initial_n || (unsigned)kind > DICT_HASH) {
        return DICT_INVALID_INPUT;
    }
    *out = NULL;
    if (kind == DICT_HASH && (config->initial_n > SIZE_MAX / 2 ||
        config->initial_n * 2 > SIZE_MAX / sizeof(HashNode *))) return DICT_INVALID_INPUT;
    dict = core_calloc(1, sizeof *dict);
    if (!dict) return DICT_OUT_OF_MEMORY;
    dict->kind = kind;
    if (kind == DICT_LIST) {
        dict->data.head = core_calloc(1, sizeof(ListNode));
        if (!dict->data.head) { core_free(dict); return DICT_OUT_OF_MEMORY; }
        dict->data.head->next = dict->data.head;
        dict->data.head->prev = dict->data.head;
    } else if (kind == DICT_HASH) {
        dict->capacity = config->initial_n * 2;
        dict->data.buckets = core_calloc(dict->capacity, sizeof(HashNode *));
        if (!dict->data.buckets) { core_free(dict); return DICT_OUT_OF_MEMORY; }
    }
    *out = dict;
    return DICT_OK;
}

DictStatus dict_search(const Dict *dict, const char *word)
{
    if (!dict || !valid_word(word, true)) return DICT_INVALID_INPUT;
    if (dict->kind == DICT_LIST) {
        const ListNode *node;
        for (node = dict->data.head->next; node != dict->data.head; node = node->next)
            if (!strcmp(node->word, word)) return DICT_FOUND;
    } else if (dict->kind == DICT_AVL) {
        const AvlNode *node = dict->data.root;
        while (node) {
            int compare = strcmp(word, node->word);
            if (!compare) return DICT_FOUND;
            node = compare < 0 ? node->left : node->right;
        }
    } else {
        size_t bucket = dict_fnv1a32(word) % dict->capacity;
        const HashNode *node;
        for (node = dict->data.buckets[bucket]; node; node = node->next)
            if (!strcmp(node->word, word)) return DICT_FOUND;
    }
    return DICT_NOT_FOUND;
}

DictStatus dict_insert(Dict *dict, const char *word)
{
    DictStatus status;
    if (!dict || !valid_word(word, false)) return DICT_INVALID_INPUT;
    if (dict->count == SIZE_MAX) return DICT_INTERNAL_ERROR;
    status = dict_search(dict, word);
    if (status == DICT_FOUND) return DICT_ALREADY_EXISTS;
    if (status != DICT_NOT_FOUND) return status;
    if (dict->kind == DICT_LIST) {
        ListNode *node = core_calloc(1, sizeof *node);
        ListNode *head = dict->data.head;
        if (!node) return DICT_OUT_OF_MEMORY;
        memcpy(node->word, word, strlen(word) + 1);
        node->next = head->next;
        node->prev = head;
        head->next->prev = node;
        head->next = node;
    } else if (dict->kind == DICT_AVL) {
        dict->data.root = avl_insert_node(dict, dict->data.root, word, &status);
        if (status != DICT_OK) return status;
    } else {
        size_t bucket = dict_fnv1a32(word) % dict->capacity;
        HashNode *node = core_calloc(1, sizeof *node);
        if (!node) return DICT_OUT_OF_MEMORY;
        memcpy(node->word, word, strlen(word) + 1);
        node->next = dict->data.buckets[bucket];
        dict->data.buckets[bucket] = node;
    }
    ++dict->count;
    return DICT_OK;
}

DictStatus dict_remove(Dict *dict, const char *word)
{
    if (!dict || !valid_word(word, true)) return DICT_INVALID_INPUT;
    if (dict->kind == DICT_LIST) {
        ListNode *node;
        for (node = dict->data.head->next; node != dict->data.head; node = node->next) {
            if (!strcmp(node->word, word)) {
                node->prev->next = node->next;
                node->next->prev = node->prev;
                core_free(node);
                --dict->count;
                return DICT_OK;
            }
        }
    } else if (dict->kind == DICT_AVL) {
        bool removed = false;
        dict->data.root = avl_remove_node(dict, dict->data.root, word, &removed);
        if (removed) { --dict->count; return DICT_OK; }
    } else {
        size_t bucket = dict_fnv1a32(word) % dict->capacity;
        HashNode **link = &dict->data.buckets[bucket];
        while (*link) {
            if (!strcmp((*link)->word, word)) {
                HashNode *removed = *link;
                *link = removed->next;
                core_free(removed);
                --dict->count;
                return DICT_OK;
            }
            link = &(*link)->next;
        }
    }
    return DICT_NOT_FOUND;
}

DictStatus dict_size(const Dict *dict, size_t *out_count)
{
    if (!dict || !out_count) return DICT_INVALID_INPUT;
    *out_count = dict->count;
    return DICT_OK;
}

size_t dict_capacity(const Dict *dict)
{
    return dict && dict->kind == DICT_HASH ? dict->capacity : 0;
}

static void merge_words(const char **words, const char **scratch, size_t begin, size_t end)
{
    size_t middle;
    size_t left;
    size_t right;
    size_t cursor;
    if (end - begin < 2) return;
    middle = begin + (end - begin) / 2;
    merge_words(words, scratch, begin, middle);
    merge_words(words, scratch, middle, end);
    left = begin; right = middle;
    for (cursor = begin; cursor < end; ++cursor) {
        if (right == end || (left < middle && strcmp(words[left], words[right]) <= 0))
            scratch[cursor] = words[left++];
        else scratch[cursor] = words[right++];
    }
    for (cursor = begin; cursor < end; ++cursor) words[cursor] = scratch[cursor];
}

static bool avl_visit(const AvlNode *node, DictVisitor visitor, void *context)
{
    if (!node) return true;
    return avl_visit(node->left, visitor, context) && visitor(node->word, context) &&
        avl_visit(node->right, visitor, context);
}

DictStatus dict_visit_sorted(const Dict *dict, DictVisitor visitor, void *context)
{
    const char **words;
    const char **scratch;
    size_t count = 0;
    size_t i;
    DictStatus result = DICT_OK;
    if (!dict || !visitor) return DICT_INVALID_INPUT;
    if (dict->kind == DICT_AVL) return avl_visit(dict->data.root, visitor, context) ? DICT_OK : DICT_IO_ERROR;
    if (!dict->count) return DICT_OK;
    if (dict->count > SIZE_MAX / sizeof *words) return DICT_OUT_OF_MEMORY;
    words = core_calloc(dict->count, sizeof *words);
    if (!words) return DICT_OUT_OF_MEMORY;
    scratch = core_calloc(dict->count, sizeof *scratch);
    if (!scratch) { core_free((void *)words); return DICT_OUT_OF_MEMORY; }
    if (dict->kind == DICT_LIST) {
        const ListNode *node = dict->data.head->next;
        while (node != dict->data.head && count < dict->count) {
            words[count++] = node->word;
            node = node->next;
        }
        if (node != dict->data.head) result = DICT_INTERNAL_ERROR;
    } else {
        for (i = 0; i < dict->capacity && result == DICT_OK; ++i) {
            const HashNode *node = dict->data.buckets[i];
            while (node) {
                if (count == dict->count) { result = DICT_INTERNAL_ERROR; break; }
                words[count++] = node->word;
                node = node->next;
            }
        }
    }
    if (count != dict->count) result = DICT_INTERNAL_ERROR;
    if (result == DICT_OK) {
        merge_words(words, scratch, 0, count);
        for (i = 0; i < count; ++i) {
            if (!visitor(words[i], context)) { result = DICT_IO_ERROR; break; }
        }
    }
    core_free((void *)scratch);
    core_free((void *)words);
    return result;
}

void dict_destroy(Dict **handle)
{
    Dict *dict;
    if (!handle || !*handle) return;
    dict = *handle;
    if (dict->kind == DICT_LIST) {
        ListNode *head = dict->data.head;
        ListNode *node = head->next;
        while (node != head) {
            ListNode *next = node->next;
            core_free(node);
            node = next;
        }
        core_free(head);
    } else if (dict->kind == DICT_AVL) {
        avl_destroy(dict->data.root);
    } else if (dict->kind == DICT_HASH) {
        size_t i;
        for (i = 0; i < dict->capacity; ++i) {
            HashNode *node = dict->data.buckets[i];
            while (node) {
                HashNode *next = node->next;
                core_free(node);
                node = next;
            }
        }
        core_free(dict->data.buckets);
    }
    core_free(dict);
    *handle = NULL;
}

static bool reason_set(char *reason, size_t size, const char *message)
{
    if (reason && size) snprintf(reason, size, "%s", message);
    return false;
}

static bool validate_avl(const AvlNode *node, const char *lower, const char *upper,
    size_t limit, size_t *seen, int *height, char *reason, size_t reason_size)
{
    int left, right;
    if (!node) { *height = 0; return true; }
    if (++*seen > limit) return reason_set(reason, reason_size, "AVL cycle/count exceeded");
    if (!valid_word(node->word, false) || (lower && strcmp(node->word, lower) <= 0) ||
        (upper && strcmp(node->word, upper) >= 0))
        return reason_set(reason, reason_size, "AVL BST range or word invalid");
    if (!validate_avl(node->left, lower, node->word, limit, seen, &left, reason, reason_size) ||
        !validate_avl(node->right, node->word, upper, limit, seen, &right, reason, reason_size)) return false;
    *height = 1 + (left > right ? left : right);
    if (node->height != *height || left - right < -1 || left - right > 1)
        return reason_set(reason, reason_size, "AVL stored height or balance invalid");
    return true;
}

bool dict_test_validate(const Dict *dict, char *reason, size_t reason_size)
{
    size_t seen = 0;
    size_t i;
    int height;
    if (reason && reason_size) reason[0] = '\0';
    if (!dict) return reason_set(reason, reason_size, "NULL dict");
    if (dict->kind == DICT_LIST) {
        const ListNode *head = dict->data.head;
        const ListNode *node;
        if (!head || !head->next || !head->prev) return reason_set(reason, reason_size, "list sentinel invalid");
        node = head->next;
        while (node != head) {
            if (!node || ++seen > dict->count) return reason_set(reason, reason_size, "list cycle/count exceeded");
            if (!node->prev || !node->next || node->prev->next != node || node->next->prev != node ||
                !valid_word(node->word, false)) return reason_set(reason, reason_size, "list links/word invalid");
            node = node->next;
        }
        if (head->prev->next != head || head->next->prev != head)
            return reason_set(reason, reason_size, "list sentinel reciprocity invalid");
    } else if (dict->kind == DICT_AVL) {
        if (!validate_avl(dict->data.root, NULL, NULL, dict->count, &seen, &height, reason, reason_size)) return false;
    } else if (dict->kind == DICT_HASH) {
        if (!dict->capacity || !dict->data.buckets) return reason_set(reason, reason_size, "hash capacity invalid");
        for (i = 0; i < dict->capacity; ++i) {
            const HashNode *node = dict->data.buckets[i];
            while (node) {
                if (++seen > dict->count) return reason_set(reason, reason_size, "hash cycle/count exceeded");
                if (!valid_word(node->word, false) || dict_fnv1a32(node->word) % dict->capacity != i)
                    return reason_set(reason, reason_size, "hash bucket/word invalid");
                node = node->next;
            }
        }
    } else return reason_set(reason, reason_size, "kind invalid");
    if (seen != dict->count) return reason_set(reason, reason_size, "stored count invalid");
    return true;
}

static bool append_text(char *out, size_t size, size_t *used, const char *text)
{
    size_t length = strlen(text);
    if (*used >= size || length >= size - *used) return false;
    memcpy(out + *used, text, length + 1);
    *used += length;
    return true;
}

bool dict_test_list_order(const Dict *dict, char *out, size_t size)
{
    const ListNode *node;
    size_t used = 0;
    size_t seen = 0;
    if (!dict || dict->kind != DICT_LIST || !out || !size) return false;
    out[0] = '\0';
    for (node = dict->data.head->next; node != dict->data.head; node = node->next) {
        if (++seen > dict->count || (seen > 1 && !append_text(out, size, &used, "->")) ||
            !append_text(out, size, &used, node->word)) return false;
    }
    return seen == dict->count;
}

bool dict_test_hash_chain(const Dict *dict, size_t bucket, char *out, size_t size)
{
    const HashNode *node;
    size_t used = 0;
    size_t seen = 0;
    if (!dict || dict->kind != DICT_HASH || bucket >= dict->capacity || !out || !size) return false;
    out[0] = '\0';
    for (node = dict->data.buckets[bucket]; node; node = node->next) {
        if (++seen > dict->count || (seen > 1 && !append_text(out, size, &used, "->")) ||
            !append_text(out, size, &used, node->word)) return false;
    }
    return true;
}

static bool shape_node(const AvlNode *node, char *out, size_t size, size_t *used, size_t *seen, size_t limit)
{
    if (!node) return append_text(out, size, used, "-");
    if (++*seen > limit || !append_text(out, size, used, node->word)) return false;
    if (node->left || node->right) {
        if (!append_text(out, size, used, "(") ||
            !shape_node(node->left, out, size, used, seen, limit) ||
            !append_text(out, size, used, ",") ||
            !shape_node(node->right, out, size, used, seen, limit) ||
            !append_text(out, size, used, ")")) return false;
    }
    return true;
}

bool dict_test_avl_shape(const Dict *dict, char *out, size_t size)
{
    size_t used = 0;
    size_t seen = 0;
    if (!dict || dict->kind != DICT_AVL || !out || !size) return false;
    out[0] = '\0';
    return shape_node(dict->data.root, out, size, &used, &seen, dict->count) && seen == dict->count;
}

#ifdef DICT_TEST_HOOKS
size_t dict_test_avl_event_count(const Dict *dict)
{
    return dict && dict->kind == DICT_AVL ? dict->event_count : 0;
}

bool dict_test_avl_event(const Dict *dict, size_t index, DictAvlEvent *out)
{
    if (!out || !dict || dict->kind != DICT_AVL || index >= dict->event_count) return false;
    *out = dict->events[index];
    return true;
}

void dict_test_clear_avl_events(Dict *dict)
{
    if (dict && dict->kind == DICT_AVL) dict->event_count = 0;
}

bool dict_test_fault_detected(Dict *dict, int fault_kind)
{
    char reason[96];
    bool detected;
    if (!dict) return false;
    if (fault_kind == 1 && dict->kind == DICT_AVL && dict->data.root) {
        ++dict->data.root->height;
        detected = !dict_test_validate(dict, reason, sizeof reason);
        --dict->data.root->height;
        return detected && dict_test_validate(dict, reason, sizeof reason);
    }
    if (fault_kind == 2 && dict->kind == DICT_LIST && dict->data.head->next != dict->data.head) {
        ListNode *first = dict->data.head->next;
        ListNode *saved = first->prev;
        first->prev = NULL;
        detected = !dict_test_validate(dict, reason, sizeof reason);
        first->prev = saved;
        return detected && dict_test_validate(dict, reason, sizeof reason);
    }
    return false;
}
#endif
