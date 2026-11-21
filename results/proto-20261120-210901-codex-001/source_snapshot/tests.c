#include "tests.h"
#include "dict.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    FILE *log;
    TestSummary *summary;
    const char *case_id;
    const char *kind;
    bool diagnostic;
} TestContext;

typedef struct {
    char text[4096];
    size_t used;
    size_t calls;
    size_t stop_after;
} Capture;

static void check(TestContext *test, const char *step, const char *expected, const char *actual, bool okay)
{
    ++test->summary->checks;
    if (test->diagnostic) ++test->summary->diagnostic_checks;
    if (!okay) ++test->summary->failed;
    if (fprintf(test->log, "%s\t%s\t%s\t%s\t%s\t%s\n", test->case_id, test->kind,
        step, expected, actual, okay ? "PASS" : "FAIL") < 0) ++test->summary->failed;
    if (!okay) fprintf(stderr, "FAIL %s %s %s expected=%s actual=%s\n",
        test->case_id, test->kind, step, expected, actual);
}

static void check_status(TestContext *test, const char *step, DictStatus expected, DictStatus actual)
{
    check(test, step, dict_status_name(expected), dict_status_name(actual), expected == actual);
}

static void check_size(TestContext *test, Dict *dict, size_t expected)
{
    char wanted[32], actual[32];
    size_t count = SIZE_MAX;
    DictStatus status = dict_size(dict, &count);
    snprintf(wanted, sizeof wanted, "%zu", expected);
    snprintf(actual, sizeof actual, "%zu", count);
    check_status(test, "size_status", DICT_OK, status);
    check(test, "size", wanted, actual, status == DICT_OK && count == expected);
}

static bool capture_word(const char *word, void *context)
{
    Capture *capture = context;
    size_t length = strlen(word);
    ++capture->calls;
    if (capture->stop_after && capture->calls == capture->stop_after) return false;
    if (capture->used + length + (capture->used ? 1 : 0) >= sizeof capture->text) return false;
    if (capture->used) capture->text[capture->used++] = ',';
    memcpy(capture->text + capture->used, word, length + 1);
    capture->used += length;
    return true;
}

static void check_state(TestContext *test, Dict *dict, size_t expected_count, const char *expected_words)
{
    Capture capture = {0};
    char reason[128] = {0};
    bool structure_ok;
    check_size(test, dict, expected_count);
    check_status(test, "visit_status", DICT_OK, dict_visit_sorted(dict, capture_word, &capture));
    check(test, "sorted_words", expected_words, capture.text, !strcmp(capture.text, expected_words));
    structure_ok = dict_test_validate(dict, reason, sizeof reason);
    check(test, "structure", "valid", structure_ok ? "valid" : reason, structure_ok);
}

static Dict *new_dict(TestContext *test, DictKind kind, size_t initial_n)
{
    Dict *dict = NULL;
    DictConfig config = {initial_n};
    DictStatus status = dict_create(kind, &config, &dict);
    check_status(test, "create", DICT_OK, status);
    check(test, "create_handle", "non-NULL", dict ? "non-NULL" : "NULL", dict != NULL);
    return dict;
}

static void finish_dict(TestContext *test, Dict **dict)
{
    dict_destroy(dict);
    check(test, "destroy_handle", "NULL", *dict ? "non-NULL" : "NULL", *dict == NULL);
    dict_destroy(dict);
    check(test, "destroy_twice", "NULL", *dict ? "non-NULL" : "NULL", *dict == NULL);
#ifdef DICT_TEST_HOOKS
    {
        DictAllocStats stats = dict_test_alloc_stats();
        char actual[96];
        snprintf(actual, sizeof actual, "count=%zu bytes=%zu owner=%016llx", stats.live_count,
            stats.live_bytes, (unsigned long long)stats.ownership_fingerprint);
        check(test, "core_live_after_destroy", "count=0 bytes=0 owner=0000000000000000", actual,
            stats.live_count == 0 && stats.live_bytes == 0 && stats.ownership_fingerprint == 0);
    }
#endif
}

static void test_crud(TestContext *test, DictKind kind)
{
    char buffer[17] = "apple";
    Dict *dict = new_dict(test, kind, 16);
    if (!dict) return;
    check_state(test, dict, 0, "");
    check_status(test, "empty_search", DICT_NOT_FOUND, dict_search(dict, "apple"));
    check_status(test, "empty_remove", DICT_NOT_FOUND, dict_remove(dict, "apple"));
    check_status(test, "insert_apple", DICT_OK, dict_insert(dict, buffer));
    strcpy_s(buffer, sizeof buffer, "grape");
    check_status(test, "buffer_reused_apple", DICT_FOUND, dict_search(dict, "apple"));
    check_status(test, "insert_grape", DICT_OK, dict_insert(dict, buffer));
    check_status(test, "insert_pear", DICT_OK, dict_insert(dict, "pear"));
    check_status(test, "duplicate_apple", DICT_ALREADY_EXISTS, dict_insert(dict, "apple"));
    check_state(test, dict, 3, "apple,grape,pear");
    check_status(test, "remove_grape", DICT_OK, dict_remove(dict, "grape"));
    check_status(test, "search_removed", DICT_NOT_FOUND, dict_search(dict, "grape"));
    check_status(test, "remove_again", DICT_NOT_FOUND, dict_remove(dict, "grape"));
    check_state(test, dict, 2, "apple,pear");
    check_status(test, "reinsert_grape", DICT_OK, dict_insert(dict, "grape"));
    check_state(test, dict, 3, "apple,grape,pear");
    finish_dict(test, &dict);
}

static void test_inputs(TestContext *test, DictKind kind)
{
    static const char *valid[] = {"abc", "abcdefghijklmno"};
    static const char *invalid[] = {"ab", "abcdefghijklmnop", "", "Apple", "apple#", "#apple", "apple##"};
    Dict *dict = new_dict(test, kind, 2);
    size_t i;
    Capture capture = {0};
    if (!dict) return;
    for (i = 0; i < sizeof valid / sizeof valid[0]; ++i)
        check_status(test, valid[i], DICT_OK, dict_insert(dict, valid[i]));
    for (i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        check_status(test, invalid[i][0] ? invalid[i] : "empty_insert", DICT_INVALID_INPUT,
            dict_insert(dict, invalid[i]));
    check_status(test, "null_insert", DICT_INVALID_INPUT, dict_insert(dict, NULL));
    check_status(test, "apple_hash_search", DICT_NOT_FOUND, dict_search(dict, "apple#"));
    check_status(test, "apple_hash_remove", DICT_NOT_FOUND, dict_remove(dict, "apple#"));
    check_status(test, "long_hash_search", DICT_NOT_FOUND, dict_search(dict, "abcdefghijklmno#"));
    check_status(test, "long_hash_remove", DICT_NOT_FOUND, dict_remove(dict, "abcdefghijklmno#"));
    check_status(test, "double_hash_search", DICT_INVALID_INPUT, dict_search(dict, "apple##"));
    check_status(test, "prefix_hash_remove", DICT_INVALID_INPUT, dict_remove(dict, "#apple"));
    check_status(test, "normal_search", DICT_FOUND, dict_search(dict, "abc"));
    check_state(test, dict, 2, "abc,abcdefghijklmno");
    capture.stop_after = 2;
    check_status(test, "visitor_false", DICT_IO_ERROR, dict_visit_sorted(dict, capture_word, &capture));
    check(test, "visitor_calls", "2", capture.calls == 2 ? "2" : "other", capture.calls == 2);
    check_state(test, dict, 2, "abc,abcdefghijklmno");
    finish_dict(test, &dict);
}

static void test_api_invalid(TestContext *test)
{
    DictConfig config = {2};
    Dict *dict = NULL;
    size_t count = 0;
    check_status(test, "create_null_out", DICT_INVALID_INPUT, dict_create(DICT_LIST, &config, NULL));
    check_status(test, "create_null_config", DICT_INVALID_INPUT, dict_create(DICT_LIST, NULL, &dict));
    config.initial_n = 0;
    check_status(test, "create_zero_n", DICT_INVALID_INPUT, dict_create(DICT_HASH, &config, &dict));
    config.initial_n = SIZE_MAX;
    check_status(test, "create_overflow_n", DICT_INVALID_INPUT, dict_create(DICT_HASH, &config, &dict));
    check_status(test, "size_null_dict", DICT_INVALID_INPUT, dict_size(NULL, &count));
    check_status(test, "size_null_out", DICT_INVALID_INPUT, dict_size(NULL, NULL));
    check_status(test, "search_null_dict", DICT_INVALID_INPUT, dict_search(NULL, "apple"));
    check_status(test, "remove_null_dict", DICT_INVALID_INPUT, dict_remove(NULL, "apple"));
    dict_destroy(&dict);
    check(test, "null_destroy", "NULL", dict ? "non-NULL" : "NULL", dict == NULL);
}

static void test_list_links(TestContext *test)
{
    static const char *removals[] = {"grape", "pear", "apple"};
    static const char *orders[] = {"pear->apple", "grape->apple", "pear->grape"};
    size_t i;
    for (i = 0; i < 3; ++i) {
        Dict *dict = new_dict(test, DICT_LIST, 2);
        char order[128] = {0};
        bool order_ok;
        if (!dict) continue;
        dict_insert(dict, "apple"); dict_insert(dict, "grape"); dict_insert(dict, "pear");
        order_ok = dict_test_list_order(dict, order, sizeof order);
        check(test, "initial_links", "pear->grape->apple",
            order_ok ? order : "INVALID", order_ok && !strcmp(order, "pear->grape->apple"));
        check_status(test, removals[i], DICT_OK, dict_remove(dict, removals[i]));
        order_ok = dict_test_list_order(dict, order, sizeof order);
        check(test, "after_delete_links", orders[i],
            order_ok ? order : "INVALID", order_ok && !strcmp(order, orders[i]));
        check_state(test, dict, 2, i == 0 ? "apple,pear" : i == 1 ? "apple,grape" : "grape,pear");
        finish_dict(test, &dict);
    }
    {
        Dict *dict = new_dict(test, DICT_LIST, 2);
        if (!dict) return;
        check_status(test, "single_insert", DICT_OK, dict_insert(dict, "apple"));
        check_status(test, "single_remove", DICT_OK, dict_remove(dict, "apple"));
        check_state(test, dict, 0, "");
        finish_dict(test, &dict);
    }
}

static void test_avl_inserts(TestContext *test)
{
    static const char *orders[][3] = {
        {"mango","grape","apple"}, {"apple","grape","mango"},
        {"mango","apple","grape"}, {"apple","mango","grape"}
    };
    static const char *names[] = {"LL", "RR", "LR", "RL"};
    size_t i, j;
    for (i = 0; i < 4; ++i) {
        Dict *dict = new_dict(test, DICT_AVL, 2);
        char shape[128] = {0};
        bool shape_ok;
        if (!dict) continue;
        for (j = 0; j < 3; ++j) check_status(test, names[i], DICT_OK, dict_insert(dict, orders[i][j]));
        check_state(test, dict, 3, "apple,grape,mango");
        shape_ok = dict_test_avl_shape(dict, shape, sizeof shape);
        check(test, "insert_shape", "grape(apple,mango)",
            shape_ok ? shape : "INVALID", shape_ok && !strcmp(shape, "grape(apple,mango)"));
        finish_dict(test, &dict);
    }
}

typedef struct {
    const char *id;
    const char *insert[12];
    size_t length;
    const char *remove;
    const char *before;
    const char *after;
    const char *sorted;
    size_t before_height;
    size_t after_height;
    const char *rotations;
} AvlFixture;

static const AvlFixture avl_deletions[] = {
    {"prototype:avl-del-v1:D01", {"ddd","bbb","eee","aaa","ccc"},5,"eee",NULL,"bbb(aaa,ddd(ccc,-))","aaa,bbb,ccc,ddd",3,3,"R:0"},
    {"prototype:avl-del-v1:D02", {"bbb","aaa","ddd","ccc","eee"},5,"aaa",NULL,"ddd(bbb(-,ccc),eee)","bbb,ccc,ddd,eee",3,3,"L:0"},
    {"prototype:avl-del-v1:D03", {"ddd","bbb","eee","aaa"},4,"eee",NULL,"bbb(aaa,ddd)","aaa,bbb,ddd",3,2,"R:1"},
    {"prototype:avl-del-v1:D04", {"bbb","aaa","ddd","eee"},4,"aaa",NULL,"ddd(bbb,eee)","bbb,ddd,eee",3,2,"L:-1"},
    {"prototype:avl-del-v1:D05", {"eee","bbb","fff","ddd"},4,"fff",NULL,"ddd(bbb,eee)","bbb,ddd,eee",3,2,"LR:-1"},
    {"prototype:avl-del-v1:D06", {"bbb","aaa","eee","ccc"},4,"aaa",NULL,"ccc(bbb,eee)","bbb,ccc,eee",3,2,"RL:1"},
    {"prototype:avl-del-v1:D07", {"ddd","bbb","fff","aaa"},4,"aaa",NULL,"ddd(bbb,fff)","bbb,ddd,fff",3,2,"none"},
    {"prototype:avl-del-v1:D08", {"eee","hhh","bbb","ccc","fff","jjj","ggg","lll","kkk","iii","aaa","ddd"},12,"kkk",
        "hhh(eee(bbb(aaa,ccc(-,ddd)),fff(-,ggg)),kkk(jjj(iii,-),lll))",
        "eee(bbb(aaa,ccc(-,ddd)),hhh(fff(-,ggg),jjj(iii,lll)))",
        "aaa,bbb,ccc,ddd,eee,fff,ggg,hhh,iii,jjj,lll",5,4,"R:1,R:1"},
    {"prototype:avl-del-v1:D09", {"bbb","aaa","ccc","ddd"},4,"bbb",NULL,"ccc(aaa,ddd)","aaa,ccc,ddd",3,2,"none"}
};

static int shape_height(const char *shape)
{
    int depth = 0, maximum = 0;
    const char *p = shape;
    while (*p) {
        if (*p == '(') { ++depth; if (depth > maximum) maximum = depth; }
        if (*p == ')') --depth;
        ++p;
    }
    return maximum + 1;
}

static void test_avl_delete_fixtures(TestContext *test)
{
    size_t i, j;
    for (i = 0; i < sizeof avl_deletions / sizeof avl_deletions[0]; ++i) {
        const AvlFixture *fixture = &avl_deletions[i];
        Dict *dict;
        char shape[1024] = {0};
        char actual_height[24], expected_height[24];
        test->case_id = fixture->id;
        dict = new_dict(test, DICT_AVL, 16);
        if (!dict) continue;
        for (j = 0; j < fixture->length; ++j)
            check_status(test, "fixture_insert", DICT_OK, dict_insert(dict, fixture->insert[j]));
        check(test, "before_structure", "valid", "checked", dict_test_validate(dict, NULL, 0));
        if (fixture->before) {
            dict_test_avl_shape(dict, shape, sizeof shape);
            check(test, "before_shape", fixture->before, shape, !strcmp(shape, fixture->before));
        }
        dict_test_avl_shape(dict, shape, sizeof shape);
        snprintf(expected_height, sizeof expected_height, "%zu", fixture->before_height);
        snprintf(actual_height, sizeof actual_height, "%d", shape_height(shape));
        check(test, "before_height", expected_height, actual_height,
            fixture->before_height == (size_t)shape_height(shape));
#ifdef DICT_TEST_HOOKS
        dict_test_clear_avl_events(dict);
#endif
        check_status(test, "remove", DICT_OK, dict_remove(dict, fixture->remove));
        check_state(test, dict, fixture->length - 1, fixture->sorted);
        dict_test_avl_shape(dict, shape, sizeof shape);
        check(test, "after_shape", fixture->after, shape, !strcmp(shape, fixture->after));
        snprintf(expected_height, sizeof expected_height, "%zu", fixture->after_height);
        snprintf(actual_height, sizeof actual_height, "%d", shape_height(shape));
        check(test, "after_height", expected_height, actual_height,
            fixture->after_height == (size_t)shape_height(shape));
#ifdef DICT_TEST_HOOKS
        {
            char events[128] = {0};
            size_t used = 0;
            for (j = 0; j < dict_test_avl_event_count(dict); ++j) {
                DictAvlEvent event;
                if (dict_test_avl_event(dict, j, &event)) {
                    int written = snprintf(events + used, sizeof events - used, "%s%s:%d",
                        used ? "," : "", event.rotation, event.child_balance_before);
                    if (written > 0 && (size_t)written < sizeof events - used) used += (size_t)written;
                }
            }
            if (!used) strcpy_s(events, sizeof events, "none");
            check(test, "delete_rotation_events", fixture->rotations, events,
                !strcmp(events, fixture->rotations));
        }
#endif
        finish_dict(test, &dict);
    }
    test->case_id = "T05:AVL:OTHER_DELETE";
    {
        const char *words[] = {"mango","grape","tango","apple","kiwi","peach","zebra","plum"};
        Dict *dict = new_dict(test, DICT_AVL, 16);
        char shape[256] = {0};
        if (dict) {
            for (i = 0; i < 8; ++i) dict_insert(dict, words[i]);
            check_status(test, "deep_successor_remove", DICT_OK, dict_remove(dict, "mango"));
            check_state(test, dict, 7, "apple,grape,kiwi,peach,plum,tango,zebra");
            dict_test_avl_shape(dict, shape, sizeof shape);
            check(test, "deep_successor_shape", "peach(grape(apple,kiwi),tango(plum,zebra))", shape,
                !strcmp(shape, "peach(grape(apple,kiwi),tango(plum,zebra))"));
            finish_dict(test, &dict);
        }
    }
    {
        Dict *dict = new_dict(test, DICT_AVL, 2);
        if (dict) {
            dict_insert(dict, "apple");
            check_status(test, "single_root_remove", DICT_OK, dict_remove(dict, "apple"));
            check_state(test, dict, 0, "");
            finish_dict(test, &dict);
        }
    }
}

static void test_hash(TestContext *test)
{
    static const struct { const char *word; uint32_t hash; } vectors[] = {
        {"", UINT32_C(2166136261)}, {"a", UINT32_C(3826002220)},
        {"grape", UINT32_C(2893541840)}, {"banana", UINT32_C(3649609552)},
        {"lemon", UINT32_C(3368108496)}, {"melon", UINT32_C(1927437660)},
        {"pear", UINT32_C(3491742317)}, {"costarring", UINT32_C(1582148253)},
        {"liquid", UINT32_C(1582148253)}, {"skvxieetw", UINT32_C(2172590711)},
        {"hxftijesa", UINT32_C(2172590711)}
    };
    const char *words[] = {"grape","banana","lemon","melon","pear"};
    size_t i;
    Dict *dict;
    char chain[160] = {0};
    for (i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        char expected[32], actual[32];
        uint32_t got = dict_fnv1a32(vectors[i].word);
        snprintf(expected, sizeof expected, "%u", vectors[i].hash);
        snprintf(actual, sizeof actual, "%u", got);
        check(test, vectors[i].word[0] ? vectors[i].word : "empty_fnv", expected, actual, got == vectors[i].hash);
    }
    dict = new_dict(test, DICT_HASH, 2);
    if (!dict) return;
    check(test, "capacity", "4", dict_capacity(dict) == 4 ? "4" : "other", dict_capacity(dict) == 4);
    for (i = 0; i < 5; ++i) check_status(test, "collision_insert", DICT_OK, dict_insert(dict, words[i]));
    check_state(test, dict, 5, "banana,grape,lemon,melon,pear");
    {
        bool chain_ok = dict_test_hash_chain(dict, 0, chain, sizeof chain);
    check(test, "chain_initial", "melon->lemon->banana->grape",
        chain_ok ? chain : "INVALID", chain_ok && !strcmp(chain, "melon->lemon->banana->grape"));
    }
    check_status(test, "duplicate", DICT_ALREADY_EXISTS, dict_insert(dict, "banana"));
    check_status(test, "lemon_remove", DICT_OK, dict_remove(dict, "lemon"));
    dict_test_hash_chain(dict, 0, chain, sizeof chain);
    check(test, "chain_middle_remove", "melon->banana->grape", chain,
        !strcmp(chain, "melon->banana->grape"));
    check_status(test, "melon_remove", DICT_OK, dict_remove(dict, "melon"));
    dict_test_hash_chain(dict, 0, chain, sizeof chain);
    check(test, "chain_head_remove", "banana->grape", chain, !strcmp(chain, "banana->grape"));
    check_status(test, "grape_remove", DICT_OK, dict_remove(dict, "grape"));
    dict_test_hash_chain(dict, 0, chain, sizeof chain);
    check(test, "chain_tail_remove", "banana", chain, !strcmp(chain, "banana"));
    check_status(test, "kiwi_same_bucket", DICT_NOT_FOUND, dict_search(dict, "kiwi"));
    check(test, "capacity_fixed", "4", dict_capacity(dict) == 4 ? "4" : "other", dict_capacity(dict) == 4);
    finish_dict(test, &dict);
    test->case_id = "prototype:hash-v1:FULL";
    dict = new_dict(test, DICT_HASH, 2);
    if (dict) {
        check_status(test, "costarring_insert", DICT_OK, dict_insert(dict, "costarring"));
        check_status(test, "liquid_insert", DICT_OK, dict_insert(dict, "liquid"));
        check_state(test, dict, 2, "costarring,liquid");
        check_status(test, "costarring_duplicate", DICT_ALREADY_EXISTS, dict_insert(dict, "costarring"));
        check_status(test, "costarring_remove", DICT_OK, dict_remove(dict, "costarring"));
        check_status(test, "liquid_survives", DICT_FOUND, dict_search(dict, "liquid"));
        check_status(test, "costarring_reinsert", DICT_OK, dict_insert(dict, "costarring"));
        check_status(test, "liquid_remove", DICT_OK, dict_remove(dict, "liquid"));
        check_state(test, dict, 1, "costarring");
        finish_dict(test, &dict);
    }
    test->case_id = "extension:0.2:T06-FULL";
    dict = new_dict(test, DICT_HASH, 2);
    if (dict) {
        check_status(test, "same_length_first", DICT_OK, dict_insert(dict, "skvxieetw"));
        check_status(test, "same_length_second", DICT_OK, dict_insert(dict, "hxftijesa"));
        check_state(test, dict, 2, "hxftijesa,skvxieetw");
        check_status(test, "same_length_remove", DICT_OK, dict_remove(dict, "skvxieetw"));
        check_status(test, "other_survives", DICT_FOUND, dict_search(dict, "hxftijesa"));
        finish_dict(test, &dict);
    }
}

typedef struct { char words[64][16]; size_t count; } Model;

static DictStatus model_search(const Model *model, const char *word)
{
    size_t i;
    for (i = 0; i < model->count; ++i) if (!strcmp(model->words[i], word)) return DICT_FOUND;
    return DICT_NOT_FOUND;
}

static DictStatus model_insert(Model *model, const char *word)
{
    size_t i;
    if (model_search(model, word) == DICT_FOUND) return DICT_ALREADY_EXISTS;
    if (model->count == 64) return DICT_OUT_OF_MEMORY;
    for (i = model->count; i > 0 && strcmp(model->words[i - 1], word) > 0; --i)
        memcpy(model->words[i], model->words[i - 1], sizeof model->words[i]);
    strcpy_s(model->words[i], sizeof model->words[i], word);
    ++model->count;
    return DICT_OK;
}

static DictStatus model_remove(Model *model, const char *word)
{
    size_t i;
    for (i = 0; i < model->count; ++i) {
        if (!strcmp(model->words[i], word)) {
            for (; i + 1 < model->count; ++i)
                memcpy(model->words[i], model->words[i + 1], sizeof model->words[i]);
            --model->count;
            return DICT_OK;
        }
    }
    return DICT_NOT_FOUND;
}

static void model_text(const Model *model, char *out, size_t capacity)
{
    size_t i, used = 0;
    out[0] = '\0';
    for (i = 0; i < model->count; ++i) {
        int written = snprintf(out + used, capacity - used, "%s%s", i ? "," : "", model->words[i]);
        if (written < 0 || (size_t)written >= capacity - used) return;
        used += (size_t)written;
    }
}

static void test_model(TestContext *test, DictKind kind)
{
    static const char *words[] = {"apple","grape","pear","mango","tango","kiwi","peach","zebra",
        "plum","banana","melon","lemon","alpha","bravo","charlie","delta","echo","foxtrot","gamma","hotel"};
    uint32_t state = UINT32_C(0x12345678);
    Model model = {0};
    Dict *dict = new_dict(test, kind, 16);
    size_t i;
    if (!dict) return;
    check_status(test, "model_manual_insert_apple", DICT_OK, model_insert(&model, "apple"));
    check_status(test, "model_manual_insert_grape", DICT_OK, model_insert(&model, "grape"));
    check(test, "model_manual_sorted", "apple,grape",
        !strcmp(model.words[0], "apple") && !strcmp(model.words[1], "grape") ? "apple,grape" : "other",
        model.count == 2 && !strcmp(model.words[0], "apple") && !strcmp(model.words[1], "grape"));
    model.count = 0;
    for (i = 0; i < 300; ++i) {
        const char *word;
        DictStatus expected, actual;
        char step[40], sorted[1024];
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        word = words[state % (sizeof words / sizeof words[0])];
        snprintf(step, sizeof step, "operation_%03zu", i);
        if (i % 5 < 2) { expected = model_insert(&model, word); actual = dict_insert(dict, word); }
        else if (i % 5 == 2) { expected = model_remove(&model, word); actual = dict_remove(dict, word); }
        else { expected = model_search(&model, word); actual = dict_search(dict, word); }
        check_status(test, step, expected, actual);
        model_text(&model, sorted, sizeof sorted);
        check_state(test, dict, model.count, sorted);
    }
    finish_dict(test, &dict);
}

#ifdef DICT_TEST_HOOKS
static bool same_owned(DictAllocStats a, DictAllocStats b)
{
    return a.live_count == b.live_count && a.live_bytes == b.live_bytes &&
        a.ownership_fingerprint == b.ownership_fingerprint;
}

static void check_owned(TestContext *test, const char *step, DictAllocStats before, DictAllocStats after)
{
    char expected[96], actual[96];
    snprintf(expected, sizeof expected, "count=%zu bytes=%zu owner=%016llx", before.live_count,
        before.live_bytes, (unsigned long long)before.ownership_fingerprint);
    snprintf(actual, sizeof actual, "count=%zu bytes=%zu owner=%016llx", after.live_count,
        after.live_bytes, (unsigned long long)after.ownership_fingerprint);
    check(test, step, expected, actual, same_owned(before, after));
}

static void test_allocation_failures(TestContext *test, DictKind kind)
{
    size_t point;
    size_t create_points = kind == DICT_AVL ? 1 : 2;
    for (point = 1; point <= create_points; ++point) {
        DictConfig config = {2};
        Dict *dict = NULL;
        DictAllocStats after;
        dict_test_fail_allocation(point);
        check_status(test, "create_injected_failure", DICT_OUT_OF_MEMORY, dict_create(kind, &config, &dict));
        after = dict_test_alloc_stats();
        check(test, "create_out_null", "NULL", dict ? "non-NULL" : "NULL", dict == NULL);
        {
            char actual[96];
            snprintf(actual, sizeof actual, "count=%zu bytes=%zu owner=%016llx", after.live_count,
                after.live_bytes, (unsigned long long)after.ownership_fingerprint);
            check(test, "create_live_zero", "count=0 bytes=0 owner=0000000000000000", actual,
                after.live_count == 0 && after.live_bytes == 0 && after.ownership_fingerprint == 0);
        }
        check(test, "create_attempt", "reached", after.allocation_attempts >= point ? "reached" : "missed",
            after.allocation_attempts >= point);
        dict_test_clear_allocation_failure();
    }
    {
        Dict *dict = new_dict(test, kind, 2);
        DictAllocStats before, after;
        if (!dict) return;
        dict_insert(dict, "apple"); dict_insert(dict, "grape");
        before = dict_test_alloc_stats();
        dict_test_fail_allocation(1);
        check_status(test, "insert_node_oom", DICT_OUT_OF_MEMORY, dict_insert(dict, "pear"));
        after = dict_test_alloc_stats();
        check_owned(test, "insert_existing_ownership", before, after);
        dict_test_clear_allocation_failure();
        check_state(test, dict, 2, "apple,grape");
        finish_dict(test, &dict);
    }
    if (kind == DICT_AVL) return;
    for (point = 1; point <= 2; ++point) {
        Dict *dict = new_dict(test, kind, 2);
        DictAllocStats before, after;
        Capture capture = {0};
        if (!dict) continue;
        dict_insert(dict, "apple"); dict_insert(dict, "grape");
        before = dict_test_alloc_stats();
        dict_test_fail_allocation(point);
        check_status(test, "sorted_array_oom", DICT_OUT_OF_MEMORY,
            dict_visit_sorted(dict, capture_word, &capture));
        after = dict_test_alloc_stats();
        check(test, "visitor_not_called", "0", capture.calls ? "nonzero" : "0", capture.calls == 0);
        check_owned(test, "temporary_freed_existing_preserved", before, after);
        dict_test_clear_allocation_failure();
        check_state(test, dict, 2, "apple,grape");
        finish_dict(test, &dict);
    }
}

static void test_checker_faults(TestContext *test)
{
    Dict *list = new_dict(test, DICT_LIST, 2);
    Dict *avl;
    bool detected;
    if (list) {
        dict_insert(list, "apple");
        detected = dict_test_fault_detected(list, 2);
        check(test, "broken_prev_detected", "EXPECTED_FAILURE_DETECTED",
            detected ? "EXPECTED_FAILURE_DETECTED" : "MISSED", detected);
        finish_dict(test, &list);
    }
    avl = new_dict(test, DICT_AVL, 2);
    if (avl) {
        dict_insert(avl, "apple");
        detected = dict_test_fault_detected(avl, 1);
        check(test, "wrong_height_detected", "EXPECTED_FAILURE_DETECTED",
            detected ? "EXPECTED_FAILURE_DETECTED" : "MISSED", detected);
        finish_dict(test, &avl);
    }
    check(test, "wrong_expected_search_detected", "EXPECTED_FAILURE_DETECTED",
        DICT_NOT_FOUND != DICT_FOUND ? "EXPECTED_FAILURE_DETECTED" : "MISSED",
        DICT_NOT_FOUND != DICT_FOUND);
    check(test, "hash_only_equality_fault_detected", "EXPECTED_FAILURE_DETECTED",
        dict_fnv1a32("costarring") == dict_fnv1a32("liquid") &&
        strcmp("costarring", "liquid") ? "EXPECTED_FAILURE_DETECTED" : "MISSED",
        dict_fnv1a32("costarring") == dict_fnv1a32("liquid") &&
        strcmp("costarring", "liquid"));
    check(test, "hash_and_length_fault_detected", "EXPECTED_FAILURE_DETECTED",
        dict_fnv1a32("skvxieetw") == dict_fnv1a32("hxftijesa") &&
        strlen("skvxieetw") == strlen("hxftijesa") && strcmp("skvxieetw", "hxftijesa") ?
            "EXPECTED_FAILURE_DETECTED" : "MISSED",
        dict_fnv1a32("skvxieetw") == dict_fnv1a32("hxftijesa") &&
        strlen("skvxieetw") == strlen("hxftijesa") && strcmp("skvxieetw", "hxftijesa"));
}
#endif

int tests_run(FILE *accuracy_log, bool diagnostics, TestSummary *summary)
{
    TestContext test;
    DictKind kind;
    if (!accuracy_log || !summary) return 8;
    memset(summary, 0, sizeof *summary);
    test.log = accuracy_log;
    test.summary = summary;
    test.diagnostic = false;
    if (fprintf(accuracy_log, "case_id\tkind\tstep\texpected\tactual\tresult\n") < 0) return 6;
    for (kind = DICT_LIST; kind <= DICT_HASH; kind = (DictKind)(kind + 1)) {
        test.kind = dict_kind_name(kind);
        test.case_id = "T02:CRUD"; ++summary->cases; test_crud(&test, kind);
        test.case_id = "T02:INPUT"; ++summary->cases; test_inputs(&test, kind);
        test.case_id = "T02:MODEL"; ++summary->cases; test_model(&test, kind);
    }
    test.kind = "ALL"; test.case_id = "T07:API"; ++summary->cases; test_api_invalid(&test);
    test.kind = "LIST"; test.case_id = "T04:LIST_LINKS"; ++summary->cases; test_list_links(&test);
    test.kind = "AVL"; test.case_id = "T05:INSERT_ROTATIONS"; ++summary->cases; test_avl_inserts(&test);
    test.case_id = "T05:DELETE"; ++summary->cases; test_avl_delete_fixtures(&test);
    test.kind = "HASH"; test.case_id = "T06:HASH"; ++summary->cases; test_hash(&test);
#ifdef DICT_TEST_HOOKS
    if (diagnostics) {
        test.diagnostic = true;
        for (kind = DICT_LIST; kind <= DICT_HASH; kind = (DictKind)(kind + 1)) {
            test.kind = dict_kind_name(kind);
            test.case_id = "T08:ALLOCATION"; ++summary->cases; test_allocation_failures(&test, kind);
        }
        test.kind = "ALL"; test.case_id = "CHECK-FAULT:prototype-v1"; ++summary->cases;
        test_checker_faults(&test);
    }
#else
    (void)diagnostics;
#endif
    if (fflush(accuracy_log) != 0) return 6;
    return summary->failed ? 3 : 0;
}
