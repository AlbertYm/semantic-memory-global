/*
 * test_ui.c — Tests for the graph visualization UI module.
 *
 * Covers: config persistence, embedded asset lookup, layout engine.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h"
#include "../src/foundation/compat_thread.h"
#include "../src/foundation/platform.h"
#include "test_framework.h"
#include "test_helpers.h"
#include "ui/config.h"
#include "ui/embedded_assets.h"
#include "ui/httpd.h"
#include "ui/http_server.h"
#include "ui/layout3d.h"
#include "memory/evolution_engine.h"
#include "memory/global_memory.h"
#include "store/store.h"
#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ui_manager_sock_t;
#define UI_MANAGER_BAD_SOCKET INVALID_SOCKET
#define ui_manager_close_socket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/wait.h>
typedef int ui_manager_sock_t;
#define UI_MANAGER_BAD_SOCKET (-1)
#define ui_manager_close_socket close
#endif

/* ── Config tests ─────────────────────────────────────────────── */

TEST(config_load_defaults) {
    /* Loading with no config file should give defaults */
    cbm_ui_config_t cfg;
    cfg.ui_enabled = true; /* set non-default to verify load overwrites */
    cfg.ui_port = 1234;

    /* Use a temp HOME to avoid touching real config */
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_test_config_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    cbm_setenv("HOME", td, 1);

    cbm_ui_config_load(&cfg);

    ASSERT_FALSE(cfg.ui_enabled);
    ASSERT_EQ(cfg.ui_port, 9749);

    /* Restore HOME */
    if (old_home) {
        cbm_setenv("HOME", old_home, 1);
        free(old_home);
    }

    PASS();
}

TEST(config_save_and_reload) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_test_config_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    cbm_setenv("HOME", td, 1);

    /* Save */
    cbm_ui_config_t cfg = {.ui_enabled = true, .ui_port = 8080};
    cbm_ui_config_save(&cfg);

    /* Reload */
    cbm_ui_config_t loaded;
    cbm_ui_config_load(&loaded);

    ASSERT_TRUE(loaded.ui_enabled);
    ASSERT_EQ(loaded.ui_port, 8080);

    if (old_home) {
        cbm_setenv("HOME", old_home, 1);
        free(old_home);
    }

    PASS();
}

TEST(config_overwrite) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_test_config_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    cbm_setenv("HOME", td, 1);

    /* Save with ui_enabled=true */
    cbm_ui_config_t cfg1 = {.ui_enabled = true, .ui_port = 9749};
    cbm_ui_config_save(&cfg1);

    /* Overwrite with ui_enabled=false */
    cbm_ui_config_t cfg2 = {.ui_enabled = false, .ui_port = 9749};
    cbm_ui_config_save(&cfg2);

    /* Reload should show false */
    cbm_ui_config_t loaded;
    cbm_ui_config_load(&loaded);
    ASSERT_FALSE(loaded.ui_enabled);

    if (old_home) {
        cbm_setenv("HOME", old_home, 1);
        free(old_home);
    }

    PASS();
}

TEST(config_corrupt_file) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_test_config_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    cbm_setenv("HOME", td, 1);

    /* Write garbage to config path */
    char path[1024];
    cbm_ui_config_path(path, (int)sizeof(path));

    /* Ensure directory exists (portable — no system("mkdir -p")) */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.cache/semantic-memory-mcp", td);
    cbm_mkdir_p(dir, 0755);

    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "this is not json!!!");
    fclose(f);

    /* Should load defaults, not crash */
    cbm_ui_config_t cfg;
    cbm_ui_config_load(&cfg);
    ASSERT_FALSE(cfg.ui_enabled);
    ASSERT_EQ(cfg.ui_port, 9749);

    if (old_home) {
        cbm_setenv("HOME", old_home, 1);
        free(old_home);
    }

    PASS();
}

TEST(config_missing_fields) {
    char tmpdir[256];
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cbm_test_config_XXXXXX");
    char *td = cbm_mkdtemp(tmpdir);
    ASSERT_NOT_NULL(td);

    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;
    cbm_setenv("HOME", td, 1);

    /* Write JSON with only ui_port */
    char path[1024];
    cbm_ui_config_path(path, (int)sizeof(path));

    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.cache/semantic-memory-mcp", td);
    cbm_mkdir_p(dir, 0755);

    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fprintf(f, "{\"ui_port\": 5555}");
    fclose(f);

    cbm_ui_config_t cfg;
    cbm_ui_config_load(&cfg);
    ASSERT_FALSE(cfg.ui_enabled); /* defaults for missing field */
    ASSERT_EQ(cfg.ui_port, 5555); /* present field loaded */

    if (old_home) {
        cbm_setenv("HOME", old_home, 1);
        free(old_home);
    }

    PASS();
}

/* ── Embedded asset tests ─────────────────────────────────────── */

TEST(embedded_lookup_not_found) {
    /* With stub, everything should return NULL */
    const cbm_embedded_file_t *f = cbm_embedded_lookup("/nonexistent");
    ASSERT_NULL(f);
    PASS();
}

TEST(embedded_stub_count) {
    /* Stub should have 0 files */
    ASSERT_EQ(CBM_EMBEDDED_FILE_COUNT, 0);
    PASS();
}

/* ── Layout tests ─────────────────────────────────────────────── */

TEST(layout_empty_graph) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    /* No nodes in store → empty result */
    cbm_layout_result_t *r =
        cbm_layout_compute(store, "test-project", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->node_count, 0);
    ASSERT_EQ(r->edge_count, 0);

    cbm_layout_free(r);
    cbm_store_close(store);
    PASS();
}

TEST(layout_single_node) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    cbm_store_upsert_project(store, "test", "/tmp/test");
    cbm_node_t node = {
        .project = "test",
        .label = "Function",
        .name = "main",
        .qualified_name = "test::main",
        .file_path = "main.c",
        .start_line = 1,
        .end_line = 10,
    };
    int64_t id = cbm_store_upsert_node(store, &node);
    ASSERT_GT(id, 0);

    cbm_layout_result_t *r = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->node_count, 1);
    ASSERT_STR_EQ(r->nodes[0].name, "main");
    ASSERT_EQ(r->total_nodes, 1);

    cbm_layout_free(r);
    cbm_store_close(store);
    PASS();
}

TEST(layout_two_connected) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    cbm_store_upsert_project(store, "test", "/tmp/test");

    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "foo",
                     .qualified_name = "test::foo",
                     .file_path = "a.c",
                     .start_line = 1,
                     .end_line = 5};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "bar",
                     .qualified_name = "test::bar",
                     .file_path = "b.c",
                     .start_line = 1,
                     .end_line = 5};
    int64_t id1 = cbm_store_upsert_node(store, &n1);
    int64_t id2 = cbm_store_upsert_node(store, &n2);

    cbm_edge_t edge = {.project = "test", .source_id = id1, .target_id = id2, .type = "CALLS"};
    cbm_store_insert_edge(store, &edge);

    cbm_layout_result_t *r = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NOT_NULL(r);
    ASSERT_EQ(r->node_count, 2);

    /* Nodes should be positioned apart (not at same point) */
    float dx = r->nodes[0].x - r->nodes[1].x;
    float dy = r->nodes[0].y - r->nodes[1].y;
    float dz = r->nodes[0].z - r->nodes[1].z;
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    ASSERT_GT((long long)(dist * 100), 0);

    ASSERT_EQ(r->edge_count, 1);

    cbm_layout_free(r);
    cbm_store_close(store);
    PASS();
}

TEST(layout_respects_max_nodes) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    cbm_store_upsert_project(store, "test", "/tmp/test");

    /* Insert 20 nodes */
    for (int i = 0; i < 20; i++) {
        char name[32], qn[64];
        snprintf(name, sizeof(name), "fn%d", i);
        snprintf(qn, sizeof(qn), "test::fn%d", i);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "a.c",
                        .start_line = i,
                        .end_line = i + 1};
        cbm_store_upsert_node(store, &n);
    }

    /* max_nodes=5 should return at most 5 */
    cbm_layout_result_t *r = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 5);
    ASSERT_NOT_NULL(r);
    ASSERT_LTE(r->node_count, 5);
    ASSERT_EQ(r->total_nodes, 20);

    cbm_layout_free(r);
    cbm_store_close(store);
    PASS();
}

TEST(layout_clamps_render_cap_from_env) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    const char *old_raw = getenv("CBM_UI_MAX_RENDER_NODES");
    char *old_cap = old_raw ? strdup(old_raw) : NULL;
    cbm_setenv("CBM_UI_MAX_RENDER_NODES", "25", 1);

    cbm_store_upsert_project(store, "test", "/tmp/test");

    for (int i = 0; i < 40; i++) {
        char name[32], qn[64];
        snprintf(name, sizeof(name), "fn%d", i);
        snprintf(qn, sizeof(qn), "test::fn%d", i);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "a.c",
                        .start_line = i,
                        .end_line = i + 1};
        cbm_store_upsert_node(store, &n);
    }

    cbm_layout_result_t *r = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 50000);
    ASSERT_NOT_NULL(r);
    ASSERT_LTE(r->node_count, 25);
    ASSERT_EQ(r->total_nodes, 40);

    cbm_layout_free(r);
    cbm_store_close(store);
    if (old_cap) {
        cbm_setenv("CBM_UI_MAX_RENDER_NODES", old_cap, 1);
        free(old_cap);
    } else {
        cbm_unsetenv("CBM_UI_MAX_RENDER_NODES");
    }
    PASS();
}

TEST(layout_deterministic) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    cbm_store_upsert_project(store, "test", "/tmp/test");

    cbm_node_t n1 = {.project = "test",
                     .label = "Function",
                     .name = "alpha",
                     .qualified_name = "test::alpha",
                     .file_path = "a.c",
                     .start_line = 1,
                     .end_line = 5};
    cbm_node_t n2 = {.project = "test",
                     .label = "Function",
                     .name = "beta",
                     .qualified_name = "test::beta",
                     .file_path = "b.c",
                     .start_line = 1,
                     .end_line = 5};
    cbm_store_upsert_node(store, &n1);
    cbm_store_upsert_node(store, &n2);

    /* Run twice, check positions match */
    cbm_layout_result_t *r1 = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    cbm_layout_result_t *r2 = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NOT_NULL(r1);
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ(r1->node_count, r2->node_count);

    for (int i = 0; i < r1->node_count; i++) {
        ASSERT_FLOAT_EQ(r1->nodes[i].x, r2->nodes[i].x, 0.001);
        ASSERT_FLOAT_EQ(r1->nodes[i].y, r2->nodes[i].y, 0.001);
        ASSERT_FLOAT_EQ(r1->nodes[i].z, r2->nodes[i].z, 0.001);
    }

    cbm_layout_free(r1);
    cbm_layout_free(r2);
    cbm_store_close(store);
    PASS();
}

TEST(layout_to_json) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);

    cbm_store_upsert_project(store, "test", "/tmp/test");

    cbm_node_t n = {.project = "test",
                    .label = "Function",
                    .name = "hello",
                    .qualified_name = "test::hello",
                    .file_path = "a.c",
                    .start_line = 1,
                    .end_line = 5};
    cbm_store_upsert_node(store, &n);

    cbm_layout_result_t *r = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NOT_NULL(r);

    char *json = cbm_layout_to_json(r);
    ASSERT_NOT_NULL(json);

    /* Should contain key fields */
    ASSERT(strstr(json, "\"nodes\"") != NULL);
    ASSERT(strstr(json, "\"edges\"") != NULL);
    ASSERT(strstr(json, "\"total_nodes\"") != NULL);
    ASSERT(strstr(json, "\"hello\"") != NULL);
    ASSERT(strstr(json, "\"Function\"") != NULL);

    free(json);
    cbm_layout_free(r);
    cbm_store_close(store);
    PASS();
}

TEST(layout_null_inputs) {
    /* NULL store → NULL result */
    cbm_layout_result_t *r = cbm_layout_compute(NULL, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NULL(r);

    /* NULL project → NULL result */
    cbm_store_t *store = cbm_store_open_memory();
    r = cbm_layout_compute(store, NULL, CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NULL(r);

    /* cbm_layout_free(NULL) should not crash */
    cbm_layout_free(NULL);

    /* cbm_layout_to_json(NULL) should return NULL */
    char *json = cbm_layout_to_json(NULL);
    ASSERT_NULL(json);

    cbm_store_close(store);
    PASS();
}

/* ── Dead-code classification (distilled from PR #789) ────────── */

static const cbm_layout_node_t *find_layout_node(const cbm_layout_result_t *r, const char *name) {
    for (int i = 0; i < r->node_count; i++) {
        if (r->nodes[i].name && strcmp(r->nodes[i].name, name) == 0) {
            return &r->nodes[i];
        }
    }
    return NULL;
}

/* A function with zero callers/usages and no entry/test/exported flag is
 * "dead"; entry-point, test, and exported functions are NOT dead even at zero
 * callers; a called function reports its true full-graph incoming CALLS degree
 * ("single" at 1, "normal" at >=2). Non-Function labels are "structural". */
TEST(layout_dead_code_classification) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_NOT_NULL(store);
    ASSERT_EQ(cbm_store_upsert_project(store, "dc", "/tmp/dc"), CBM_STORE_OK);

    /* Candidates (Function, non-test path unless noted). */
    cbm_node_t dead = {.project = "dc",
                       .label = "Function",
                       .name = "deadfn",
                       .qualified_name = "dc::deadfn",
                       .file_path = "src/a.c",
                       .properties_json = "{\"is_entry_point\":false,\"is_test\":false,"
                                          "\"is_exported\":false}"};
    cbm_node_t entry = {.project = "dc",
                        .label = "Function",
                        .name = "entryfn",
                        .qualified_name = "dc::entryfn",
                        .file_path = "src/b.c",
                        .properties_json = "{\"is_entry_point\":true}"};
    cbm_node_t tst = {.project = "dc",
                      .label = "Function",
                      .name = "testfn",
                      .qualified_name = "dc::testfn",
                      .file_path = "src/c.c",
                      .properties_json = "{\"is_test\":true}"};
    cbm_node_t tstpath = {.project = "dc",
                          .label = "Function",
                          .name = "bypathfn",
                          .qualified_name = "dc::bypathfn",
                          .file_path = "tests/mod_helpers.c",
                          .properties_json = "{}"};
    cbm_node_t exp = {.project = "dc",
                      .label = "Function",
                      .name = "exportedfn",
                      .qualified_name = "dc::exportedfn",
                      .file_path = "src/d.c",
                      .properties_json = "{\"is_exported\":true}"};
    cbm_node_t single = {.project = "dc",
                         .label = "Function",
                         .name = "calledonce",
                         .qualified_name = "dc::calledonce",
                         .file_path = "src/e.c",
                         .properties_json = "{}"};
    cbm_node_t norm = {.project = "dc",
                       .label = "Function",
                       .name = "callednormal",
                       .qualified_name = "dc::callednormal",
                       .file_path = "src/f.c",
                       .properties_json = "{}"};
    cbm_node_t caller = {.project = "dc",
                         .label = "Function",
                         .name = "caller",
                         .qualified_name = "dc::caller",
                         .file_path = "src/g.c",
                         .properties_json = "{}"};
    /* A structural (non-Function) node is never a dead-code candidate. */
    cbm_node_t cls = {.project = "dc",
                      .label = "Class",
                      .name = "SomeClass",
                      .qualified_name = "dc::SomeClass",
                      .file_path = "src/h.c",
                      .properties_json = "{}"};

    int64_t id_dead = cbm_store_upsert_node(store, &dead);
    cbm_store_upsert_node(store, &entry);
    cbm_store_upsert_node(store, &tst);
    cbm_store_upsert_node(store, &tstpath);
    cbm_store_upsert_node(store, &exp);
    int64_t id_single = cbm_store_upsert_node(store, &single);
    int64_t id_norm = cbm_store_upsert_node(store, &norm);
    int64_t id_caller = cbm_store_upsert_node(store, &caller);
    cbm_store_upsert_node(store, &cls);
    ASSERT_GT(id_dead, 0);

    /* calledonce ← 1 CALLS; callednormal ← 2 CALLS (full-graph inbound). */
    cbm_edge_t e1 = {
        .project = "dc", .source_id = id_caller, .target_id = id_single, .type = "CALLS"};
    cbm_edge_t e2 = {
        .project = "dc", .source_id = id_caller, .target_id = id_norm, .type = "CALLS"};
    cbm_edge_t e3 = {.project = "dc", .source_id = id_dead, .target_id = id_norm, .type = "CALLS"};
    cbm_store_insert_edge(store, &e1);
    cbm_store_insert_edge(store, &e2);
    cbm_store_insert_edge(store, &e3);

    cbm_layout_result_t *r = cbm_layout_compute(store, "dc", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    ASSERT_NOT_NULL(r);

    const cbm_layout_node_t *ln;

    ln = find_layout_node(r, "deadfn");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "dead");
    ASSERT_EQ(ln->in_calls, 0);

    ln = find_layout_node(r, "entryfn");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "entry");

    ln = find_layout_node(r, "testfn");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "test");

    ln = find_layout_node(r, "bypathfn"); /* test detected via file path */
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "test");

    ln = find_layout_node(r, "exportedfn");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "exported");

    ln = find_layout_node(r, "calledonce");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "single");
    ASSERT_EQ(ln->in_calls, 1);

    ln = find_layout_node(r, "callednormal");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "normal");
    ASSERT_EQ(ln->in_calls, 2);

    ln = find_layout_node(r, "SomeClass");
    ASSERT_NOT_NULL(ln);
    ASSERT_STR_EQ(ln->status, "structural");

    /* The classification must survive JSON serialization. */
    char *json = cbm_layout_to_json(r);
    ASSERT_NOT_NULL(json);
    ASSERT(strstr(json, "\"status\":\"dead\"") != NULL);
    ASSERT(strstr(json, "\"in_calls\":2") != NULL);
    free(json);

    cbm_layout_free(r);
    cbm_store_close(store);
    PASS();
}

/* ── Octree recursion guard (distilled from PR #821; refs #498/#726/#402) ── */

/* Bodies that share a position made octree_insert subdivide forever — the
 * cell around them shrinks but never separates them, so one octree cell is
 * calloc'd per level until the process dies (stack overflow) or freezes the
 * machine allocating (the 34GB-swap reports). Fixed by the depth/half-size
 * floor in src/ui/layout3d.c (OCTREE_MAX_DEPTH / OCTREE_MIN_HALF).
 *
 * Coincident positions are reachable through the public layout API: layout3d
 * anchors each node by fnv1a(file cluster key) and jitters it with a PRNG
 * seeded by fnv1a(qualified_name). The three QNs below are distinct strings
 * with IDENTICAL 32-bit FNV-1a hashes (0x06bb012e, found by offline brute
 * force), so in the same file they get bit-identical positions on every
 * platform (integer hashing only — no libm in the coincidence path).
 *
 * A literal sub-ULP-separated pair cannot be constructed through the public
 * API: same-anchor positions are quantized to exact multiples of the jitter
 * quantum (5/4096 — exactly 20 ULP at anchor magnitude ~600), and
 * cross-anchor separations depend on the platform's cosf/sinf bits. Exact
 * coincidence is the API-reachable degenerate input, and it necessarily
 * drives the recursion through the sub-ULP regime: half_size falls below
 * ULP(center) with the bodies still unseparated, freezing child centers
 * while cells keep being allocated.
 */
#if !defined(_WIN32)
/* Child body: builds the store and runs the layout so a crash or hang cannot
 * take down the runner (alarm bounds a hang, fork isolates a SIGSEGV).
 * Deliberately NO memory rlimit: under a rlimit a failing calloc makes
 * octree_insert silently truncate and the UNFIXED code would complete —
 * turning this guard vacuously green. The alarm alone bounds the runaway.
 * Exit codes: 0 ok, 2 store setup, 3 layout NULL, 4 node count/lookup,
 * 5 fixture no longer coincident, 6 non-finite coordinate. Never returns. */
static void layout_octree_guard_child(void) {
    alarm(5); /* post-fix the whole child runs in milliseconds */
    cbm_store_t *store = cbm_store_open_memory();
    if (!store)
        _exit(2);
    if (cbm_store_upsert_project(store, "test", "/tmp/test") != CBM_STORE_OK)
        _exit(2);

    /* Distinct QNs, one fnv1a hash — coincident after anchor + jitter. */
    static const char *cqn[3] = {"test::octree_c5988474", "test::octree_c11394919",
                                 "test::octree_c33141700"};
    for (int i = 0; i < 3; i++) {
        char name[32];
        snprintf(name, sizeof(name), "co%d", i);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = cqn[i],
                        .file_path = "pkg/sub/mod/a.c",
                        .start_line = i + 1,
                        .end_line = i + 2};
        if (cbm_store_upsert_node(store, &n) <= 0)
            _exit(2);
    }
    /* A few normally-spread nodes so the octree root box has realistic
     * (non-degenerate) extent, as in the reported repositories. */
    for (int i = 0; i < 3; i++) {
        char name[32], qn[64], fp[32];
        snprintf(name, sizeof(name), "fn%d", i);
        snprintf(qn, sizeof(qn), "test::spread_fn%d", i);
        snprintf(fp, sizeof(fp), "dir%d/f%d.c", i, i);
        cbm_node_t n = {.project = "test",
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = fp,
                        .start_line = 1,
                        .end_line = 2};
        if (cbm_store_upsert_node(store, &n) <= 0)
            _exit(2);
    }

    cbm_layout_result_t *r = cbm_layout_compute(store, "test", CBM_LAYOUT_OVERVIEW, NULL, 0, 100);
    if (!r)
        _exit(3);
    if (r->node_count != 6)
        _exit(4);

    /* The colliding QNs must actually be coincident — identical output
     * coordinates (identical seeds → identical positions, and coincident
     * bodies receive identical forces every iteration, so they stay
     * together). If a seeding change ever breaks this, the fixture no longer
     * reproduces the bug: fail loudly instead of going vacuously green. */
    int ci[3], nc = 0;
    for (int i = 0; i < r->node_count && nc < 3; i++) {
        if (r->nodes[i].qualified_name &&
            strncmp(r->nodes[i].qualified_name, "test::octree_c", 14) == 0)
            ci[nc++] = i;
    }
    if (nc != 3)
        _exit(4);
    for (int k = 1; k < 3; k++) {
        if (r->nodes[ci[k]].x != r->nodes[ci[0]].x || r->nodes[ci[k]].y != r->nodes[ci[0]].y ||
            r->nodes[ci[k]].z != r->nodes[ci[0]].z)
            _exit(5);
    }
    for (int i = 0; i < r->node_count; i++) {
        if (!isfinite(r->nodes[i].x) || !isfinite(r->nodes[i].y) || !isfinite(r->nodes[i].z))
            _exit(6);
    }

    cbm_layout_free(r);
    cbm_store_close(store);
    _exit(0);
}
#endif

TEST(layout_coincident_nodes_bounded) {
#if defined(_WIN32)
    SKIP_PLATFORM("fork/alarm not available; POSIX-only bounded-hang reproduction");
#else
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        FAIL("fork() failed");
    if (pid == 0)
        layout_octree_guard_child(); /* never returns */

    int status = 0;
    (void)waitpid(pid, &status, 0);

    /* Unfixed code dies here: SIGSEGV (unbounded recursion overflowing the
     * stack) or SIGALRM (tail-call-optimized allocation runaway cut off by
     * the child's alarm). Fixed code exits 0 well within the budget. */
    ASSERT_FALSE(WIFSIGNALED(status));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    PASS();
#endif
}

/* ── Stage12 manager live security contract ───────────────────── */

typedef struct {
    cbm_http_server_t *server;
    cbm_thread_t thread;
    int port;
    char token[65];
} ui_manager_fixture_t;

static void *ui_manager_server_thread(void *argument) {
    cbm_http_server_run((cbm_http_server_t *)argument);
    return NULL;
}

static int ui_manager_start(ui_manager_fixture_t *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    fixture->server = cbm_http_server_new_manager();
    if (!fixture->server)
        return -1;
    char url[192];
    if (!cbm_http_server_manager_url(fixture->server, url, sizeof(url)) ||
        sscanf(url, "http://127.0.0.1:%d/#token=%64[0-9a-f]", &fixture->port, fixture->token) !=
            2 ||
        strlen(fixture->token) != 64) {
        cbm_http_server_free(fixture->server);
        return -1;
    }
    if (cbm_thread_create(&fixture->thread, 0, ui_manager_server_thread, fixture->server) != 0) {
        cbm_http_server_free(fixture->server);
        return -1;
    }
    return 0;
}

static void ui_manager_stop(ui_manager_fixture_t *fixture) {
    cbm_http_server_stop(fixture->server);
    cbm_thread_join(&fixture->thread);
    cbm_http_server_free(fixture->server);
    memset(fixture->token, 0, sizeof(fixture->token));
}

static ui_manager_sock_t ui_manager_connect(int port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    ui_manager_sock_t socket_value = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_value == UI_MANAGER_BAD_SOCKET)
        return UI_MANAGER_BAD_SOCKET;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(socket_value, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ui_manager_close_socket(socket_value);
        return UI_MANAGER_BAD_SOCKET;
    }
    return socket_value;
}

static int ui_manager_http(int port, const char *request, char *response, size_t capacity) {
    ui_manager_sock_t socket_value = ui_manager_connect(port);
    if (socket_value == UI_MANAGER_BAD_SOCKET)
        return 0;
    size_t sent = 0, request_length = strlen(request);
    while (sent < request_length) {
#ifdef _WIN32
        int count = send(socket_value, request + sent, (int)(request_length - sent), 0);
#else
        ssize_t count = send(socket_value, request + sent, request_length - sent, 0);
#endif
        if (count <= 0) {
            ui_manager_close_socket(socket_value);
            return 0;
        }
        sent += (size_t)count;
    }
    size_t received = 0;
    while (received + 1 < capacity) {
#ifdef _WIN32
        int count = recv(socket_value, response + received, (int)(capacity - received - 1), 0);
#else
        ssize_t count = recv(socket_value, response + received, capacity - received - 1, 0);
#endif
        if (count <= 0)
            break;
        received += (size_t)count;
    }
    response[received] = '\0';
    ui_manager_close_socket(socket_value);
    return (int)received;
}

static int ui_manager_rpc(int port, const char *host, const char *origin, const char *token,
                          const char *body, char *response, size_t capacity) {
    char request[4096];
    int written = snprintf(request, sizeof(request),
                           "POST /rpc HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\n"
                           "X-Manager-Token: %s\r\nContent-Type: application/json\r\n"
                           "Content-Length: %d\r\n\r\n%s",
                           host, origin, token, (int)strlen(body), body);
    if (written < 0 || (size_t)written >= sizeof(request))
        return 0;
    return ui_manager_http(port, request, response, capacity);
}

static int ui_manager_report_id(const char *json, const char *name, char *out, size_t size) {
    const char *key = strstr(json, name);
    if (!key)
        return 0;
    const char *colon = strchr(key, ':');
    if (!colon)
        return 0;
    const char *start = strchr(colon, '"');
    if (!start)
        return 0;
    start++;
    const char *end = strchr(start, '"');
    if (!end || (size_t)(end - start) >= size)
        return 0;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return 1;
}

static int ui_manager_status(const char *response) {
    return response && strncmp(response, "HTTP/1.1 ", 9) == 0 ? atoi(response + 9) : -1;
}

static int ui_stage14_exec(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    sqlite3_free(error);
    return rc;
}

static int ui_stage14_bind_and_step(sqlite3_stmt *stmt, const char *id, const char *project,
                                    int ordinal, int64_t timestamp) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, ordinal);
    sqlite3_bind_int64(stmt, 4, timestamp);
    return sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
}

static int ui_stage14_p95_ms(uint64_t *samples, int count) {
    for (int i = 1; i < count; i++) {
        uint64_t value = samples[i];
        int j = i - 1;
        while (j >= 0 && samples[j] > value) {
            samples[j + 1] = samples[j];
            j--;
        }
        samples[j + 1] = value;
    }
    return (int)samples[((95 * count + 99) / 100) - 1];
}

static int ui_stage14_measure_rpc(int port, const char *host, const char *origin,
                                  const char *token, const char *body, const char *marker,
                                  int *out_p95_ms) {
    enum { WARMUP = 1, REPEATS = 5 };
    char response[65536];
    uint64_t samples[REPEATS];
    for (int i = 0; i < WARMUP + REPEATS; i++) {
        uint64_t started = cbm_now_ms();
        if (ui_manager_rpc(port, host, origin, token, body, response, sizeof(response)) <= 0 ||
            ui_manager_status(response) != 200 || !strstr(response, marker))
            return 0;
        if (i >= WARMUP)
            samples[i - WARMUP] = cbm_now_ms() - started;
    }
    *out_p95_ms = ui_stage14_p95_ms(samples, REPEATS);
    return 1;
}

static int ui_stage14_seed_scale_fixture(cbm_global_memory_t *global,
                                         const char *project_uuid) {
    enum { MEMORY_ITEMS = 50000, CROSS_PROJECT_EDGES = 250000, TASK_SESSIONS = 100000 };
    sqlite3 *memory = cbm_global_memory_db(global);
    sqlite3 *graph = cbm_global_graph_db(global);
    sqlite3_stmt *memory_insert = NULL, *task_insert = NULL, *workspace_insert = NULL;
    sqlite3_stmt *edge_insert = NULL, *edge_version_insert = NULL;
    const int64_t base_time = 1785100000000LL;
    int rc = ui_stage14_exec(memory, "BEGIN IMMEDIATE;");
    if (rc != SQLITE_OK)
        return rc;
    rc = ui_stage14_exec(graph, "BEGIN IMMEDIATE;");
    if (rc != SQLITE_OK) {
        ui_stage14_exec(memory, "ROLLBACK;");
        return SQLITE_ERROR;
    }

    rc = sqlite3_prepare_v2(memory,
        "INSERT INTO memory_item(id,kind,layer,title,summary,content,scope_project,importance,confidence,reusability,specificity,status,version,created_at,updated_at) "
        "VALUES(?1,'lesson','episodic','Stage14 scale memory','Deterministic scale fixture','Deterministic Stage14 Manager scale fixture',?2,0.9,0.9,0.9,0.9,'active',1,?4,?4);",
        -1, &memory_insert, NULL);
    if (rc == SQLITE_OK) {
        for (int i = 0; i < MEMORY_ITEMS && rc == SQLITE_OK; i++) {
            char id[64];
            snprintf(id, sizeof(id), "scale-memory-%06d", i);
            rc = ui_stage14_bind_and_step(memory_insert, id, project_uuid, i, base_time + i);
        }
    }
    sqlite3_finalize(memory_insert);

    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(memory,
            "INSERT INTO memory_task(task_id,project,task_type,created_at) VALUES(?1,?2,'test','2026-07-27T00:00:00Z');",
            -1, &task_insert, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(memory,
            "INSERT INTO global_task_workspace(task_id,project_uuid,session_id,turn_id,resolver_payload_sha256,created_at) "
            "VALUES(?1,?2,printf('scale-session-%06d',?3),printf('scale-turn-%06d',?3),'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','2026-07-27T00:00:00Z');",
            -1, &workspace_insert, NULL);
    for (int i = 0; i < TASK_SESSIONS && rc == SQLITE_OK; i++) {
        char id[64];
        snprintf(id, sizeof(id), "scale-task-%06d", i);
        rc = ui_stage14_bind_and_step(task_insert, id, project_uuid, i, base_time + i);
        if (rc == SQLITE_OK)
            rc = ui_stage14_bind_and_step(workspace_insert, id, project_uuid, i, base_time + i);
    }
    sqlite3_finalize(task_insert);
    sqlite3_finalize(workspace_insert);

    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(graph,
            "INSERT INTO global_cross_project_edge(edge_id,source_project_uuid,target_project_uuid,relation_type,weight_ppm,confidence_ppm,status,version,updated_at) "
            "VALUES(?1,?2,?2,'scale_relation',900000,800000,'active',1,'2026-07-27T00:00:00Z');",
            -1, &edge_insert, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_prepare_v2(graph,
            "INSERT INTO global_cross_project_edge_version(edge_id,version,payload_sha256,evidence_event_id,created_at) "
            "VALUES(?1,1,'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','scale-evidence','2026-07-27T00:00:00Z');",
            -1, &edge_version_insert, NULL);
    for (int i = 0; i < CROSS_PROJECT_EDGES && rc == SQLITE_OK; i++) {
        char id[64];
        snprintf(id, sizeof(id), "scale-edge-%06d", i);
        rc = ui_stage14_bind_and_step(edge_insert, id, project_uuid, i, base_time + i);
        if (rc == SQLITE_OK) {
            sqlite3_reset(edge_version_insert);
            sqlite3_clear_bindings(edge_version_insert);
            sqlite3_bind_text(edge_version_insert, 1, id, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(edge_version_insert) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
        }
    }
    sqlite3_finalize(edge_insert);
    sqlite3_finalize(edge_version_insert);
    if (rc == SQLITE_OK)
        rc = ui_stage14_exec(memory, "COMMIT;");
    else
        ui_stage14_exec(memory, "ROLLBACK;");
    if (rc == SQLITE_OK)
        rc = ui_stage14_exec(graph, "COMMIT;");
    else
        ui_stage14_exec(graph, "ROLLBACK;");
    return rc;
}

static int ui_stage14_count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static int ui_stage14_print_query_plan(sqlite3 *db, const char *label, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int rows = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *detail = sqlite3_column_text(stmt, 3);
        if (detail) {
            fprintf(stderr, "stage14-scale-query-plan %s: %s\n", label, detail);
            rows++;
        }
    }
    sqlite3_finalize(stmt);
    return rows > 0;
}

TEST(stage12_manager_live_security_contract) {
    char *cache = th_mktempdir("cbm_stage12_manager");
    ASSERT_NOT_NULL(cache);
    const char *existing = getenv("CBM_CACHE_DIR");
    char *saved_cache = existing ? strdup(existing) : NULL;
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", cache, 1), 0);

    ui_manager_fixture_t fixture;
    ASSERT_EQ(ui_manager_start(&fixture), 0);
    char host[64], origin[96], request[2048], response[8192];
    snprintf(host, sizeof(host), "127.0.0.1:%d", fixture.port);
    snprintf(origin, sizeof(origin), "http://%s", host);

    snprintf(request, sizeof(request), "GET / HTTP/1.1\r\nHost: %s\r\n\r\n", host);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_NOT_NULL(strstr(response, "Content-Security-Policy:"));
    ASSERT_NOT_NULL(strstr(response, "X-Content-Type-Options: nosniff"));
    ASSERT_NOT_NULL(strstr(response, "X-Frame-Options: DENY"));
    ASSERT_NOT_NULL(strstr(response, "Cache-Control: no-store"));
    ASSERT_NOT_NULL(strstr(response, "Referrer-Policy: no-referrer"));

    snprintf(request, sizeof(request),
             "GET /api/manager/health?project=stage12-fixture HTTP/1.1\r\nHost: %s\r\n\r\n", host);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 401);

    snprintf(request, sizeof(request),
             "GET /api/manager/health?project=stage12-fixture HTTP/1.1\r\nHost: "
             "%s\r\nX-Manager-Token: %064d\r\n\r\n",
             host, 0);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 401);

    snprintf(request, sizeof(request),
             "GET /api/manager/health?project=stage12-fixture HTTP/1.1\r\nHost: "
             "%s\r\nX-Manager-Token: %s\r\n\r\n",
             host, fixture.token);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);

    snprintf(request, sizeof(request),
             "GET /api/manager/health?project=stage12-fixture HTTP/1.1\r\nHost: "
             "localhost:%d\r\nX-Manager-Token: %s\r\n\r\n",
             fixture.port, fixture.token);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 401);

    const char *body = "{\"project\":\"stage12-fixture\",\"destination_root\":\"unused\"}";
    snprintf(
        request, sizeof(request),
        "POST /api/manager/backup HTTP/1.1\r\nHost: %s\r\nOrigin: http://evil.invalid\r\n"
        "X-Manager-Token: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
        host, fixture.token, (int)strlen(body), body);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 403);
    ASSERT_NOT_NULL(strstr(response, "ORIGIN_REJECTED"));

    snprintf(request, sizeof(request),
             "POST /api/manager/backup HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\n"
             "X-Manager-Token: %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s",
             host, origin, fixture.token, (int)strlen(body), body);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 415);

    const char *malformed = "{";
    snprintf(request, sizeof(request),
             "POST /api/manager/backup HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\n"
             "X-Manager-Token: %s\r\nContent-Type: application/json\r\nContent-Length: 1\r\n\r\n%s",
             host, origin, fixture.token, malformed);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 400);

    snprintf(request, sizeof(request),
             "POST /api/process-kill HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\nX-Manager-Token: %s\r\n"
             "Content-Type: application/json\r\nContent-Length: 2\r\n\r\n{}",
             host, origin, fixture.token);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 403);
    ASSERT_NOT_NULL(strstr(response, "FEATURE_DISABLED"));

    snprintf(
        request, sizeof(request),
        "POST /api/manager/backup HTTP/1.1\r\nHost: %s\r\nOrigin: %s\r\nX-Manager-Token: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n",
        host, origin, fixture.token, CBM_HTTP_MAX_BODY + 1);
    ASSERT_TRUE(ui_manager_http(fixture.port, request, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 413);

    ui_manager_stop(&fixture);
    if (saved_cache) {
        cbm_setenv("CBM_CACHE_DIR", saved_cache, 1);
        free(saved_cache);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    th_cleanup(cache);
    PASS();
}

/* ── Suite ────────────────────────────────────────────────────── */

TEST(stage14_manager_rpc_global_contract) {
    char *cache = th_mktempdir("cbm_stage14_manager_rpc");
    ASSERT_NOT_NULL(cache);
    const char *existing_cache = getenv("CBM_CACHE_DIR");
    const char *existing_mock = getenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK");
    char *saved_cache = existing_cache ? strdup(existing_cache) : NULL;
    char *saved_mock = existing_mock ? strdup(existing_mock) : NULL;
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", cache, 1), 0);
    ASSERT_EQ(cbm_setenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK", "1", 1), 0);

    char memory_path[1024], graph_path[1024];
#ifdef _WIN32
    snprintf(memory_path, sizeof(memory_path), "%s\\__global__-memory.db", cache);
    snprintf(graph_path, sizeof(graph_path), "%s\\__global__-graph.db", cache);
#else
    snprintf(memory_path, sizeof(memory_path), "%s/__global__-memory.db", cache);
    snprintf(graph_path, sizeof(graph_path), "%s/__global__-graph.db", cache);
#endif
    cbm_global_memory_t *global = cbm_global_memory_open(memory_path, graph_path);
    ASSERT_NOT_NULL(global);
    cbm_project_resolution_t source = {0}, target = {0};
    ASSERT_EQ(cbm_project_resolve("H:\\Codex_H\\runtime-data\\stage14\\fixtures\\manager-rpc-source", NULL, NULL, &source), 0);
    ASSERT_EQ(cbm_project_resolve("H:\\Codex_H\\runtime-data\\stage14\\fixtures\\manager-rpc-target", NULL, NULL, &target), 0);
    char *report = NULL;
    ASSERT_EQ(cbm_global_ensure_project(global, &source, "manager-rpc-project-source", &report), CBM_STORE_OK);
    free(report); report = NULL;
    ASSERT_EQ(cbm_global_ensure_project(global, &target, "manager-rpc-project-target", &report), CBM_STORE_OK);
    free(report); report = NULL;

    cbm_memory_item_t first = {0};
    first.id = "rpc-manager-memory-a";
    first.kind = "lesson";
    first.layer = "episodic";
    first.title = "RPC Manager memory A";
    first.summary = "First isolated Manager memory";
    first.content = "First isolated Manager memory";
    first.scope_project = source.project_uuid;
    first.importance = first.confidence = first.reusability = first.specificity = 0.9;
    first.status = "active";
    first.version = 1;
    char *new_id = NULL;
    ASSERT_EQ(cbm_store_memory_append_candidate(cbm_global_memory_store(global), &first, &new_id), CBM_STORE_OK);
    free(new_id);
    ASSERT_EQ(cbm_store_memory_index_candidate(cbm_global_memory_store(global), &first, first.id, NULL), CBM_STORE_OK);
    cbm_memory_item_t second = first;
    second.id = "rpc-manager-memory-b";
    second.title = "RPC Manager memory B";
    second.summary = "Second isolated Manager memory";
    second.content = "Second isolated Manager memory";
    ASSERT_EQ(cbm_store_memory_append_candidate(cbm_global_memory_store(global), &second, &new_id), CBM_STORE_OK);
    free(new_id);
    ASSERT_EQ(cbm_store_memory_index_candidate(cbm_global_memory_store(global), &second, second.id, NULL), CBM_STORE_OK);
    sqlite3 *memory_db = cbm_global_memory_db(global);
    char provenance_sql[2048];
    snprintf(provenance_sql, sizeof(provenance_sql),
             "INSERT INTO global_memory_provenance(memory_item_id,project_uuid,legacy_project_id,source_kind,payload_sha256,created_at) VALUES"
             "('rpc-manager-memory-a','%s','legacy-rpc-a','isolated_rpc_fixture','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','2026-07-27T00:00:00Z'),"
             "('rpc-manager-memory-b','%s','legacy-rpc-b','isolated_rpc_fixture','bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','2026-07-27T00:00:01Z');",
             source.project_uuid, source.project_uuid);
    ASSERT_EQ(sqlite3_exec(memory_db, provenance_sql, NULL, NULL, NULL), SQLITE_OK);

    cbm_memory_query_t retrieve_query = {.kind = "lesson", .limit = 10};
    cbm_global_retrieval_result_t retrieved = {0};
    ASSERT_EQ(cbm_global_memory_retrieve(global, "manager-rpc-retrieval", source.project_uuid,
                                         100000, &retrieve_query, &retrieved), CBM_STORE_OK);
    ASSERT_NOT_NULL(retrieved.session_id);

    cbm_task_begin_input_t begin = {.session_id = "manager-rpc-session", .turn_id = "manager-rpc-turn",
        .prompt_sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc", .prompt_length = 20,
        .retrieval_session_id = retrieved.session_id, .idempotency_key = "manager-rpc-task-begin"};
    char task_id[128];
    ASSERT_EQ(cbm_global_task_begin(global, &source, &begin, &report), CBM_STORE_OK);
    ASSERT_TRUE(ui_manager_report_id(report, "\"task_id\"", task_id, sizeof(task_id)));
    free(report); report = NULL;
    cbm_task_evidence_input_t evidence = {.task_id = task_id, .result_id = "manager-rpc-result",
        .result_hash = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd", .evidence_id = "manager-rpc-evidence",
        .evidence_hash = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", .evidence_trust = "external_verified",
        .evidence_source = "runtime", .idempotency_key = "manager-rpc-evidence-key"};
    ASSERT_EQ(cbm_global_task_record_evidence(global, &evidence, &report), CBM_STORE_OK);
    free(report); report = NULL;
    cbm_task_attribution_input_t attribution = {.memory_item_id = first.id, .state = "used", .evidence_id = evidence.evidence_id};
    cbm_task_complete_input_t complete = {.project = source.project_uuid, .task_id = task_id, .outcome = "completed",
        .idempotency_key = "manager-rpc-task-complete", .attributions = &attribution, .attribution_count = 1};
    ASSERT_EQ(cbm_global_task_complete(global, &complete, &report), CBM_STORE_OK);
    free(report); report = NULL;
    cbm_evolution_memory_input_t evolution = {.mode = "active", .task_id = task_id, .project_uuid = source.project_uuid,
        .memory_item_id = first.id, .operation = "archive", .evidence_grade = "A", .evidence_id = evidence.evidence_id,
        .idempotency_key = "manager-rpc-evolution", .isolated_write_allowed = 1};
    cbm_evolution_result_t evolution_result = {0};
    ASSERT_EQ(cbm_evolution_memory_state(global, &evolution, &evolution_result), CBM_STORE_OK);
    ASSERT_TRUE(evolution_result.wrote);
    cbm_evolution_result_free(&evolution_result);
    char evolution_event_id[128] = {0};
    sqlite3_stmt *event_stmt = NULL;
    ASSERT_EQ(sqlite3_prepare_v2(memory_db,
        "SELECT event_id FROM global_evolution_event WHERE idempotency_key=?1;", -1, &event_stmt, NULL), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_text(event_stmt, 1, "manager-rpc-evolution", -1, SQLITE_STATIC), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(event_stmt), SQLITE_ROW);
    snprintf(evolution_event_id, sizeof(evolution_event_id), "%s", sqlite3_column_text(event_stmt, 0));
    sqlite3_finalize(event_stmt);
    ASSERT_TRUE(evolution_event_id[0] != '\0');
    ASSERT_EQ(cbm_global_cross_project_edge(global, "manager-rpc-topology-edge", source.project_uuid, target.project_uuid,
                                             "derived_from", 900000, 800000, "active", 1, evolution_event_id,
                                             "manager-rpc-topology-key", &report), CBM_STORE_OK);
    free(report);
    cbm_global_retrieval_result_free(&retrieved);
    cbm_global_memory_close(global);

    ui_manager_fixture_t fixture;
    ASSERT_EQ(ui_manager_start(&fixture), 0);
    char host[64], origin[96], wrong_host[64], response[16384], body[1024];
    snprintf(host, sizeof(host), "127.0.0.1:%d", fixture.port);
    snprintf(origin, sizeof(origin), "http://%s", host);
    snprintf(wrong_host, sizeof(wrong_host), "localhost:%d", fixture.port);
    const char *overview = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_overview\",\"arguments\":{}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, "", overview, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 401);
    ASSERT_TRUE(ui_manager_rpc(fixture.port, wrong_host, origin, fixture.token, overview, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 401);
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, "http://evil.invalid", fixture.token, overview, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 403);
    ASSERT_NOT_NULL(strstr(response, "ORIGIN_REJECTED"));
    const char *non_manager_write = "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"tools/call\",\"params\":{\"name\":\"memory_task_complete\",\"arguments\":{}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, non_manager_write, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 403);
    ASSERT_NOT_NULL(strstr(response, "FEATURE_DISABLED"));

    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, overview, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-global-overview/v1"));
    ASSERT_NOT_NULL(strstr(response, source.project_uuid));
    const char *memory_page_one = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_memory\",\"arguments\":{\"limit\":1,\"cursor\":0}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, memory_page_one, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-global-memory/v1"));
    ASSERT_NOT_NULL(strstr(response, "next_cursor"));
    char manager_memory_page_one_id[128], manager_memory_page_two_id[128];
    ASSERT_TRUE(ui_manager_report_id(response, "\"memory_item_id\"", manager_memory_page_one_id,
                                     sizeof(manager_memory_page_one_id)));
    const char *memory_page_two = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_memory\",\"arguments\":{\"limit\":1,\"cursor\":1}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, memory_page_two, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_TRUE(ui_manager_report_id(response, "\"memory_item_id\"", manager_memory_page_two_id,
                                     sizeof(manager_memory_page_two_id)));
    ASSERT_TRUE(strcmp(manager_memory_page_one_id, manager_memory_page_two_id) != 0);
    const char *topology = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_topology\",\"arguments\":{\"limit\":10,\"cursor\":0}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, topology, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-global-topology/v1"));
    ASSERT_NOT_NULL(strstr(response, "manager-rpc-topology-edge"));
    const char *evolution_page = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_evolution\",\"arguments\":{\"limit\":10,\"cursor\":0}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, evolution_page, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-evolution/v1"));
    ASSERT_NOT_NULL(strstr(response, "archive"));
    snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_task_chain\",\"arguments\":{\"task_id\":\"%s\"}}}", task_id);
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, body, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-task-chain/v1"));
    ASSERT_NOT_NULL(strstr(response, "manager-rpc-evidence"));
    ASSERT_NOT_NULL(strstr(response, "attribution"));
    ASSERT_NOT_NULL(strstr(response, "evolution"));
    const char *drift = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_drift_preview\",\"arguments\":{}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, drift, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-drift-preview/v1"));
    ASSERT_NOT_NULL(strstr(response, "third_party_config_body_included"));
    const char *maintenance = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_maintenance_preview\",\"arguments\":{}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, maintenance, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-maintenance-preview/v1"));
    ASSERT_NOT_NULL(strstr(response, "isolated_mock"));
    const char *control = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_maintenance_control\",\"arguments\":{\"action\":\"dry_run\",\"scope\":\"isolated_mock\"}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, control, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "semantic-memory-manager-maintenance-control/v1"));
    ASSERT_NOT_NULL(strstr(response, "production_state_written"));
    ASSERT_NOT_NULL(strstr(response, "planned"));

    ui_manager_stop(&fixture);
    if (saved_cache) { cbm_setenv("CBM_CACHE_DIR", saved_cache, 1); free(saved_cache); } else cbm_unsetenv("CBM_CACHE_DIR");
    if (saved_mock) { cbm_setenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK", saved_mock, 1); free(saved_mock); } else cbm_unsetenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK");
    th_cleanup(cache);
    PASS();
}

TEST(stage14_manager_scale_contract) {
    enum { MEMORY_ITEMS = 50000, CROSS_PROJECT_EDGES = 250000, TASK_SESSIONS = 100000 };
    char *cache = th_mktempdir("cbm_stage14_manager_scale");
    ASSERT_NOT_NULL(cache);
    const char *existing_cache = getenv("CBM_CACHE_DIR");
    const char *existing_mock = getenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK");
    char *saved_cache = existing_cache ? strdup(existing_cache) : NULL;
    char *saved_mock = existing_mock ? strdup(existing_mock) : NULL;
    ASSERT_EQ(cbm_setenv("CBM_CACHE_DIR", cache, 1), 0);
    ASSERT_EQ(cbm_setenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK", "1", 1), 0);

    char memory_path[1024], graph_path[1024];
#ifdef _WIN32
    snprintf(memory_path, sizeof(memory_path), "%s\\__global__-memory.db", cache);
    snprintf(graph_path, sizeof(graph_path), "%s\\__global__-graph.db", cache);
#else
    snprintf(memory_path, sizeof(memory_path), "%s/__global__-memory.db", cache);
    snprintf(graph_path, sizeof(graph_path), "%s/__global__-graph.db", cache);
#endif
    cbm_global_memory_t *global = cbm_global_memory_open(memory_path, graph_path);
    ASSERT_NOT_NULL(global);
    cbm_project_resolution_t project = {0};
    ASSERT_EQ(cbm_project_resolve("H:\\Codex_H\\runtime-data\\stage14\\fixtures\\manager-scale", NULL, NULL, &project), 0);
    char *report = NULL;
    ASSERT_EQ(cbm_global_ensure_project(global, &project, "manager-scale-project", &report), CBM_STORE_OK);
    free(report);
    ASSERT_EQ(ui_stage14_seed_scale_fixture(global, project.project_uuid), SQLITE_OK);

    sqlite3 *memory = cbm_global_memory_db(global);
    sqlite3 *graph = cbm_global_graph_db(global);
    ASSERT_EQ(ui_stage14_count(memory, "SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NULL;"), MEMORY_ITEMS);
    ASSERT_EQ(ui_stage14_count(memory, "SELECT COUNT(*) FROM memory_task;"), TASK_SESSIONS);
    ASSERT_EQ(ui_stage14_count(memory, "SELECT COUNT(*) FROM global_task_workspace;"), TASK_SESSIONS);
    ASSERT_EQ(ui_stage14_count(graph, "SELECT COUNT(*) FROM global_cross_project_edge;"), CROSS_PROJECT_EDGES);
    ASSERT_EQ(ui_stage14_count(graph, "SELECT COUNT(*) FROM global_cross_project_edge_version;"), CROSS_PROJECT_EDGES);
    ASSERT_TRUE(ui_stage14_print_query_plan(memory, "memory-page",
        "EXPLAIN QUERY PLAN SELECT m.id FROM memory_item m WHERE m.deleted_at IS NULL ORDER BY m.updated_at DESC,m.id LIMIT 50 OFFSET 0;"));
    ASSERT_TRUE(ui_stage14_print_query_plan(graph, "topology-page",
        "EXPLAIN QUERY PLAN SELECT e.edge_id FROM global_cross_project_edge e ORDER BY e.edge_id LIMIT 50 OFFSET 0;"));
    cbm_global_memory_close(global);

    ui_manager_fixture_t fixture;
    ASSERT_EQ(ui_manager_start(&fixture), 0);
    char host[64], origin[96], response[65536], body[1024];
    snprintf(host, sizeof(host), "127.0.0.1:%d", fixture.port);
    snprintf(origin, sizeof(origin), "http://%s", host);
    const char *overview = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_overview\",\"arguments\":{}}}";
    const char *memory_page = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_memory\",\"arguments\":{\"limit\":50,\"cursor\":0}}}";
    const char *topology_page = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_topology\",\"arguments\":{\"limit\":50,\"cursor\":0}}}";
    const char *evolution_page = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_evolution\",\"arguments\":{\"limit\":50,\"cursor\":0}}}";
    const char *drift = "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_drift_preview\",\"arguments\":{}}}";
    const char *maintenance = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_maintenance_preview\",\"arguments\":{}}}";
    const char *control = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_maintenance_control\",\"arguments\":{\"action\":\"dry_run\",\"scope\":\"isolated_mock\"}}}";
    snprintf(body, sizeof(body), "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_task_chain\",\"arguments\":{\"task_id\":\"scale-task-000000\"}}}");

    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, overview, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "\"memory_count\":50000"));
    ASSERT_NOT_NULL(strstr(response, "\"cross_project_edge_count\":250000"));
    ASSERT_NOT_NULL(strstr(response, "\"task_count\":100000"));
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, memory_page, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "next_cursor"));
    char memory_page_one_id[128], memory_page_two_id[128];
    ASSERT_TRUE(ui_manager_report_id(response, "\"memory_item_id\"", memory_page_one_id, sizeof(memory_page_one_id)));
    const char *memory_page_next = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_memory\",\"arguments\":{\"limit\":50,\"cursor\":50}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, memory_page_next, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_NOT_NULL(strstr(response, "next_cursor"));
    ASSERT_TRUE(ui_manager_report_id(response, "\"memory_item_id\"", memory_page_two_id, sizeof(memory_page_two_id)));
    ASSERT_TRUE(strcmp(memory_page_one_id, memory_page_two_id) != 0);
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, topology_page, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    char topology_page_one_id[128], topology_page_two_id[128];
    ASSERT_TRUE(ui_manager_report_id(response, "\"edge_id\"", topology_page_one_id, sizeof(topology_page_one_id)));
    const char *topology_page_next = "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\",\"params\":{\"name\":\"manager_global_topology\",\"arguments\":{\"limit\":50,\"cursor\":50}}}";
    ASSERT_TRUE(ui_manager_rpc(fixture.port, host, origin, fixture.token, topology_page_next, response, sizeof(response)) > 0);
    ASSERT_EQ(ui_manager_status(response), 200);
    ASSERT_TRUE(ui_manager_report_id(response, "\"edge_id\"", topology_page_two_id, sizeof(topology_page_two_id)));
    ASSERT_TRUE(strcmp(topology_page_one_id, topology_page_two_id) != 0);

    int overview_p95, memory_p95, topology_p95, evolution_p95, task_chain_p95, drift_p95, maintenance_p95, control_p95;
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, overview, "global-overview", &overview_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, memory_page, "global-memory", &memory_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, topology_page, "global-topology", &topology_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, evolution_page, "manager-evolution", &evolution_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, body, "manager-task-chain", &task_chain_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, drift, "drift-preview", &drift_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, maintenance, "maintenance-preview", &maintenance_p95));
    ASSERT_TRUE(ui_stage14_measure_rpc(fixture.port, host, origin, fixture.token, control, "maintenance-control", &control_p95));
    ASSERT_LTE(overview_p95, 500); ASSERT_LTE(memory_p95, 500); ASSERT_LTE(topology_p95, 500);
    ASSERT_LTE(evolution_p95, 500); ASSERT_LTE(task_chain_p95, 500); ASSERT_LTE(drift_p95, 500);
    ASSERT_LTE(maintenance_p95, 500); ASSERT_LTE(control_p95, 500);
    fprintf(stderr, "stage14-scale-p95-ms overview=%d memory=%d topology=%d evolution=%d task_chain=%d drift=%d maintenance=%d control=%d\n",
            overview_p95, memory_p95, topology_p95, evolution_p95, task_chain_p95, drift_p95, maintenance_p95, control_p95);

    ui_manager_stop(&fixture);
    if (saved_cache) { cbm_setenv("CBM_CACHE_DIR", saved_cache, 1); free(saved_cache); } else cbm_unsetenv("CBM_CACHE_DIR");
    if (saved_mock) { cbm_setenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK", saved_mock, 1); free(saved_mock); } else cbm_unsetenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK");
    th_cleanup(cache);
    PASS();
}

SUITE(ui) {
    /* Config */
    RUN_TEST(config_load_defaults);
    RUN_TEST(config_save_and_reload);
    RUN_TEST(config_overwrite);
    RUN_TEST(config_corrupt_file);
    RUN_TEST(config_missing_fields);

    /* Embedded assets (stub) */
    RUN_TEST(embedded_lookup_not_found);
    RUN_TEST(embedded_stub_count);

    /* Layout engine */
    RUN_TEST(layout_empty_graph);
    RUN_TEST(layout_single_node);
    RUN_TEST(layout_two_connected);
    RUN_TEST(layout_respects_max_nodes);
    RUN_TEST(layout_clamps_render_cap_from_env);
    RUN_TEST(layout_deterministic);
    RUN_TEST(layout_to_json);
    RUN_TEST(layout_null_inputs);
    RUN_TEST(layout_dead_code_classification);
    RUN_TEST(layout_coincident_nodes_bounded);

    /* Stage12 manager */
    RUN_TEST(stage12_manager_live_security_contract);
    RUN_TEST(stage14_manager_rpc_global_contract);
    RUN_TEST(stage14_manager_scale_contract);
}
