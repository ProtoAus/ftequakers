#!/usr/bin/env python3
"""Compile the actual shader-rebucketing functions in a small C regression harness."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def infer_engine():
    for parent in Path(__file__).resolve().parents:
        for candidate in (parent, parent / 'engine-alpha'):
            if (candidate / 'engine/gl/gl_model.c').is_file():
                return candidate
    raise SystemExit('Cannot infer project root; supply --engine')


STUBS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
%s
%s
%s
typedef struct { int sort; } shader_t;
typedef struct batch_s {
    struct batch_s *next;
    shader_t *shader;
    union {
        struct { unsigned int ebobatch, shadowbatch, sortsequence; } bmodel;
        unsigned int alias[3];
    } user;
    int id;
} batch_t;
typedef struct {
    int loadstate;
    modtype_t type;
    batch_t *batches[SHADER_SORT_COUNT];
    int numbatches;
} model_t;
static model_t mod_known[4];
static int mod_numknown, cache_calls;
static void Surf_ClearSceneCache(void) { cache_calls++; }
'''

HARNESS = r'''
#define CHECK(test, message) do { if (!(test)) { \
    fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); exit(1); } } while (0)
static void reset(int count)
{
    memset(mod_known, 0, sizeof(mod_known));
    mod_numknown = count;
    cache_calls = 0;
    for (int i = 0; i < count; i++) {
        mod_known[i].type = mod_brush;
        mod_known[i].loadstate = MLS_LOADED;
        mod_known[i].numbatches = 73 + i;
    }
}
static void node(batch_t *b, int id, unsigned int sequence, shader_t *shader)
{
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->shader = shader;
    b->user.bmodel.sortsequence = sequence;
    b->user.bmodel.ebobatch = 100 + (unsigned int)id;
    b->user.bmodel.shadowbatch = 200 + (unsigned int)id;
}
static void append(model_t *m, int band, batch_t *b)
{
    batch_t **tail = &m->batches[band];
    while (*tail) tail = &(*tail)->next;
    *tail = b;
}
static void order(model_t *m, int band, const int *ids, size_t count, const char *message)
{
    batch_t *b = m->batches[band];
    for (size_t i = 0; i < count; i++) {
        CHECK(b && b->id == ids[i], message);
        b = b->next;
    }
    CHECK(!b, "extra node or cycle");
}
static void resort(void)
{
    int before = cache_calls;
    Mod_ResortShaders();
    CHECK(cache_calls == before + 1, "exact scene-cache invalidation");
    for (int i = 0; i < mod_numknown; i++)
        if (mod_known[i].loadstate == MLS_LOADED)
            CHECK(mod_known[i].numbatches == 0, "loaded numbatches invalidation");
}
static void history_convergence(void)
{
    batch_t a[6], b[6];
    shader_t shared = {SHADER_SORT_BLEND};
    const int expected[] = {6, 5, 4, 3, 2, 1};
    reset(2);
    for (int i = 0; i < 6; i++) {
        node(&a[i], i + 1, (unsigned int)i + 1, &shared);
        node(&b[i], i + 1, (unsigned int)i + 1, &shared);
        append(&mod_known[0], i % 2 ? SHADER_SORT_OPAQUE : SHADER_SORT_BLEND, &a[i]);
        append(&mod_known[1], i % 3 ? SHADER_SORT_DECAL : SHADER_SORT_ADDITIVE, &b[i]);
    }
    resort();
    order(&mod_known[0], SHADER_SORT_BLEND, expected, 6, "history-convergence");
    order(&mod_known[1], SHADER_SORT_BLEND, expected, 6, "history-convergence");
    for (int repeat = 0; repeat < 5; repeat++) {
        resort();
        order(&mod_known[0], SHADER_SORT_BLEND, expected, 6, "unchanged-band descending order");
        order(&mod_known[1], SHADER_SORT_BLEND, expected, 6, "unchanged-band descending order");
    }
    shared.sort = SHADER_SORT_ADDITIVE;
    resort();
    for (int m = 0; m < 2; m++) {
        CHECK(!mod_known[m].batches[SHADER_SORT_BLEND], "shared shader vacates old destination");
        order(&mod_known[m], SHADER_SORT_ADDITIVE, expected, 6, "shared destination migration");
    }
    for (int i = 0; i < 6; i++) {
        CHECK(a[i].user.bmodel.sortsequence == (unsigned int)i + 1, "immutable creation key");
        CHECK(a[i].user.bmodel.ebobatch == 101u + (unsigned int)i, "cache index unchanged");
        CHECK(a[i].user.bmodel.shadowbatch == 201u + (unsigned int)i, "shadow index unchanged");
    }
}
static void split_reunite(void)
{
    batch_t nodes[6];
    shader_t shaders[2] = {{SHADER_SORT_BLEND}, {SHADER_SORT_BLEND}};
    const int all[] = {6, 5, 4, 3, 2, 1}, odd[] = {5, 3, 1}, even[] = {6, 4, 2};
    reset(1);
    for (int i = 5; i >= 0; i--) {
        node(&nodes[i], i + 1, (unsigned int)i + 1, &shaders[i % 2]);
        append(&mod_known[0], SHADER_SORT_BLEND, &nodes[i]);
    }
    resort();
    shaders[0].sort = SHADER_SORT_OPAQUE;
    resort();
    order(&mod_known[0], SHADER_SORT_OPAQUE, odd, 3, "split odd");
    order(&mod_known[0], SHADER_SORT_BLEND, even, 3, "split even");
    shaders[0].sort = SHADER_SORT_BLEND;
    resort();
    order(&mod_known[0], SHADER_SORT_BLEND, all, 6, "split/reunite restores creation order");
}
static void unknown_and_nonbrush(void)
{
    batch_t nodes[12];
    shader_t shared = {SHADER_SORT_BLEND};
    const int stable[] = {1, 2, 3, 4};
    unsigned int saved[4][3];
    reset(3);
    mod_known[1].type = mod_alias;
    for (int i = 0; i < 4; i++) {
        /* One unknown key disables canonical sorting for this entire brush band. */
        node(&nodes[i], i + 1, i == 1 ? 0u : (unsigned int)i + 1, &shared);
        append(&mod_known[0], i < 2 ? SHADER_SORT_OPAQUE : SHADER_SORT_ADDITIVE, &nodes[i]);
        node(&nodes[i + 4], i + 1, 0, &shared);
        /* Alias-owned union data must not be interpreted as brush sequence keys. */
        for (int j = 0; j < 3; j++) saved[i][j] = nodes[i + 4].user.alias[j] = 999u + (unsigned int)i;
        append(&mod_known[1], i < 2 ? SHADER_SORT_OPAQUE : SHADER_SORT_ADDITIVE, &nodes[i + 4]);
        node(&nodes[i + 8], i + 1, 0, &shared);
        append(&mod_known[2], i < 2 ? SHADER_SORT_OPAQUE : SHADER_SORT_ADDITIVE, &nodes[i + 8]);
    }
    for (int repeat = 0; repeat < 4; repeat++) {
        resort();
        order(&mod_known[0], SHADER_SORT_BLEND, stable, 4, "unknown-key stable append order");
        order(&mod_known[1], SHADER_SORT_BLEND, stable, 4, "nonbrush stable append order");
        order(&mod_known[2], SHADER_SORT_BLEND, stable, 4, "all-zero-key stable append order");
        for (int i = 0; i < 4; i++)
            CHECK(!memcmp(saved[i], nodes[i + 4].user.alias, sizeof(saved[i])), "alias union unchanged");
    }
}
static void skipped_and_null(void)
{
    batch_t nodes[4];
    shader_t moved = {SHADER_SORT_BLEND};
    reset(4);
    mod_known[1].loadstate = MLS_LOADING;
    mod_known[2].loadstate = MLS_NOTLOADED;
    for (int i = 0; i < 4; i++) {
        node(&nodes[i], i + 1, 1, i == 0 ? NULL : &moved);
        append(&mod_known[i], SHADER_SORT_DECAL, &nodes[i]);
    }
    resort();
    CHECK(mod_known[0].batches[SHADER_SORT_DECAL] == &nodes[0], "null shader retains old band");
    CHECK(!mod_known[0].batches[SHADER_SORT_BLEND], "null shader not migrated");
    CHECK(mod_known[3].batches[SHADER_SORT_BLEND] == &nodes[3], "later loaded model processed");
    for (int i = 1; i < 3; i++) {
        CHECK(mod_known[i].batches[SHADER_SORT_DECAL] == &nodes[i], "loading/unloaded list untouched");
        CHECK(!mod_known[i].batches[SHADER_SORT_BLEND] && !nodes[i].next, "skipped node untouched");
        CHECK(mod_known[i].numbatches == 73 + i, "skipped numbatches untouched");
    }
    reset(0);
    resort(); /* Cache invalidation is once per call even with no models. */
}
static void long_list(void)
{
    enum { COUNT = 4096 };
    batch_t *nodes = calloc(COUNT, sizeof(*nodes));
    shader_t shared = {SHADER_SORT_BLEND};
    CHECK(nodes, "allocation");
    reset(1);
    for (unsigned int i = 0; i < COUNT; i++) {
        unsigned int key = (i * 2053u) % COUNT + 1u;
        node(&nodes[i], (int)key, key, &shared);
        append(&mod_known[0], (int)(i % SHADER_SORT_COUNT), &nodes[i]);
    }
    for (int repeat = 0; repeat < 2; repeat++) {
        batch_t *b;
        resort();
        b = mod_known[0].batches[SHADER_SORT_BLEND];
        for (int key = COUNT; key > 0; key--) {
            CHECK(b && b->id == key, "4096 nodes: descending order, no loss/duplicates/cycles");
            b = b->next;
        }
        CHECK(!b, "4096 nodes: exact termination");
        for (int band = 0; band < SHADER_SORT_COUNT; band++)
            if (band != SHADER_SORT_BLEND) CHECK(!mod_known[0].batches[band], "old bands empty");
    }
    free(nodes);
}
static void equal_keys(void)
{
    batch_t nodes[3];
    shader_t shader = {SHADER_SORT_BLEND};
    const int expected[] = {3, 1, 2};
    reset(1);
    for (int i = 0; i < 3; i++) {
        node(&nodes[i], i + 1, i == 2 ? 8u : 7u, &shader);
        append(&mod_known[0], SHADER_SORT_BLEND, &nodes[i]);
    }
    resort();
    order(&mod_known[0], SHADER_SORT_BLEND, expected, 3, "equal-key merge stability");
}
int main(void)
{
    history_convergence();
    split_reunite();
    unknown_and_nonbrush();
    skipped_and_null();
    long_list();
    equal_keys();
    puts("PASS: shader rebucketing regression");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, help='project root containing engine/gl/gl_model.c')
    parser.add_argument('--cc', default='gcc', help='C compiler executable')
    parser.add_argument('--output', type=Path, help='directory for generated C and executables (temporary by default)')
    args = parser.parse_args()
    engine = args.engine or infer_engine()
    source = (engine / 'engine/gl/gl_model.c').read_text(encoding='utf-8')
    header = (engine / 'engine/gl/gl_model.h').read_text(encoding='utf-8')
    enums = [re.search(pattern, header, re.S).group(0) for pattern in (
        r'typedef enum \{\s*SHADER_SORT_NONE,.*?\} shadersort_t;',
        r'typedef enum \{mod_brush,.*?\} modtype_t;',
        r'enum\s*\{\s*MLS_NOTLOADED,.*?\};')]
    if not re.search(r'unsigned int\s+sortsequence\s*;', header):
        raise SystemExit('Missing brush sortsequence field')
    if not re.search(r'batch = ZG_Malloc\([^;]+;\s*batch->user\.bmodel\.sortsequence = i \+ 1;', source):
        raise SystemExit('Missing immutable sequence assignment at BSP batch allocation')
    production = function(source, 'static batch_t *Mod_SortBatchSequence(') + '\n' + function(source, 'void Mod_ResortShaders(')
    append_code = 'b->next = NULL;\n\t\t\t\t*tails[bs] = b;\n\t\t\t\ttails[bs] = &b->next;'
    if production.count(append_code) != 1:
        raise SystemExit('Cannot locate append block for prepend negative control')
    prepend = production.replace(append_code, 'b->next = mod->batches[bs];\n\t\t\t\tmod->batches[bs] = b;\n\t\t\t\t(void)tails[bs];')
    sort_call = 'mod->batches[j] = Mod_SortBatchSequence(mod->batches[j]);'
    if production.count(sort_call) != 1:
        raise SystemExit('Cannot locate canonical sort for negative control')
    unsorted = production.replace(sort_call, '(void)Mod_SortBatchSequence;')
    variants = [('production', production, None),
                ('old_prepend', prepend, 'unknown-key stable append order'),
                ('no_canonical_sort', unsorted, 'history-convergence')]
    temporary = tempfile.TemporaryDirectory() if args.output is None else None
    output = args.output or Path(temporary.name)
    output.mkdir(parents=True, exist_ok=True)
    try:
        for name, code, expected_failure in variants:
            cfile = output / (name + '.c')
            cfile.write_text(STUBS % tuple(enums) + code + HARNESS, encoding='utf-8')
            for optimization in ('O0', 'O2'):
                executable = (output / (name + '_' + optimization + '.exe')).resolve()
                command = [args.cc, '-std=c99', '-Wall', '-Wextra', '-Werror', '-' + optimization, str(cfile), '-o', str(executable)]
                subprocess.run(command, check=True)
                result = subprocess.run([str(executable)], capture_output=True, text=True)
                if expected_failure is None:
                    if result.returncode:
                        raise SystemExit(result.stdout + result.stderr)
                    print(f'{name} {optimization}: {result.stdout.strip()}')
                else:
                    if result.returncode == 0 or 'FAIL: ' + expected_failure not in result.stderr:
                        raise SystemExit(f'{name} {optimization}: unexpected control outcome\n{result.stdout}{result.stderr}')
                    print(f'{name} {optimization}: compiled; rejected by {expected_failure} assertion')
    finally:
        if temporary:
            temporary.cleanup()


if __name__ == '__main__':
    main()
