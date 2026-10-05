#include "memory/memory_orchestrator.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT_OR_FAIL(value) do { if (!(value)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); return 1; } } while (0)

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static bool report_task_id(const char *json, char output[128]) {
    yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *value = root ? yyjson_obj_get(root, "task_id") : NULL;
    bool ok = value && yyjson_is_str(value);
    if (ok) snprintf(output, 128, "%s", yyjson_get_str(value));
    yyjson_doc_free(doc);
    return ok;
}

static int test_migration_and_replay(void) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_OR_FAIL(store != NULL);
    sqlite3 *db = cbm_store_get_db(store);
    int user_version = scalar(db, "PRAGMA user_version;");
    bool replayed = true;
    char *report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_migrate(store, &replayed, &report) == CBM_STORE_OK);
    ASSERT_OR_FAIL(!replayed && report && strstr(report, "\"status\":\"recorded\""));
    free(report);
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_migrate(store, &replayed, &report) == CBM_STORE_REPLAYED);
    ASSERT_OR_FAIL(replayed && report && strstr(report, "\"status\":\"replayed\""));
    ASSERT_OR_FAIL(scalar(db, "PRAGMA user_version;") == user_version);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM stage12_component_ledger;") == 1);
    ASSERT_OR_FAIL(sqlite3_exec(db, "UPDATE stage12_component_ledger SET name=name;", NULL, NULL, NULL) != SQLITE_OK);
    free(report);
    cbm_store_close(store);
    return 0;
}

static int test_lifecycle_replay_conflict_and_hash_only(void) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_OR_FAIL(store != NULL);
    bool replayed = false;
    char *report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_migrate(store, &replayed, &report) == CBM_STORE_OK);
    free(report);
    cbm_task_begin_input_t begin = {
        .project = "stage12-fixture", .session_id = "session-1", .turn_id = "turn-1",
        .prompt_sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .prompt_length = 37, .retrieval_session_id = NULL,
        .idempotency_key = "stage12-begin-key-1"
    };
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_begin(store, &begin, &report) == CBM_STORE_OK);
    char task_id[128];
    ASSERT_OR_FAIL(report_task_id(report, task_id));
    free(report);
    sqlite3 *db = cbm_store_get_db(store);
    int rows = scalar(db, "SELECT COUNT(*) FROM codex_task_lifecycle;");
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_begin(store, &begin, &report) == CBM_STORE_REPLAYED);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM codex_task_lifecycle;") == rows);
    free(report);
    begin.prompt_sha256 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_begin(store, &begin, &report) == CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT_OR_FAIL(report && strstr(report, "IDEMPOTENCY_CONFLICT"));
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM codex_task_lifecycle;") == rows);
    free(report);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM codex_task_lifecycle WHERE prompt_sha256 LIKE '%raw-prompt-marker%';") == 0);
    cbm_store_close(store);
    return 0;
}

static int test_evidence_complete_and_rollback(void) {
    cbm_store_t *store = cbm_store_open_memory();
    ASSERT_OR_FAIL(store != NULL);
    bool replayed = false;
    char *report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_migrate(store, &replayed, &report) == CBM_STORE_OK);
    free(report);
    cbm_task_begin_input_t begin = {
        .project = "stage12-fixture", .session_id = "session-2", .turn_id = "turn-2",
        .prompt_sha256 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
        .prompt_length = 12, .idempotency_key = "stage12-begin-key-2"
    };
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_begin(store, &begin, &report) == CBM_STORE_OK);
    char task_id[128];
    ASSERT_OR_FAIL(report_task_id(report, task_id));
    free(report);
    cbm_task_evidence_input_t evidence = {
        .task_id = task_id, .result_id = "result-stage12-1",
        .result_hash = "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
        .evidence_id = "evidence-stage12-1",
        .evidence_hash = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
        .evidence_trust = "external_verified", .evidence_source = "runtime",
        .idempotency_key = "stage12-evidence-key-1"
    };
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_record_evidence(store, &evidence, &report) == CBM_STORE_OK);
    free(report);
    sqlite3 *db = cbm_store_get_db(store);
    int evidence_rows = scalar(db, "SELECT COUNT(*) FROM memory_evidence;");
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_record_evidence(store, &evidence, &report) == CBM_STORE_REPLAYED);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM memory_evidence;") == evidence_rows);
    free(report);
    evidence.evidence_hash = "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_record_evidence(store, &evidence, &report) == CBM_STORE_IDEMPOTENCY_CONFLICT);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM memory_evidence;") == evidence_rows);
    free(report);
    cbm_task_complete_input_t complete = {
        .project = "stage12-fixture", .task_id = task_id, .outcome = "completed",
        .idempotency_key = "stage12-complete-key-1", .attributions = NULL,
        .attribution_count = 0
    };
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_complete(store, &complete, &report) == CBM_STORE_OK);
    free(report);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM feedback_event;") == 0);
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_complete(store, &complete, &report) == CBM_STORE_REPLAYED);
    free(report);
    int terminal_lifecycle_rows = scalar(db, "SELECT COUNT(*) FROM codex_task_lifecycle;");
    int terminal_evidence_rows = scalar(db, "SELECT COUNT(*) FROM memory_evidence;");
    int terminal_result_rows = scalar(db, "SELECT COUNT(*) FROM memory_task_result;");
    evidence.result_id = "result-stage12-after-terminal";
    evidence.result_hash = "1111111111111111111111111111111111111111111111111111111111111111";
    evidence.evidence_id = "evidence-stage12-after-terminal";
    evidence.evidence_hash = "2222222222222222222222222222222222222222222222222222222222222222";
    evidence.idempotency_key = "stage12-evidence-after-terminal-key-1";
    report = NULL;
    ASSERT_OR_FAIL(cbm_orchestrator_record_evidence(store, &evidence, &report) == CBM_STORE_REPLAYED);
    ASSERT_OR_FAIL(report && strstr(report, "\"code\":\"TASK_ALREADY_TERMINAL\"") &&
                   strstr(report, "\"production_state_written\":false"));
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM codex_task_lifecycle;") == terminal_lifecycle_rows);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM memory_evidence;") == terminal_evidence_rows);
    ASSERT_OR_FAIL(scalar(db, "SELECT COUNT(*) FROM memory_task_result;") == terminal_result_rows);
    free(report);
    cbm_store_close(store);
    return 0;
}

int main(void) {
    int failed = 0;
    failed += test_migration_and_replay();
    failed += test_lifecycle_replay_conflict_and_hash_only();
    failed += test_evidence_complete_and_rollback();
    fprintf(stderr, "%s: 3 tests\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
