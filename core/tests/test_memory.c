#include "store/store.h"
#include "store/embed.h"
#include "memory/edge_lifecycle.h"
#include "memory/concept_growth.h"
#include "foundation/platform.h"
#include "foundation/compat_thread.h"
#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST(name) static int test_##name(void)
#define ASSERT(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s:%d\n", __FILE__, __LINE__); return 1; } } while(0)
#define RUN(name) do { fprintf(stderr, "  %s... ", #name); fflush(stderr); int r = test_##name(); if (r) { fprintf(stderr, "FAIL\n"); fail++; } else { fprintf(stderr, "OK\n"); pass++; } total++; } while(0)

static int scalar_int(cbm_store_t *s, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int out = -1;
    if (sqlite3_prepare_v2(cbm_store_get_db(s), sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) out = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return out;
}

static int stage7_row_total(cbm_store_t *s) {
    return scalar_int(
        s,
        "SELECT (SELECT COUNT(*) FROM memory_task)+"
        "(SELECT COUNT(*) FROM memory_task_session)+"
        "(SELECT COUNT(*) FROM memory_task_result)+"
        "(SELECT COUNT(*) FROM memory_evidence)+"
        "(SELECT COUNT(*) FROM feedback_event)+"
        "(SELECT COUNT(*) FROM feedback_attribution)+"
        "(SELECT COUNT(*) FROM plasticity_audit_event);");
}

static void stage7_set_env(const char *name, const char *value) {
#ifdef _WIN32
    char assignment[128];
    snprintf(assignment, sizeof(assignment), "%s=%s", name, value ? value : "");
    (void)_putenv(assignment);
#else
    if (value && value[0]) {
        (void)setenv(name, value, 1);
    } else {
        (void)unsetenv(name);
    }
#endif
}

static void stage7_temp_db_path(char *path, size_t path_size, const char *label, int ordinal) {
    const char *tmp = getenv("TMP");
    if (!tmp || !tmp[0]) tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(path, path_size, "%s/cbm_stage7_%s_%lld_%d.db", tmp, label,
             (long long)time(NULL), ordinal);
}

static void stage7_remove_db_files(const char *path) {
    char sidecar[640];
    (void)remove(path);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", path);
    (void)remove(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", path);
    (void)remove(sidecar);
}

static int insert_test_memory_edge(cbm_store_t *s, const char *edge_id, const char *src_id,
                                   const char *dst_id, const char *type) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO memory_edge(id,src_id,dst_id,type,weight,origin,confidence,created_at) "
        "VALUES(?1,?2,?3,?4,1.0,'unit_test',0.9,1);";
    if (sqlite3_prepare_v2(cbm_store_get_db(s), sql, -1, &stmt, NULL) != SQLITE_OK)
        return SQLITE_ERROR;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, src_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, dst_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, type, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(stmt);
    return rc;
}

#define STAGE9_TEST_PROJECT "stage9-fixture-edge-lifecycle-v1"
#define STAGE9_TEST_AS_OF_MS INT64_C(1893456000000)
#define STAGE9_TEST_DAY_MS INT64_C(86400000)

static int stage9_test_exec(cbm_store_t *s, const char *sql) {
    return sqlite3_exec(cbm_store_get_db(s), sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : 1;
}

static int stage9_test_add_edge(cbm_store_t *s, const char *edge_id, const char *type,
                                int64_t last_signal_ms, double confidence, int pheromone_ppm,
                                int success_count, int failure_count, const char *src_kind,
                                const char *dst_project, double importance, double reusability) {
    char src_id[160], dst_id[160], src_content[220], dst_content[220];
    snprintf(src_id, sizeof(src_id), "%s-src", edge_id);
    snprintf(dst_id, sizeof(dst_id), "%s-dst", edge_id);
    snprintf(src_content, sizeof(src_content), "STAGE9_DIRECT_RECALL_%s_SOURCE", edge_id);
    snprintf(dst_content, sizeof(dst_content), "STAGE9_DIRECT_RECALL_%s_TARGET", edge_id);
    if (strcmp(edge_id, "edge-used-in-to-cold") == 0)
        snprintf(src_content, sizeof(src_content), "STAGE9ARCHIVEDENDPOINTTOKEN");
    cbm_memory_item_t src = {0};
    src.id = src_id;
    src.kind = src_kind;
    src.layer = "semantic";
    src.content = src_content;
    src.scope_project = STAGE9_TEST_PROJECT;
    src.entity_key = edge_id;
    src.status = "active";
    src.importance = importance;
    src.confidence = confidence;
    src.reusability = reusability;
    src.specificity = 0.5;
    src.created_at = last_signal_ms;
    src.updated_at = last_signal_ms;
    cbm_memory_item_t dst = src;
    dst.id = dst_id;
    dst.kind = "fact";
    dst.content = dst_content;
    dst.scope_project = dst_project ? dst_project : STAGE9_TEST_PROJECT;
    dst.entity_key = NULL;
    if (cbm_store_memory_append_candidate(s, &src, NULL) != CBM_STORE_OK ||
        cbm_store_memory_append_candidate(s, &dst, NULL) != CBM_STORE_OK) {
        fprintf(stderr, "stage9 fixture item failed for %s: %s\n", edge_id,
                sqlite3_errmsg(cbm_store_get_db(s)));
        return 1;
    }
    if (cbm_store_memory_index_candidate(s, &src, src_id, NULL) != CBM_STORE_OK ||
        cbm_store_memory_index_candidate(s, &dst, dst_id, NULL) != CBM_STORE_OK) {
        fprintf(stderr, "stage9 fixture FTS failed for %s: %s\n", edge_id,
                sqlite3_errmsg(cbm_store_get_db(s)));
        return 1;
    }

    sqlite3_stmt *stmt = NULL;
    const char *edge_sql =
        "INSERT INTO memory_edge(id,src_id,dst_id,type,weight,origin,confidence,created_at) "
        "VALUES(?1,?2,?3,?4,1.0,'stage9_fixture',?5,?6);";
    if (sqlite3_prepare_v2(cbm_store_get_db(s), edge_sql, -1, &stmt, NULL) != SQLITE_OK)
        return 1;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, src_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, dst_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 5, confidence);
    sqlite3_bind_int64(stmt, 6, last_signal_ms);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : 1;
    sqlite3_finalize(stmt);
    if (rc) {
        fprintf(stderr, "stage9 fixture edge failed for %s: %s\n", edge_id,
                sqlite3_errmsg(cbm_store_get_db(s)));
        return rc;
    }

    const char *state_sql =
        "INSERT INTO plastic_edge_state(edge_id,pheromone_ppm,success_count,failure_count,"
        "rebuilt_at) VALUES(?1,?2,?3,?4,datetime(?5/1000,'unixepoch'));";
    if (sqlite3_prepare_v2(cbm_store_get_db(s), state_sql, -1, &stmt, NULL) != SQLITE_OK)
        return 1;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, pheromone_ppm);
    sqlite3_bind_int(stmt, 3, success_count);
    sqlite3_bind_int(stmt, 4, failure_count);
    sqlite3_bind_int64(stmt, 5, last_signal_ms);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : 1;
    sqlite3_finalize(stmt);
    if (rc)
        fprintf(stderr, "stage9 fixture plastic state failed for %s: %s\n", edge_id,
                sqlite3_errmsg(cbm_store_get_db(s)));
    return rc;
}

static cbm_store_t *stage9_test_store(void) {
    cbm_store_t *s = cbm_store_open_memory();
    if (!s) return NULL;
    if (stage9_test_exec(s,
            "CREATE TABLE plastic_edge_state(edge_id TEXT PRIMARY KEY,pheromone_ppm INTEGER "
            "NOT NULL,success_count INTEGER NOT NULL,failure_count INTEGER NOT NULL,rebuilt_at "
            "TEXT NOT NULL);"
            "INSERT INTO memory_task(task_id,project,task_type,created_at) VALUES("
            "'stage9-task-v1','stage9-fixture-edge-lifecycle-v1','test','2030-01-01T00:00:00Z');"
            "INSERT INTO memory_task_result(result_id,task_id,result_type,status,result_ref,"
            "result_hash,recorded_at) VALUES('stage9-result-v1','stage9-task-v1','test',"
            "'succeeded','test:stage9','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaa','2030-01-01T00:00:00Z');"
            "INSERT INTO memory_evidence(evidence_id,task_id,result_id,trust_class,evidence_state,"
            "source_type,evidence_ref,evidence_hash,created_at) VALUES('stage9-evidence-v1',"
            "'stage9-task-v1','stage9-result-v1','external_verified','valid','test','test:stage9',"
            "'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb',"
            "'2030-01-01T00:00:00Z');") != 0) {
        fprintf(stderr, "stage9 fixture baseline failed: %s\n", sqlite3_errmsg(cbm_store_get_db(s)));
        cbm_store_close(s);
        return NULL;
    }
    const int64_t day = STAGE9_TEST_DAY_MS;
    if (stage9_test_add_edge(s, "edge-derived-recent-success", "derived_from",
                             STAGE9_TEST_AS_OF_MS - day, 1.0, 1045000, 1, 0, "fact", NULL,
                             0.6, 0.6) ||
        stage9_test_add_edge(s, "edge-used-in-to-cold", "used_in",
                             STAGE9_TEST_AS_OF_MS - 400 * day, 0.2, 300000, 0, 3, "fact", NULL,
                             0.3, 0.3) ||
        stage9_test_add_edge(s, "edge-supports-to-cold", "supports",
                             STAGE9_TEST_AS_OF_MS - 800 * day, 0.1, 200000, 0, 4, "fact", NULL,
                             0.2, 0.2) ||
        stage9_test_add_edge(s, "edge-contradicts-protected", "contradicts",
                             STAGE9_TEST_AS_OF_MS - 1000 * day, 0.1, 0, 0, 9, "fact", NULL,
                             0.1, 0.1) ||
        stage9_test_add_edge(s, "edge-supersedes-protected", "supersedes",
                             STAGE9_TEST_AS_OF_MS - 1000 * day, 0.1, 0, 0, 9, "fact", NULL,
                             0.1, 0.1) ||
        stage9_test_add_edge(s, "edge-constraint-protected", "supports",
                             STAGE9_TEST_AS_OF_MS - 1000 * day, 0.1, 0, 0, 9, "constraint", NULL,
                             0.5, 0.5) ||
        stage9_test_add_edge(s, "edge-high-value-protected", "used_in",
                             STAGE9_TEST_AS_OF_MS - 1000 * day, 0.1, 0, 0, 9, "fact", NULL,
                             0.95, 0.1) ||
        stage9_test_add_edge(s, "edge-low-traffic-no-evidence", "derived_from",
                             STAGE9_TEST_AS_OF_MS - 1000 * day, 0.0, 0, 0, 0, "fact", NULL,
                             0.1, 0.1) ||
        stage9_test_add_edge(s, "edge-cross-project", "used_in",
                             STAGE9_TEST_AS_OF_MS - 1000 * day, 0.0, 0, 0, 9, "fact",
                             "other-project", 0.1, 0.1) ||
        stage9_test_add_edge(s, "edge-boundary-exact", "used_in",
                             STAGE9_TEST_AS_OF_MS - 90 * day, 0.0, 0, 0, 2, "fact", NULL,
                             0.1, 0.1) ||
        stage9_test_add_edge(s, "edge-boundary-before", "used_in",
                             STAGE9_TEST_AS_OF_MS - 90 * day + 1, 0.0, 0, 0, 2, "fact", NULL,
                             0.1, 0.1) ||
        stage9_test_add_edge(s, "edge-disabled-manual", "used_in",
                             STAGE9_TEST_AS_OF_MS - day, 0.9, 1000000, 3, 0, "fact", NULL,
                             0.5, 0.5)) {
        cbm_store_close(s);
        return NULL;
    }
    cbm_memory_event_t event = {0};
    event.id = "stage9-event-v1";
    event.type = "stage9.fixture";
    event.source = "unit-test";
    event.timestamp_ms = STAGE9_TEST_AS_OF_MS;
    event.project = STAGE9_TEST_PROJECT;
    event.payload = "fixture";
    if (cbm_store_memory_append_event(s, &event, NULL) != CBM_STORE_OK) {
        cbm_store_close(s);
        return NULL;
    }
    return s;
}

static cbm_edge_lifecycle_input_t stage9_test_input(const char *mode, const char *run_id,
                                                     int64_t as_of_ms) {
    cbm_edge_lifecycle_input_t input = {0};
    input.project = STAGE9_TEST_PROJECT;
    input.mode = mode;
    input.run_id = run_id;
    input.as_of_ms = as_of_ms;
    input.algorithm_version = CBM_STAGE9_ALGORITHM_VERSION;
    input.policy_sha256 = CBM_STAGE9_POLICY_SHA256;
    input.policy_version = CBM_STAGE9_POLICY_VERSION;
    input.config_version = CBM_STAGE9_CONFIG_VERSION;
    return input;
}

static int stage9_test_write_manifest(const char *report_json, const char *run_id,
                                      const char *operation, const char *path,
                                      char out_sha256[65]) {
    yyjson_doc *report_doc = yyjson_read(report_json, strlen(report_json), 0);
    yyjson_val *report = report_doc ? yyjson_doc_get_root(report_doc) : NULL;
    yyjson_val *decisions = report ? yyjson_obj_get(report, "decisions") : NULL;
    if (!report || !yyjson_is_obj(report) || !decisions || !yyjson_is_arr(decisions)) {
        yyjson_doc_free(report_doc);
        return 1;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *edges = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !edges) {
        yyjson_mut_doc_free(doc);
        yyjson_doc_free(report_doc);
        return 1;
    }
    yyjson_mut_doc_set_root(doc, root);
    size_t index, max;
    yyjson_val *decision;
    yyjson_arr_foreach(decisions, index, max, decision) {
        yyjson_val *edge_id = yyjson_obj_get(decision, "edge_id");
        if (!edge_id || !yyjson_is_str(edge_id)) {
            yyjson_mut_doc_free(doc);
            yyjson_doc_free(report_doc);
            return 1;
        }
        yyjson_mut_arr_add_strcpy(doc, edges, yyjson_get_str(edge_id));
    }
    yyjson_mut_obj_add_str(doc, root, "schema", "stage9-production-canary-manifest/v1");
    yyjson_mut_obj_add_strcpy(doc, root, "project",
                              yyjson_get_str(yyjson_obj_get(report, "project")));
    yyjson_mut_obj_add_str(doc, root, "run_id", run_id);
    yyjson_mut_obj_add_sint(doc, root, "as_of_ms",
                            yyjson_get_sint(yyjson_obj_get(report, "as_of_ms")));
    yyjson_mut_obj_add_strcpy(doc, root, "algorithm_version",
                              yyjson_get_str(yyjson_obj_get(report, "algorithm_version")));
    yyjson_mut_obj_add_int(doc, root, "policy_version",
                           (int)yyjson_get_sint(yyjson_obj_get(report, "policy_version")));
    yyjson_mut_obj_add_int(doc, root, "config_version",
                           (int)yyjson_get_sint(yyjson_obj_get(report, "config_version")));
    yyjson_mut_obj_add_strcpy(doc, root, "policy_sha256",
                              yyjson_get_str(yyjson_obj_get(report, "policy_sha256")));
    yyjson_mut_obj_add_val(doc, root, "edge_ids", edges);
    yyjson_mut_obj_add_strcpy(doc, root, "decision_set_sha256",
                              yyjson_get_str(yyjson_obj_get(report, "decision_set_sha256")));
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    size_t size = 0;
    char *json = yyjson_mut_write(doc, 0, &size);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(report_doc);
    if (!json) return 1;
    FILE *file = fopen(path, "wb");
    int rc = !file || fwrite(json, 1, size, file) != size;
    if (file) fclose(file);
    if (!rc && cbm_stage7_sha256_hex(json, size, out_sha256) != CBM_STORE_OK) rc = 1;
    free(json);
    return rc;
}

TEST(stage9_migration_is_additive_idempotent_and_atomic) {
    const int failpoints[] = {1, 11, 22};
    for (size_t i = 0; i < sizeof(failpoints) / sizeof(failpoints[0]); i++) {
        cbm_store_t *failed = cbm_store_open_memory();
        ASSERT(failed != NULL);
        ASSERT(stage9_test_exec(failed, "CREATE TABLE IF NOT EXISTS memory_evidence("
                                        "evidence_id TEXT PRIMARY KEY);") == 0);
        char value[16];
        snprintf(value, sizeof(value), "%d", failpoints[i]);
        stage7_set_env("CBM_STAGE9_MIGRATION_FAIL_AFTER", value);
        ASSERT(cbm_store_memory_stage9_migrate(failed) != CBM_STORE_OK);
        ASSERT(cbm_store_memory_stage9_object_count(failed) == 0);
        cbm_store_close(failed);
    }
    stage7_set_env("CBM_STAGE9_MIGRATION_FAIL_AFTER", NULL);

    cbm_store_t *s = stage9_test_store();
    ASSERT(s != NULL);
    int user_version = scalar_int(s, "PRAGMA user_version;");
    int edge_count = scalar_int(s, "SELECT COUNT(*) FROM memory_edge;");
    int item_count = scalar_int(s, "SELECT COUNT(*) FROM memory_item;");
    int event_count = scalar_int(s, "SELECT COUNT(*) FROM memory_event;");
    int evidence_count = scalar_int(s, "SELECT COUNT(*) FROM memory_evidence;");
    int stage8_count = scalar_int(s, "SELECT COUNT(*) FROM plastic_edge_state;");
    ASSERT(cbm_store_memory_stage9_object_count(s) == 0);
    ASSERT(cbm_store_memory_stage9_migrate(s) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_stage9_object_count(s) == 22);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM stage9_component_ledger WHERE component="
                         "'stage9_edge_lifecycle' AND version=1 AND applied_at NOT LIKE "
                         "'2030-01-01%';") == 1);
    ASSERT(cbm_store_memory_stage9_migrate(s) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_stage9_object_count(s) == 22);
    ASSERT(scalar_int(s, "PRAGMA user_version;") == user_version);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM plastic_edge_state;") == stage8_count);

    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "DELETE FROM memory_edge WHERE id='edge-used-in-to-cold';",
                        NULL, NULL, NULL) != SQLITE_OK);
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "DELETE FROM memory_item WHERE id='edge-used-in-to-cold-src';",
                        NULL, NULL, NULL) != SQLITE_OK);
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "DELETE FROM memory_event WHERE id='stage9-event-v1';",
                        NULL, NULL, NULL) != SQLITE_OK);
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "DELETE FROM memory_evidence WHERE evidence_id='stage9-evidence-v1';",
                        NULL, NULL, NULL) != SQLITE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge;") == edge_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item;") == item_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event;") == event_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_evidence;") == evidence_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM pragma_quick_check WHERE quick_check='ok';") ==
           1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM pragma_foreign_key_check;") == 0);
    cbm_store_close(s);
    return 0;
}

TEST(stage9_shadow_dry_run_policy_and_zero_write) {
    cbm_store_t *s = stage9_test_store();
    ASSERT(s != NULL);
    int items_before = scalar_int(s, "SELECT COUNT(*) FROM memory_item;");
    int edges_before = scalar_int(s, "SELECT COUNT(*) FROM memory_edge;");
    int stage8_before = scalar_int(s, "SELECT COUNT(*) FROM plastic_edge_state;");

    cbm_edge_lifecycle_result_t off = {0};
    cbm_edge_lifecycle_input_t off_input =
        stage9_test_input("off", "stage9-off-v1", STAGE9_TEST_AS_OF_MS);
    ASSERT(cbm_store_memory_edge_maintenance(s, &off_input, &off) == CBM_STORE_OK);
    ASSERT(off.decision_count == 0 && off.transition_count == 0);
    ASSERT(strstr(off.report_json, "\"long_term_state_written\":false") != NULL);

    cbm_edge_lifecycle_input_t shadow_input =
        stage9_test_input("shadow", "stage9-shadow-v1", STAGE9_TEST_AS_OF_MS);
    cbm_edge_lifecycle_result_t shadow1 = {0}, shadow2 = {0}, shadow3 = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &shadow_input, &shadow1) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_edge_maintenance(s, &shadow_input, &shadow2) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_edge_maintenance(s, &shadow_input, &shadow3) == CBM_STORE_OK);
    ASSERT(strcmp(shadow1.report_json, shadow2.report_json) == 0);
    ASSERT(strcmp(shadow2.report_json, shadow3.report_json) == 0);
    ASSERT(shadow1.decision_count == 12);
    ASSERT(shadow1.transition_count == 3);

    cbm_edge_lifecycle_input_t dry_input =
        stage9_test_input("dry_run", "stage9-dry-v1", STAGE9_TEST_AS_OF_MS);
    cbm_edge_lifecycle_result_t dry = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &dry_input, &dry) == CBM_STORE_OK);
    ASSERT(dry.decision_count == shadow1.decision_count);
    ASSERT(dry.transition_count == shadow1.transition_count);
    ASSERT(strstr(dry.report_json, "TRANSITION_ACTIVE_TO_COLD") != NULL);
    ASSERT(strstr(dry.report_json, "PROTECTED_RELATION_TYPE") != NULL);
    ASSERT(strstr(dry.report_json, "PROTECTED_ENDPOINT_CONSTRAINT") != NULL);
    ASSERT(strstr(dry.report_json, "PROTECTED_HIGH_VALUE_ENDPOINT") != NULL);
    ASSERT(strstr(dry.report_json, "CROSS_SCOPE_BLOCKED") != NULL);
    ASSERT(strstr(dry.report_json, "KEEP_INSUFFICIENT_FAILURE_EVIDENCE") != NULL);

    yyjson_doc *doc = yyjson_read(dry.report_json, strlen(dry.report_json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *decisions = root ? yyjson_obj_get(root, "decisions") : NULL;
    bool exact_cold = false, before_active = false, low_traffic_active = false;
    bool contradicts_protected = false, supersedes_protected = false;
    size_t index, max;
    yyjson_val *decision;
    yyjson_arr_foreach(decisions, index, max, decision) {
        const char *edge_id = yyjson_get_str(yyjson_obj_get(decision, "edge_id"));
        const char *to_state = yyjson_get_str(yyjson_obj_get(decision, "to_state"));
        bool protected_edge = yyjson_get_bool(yyjson_obj_get(decision, "protected"));
        if (strcmp(edge_id, "edge-boundary-exact") == 0)
            exact_cold = strcmp(to_state, "cold") == 0;
        if (strcmp(edge_id, "edge-boundary-before") == 0)
            before_active = strcmp(to_state, "active") == 0;
        if (strcmp(edge_id, "edge-low-traffic-no-evidence") == 0)
            low_traffic_active = strcmp(to_state, "active") == 0;
        if (strcmp(edge_id, "edge-contradicts-protected") == 0)
            contradicts_protected = protected_edge && strcmp(to_state, "active") == 0;
        if (strcmp(edge_id, "edge-supersedes-protected") == 0)
            supersedes_protected = protected_edge && strcmp(to_state, "active") == 0;
    }
    yyjson_doc_free(doc);
    ASSERT(exact_cold && before_active && low_traffic_active);
    ASSERT(contradicts_protected && supersedes_protected);
    ASSERT(cbm_store_memory_stage9_object_count(s) == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item;") == items_before);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge;") == edges_before);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM plastic_edge_state;") == stage8_before);

    cbm_store_memory_edge_lifecycle_result_free(&dry);
    cbm_store_memory_edge_lifecycle_result_free(&shadow3);
    cbm_store_memory_edge_lifecycle_result_free(&shadow2);
    cbm_store_memory_edge_lifecycle_result_free(&shadow1);
    cbm_store_memory_edge_lifecycle_result_free(&off);
    cbm_store_close(s);
    return 0;
}

TEST(stage9_active_replay_archive_restore_and_direct_recall) {
    cbm_store_t *s = stage9_test_store();
    ASSERT(s != NULL);
    ASSERT(cbm_store_memory_stage9_migrate(s) == CBM_STORE_OK);
    char manifest1[640], manifest2[640], manifest3[640];
    stage7_temp_db_path(manifest1, sizeof(manifest1), "stage9_manifest_active", 1);
    stage7_temp_db_path(manifest2, sizeof(manifest2), "stage9_manifest_archive", 2);
    stage7_temp_db_path(manifest3, sizeof(manifest3), "stage9_manifest_restore", 3);

    cbm_edge_lifecycle_input_t dry1 =
        stage9_test_input("dry_run", "stage9-active-v1", STAGE9_TEST_AS_OF_MS);
    cbm_edge_lifecycle_result_t preview1 = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &dry1, &preview1) == CBM_STORE_OK);
    char manifest1_hash[65];
    ASSERT(stage9_test_write_manifest(preview1.report_json, dry1.run_id, "maintenance",
                                      manifest1, manifest1_hash) == 0);
    cbm_edge_lifecycle_input_t active1 = dry1;
    active1.mode = "active";
    active1.manifest_path = manifest1;
    active1.manifest_sha256 = manifest1_hash;
    cbm_edge_lifecycle_result_t blocked = {0};
    stage7_set_env("CBM_STAGE9_ACTIVE_FIXTURE", NULL);
    ASSERT(cbm_store_memory_edge_maintenance(s, &active1, &blocked) == CBM_STORE_REJECTED);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_run;") == 0);

    stage7_set_env("CBM_STAGE9_ACTIVE_FIXTURE", "1");
    cbm_edge_lifecycle_result_t applied1 = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &active1, &applied1) == CBM_STORE_OK);
    ASSERT(applied1.recorded_count == 1 && applied1.transition_count == 3);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_run;") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_decision;") == 12);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_state;") == 12);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_audit_event;") == 12);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_state WHERE edge_id="
                         "'edge-used-in-to-cold' AND lifecycle_state='cold';") == 1);

    cbm_edge_lifecycle_result_t replay1 = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &active1, &replay1) == CBM_STORE_OK);
    ASSERT(replay1.recorded_count == 0 && replay1.replayed_count == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_run;") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_audit_event;") == 12);
    cbm_edge_lifecycle_input_t conflict = active1;
    conflict.as_of_ms++;
    cbm_edge_lifecycle_result_t conflict_result = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &conflict, &conflict_result) ==
           CBM_STORE_IDEMPOTENCY_CONFLICT);

    const int64_t archive_as_of = STAGE9_TEST_AS_OF_MS + 90 * STAGE9_TEST_DAY_MS;
    cbm_edge_lifecycle_input_t dry2 =
        stage9_test_input("dry_run", "stage9-archive-v1", archive_as_of);
    cbm_edge_lifecycle_result_t preview2 = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &dry2, &preview2) == CBM_STORE_OK);
    char manifest2_hash[65];
    ASSERT(stage9_test_write_manifest(preview2.report_json, dry2.run_id, "maintenance",
                                      manifest2, manifest2_hash) == 0);
    cbm_edge_lifecycle_input_t active2 = dry2;
    active2.mode = "active";
    active2.manifest_path = manifest2;
    active2.manifest_sha256 = manifest2_hash;
    cbm_edge_lifecycle_result_t applied2 = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &active2, &applied2) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_state WHERE edge_id="
                         "'edge-used-in-to-cold' AND lifecycle_state='archived';") == 1);
    ASSERT(!cbm_store_memory_edge_allows_propagation(s, "edge-used-in-to-cold"));

    cbm_memory_query_t query = {0};
    query.project = STAGE9_TEST_PROJECT;
    query.query = "STAGE9ARCHIVEDENDPOINTTOKEN";
    query.limit = 50;
    cbm_memory_result_t recall = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &recall) == CBM_STORE_OK);
    bool endpoint_recalled = false;
    for (int i = 0; i < recall.count; i++) {
        if (strcmp(recall.items[i].id, "edge-used-in-to-cold-src") == 0)
            endpoint_recalled = true;
    }
    ASSERT(endpoint_recalled);
    cbm_store_memory_result_free(&recall);
    cbm_memory_query_t structured_query = {0};
    structured_query.project = STAGE9_TEST_PROJECT;
    structured_query.entity_key = "edge-used-in-to-cold";
    structured_query.limit = 5;
    cbm_memory_result_t structured_recall = {0};
    ASSERT(cbm_store_memory_retrieve(s, &structured_query, &structured_recall) == CBM_STORE_OK);
    ASSERT(structured_recall.count >= 1);
    ASSERT(strcmp(structured_recall.items[0].id, "edge-used-in-to-cold-src") == 0);
    cbm_store_memory_result_free(&structured_recall);

    const char *restore_ids[] = {"edge-used-in-to-cold"};
    cbm_edge_lifecycle_restore_input_t restore = {0};
    restore.edge_ids = restore_ids;
    restore.edge_count = 1;
    restore.lifecycle = stage9_test_input("dry_run", "stage9-restore-v1",
                                          archive_as_of + STAGE9_TEST_DAY_MS);
    cbm_edge_lifecycle_result_t restore_preview = {0};
    ASSERT(cbm_store_memory_edge_restore(s, &restore, &restore_preview) == CBM_STORE_OK);
    ASSERT(restore_preview.transition_count == 1);
    ASSERT(strstr(restore_preview.report_json, "RESTORE_TO_ACTIVE") != NULL);
    char manifest3_hash[65];
    ASSERT(stage9_test_write_manifest(restore_preview.report_json, restore.lifecycle.run_id,
                                      "restore", manifest3, manifest3_hash) == 0);
    restore.lifecycle.mode = "active";
    restore.lifecycle.manifest_path = manifest3;
    restore.lifecycle.manifest_sha256 = manifest3_hash;
    cbm_edge_lifecycle_result_t restored = {0}, restore_replay = {0};
    ASSERT(cbm_store_memory_edge_restore(s, &restore, &restored) == CBM_STORE_OK);
    ASSERT(restored.recorded_count == 1 && restored.transition_count == 1);
    ASSERT(cbm_store_memory_edge_allows_propagation(s, "edge-used-in-to-cold"));
    ASSERT(cbm_store_memory_edge_restore(s, &restore, &restore_replay) == CBM_STORE_OK);
    ASSERT(restore_replay.recorded_count == 0 && restore_replay.replayed_count == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_run;") == 3);
    int audit_count = 0;
    ASSERT(cbm_store_memory_stage9_audit_verify(s, &audit_count) == CBM_STORE_OK);
    ASSERT(audit_count >= 15);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_state s LEFT JOIN "
                         "edge_lifecycle_audit_event a ON a.event_id=s.last_audit_event_id "
                         "WHERE a.event_id IS NULL OR length(s.state_sha256)!=64;") == 0);

    ASSERT(stage9_test_exec(s, "UPDATE edge_lifecycle_state SET lifecycle_state='disabled',"
                               "protection_reason='manual_safety_block' WHERE edge_id="
                               "'edge-disabled-manual';") == 0);
    cbm_edge_lifecycle_input_t disabled_shadow =
        stage9_test_input("shadow", "stage9-disabled-v1",
                          archive_as_of + 2 * STAGE9_TEST_DAY_MS);
    cbm_edge_lifecycle_result_t disabled_report = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &disabled_shadow, &disabled_report) ==
           CBM_STORE_OK);
    ASSERT(strstr(disabled_report.report_json, "DISABLED_MANUAL_BLOCK") != NULL);
    const char *disabled_ids[] = {"edge-disabled-manual"};
    cbm_edge_lifecycle_restore_input_t disabled_restore = {0};
    disabled_restore.edge_ids = disabled_ids;
    disabled_restore.edge_count = 1;
    disabled_restore.lifecycle = disabled_shadow;
    cbm_edge_lifecycle_result_t disabled_restore_report = {0};
    ASSERT(cbm_store_memory_edge_restore(s, &disabled_restore, &disabled_restore_report) ==
           CBM_STORE_OK);
    ASSERT(disabled_restore_report.transition_count == 0);
    ASSERT(strstr(disabled_restore_report.report_json, "RESTORE_DISABLED_REJECTED") != NULL);

    cbm_store_memory_edge_lifecycle_result_free(&disabled_restore_report);
    cbm_store_memory_edge_lifecycle_result_free(&disabled_report);
    cbm_store_memory_edge_lifecycle_result_free(&restore_replay);
    cbm_store_memory_edge_lifecycle_result_free(&restored);
    cbm_store_memory_edge_lifecycle_result_free(&restore_preview);
    cbm_store_memory_edge_lifecycle_result_free(&applied2);
    cbm_store_memory_edge_lifecycle_result_free(&preview2);
    cbm_store_memory_edge_lifecycle_result_free(&replay1);
    cbm_store_memory_edge_lifecycle_result_free(&applied1);
    cbm_store_memory_edge_lifecycle_result_free(&blocked);
    cbm_store_memory_edge_lifecycle_result_free(&preview1);
    stage7_set_env("CBM_STAGE9_ACTIVE_FIXTURE", NULL);
    remove(manifest3);
    remove(manifest2);
    remove(manifest1);
    cbm_store_close(s);
    return 0;
}

TEST(stage9_active_fault_rolls_back_and_retry_succeeds) {
    cbm_store_t *s = stage9_test_store();
    ASSERT(s != NULL);
    ASSERT(cbm_store_memory_stage9_migrate(s) == CBM_STORE_OK);
    cbm_edge_lifecycle_input_t dry =
        stage9_test_input("dry_run", "stage9-fault-v1", STAGE9_TEST_AS_OF_MS);
    cbm_edge_lifecycle_result_t preview = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &dry, &preview) == CBM_STORE_OK);
    char manifest[640], manifest_hash[65];
    stage7_temp_db_path(manifest, sizeof(manifest), "stage9_manifest_fault", 4);
    ASSERT(stage9_test_write_manifest(preview.report_json, dry.run_id, "maintenance", manifest,
                                      manifest_hash) == 0);
    cbm_edge_lifecycle_input_t active = dry;
    active.mode = "active";
    active.manifest_path = manifest;
    active.manifest_sha256 = manifest_hash;
    stage7_set_env("CBM_STAGE9_ACTIVE_FIXTURE", "1");
    stage7_set_env("CBM_STAGE9_ACTIVE_FAIL_AFTER", "2");
    cbm_edge_lifecycle_result_t failed = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &active, &failed) != CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_run;") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_decision;") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_audit_event;") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_lifecycle_state;") == 0);
    stage7_set_env("CBM_STAGE9_ACTIVE_FAIL_AFTER", NULL);
    cbm_edge_lifecycle_result_t retried = {0};
    ASSERT(cbm_store_memory_edge_maintenance(s, &active, &retried) == CBM_STORE_OK);
    ASSERT(retried.recorded_count == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_maintenance_run;") == 1);
    cbm_store_memory_edge_lifecycle_result_free(&retried);
    cbm_store_memory_edge_lifecycle_result_free(&failed);
    cbm_store_memory_edge_lifecycle_result_free(&preview);
    stage7_set_env("CBM_STAGE9_ACTIVE_FIXTURE", NULL);
    remove(manifest);
    cbm_store_close(s);
    return 0;
}

TEST(memory_derived_from_link_is_event_bound_and_atomic) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);

    cbm_memory_item_t target = {0};
    target.id = "derived-target";
    target.kind = "fact";
    target.layer = "semantic";
    target.content = "Existing target memory";
    target.scope_project = "test-proj";
    target.status = "active";
    ASSERT(cbm_store_memory_append_candidate(s, &target, NULL) == CBM_STORE_OK);

    cbm_memory_event_t event = {0};
    event.id = "derived-source-event";
    event.type = "memory.event";
    event.source = "unit-test";
    event.project = "test-proj";
    event.payload = "source payload";
    cbm_memory_item_t source = {0};
    source.id = "derived-source";
    source.kind = "fact";
    source.layer = "semantic";
    source.content = "New derived memory";
    source.scope_project = "test-proj";
    source.status = "candidate";
    source.source_event_ids = "[\"derived-source-event\"]";

    ASSERT(cbm_store_begin(s) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_event(s, &event, NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_candidate(s, &source, NULL) == CBM_STORE_OK);
    char *edge_id = NULL;
    ASSERT(cbm_store_memory_link_derived_from(s, source.id, target.id, "test-proj", event.id,
                                              &edge_id) == CBM_STORE_OK);
    ASSERT(edge_id != NULL);
    ASSERT(cbm_store_commit(s) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE src_id='derived-source' "
                         "AND dst_id='derived-target' AND type='derived_from' "
                         "AND confidence=1.0 AND origin='derived-source-event'") == 1);
    free(edge_id);

    int event_count = scalar_int(s, "SELECT COUNT(*) FROM memory_event");
    int item_count = scalar_int(s, "SELECT COUNT(*) FROM memory_item");
    int edge_count = scalar_int(s, "SELECT COUNT(*) FROM memory_edge");
    ASSERT(cbm_store_begin(s) == CBM_STORE_OK);
    event.id = "derived-invalid-event";
    source.id = "derived-invalid-source";
    source.source_event_ids = "[\"derived-invalid-event\"]";
    ASSERT(cbm_store_memory_append_event(s, &event, NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_candidate(s, &source, NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_link_derived_from(s, source.id, "missing", "test-proj", event.id,
                                              NULL) == CBM_STORE_NOT_FOUND);
    ASSERT(cbm_store_rollback(s) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event") == event_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == item_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == edge_count);

    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "CREATE TRIGGER fail_derived_edge BEFORE INSERT ON memory_edge "
                        "WHEN NEW.type='derived_from' BEGIN SELECT RAISE(ABORT,'edge failure'); END;",
                        NULL, NULL, NULL) == SQLITE_OK);
    ASSERT(cbm_store_begin(s) == CBM_STORE_OK);
    event.id = "derived-edge-fail-event";
    source.id = "derived-edge-fail-source";
    source.source_event_ids = "[\"derived-edge-fail-event\"]";
    ASSERT(cbm_store_memory_append_event(s, &event, NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_candidate(s, &source, NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_link_derived_from(s, source.id, target.id, "test-proj", event.id,
                                              NULL) == CBM_STORE_ERR);
    ASSERT(cbm_store_rollback(s) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event") == event_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == item_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == edge_count);

    cbm_store_close(s);
    return 0;
}

static int insert_test_memory_fts(cbm_store_t *s, const char *item_id, const char *content) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO memory_fts(item_id,title,summary,content) VALUES(?1,'','',?2);";
    if (sqlite3_prepare_v2(cbm_store_get_db(s), sql, -1, &stmt, NULL) != SQLITE_OK)
        return SQLITE_ERROR;
    sqlite3_bind_text(stmt, 1, item_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, content, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(stmt);
    return rc;
}

typedef struct {
    char *item_id;
    char *session_id;
    char *candidate_id;
    char *content_hash;
} stage7_chain_t;

static void stage7_chain_free(stage7_chain_t *chain) {
    if (!chain) return;
    free(chain->item_id);
    free(chain->session_id);
    free(chain->candidate_id);
    free(chain->content_hash);
    memset(chain, 0, sizeof(*chain));
}

static int stage7_create_chain(cbm_store_t *s, const char *suffix, stage7_chain_t *out) {
    char request_id[128];
    char injection_id[128];
    char usage_id[128];
    snprintf(request_id, sizeof(request_id), "stage7-request-%s", suffix);
    snprintf(injection_id, sizeof(injection_id), "stage7-injection-%s", suffix);
    snprintf(usage_id, sizeof(usage_id), "stage7-usage-%s", suffix);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.layer = "semantic";
    item.content = "Stage 7 synthetic unit fixture";
    item.scope_project = "test-proj";
    item.status = "active";
    if (cbm_store_memory_append_candidate(s, &item, &out->item_id) != CBM_STORE_OK) return 1;
    cbm_retrieval_session_input_t session = {0};
    session.request_id = request_id;
    session.project_scope = "test-proj";
    session.memory_scope = "project";
    session.algorithm_version = "stage7-test-v1";
    session.config_version = 7;
    session.query_text = "stage7 unit query";
    char *returned_request = NULL;
    bool replayed = false;
    if (cbm_store_memory_observe_session_begin(s, &session, &out->session_id,
                                                &returned_request, &replayed) != CBM_STORE_OK) {
        free(returned_request);
        return 1;
    }
    free(returned_request);
    cbm_retrieval_candidate_observation_t candidate = {0};
    candidate.source_store_kind = "project";
    candidate.source_store_id = "test-proj";
    candidate.memory_item_id = out->item_id;
    candidate.retrieval_source = "manual";
    candidate.source_rank = 1;
    candidate.raw_score = 1.0;
    candidate.normalized_score = 1.0;
    candidate.aggregate_rank = 1;
    candidate.decision_status = "selected";
    cbm_retrieval_observation_ref_t ref = {0};
    if (cbm_store_memory_observe_candidates(s, out->session_id, &candidate, 1, &ref) !=
        CBM_STORE_OK) return 1;
    out->candidate_id = ref.candidate_id;
    out->content_hash = ref.content_hash;
    free(ref.provenance_id);
    free(ref.evidence_id);
    cbm_observe_injection_input_t injection = {0};
    injection.event_id = injection_id;
    injection.session_id = out->session_id;
    injection.candidate_id = out->candidate_id;
    injection.injection_index = 0;
    injection.target = "context";
    injection.content_hash = out->content_hash;
    injection.token_count = 8;
    injection.classifier_status = "pass";
    injection.classification = "safe";
    if (cbm_store_memory_observe_injection(s, &injection) != CBM_STORE_OK) return 1;
    cbm_observe_usage_input_t usage = {0};
    usage.event_id = usage_id;
    usage.session_id = out->session_id;
    usage.candidate_id = out->candidate_id;
    usage.injection_id = injection_id;
    usage.outcome = "used";
    usage.evidence_type = "external_verified";
    usage.evidence_ref = "unit:test";
    usage.evidence_hash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    if (cbm_store_memory_observe_usage(s, &usage) != CBM_STORE_OK) return 1;
    return cbm_store_memory_observe_session_complete(s, out->session_id, "completed", NULL) ==
                   CBM_STORE_OK
               ? 0
               : 1;
}

static cbm_feedback_observe_input_t stage7_feedback_input(const stage7_chain_t *chain,
                                                           const char *suffix) {
    static char event_id[128];
    static char task_id[128];
    static char result_id[128];
    static char evidence_id[128];
    static char injection_id[128];
    static char usage_id[128];
    static char result_hash[65];
    static char evidence_hash[65];
    snprintf(event_id, sizeof(event_id), "stage7-feedback-%s", suffix);
    snprintf(task_id, sizeof(task_id), "stage7-task-%s", suffix);
    snprintf(result_id, sizeof(result_id), "stage7-result-%s", suffix);
    snprintf(evidence_id, sizeof(evidence_id), "stage7-evidence-%s", suffix);
    snprintf(injection_id, sizeof(injection_id), "stage7-injection-%s", suffix);
    snprintf(usage_id, sizeof(usage_id), "stage7-usage-%s", suffix);
    const char *result_payload = "build passed";
    const char *evidence_payload = "verified build log";
    (void)cbm_stage7_sha256_hex(result_payload, strlen(result_payload), result_hash);
    (void)cbm_stage7_sha256_hex(evidence_payload, strlen(evidence_payload), evidence_hash);
    cbm_feedback_observe_input_t input = {0};
    input.project = "test-proj";
    input.processing_mode = "observe_only";
    input.event_id = event_id;
    input.task_id = task_id;
    input.task_type = "build";
    input.session_id = chain->session_id;
    input.candidate_id = chain->candidate_id;
    input.injection_id = injection_id;
    input.usage_id = usage_id;
    input.result_id = result_id;
    input.result_type = "build";
    input.result_status = "succeeded";
    input.result_ref = "test-results/unit-build.log";
    input.result_hash = result_hash;
    input.result_payload = result_payload;
    input.evidence_id = evidence_id;
    input.evidence_trust = "external_verified";
    input.evidence_state = "valid";
    input.evidence_source = "build";
    input.evidence_ref = "test-results/unit-build.log";
    input.evidence_hash = evidence_hash;
    input.evidence_payload = evidence_payload;
    input.action = "confirm";
    input.algorithm_version = "stage7-reward-v1";
    input.config_version = 7;
    return input;
}

static int stage7_add_visited_edge(cbm_store_t *s, const stage7_chain_t *chain,
                                   const char *edge_id, char **out_peer_id) {
    cbm_memory_item_t peer = {0};
    peer.kind = "fact";
    peer.layer = "semantic";
    peer.content = "Stage 7 visited edge peer fixture";
    peer.scope_project = "test-proj";
    peer.status = "active";
    if (cbm_store_memory_append_candidate(s, &peer, out_peer_id) != CBM_STORE_OK) return 1;
    if (insert_test_memory_edge(s, edge_id, chain->item_id, *out_peer_id, "supports") !=
        SQLITE_OK) return 1;
    cbm_retrieval_candidate_observation_t candidate = {0};
    candidate.source_store_kind = "project";
    candidate.source_store_id = "test-proj";
    candidate.memory_item_id = *out_peer_id;
    candidate.retrieval_source = "manual";
    candidate.source_rank = 2;
    candidate.raw_score = 0.8;
    candidate.normalized_score = 0.8;
    candidate.aggregate_rank = 2;
    candidate.decision_status = "selected";
    cbm_retrieval_observation_ref_t ref = {0};
    int rc = cbm_store_memory_observe_candidates(s, chain->session_id, &candidate, 1, &ref);
    cbm_store_memory_observation_refs_free(&ref, 1);
    return rc == CBM_STORE_OK ? 0 : 1;
}

static int stage7_raw_scalar_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

static int stage7_rejected_without_rows(cbm_store_t *s,
                                        const cbm_feedback_observe_input_t *input,
                                        int expected_rows) {
    cbm_feedback_observe_result_t result = {0};
    int rc = cbm_store_memory_feedback_observe(s, input, &result);
    cbm_store_memory_feedback_observe_result_free(&result);
    return rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED &&
                   stage7_row_total(s) == expected_rows
               ? 0
               : 1;
}

static cbm_edge_reinforcement_input_t stage8_replay_input(const char *mode) {
    cbm_edge_reinforcement_input_t input = {0};
    input.project = "stage6-fixture-stage8-g8-candidate-v1";
    input.mode = mode;
    input.algorithm_version = "stage8-edge-reinforcement-v1";
    input.config_version = 1;
    return input;
}

static int stage8_object_count(cbm_store_t *s) {
    return scalar_int(s,
                      "SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND "
                      "(name LIKE 'stage8_%' OR name LIKE 'edge_contribution_%' OR "
                      "name='plastic_edge_state' OR name LIKE 'edge_reinforcement_%')");
}

static int stage8_create_edge_feedback(cbm_store_t *s, const char *suffix,
                                       stage7_chain_t *chain, const char *edge_id,
                                       char **peer_id) {
    if (stage7_create_chain(s, suffix, chain) != 0 ||
        stage7_add_visited_edge(s, chain, edge_id, peer_id) != 0) {
        return 1;
    }
    cbm_feedback_observe_input_t feedback = stage7_feedback_input(chain, suffix);
    feedback.edge_id = edge_id;
    cbm_feedback_observe_result_t result = {0};
    int rc = cbm_store_memory_feedback_observe(s, &feedback, &result);
    cbm_store_memory_feedback_observe_result_free(&result);
    return rc == CBM_STORE_OK ? 0 : 1;
}

#define STAGE10_TEST_PROJECT "stage10-fixture-concept-growth-v1"

static int stage10_update_source(cbm_store_t *s, const char *item_id, const char *entity,
                                 const char *kind, const char *content, int ordinal) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE memory_item SET scope_project=?1,entity_key=?2,kind=?3,content=?4,"
        "source_event_ids=?5,confidence=0.95,status='active',supersedes=NULL "
        "WHERE id=?6;";
    if (sqlite3_prepare_v2(cbm_store_get_db(s), sql, -1, &stmt, NULL) != SQLITE_OK) return 1;
    char source_key[96];
    snprintf(source_key, sizeof(source_key), "[\"stage10-source-%d\"]", ordinal);
    sqlite3_bind_text(stmt, 1, STAGE10_TEST_PROJECT, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, entity, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, source_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, item_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(cbm_store_get_db(s)) == 1 ? 0 : 1;
    sqlite3_finalize(stmt);
    return rc;
}

static cbm_store_t *stage10_test_store(void) {
    static const char *entities[] = {
        "stage10-a-edit", "stage10-a-edit", "stage10-b-withdraw", "stage10-b-withdraw",
        "stage10-c-reject", "stage10-c-reject", "stage10-d-rule", "stage10-d-rule",
        "stage10-d-rule",
    };
    static const char *kinds[] = {
        "fact", "lesson", "fact", "lesson", "fact", "lesson", "constraint", "constraint",
        "constraint",
    };
    static const char *contents[] = {
        "Verify capability visibility before a state changing operation.",
        "Check required interface availability prior to mutation.",
        "Preserve immutable history before applying a controlled transition.",
        "Keep append only evidence when changing reviewed state.",
        "Confirm database health before starting a guarded transaction.",
        "Inspect foreign keys prior to a bounded local write.",
        "Schema drift requires stopping before any write.",
        "Abort mutation whenever the database contract changes.",
        "Do not persist after detecting a structural mismatch.",
    };
    cbm_store_t *s = cbm_store_open_memory();
    if (!s) return NULL;
    stage7_chain_t chains[9] = {{0}};
    bool ok = true;
    for (int i = 0; ok && i < 9; i++) {
        char suffix[64];
        snprintf(suffix, sizeof(suffix), "stage10-source-%d", i + 1);
        ok = stage7_create_chain(s, suffix, &chains[i]) == 0;
        if (ok) {
            cbm_feedback_observe_input_t feedback = stage7_feedback_input(&chains[i], suffix);
            cbm_feedback_observe_result_t result = {0};
            ok = cbm_store_memory_feedback_observe(s, &feedback, &result) == CBM_STORE_OK;
            cbm_store_memory_feedback_observe_result_free(&result);
        }
        if (ok)
            ok = stage10_update_source(s, chains[i].item_id, entities[i], kinds[i], contents[i],
                                       i + 1) == 0;
        if (!ok)
            fprintf(stderr, "stage10 fixture source %d failed: %s\n", i + 1,
                    sqlite3_errmsg(cbm_store_get_db(s)));
    }
    for (int i = 0; i < 9; i++) stage7_chain_free(&chains[i]);
    if (!ok) {
        cbm_store_close(s);
        return NULL;
    }
    return s;
}

static cbm_concept_generate_input_t stage10_generate_input(const char *mode,
                                                            const char *run_id,
                                                            const char *idempotency_key) {
    cbm_concept_generate_input_t input = {0};
    input.project = STAGE10_TEST_PROJECT;
    input.store = "project-memory";
    input.operation = "generate";
    input.mode = mode;
    input.run_id = run_id;
    input.idempotency_key = idempotency_key;
    input.algorithm_version = CBM_STAGE10_ALGORITHM_VERSION;
    input.policy_sha256 = CBM_STAGE10_POLICY_SHA256;
    input.policy_version = CBM_STAGE10_POLICY_VERSION;
    input.config_version = CBM_STAGE10_CONFIG_VERSION;
    input.generator_version = CBM_STAGE10_GENERATOR_VERSION;
    return input;
}

static int stage10_write_manifest(const char *report_json, const char *path, char out_hash[65]) {
    yyjson_doc *report_doc = yyjson_read(report_json, strlen(report_json), 0);
    yyjson_val *report = report_doc ? yyjson_doc_get_root(report_doc) : NULL;
    yyjson_val *candidates = report ? yyjson_obj_get(report, "candidates") : NULL;
    if (!report || !yyjson_is_obj(report) || !candidates || !yyjson_is_arr(candidates)) {
        yyjson_doc_free(report_doc);
        return 1;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        yyjson_doc_free(report_doc);
        return 1;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage10-production-canary-manifest/v1");
    const char *string_fields[] = {"project", "store", "run_id", "request_sha256",
                                   "algorithm_version", "generator_version", "policy_sha256",
                                   "schema_sha256", "decision_set_sha256"};
    for (size_t i = 0; i < sizeof(string_fields) / sizeof(string_fields[0]); i++) {
        yyjson_val *value = yyjson_obj_get(report, string_fields[i]);
        if (!value || !yyjson_is_str(value)) {
            yyjson_mut_doc_free(doc);
            yyjson_doc_free(report_doc);
            return 1;
        }
        yyjson_mut_obj_add_strcpy(doc, root, string_fields[i], yyjson_get_str(value));
    }
    yyjson_mut_obj_add_int(doc, root, "policy_version", CBM_STAGE10_POLICY_VERSION);
    yyjson_mut_obj_add_int(doc, root, "config_version", CBM_STAGE10_CONFIG_VERSION);
    yyjson_mut_obj_add_int(doc, root, "eligible_count", (int)yyjson_arr_size(candidates));
    yyjson_mut_obj_add_val(doc, root, "candidates", yyjson_val_mut_copy(doc, candidates));
    size_t size = 0;
    char *json = yyjson_mut_write(doc, 0, &size);
    yyjson_mut_doc_free(doc);
    yyjson_doc_free(report_doc);
    if (!json) return 1;
    FILE *file = fopen(path, "wb");
    int rc = !file || fwrite(json, 1, size, file) != size;
    if (file) fclose(file);
    if (!rc && cbm_stage7_sha256_hex(json, size, out_hash) != CBM_STORE_OK) rc = 1;
    free(json);
    return rc;
}

static int stage10_report_candidate_ids(const char *report_json, char ids[4][40]) {
    yyjson_doc *doc = yyjson_read(report_json, strlen(report_json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *candidates = root ? yyjson_obj_get(root, "candidates") : NULL;
    if (!candidates || !yyjson_is_arr(candidates) || yyjson_arr_size(candidates) != 4) {
        yyjson_doc_free(doc);
        return 1;
    }
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *candidate = yyjson_arr_get(candidates, i);
        yyjson_val *id = candidate ? yyjson_obj_get(candidate, "candidate_id") : NULL;
        if (!id || !yyjson_is_str(id)) {
            yyjson_doc_free(doc);
            return 1;
        }
        snprintf(ids[i], 40, "%s", yyjson_get_str(id));
    }
    yyjson_doc_free(doc);
    return 0;
}

TEST(stage10_migration_is_additive_replayable_and_atomic) {
    cbm_store_t *failed = cbm_store_open_memory();
    ASSERT(failed != NULL);
    stage7_set_env("CBM_STAGE10_MIGRATION_FAIL_AFTER", "schema");
    ASSERT(cbm_store_memory_stage10_migrate(failed) == CBM_STORE_ERR);
    ASSERT(cbm_store_memory_stage10_object_count(failed) == 0);
    stage7_set_env("CBM_STAGE10_MIGRATION_FAIL_AFTER", "");
    ASSERT(cbm_store_memory_stage10_migrate(failed) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_stage10_object_count(failed) == 46);
    ASSERT(cbm_store_memory_stage10_migrate(failed) == CBM_STORE_REPLAYED);
    ASSERT(scalar_int(failed, "SELECT COUNT(*) FROM stage10_component_ledger") == 1);
    ASSERT(scalar_int(failed, "SELECT COUNT(*) FROM pragma_foreign_key_check") == 0);
    cbm_store_close(failed);

    cbm_store_t *drift = cbm_store_open_memory();
    ASSERT(drift != NULL);
    ASSERT(sqlite3_exec(cbm_store_get_db(drift),
                        "CREATE TABLE stage10_component_ledger(component TEXT PRIMARY KEY);",
                        NULL, NULL, NULL) == SQLITE_OK);
    ASSERT(cbm_store_memory_stage10_migrate(drift) == CBM_STORE_IDEMPOTENCY_CONFLICT);
    cbm_store_close(drift);
    return 0;
}

TEST(stage10_generation_review_relations_replay_and_guards) {
    cbm_store_t *s = stage10_test_store();
    ASSERT(s != NULL);
    cbm_concept_generate_input_t off_input = stage10_generate_input("off", "stage10-off", "off-key");
    cbm_concept_result_t off = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &off_input, &off) == CBM_STORE_OK);
    ASSERT(off.report_json && strstr(off.report_json, "\"status\":\"disabled\""));
    ASSERT(cbm_store_memory_stage10_object_count(s) == 0);
    cbm_store_memory_concept_result_free(&off);

    cbm_concept_generate_input_t shadow_input =
        stage10_generate_input("shadow", "stage10-shadow", "shadow-key");
    cbm_concept_result_t shadow1 = {0}, shadow2 = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &shadow_input, &shadow1) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_concept_generate(s, &shadow_input, &shadow2) == CBM_STORE_OK);
    ASSERT(shadow1.report_json && shadow2.report_json &&
           strcmp(shadow1.report_json, shadow2.report_json) == 0);
    ASSERT(shadow1.eligible_count == 4 && cbm_store_memory_stage10_object_count(s) == 0);
    cbm_store_memory_concept_result_free(&shadow2);
    cbm_store_memory_concept_result_free(&shadow1);

    ASSERT(cbm_store_memory_stage10_migrate(s) == CBM_STORE_OK);
    cbm_concept_generate_input_t dry =
        stage10_generate_input("dry_run", "stage10-active", "stage10-active-key");
    cbm_concept_result_t preview = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &dry, &preview) == CBM_STORE_OK);
    ASSERT(preview.eligible_count == 4);
    char manifest[640];
    stage7_temp_db_path(manifest, sizeof(manifest), "stage10_manifest", 1);
    char manifest_hash[65] = {0};
    ASSERT(stage10_write_manifest(preview.report_json, manifest, manifest_hash) == 0);
    char candidate_ids[4][40] = {{0}};
    ASSERT(stage10_report_candidate_ids(preview.report_json, candidate_ids) == 0);
    cbm_store_memory_concept_result_free(&preview);

    cbm_concept_generate_input_t active =
        stage10_generate_input("active", "stage10-active", "stage10-active-key");
    active.manifest_path = manifest;
    active.manifest_sha256 = manifest_hash;
    cbm_concept_result_t blocked = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &active, &blocked) == CBM_STORE_REJECTED);
    ASSERT(blocked.failure_code && strcmp(blocked.failure_code, "ACTIVE_FIXTURE_GUARD") == 0);
    cbm_store_memory_concept_result_free(&blocked);
    stage7_set_env("CBM_STAGE10_ACTIVE_FIXTURE", "1");
    stage7_set_env("CBM_STAGE10_GENERATE_FAIL_AFTER", "run");
    cbm_concept_result_t generate_failed = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &active, &generate_failed) == CBM_STORE_ERR);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_growth_run") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_candidate") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_growth_audit_event") == 0);
    cbm_store_memory_concept_result_free(&generate_failed);
    stage7_set_env("CBM_STAGE10_GENERATE_FAIL_AFTER", "");
    cbm_concept_result_t applied = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &active, &applied) == CBM_STORE_OK);
    ASSERT(applied.production_state_written && applied.proposed_count == 4);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_candidate") == 4);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_node") == 0);
    cbm_store_memory_concept_result_free(&applied);
    cbm_concept_result_t replay = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &active, &replay) == CBM_STORE_REPLAYED);
    ASSERT(!replay.production_state_written && scalar_int(s, "SELECT COUNT(*) FROM concept_growth_run") == 1);
    cbm_store_memory_concept_result_free(&replay);
    cbm_concept_generate_input_t conflict = active;
    conflict.run_id = "stage10-changed-run";
    cbm_concept_result_t conflict_result = {0};
    ASSERT(cbm_store_memory_concept_generate(s, &conflict, &conflict_result) ==
           CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT(conflict_result.failure_code &&
           strcmp(conflict_result.failure_code, "IDEMPOTENCY_CONFLICT") == 0);
    cbm_store_memory_concept_result_free(&conflict_result);

    cbm_concept_review_input_t edit = {STAGE10_TEST_PROJECT, "project-memory", candidate_ids[0],
                                       "edit", "stage10-edit", "Edited fixture concept.", NULL,
                                       "fixture"};
    cbm_concept_result_t reviewed = {0};
    stage7_set_env("CBM_STAGE10_REVIEW_FAIL_AFTER", "event");
    ASSERT(cbm_store_memory_concept_review(s, &edit, &reviewed) == CBM_STORE_ERR);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_review_event") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_candidate_version") == 4);
    cbm_store_memory_concept_result_free(&reviewed);
    stage7_set_env("CBM_STAGE10_REVIEW_FAIL_AFTER", "");
    ASSERT(cbm_store_memory_concept_review(s, &edit, &reviewed) == CBM_STORE_OK);
    cbm_store_memory_concept_result_free(&reviewed);
    cbm_concept_review_input_t approve = {STAGE10_TEST_PROJECT, "project-memory", candidate_ids[0],
                                          "approve", "stage10-approve", NULL, NULL, "fixture"};
    ASSERT(cbm_store_memory_concept_review(s, &approve, &reviewed) == CBM_STORE_OK);
    cbm_store_memory_concept_result_free(&reviewed);
    cbm_concept_review_input_t approve_related = {
        STAGE10_TEST_PROJECT, "project-memory", candidate_ids[1], "approve",
        "stage10-approve-related", NULL, candidate_ids[0], "fixture"};
    ASSERT(cbm_store_memory_concept_review(s, &approve_related, &reviewed) == CBM_STORE_OK);
    cbm_store_memory_concept_result_free(&reviewed);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_relation") == 2);
    cbm_concept_review_input_t withdraw = {STAGE10_TEST_PROJECT, "project-memory", candidate_ids[1],
                                           "withdraw", "stage10-withdraw", NULL, NULL, "fixture"};
    ASSERT(cbm_store_memory_concept_review(s, &withdraw, &reviewed) == CBM_STORE_OK);
    cbm_store_memory_concept_result_free(&reviewed);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM concept_node") == 2);
    cbm_concept_review_input_t reject = {STAGE10_TEST_PROJECT, "project-memory", candidate_ids[2],
                                         "reject", "stage10-reject", NULL, NULL, "fixture"};
    ASSERT(cbm_store_memory_concept_review(s, &reject, &reviewed) == CBM_STORE_OK);
    cbm_store_memory_concept_result_free(&reviewed);
    cbm_concept_result_t inspect = {0};
    ASSERT(cbm_store_memory_concept_inspect(s, STAGE10_TEST_PROJECT, "project-memory",
                                            candidate_ids[1], &inspect) == CBM_STORE_OK);
    ASSERT(inspect.report_json && strstr(inspect.report_json, "\"state\":\"withdrawn\"") &&
           strstr(inspect.report_json, "\"node_count\":1"));
    cbm_store_memory_concept_result_free(&inspect);
    int growth_count = 0, review_count = 0;
    ASSERT(cbm_store_memory_stage10_audit_verify(s, &growth_count, &review_count) == CBM_STORE_OK);
    ASSERT(growth_count == 10 && review_count == 5);
    ASSERT(sqlite3_exec(cbm_store_get_db(s), "DELETE FROM concept_candidate;", NULL, NULL, NULL) !=
           SQLITE_OK);
    ASSERT(sqlite3_exec(cbm_store_get_db(s), "DELETE FROM memory_item;", NULL, NULL, NULL) !=
           SQLITE_OK);
    stage7_set_env("CBM_STAGE10_ACTIVE_FIXTURE", "");
    stage7_remove_db_files(manifest);
    cbm_store_close(s);
    return 0;
}

TEST(stage8_shadow_is_deterministic_and_writes_no_stage8_tables) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    char *peer = NULL;
    ASSERT(stage8_create_edge_feedback(s, "stage8-shadow", &chain, "stage8-shadow-edge",
                                       &peer) == 0);
    ASSERT(stage8_object_count(s) == 0);
    cbm_edge_reinforcement_input_t input = stage8_replay_input("shadow");
    cbm_edge_reinforcement_result_t first = {0};
    cbm_edge_reinforcement_result_t second = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &input, &first) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_reinforcement_replay(s, &input, &second) == CBM_STORE_OK);
    ASSERT(first.report_json && second.report_json);
    ASSERT(strcmp(first.report_json, second.report_json) == 0);
    ASSERT(strstr(first.report_json, "\"mode\":\"shadow\""));
    ASSERT(strstr(first.report_json, "\"pheromone_ppm\":1045000"));
    ASSERT(strstr(first.report_json, "\"long_term_state_written\":false"));
    ASSERT(stage8_object_count(s) == 0);
    cbm_store_memory_reinforcement_result_free(&second);
    cbm_store_memory_reinforcement_result_free(&first);
    free(peer);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage8_active_requires_both_guards_and_exact_replay_is_zero_addition) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    char *peer = NULL;
    ASSERT(stage8_create_edge_feedback(s, "stage8-active", &chain, "stage8-active-edge",
                                       &peer) == 0);
    cbm_edge_reinforcement_input_t active = stage8_replay_input("active");
    cbm_edge_reinforcement_result_t rejected = {0};
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", NULL);
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &rejected) == CBM_STORE_REJECTED);
    ASSERT(stage8_object_count(s) == 0);
    cbm_store_memory_reinforcement_result_free(&rejected);

    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", "1");
    cbm_edge_reinforcement_result_t first = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &first) == CBM_STORE_OK);
    ASSERT(first.recorded_count == 1);
    ASSERT(first.replayed_count == 0);
    ASSERT(strstr(first.report_json, "\"long_term_state_written\":true"));
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_contribution_event") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM plastic_edge_state WHERE pheromone_ppm=1045000") ==
           1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_reinforcement_audit_event") == 1);
    int audit_count = 0;
    ASSERT(cbm_store_memory_stage8_audit_verify(s, &audit_count) == CBM_STORE_OK);
    ASSERT(audit_count == 1);
    cbm_edge_reinforcement_result_t replay = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &replay) == CBM_STORE_OK);
    ASSERT(replay.recorded_count == 0);
    ASSERT(replay.replayed_count == 1);
    ASSERT(strstr(replay.report_json, "\"long_term_state_written\":false"));
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_contribution_event") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_reinforcement_audit_event") == 1);
    cbm_store_memory_reinforcement_result_free(&replay);
    cbm_store_memory_reinforcement_result_free(&first);
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", NULL);
    free(peer);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage8_production_active_requires_exact_canary_manifest_guard) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_edge_reinforcement_input_t input = {0};
    input.project = "H-Codex_H-neuroplastic-main";
    input.mode = "active";
    input.algorithm_version = "stage8-edge-reinforcement-v1";
    input.config_version = 1;
    stage7_set_env("CBM_STAGE8_PRODUCTION_CANARY", NULL);
    stage7_set_env("CBM_STAGE8_PRODUCTION_CANARY_MANIFEST", NULL);
    stage7_set_env("CBM_STAGE8_PRODUCTION_CANARY_SHA256", NULL);
    cbm_edge_reinforcement_result_t result = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &input, &result) == CBM_STORE_REJECTED);
    ASSERT(stage8_object_count(s) == 0);
    cbm_store_memory_reinforcement_result_free(&result);
    cbm_store_close(s);
    return 0;
}

TEST(stage8_correction_and_withdraw_rebuild_from_ledger) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    char *peer = NULL;
    ASSERT(stage8_create_edge_feedback(s, "stage8-comp", &chain, "stage8-comp-edge", &peer) ==
           0);
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", "1");
    cbm_edge_reinforcement_input_t active = stage8_replay_input("active");
    cbm_edge_reinforcement_result_t applied = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &applied) == CBM_STORE_OK);
    cbm_store_memory_reinforcement_result_free(&applied);

    cbm_feedback_observe_input_t correction = stage7_feedback_input(&chain, "stage8-comp");
    correction.event_id = "stage8-feedback-correction";
    correction.result_id = "stage8-result-correction";
    correction.evidence_id = "stage8-evidence-correction";
    correction.action = "correct";
    correction.edge_id = "stage8-comp-edge";
    correction.supersedes_event_id = "stage7-feedback-stage8-comp";
    cbm_feedback_observe_result_t correction_result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &correction, &correction_result) == CBM_STORE_OK);
    cbm_store_memory_feedback_observe_result_free(&correction_result);
    cbm_edge_reinforcement_result_t corrected = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &corrected) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT pheromone_ppm FROM plastic_edge_state") == 970000);
    cbm_store_memory_reinforcement_result_free(&corrected);

    cbm_feedback_observe_input_t withdrawal = stage7_feedback_input(&chain, "stage8-comp");
    withdrawal.event_id = "stage8-feedback-withdrawal";
    withdrawal.result_id = "stage8-result-withdrawal";
    withdrawal.evidence_id = "stage8-evidence-withdrawal";
    withdrawal.evidence_state = "withdrawn";
    withdrawal.action = "withdraw";
    withdrawal.edge_id = "stage8-comp-edge";
    withdrawal.supersedes_event_id = "stage8-feedback-correction";
    cbm_feedback_observe_result_t withdrawal_result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &withdrawal, &withdrawal_result) == CBM_STORE_OK);
    cbm_store_memory_feedback_observe_result_free(&withdrawal_result);
    cbm_edge_reinforcement_result_t withdrawn = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &withdrawn) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT pheromone_ppm FROM plastic_edge_state") == 1000000);
    ASSERT(scalar_int(s, "SELECT effective_event_count FROM plastic_edge_state") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_contribution_event") == 3);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM edge_reinforcement_audit_event") == 3);
    int audit_count = 0;
    ASSERT(cbm_store_memory_stage8_audit_verify(s, &audit_count) == CBM_STORE_OK);
    ASSERT(audit_count == 3);
    cbm_store_memory_reinforcement_result_free(&withdrawn);
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", NULL);
    free(peer);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage8_migration_and_reinforcement_failpoints_are_atomic) {
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", "1");
    for (int fail = 1; fail <= 6; fail++) {
        cbm_store_t *s = cbm_store_open_memory();
        ASSERT(s != NULL);
        char suffix[64];
        char edge[64];
        char value[16];
        snprintf(suffix, sizeof(suffix), "stage8-fail-%d", fail);
        snprintf(edge, sizeof(edge), "stage8-fail-edge-%d", fail);
        snprintf(value, sizeof(value), "%d", fail);
        stage7_chain_t chain = {0};
        char *peer = NULL;
        ASSERT(stage8_create_edge_feedback(s, suffix, &chain, edge, &peer) == 0);
        stage7_set_env("CBM_STAGE8_REINFORCEMENT_FAIL_AFTER", value);
        cbm_edge_reinforcement_input_t active = stage8_replay_input("active");
        cbm_edge_reinforcement_result_t failed = {0};
        ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &failed) != CBM_STORE_OK);
        cbm_store_memory_reinforcement_result_free(&failed);
        ASSERT(stage8_object_count(s) == 0);
        ASSERT(scalar_int(s, "SELECT COUNT(*) FROM pragma_quick_check WHERE quick_check='ok'") ==
               1);
        stage7_set_env("CBM_STAGE8_REINFORCEMENT_FAIL_AFTER", NULL);
        cbm_edge_reinforcement_result_t retry = {0};
        ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &retry) == CBM_STORE_OK);
        ASSERT(retry.recorded_count == 1);
        cbm_store_memory_reinforcement_result_free(&retry);
        free(peer);
        stage7_chain_free(&chain);
        cbm_store_close(s);
    }
    cbm_store_t *migration = cbm_store_open_memory();
    ASSERT(migration != NULL);
    stage7_chain_t chain = {0};
    char *peer = NULL;
    ASSERT(stage8_create_edge_feedback(migration, "stage8-migration-fail", &chain,
                                       "stage8-migration-fail-edge", &peer) == 0);
    stage7_set_env("CBM_STAGE8_MIGRATION_FAIL_AFTER", "1");
    cbm_edge_reinforcement_input_t active = stage8_replay_input("active");
    cbm_edge_reinforcement_result_t failed = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(migration, &active, &failed) != CBM_STORE_OK);
    ASSERT(stage8_object_count(migration) == 0);
    cbm_store_memory_reinforcement_result_free(&failed);
    stage7_set_env("CBM_STAGE8_MIGRATION_FAIL_AFTER", NULL);
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", NULL);
    free(peer);
    stage7_chain_free(&chain);
    cbm_store_close(migration);
    return 0;
}

TEST(stage8_caps_bound_repeated_tasks_and_unrelated_edges_stay_absent) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    char *peer = NULL;
    ASSERT(stage7_create_chain(s, "stage8-cap", &chain) == 0);
    ASSERT(stage7_add_visited_edge(s, &chain, "stage8-cap-edge", &peer) == 0);
    ASSERT(insert_test_memory_edge(s, "stage8-unrelated-edge", chain.item_id, peer, "used_in") ==
           SQLITE_OK);
    for (int i = 0; i < 10; i++) {
        char event_id[64];
        char task_id[64];
        char result_id[64];
        char evidence_id[64];
        snprintf(event_id, sizeof(event_id), "stage8-cap-feedback-%02d", i);
        snprintf(task_id, sizeof(task_id), "stage8-cap-task-%02d", i);
        snprintf(result_id, sizeof(result_id), "stage8-cap-result-%02d", i);
        snprintf(evidence_id, sizeof(evidence_id), "stage8-cap-evidence-%02d", i);
        cbm_feedback_observe_input_t feedback = stage7_feedback_input(&chain, "stage8-cap");
        feedback.event_id = event_id;
        feedback.task_id = task_id;
        feedback.result_id = result_id;
        feedback.evidence_id = evidence_id;
        feedback.edge_id = "stage8-cap-edge";
        cbm_feedback_observe_result_t result = {0};
        ASSERT(cbm_store_memory_feedback_observe(s, &feedback, &result) == CBM_STORE_OK);
        cbm_store_memory_feedback_observe_result_free(&result);
    }
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", "1");
    cbm_edge_reinforcement_input_t active = stage8_replay_input("active");
    cbm_edge_reinforcement_result_t replay = {0};
    ASSERT(cbm_store_memory_reinforcement_replay(s, &active, &replay) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT pheromone_ppm FROM plastic_edge_state") == 1075000);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM plastic_edge_state WHERE edge_id='stage8-unrelated-edge'") ==
           0);
    ASSERT(strstr(replay.report_json, "\"path\":9"));
    cbm_store_memory_reinforcement_result_free(&replay);
    stage7_set_env("CBM_STAGE8_ACTIVE_FIXTURE", NULL);
    free(peer);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

typedef struct {
    cbm_store_t *store;
    const cbm_feedback_observe_input_t *input;
    atomic_int *ready;
    atomic_int *go;
    int rc;
    cbm_feedback_observe_result_t result;
} stage7_feedback_thread_t;

static void *stage7_feedback_thread(void *arg) {
    stage7_feedback_thread_t *ctx = (stage7_feedback_thread_t *)arg;
    (void)atomic_fetch_add_explicit(ctx->ready, 1, memory_order_acq_rel);
    while (atomic_load_explicit(ctx->go, memory_order_acquire) == 0) {
    }
    ctx->rc = cbm_store_memory_feedback_observe(ctx->store, ctx->input, &ctx->result);
    return NULL;
}

TEST(stage7_schema_is_additive_and_user_version_stays_six) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 6);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM stage7_component_ledger") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                         "AND name IN ('memory_task','memory_task_result','memory_evidence',"
                         "'feedback_event','feedback_attribution','plasticity_audit_event')") == 6);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_feedback_shadow_idempotency_and_long_term_zero_change) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    ASSERT(stage7_create_chain(s, "main", &chain) == 0);
    cbm_memory_item_t before = {0};
    ASSERT(cbm_store_memory_get_item(s, chain.item_id, &before) == CBM_STORE_OK);
    cbm_feedback_observe_input_t input = stage7_feedback_input(&chain, "main");
    cbm_feedback_observe_result_t first = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &input, &first) == CBM_STORE_OK);
    ASSERT(first.result_json && strstr(first.result_json, "\"final_reward\":0.75"));
    ASSERT(strstr(first.result_json, "\"long_term_state_written\":false"));
    ASSERT(strlen(first.canonical_payload_sha256) == 64);
    int feedback_count = scalar_int(s, "SELECT COUNT(*) FROM feedback_event");
    int audit_count = scalar_int(s, "SELECT COUNT(*) FROM plasticity_audit_event");
    cbm_feedback_observe_result_t replay = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &input, &replay) == CBM_STORE_REPLAYED);
    ASSERT(strcmp(first.result_json, replay.result_json) == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM feedback_event") == feedback_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM plasticity_audit_event") == audit_count);
    cbm_feedback_observe_input_t conflict = input;
    conflict.action = "reject";
    cbm_feedback_observe_result_t conflict_result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &conflict, &conflict_result) ==
           CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM feedback_event") == feedback_count);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM plasticity_audit_event") == audit_count);
    cbm_memory_item_t after = {0};
    ASSERT(cbm_store_memory_get_item(s, chain.item_id, &after) == CBM_STORE_OK);
    ASSERT(before.hit_count == after.hit_count);
    ASSERT(before.confidence == after.confidence);
    ASSERT(before.reusability == after.reusability);
    ASSERT(before.importance == after.importance);
    ASSERT(before.decay == after.decay);
    ASSERT(strcmp(before.status, after.status) == 0);
    int verified = -1;
    ASSERT(cbm_store_memory_stage7_audit_verify(s, &verified) == CBM_STORE_OK);
    ASSERT(verified == audit_count);
    ASSERT(sqlite3_exec(cbm_store_get_db(s), "UPDATE feedback_event SET action='reject';", NULL,
                        NULL, NULL) != SQLITE_OK);
    cbm_store_memory_feedback_observe_result_free(&conflict_result);
    cbm_store_memory_feedback_observe_result_free(&replay);
    cbm_store_memory_feedback_observe_result_free(&first);
    cbm_store_memory_item_free(&after);
    cbm_store_memory_item_free(&before);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_model_self_report_is_pending_and_wrong_hash_fails_closed) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    ASSERT(stage7_create_chain(s, "model", &chain) == 0);
    cbm_feedback_observe_input_t input = stage7_feedback_input(&chain, "model");
    input.task_type = "health_check";
    input.result_type = "health_check";
    input.evidence_trust = "model_self_report";
    input.evidence_source = "model";
    cbm_feedback_observe_result_t result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &input, &result) == CBM_STORE_OK);
    ASSERT(strstr(result.result_json, "\"final_reward\":0.0"));
    ASSERT(strstr(result.result_json, "\"status\":\"pending_confirmation\""));
    int before = scalar_int(s, "SELECT COUNT(*) FROM feedback_event");
    cbm_feedback_observe_input_t bad = stage7_feedback_input(&chain, "bad-hash");
    bad.result_hash = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    cbm_feedback_observe_result_t rejected = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &bad, &rejected) == CBM_STORE_ERR);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM feedback_event") == before);
    cbm_store_memory_feedback_observe_result_free(&rejected);
    cbm_store_memory_feedback_observe_result_free(&result);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_sha256_vectors_and_payloads_are_not_persisted) {
    char hash[65];
    ASSERT(cbm_stage7_sha256_hex("", 0, hash) == CBM_STORE_OK);
    ASSERT(strcmp(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") ==
           0);
    ASSERT(cbm_stage7_sha256_hex("abc", 3, hash) == CBM_STORE_OK);
    ASSERT(strcmp(hash, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") ==
           0);
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    ASSERT(stage7_create_chain(s, "payload", &chain) == 0);
    cbm_feedback_observe_input_t input = stage7_feedback_input(&chain, "payload");
    cbm_feedback_observe_result_t result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &input, &result) == CBM_STORE_OK);
    ASSERT(result.canonical_payload_sha256 && strlen(result.canonical_payload_sha256) == 64);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM feedback_event WHERE "
                      "instr(payload_json,'build passed')>0 OR "
                      "instr(payload_json,'verified build log')>0 OR "
                      "instr(result_json,'verified build log')>0") == 0);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM pragma_table_info('memory_task_result') "
                      "WHERE name LIKE '%payload%'") == 0);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM pragma_table_info('memory_evidence') "
                      "WHERE name LIKE '%payload%'") == 0);
    cbm_store_memory_feedback_observe_result_free(&result);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_reward_report_uses_only_visited_edges) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    ASSERT(stage7_create_chain(s, "edge", &chain) == 0);
    char *peer_id = NULL;
    ASSERT(stage7_add_visited_edge(s, &chain, "stage7-visited-edge", &peer_id) == 0);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM retrieval_edge_visit WHERE "
                      "memory_edge_id='stage7-visited-edge'") >= 1);
    cbm_feedback_observe_input_t input = stage7_feedback_input(&chain, "edge");
    input.edge_id = "stage7-visited-edge";
    cbm_feedback_observe_result_t result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &input, &result) == CBM_STORE_OK);
    ASSERT(result.result_json && strstr(result.result_json, "\"task\":"));
    ASSERT(strstr(result.result_json, "\"session_id\":"));
    ASSERT(strstr(result.result_json, "\"candidate_id\":"));
    ASSERT(strstr(result.result_json, "\"memory_item_id\":"));
    ASSERT(strstr(result.result_json, "\"evidence\":"));
    ASSERT(strstr(result.result_json, "\"edge_id\":\"stage7-visited-edge\""));
    ASSERT(strstr(result.result_json, "\"visited\":true"));
    ASSERT(strstr(result.result_json, "\"cap\":{\"min\":-1.0,\"max\":1.0}"));
    ASSERT(strstr(result.result_json, "\"long_term_state_written\":false"));
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM feedback_attribution WHERE "
                      "edge_id='stage7-visited-edge' AND "
                      "abs(node_contribution-0.525)<0.000001 AND "
                      "abs(edge_contribution-0.225)<0.000001 AND final_reward=0.75") == 1);
    cbm_feedback_observe_input_t unvisited = input;
    unvisited.event_id = "stage7-feedback-unvisited-edge";
    unvisited.edge_id = "stage7-existing-unvisited-edge";
    ASSERT(insert_test_memory_edge(s, unvisited.edge_id, chain.item_id, peer_id, "used_in") ==
           SQLITE_OK);
    int rows = stage7_row_total(s);
    ASSERT(stage7_rejected_without_rows(s, &unvisited, rows) == 0);
    cbm_feedback_observe_input_t fabricated = input;
    fabricated.event_id = "stage7-feedback-fabricated-edge";
    fabricated.edge_id = "stage7-edge-does-not-exist";
    ASSERT(stage7_rejected_without_rows(s, &fabricated, rows) == 0);
    cbm_store_memory_feedback_observe_result_free(&result);
    free(peer_id);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_cross_chain_and_forged_evidence_fail_closed) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t a = {0};
    stage7_chain_t b = {0};
    ASSERT(stage7_create_chain(s, "cross-a", &a) == 0);
    ASSERT(stage7_create_chain(s, "cross-b", &b) == 0);
    cbm_feedback_observe_input_t base = stage7_feedback_input(&a, "cross");
    int rows = stage7_row_total(s);
    cbm_feedback_observe_input_t bad = base;
    bad.processing_mode = "active";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.session_id = b.session_id;
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.candidate_id = b.candidate_id;
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.injection_id = "stage7-injection-cross-b";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.usage_id = "stage7-usage-cross-b";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.evidence_trust = "explicit_user";
    bad.evidence_source = "model";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.evidence_trust = "model_self_report";
    bad.evidence_source = "user";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.evidence_trust = "external_verified";
    bad.evidence_source = "model";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    bad = base;
    bad.evidence_hash = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    ASSERT(stage7_rejected_without_rows(s, &bad, rows) == 0);
    stage7_chain_free(&b);
    stage7_chain_free(&a);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_correction_withdrawal_and_evidence_states_are_append_only) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    ASSERT(stage7_create_chain(s, "append", &chain) == 0);
    cbm_memory_item_t before = {0};
    ASSERT(cbm_store_memory_get_item(s, chain.item_id, &before) == CBM_STORE_OK);
    cbm_feedback_observe_input_t initial = stage7_feedback_input(&chain, "append");
    cbm_feedback_observe_result_t first = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &initial, &first) == CBM_STORE_OK);

    cbm_feedback_observe_input_t correction = initial;
    correction.event_id = "stage7-feedback-append-correct";
    correction.evidence_id = "stage7-evidence-append-correct";
    correction.action = "correct";
    correction.supersedes_event_id = initial.event_id;
    cbm_feedback_observe_result_t corrected = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &correction, &corrected) == CBM_STORE_OK);
    ASSERT(strstr(corrected.result_json, "\"final_reward\":-0.5"));

    cbm_feedback_observe_input_t invalid = correction;
    invalid.event_id = "stage7-feedback-append-invalid";
    invalid.evidence_id = "stage7-evidence-append-invalid";
    invalid.evidence_state = "invalid";
    cbm_feedback_observe_result_t invalid_result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &invalid, &invalid_result) == CBM_STORE_OK);
    ASSERT(strstr(invalid_result.result_json, "\"status\":\"invalid\""));
    ASSERT(strstr(invalid_result.result_json, "\"final_reward\":0.0"));

    cbm_feedback_observe_input_t expired = correction;
    expired.event_id = "stage7-feedback-append-expired";
    expired.evidence_id = "stage7-evidence-append-expired";
    expired.evidence_state = "expired";
    cbm_feedback_observe_result_t expired_result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &expired, &expired_result) == CBM_STORE_OK);
    ASSERT(strstr(expired_result.result_json, "\"status\":\"invalid\""));
    ASSERT(strstr(expired_result.result_json, "\"final_reward\":0.0"));

    cbm_feedback_observe_input_t withdrawal = correction;
    withdrawal.event_id = "stage7-feedback-append-withdraw";
    withdrawal.evidence_id = "stage7-evidence-append-withdraw";
    withdrawal.evidence_state = "withdrawn";
    withdrawal.action = "withdraw";
    cbm_feedback_observe_result_t withdrawn = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &withdrawal, &withdrawn) == CBM_STORE_OK);
    ASSERT(strstr(withdrawn.result_json, "\"status\":\"withdrawn\""));
    ASSERT(strstr(withdrawn.result_json, "\"final_reward\":0.0"));

    cbm_feedback_observe_input_t model_withdrawal = withdrawal;
    model_withdrawal.event_id = "stage7-feedback-append-model-withdraw";
    model_withdrawal.evidence_id = "stage7-evidence-append-model-withdraw";
    model_withdrawal.evidence_trust = "model_self_report";
    model_withdrawal.evidence_source = "model";
    cbm_feedback_observe_result_t model_result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &model_withdrawal, &model_result) == CBM_STORE_OK);
    ASSERT(strstr(model_result.result_json, "\"status\":\"pending_confirmation\""));
    ASSERT(strstr(model_result.result_json, "\"final_reward\":0.0"));

    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_task") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_task_result") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_evidence") == 6);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM feedback_event") == 6);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM feedback_attribution") == 6);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM plasticity_audit_event") == 6);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM plasticity_audit_event WHERE "
                      "operation='compensating_correction'") == 3);
    ASSERT(scalar_int(s,
                      "SELECT COUNT(*) FROM plasticity_audit_event WHERE operation='withdrawal'") ==
           2);
    const char *append_only_updates[] = {
        "UPDATE memory_task SET task_type=task_type;",
        "UPDATE memory_task_session SET linked_at=linked_at;",
        "UPDATE memory_task_result SET status=status;",
        "UPDATE memory_evidence SET evidence_state=evidence_state;",
        "UPDATE feedback_event SET action=action;",
        "UPDATE feedback_attribution SET final_reward=final_reward;",
        "UPDATE plasticity_audit_event SET after_json=after_json;",
    };
    for (size_t i = 0; i < sizeof(append_only_updates) / sizeof(append_only_updates[0]); i++) {
        ASSERT(sqlite3_exec(cbm_store_get_db(s), append_only_updates[i], NULL, NULL, NULL) !=
               SQLITE_OK);
    }
    int verified = -1;
    ASSERT(cbm_store_memory_stage7_audit_verify(s, &verified) == CBM_STORE_OK);
    ASSERT(verified == 6);
    cbm_memory_item_t after = {0};
    ASSERT(cbm_store_memory_get_item(s, chain.item_id, &after) == CBM_STORE_OK);
    ASSERT(before.hit_count == after.hit_count && before.confidence == after.confidence &&
           before.reusability == after.reusability && before.importance == after.importance &&
           before.decay == after.decay && strcmp(before.status, after.status) == 0);
    cbm_store_memory_item_free(&after);
    cbm_store_memory_item_free(&before);
    cbm_store_memory_feedback_observe_result_free(&model_result);
    cbm_store_memory_feedback_observe_result_free(&withdrawn);
    cbm_store_memory_feedback_observe_result_free(&expired_result);
    cbm_store_memory_feedback_observe_result_free(&invalid_result);
    cbm_store_memory_feedback_observe_result_free(&corrected);
    cbm_store_memory_feedback_observe_result_free(&first);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_feedback_failure_injection_rolls_back_and_retries_cleanly) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    for (int point = 1; point <= 4; point++) {
        char suffix[32];
        char failpoint[16];
        snprintf(suffix, sizeof(suffix), "failure-%d", point);
        snprintf(failpoint, sizeof(failpoint), "%d", point);
        stage7_chain_t chain = {0};
        ASSERT(stage7_create_chain(s, suffix, &chain) == 0);
        cbm_memory_item_t before = {0};
        ASSERT(cbm_store_memory_get_item(s, chain.item_id, &before) == CBM_STORE_OK);
        cbm_feedback_observe_input_t input = stage7_feedback_input(&chain, suffix);
        int rows = stage7_row_total(s);
        stage7_set_env("CBM_STAGE7_FEEDBACK_FAIL_AFTER", failpoint);
        cbm_feedback_observe_result_t failed = {0};
        int rc = cbm_store_memory_feedback_observe(s, &input, &failed);
        stage7_set_env("CBM_STAGE7_FEEDBACK_FAIL_AFTER", NULL);
        ASSERT(rc == CBM_STORE_ERR);
        ASSERT(stage7_row_total(s) == rows);
        cbm_memory_item_t after_failure = {0};
        ASSERT(cbm_store_memory_get_item(s, chain.item_id, &after_failure) == CBM_STORE_OK);
        ASSERT(before.hit_count == after_failure.hit_count &&
               before.confidence == after_failure.confidence &&
               before.reusability == after_failure.reusability &&
               before.importance == after_failure.importance && before.decay == after_failure.decay &&
               strcmp(before.status, after_failure.status) == 0);
        cbm_feedback_observe_result_t retry = {0};
        ASSERT(cbm_store_memory_feedback_observe(s, &input, &retry) == CBM_STORE_OK);
        ASSERT(stage7_row_total(s) == rows + 7);
        cbm_store_memory_feedback_observe_result_free(&retry);
        cbm_store_memory_feedback_observe_result_free(&failed);
        cbm_store_memory_item_free(&after_failure);
        cbm_store_memory_item_free(&before);
        stage7_chain_free(&chain);
    }
    stage7_set_env("CBM_STAGE7_FEEDBACK_FAIL_AFTER", NULL);
    cbm_store_close(s);
    return 0;
}

TEST(stage7_migration_failure_is_atomic_and_recoverable) {
    const int failure_points[] = {1, 10};
    for (int i = 0; i < 2; i++) {
        char path[512];
        char value[16];
        stage7_temp_db_path(path, sizeof(path), "migration_failure", i);
        stage7_remove_db_files(path);
        snprintf(value, sizeof(value), "%d", failure_points[i]);
        stage7_set_env("CBM_STAGE7_MIGRATION_FAIL_AFTER", value);
        cbm_store_t *failed = cbm_store_open_path(path);
        stage7_set_env("CBM_STAGE7_MIGRATION_FAIL_AFTER", NULL);
        ASSERT(failed == NULL);
        sqlite3 *raw = NULL;
        ASSERT(sqlite3_open(path, &raw) == SQLITE_OK);
        ASSERT(stage7_raw_scalar_int(raw, "PRAGMA user_version") == 6);
        ASSERT(stage7_raw_scalar_int(
                   raw,
                   "SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND "
                   "(name LIKE 'stage7_%' OR name LIKE 'memory_task%' OR "
                   "name LIKE 'memory_evidence%' OR name LIKE 'feedback_%' OR "
                   "name LIKE 'plasticity_%')") == 0);
        ASSERT(stage7_raw_scalar_int(raw, "SELECT COUNT(*) FROM pragma_quick_check") == 1);
        sqlite3_close(raw);
        cbm_store_t *recovered = cbm_store_open_path(path);
        ASSERT(recovered != NULL);
        ASSERT(scalar_int(recovered, "PRAGMA user_version") == 6);
        ASSERT(scalar_int(recovered, "SELECT COUNT(*) FROM stage7_component_ledger") == 1);
        cbm_store_close(recovered);
        stage7_remove_db_files(path);
    }
    stage7_set_env("CBM_STAGE7_MIGRATION_FAIL_AFTER", NULL);
    return 0;
}

TEST(stage7_audit_tamper_is_detected_and_schema_drift_fails_reopen) {
    char path[512];
    stage7_temp_db_path(path, sizeof(path), "audit_tamper", 0);
    stage7_remove_db_files(path);
    cbm_store_t *s = cbm_store_open_path(path);
    ASSERT(s != NULL);
    stage7_chain_t chain = {0};
    ASSERT(stage7_create_chain(s, "tamper", &chain) == 0);
    cbm_feedback_observe_input_t input = stage7_feedback_input(&chain, "tamper");
    cbm_feedback_observe_result_t result = {0};
    ASSERT(cbm_store_memory_feedback_observe(s, &input, &result) == CBM_STORE_OK);
    int verified = -1;
    ASSERT(cbm_store_memory_stage7_audit_verify(s, &verified) == CBM_STORE_OK);
    ASSERT(verified == 1);
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "UPDATE plasticity_audit_event SET after_json='{}';", NULL, NULL,
                        NULL) != SQLITE_OK);
    ASSERT(sqlite3_exec(cbm_store_get_db(s), "DROP TRIGGER plasticity_audit_no_update;", NULL,
                        NULL, NULL) == SQLITE_OK);
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "UPDATE plasticity_audit_event SET after_json='{}';", NULL, NULL,
                        NULL) == SQLITE_OK);
    ASSERT(cbm_store_memory_stage7_audit_verify(s, &verified) == CBM_STORE_ERR);
    cbm_store_memory_feedback_observe_result_free(&result);
    stage7_chain_free(&chain);
    cbm_store_close(s);
    ASSERT(cbm_store_open_path(path) == NULL);
    stage7_remove_db_files(path);
    return 0;
}

TEST(stage7_concurrent_replay_and_conflict_are_serialized) {
    char path[512];
    stage7_temp_db_path(path, sizeof(path), "concurrency", 0);
    stage7_remove_db_files(path);
    cbm_store_t *setup = cbm_store_open_path(path);
    ASSERT(setup != NULL);
    stage7_chain_t exact_chain = {0};
    ASSERT(stage7_create_chain(setup, "concurrent-exact", &exact_chain) == 0);
    cbm_feedback_observe_input_t exact_input =
        stage7_feedback_input(&exact_chain, "concurrent-exact");
    cbm_store_close(setup);
    cbm_store_t *exact_a = cbm_store_open_path(path);
    cbm_store_t *exact_b = cbm_store_open_path(path);
    ASSERT(exact_a != NULL && exact_b != NULL);
    atomic_int ready = ATOMIC_VAR_INIT(0);
    atomic_int go = ATOMIC_VAR_INIT(0);
    stage7_feedback_thread_t exact_ctx_a = {exact_a, &exact_input, &ready, &go, CBM_STORE_ERR, {0}};
    stage7_feedback_thread_t exact_ctx_b = {exact_b, &exact_input, &ready, &go, CBM_STORE_ERR, {0}};
    cbm_thread_t exact_thread_a;
    cbm_thread_t exact_thread_b;
    ASSERT(cbm_thread_create(&exact_thread_a, 0, stage7_feedback_thread, &exact_ctx_a) == 0);
    ASSERT(cbm_thread_create(&exact_thread_b, 0, stage7_feedback_thread, &exact_ctx_b) == 0);
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) {
    }
    atomic_store_explicit(&go, 1, memory_order_release);
    ASSERT(cbm_thread_join(&exact_thread_a) == 0);
    ASSERT(cbm_thread_join(&exact_thread_b) == 0);
    ASSERT((exact_ctx_a.rc == CBM_STORE_OK && exact_ctx_b.rc == CBM_STORE_REPLAYED) ||
           (exact_ctx_b.rc == CBM_STORE_OK && exact_ctx_a.rc == CBM_STORE_REPLAYED));
    ASSERT(scalar_int(exact_a,
                      "SELECT COUNT(*) FROM feedback_event WHERE "
                      "event_id='stage7-feedback-concurrent-exact'") == 1);
    ASSERT(scalar_int(exact_a,
                      "SELECT COUNT(*) FROM plasticity_audit_event WHERE "
                      "feedback_event_id='stage7-feedback-concurrent-exact'") == 1);
    cbm_store_memory_feedback_observe_result_free(&exact_ctx_b.result);
    cbm_store_memory_feedback_observe_result_free(&exact_ctx_a.result);
    cbm_store_close(exact_b);
    cbm_store_close(exact_a);
    stage7_chain_free(&exact_chain);

    setup = cbm_store_open_path(path);
    ASSERT(setup != NULL);
    stage7_chain_t conflict_chain = {0};
    ASSERT(stage7_create_chain(setup, "concurrent-conflict", &conflict_chain) == 0);
    cbm_feedback_observe_input_t confirm =
        stage7_feedback_input(&conflict_chain, "concurrent-conflict");
    cbm_feedback_observe_input_t reject = confirm;
    reject.action = "reject";
    cbm_store_close(setup);
    cbm_store_t *conflict_a = cbm_store_open_path(path);
    cbm_store_t *conflict_b = cbm_store_open_path(path);
    ASSERT(conflict_a != NULL && conflict_b != NULL);
    atomic_store_explicit(&ready, 0, memory_order_release);
    atomic_store_explicit(&go, 0, memory_order_release);
    stage7_feedback_thread_t conflict_ctx_a = {
        conflict_a, &confirm, &ready, &go, CBM_STORE_ERR, {0}};
    stage7_feedback_thread_t conflict_ctx_b = {
        conflict_b, &reject, &ready, &go, CBM_STORE_ERR, {0}};
    cbm_thread_t conflict_thread_a;
    cbm_thread_t conflict_thread_b;
    ASSERT(cbm_thread_create(&conflict_thread_a, 0, stage7_feedback_thread, &conflict_ctx_a) == 0);
    ASSERT(cbm_thread_create(&conflict_thread_b, 0, stage7_feedback_thread, &conflict_ctx_b) == 0);
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) {
    }
    atomic_store_explicit(&go, 1, memory_order_release);
    ASSERT(cbm_thread_join(&conflict_thread_a) == 0);
    ASSERT(cbm_thread_join(&conflict_thread_b) == 0);
    ASSERT((conflict_ctx_a.rc == CBM_STORE_OK &&
            conflict_ctx_b.rc == CBM_STORE_IDEMPOTENCY_CONFLICT) ||
           (conflict_ctx_b.rc == CBM_STORE_OK &&
            conflict_ctx_a.rc == CBM_STORE_IDEMPOTENCY_CONFLICT));
    ASSERT(scalar_int(conflict_a,
                      "SELECT COUNT(*) FROM feedback_event WHERE "
                      "event_id='stage7-feedback-concurrent-conflict'") == 1);
    ASSERT(scalar_int(conflict_a,
                      "SELECT COUNT(*) FROM plasticity_audit_event WHERE "
                      "feedback_event_id='stage7-feedback-concurrent-conflict'") == 1);
    cbm_store_memory_feedback_observe_result_free(&conflict_ctx_b.result);
    cbm_store_memory_feedback_observe_result_free(&conflict_ctx_a.result);
    cbm_store_close(conflict_b);
    cbm_store_close(conflict_a);
    stage7_chain_free(&conflict_chain);
    stage7_remove_db_files(path);
    return 0;
}

TEST(memory_schema_init) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_store_close(s);
    return 0;
}

TEST(memory_append_event) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_event_t ev = {0};
    ev.type = "test.event";
    ev.source = "unit_test";
    ev.project = "test-proj";
    ev.payload = "{\"key\":\"value\"}";
    ev.confidence = 0.8;
    char *id = NULL;
    int rc = cbm_store_memory_append_event(s, &ev, &id);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(id != NULL);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_append_candidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.layer = "semantic";
    item.title = "Test Fact";
    item.content = "This is a test memory item for MVP validation.";
    item.scope_project = "test-proj";
    item.status = "candidate";
    item.confidence = 0.9;
    char *id = NULL;
    int rc = cbm_store_memory_append_candidate(s, &item, &id);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(id != NULL);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_stage11_secondary_fields_reject_before_write) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);

    cbm_memory_event_t event = {0};
    event.type = "test.event";
    event.source = "unit_test";
    event.project = "test-proj";
    event.payload = "{}";
    event.context_json = "password=fixture-value";
    ASSERT(cbm_store_memory_append_event(s, &event, NULL) == CBM_STORE_REJECTED);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event") == 0);

    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.title = "api_key=fixture-value";
    item.content = "ordinary safe fixture content";
    item.scope_project = "test-proj";
    item.status = "candidate";
    ASSERT(cbm_store_memory_append_candidate(s, &item, NULL) == CBM_STORE_REJECTED);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_fts") == 0);

    cbm_store_close(s);
    return 0;
}

TEST(memory_append_structured_candidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "decision";
    item.layer = "semantic";
    item.title = "Use structured memory events";
    item.summary = "Structured fields should survive the hot path.";
    item.content = "Store user decisions with explicit entity and predicate.";
    item.scope_user = "alice";
    item.scope_project = "test-proj";
    item.scope_task = "memory-mvp";
    item.entity_key = "memory.events";
    item.predicate = "decides";
    item.importance = 0.8;
    item.confidence = 0.9;
    item.reusability = 0.7;
    item.specificity = 0.6;
    item.status = "candidate";
    item.source_event_ids = "[\"evt-structured\"]";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.kind, "decision") == 0);
    ASSERT(strcmp(out.layer, "semantic") == 0);
    ASSERT(strcmp(out.scope_user, "alice") == 0);
    ASSERT(strcmp(out.scope_task, "memory-mvp") == 0);
    ASSERT(strcmp(out.entity_key, "memory.events") == 0);
    ASSERT(strcmp(out.predicate, "decides") == 0);
    ASSERT(out.importance > 0.79 && out.reusability > 0.69 && out.specificity > 0.59);
    ASSERT(strstr(out.source_event_ids, "evt-structured") != NULL);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}


TEST(memory_get_item) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "Get item test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    cbm_memory_item_t out = {0};
    int rc = cbm_store_memory_get_item(s, id, &out);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(out.content != NULL);
    ASSERT(strstr(out.content, "Get item test") != NULL);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_structured) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    for (int i = 0; i < 3; i++) {
        cbm_memory_item_t item = {0};
        item.kind = "fact";
        item.content = "Structured retrieval test item";
        item.scope_project = "test-proj";
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    int processed = -1;
    cbm_store_memory_consolidate(s, "test-proj", 100, &processed);
    fprintf(stderr, "processed=%d\n", processed); ASSERT(processed == 3);
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    int rc = cbm_store_memory_retrieve(s, &q, &res);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    ASSERT(res.count <= 3);
    cbm_store_memory_result_free(&res);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_fts) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "FTS5 full text search test for memory system";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    free(id);
    int processed = -1;
    cbm_store_memory_consolidate(s, "test-proj", 100, &processed);
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "FTS5";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    int rc = cbm_store_memory_retrieve(s, &q, &res);
    if (rc == CBM_STORE_OK) { ASSERT(res.count >= 1); }
    cbm_store_memory_result_free(&res);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_long_prompt_accepts_four_tokens_and_rejects_three_token_noise) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t target = {0};
    target.kind = "fact";
    target.content = "stage12 lifecycle completion evidence";
    target.scope_project = "long-query-overlap";
    target.status = "candidate";
    char *target_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &target, &target_id) == CBM_STORE_OK);

    cbm_memory_item_t noise = target;
    noise.content = "stage12 lifecycle completion";
    char *noise_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &noise, &noise_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "long-query-overlap", 100, &processed) ==
           CBM_STORE_OK);
    ASSERT(processed == 2);
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
                        "DELETE FROM memory_vec WHERE item_id IN "
                        "(SELECT id FROM memory_item WHERE scope_project='long-query-overlap');",
                        NULL, NULL, NULL) == SQLITE_OK);

    cbm_memory_query_t query = {0};
    query.project = "long-query-overlap";
    query.query = "please verify stage12 lifecycle completion evidence with unrelated filler "
                  "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima";
    query.limit = 5;
    cbm_memory_result_t result = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_OK);
    bool found_target = false;
    bool found_noise = false;
    for (int i = 0; i < result.count; i++) {
        if (result.items[i].id && strcmp(result.items[i].id, target_id) == 0)
            found_target = true;
        if (result.items[i].id && strcmp(result.items[i].id, noise_id) == 0)
            found_noise = true;
    }
    ASSERT(found_target);
    ASSERT(!found_noise);

    cbm_store_memory_result_free(&result);
    free(target_id);
    free(noise_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_vector_fusion) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "vector-only-alpha";
    item.scope_project = "test-proj";
    item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    sqlite3_stmt *del = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s), "DELETE FROM memory_fts WHERE item_id=?1;",
                              -1, &del, NULL) == SQLITE_OK);
    sqlite3_bind_text(del, 1, id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(del) == SQLITE_DONE);
    sqlite3_finalize(del);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "vector-only-alpha";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    bool found_vector = false;
    for (int i = 0; i < res.count; i++) {
        if (res.items[i].id && strcmp(res.items[i].id, id) == 0 &&
            res.items[i].retrieval_source &&
            strcmp(res.items[i].retrieval_source, "vector") == 0) {
            found_vector = true;
            ASSERT(res.items[i].retrieval_score > 0.99);
        }
    }
    ASSERT(found_vector);
    cbm_store_memory_result_free(&res);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_safety_fixture_requires_category_relevance) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t normal = {0};
    normal.kind = "fact";
    normal.content = "ordinary retrieval anchor";
    normal.scope_project = "test-proj";
    normal.status = "candidate";
    char *normal_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &normal, &normal_id) == CBM_STORE_OK);

    cbm_memory_item_t prompt = {0};
    prompt.kind = "safety_fixture";
    prompt.entity_key = "stage4:unsafe-prompt-injection";
    prompt.content = "synthetic prompt injection canary";
    prompt.scope_project = "test-proj";
    prompt.status = "candidate";
    char *prompt_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &prompt, &prompt_id) == CBM_STORE_OK);

    cbm_memory_item_t secret = {0};
    secret.kind = "safety_fixture";
    secret.entity_key = "stage4:unsafe-secret";
    secret.content = "synthetic secret canary";
    secret.scope_project = "test-proj";
    secret.status = "candidate";
    char *secret_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &secret, &secret_id) == CBM_STORE_OK);

    cbm_memory_item_t other = prompt;
    other.scope_project = "other-proj";
    char *other_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &other, &other_id) == CBM_STORE_OK);

    cbm_memory_item_t bob = prompt;
    bob.scope_user = "bob";
    char *bob_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &bob, &bob_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, NULL, 100, &processed) == CBM_STORE_OK);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.user = "alice";
    q.query = "ordinary retrieval anchor";
    q.limit = 10;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, normal_id) == 0);
    cbm_store_memory_result_free(&res);

    q.query = "stage4 unsafe prompt injection synthetic fixture";
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    bool prompt_found = false;
    for (int i = 0; i < res.count; i++) {
        prompt_found = prompt_found || strcmp(res.items[i].id, prompt_id) == 0;
        ASSERT(strcmp(res.items[i].id, secret_id) != 0);
        ASSERT(strcmp(res.items[i].id, other_id) != 0);
        ASSERT(strcmp(res.items[i].id, bob_id) != 0);
    }
    ASSERT(prompt_found);
    cbm_store_memory_result_free(&res);

    free(normal_id);
    free(prompt_id);
    free(secret_id);
    free(other_id);
    free(bob_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_conflict_resolution) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t base = {0};
    base.kind = "fact";
    base.layer = "semantic";
    base.content = "Service timeout is 30 seconds";
    base.scope_project = "test-proj";
    base.entity_key = "service.timeout";
    base.predicate = "is";
    base.status = "active";
    base.confidence = 0.95;
    base.updated_at = 1000;
    char *base_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &base, &base_id) == CBM_STORE_OK);

    cbm_memory_item_t task = {0};
    task.kind = "fact";
    task.layer = "semantic";
    task.content = "Service timeout is 45 seconds for import task";
    task.scope_project = "test-proj";
    task.scope_task = "import";
    task.entity_key = "service.timeout";
    task.predicate = "is";
    task.status = "active";
    task.confidence = 0.80;
    task.updated_at = 2000;
    char *task_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &task, &task_id) == CBM_STORE_OK);

    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) "
                              "VALUES ('edge-1',?1,?2,'contradicts',1.0,'test',1.0,1),"
                              "('edge-2',?2,?1,'contradicts',1.0,'test',1.0,1);",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, base_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.entity_key = "service.timeout";
    q.task = "import";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, task_id) == 0);
    ASSERT(res.items[0].conflict_count == 1);
    ASSERT(strstr(res.items[0].conflict_ids, base_id) != NULL);
    ASSERT(res.items[0].conflict_resolution != NULL);
    ASSERT(strstr(res.items[0].conflict_resolution, "winner_by_scope") != NULL);
    cbm_store_memory_result_free(&res);

    memset(&q, 0, sizeof(q));
    q.project = "test-proj";
    q.entity_key = "service.timeout";
    q.limit = 5;
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, base_id) == 0);
    ASSERT(res.items[0].conflict_count == 1);
    ASSERT(strstr(res.items[0].conflict_ids, task_id) != NULL);
    ASSERT(res.items[0].conflict_resolution != NULL);
    ASSERT(strstr(res.items[0].conflict_resolution, "winner_by_confidence") != NULL);
    cbm_store_memory_result_free(&res);

    free(base_id);
    free(task_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_negative_evidence_prefers_supported_correction) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t claim = {0};
    claim.kind = "fact";
    claim.content = "fixed dataset may contain real credentials";
    claim.scope_project = "test-proj";
    claim.entity_key = "hardening:negative-evidence";
    claim.status = "candidate";
    claim.confidence = 0.3;
    char *claim_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &claim, &claim_id) == CBM_STORE_OK);

    cbm_memory_item_t counter = {0};
    counter.kind = "evidence";
    counter.content = "counterevidence says fixed dataset uses synthetic values only";
    counter.scope_project = "test-proj";
    counter.entity_key = "hardening:negative-evidence";
    counter.status = "candidate";
    counter.confidence = 0.99;
    char *counter_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &counter, &counter_id) == CBM_STORE_OK);

    cbm_memory_item_t corrected = {0};
    corrected.kind = "decision";
    corrected.content = "fixed dataset must use synthetic values and never real credentials";
    corrected.scope_project = "test-proj";
    corrected.entity_key = "hardening:negative-evidence";
    corrected.status = "candidate";
    corrected.confidence = 0.99;
    char *corrected_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &corrected, &corrected_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) VALUES "
                              "('negative-contradicts',?1,?2,'contradicts',1.0,'test',1.0,1),"
                              "('negative-supports',?1,?3,'supports',1.0,'test',1.0,1);",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, counter_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, claim_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, corrected_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "fixed dataset real credentials";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 2);
    ASSERT(strcmp(res.items[0].id, corrected_id) == 0);
    ASSERT(strcmp(res.items[1].id, counter_id) == 0);
    cbm_store_memory_result_free(&res);

    cbm_memory_item_t historical = {0};
    ASSERT(cbm_store_memory_get_item(s, claim_id, &historical) == CBM_STORE_OK);
    ASSERT(strcmp(historical.status, "active") == 0);
    cbm_store_memory_item_free(&historical);

    const char *event_id = "hardening-negative-wrong-event";
    char *first_id = NULL;
    char *replay_id = NULL;
    ASSERT(cbm_store_memory_feedback_idempotent(s, claim_id, "test-proj", "wrong",
                                                "contradicted by synthetic-only policy", "alice",
                                                event_id, &first_id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_feedback_idempotent(s, claim_id, "test-proj", "wrong",
                                                "contradicted by synthetic-only policy", "alice",
                                                event_id, &replay_id) == CBM_STORE_REPLAYED);
    ASSERT(strcmp(first_id, event_id) == 0 && strcmp(replay_id, event_id) == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE id='hardening-negative-wrong-event'") == 1);
    ASSERT(cbm_store_memory_get_item(s, claim_id, &historical) == CBM_STORE_OK);
    ASSERT(strcmp(historical.status, "retracted") == 0);
    cbm_store_memory_item_free(&historical);

    free(first_id);
    free(replay_id);
    free(claim_id);
    free(counter_id);
    free(corrected_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_evidence_graph) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t root = {0};
    root.kind = "decision";
    root.layer = "semantic";
    root.content = "Use SQLite as the only MVP memory store";
    root.scope_project = "test-proj";
    root.entity_key = "memory.storage";
    root.predicate = "decides";
    root.status = "active";
    root.confidence = 0.9;
    char *root_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &root, &root_id) == CBM_STORE_OK);

    cbm_memory_item_t support = {0};
    support.kind = "fact";
    support.layer = "semantic";
    support.content = "Single SQLite file keeps transactions simple";
    support.scope_project = "test-proj";
    support.entity_key = "memory.storage";
    support.predicate = "supports";
    support.status = "active";
    support.confidence = 0.8;
    char *support_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &support, &support_id) == CBM_STORE_OK);

    cbm_memory_item_t task = {0};
    task.kind = "task";
    task.layer = "episodic";
    task.content = "MVP implementation task";
    task.scope_project = "test-proj";
    task.entity_key = "memory.mvp";
    task.predicate = "used_in";
    task.status = "active";
    char *task_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &task, &task_id) == CBM_STORE_OK);

    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) VALUES "
                              "('edge-derived',?1,'evt-1','derived_from',1.0,'rule',1.0,1),"
                              "('edge-support',?2,?1,'supports',1.0,'test',0.9,1),"
                              "('edge-used',?2,?3,'used_in',1.0,'test',0.8,1);",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, root_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, support_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, task_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.entity_key = "memory.storage";
    q.kind = "decision";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(res.items[0].evidence_json != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"derived_from\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"supports\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"used_in\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"hop\":2") != NULL);
    cbm_store_memory_result_free(&res);

    free(root_id);
    free(support_id);
    free(task_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_supersedes_path_is_complete) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t old_item = {0};
    old_item.kind = "decision";
    old_item.content = "retention old policy thirty days";
    old_item.scope_project = "test-proj";
    old_item.entity_key = "hardening:retention-old";
    old_item.status = "candidate";
    char *old_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &old_item, &old_id) == CBM_STORE_OK);

    cbm_memory_item_t new_item = {0};
    new_item.kind = "decision";
    new_item.content = "retention current policy forty five days";
    new_item.scope_project = "test-proj";
    new_item.entity_key = "hardening:retention-current";
    new_item.status = "candidate";
    new_item.supersedes = old_id;
    char *new_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &new_item, &new_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "UPDATE memory_item SET status='archived' WHERE id=?1;", -1,
                              &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, old_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) VALUES "
                              "('hardening-supersedes',?1,?2,'supersedes',1.0,'test',1.0,1);",
                              -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, new_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, old_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "retention current policy";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    ASSERT(strcmp(res.items[0].id, new_id) == 0);
    ASSERT(res.items[0].evidence_json != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"type\":\"supersedes\"") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, new_id) != NULL);
    ASSERT(strstr(res.items[0].evidence_json, old_id) != NULL);
    for (int i = 0; i < res.count; i++) {
        ASSERT(strcmp(res.items[i].id, old_id) != 0);
    }
    cbm_store_memory_result_free(&res);

    cbm_memory_item_t old_history = {0};
    cbm_memory_item_t new_history = {0};
    ASSERT(cbm_store_memory_get_item(s, old_id, &old_history) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_get_item(s, new_id, &new_history) == CBM_STORE_OK);
    ASSERT(strcmp(old_history.status, "archived") == 0);
    ASSERT(new_history.supersedes != NULL && strcmp(new_history.supersedes, old_id) == 0);
    cbm_store_memory_item_free(&old_history);
    cbm_store_memory_item_free(&new_history);

    free(old_id);
    free(new_id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_retrieve_graph_expansion_is_bounded_three_hops) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    const char *contents[] = {
        "hardening outcome unique root",
        "hardening expected rule",
        "hardening source fact",
        "hardening terminal evidence",
        "hardening branch merge"
    };
    char *ids[5] = {0};
    for (int i = 0; i < 5; i++) {
        cbm_memory_item_t item = {0};
        item.kind = i == 1 ? "decision" : "fact";
        item.content = contents[i];
        item.scope_project = "test-proj";
        item.status = "candidate";
        ASSERT(cbm_store_memory_append_candidate(s, &item, &ids[i]) == CBM_STORE_OK);
    }
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    sqlite3_stmt *stmt = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
                              "INSERT INTO memory_edge "
                              "(id,src_id,dst_id,type,weight,origin,confidence,created_at) VALUES "
                              "('mh-used',?1,?2,'used_in',1.0,'test',1.0,1),"
                              "('mh-support',?2,?3,'supports',1.0,'test',1.0,2),"
                              "('mh-derived',?4,?3,'derived_from',1.0,'test',1.0,3),"
                              "('mh-cycle',?4,?1,'supports',1.0,'test',1.0,4),"
                              "('mh-branch-a',?2,?5,'supports',1.0,'test',1.0,5),"
                              "('mh-branch-b',?3,?5,'used_in',1.0,'test',1.0,6);",
                              -1, &stmt, NULL) == SQLITE_OK);
    for (int i = 0; i < 5; i++) {
        sqlite3_bind_text(stmt, i + 1, ids[i], -1, SQLITE_TRANSIENT);
    }
    ASSERT(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    int edge_count_before = scalar_int(s, "SELECT COUNT(*) FROM memory_edge");
    int edge_weight_before = scalar_int(s, "SELECT CAST(SUM(weight * 1000) AS INTEGER) FROM memory_edge");

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "hardening outcome unique root";
    q.limit = 5;
    cbm_memory_result_t first = {0};
    cbm_memory_result_t second = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &first) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_retrieve(s, &q, &second) == CBM_STORE_OK);
    ASSERT(first.count == 5 && second.count == 5);
    bool rule_found = false;
    bool source_found = false;
    bool terminal_found = false;
    for (int i = 0; i < first.count; i++) {
        ASSERT(strcmp(first.items[i].id, second.items[i].id) == 0);
        for (int j = i + 1; j < first.count; j++) {
            ASSERT(strcmp(first.items[i].id, first.items[j].id) != 0);
        }
        rule_found = rule_found || strcmp(first.items[i].id, ids[1]) == 0;
        source_found = source_found || strcmp(first.items[i].id, ids[2]) == 0;
        terminal_found = terminal_found || strcmp(first.items[i].id, ids[3]) == 0;
    }
    ASSERT(rule_found && source_found && terminal_found);
    ASSERT(first.items[0].evidence_json != NULL);
    ASSERT(strstr(first.items[0].evidence_json, "\"hop\":3") != NULL);
    ASSERT(strstr(first.items[0].evidence_json, "\"hop\":4") == NULL);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == edge_count_before);
    ASSERT(scalar_int(s, "SELECT CAST(SUM(weight * 1000) AS INTEGER) FROM memory_edge") == edge_weight_before);
    cbm_store_memory_result_free(&first);
    cbm_store_memory_result_free(&second);

    for (int i = 0; i < 5; i++) {
        free(ids[i]);
    }
    cbm_store_close(s);
    return 0;
}

static const cbm_memory_activation_candidate_t *stage6_find_activation_candidate(
    const cbm_memory_result_t *result, const char *item_id) {
    if (!result || !item_id) {
        return NULL;
    }
    for (int i = 0; i < result->activation.candidate_count; i++) {
        if (result->activation.candidates[i].item_id &&
            strcmp(result->activation.candidates[i].item_id, item_id) == 0) {
            return &result->activation.candidates[i];
        }
    }
    return NULL;
}

static bool stage6_result_contains_item(const cbm_memory_result_t *result,
                                        const char *item_id) {
    if (!result || !item_id) {
        return false;
    }
    for (int i = 0; i < result->count; i++) {
        if (result->items[i].id && strcmp(result->items[i].id, item_id) == 0) {
            return true;
        }
    }
    return false;
}

TEST(stage6_shadow_multi_seed_merge_scope_cycle_and_budget) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    const char *contents[] = {
        "stage6 direct bridge", "stage6 direct bridge", "branch alpha unrelated",
        "branch beta unrelated", "converged indirect target", "three hop evidence",
        "wrong project node", "wrong user node", "wrong task node", "unsafe canary node"
    };
    char *ids[10] = {0};
    for (int i = 0; i < 10; i++) {
        cbm_memory_item_t item = {0};
        item.kind = i == 9 ? "safety_fixture" : "fact";
        item.content = contents[i];
        item.scope_project = i == 6 ? "other-project" : "stage6-fixture-shadow";
        item.scope_user = i == 7 ? "bob" : "alice";
        item.scope_task = i == 8 ? "other-task" : "task-a";
        item.entity_key = i == 9 ? "fixture:unsafe-canary" : NULL;
        item.status = "candidate";
        item.version = 1;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &ids[i]) == CBM_STORE_OK);
    }
    ASSERT(insert_test_memory_fts(s, ids[0], contents[0]) == SQLITE_OK);
    ASSERT(insert_test_memory_fts(s, ids[1], contents[1]) == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-seed-a", ids[0], ids[2], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-seed-b", ids[1], ids[3], "derived_from") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-merge-a", ids[2], ids[4], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-merge-b", ids[3], ids[4], "used_in") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-tail", ids[4], ids[5], "derived_from") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-cycle", ids[0], ids[5], "supersedes") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-wrong-project", ids[2], ids[6], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-wrong-user", ids[2], ids[7], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-wrong-task", ids[2], ids[8], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-unsafe", ids[2], ids[9], "supports") == SQLITE_OK);
    int edges_before = scalar_int(s, "SELECT COUNT(*) FROM memory_edge");

    cbm_memory_query_t query = {0};
    query.project = "stage6-fixture-shadow";
    query.user = "alice";
    query.task = "task-a";
    query.query = "stage6 direct bridge";
    query.limit = 10;
    query.activation_mode = "shadow";
    query.activation_session_id = "stage6-shadow-session-1";
    query.activation_max_hops = 3;
    query.activation_max_nodes = 8;
    query.activation_max_visits = 32;
    query.activation_token_budget = 2048;
    query.activation_latency_ms = 25;
    cbm_memory_result_t result = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_OK);
    ASSERT(result.activation.status != NULL);
    ASSERT(strcmp(result.activation.status, "completed") == 0);
    ASSERT(strcmp(result.activation.session_id, "stage6-shadow-session-1") == 0);
    if (result.activation.seed_count != 2) {
        fprintf(stderr, "stage6 seeds=%d stage5_count=%d seed0=%s seed1=%s fts_rows=%d\n",
                result.activation.seed_count, result.count, ids[0], ids[1],
                scalar_int(s, "SELECT COUNT(*) FROM memory_fts"));
        for (int i = 0; i < result.count; i++) {
            fprintf(stderr, "  item=%s source=%s content=%s\n", result.items[i].id,
                    result.items[i].retrieval_source, result.items[i].content);
        }
    }
    ASSERT(result.activation.seed_count == 2);
    ASSERT(result.activation.max_hop_observed == 3);
    ASSERT(result.activation.edge_visits <= 32);
    ASSERT(result.activation.node_count <= 8);
    ASSERT(result.activation.token_proxy <= 2048);
    const cbm_memory_activation_candidate_t *merge =
        stage6_find_activation_candidate(&result, ids[4]);
    const cbm_memory_activation_candidate_t *tail =
        stage6_find_activation_candidate(&result, ids[5]);
    ASSERT(merge != NULL && merge->predecessor_count == 2);
    ASSERT(tail != NULL && tail->hop == 3);
    ASSERT(merge->candidate_id && merge->path_id && merge->evidence_id);
    ASSERT(merge->explanation_json && strstr(merge->explanation_json, "supports"));
    ASSERT(stage6_find_activation_candidate(&result, ids[6]) == NULL);
    ASSERT(stage6_find_activation_candidate(&result, ids[7]) == NULL);
    ASSERT(stage6_find_activation_candidate(&result, ids[8]) == NULL);
    ASSERT(stage6_find_activation_candidate(&result, ids[9]) == NULL);
    ASSERT(result.activation.scope_rejections >= 3);
    ASSERT(result.activation.unsafe_rejections >= 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == edges_before);
    cbm_store_memory_result_free(&result);

    for (int i = 0; i < 10; i++) {
        free(ids[i]);
    }
    cbm_store_close(s);
    return 0;
}

TEST(stage6_off_shadow_equivalence_and_vector_seed_policy) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t seed = {0};
    seed.kind = "fact";
    seed.content = "stage6 vector only seed";
    seed.scope_project = "stage6-fixture-vector";
    seed.status = "candidate";
    char *seed_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &seed, &seed_id) == CBM_STORE_OK);
    cbm_memory_item_t target = seed;
    target.content = "indirect target unavailable to direct text";
    char *target_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &target, &target_id) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "stage6-fixture-vector", 100, &processed) ==
           CBM_STORE_OK);
    sqlite3_stmt *del = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s), "DELETE FROM memory_fts WHERE item_id=?1;",
                              -1, &del, NULL) == SQLITE_OK);
    sqlite3_bind_text(del, 1, seed_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(del) == SQLITE_DONE);
    sqlite3_finalize(del);
    ASSERT(insert_test_memory_edge(s, "s6-vector-edge", seed_id, target_id, "supports") ==
           SQLITE_OK);

    cbm_memory_query_t off_query = {0};
    off_query.project = "stage6-fixture-vector";
    off_query.query = "stage6 vector only seed";
    off_query.limit = 5;
    cbm_memory_result_t off = {0};
    ASSERT(cbm_store_memory_retrieve(s, &off_query, &off) == CBM_STORE_OK);
    ASSERT(off.activation.status == NULL);

    cbm_memory_query_t shadow_query = off_query;
    shadow_query.activation_mode = "shadow";
    shadow_query.activation_session_id = "stage6-vector-shadow";
    cbm_memory_result_t shadow = {0};
    ASSERT(cbm_store_memory_retrieve(s, &shadow_query, &shadow) == CBM_STORE_OK);
    ASSERT(shadow.count == off.count);
    for (int i = 0; i < off.count; i++) {
        ASSERT(strcmp(off.items[i].id, shadow.items[i].id) == 0);
        ASSERT(off.items[i].retrieval_score == shadow.items[i].retrieval_score);
    }
    ASSERT(shadow.activation.vector_seeds_blocked >= 1);
    ASSERT(shadow.activation.seed_count == 0);
    ASSERT(shadow.activation.candidate_count == 0);
    cbm_store_memory_result_free(&off);
    cbm_store_memory_result_free(&shadow);
    free(seed_id);
    free(target_id);
    cbm_store_close(s);
    return 0;
}

TEST(stage6_active_fixture_guard_and_failure_atomicity) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t seed = {0};
    seed.kind = "fact";
    seed.content = "stage6 active direct";
    seed.scope_project = "stage6-fixture-active";
    seed.status = "candidate";
    char *seed_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &seed, &seed_id) == CBM_STORE_OK);
    cbm_memory_item_t target = seed;
    target.content = "stage6 isolated indirect";
    char *target_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &target, &target_id) == CBM_STORE_OK);
    ASSERT(insert_test_memory_fts(s, seed_id, seed.content) == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-active-edge", seed_id, target_id, "supports") ==
           SQLITE_OK);

    cbm_memory_query_t query = {0};
    query.project = "stage6-fixture-active";
    query.query = "stage6 active direct";
    query.limit = 5;
    query.activation_mode = "active";
    query.activation_session_id = "stage6-active-session";
    cbm_memory_result_t result = {0};
#ifdef _WIN32
    _putenv("CBM_STAGE6_ACTIVE_FIXTURE=");
#else
    unsetenv("CBM_STAGE6_ACTIVE_FIXTURE");
#endif
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_ERR);
    ASSERT(result.count == 0);
    cbm_store_memory_result_free(&result);

#ifdef _WIN32
    _putenv("CBM_STAGE6_ACTIVE_FIXTURE=1");
#else
    setenv("CBM_STAGE6_ACTIVE_FIXTURE", "1", 1);
#endif
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_OK);
    ASSERT(stage6_find_activation_candidate(&result, target_id) != NULL);
    cbm_store_memory_result_free(&result);

    query.activation_mode = "shadow";
    query.activation_failpoint = "after_first_candidate";
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_OK);
    ASSERT(result.count > 0);
    ASSERT(result.activation.status != NULL && strcmp(result.activation.status, "failed") == 0);
    ASSERT(result.activation.candidate_count == 0);
    cbm_store_memory_result_free(&result);

    query.activation_mode = "active";
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_ERR);
    ASSERT(result.count == 0);
    ASSERT(result.activation.candidate_count == 0);
    cbm_store_memory_result_free(&result);
#ifdef _WIN32
    _putenv("CBM_STAGE6_ACTIVE_FIXTURE=");
#else
    unsetenv("CBM_STAGE6_ACTIVE_FIXTURE");
#endif
    free(seed_id);
    free(target_id);
    cbm_store_close(s);
    return 0;
}

TEST(stage6_structured_and_code_anchor_seed_fusion) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);

    cbm_memory_item_t structured_seed = {0};
    structured_seed.kind = "fact";
    structured_seed.content = "structured scope seed";
    structured_seed.scope_project = "stage6-fixture-structured";
    structured_seed.entity_key = "stage6:structured-seed";
    structured_seed.status = "candidate";
    char *structured_seed_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &structured_seed, &structured_seed_id) ==
           CBM_STORE_OK);
    cbm_memory_item_t structured_target = structured_seed;
    structured_target.content = "indirect structured target with no direct key";
    structured_target.entity_key = "stage6:structured-target";
    char *structured_target_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &structured_target, &structured_target_id) ==
           CBM_STORE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-structured-edge", structured_seed_id,
                                   structured_target_id, "supports") == SQLITE_OK);

    cbm_memory_query_t structured_query = {0};
    structured_query.project = "stage6-fixture-structured";
    structured_query.entity_key = "stage6:structured-seed";
    structured_query.limit = 5;
    structured_query.activation_mode = "shadow";
    structured_query.activation_session_id = "stage6-structured-session";
    cbm_memory_result_t structured = {0};
    ASSERT(cbm_store_memory_retrieve(s, &structured_query, &structured) == CBM_STORE_OK);
    ASSERT(structured.count == 1);
    ASSERT(strcmp(structured.items[0].id, structured_seed_id) == 0);
    ASSERT(stage6_find_activation_candidate(&structured, structured_target_id) != NULL);
    ASSERT(!stage6_result_contains_item(&structured, structured_target_id));
    cbm_store_memory_result_free(&structured);

    cbm_memory_item_t code_seed = {0};
    code_seed.kind = "fact";
    code_seed.content = "code anchor vector seed phrase";
    code_seed.scope_project = "stage6-fixture-code-anchor";
    code_seed.status = "candidate";
    char *code_seed_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &code_seed, &code_seed_id) == CBM_STORE_OK);
    int code_processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "stage6-fixture-code-anchor", 100,
                                        &code_processed) == CBM_STORE_OK);
    sqlite3_stmt *delete_fts = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s), "DELETE FROM memory_fts WHERE item_id=?1;",
                              -1, &delete_fts, NULL) == SQLITE_OK);
    sqlite3_bind_text(delete_fts, 1, code_seed_id, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(delete_fts) == SQLITE_DONE);
    sqlite3_finalize(delete_fts);
    cbm_memory_item_t code_target = code_seed;
    code_target.content = "indirect code target outside direct vector set";
    char *code_target_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &code_target, &code_target_id) == CBM_STORE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-code-anchor", code_seed_id, "code:Fixture.fn",
                                   "about_code") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-code-target", code_seed_id, code_target_id,
                                   "supports") == SQLITE_OK);
    sqlite3 *graph_db = NULL;
    ASSERT(sqlite3_open(":memory:", &graph_db) == SQLITE_OK);
    ASSERT(sqlite3_exec(graph_db,
                        "CREATE TABLE nodes(project TEXT,qualified_name TEXT,file_path TEXT);"
                        "INSERT INTO nodes VALUES('stage6-fixture-code-anchor','Fixture.fn',"
                        "'fixture.c');",
                        NULL, NULL, NULL) == SQLITE_OK);
    cbm_memory_query_t code_query = {0};
    code_query.project = "stage6-fixture-code-anchor";
    code_query.query = "code anchor vector seed phrase";
    code_query.code_context = "Fixture.fn";
    code_query.graph_db = graph_db;
    code_query.limit = 5;
    cbm_memory_result_t code_off = {0};
    ASSERT(cbm_store_memory_retrieve(s, &code_query, &code_off) == CBM_STORE_OK);
    ASSERT(stage6_result_contains_item(&code_off, code_seed_id));
    ASSERT(!stage6_result_contains_item(&code_off, code_target_id));
    cbm_store_memory_result_free(&code_off);

    code_query.activation_mode = "shadow";
    code_query.activation_session_id = "stage6-code-anchor-session";
    cbm_memory_result_t code_shadow = {0};
    ASSERT(cbm_store_memory_retrieve(s, &code_query, &code_shadow) == CBM_STORE_OK);
    ASSERT(code_shadow.activation.seed_count == 1);
    ASSERT(stage6_find_activation_candidate(&code_shadow, code_target_id) != NULL);
    ASSERT(!stage6_result_contains_item(&code_shadow, code_target_id));
    cbm_store_memory_result_free(&code_shadow);
    sqlite3_close(graph_db);

    free(structured_seed_id);
    free(structured_target_id);
    free(code_seed_id);
    free(code_target_id);
    cbm_store_close(s);
    return 0;
}

TEST(stage6_relation_direction_version_and_unsafe_guards) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    const char *project = "stage6-fixture-guards";
    const char *contents[] = {
        "stage6 guard direct seed", "negative correction evidence", "blocked old claim",
        "new superseding decision", "blocked supersedes history", "stale entity version",
        "current entity version", "prompt injection fixture", "secret fixture", "pii fixture",
        "canary fixture", "unsafe fixture"
    };
    const char *entity_keys[] = {
        "stage6:guard-seed", "stage6:negative-correction", "stage6:reverse-contradicts",
        "stage6:new-decision", "stage6:reverse-supersedes", "stage6:versioned",
        "stage6:versioned", "fixture:prompt_injection", "fixture:secret",
        "fixture:pii", "fixture:canary", "fixture:unsafe"
    };
    char *ids[12] = {0};
    for (int i = 0; i < 12; i++) {
        cbm_memory_item_t item = {0};
        item.kind = i >= 7 ? "safety_fixture" : "fact";
        item.content = contents[i];
        item.scope_project = project;
        item.scope_user = "alice";
        item.scope_task = "task-a";
        item.entity_key = entity_keys[i];
        item.status = "candidate";
        item.version = i == 6 ? 2 : 1;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &ids[i]) == CBM_STORE_OK);
    }
    ASSERT(insert_test_memory_fts(s, ids[0], contents[0]) == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-negative-claim", ids[0], ids[2], "supports") ==
           SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-negative-forward", ids[1], ids[2],
                                   "contradicts") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-super-old", ids[0], ids[4], "supports") ==
           SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-super-forward", ids[3], ids[4], "supersedes") ==
           SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-version-old", ids[0], ids[5], "supports") ==
           SQLITE_OK);
    for (int i = 7; i < 12; i++) {
        char edge_id[64];
        snprintf(edge_id, sizeof(edge_id), "s6-unsafe-%d", i);
        ASSERT(insert_test_memory_edge(s, edge_id, ids[0], ids[i], "supports") == SQLITE_OK);
    }
    int edges_before = scalar_int(s, "SELECT COUNT(*) FROM memory_edge");

    cbm_memory_query_t query = {0};
    query.project = project;
    query.user = "alice";
    query.task = "task-a";
    query.query = "stage6 guard direct seed";
    query.limit = 20;
    query.activation_mode = "shadow";
    query.activation_session_id = "stage6-guard-session";
    query.activation_max_hops = 2;
    cbm_memory_result_t result = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &result) == CBM_STORE_OK);
    const cbm_memory_activation_candidate_t *negative =
        stage6_find_activation_candidate(&result, ids[1]);
    const cbm_memory_activation_candidate_t *superseding =
        stage6_find_activation_candidate(&result, ids[3]);
    ASSERT(negative != NULL && strstr(negative->explanation_json, "contradicts") != NULL);
    ASSERT(superseding != NULL && strstr(superseding->explanation_json, "supersedes") != NULL);
    ASSERT(negative->hop == 2 && superseding->hop == 2);
    ASSERT(stage6_find_activation_candidate(&result, ids[2]) != NULL);
    ASSERT(stage6_find_activation_candidate(&result, ids[4]) != NULL);
    ASSERT(stage6_find_activation_candidate(&result, ids[5]) == NULL);
    ASSERT(result.activation.version_rejections >= 1);
    ASSERT(result.activation.unsafe_rejections == 5);
    for (int i = 7; i < 12; i++) {
        ASSERT(stage6_find_activation_candidate(&result, ids[i]) == NULL);
        ASSERT(!stage6_result_contains_item(&result, ids[i]));
    }
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == edges_before);
    cbm_store_memory_result_free(&result);

    for (int i = 0; i < 12; i++) {
        free(ids[i]);
    }
    cbm_store_close(s);
    return 0;
}

TEST(stage6_hop_budget_timeout_and_stable_ids) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    const char *contents[] = {"stage6 budget direct seed", "first hop candidate payload",
                              "second hop candidate payload", "third hop candidate payload"};
    char *ids[4] = {0};
    for (int i = 0; i < 4; i++) {
        cbm_memory_item_t item = {0};
        item.kind = "fact";
        item.content = contents[i];
        item.scope_project = "stage6-fixture-budgets";
        item.status = "candidate";
        ASSERT(cbm_store_memory_append_candidate(s, &item, &ids[i]) == CBM_STORE_OK);
    }
    ASSERT(insert_test_memory_fts(s, ids[0], contents[0]) == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-budget-1", ids[0], ids[1], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-budget-2", ids[1], ids[2], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s6-budget-3", ids[2], ids[3], "supports") == SQLITE_OK);

    cbm_memory_query_t query = {0};
    query.project = "stage6-fixture-budgets";
    query.query = "stage6 budget direct seed";
    query.limit = 10;
    query.activation_mode = "shadow";
    query.activation_max_hops = 1;
    query.activation_session_id = "stage6-hop-1";
    cbm_memory_result_t hop1 = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &hop1) == CBM_STORE_OK);
    const cbm_memory_activation_candidate_t *first =
        stage6_find_activation_candidate(&hop1, ids[1]);
    ASSERT(first != NULL && first->hop == 1 && first->score > 0.719 && first->score < 0.721);
    ASSERT(stage6_find_activation_candidate(&hop1, ids[2]) == NULL);
    ASSERT(hop1.activation.max_hop_observed == 1);
    cbm_store_memory_result_free(&hop1);

    query.activation_max_hops = 2;
    query.activation_session_id = "stage6-hop-2";
    cbm_memory_result_t hop2 = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &hop2) == CBM_STORE_OK);
    const cbm_memory_activation_candidate_t *second =
        stage6_find_activation_candidate(&hop2, ids[2]);
    ASSERT(second != NULL && second->hop == 2 && second->score > 0.517 && second->score < 0.520);
    ASSERT(stage6_find_activation_candidate(&hop2, ids[3]) == NULL);
    cbm_store_memory_result_free(&hop2);

    query.activation_max_hops = 3;
    query.activation_session_id = "stage6-stable-session";
    cbm_memory_result_t hop3a = {0};
    cbm_memory_result_t hop3b = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &hop3a) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_retrieve(s, &query, &hop3b) == CBM_STORE_OK);
    const cbm_memory_activation_candidate_t *third_a =
        stage6_find_activation_candidate(&hop3a, ids[3]);
    const cbm_memory_activation_candidate_t *third_b =
        stage6_find_activation_candidate(&hop3b, ids[3]);
    ASSERT(third_a != NULL && third_b != NULL && third_a->hop == 3);
    ASSERT(strcmp(third_a->candidate_id, third_b->candidate_id) == 0);
    ASSERT(strcmp(third_a->path_id, third_b->path_id) == 0);
    ASSERT(strcmp(third_a->evidence_id, third_b->evidence_id) == 0);
    cbm_store_memory_result_free(&hop3a);
    cbm_store_memory_result_free(&hop3b);

    query.activation_max_hops = 3;
    query.activation_max_visits = 1;
    query.activation_session_id = "stage6-visit-budget";
    cbm_memory_result_t visit = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &visit) == CBM_STORE_OK);
    ASSERT(visit.activation.budget_exhausted);
    ASSERT(strcmp(visit.activation.termination_reason, "visit_budget") == 0);
    ASSERT(visit.activation.edge_visits == 1);
    cbm_store_memory_result_free(&visit);

    query.activation_max_visits = 0;
    query.activation_max_nodes = 1;
    query.activation_session_id = "stage6-node-budget";
    cbm_memory_result_t node = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &node) == CBM_STORE_OK);
    ASSERT(node.activation.budget_exhausted);
    ASSERT(strcmp(node.activation.termination_reason, "node_budget") == 0);
    ASSERT(node.activation.candidate_count == 0);
    cbm_store_memory_result_free(&node);

    query.activation_max_nodes = 0;
    query.activation_token_budget = 1;
    query.activation_session_id = "stage6-token-budget";
    cbm_memory_result_t token = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &token) == CBM_STORE_OK);
    ASSERT(token.activation.budget_exhausted);
    ASSERT(strcmp(token.activation.termination_reason, "token_budget") == 0);
    ASSERT(token.activation.candidate_count == 0);
    cbm_store_memory_result_free(&token);

    query.activation_token_budget = 0;
    query.activation_failpoint = "timeout_before_expand";
    query.activation_session_id = "stage6-timeout";
    cbm_memory_result_t timeout = {0};
    ASSERT(cbm_store_memory_retrieve(s, &query, &timeout) == CBM_STORE_OK);
    ASSERT(timeout.activation.budget_exhausted);
    ASSERT(strcmp(timeout.activation.termination_reason, "timeout") == 0);
    ASSERT(timeout.activation.edge_visits == 0);
    ASSERT(timeout.activation.candidate_count == 0);
    cbm_store_memory_result_free(&timeout);

    for (int i = 0; i < 4; i++) {
        free(ids[i]);
    }
    cbm_store_close(s);
    return 0;
}

TEST(memory_consolidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Consolidation test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = -1;
    int rc = cbm_store_memory_consolidate(s, "test-proj", 100, &processed);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(processed == 1);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    ASSERT(out.entity_key != NULL);
    ASSERT(out.predicate != NULL);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='belongs_to'") == 1);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_consolidate_merge_keeps_new_event_evidence) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t active = {0};
    active.kind = "fact";
    active.layer = "semantic";
    active.content = "Use SQLite for memory storage";
    active.scope_project = "test-proj";
    active.entity_key = "memory.storage";
    active.predicate = "decides";
    active.status = "active";
    char *active_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &active, &active_id) == CBM_STORE_OK);

    cbm_memory_item_t candidate = {0};
    candidate.kind = "fact";
    candidate.layer = "semantic";
    candidate.content = "Use SQLite for memory storage";
    candidate.scope_project = "test-proj";
    candidate.entity_key = "memory.storage";
    candidate.predicate = "decides";
    candidate.status = "candidate";
    candidate.source_event_ids = "[\"evt-merge-support\"]";
    char *candidate_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &candidate, &candidate_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);

    /* Archived candidate: supersedes must be NULL (it is retired, not superseding). */
    cbm_memory_item_t archived = {0};
    ASSERT(cbm_store_memory_get_item(s, candidate_id, &archived) == CBM_STORE_OK);
    ASSERT(strcmp(archived.status, "archived") == 0);
    ASSERT(archived.supersedes == NULL || archived.supersedes[0] == '\0');
    cbm_store_memory_item_free(&archived);

    /* Surviving active item: supersedes must point to the retired candidate. */
    cbm_memory_item_t survivor = {0};
    ASSERT(cbm_store_memory_get_item(s, active_id, &survivor) == CBM_STORE_OK);
    ASSERT(survivor.supersedes != NULL && strcmp(survivor.supersedes, candidate_id) == 0);
    cbm_store_memory_item_free(&survivor);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.entity_key = "memory.storage";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].id, active_id) == 0);
    ASSERT(res.items[0].evidence_json != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "evt-merge-support") != NULL);
    ASSERT(strstr(res.items[0].evidence_json, "\"origin\":\"merge\"") != NULL);
    cbm_store_memory_result_free(&res);

    free(active_id);
    free(candidate_id);
    cbm_store_close(s);
    return 0;
}


TEST(memory_health) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_health_t h = {0};
    int rc = cbm_store_memory_health(s, "test-proj", &h);
    ASSERT(rc == CBM_STORE_OK);
    ASSERT(h.item_count >= 0);
    ASSERT(h.hit_rate >= 0.0);
    cbm_store_close(s);
    return 0;
}

TEST(memory_mark_hits) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Hit counter test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    const char *ids[] = {id};
    ASSERT(cbm_store_memory_mark_hits(s, ids, 1, 0) == CBM_STORE_OK);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(out.hit_count == 1);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_update_status_retracts_from_default_retrieval) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact";
    item.content = "Retracted memory should be hidden";
    item.scope_project = "test-proj";
    item.status = "active";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_update_status(s, id, "test-proj", "retracted") == CBM_STORE_OK);

    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 0);
    cbm_store_memory_result_free(&res);

    q.include_inactive = true;
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].status, "retracted") == 0);
    cbm_store_memory_result_free(&res);

    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_update_status_rejects_invalid_status) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Invalid status test";
    item.scope_project = "test-proj";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_update_status(s, id, "test-proj", "deleted") == CBM_STORE_ERR);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_feedback_useful_records_event_and_boosts_hit_signal) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Useful feedback target";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.5;
    item.reusability = 0.5;
    item.decay = 0.2;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    char *event_id = NULL;
    ASSERT(cbm_store_memory_feedback(s, id, "test-proj", "useful", "helped answer", "alice", &event_id) == CBM_STORE_OK);
    ASSERT(event_id != NULL);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(out.hit_count == 1);
    ASSERT(out.confidence > 0.54);
    ASSERT(out.reusability > 0.54);
    ASSERT(out.decay < 0.11);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") == 1);
    cbm_store_memory_item_free(&out);
    free(event_id);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_feedback_idempotent_exact_replay_is_side_effect_free) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Idempotent feedback replay target";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.5;
    item.reusability = 0.5;
    item.decay = 0.2;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    const char *requested_event_id = "feedback-idempotent-replay";
    char *event_id_first = NULL;
    ASSERT(cbm_store_memory_feedback_idempotent(
               s, id, "test-proj", "useful", "helped answer", "alice",
               requested_event_id, &event_id_first) == CBM_STORE_OK);
    ASSERT(event_id_first != NULL);
    ASSERT(strcmp(event_id_first, requested_event_id) == 0);

    cbm_memory_item_t after_first = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &after_first) == CBM_STORE_OK);
    ASSERT(after_first.hit_count == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event "
                         "WHERE id='feedback-idempotent-replay'") == 1);

    char *event_id_replay = NULL;
    ASSERT(cbm_store_memory_feedback_idempotent(
               s, id, "test-proj", "useful", "helped answer", "alice",
               requested_event_id, &event_id_replay) == CBM_STORE_REPLAYED);
    ASSERT(event_id_replay != NULL);
    ASSERT(strcmp(event_id_replay, requested_event_id) == 0);

    cbm_memory_item_t after_replay = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &after_replay) == CBM_STORE_OK);
    ASSERT(after_replay.hit_count == after_first.hit_count);
    ASSERT(after_replay.confidence == after_first.confidence);
    ASSERT(after_replay.reusability == after_first.reusability);
    ASSERT(after_replay.importance == after_first.importance);
    ASSERT(after_replay.decay == after_first.decay);
    ASSERT(after_replay.last_hit_at == after_first.last_hit_at);
    ASSERT(after_replay.updated_at == after_first.updated_at);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event "
                         "WHERE id='feedback-idempotent-replay'") == 1);

    cbm_store_memory_item_free(&after_replay);
    cbm_store_memory_item_free(&after_first);
    free(event_id_replay);
    free(event_id_first);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_feedback_idempotency_conflict_is_side_effect_free) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Idempotent feedback conflict target";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.5;
    item.reusability = 0.5;
    item.decay = 0.2;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    const char *requested_event_id = "feedback-idempotent-conflict";
    char *event_id_first = NULL;
    ASSERT(cbm_store_memory_feedback_idempotent(
               s, id, "test-proj", "useful", "canonical note A", "alice",
               requested_event_id, &event_id_first) == CBM_STORE_OK);
    ASSERT(event_id_first != NULL);
    ASSERT(strcmp(event_id_first, requested_event_id) == 0);

    cbm_memory_item_t before_conflict = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &before_conflict) == CBM_STORE_OK);
    int feedback_count_before =
        scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'");
    ASSERT(feedback_count_before == 1);

    char *event_id_conflict = NULL;
    ASSERT(cbm_store_memory_feedback_idempotent(
               s, id, "test-proj", "useful", "DIFFERENT canonical note B", "alice",
               requested_event_id, &event_id_conflict) == CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT(event_id_conflict != NULL);
    ASSERT(strcmp(event_id_conflict, requested_event_id) == 0);

    cbm_memory_item_t after_conflict = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &after_conflict) == CBM_STORE_OK);
    ASSERT(after_conflict.hit_count == before_conflict.hit_count);
    ASSERT(after_conflict.confidence == before_conflict.confidence);
    ASSERT(after_conflict.reusability == before_conflict.reusability);
    ASSERT(after_conflict.importance == before_conflict.importance);
    ASSERT(after_conflict.decay == before_conflict.decay);
    ASSERT(after_conflict.last_hit_at == before_conflict.last_hit_at);
    ASSERT(after_conflict.updated_at == before_conflict.updated_at);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") ==
           feedback_count_before);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event "
                         "WHERE id='feedback-idempotent-conflict'") == 1);

    cbm_store_memory_item_free(&after_conflict);
    cbm_store_memory_item_free(&before_conflict);
    free(event_id_conflict);
    free(event_id_first);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_feedback_wrong_retracts_from_default_retrieval) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Wrong feedback target";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.8;
    item.importance = 0.9; /* P4: a falsified high-importance item must lose importance */
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_feedback(s, id, "test-proj", "wrong", "contradicted by user", NULL, NULL) == CBM_STORE_OK);
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 0);
    cbm_store_memory_result_free(&res);
    q.include_inactive = true;
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 1);
    ASSERT(strcmp(res.items[0].status, "retracted") == 0);
    /* P4 zombie fix: importance collapsed (0.9 - 0.5 = 0.4), not left high. */
    ASSERT(res.items[0].importance < 0.5);
    cbm_store_memory_result_free(&res);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_decay_archives_stale) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Low confidence stale memory";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.1;
    item.reusability = 0.1;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_decay(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0 || strcmp(out.status, "archived") == 0);
    ASSERT(out.decay > 0.0);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* Regression for the decay-pass column swap (SELECT ...,importance,decay read
 * as ...,decay,importance): a maximum-importance item has a (1-importance)=0
 * decay increment, so one pass must leave decay AT ZERO and status active.
 * With the columns swapped, old_decay reads 1.0 from the importance column and
 * the item archives on the spot — the exact prod incident of 2026-07-09. */
TEST(memory_decay_max_importance_never_decays) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "Maximum importance memory must not decay";
    item.scope_project = "test-proj";
    item.status = "active";
    item.confidence = 0.1;  /* aggressive decay factors everywhere else... */
    item.reusability = 0.1;
    item.importance = 1.0;  /* ...but importance alone zeroes the increment */
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_decay(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    ASSERT(out.decay < 0.000001);
    cbm_store_memory_item_free(&out);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* The dedup vector must carry lexical-semantic signal: two near-identical
 * statements in the same (entity, predicate, scope) bucket should MERGE via the
 * cosine>=0.90 path during consolidation, not coexist. A whole-string hash
 * would make these orthogonal and never merge. */
TEST(memory_consolidate_merges_paraphrase) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t base = {0};
    base.kind = "preference";
    base.content = "user prefers dark mode for the editor theme";
    base.scope_project = "test-proj";
    base.entity_key = "user:theme";
    base.predicate = "prefers";
    base.status = "active";
    char *base_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &base, &base_id) == CBM_STORE_OK);

    /* Same fact, one extra word: high lexical overlap → cosine should clear 0.90. */
    cbm_memory_item_t dup = {0};
    dup.kind = "preference";
    dup.content = "user really prefers dark mode for the editor theme";
    dup.scope_project = "test-proj";
    dup.entity_key = "user:theme";
    dup.predicate = "prefers";
    dup.status = "candidate";
    char *dup_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &dup, &dup_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    cbm_memory_item_t merged = {0};
    ASSERT(cbm_store_memory_get_item(s, dup_id, &merged) == CBM_STORE_OK);
    ASSERT(strcmp(merged.status, "archived") == 0); /* merged away, not coexisting */
    cbm_store_memory_item_free(&merged);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='similar_to'") == 1);

    free(base_id);
    free(dup_id);
    cbm_store_close(s);
    return 0;
}

/* Divergent (non-equivalent, low-cosine) values under the SAME (entity,
 * predicate, scope) bucket must COEXIST after consolidation — NOT be merged and
 * NOT be auto-marked contradictory. P3-c (2026-06-30) removed the old
 * cosine<0.90 → contradicts auto-rule: a contradicts edge HIDES a memory from
 * recall, so it must come from explicit evidence, never a cosine guess (real
 * topic-clustered embeddings made the guess wrong — note the fixture below is
 * actually two ALIGNED anti-spaces decisions the old rule falsely opposed).
 * Read-time scope-aware adjudication ranks coexisting peers instead. */
TEST(memory_consolidate_keeps_lowcosine_peers_without_contradiction) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t a = {0};
    a.kind = "decision";
    a.content = "always indent using tabs";
    a.scope_project = "test-proj";
    a.entity_key = "style:indentation";
    a.predicate = "decides";
    a.status = "active";
    char *a_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &a, &a_id) == CBM_STORE_OK);

    cbm_memory_item_t b = {0};
    b.kind = "decision";
    b.content = "never use four whitespace characters";
    b.scope_project = "test-proj";
    b.entity_key = "style:indentation";
    b.predicate = "decides";
    b.status = "candidate";
    char *b_id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &b, &b_id) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    /* Not merged (low cosine) → coexists as active, and NO contradicts edge is
     * fabricated by the rule path (P3-c). */
    cbm_memory_item_t bout = {0};
    ASSERT(cbm_store_memory_get_item(s, b_id, &bout) == CBM_STORE_OK);
    ASSERT(strcmp(bout.status, "active") == 0); /* coexists, not archived */
    cbm_store_memory_item_free(&bout);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='contradicts' AND "
                         "origin='rule'") == 0);

    free(a_id);
    free(b_id);
    cbm_store_close(s);
    return 0;
}

/* A successful recall must let accumulated decay fall back (framework §11.2),
 * not merely refresh recency. */
TEST(memory_mark_hits_relaxes_decay) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.content = "decayed but still recalled";
    item.scope_project = "test-proj";
    item.status = "active";
    item.decay = 0.5;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    const char *ids[1] = { id };
    ASSERT(cbm_store_memory_mark_hits(s, ids, 1, 0) == CBM_STORE_OK);

    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(out.hit_count == 1);
    ASSERT(out.last_hit_at > 0);
    ASSERT(out.decay < 0.5);          /* decay relaxed by the hit */
    ASSERT(out.decay >= 0.0);
    cbm_store_memory_item_free(&out);

    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── Step-1: real 768-d semantic embeddings ─────────────────────── */

/* The whole point of the 256→768 nomic switch: vector recall ranks a memory
 * by *meaning*, not shared spelling. Store two memories — one topically related
 * to the query but sharing few exact words, one unrelated — and assert the
 * related one gets the higher vector score. Under the old bag-of-words hashing
 * the related pair (different words, same topic) would have been near-orthogonal
 * and this ranking would not hold. */
TEST(memory_embedding_ranks_semantic_neighbor_higher) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);

    cbm_memory_item_t related = {0};
    related.kind = "fact"; related.layer = "semantic";
    related.content = "the function returns an integer value";
    related.scope_project = "test-proj"; related.status = "candidate";
    char *rid = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &related, &rid) == CBM_STORE_OK);

    cbm_memory_item_t unrelated = {0};
    unrelated.kind = "fact"; unrelated.layer = "semantic";
    unrelated.content = "the weather today is sunny and warm";
    unrelated.scope_project = "test-proj"; unrelated.status = "candidate";
    char *uid = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &unrelated, &uid) == CBM_STORE_OK);

    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    /* Query shares almost no exact tokens with the related item ("method"≠"function",
     * "int"≠"integer", "result"≠"value") but is the same topic; embeddings should
     * still rank it above the weather item. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj";
    q.query = "the method yields an int result";
    q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);

    double related_score = -1.0, unrelated_score = -1.0;
    for (int i = 0; i < res.count; i++) {
        if (res.items[i].id && strcmp(res.items[i].id, rid) == 0) related_score = res.items[i].retrieval_score;
        if (res.items[i].id && strcmp(res.items[i].id, uid) == 0) unrelated_score = res.items[i].retrieval_score;
    }
    ASSERT(related_score >= 0.0);              /* related item retrieved */
    ASSERT(related_score > unrelated_score);   /* and ranked above the unrelated one */

    cbm_store_memory_result_free(&res);
    free(rid); free(uid);
    cbm_store_close(s);
    return 0;
}

/* The stored memory vector must be the full 768-d nomic dimension, not the old
 * 256. Guards against a silent regression in MEMORY_VEC_DIM wiring. */
TEST(memory_embedding_dim_is_1024) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic";
    item.content = "dimension probe"; item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    /* Vectors are stored at bge-m3 width (1024); the static path zero-pads. */
    ASSERT(scalar_int(s, "SELECT dim FROM memory_vec LIMIT 1") == 1024);
    ASSERT(scalar_int(s, "SELECT length(embedding) FROM memory_vec LIMIT 1") == 1024);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── Step-3: code anchoring (about_code) ────────────────────────── */

/* A memory anchored to the code symbol the agent is viewing must rank above an
 * equally-relevant memory that isn't anchored. Proves the about_code edge +
 * code_context boost actually reorder retrieval. */
TEST(memory_anchor_boost_raises_rank) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_store_upsert_project(s, "test-proj", "/tmp/test-proj");
    cbm_node_t fn = {.project = "test-proj", .label = "Function", .name = "login",
                     .qualified_name = "auth.Service.login", .file_path = "auth.py",
                     .start_line = 10, .end_line = 40};
    ASSERT(cbm_store_upsert_node(s, &fn) > 0);

    /* Two memories that both match the query "认证" but are distinct topics with
     * distinct entity_keys, so consolidation neither merges nor contradicts them
     * — keeping the test focused purely on the anchoring boost. Only m2 is anchored. */
    cbm_memory_item_t m1 = {0};
    m1.kind = "decision"; m1.layer = "semantic"; m1.content = "认证服务使用 JWT 令牌对接口签名";
    m1.entity_key = "auth.jwt"; m1.predicate = "uses";
    m1.scope_project = "test-proj"; m1.status = "candidate";
    char *id1 = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &m1, &id1) == CBM_STORE_OK);

    cbm_memory_item_t m2 = {0};
    m2.kind = "decision"; m2.layer = "semantic"; m2.content = "认证流程要求双因子验证才能登录";
    m2.entity_key = "auth.mfa"; m2.predicate = "requires";
    m2.scope_project = "test-proj"; m2.status = "candidate";
    char *id2 = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &m2, &id2) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);

    /* Anchor only m2 to the symbol. */
    ASSERT(cbm_store_memory_link_code(s, id2, "auth.Service.login", "user") == CBM_STORE_OK);

    /* Without code_context: both present, anchoring inert. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj"; q.query = "认证"; q.limit = 5;
    /* Single-file test DB: the code graph lives in the same handle, so borrow it
     * as the anchor-boost graph handle (in production this is the separate
     * <project>-graph.db handle). */
    q.graph_db = cbm_store_get_db(s);
    cbm_memory_result_t r0 = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &r0) == CBM_STORE_OK);
    ASSERT(r0.count >= 2);
    cbm_store_memory_result_free(&r0);

    /* With code_context = the anchored symbol: m2 must rank #1. */
    q.code_context = "auth.Service.login";
    cbm_memory_result_t r1 = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &r1) == CBM_STORE_OK);
    ASSERT(r1.count >= 1);
    ASSERT(r1.items[0].id && strcmp(r1.items[0].id, id2) == 0);
    cbm_store_memory_result_free(&r1);

    free(id1); free(id2);
    cbm_store_close(s);
    return 0;
}

/* A stale anchor (symbol absent from the code graph) must NOT boost, but the
 * memory itself must still be retrievable. Proves lazy stale handling: silent
 * de-weight, no hard delete (graveyard philosophy). */
TEST(memory_anchor_stale_no_boost_but_kept) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_store_upsert_project(s, "test-proj", "/tmp/test-proj");
    /* Note: we deliberately do NOT create the node "gone.symbol". */

    cbm_memory_item_t m = {0};
    m.kind = "decision"; m.layer = "semantic"; m.content = "认证逻辑要点 C";
    m.scope_project = "test-proj"; m.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &m, &id) == CBM_STORE_OK);
    int processed = -1;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_link_code(s, id, "gone.symbol", "user") == CBM_STORE_OK);

    /* code_context points at the (nonexistent) anchored symbol: boost must be
     * skipped (stale), yet the memory is still recalled by content. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj"; q.query = "认证"; q.limit = 5;
    q.code_context = "gone.symbol";
    q.graph_db = cbm_store_get_db(s);
    cbm_memory_result_t r = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &r) == CBM_STORE_OK);
    bool found = false;
    for (int i = 0; i < r.count; i++) {
        if (r.items[i].id && strcmp(r.items[i].id, id) == 0) found = true;
    }
    ASSERT(found);
    cbm_store_memory_result_free(&r);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── A1: schema migration framework ─────────────────────────────── */

TEST(migration_fresh_db_sets_version) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* A freshly opened DB must be stamped at the current schema version. */
    ASSERT(scalar_int(s, "PRAGMA user_version") == 6);
    /* Baseline tables must exist (migration 0->1 ran init_schema). */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memory_item'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memory_event'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(migration_idempotent_reopen) {
    char path[512];
    const char *tmp = getenv("TMP");
    if (!tmp || !tmp[0]) tmp = getenv("TEMP");
    if (!tmp || !tmp[0]) tmp = ".";
    snprintf(path, sizeof(path), "%s/cbm_migrate_test_%d.db", tmp, (int)1234);
    remove(path);

    /* First open: fresh DB, runs baseline migration, writes an item. */
    cbm_store_t *s = cbm_store_open_path(path);
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 6);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic"; item.title = "persisted";
    item.content = "survives reopen"; item.scope_project = "test-proj";
    item.status = "candidate"; item.confidence = 0.9;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    free(id);
    cbm_store_close(s);

    /* Reopen the same on-disk DB: migration must be a no-op (already current),
     * version unchanged, data intact. */
    cbm_store_t *s2 = cbm_store_open_path(path);
    ASSERT(s2 != NULL);
    ASSERT(scalar_int(s2, "PRAGMA user_version") == 6);
    ASSERT(scalar_int(s2, "SELECT COUNT(*) FROM memory_item") == 1);
    cbm_store_close(s2);
    remove(path);
    return 0;
}

TEST(stage5_observe_schema_and_ledger) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 6);
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ("
        "'stage5_schema_migrations','retrieval_session','retrieval_candidate',"
        "'retrieval_candidate_source','retrieval_edge_visit','context_injection',"
        "'memory_usage_attribution')") == 7);
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name IN ("
        "'retrieval_edge_visit_session_guard_insert','retrieval_edge_visit_session_guard_update',"
        "'context_injection_session_guard_insert','context_injection_session_guard_update',"
        "'memory_usage_session_guard_insert','memory_usage_session_guard_update')") == 6);
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' "
        "AND name='retrieval_candidate_session_idx'") == 1);
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM stage5_schema_migrations "
        "WHERE component='stage5-observe-only-journal' AND version=1 "
        "AND name='stage5-observe-only-journal-v1' "
        "AND checksum='d85351629783b87162176daca36b31e68d4a66e3bd2f55bdeae013a5be15ab41'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(stage5_observe_direct_indirect_three_hop_and_guards) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);

    const char *contents[] = {"seed one", "seed two", "branch one", "branch two",
                              "converged target", "three hop tail"};
    char *ids[6] = {0};
    for (int i = 0; i < 6; i++) {
        cbm_memory_item_t item = {0};
        item.kind = "fact";
        item.layer = "semantic";
        item.content = contents[i];
        item.scope_project = "test-proj";
        item.status = "active";
        ASSERT(cbm_store_memory_append_candidate(s, &item, &ids[i]) == CBM_STORE_OK);
    }
    ASSERT(insert_test_memory_edge(s, "s5e-1", ids[0], ids[2], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s5e-2", ids[1], ids[3], "supports") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s5e-3", ids[2], ids[4], "derived_from") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s5e-4", ids[3], ids[4], "derived_from") == SQLITE_OK);
    ASSERT(insert_test_memory_edge(s, "s5e-5", ids[4], ids[5], "used_in") == SQLITE_OK);
    int long_term_edges_before = scalar_int(s, "SELECT COUNT(*) FROM memory_edge");

    cbm_retrieval_session_input_t session = {0};
    session.request_id = "stage5-unit-request-1";
    session.project_scope = "test-proj";
    session.memory_scope = "project";
    session.algorithm_version = "stage5-observe-only-v1";
    session.config_version = 1;
    session.query_text = "multi seed converged target";
    char *session_id = NULL;
    char *request_id = NULL;
    bool replayed = false;
    ASSERT(cbm_store_memory_observe_session_begin(s, &session, &session_id, &request_id,
                                                   &replayed) == CBM_STORE_OK);
    ASSERT(session_id != NULL && request_id != NULL && replayed == false);

    cbm_retrieval_candidate_observation_t candidates[6] = {0};
    cbm_retrieval_observation_ref_t refs[6] = {0};
    for (int i = 0; i < 6; i++) {
        candidates[i].source_store_kind = "project";
        candidates[i].source_store_id = "test-proj";
        candidates[i].memory_item_id = ids[i];
        candidates[i].retrieval_source = i < 2 ? "fts" : "graph";
        candidates[i].source_rank = i + 1;
        candidates[i].raw_score = 1.0 - (double)i / 10.0;
        candidates[i].normalized_score = candidates[i].raw_score;
        candidates[i].aggregate_rank = i + 1;
        candidates[i].decision_status = "selected";
        candidates[i].evidence_json = "[]";
    }
    ASSERT(cbm_store_memory_observe_candidates(s, session_id, candidates, 6, refs) ==
           CBM_STORE_OK);
    for (int i = 0; i < 6; i++) {
        ASSERT(refs[i].candidate_id != NULL);
        ASSERT(refs[i].provenance_id != NULL);
        ASSERT(refs[i].evidence_id != NULL);
        ASSERT(refs[i].content_hash != NULL);
    }
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM retrieval_candidate_source rcs "
        "JOIN retrieval_candidate rc ON rc.id=rcs.candidate_id "
        "WHERE rc.session_id=(SELECT id FROM retrieval_session "
        "WHERE request_id='stage5-unit-request-1') AND rcs.source_type='fts'") == 2);
    ASSERT(scalar_int(s, "SELECT MAX(hop_depth) FROM retrieval_edge_visit") == 3);
    ASSERT(scalar_int(s,
        "SELECT COUNT(DISTINCT from_candidate_id) FROM retrieval_edge_visit "
        "WHERE to_candidate_id=(SELECT id FROM retrieval_candidate "
        "WHERE session_id=(SELECT id FROM retrieval_session "
        "WHERE request_id='stage5-unit-request-1') AND memory_item_id=(SELECT dst_id "
        "FROM memory_edge WHERE id='s5e-3'))") == 2);

    cbm_observe_injection_input_t injection = {0};
    injection.event_id = "stage5-injection-1";
    injection.session_id = session_id;
    injection.candidate_id = refs[4].candidate_id;
    injection.injection_index = 0;
    injection.target = "assistant_context";
    injection.content_hash = refs[4].content_hash;
    injection.token_count = 3;
    injection.classifier_status = "pass";
    injection.classification = "safe";
    ASSERT(cbm_store_memory_observe_injection(s, &injection) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_observe_injection(s, &injection) == CBM_STORE_REPLAYED);
    int injection_count = scalar_int(s, "SELECT COUNT(*) FROM context_injection");
    injection.token_count = 4;
    ASSERT(cbm_store_memory_observe_injection(s, &injection) ==
           CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM context_injection") == injection_count);
    injection.token_count = 3;

    cbm_observe_usage_input_t usage = {0};
    usage.event_id = "stage5-usage-1";
    usage.session_id = session_id;
    usage.candidate_id = refs[4].candidate_id;
    usage.injection_id = injection.event_id;
    usage.outcome = "used";
    usage.evidence_type = "deterministic_stub";
    usage.evidence_ref = "answer-token-overlap";
    usage.evidence_hash = "xxh3-test-evidence";
    ASSERT(cbm_store_memory_observe_usage(s, &usage) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_observe_usage(s, &usage) == CBM_STORE_REPLAYED);
    int usage_count = scalar_int(s, "SELECT COUNT(*) FROM memory_usage_attribution");
    usage.outcome = "contradicted";
    ASSERT(cbm_store_memory_observe_usage(s, &usage) == CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_usage_attribution") == usage_count);
    usage.outcome = "used";

    cbm_retrieval_session_input_t other = session;
    other.request_id = "stage5-unit-request-2";
    char *other_session_id = NULL;
    char *other_request_id = NULL;
    ASSERT(cbm_store_memory_observe_session_begin(s, &other, &other_session_id,
                                                   &other_request_id, &replayed) == CBM_STORE_OK);
    cbm_observe_usage_input_t wrong = usage;
    wrong.event_id = "stage5-usage-wrong-session";
    wrong.session_id = other_session_id;
    ASSERT(cbm_store_memory_observe_usage(s, &wrong) == CBM_STORE_ERR);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_usage_attribution") == usage_count);

    cbm_observe_injection_input_t blocked = injection;
    blocked.event_id = "stage5-injection-blocked";
    blocked.injection_index = 1;
    blocked.classifier_status = "error";
    blocked.classification = "prompt_injection";
    ASSERT(cbm_store_memory_observe_injection(s, &blocked) == CBM_STORE_REJECTED);
    ASSERT(cbm_store_memory_observe_injection(s, &blocked) == CBM_STORE_REJECTED);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM context_injection") == injection_count);
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM memory_usage_attribution WHERE id='stage5-injection-blocked' "
        "AND outcome='rejected' AND injection_id IS NULL") == 0);

    ASSERT(cbm_store_memory_observe_session_complete(s, session_id, "completed", NULL) ==
           CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM retrieval_session WHERE id='stage5-unit-request-1' "
                         "AND status='completed'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == long_term_edges_before);

    cbm_store_memory_observation_refs_free(refs, 6);
    free(session_id);
    free(request_id);
    free(other_session_id);
    free(other_request_id);
    for (int i = 0; i < 6; i++)
        free(ids[i]);
    cbm_store_close(s);
    return 0;
}

/* ── P0-1: memory-path transaction atomicity ────────────────────── */

/* feedback writes a memory_item UPDATE + a memory_event audit row; both must
 * commit together. Inject a failure on the event insert via a trigger, then
 * assert the item UPDATE was rolled back (status NOT changed, no audit row). */
TEST(feedback_atomic_rolls_back_item_on_event_failure) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic"; item.title = "atomic feedback";
    item.content = "feedback should be all-or-nothing"; item.scope_project = "test-proj";
    item.status = "active"; item.confidence = 0.9;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    /* Force any memory_event INSERT to fail mid-transaction. */
    char *err = NULL;
    ASSERT(sqlite3_exec(cbm_store_get_db(s),
        "CREATE TRIGGER cbm_fail_event BEFORE INSERT ON memory_event "
        "BEGIN SELECT RAISE(ABORT, 'injected event failure'); END;",
        NULL, NULL, &err) == SQLITE_OK);

    /* "wrong" feedback would retract the item AND write an audit event.
     * The event insert fails -> the whole op must roll back. */
    int rc = cbm_store_memory_feedback(s, id, "test-proj", "wrong", "should not persist", NULL, NULL);
    ASSERT(rc == CBM_STORE_ERR);

    /* Item must NOT be retracted (UPDATE rolled back). */
    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    cbm_store_memory_item_free(&out);
    /* No audit event persisted. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'") == 0);

    (void)sqlite3_exec(cbm_store_get_db(s), "DROP TRIGGER cbm_fail_event;", NULL, NULL, NULL);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* consolidate activates an item and writes its vector + FTS + edges in one
 * batch transaction. Verify the positive invariant: a successfully activated
 * item always has all secondary rows (never a half-written active item). */
TEST(consolidate_active_item_fully_indexed) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic"; item.title = "indexed";
    item.content = "active items carry vector and fts and belongs_to edge";
    item.scope_project = "test-proj"; item.status = "candidate"; item.confidence = 0.9;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);

    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);

    cbm_memory_item_t out = {0};
    ASSERT(cbm_store_memory_get_item(s, id, &out) == CBM_STORE_OK);
    ASSERT(strcmp(out.status, "active") == 0);
    cbm_store_memory_item_free(&out);

    /* All-or-nothing: the active item has its vector, FTS row, and belongs_to edge. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_fts") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge WHERE type='belongs_to'") == 1);

    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── Step-1: lazy auto-maintenance trigger ──────────────────────── */

TEST(migration_v2_adds_meta_table) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 6);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='memory_meta'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(maintain_triggers_consolidate_on_threshold) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* Append the trigger threshold (8) of candidates. */
    for (int i = 0; i < 8; i++) {
        cbm_memory_item_t item = {0};
        char content[64];
        snprintf(content, sizeof(content), "auto maintain candidate number %d", i);
        item.kind = "fact"; item.layer = "semantic"; item.title = "auto";
        item.content = content; item.scope_project = "test-proj";
        item.status = "candidate"; item.confidence = 0.9;
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 8);

    /* Fresh DB: last_consolidate_ms=0, so the elapsed-time gate is satisfied;
     * 8 >= threshold triggers consolidation. */
    cbm_memory_maintain_report_t rep = {0};
    ASSERT(cbm_store_memory_maintain_if_due(s, "test-proj", &rep) == CBM_STORE_OK);
    ASSERT(rep.consolidated == true);
    ASSERT(rep.consolidate_count == 8);
    /* Candidates are now active and fully indexed. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='active'") == 8);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 8);
    /* Timestamp was recorded. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_meta WHERE key='last_consolidate_ms'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(maintain_below_threshold_does_not_consolidate) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* Only a few candidates (below threshold 8). Fresh DB means the backstop
     * (MAX_INTERVAL) gate would fire on since=now-0 — but that is the documented
     * behavior: a backstop ensures stray candidates eventually consolidate.
     * To test the threshold path specifically we pre-seed last_consolidate_ms to
     * "now" so neither the threshold (count<8) nor the backstop (just ran) fires. */
    {
        cbm_memory_item_t item = {0};
        item.kind = "fact"; item.layer = "semantic"; item.title = "few";
        item.content = "only one candidate, below threshold";
        item.scope_project = "test-proj"; item.status = "candidate"; item.confidence = 0.9;
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    /* Mark maintenance as just-run so debounce + backstop both block. */
    {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "INSERT OR REPLACE INTO memory_meta (key,value) VALUES "
                 "('last_consolidate_ms','%lld'),('last_decay_ms','%lld');",
                 (long long)((int64_t)time(NULL) * 1000LL),
                 (long long)((int64_t)time(NULL) * 1000LL));
        ASSERT(sqlite3_exec(cbm_store_get_db(s), sql, NULL, NULL, NULL) == SQLITE_OK);
    }
    cbm_memory_maintain_report_t rep = {0};
    ASSERT(cbm_store_memory_maintain_if_due(s, "test-proj", &rep) == CBM_STORE_OK);
    ASSERT(rep.consolidated == false);
    ASSERT(rep.decayed == false);
    /* Candidate untouched. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 0);
    cbm_store_close(s);
    return 0;
}

TEST(maintain_disabled_by_env) {
    /* With the kill switch set, even an over-threshold batch must not trigger. */
#ifdef _WIN32
    _putenv("CBM_MEMORY_AUTO_MAINTAIN=0");
#else
    setenv("CBM_MEMORY_AUTO_MAINTAIN", "0", 1);
#endif
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    for (int i = 0; i < 8; i++) {
        cbm_memory_item_t item = {0};
        char content[64];
        snprintf(content, sizeof(content), "disabled env candidate %d", i);
        item.kind = "fact"; item.layer = "semantic"; item.title = "off";
        item.content = content; item.scope_project = "test-proj";
        item.status = "candidate"; item.confidence = 0.9;
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    cbm_memory_maintain_report_t rep = {0};
    ASSERT(cbm_store_memory_maintain_if_due(s, "test-proj", &rep) == CBM_STORE_OK);
    ASSERT(rep.consolidated == false);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='candidate'") == 8);
    cbm_store_close(s);
#ifdef _WIN32
    _putenv("CBM_MEMORY_AUTO_MAINTAIN=");
#else
    unsetenv("CBM_MEMORY_AUTO_MAINTAIN");
#endif
    return 0;
}

/* ── P0-2: hard / soft / purge delete + retention sweep ─────────────── */

TEST(memory_migration_v4_adds_deleted_at) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    ASSERT(scalar_int(s, "PRAGMA user_version") == 6);
    /* deleted_at column exists on memory_item. */
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM pragma_table_info('memory_item') WHERE name='deleted_at'") == 1);
    /* sweep index exists. */
    ASSERT(scalar_int(s,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_memory_item_deleted'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(memory_hard_delete_removes_all_rows) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.layer = "semantic";
    item.content = "hard delete removes every satellite row";
    item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);
    /* Baseline: item + vec + fts rows present. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_fts") == 1);
    /* Hard delete. */
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "hard", "tester") == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec")  == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_fts")  == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == 0);
    /* A delete tombstone audit event was written. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='delete'") == 1);
    /* Idempotent: second delete finds nothing. */
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "hard", "tester") == CBM_STORE_NOT_FOUND);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_delete_cascades_edges) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t a = {0}, b = {0};
    a.kind = "fact"; a.content = "edge cascade item A"; a.scope_project = "test-proj"; a.status = "active";
    b.kind = "fact"; b.content = "edge cascade item B"; b.scope_project = "test-proj"; b.status = "active";
    char *id_a = NULL, *id_b = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &a, &id_a) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_candidate(s, &b, &id_b) == CBM_STORE_OK);
    /* Insert an edge A->B directly. */
    sqlite3_stmt *st = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
        "INSERT INTO memory_edge (id,src_id,dst_id,type,weight,origin,confidence,created_at) "
        "VALUES ('e1',?1,?2,'supports',1.0,'test',1.0,1);", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, id_a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id_b, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == 1);
    /* Deleting A removes the edge (dst_id=B side too), B survives. */
    ASSERT(cbm_store_memory_delete(s, id_a, "test-proj", "hard", NULL) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_edge") == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 1);
    free(id_a); free(id_b);
    cbm_store_close(s);
    return 0;
}

TEST(memory_soft_delete_hides_from_retrieve) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "soft delete hides this from retrieval";
    item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    /* Soft delete. */
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "soft", "tester") == CBM_STORE_OK);
    /* Row is still on disk, but marked. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL") == 1);
    /* Hidden from retrieval even with include_inactive. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj"; q.query = "soft delete hides"; q.limit = 5; q.include_inactive = true;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count == 0);
    cbm_store_memory_result_free(&res);
    /* Structured path too. */
    cbm_memory_query_t q2 = {0};
    q2.project = "test-proj"; q2.limit = 5; q2.include_inactive = true;
    cbm_memory_result_t res2 = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q2, &res2) == CBM_STORE_OK);
    ASSERT(res2.count == 0);
    cbm_store_memory_result_free(&res2);
    /* Soft-delete audit event written. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='soft_delete'") == 1);
    /* Idempotent: a second soft delete is a no-op NOT_FOUND. */
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "soft", "tester") == CBM_STORE_NOT_FOUND);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_restore_undeletes) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "restore brings this back to retrieval";
    item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "soft", NULL) == CBM_STORE_OK);
    /* Restore. */
    ASSERT(cbm_store_memory_restore(s, id, "test-proj", "tester") == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE deleted_at IS NOT NULL") == 0);
    /* Retrievable again. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj"; q.query = "restore brings this back"; q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    cbm_store_memory_result_free(&res);
    /* Restoring a live item is a NOT_FOUND no-op. */
    ASSERT(cbm_store_memory_restore(s, id, "test-proj", NULL) == CBM_STORE_NOT_FOUND);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_purge_expired_respects_grace) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t old_item = {0}, fresh = {0};
    old_item.kind = "fact"; old_item.content = "old soft-deleted item past grace";
    old_item.scope_project = "test-proj"; old_item.status = "candidate";
    fresh.kind = "fact"; fresh.content = "freshly soft-deleted item within grace";
    fresh.scope_project = "test-proj"; fresh.status = "candidate";
    char *id_old = NULL, *id_fresh = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &old_item, &id_old) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_candidate(s, &fresh, &id_fresh) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    /* Soft delete both. */
    ASSERT(cbm_store_memory_delete(s, id_old, "test-proj", "soft", NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_delete(s, id_fresh, "test-proj", "soft", NULL) == CBM_STORE_OK);
    /* Backdate the "old" one's deleted_at well past any grace window. */
    sqlite3_stmt *st = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
        "UPDATE memory_item SET deleted_at=1000 WHERE id=?1;", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, id_old, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    /* Sweep with a 1-day grace: old purged, fresh kept. */
    int purged = 0;
    int64_t grace = 24LL * 60 * 60 * 1000;
    ASSERT(cbm_store_memory_purge_expired(s, "test-proj", grace, &purged) == CBM_STORE_OK);
    ASSERT(purged == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_vec")  == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE id IN (SELECT id FROM memory_item)") == 1);
    free(id_old); free(id_fresh);
    cbm_store_close(s);
    return 0;
}

/* P4 red line: a code-anchored decision ADR is NEVER physically purged, even
 * when soft-deleted past the grace window — it stays as supersede-chain head.
 * An unanchored note in the same sweep purges normally. */
TEST(memory_purge_spares_anchored_decision_adr) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t adr = {0}, note = {0};
    adr.kind = "decision"; adr.content = "anchored decision that must survive purge";
    adr.scope_project = "test-proj"; adr.status = "candidate";
    note.kind = "fact"; note.content = "unanchored note that should purge normally";
    note.scope_project = "test-proj"; note.status = "candidate";
    char *id_adr = NULL, *id_note = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &adr, &id_adr) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_append_candidate(s, &note, &id_note) == CBM_STORE_OK);
    /* Anchor the decision to a code symbol (memory_edge only; no graph needed). */
    ASSERT(cbm_store_memory_link_code(s, id_adr, "proj.mod.func", "user") == CBM_STORE_OK);
    /* Soft delete both and backdate past any grace. */
    ASSERT(cbm_store_memory_delete(s, id_adr, "test-proj", "soft", NULL) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_delete(s, id_note, "test-proj", "soft", NULL) == CBM_STORE_OK);
    sqlite3_stmt *st = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
        "UPDATE memory_item SET deleted_at=1000 WHERE id IN (?1,?2);", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, id_adr, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id_note, -1, SQLITE_TRANSIENT);
    ASSERT(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    int purged = 0;
    int64_t grace = 24LL * 60 * 60 * 1000;
    ASSERT(cbm_store_memory_purge_expired(s, "test-proj", grace, &purged) == CBM_STORE_OK);
    /* Only the unanchored note is purged; the anchored decision survives. */
    ASSERT(purged == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE kind='decision'") == 1);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE kind='fact'") == 0);
    free(id_adr); free(id_note);
    cbm_store_close(s);
    return 0;
}

TEST(memory_purge_mode_deletes_source_events) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* Append a real event, then an item whose source_event_ids references it. */
    cbm_memory_event_t ev = {0};
    ev.type = "conversation"; ev.source = "user"; ev.project = "test-proj";
    ev.payload = "{\"k\":\"v\"}"; ev.confidence = 0.8;
    char *ev_id = NULL;
    ASSERT(cbm_store_memory_append_event(s, &ev, &ev_id) == CBM_STORE_OK);
    ASSERT(ev_id != NULL);
    char src_json[256];
    snprintf(src_json, sizeof(src_json), "[\"%s\"]", ev_id);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "purge mode erases my source event";
    item.scope_project = "test-proj"; item.status = "active";
    item.source_event_ids = src_json;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    /* The referenced event exists. */
    char qbuf[256];
    snprintf(qbuf, sizeof(qbuf), "SELECT COUNT(*) FROM memory_event WHERE id='%s'", ev_id);
    ASSERT(scalar_int(s, qbuf) == 1);
    /* Purge mode deletes the source event too. */
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "purge", "tester") == CBM_STORE_OK);
    ASSERT(scalar_int(s, qbuf) == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 0);
    /* The delete tombstone event still survives. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='delete'") == 1);
    free(ev_id); free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_hard_delete_keeps_source_events) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_event_t ev = {0};
    ev.type = "conversation"; ev.source = "user"; ev.project = "test-proj";
    ev.payload = "{\"k\":\"v\"}"; ev.confidence = 0.8;
    char *ev_id = NULL;
    ASSERT(cbm_store_memory_append_event(s, &ev, &ev_id) == CBM_STORE_OK);
    char src_json[256];
    snprintf(src_json, sizeof(src_json), "[\"%s\"]", ev_id);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "hard delete keeps my source event";
    item.scope_project = "test-proj"; item.status = "active";
    item.source_event_ids = src_json;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    char qbuf[256];
    snprintf(qbuf, sizeof(qbuf), "SELECT COUNT(*) FROM memory_event WHERE id='%s'", ev_id);
    ASSERT(scalar_int(s, qbuf) == 1);
    /* Hard mode keeps the source event. */
    ASSERT(cbm_store_memory_delete(s, id, "test-proj", "hard", NULL) == CBM_STORE_OK);
    ASSERT(scalar_int(s, qbuf) == 1);
    free(ev_id); free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_delete_scope_guard) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "scope guard protects cross-project delete";
    item.scope_project = "proj-a"; item.status = "active";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    /* Wrong project: NOT_FOUND, row untouched. */
    ASSERT(cbm_store_memory_delete(s, id, "proj-b", "hard", NULL) == CBM_STORE_NOT_FOUND);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 1);
    /* Correct project: deletes. */
    ASSERT(cbm_store_memory_delete(s, id, "proj-a", "hard", NULL) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item") == 0);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── P0-4: audit-event coverage for lifecycle mutations ─────────────── */

TEST(memory_update_status_writes_audit_event) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "status change leaves an audit trail";
    item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    ASSERT(cbm_store_memory_update_status(s, id, "test-proj", "archived") == CBM_STORE_OK);
    /* A status_change audit event was recorded with the new status. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='status_change'") == 1);
    /* A no-op (wrong scope) writes NO event. */
    ASSERT(cbm_store_memory_update_status(s, id, "other-proj", "active") == CBM_STORE_NOT_FOUND);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='status_change'") == 1);
    free(id);
    cbm_store_close(s);
    return 0;
}

TEST(memory_consolidate_writes_audit_event) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    for (int i = 0; i < 3; i++) {
        cbm_memory_item_t item = {0};
        char content[64];
        snprintf(content, sizeof(content), "consolidate audit candidate %d", i);
        item.kind = "fact"; item.content = content;
        item.scope_project = "test-proj"; item.status = "candidate";
        char *id = NULL;
        ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
        free(id);
    }
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 3);
    /* One summary consolidate event for the batch (not one per item). */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='consolidate'") == 1);
    /* A consolidate run that processes nothing writes no event. */
    int processed2 = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed2) == CBM_STORE_OK);
    ASSERT(processed2 == 0);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='consolidate'") == 1);
    cbm_store_close(s);
    return 0;
}

TEST(memory_decay_writes_audit_event_when_archiving) {
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    /* An active item that is stale and low-value will cross the archive
     * threshold on decay. Force the conditions: old last_hit, low conf/reuse. */
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "stale low-value item gets archived by decay";
    item.scope_project = "test-proj"; item.status = "candidate";
    item.confidence = 0.0; item.reusability = 0.0;
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    /* Backdate last_hit_at far into the past and zero the value signals so the
     * decay formula pushes it past 1.0 in one pass. */
    sqlite3_stmt *st = NULL;
    ASSERT(sqlite3_prepare_v2(cbm_store_get_db(s),
        "UPDATE memory_item SET last_hit_at=1, confidence=0.0, reusability=0.0, decay=0.95;",
        -1, &st, NULL) == SQLITE_OK);
    ASSERT(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    int decayed = 0;
    ASSERT(cbm_store_memory_decay(s, "test-proj", 100, &decayed) == CBM_STORE_OK);
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_item WHERE status='archived'") == 1);
    /* A decay summary event was recorded because something was archived. */
    ASSERT(scalar_int(s, "SELECT COUNT(*) FROM memory_event WHERE type='decay'") == 1);
    free(id);
    cbm_store_close(s);
    return 0;
}

/* ── Embedding sidecar backend (bge-m3 integration) ────────────────── */

/* Default backend is static: cbm_embed_backend() reports static when the env
 * is unset, and embeddings are still produced (zero-padded to 1024). */
TEST(embed_default_backend_is_static) {
#ifdef _WIN32
    _putenv("CBM_MEMORY_EMBED_BACKEND=");
#else
    unsetenv("CBM_MEMORY_EMBED_BACKEND");
#endif
    cbm_embed_reset_for_test();
    ASSERT(cbm_embed_backend() == CBM_EMBED_STATIC);
    cbm_embed_reset_for_test();
    return 0;
}

/* With the sidecar backend + a mock sidecar, embeddings round-trip through the
 * child process: the stored vector is 1024-d and its tail (beyond the static
 * 768) is non-zero — proof it came from the sidecar, not the zero-padded
 * static path. */
TEST(embed_sidecar_roundtrip_via_mock) {
#ifdef _WIN32
    _putenv("CBM_MEMORY_EMBED_BACKEND=sidecar");
    _putenv("CBM_MEMORY_EMBED_CMD=python scripts/mock_embed_sidecar.py");
#else
    setenv("CBM_MEMORY_EMBED_BACKEND", "sidecar", 1);
    setenv("CBM_MEMORY_EMBED_CMD", "python scripts/mock_embed_sidecar.py", 1);
#endif
    cbm_embed_reset_for_test();
    ASSERT(cbm_embed_backend() == CBM_EMBED_SIDECAR);

    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "sidecar embedding roundtrip probe";
    item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    /* Stored at 1024 width. */
    ASSERT(scalar_int(s, "SELECT length(embedding) FROM memory_vec LIMIT 1") == 1024);
    /* The mock fills all 1024 dims densely; the static path would leave the
     * [768,1024) tail all-zero. Assert the tail has at least one non-zero byte,
     * proving the vector came from the sidecar. */
    ASSERT(scalar_int(s,
        "SELECT (substr(embedding,769) != zeroblob(256)) FROM memory_vec LIMIT 1;") == 1);
    free(id);
    cbm_store_close(s);

#ifdef _WIN32
    _putenv("CBM_MEMORY_EMBED_BACKEND=");
    _putenv("CBM_MEMORY_EMBED_CMD=");
#else
    unsetenv("CBM_MEMORY_EMBED_BACKEND");
    unsetenv("CBM_MEMORY_EMBED_CMD");
#endif
    cbm_embed_reset_for_test();
    return 0;
}

/* A sidecar that can't start (bad command) degrades to static: embeddings still
 * succeed (zero-padded), retrieval still works, nothing blocks or errors. */
TEST(embed_sidecar_spawn_failure_degrades_to_static) {
#ifdef _WIN32
    _putenv("CBM_MEMORY_EMBED_BACKEND=sidecar");
    _putenv("CBM_MEMORY_EMBED_CMD=definitely-not-a-real-command-xyz");
#else
    setenv("CBM_MEMORY_EMBED_BACKEND", "sidecar", 1);
    setenv("CBM_MEMORY_EMBED_CMD", "definitely-not-a-real-command-xyz", 1);
#endif
    cbm_embed_reset_for_test();
    cbm_store_t *s = cbm_store_open_memory();
    ASSERT(s != NULL);
    cbm_memory_item_t item = {0};
    item.kind = "fact"; item.content = "fallback when sidecar missing";
    item.scope_project = "test-proj"; item.status = "candidate";
    char *id = NULL;
    ASSERT(cbm_store_memory_append_candidate(s, &item, &id) == CBM_STORE_OK);
    int processed = 0;
    /* Must not fail despite the dead sidecar (static fallback kicks in). */
    ASSERT(cbm_store_memory_consolidate(s, "test-proj", 100, &processed) == CBM_STORE_OK);
    ASSERT(processed == 1);
    ASSERT(scalar_int(s, "SELECT length(embedding) FROM memory_vec LIMIT 1") == 1024);
    /* Retrieval still works via the static vector + FTS. */
    cbm_memory_query_t q = {0};
    q.project = "test-proj"; q.query = "fallback sidecar missing"; q.limit = 5;
    cbm_memory_result_t res = {0};
    ASSERT(cbm_store_memory_retrieve(s, &q, &res) == CBM_STORE_OK);
    ASSERT(res.count >= 1);
    cbm_store_memory_result_free(&res);
    free(id);
    cbm_store_close(s);
#ifdef _WIN32
    _putenv("CBM_MEMORY_EMBED_BACKEND=");
    _putenv("CBM_MEMORY_EMBED_CMD=");
#else
    unsetenv("CBM_MEMORY_EMBED_BACKEND");
    unsetenv("CBM_MEMORY_EMBED_CMD");
#endif
    cbm_embed_reset_for_test();
    return 0;
}

/* Traceability test for the consolidated 3-tier scoring (cbm_memory_score_item).
 * Pure function — no DB/graph needed. Asserts each tier fires for its case, and
 * that the monotonic floor stops an anchored low-degree ADR from scoring below
 * an unanchored one. This test IS the living spec of the L1>L2>L3 design. */
static int test_memory_score_item_tiers(void) {
    cbm_memory_score_t s;

    /* L1: anchored high-degree code ADR → graph signal drives (declared unset). */
    s = cbm_memory_score_item("decision", 3, 0.95, 0.90, 0.5, 0.5);
    ASSERT(s.confidence > 0.94 && s.confidence < 0.96);
    ASSERT(s.reusability > 0.89 && s.reusability < 0.91);

    /* Monotonic-floor fix: anchored but LOW-degree ADR (graph reuse 0.4) must NOT
     * fall below the decision kind prior 0.7 — the whole point of max composition. */
    s = cbm_memory_score_item("decision", 1, 0.5, 0.4, 0.5, 0.5);
    ASSERT(s.reusability > 0.69 && s.reusability < 0.71); /* max(0.7, 0.4) */
    ASSERT(s.confidence > 0.49 && s.confidence < 0.51);   /* conf has no kind prior */

    /* L2: unanchored decision → kind prior 0.7 reuse, conf stays 0.5. */
    s = cbm_memory_score_item("decision", 0, 0.0, 0.0, 0.5, 0.5);
    ASSERT(s.reusability > 0.69 && s.reusability < 0.71);
    ASSERT(s.confidence > 0.49 && s.confidence < 0.51);

    /* L2: unanchored preference → 0.75; event → 0.4; unknown kind → 0.5. */
    s = cbm_memory_score_item("preference", 0, 0.0, 0.0, 0.5, 0.5);
    ASSERT(s.reusability > 0.74 && s.reusability < 0.76);
    s = cbm_memory_score_item("event", 0, 0.0, 0.0, 0.5, 0.5);
    ASSERT(s.reusability > 0.39 && s.reusability < 0.41);
    s = cbm_memory_score_item("weird", 0, 0.0, 0.0, 0.5, 0.5);
    ASSERT(s.reusability > 0.49 && s.reusability < 0.51);

    /* L3: declared non-0.5 applies as a uniform offset. Unanchored decision with
     * declared reuse 0.9 → 0.7 + 0.4 = 1.0 (clamped); with 0.3 → 0.7 - 0.2 = 0.5. */
    s = cbm_memory_score_item("decision", 0, 0.0, 0.0, 0.5, 0.9);
    ASSERT(s.reusability > 0.99 && s.reusability <= 1.0001);
    s = cbm_memory_score_item("decision", 0, 0.0, 0.0, 0.5, 0.3);
    ASSERT(s.reusability > 0.49 && s.reusability < 0.51);

    /* Declared offset stacks on the L1 base too: conf base max(0.5,0.95)=0.95,
     * + (0.9-0.5) → clamp 1.0. */
    s = cbm_memory_score_item("decision", 3, 0.95, 0.9, 0.9, 0.5);
    ASSERT(s.confidence > 0.99 && s.confidence <= 1.0001);

    return 0;
}

int main(void) {
    fprintf(stderr, "START\n");
    fflush(stderr);
    int pass = 0, fail = 0, total = 0;
    RUN(memory_schema_init);
    RUN(memory_append_event);
    RUN(memory_append_candidate);
    RUN(memory_stage11_secondary_fields_reject_before_write);
    RUN(memory_derived_from_link_is_event_bound_and_atomic);
    RUN(memory_append_structured_candidate);
    RUN(memory_get_item);
    RUN(memory_retrieve_structured);
    RUN(memory_retrieve_fts);
    RUN(memory_retrieve_long_prompt_accepts_four_tokens_and_rejects_three_token_noise);
    RUN(memory_retrieve_vector_fusion);
    RUN(memory_retrieve_safety_fixture_requires_category_relevance);
    RUN(memory_retrieve_conflict_resolution);
    RUN(memory_retrieve_negative_evidence_prefers_supported_correction);
    RUN(memory_retrieve_evidence_graph);
    RUN(memory_retrieve_supersedes_path_is_complete);
    RUN(memory_retrieve_graph_expansion_is_bounded_three_hops);
    RUN(stage6_shadow_multi_seed_merge_scope_cycle_and_budget);
    RUN(stage6_off_shadow_equivalence_and_vector_seed_policy);
    RUN(stage6_active_fixture_guard_and_failure_atomicity);
    RUN(stage6_structured_and_code_anchor_seed_fusion);
    RUN(stage6_relation_direction_version_and_unsafe_guards);
    RUN(stage6_hop_budget_timeout_and_stable_ids);
    RUN(stage8_shadow_is_deterministic_and_writes_no_stage8_tables);
    RUN(stage8_active_requires_both_guards_and_exact_replay_is_zero_addition);
    RUN(stage8_production_active_requires_exact_canary_manifest_guard);
    RUN(stage8_correction_and_withdraw_rebuild_from_ledger);
    RUN(stage8_migration_and_reinforcement_failpoints_are_atomic);
    RUN(stage8_caps_bound_repeated_tasks_and_unrelated_edges_stay_absent);
    RUN(stage9_migration_is_additive_idempotent_and_atomic);
    RUN(stage9_shadow_dry_run_policy_and_zero_write);
    RUN(stage9_active_replay_archive_restore_and_direct_recall);
    RUN(stage9_active_fault_rolls_back_and_retry_succeeds);
    RUN(stage10_migration_is_additive_replayable_and_atomic);
    RUN(stage10_generation_review_relations_replay_and_guards);
    RUN(memory_consolidate);
    RUN(memory_consolidate_merge_keeps_new_event_evidence);
    RUN(memory_consolidate_merges_paraphrase);
    RUN(memory_consolidate_keeps_lowcosine_peers_without_contradiction);
    RUN(memory_mark_hits_relaxes_decay);
    RUN(memory_health);
    RUN(memory_mark_hits);
    RUN(memory_update_status_retracts_from_default_retrieval);
    RUN(memory_update_status_rejects_invalid_status);
    RUN(memory_feedback_useful_records_event_and_boosts_hit_signal);
    RUN(memory_feedback_idempotent_exact_replay_is_side_effect_free);
    RUN(memory_feedback_idempotency_conflict_is_side_effect_free);
    RUN(memory_feedback_wrong_retracts_from_default_retrieval);
    RUN(memory_decay_archives_stale);
    RUN(memory_decay_max_importance_never_decays);
    RUN(memory_embedding_ranks_semantic_neighbor_higher);
    RUN(memory_embedding_dim_is_1024);
    RUN(memory_anchor_boost_raises_rank);
    RUN(memory_anchor_stale_no_boost_but_kept);
    RUN(migration_fresh_db_sets_version);
    RUN(migration_idempotent_reopen);
    RUN(stage5_observe_schema_and_ledger);
    RUN(stage5_observe_direct_indirect_three_hop_and_guards);
    RUN(stage7_schema_is_additive_and_user_version_stays_six);
    RUN(stage7_feedback_shadow_idempotency_and_long_term_zero_change);
    RUN(stage7_model_self_report_is_pending_and_wrong_hash_fails_closed);
    RUN(stage7_sha256_vectors_and_payloads_are_not_persisted);
    RUN(stage7_reward_report_uses_only_visited_edges);
    RUN(stage7_cross_chain_and_forged_evidence_fail_closed);
    RUN(stage7_correction_withdrawal_and_evidence_states_are_append_only);
    RUN(stage7_feedback_failure_injection_rolls_back_and_retries_cleanly);
    RUN(stage7_migration_failure_is_atomic_and_recoverable);
    RUN(stage7_audit_tamper_is_detected_and_schema_drift_fails_reopen);
    RUN(stage7_concurrent_replay_and_conflict_are_serialized);
    RUN(feedback_atomic_rolls_back_item_on_event_failure);
    RUN(consolidate_active_item_fully_indexed);
    RUN(migration_v2_adds_meta_table);
    RUN(maintain_triggers_consolidate_on_threshold);
    RUN(maintain_below_threshold_does_not_consolidate);
    RUN(maintain_disabled_by_env);
    RUN(memory_migration_v4_adds_deleted_at);
    RUN(memory_hard_delete_removes_all_rows);
    RUN(memory_delete_cascades_edges);
    RUN(memory_soft_delete_hides_from_retrieve);
    RUN(memory_restore_undeletes);
    RUN(memory_purge_expired_respects_grace);
    RUN(memory_purge_spares_anchored_decision_adr);
    RUN(memory_purge_mode_deletes_source_events);
    RUN(memory_hard_delete_keeps_source_events);
    RUN(memory_delete_scope_guard);
    RUN(memory_update_status_writes_audit_event);
    RUN(memory_consolidate_writes_audit_event);
    RUN(memory_decay_writes_audit_event_when_archiving);
    RUN(embed_default_backend_is_static);
    RUN(embed_sidecar_roundtrip_via_mock);
    RUN(embed_sidecar_spawn_failure_degrades_to_static);
    RUN(memory_score_item_tiers);
    fprintf(stderr, "\n%d/%d passed\n", pass, total);
    return fail ? 1 : 0;
}
