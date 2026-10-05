#include "memory/memory_orchestrator.h"

#include "store/store.h"
#include "yyjson/yyjson.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *type;
    const char *name;
    const char *sql;
} stage12_object_t;

static const stage12_object_t STAGE12_OBJECTS[] = {
    {"table", "stage12_component_ledger",
     "CREATE TABLE stage12_component_ledger(component TEXT NOT NULL,version INTEGER NOT NULL,name "
     "TEXT NOT NULL,checksum TEXT NOT NULL,applied_at TEXT NOT NULL,PRIMARY "
     "KEY(component,version),UNIQUE(component,name))"},
    {"table", "codex_task_lifecycle",
     "CREATE TABLE codex_task_lifecycle(lifecycle_id TEXT PRIMARY KEY,task_id TEXT NOT NULL "
     "REFERENCES memory_task(task_id) ON DELETE RESTRICT,session_id TEXT NOT NULL,turn_id TEXT NOT "
     "NULL,prompt_sha256 TEXT NOT NULL,prompt_length INTEGER NOT NULL "
     "CHECK(prompt_length>=0),retrieval_session_id TEXT REFERENCES retrieval_session(id) ON DELETE "
     "RESTRICT,state TEXT NOT NULL CHECK(state IN "
     "('started','recall_completed','evidence_observed','completed','degraded','failed','cancelled'"
     ",'abandoned')),outcome TEXT CHECK(outcome IS NULL OR outcome IN "
     "('completed','failed','cancelled','abandoned')),idempotency_key TEXT NOT NULL "
     "UNIQUE,payload_sha256 TEXT NOT NULL,policy_version TEXT NOT NULL,created_at TEXT NOT NULL)"},
    {"index", "codex_task_lifecycle_task_idx",
     "CREATE INDEX codex_task_lifecycle_task_idx ON codex_task_lifecycle(task_id,created_at)"},
    {"index", "codex_task_lifecycle_session_idx",
     "CREATE INDEX codex_task_lifecycle_session_idx ON "
     "codex_task_lifecycle(session_id,turn_id,created_at)"},
    {"table", "codex_task_attribution",
     "CREATE TABLE codex_task_attribution(attribution_id TEXT PRIMARY KEY,task_id TEXT NOT NULL "
     "REFERENCES memory_task(task_id) ON DELETE RESTRICT,memory_item_id TEXT NOT NULL REFERENCES "
     "memory_item(id) ON DELETE RESTRICT,attribution_state TEXT NOT NULL CHECK(attribution_state "
     "IN ('retrieved','selected','injected','used','rejected','contradicted')),evidence_id TEXT "
     "REFERENCES memory_evidence(evidence_id) ON DELETE RESTRICT,feedback_event_id TEXT REFERENCES "
     "feedback_event(event_id) ON DELETE RESTRICT,idempotency_key TEXT NOT NULL "
     "UNIQUE,payload_sha256 TEXT NOT NULL,created_at TEXT NOT NULL)"},
    {"index", "codex_task_attribution_task_idx",
     "CREATE INDEX codex_task_attribution_task_idx ON codex_task_attribution(task_id,created_at)"},
    {"index", "codex_task_attribution_memory_idx",
     "CREATE INDEX codex_task_attribution_memory_idx ON "
     "codex_task_attribution(memory_item_id,created_at)"},
    {"trigger", "stage12_component_ledger_no_update",
     "CREATE TRIGGER stage12_component_ledger_no_update BEFORE UPDATE ON stage12_component_ledger "
     "BEGIN SELECT RAISE(ABORT,'append-only'); END"},
    {"trigger", "stage12_component_ledger_no_delete",
     "CREATE TRIGGER stage12_component_ledger_no_delete BEFORE DELETE ON stage12_component_ledger "
     "BEGIN SELECT RAISE(ABORT,'append-only'); END"},
    {"trigger", "codex_task_lifecycle_no_update",
     "CREATE TRIGGER codex_task_lifecycle_no_update BEFORE UPDATE ON codex_task_lifecycle BEGIN "
     "SELECT RAISE(ABORT,'append-only'); END"},
    {"trigger", "codex_task_lifecycle_no_delete",
     "CREATE TRIGGER codex_task_lifecycle_no_delete BEFORE DELETE ON codex_task_lifecycle BEGIN "
     "SELECT RAISE(ABORT,'append-only'); END"},
    {"trigger", "codex_task_attribution_no_update",
     "CREATE TRIGGER codex_task_attribution_no_update BEFORE UPDATE ON codex_task_attribution "
     "BEGIN SELECT RAISE(ABORT,'append-only'); END"},
    {"trigger", "codex_task_attribution_no_delete",
     "CREATE TRIGGER codex_task_attribution_no_delete BEFORE DELETE ON codex_task_attribution "
     "BEGIN SELECT RAISE(ABORT,'append-only'); END"},
};

static void s12_timestamp(char output[40]) {
    time_t shifted = time(NULL) + 8 * 60 * 60;
    struct tm tm_value;
#ifdef _WIN32
    gmtime_s(&tm_value, &shifted);
#else
    gmtime_r(&shifted, &tm_value);
#endif
    strftime(output, 40, "%Y-%m-%dT%H:%M:%S+08:00", &tm_value);
}

static bool s12_hex64(const char *value) {
    if (!value || strlen(value) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

static bool s12_text(const char *value, size_t maximum) {
    if (!value || value[0] == '\0')
        return false;
    size_t length = strlen(value);
    return length > 0 && length <= maximum;
}

static int s12_hash(const char *value, char output[65]) {
    return value && cbm_stage7_sha256_hex(value, strlen(value), output) == CBM_STORE_OK
               ? CBM_STORE_OK
               : CBM_STORE_ERR;
}

static int s12_id(const char *prefix, const char *value, char *output, size_t output_size) {
    char hash[65];
    if (!prefix || !value || s12_hash(value, hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int written = snprintf(output, output_size, "%s%s", prefix, hash);
    return written > 0 && (size_t)written < output_size ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int s12_exec(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    sqlite3_free(error);
    return rc == SQLITE_OK ? CBM_STORE_OK : CBM_STORE_ERR;
}

static void s12_bind_nullable(sqlite3_stmt *stmt, int column, const char *value) {
    if (value)
        sqlite3_bind_text(stmt, column, value, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, column);
}

static char *s12_write_json(yyjson_mut_doc *doc) {
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *s12_result(const char *status, const char *code, const char *task_id,
                        const char *payload_sha256, bool production_written) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-task-result/v1");
    yyjson_mut_obj_add_str(doc, root, "status", status ? status : "error");
    yyjson_mut_obj_add_str(doc, root, "code", code ? code : "ORCHESTRATOR_FAILED");
    if (task_id)
        yyjson_mut_obj_add_str(doc, root, "task_id", task_id);
    if (payload_sha256)
        yyjson_mut_obj_add_str(doc, root, "payload_sha256", payload_sha256);
    yyjson_mut_obj_add_bool(doc, root, "production_state_written", production_written);
    yyjson_mut_obj_add_bool(doc, root, "raw_prompt_stored", false);
    yyjson_mut_obj_add_bool(doc, root, "raw_tool_io_stored", false);
    return s12_write_json(doc);
}

static bool s12_schema_present(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                           "name='codex_task_lifecycle';",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool present = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return present;
}

static bool s12_schema_exact(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    const char *query = "SELECT type,sql FROM sqlite_master WHERE name=?1;";
    for (size_t i = 0; i < sizeof(STAGE12_OBJECTS) / sizeof(STAGE12_OBJECTS[0]); i++) {
        if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, STAGE12_OBJECTS[i].name, -1, SQLITE_STATIC);
        bool exact = sqlite3_step(stmt) == SQLITE_ROW;
        const char *type = exact ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
        const char *sql = exact ? (const char *)sqlite3_column_text(stmt, 1) : NULL;
        exact = exact && type && sql && strcmp(type, STAGE12_OBJECTS[i].type) == 0 &&
                strcmp(sql, STAGE12_OBJECTS[i].sql) == 0;
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (!exact)
            return false;
    }
    return true;
}

static int s12_existing_migration(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT name,checksum FROM stage12_component_ledger WHERE component=?1 AND version=?2;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_NOT_FOUND;
    sqlite3_bind_text(stmt, 1, CBM_STAGE12_COMPONENT, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, CBM_STAGE12_SCHEMA_VERSION);
    int step = sqlite3_step(stmt);
    int result = CBM_STORE_NOT_FOUND;
    if (step == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *checksum = (const char *)sqlite3_column_text(stmt, 1);
        result = name && checksum && strcmp(name, CBM_STAGE12_MIGRATION_NAME) == 0 &&
                         strcmp(checksum, CBM_STAGE12_MIGRATION_SHA256) == 0
                     ? CBM_STORE_REPLAYED
                     : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);
    return result;
}

int cbm_orchestrator_migrate(cbm_store_t *store, bool *out_replayed, char **out_report_json) {
    if (out_replayed)
        *out_replayed = false;
    if (out_report_json)
        *out_report_json = NULL;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !out_report_json)
        return CBM_STORE_ERR;
    if (s12_schema_present(db)) {
        int existing = s12_existing_migration(db);
        if (existing == CBM_STORE_REPLAYED && s12_schema_exact(db)) {
            if (out_replayed)
                *out_replayed = true;
            *out_report_json =
                s12_result("replayed", "OK", NULL, CBM_STAGE12_MIGRATION_SHA256, false);
            return *out_report_json ? CBM_STORE_REPLAYED : CBM_STORE_ERR;
        }
        *out_report_json = s12_result("conflict", "SCHEMA_CONFLICT", NULL, NULL, false);
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_stmt *inventory = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'stage12_%' OR name "
                           "LIKE 'codex_task_%';",
                           -1, &inventory, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    bool clean = sqlite3_step(inventory) == SQLITE_ROW && sqlite3_column_int(inventory, 0) == 0;
    sqlite3_finalize(inventory);
    if (!clean) {
        *out_report_json = s12_result("conflict", "SCHEMA_PARTIAL", NULL, NULL, false);
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    if (s12_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK) {
        *out_report_json = s12_result("error", "DB_TIMEOUT", NULL, NULL, false);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    for (size_t i = 0;
         rc == CBM_STORE_OK && i < sizeof(STAGE12_OBJECTS) / sizeof(STAGE12_OBJECTS[0]); i++) {
        rc = s12_exec(db, STAGE12_OBJECTS[i].sql);
    }
    char timestamp[40];
    s12_timestamp(timestamp);
    sqlite3_stmt *ledger = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(
            db,
            "INSERT INTO stage12_component_ledger(component,version,name,checksum,applied_at) "
            "VALUES(?1,?2,?3,?4,?5);",
            -1, &ledger, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ledger, 1, CBM_STAGE12_COMPONENT, -1, SQLITE_STATIC);
        sqlite3_bind_int(ledger, 2, CBM_STAGE12_SCHEMA_VERSION);
        sqlite3_bind_text(ledger, 3, CBM_STAGE12_MIGRATION_NAME, -1, SQLITE_STATIC);
        sqlite3_bind_text(ledger, 4, CBM_STAGE12_MIGRATION_SHA256, -1, SQLITE_STATIC);
        sqlite3_bind_text(ledger, 5, timestamp, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ledger) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK) {
        rc = CBM_STORE_ERR;
    }
    sqlite3_finalize(ledger);
    if (rc == CBM_STORE_OK && !s12_schema_exact(db))
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK)
        rc = s12_exec(db, "COMMIT;");
    else
        s12_exec(db, "ROLLBACK;");
    *out_report_json = s12_result(rc == CBM_STORE_OK ? "recorded" : "error",
                                  rc == CBM_STORE_OK ? "OK" : "MIGRATION_FAILED", NULL,
                                  CBM_STAGE12_MIGRATION_SHA256, rc == CBM_STORE_OK);
    return rc;
}

static int s12_replay(sqlite3 *db, const char *idempotency_key, const char *payload_hash,
                      char task_id[80]) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT task_id,payload_sha256 FROM codex_task_lifecycle WHERE idempotency_key=?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, idempotency_key, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    int result = CBM_STORE_NOT_FOUND;
    if (step == SQLITE_ROW) {
        const char *stored_task = (const char *)sqlite3_column_text(stmt, 0);
        const char *stored_hash = (const char *)sqlite3_column_text(stmt, 1);
        if (stored_task)
            snprintf(task_id, 80, "%s", stored_task);
        result = stored_hash && strcmp(stored_hash, payload_hash) == 0
                     ? CBM_STORE_REPLAYED
                     : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);
    return result;
}

static char *s12_begin_payload(const cbm_task_begin_input_t *input) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "session_id", input->session_id);
    yyjson_mut_obj_add_str(doc, root, "turn_id", input->turn_id);
    yyjson_mut_obj_add_str(doc, root, "prompt_sha256", input->prompt_sha256);
    yyjson_mut_obj_add_int(doc, root, "prompt_length", input->prompt_length);
    if (input->retrieval_session_id)
        yyjson_mut_obj_add_str(doc, root, "retrieval_session_id", input->retrieval_session_id);
    else
        yyjson_mut_obj_add_null(doc, root, "retrieval_session_id");
    return s12_write_json(doc);
}

static int s12_insert_lifecycle(sqlite3 *db, const char *task_id, const char *session_id,
                                const char *turn_id, const char *prompt_sha256, int prompt_length,
                                const char *retrieval_session_id, const char *state,
                                const char *outcome, const char *idempotency_key,
                                const char *payload_sha256, const char *created_at) {
    char lifecycle_id[80];
    if (s12_id("life-", idempotency_key, lifecycle_id, sizeof(lifecycle_id)) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO "
        "codex_task_lifecycle(lifecycle_id,task_id,session_id,turn_id,prompt_sha256,prompt_length,"
        "retrieval_session_id,state,outcome,idempotency_key,payload_sha256,policy_version,created_"
        "at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    const char *values[] = {lifecycle_id, task_id, session_id, turn_id, prompt_sha256};
    for (int i = 0; i < 5; i++)
        sqlite3_bind_text(stmt, i + 1, values[i], -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 6, prompt_length);
    s12_bind_nullable(stmt, 7, retrieval_session_id);
    sqlite3_bind_text(stmt, 8, state, -1, SQLITE_STATIC);
    s12_bind_nullable(stmt, 9, outcome);
    sqlite3_bind_text(stmt, 10, idempotency_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, payload_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 12, CBM_STAGE12_POLICY_VERSION, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 13, created_at, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

int cbm_orchestrator_begin(cbm_store_t *store, const cbm_task_begin_input_t *input,
                           char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !input || !out_report_json || !s12_schema_present(db) ||
        !s12_text(input->project, 255) || !s12_text(input->session_id, 255) ||
        !s12_text(input->turn_id, 255) || !s12_hex64(input->prompt_sha256) ||
        input->prompt_length < 0 || !s12_text(input->idempotency_key, 512)) {
        if (out_report_json)
            *out_report_json = s12_result("error", "INVALID_ARGUMENT", NULL, NULL, false);
        return CBM_STORE_ERR;
    }
    char *payload = s12_begin_payload(input);
    char payload_hash[65];
    if (!payload || s12_hash(payload, payload_hash) != CBM_STORE_OK) {
        free(payload);
        *out_report_json = s12_result("error", "HASH_FAILED", NULL, NULL, false);
        return CBM_STORE_ERR;
    }
    char task_id[80] = {0};
    int replay = s12_replay(db, input->idempotency_key, payload_hash, task_id);
    if (replay == CBM_STORE_REPLAYED || replay == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        *out_report_json = s12_result(replay == CBM_STORE_REPLAYED ? "replayed" : "conflict",
                                      replay == CBM_STORE_REPLAYED ? "OK" : "IDEMPOTENCY_CONFLICT",
                                      task_id[0] ? task_id : NULL, payload_hash, false);
        free(payload);
        return replay;
    }
    if (s12_id("task-", input->idempotency_key, task_id, sizeof(task_id)) != CBM_STORE_OK) {
        free(payload);
        return CBM_STORE_ERR;
    }
    if (input->retrieval_session_id) {
        sqlite3_stmt *scope = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT COUNT(*) FROM retrieval_session WHERE id=?1 AND "
                               "project_scope=?2 AND status='completed';",
                               -1, &scope, NULL) != SQLITE_OK) {
            free(payload);
            return CBM_STORE_ERR;
        }
        sqlite3_bind_text(scope, 1, input->retrieval_session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(scope, 2, input->project, -1, SQLITE_TRANSIENT);
        bool exact = sqlite3_step(scope) == SQLITE_ROW && sqlite3_column_int(scope, 0) == 1;
        sqlite3_finalize(scope);
        if (!exact) {
            *out_report_json =
                s12_result("error", "RETRIEVAL_SESSION_NOT_FOUND", NULL, payload_hash, false);
            free(payload);
            return CBM_STORE_NOT_FOUND;
        }
    }
    bool nested_transaction = sqlite3_get_autocommit(db) == 0;
    const char *begin_sql = nested_transaction ? "SAVEPOINT cbm_task_begin;" : "BEGIN IMMEDIATE;";
    if (s12_exec(db, begin_sql) != CBM_STORE_OK) {
        *out_report_json = s12_result("error", "DB_TIMEOUT", NULL, payload_hash, false);
        free(payload);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    char timestamp[40];
    s12_timestamp(timestamp);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO memory_task(task_id,project,task_type,created_at) "
                           "VALUES(?1,?2,'user_task',?3);",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, input->project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        rc = s12_insert_lifecycle(db, task_id, input->session_id, input->turn_id,
                                  input->prompt_sha256, input->prompt_length,
                                  input->retrieval_session_id, "started", NULL,
                                  input->idempotency_key, payload_hash, timestamp);
    if (rc == CBM_STORE_OK && input->retrieval_session_id) {
        if (sqlite3_prepare_v2(
                db,
                "INSERT INTO memory_task_session(task_id,session_id,linked_at) VALUES(?1,?2,?3);",
                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, input->retrieval_session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        } else
            rc = CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    if (rc == CBM_STORE_OK && input->retrieval_session_id) {
        sqlite3_stmt *candidates = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT id,memory_item_id,decision_status FROM retrieval_candidate "
                               "WHERE session_id=?1 ORDER BY aggregate_rank,id;",
                               -1, &candidates, NULL) != SQLITE_OK)
            rc = CBM_STORE_ERR;
        if (candidates)
            sqlite3_bind_text(candidates, 1, input->retrieval_session_id, -1, SQLITE_TRANSIENT);
        while (rc == CBM_STORE_OK && sqlite3_step(candidates) == SQLITE_ROW) {
            const char *candidate_id = (const char *)sqlite3_column_text(candidates, 0);
            const char *memory_item_id = (const char *)sqlite3_column_text(candidates, 1);
            const char *decision = (const char *)sqlite3_column_text(candidates, 2);
            const char *state =
                decision && strcmp(decision, "selected") == 0
                    ? "selected"
                    : (decision && strcmp(decision, "contradicted") == 0
                           ? "contradicted"
                           : (decision && strcmp(decision, "rejected") == 0 ? "rejected"
                                                                            : "retrieved"));
            char key[1024];
            snprintf(key, sizeof(key), "%s:candidate:%s:%s", input->idempotency_key,
                     candidate_id ? candidate_id : "", state);
            char attribution_id[80], attribution_hash[65];
            if (s12_id("attr-", key, attribution_id, sizeof(attribution_id)) != CBM_STORE_OK ||
                s12_hash(key, attribution_hash) != CBM_STORE_OK) {
                rc = CBM_STORE_ERR;
                break;
            }
            if (sqlite3_prepare_v2(
                    db,
                    "INSERT INTO "
                    "codex_task_attribution(attribution_id,task_id,memory_item_id,attribution_"
                    "state,evidence_id,feedback_event_id,idempotency_key,payload_sha256,created_at)"
                    " VALUES(?1,?2,?3,?4,NULL,NULL,?5,?6,?7);",
                    -1, &stmt, NULL) != SQLITE_OK) {
                rc = CBM_STORE_ERR;
                break;
            }
            sqlite3_bind_text(stmt, 1, attribution_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, memory_item_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, state, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 5, key, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, attribution_hash, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, timestamp, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
            sqlite3_finalize(stmt);
            stmt = NULL;
        }
        sqlite3_finalize(candidates);
    }
    if (rc == CBM_STORE_OK) {
        char recall_key[1024], recall_hash[65];
        snprintf(recall_key, sizeof(recall_key), "%s:recall", input->idempotency_key);
        char recall_payload[1024];
        snprintf(recall_payload, sizeof(recall_payload), "task=%s;retrieval=%s", task_id,
                 input->retrieval_session_id ? input->retrieval_session_id : "");
        rc = s12_hash(recall_payload, recall_hash);
        if (rc == CBM_STORE_OK)
            rc = s12_insert_lifecycle(db, task_id, input->session_id, input->turn_id,
                                      input->prompt_sha256, input->prompt_length,
                                      input->retrieval_session_id, "recall_completed", NULL,
                                      recall_key, recall_hash, timestamp);
    }
    if (rc == CBM_STORE_OK) {
        rc = s12_exec(db, nested_transaction ? "RELEASE cbm_task_begin;" : "COMMIT;");
    } else if (nested_transaction) {
        s12_exec(db, "ROLLBACK TO cbm_task_begin;");
        s12_exec(db, "RELEASE cbm_task_begin;");
    } else {
        s12_exec(db, "ROLLBACK;");
    }
    *out_report_json = s12_result(
        rc == CBM_STORE_OK ? "recorded" : "error", rc == CBM_STORE_OK ? "OK" : "TASK_BEGIN_FAILED",
        rc == CBM_STORE_OK ? task_id : NULL, payload_hash, rc == CBM_STORE_OK);
    free(payload);
    return rc;
}

static bool s12_task_identity(sqlite3 *db, const char *task_id, const char *project,
                              char session_id[256], char turn_id[256], char prompt_hash[65],
                              int *prompt_length, char retrieval_id[256]) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT l.session_id,l.turn_id,l.prompt_sha256,l.prompt_length,l.retrieval_session_id "
        "FROM codex_task_lifecycle l JOIN memory_task t ON t.task_id=l.task_id "
        "WHERE l.task_id=?1 AND t.project=?2 ORDER BY l.rowid LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found) {
        snprintf(session_id, 256, "%s", sqlite3_column_text(stmt, 0));
        snprintf(turn_id, 256, "%s", sqlite3_column_text(stmt, 1));
        snprintf(prompt_hash, 65, "%s", sqlite3_column_text(stmt, 2));
        *prompt_length = sqlite3_column_int(stmt, 3);
        const char *retrieval = (const char *)sqlite3_column_text(stmt, 4);
        snprintf(retrieval_id, 256, "%s", retrieval ? retrieval : "");
    }
    sqlite3_finalize(stmt);
    return found;
}

static int s12_task_terminal(sqlite3 *db, const char *task_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT 1 FROM codex_task_lifecycle WHERE task_id=?1 "
                      "AND state IN ('completed','failed','cancelled','abandoned') LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step == SQLITE_ROW)
        return 1;
    return step == SQLITE_DONE ? 0 : -1;
}

int cbm_orchestrator_status(cbm_store_t *store, const char *project, const char *task_id,
                            const char *session_id, const char *turn_id, char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !project || !out_report_json || !s12_schema_present(db))
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql_by_task =
        "SELECT "
        "l.task_id,l.state,l.outcome,l.session_id,l.turn_id,l.retrieval_session_id,l.created_at "
        "FROM codex_task_lifecycle l JOIN memory_task t ON t.task_id=l.task_id "
        "WHERE t.project=?1 AND l.task_id=?2 ORDER BY l.rowid DESC LIMIT 1;";
    const char *sql_by_turn =
        "SELECT "
        "l.task_id,l.state,l.outcome,l.session_id,l.turn_id,l.retrieval_session_id,l.created_at "
        "FROM codex_task_lifecycle l JOIN memory_task t ON t.task_id=l.task_id "
        "WHERE t.project=?1 AND l.session_id=?2 AND l.turn_id=?3 ORDER BY l.rowid DESC LIMIT 1;";
    const char *sql = task_id ? sql_by_task : sql_by_turn;
    if ((!task_id && (!session_id || !turn_id)) ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id ? task_id : session_id, -1, SQLITE_TRANSIENT);
    if (!task_id)
        sqlite3_bind_text(stmt, 3, turn_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        *out_report_json = s12_result("error", "TASK_NOT_FOUND", NULL, NULL, false);
        return CBM_STORE_NOT_FOUND;
    }
    const char *found_task = (const char *)sqlite3_column_text(stmt, 0);
    const char *state = (const char *)sqlite3_column_text(stmt, 1);
    const char *outcome = (const char *)sqlite3_column_text(stmt, 2);
    const char *found_session = (const char *)sqlite3_column_text(stmt, 3);
    const char *found_turn = (const char *)sqlite3_column_text(stmt, 4);
    const char *retrieval = (const char *)sqlite3_column_text(stmt, 5);
    const char *created = (const char *)sqlite3_column_text(stmt, 6);
    char task_buf[256], state_buf[64], outcome_buf[64], session_buf[256], turn_buf[256],
        retrieval_buf[256], created_buf[64];
    snprintf(task_buf, sizeof(task_buf), "%s", found_task ? found_task : "");
    snprintf(state_buf, sizeof(state_buf), "%s", state ? state : "");
    snprintf(outcome_buf, sizeof(outcome_buf), "%s", outcome ? outcome : "");
    snprintf(session_buf, sizeof(session_buf), "%s", found_session ? found_session : "");
    snprintf(turn_buf, sizeof(turn_buf), "%s", found_turn ? found_turn : "");
    snprintf(retrieval_buf, sizeof(retrieval_buf), "%s", retrieval ? retrieval : "");
    snprintf(created_buf, sizeof(created_buf), "%s", created ? created : "");
    sqlite3_finalize(stmt);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-task-status/v1");
    yyjson_mut_obj_add_str(doc, root, "status", "ok");
    yyjson_mut_obj_add_str(doc, root, "task_id", task_buf);
    yyjson_mut_obj_add_str(doc, root, "state", state_buf);
    if (outcome_buf[0])
        yyjson_mut_obj_add_str(doc, root, "outcome", outcome_buf);
    else
        yyjson_mut_obj_add_null(doc, root, "outcome");
    yyjson_mut_obj_add_str(doc, root, "session_id", session_buf);
    yyjson_mut_obj_add_str(doc, root, "turn_id", turn_buf);
    if (retrieval_buf[0])
        yyjson_mut_obj_add_str(doc, root, "retrieval_session_id", retrieval_buf);
    else
        yyjson_mut_obj_add_null(doc, root, "retrieval_session_id");
    yyjson_mut_obj_add_str(doc, root, "updated_at", created_buf);
    yyjson_mut_val *evidence = yyjson_mut_arr(doc);
    if (sqlite3_prepare_v2(
            db,
            "SELECT evidence_id,result_id,evidence_hash,trust_class,evidence_state,source_type "
            "FROM memory_evidence WHERE task_id=?1 ORDER BY rowid DESC LIMIT 16;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_buf, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, item, "evidence_id",
                                      (const char *)sqlite3_column_text(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc, item, "result_id",
                                      (const char *)sqlite3_column_text(stmt, 1));
            yyjson_mut_obj_add_strcpy(doc, item, "evidence_hash",
                                      (const char *)sqlite3_column_text(stmt, 2));
            yyjson_mut_obj_add_strcpy(doc, item, "trust",
                                      (const char *)sqlite3_column_text(stmt, 3));
            yyjson_mut_obj_add_strcpy(doc, item, "state",
                                      (const char *)sqlite3_column_text(stmt, 4));
            yyjson_mut_obj_add_strcpy(doc, item, "source",
                                      (const char *)sqlite3_column_text(stmt, 5));
            yyjson_mut_arr_append(evidence, item);
        }
    }
    sqlite3_finalize(stmt);
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);
    *out_report_json = s12_write_json(doc);
    return *out_report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

int cbm_orchestrator_record_evidence(cbm_store_t *store, const cbm_task_evidence_input_t *input,
                                     char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !input || !out_report_json || !s12_text(input->task_id, 255) ||
        !s12_text(input->result_id, 255) || !s12_hex64(input->result_hash) ||
        !s12_text(input->evidence_id, 255) || !s12_hex64(input->evidence_hash) ||
        !s12_text(input->idempotency_key, 512))
        return CBM_STORE_ERR;
    const char *trust = input->evidence_trust ? input->evidence_trust : "model_self_report";
    const char *source = input->evidence_source ? input->evidence_source : "runtime";
    char payload[2048];
    snprintf(payload, sizeof(payload),
             "task=%s;result=%s;result_hash=%s;evidence=%s;evidence_hash=%s;trust=%s;source=%s",
             input->task_id, input->result_id, input->result_hash, input->evidence_id,
             input->evidence_hash, trust, source);
    char payload_hash[65], task_buf[80] = {0};
    if (s12_hash(payload, payload_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int replay = s12_replay(db, input->idempotency_key, payload_hash, task_buf);
    if (replay == CBM_STORE_REPLAYED || replay == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        *out_report_json = s12_result(replay == CBM_STORE_REPLAYED ? "replayed" : "conflict",
                                      replay == CBM_STORE_REPLAYED ? "OK" : "IDEMPOTENCY_CONFLICT",
                                      input->task_id, payload_hash, false);
        return replay;
    }
    char session_id[256], turn_id[256], prompt_hash[65], retrieval_id[256];
    int prompt_length = 0;
    sqlite3_stmt *project_stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT project FROM memory_task WHERE task_id=?1;", -1,
                           &project_stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(project_stmt, 1, input->task_id, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(project_stmt) == SQLITE_ROW;
    char project[256] = {0};
    if (found)
        snprintf(project, sizeof(project), "%s", sqlite3_column_text(project_stmt, 0));
    sqlite3_finalize(project_stmt);
    if (!found || !s12_task_identity(db, input->task_id, project, session_id, turn_id, prompt_hash,
                                     &prompt_length, retrieval_id)) {
        *out_report_json = s12_result("error", "TASK_NOT_FOUND", NULL, payload_hash, false);
        return CBM_STORE_NOT_FOUND;
    }
    int terminal = s12_task_terminal(db, input->task_id);
    if (terminal != 0) {
        *out_report_json =
            s12_result(terminal > 0 ? "replayed" : "error",
                       terminal > 0 ? "TASK_ALREADY_TERMINAL" : "TASK_STATE_UNAVAILABLE",
                       input->task_id, payload_hash, false);
        return terminal > 0 ? CBM_STORE_REPLAYED : CBM_STORE_ERR;
    }
    if (s12_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK) {
        *out_report_json = s12_result("error", "DB_TIMEOUT", input->task_id, payload_hash, false);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    char timestamp[40];
    s12_timestamp(timestamp);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO "
            "memory_task_result(result_id,task_id,result_type,status,result_ref,result_hash,"
            "recorded_at) VALUES(?1,?2,'runtime','succeeded','stage12:sha256-only',?3,?4);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, input->result_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, input->task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, input->result_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(
            db,
            "INSERT INTO "
            "memory_evidence(evidence_id,task_id,result_id,trust_class,evidence_state,source_type,"
            "evidence_ref,evidence_hash,supersedes_evidence_id,created_at) "
            "VALUES(?1,?2,?3,?4,'valid',?5,'stage12:sha256-only',?6,NULL,?7);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, input->evidence_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, input->task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, input->result_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, trust, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, input->evidence_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, timestamp, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        rc = s12_insert_lifecycle(db, input->task_id, session_id, turn_id, prompt_hash,
                                  prompt_length, retrieval_id[0] ? retrieval_id : NULL,
                                  "evidence_observed", NULL, input->idempotency_key, payload_hash,
                                  timestamp);
    if (rc == CBM_STORE_OK)
        rc = s12_exec(db, "COMMIT;");
    else
        s12_exec(db, "ROLLBACK;");
    *out_report_json = s12_result(rc == CBM_STORE_OK ? "recorded" : "error",
                                  rc == CBM_STORE_OK ? "OK" : "EVIDENCE_FAILED", input->task_id,
                                  payload_hash, rc == CBM_STORE_OK);
    return rc;
}

static int s12_audit_hash(int64_t sequence, const char *audit_id, const char *feedback_id,
                          const char *operation, const char *before_json, const char *after_json,
                          const char *algorithm, int config_version, const char *prev_hash,
                          const char *created_at, char output[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_sint(doc, root, "sequence_no", sequence);
    yyjson_mut_obj_add_str(doc, root, "event_id", audit_id);
    yyjson_mut_obj_add_str(doc, root, "feedback_event_id", feedback_id);
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    yyjson_mut_obj_add_str(doc, root, "before_json", before_json);
    yyjson_mut_obj_add_str(doc, root, "after_json", after_json);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", algorithm);
    yyjson_mut_obj_add_int(doc, root, "config_version", config_version);
    yyjson_mut_obj_add_str(doc, root, "prev_hash", prev_hash);
    yyjson_mut_obj_add_str(doc, root, "created_at", created_at);
    char *json = s12_write_json(doc);
    int rc = json ? s12_hash(json, output) : CBM_STORE_ERR;
    free(json);
    return rc;
}

static int s12_feedback(sqlite3 *db, const char *task_id, const char *session_id,
                        const char *candidate_id, const char *memory_item_id, const char *state,
                        const char *evidence_id, const char *event_id, const char *idempotency_key,
                        const char *timestamp) {
    char usage_id[80], attribution_id[80], audit_id[80];
    if (s12_id("usage-", idempotency_key, usage_id, sizeof(usage_id)) != CBM_STORE_OK ||
        s12_id("feedback-attr-", event_id, attribution_id, sizeof(attribution_id)) !=
            CBM_STORE_OK ||
        s12_id("audit-", event_id, audit_id, sizeof(audit_id)) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    const char *usage_outcome =
        strcmp(state, "used") == 0
            ? "used"
            : (strcmp(state, "contradicted") == 0 ? "contradicted" : "rejected");
    const char *action = strcmp(state, "used") == 0 ? "confirm" : "reject";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT evidence_hash,trust_class,evidence_state FROM memory_evidence "
                           "WHERE evidence_id=?1 AND task_id=?2;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, evidence_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, task_id, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    char evidence_hash[65] = {0}, trust[32] = {0}, evidence_state[32] = {0};
    if (found) {
        snprintf(evidence_hash, sizeof(evidence_hash), "%s", sqlite3_column_text(stmt, 0));
        snprintf(trust, sizeof(trust), "%s", sqlite3_column_text(stmt, 1));
        snprintf(evidence_state, sizeof(evidence_state), "%s", sqlite3_column_text(stmt, 2));
    }
    sqlite3_finalize(stmt);
    if (!found || strcmp(evidence_state, "valid") != 0)
        return CBM_STORE_NOT_FOUND;
    const char *usage_sql = "INSERT INTO "
                            "memory_usage_attribution(id,session_id,candidate_id,injection_id,"
                            "outcome,evidence_type,evidence_ref,evidence_hash,recorded_at) "
                            "VALUES(?1,?2,?3,NULL,?4,'stage12_task','stage12:sha256-only',?5,?6);";
    if (sqlite3_prepare_v2(db, usage_sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, usage_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, usage_outcome, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, evidence_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK)
        return rc;
    sqlite3_stmt *result_stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT result_id FROM memory_evidence WHERE evidence_id=?1;", -1,
                           &result_stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(result_stmt, 1, evidence_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(result_stmt) != SQLITE_ROW) {
        sqlite3_finalize(result_stmt);
        return CBM_STORE_NOT_FOUND;
    }
    char result_id[256];
    snprintf(result_id, sizeof(result_id), "%s", sqlite3_column_text(result_stmt, 0));
    sqlite3_finalize(result_stmt);
    double reward = strcmp(trust, "explicit_user") == 0 ? 1.0 : 0.75;
    if (strcmp(state, "used") != 0)
        reward = -reward;
    char payload_json[2048], result_json[2048];
    snprintf(payload_json, sizeof(payload_json),
             "{\"schema\":\"stage12-task-feedback/"
             "v1\",\"task_id\":\"%s\",\"memory_item_id\":\"%s\",\"state\":\"%s\",\"evidence_id\":"
             "\"%s\"}",
             task_id, memory_item_id, state, evidence_id);
    snprintf(result_json, sizeof(result_json),
             "{\"schema\":\"stage7-reward-report/"
             "v1\",\"status\":\"attributed\",\"processing_mode\":\"observe_only\",\"reward_mode\":"
             "\"shadow\",\"long_term_state_written\":false,\"edge_reinforcement_enabled\":false,"
             "\"final_reward\":%.2f}",
             reward);
    char payload_hash[65];
    if (s12_hash(payload_json, payload_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    const char *feedback_sql =
        "INSERT INTO "
        "feedback_event(event_id,task_id,session_id,candidate_id,injection_id,usage_id,result_id,"
        "evidence_id,action,processing_mode,canonical_payload_sha256,payload_json,result_json,"
        "supersedes_event_id,algorithm_version,config_version,received_at) "
        "VALUES(?1,?2,?3,?4,NULL,?5,?6,?7,?8,'observe_only',?9,?10,?11,NULL,'stage12-task-"
        "orchestrator-v1',1,?12);";
    if (sqlite3_prepare_v2(db, feedback_sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    const char *values[] = {event_id,     task_id,      session_id,  candidate_id,
                            usage_id,     result_id,    evidence_id, action,
                            payload_hash, payload_json, result_json, timestamp};
    for (int i = 0; i < 12; i++)
        sqlite3_bind_text(stmt, i + 1, values[i], -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK)
        return rc;
    const char *attr_sql =
        "INSERT INTO "
        "feedback_attribution(attribution_id,feedback_event_id,task_id,session_id,candidate_id,"
        "memory_item_id,edge_id,evidence_id,node_contribution,edge_contribution,cap_min,cap_max,"
        "uncapped_reward,final_reward,attribution_status,explanation_json,created_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,NULL,?7,?8,0.0,-1.0,1.0,?8,?8,'attributed',?9,?10);";
    if (sqlite3_prepare_v2(db, attr_sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    const char *attr_values[] = {attribution_id, event_id,       task_id,    session_id,
                                 candidate_id,   memory_item_id, evidence_id};
    for (int i = 0; i < 7; i++)
        sqlite3_bind_text(stmt, i + 1, attr_values[i], -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 8, reward);
    sqlite3_bind_text(stmt, 9, result_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, timestamp, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK)
        return rc;
    int64_t sequence = 1;
    char prev_hash[65];
    memset(prev_hash, '0', 64);
    prev_hash[64] = '\0';
    if (sqlite3_prepare_v2(db,
                           "SELECT sequence_no,event_hash FROM plasticity_audit_event ORDER BY "
                           "sequence_no DESC LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sequence = sqlite3_column_int64(stmt, 0) + 1;
        snprintf(prev_hash, sizeof(prev_hash), "%s", sqlite3_column_text(stmt, 1));
    }
    sqlite3_finalize(stmt);
    char event_hash[65];
    if (s12_audit_hash(sequence, audit_id, event_id, "observe_feedback", "{}", result_json,
                       "stage12-task-orchestrator-v1", 1, prev_hash, timestamp,
                       event_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    const char *audit_sql =
        "INSERT INTO "
        "plasticity_audit_event(sequence_no,event_id,feedback_event_id,operation,before_json,after_"
        "json,algorithm_version,config_version,prev_hash,event_hash,created_at) "
        "VALUES(?1,?2,?3,'observe_feedback','{}',?4,'stage12-task-orchestrator-v1',1,?5,?6,?7);";
    if (sqlite3_prepare_v2(db, audit_sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_int64(stmt, 1, sequence);
    sqlite3_bind_text(stmt, 2, audit_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, result_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, prev_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, event_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, timestamp, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int s12_existing_feedback_validate(sqlite3 *db, const char *event_id, const char *task_id,
                                          const char *session_id, const char *candidate_id,
                                          const char *memory_item_id, const char *state,
                                          const char *evidence_id) {
    if (!s12_text(event_id, 255) || !s12_text(evidence_id, 255))
        return CBM_STORE_REJECTED;
    const char *usage_outcome =
        strcmp(state, "used") == 0
            ? "used"
            : (strcmp(state, "contradicted") == 0 ? "contradicted" : "rejected");
    const char *sql =
        "SELECT COUNT(*) FROM feedback_event f "
        "JOIN feedback_attribution a ON a.feedback_event_id=f.event_id "
        " AND a.task_id=f.task_id AND a.session_id=f.session_id "
        " AND a.candidate_id=f.candidate_id AND a.evidence_id=f.evidence_id "
        "JOIN memory_usage_attribution u ON u.id=f.usage_id "
        " AND u.session_id=f.session_id AND u.candidate_id=f.candidate_id "
        "JOIN memory_evidence e ON e.evidence_id=f.evidence_id "
        " AND e.task_id=f.task_id AND e.result_id=f.result_id "
        "JOIN retrieval_candidate c ON c.id=f.candidate_id AND c.session_id=f.session_id "
        "JOIN memory_task_session s ON s.session_id=c.session_id AND s.task_id=f.task_id "
        "WHERE f.event_id=?1 AND f.task_id=?2 AND f.session_id=?3 AND f.candidate_id=?4 "
        "AND f.evidence_id=?5 AND a.memory_item_id=?6 AND c.memory_item_id=?6 "
        "AND u.outcome=?7 AND e.evidence_state='valid' "
        "AND f.processing_mode='observe_only' AND a.attribution_status='attributed';";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    const char *values[] = {event_id,    task_id,        session_id,   candidate_id,
                            evidence_id, memory_item_id, usage_outcome};
    for (int i = 0; i < 7; i++)
        sqlite3_bind_text(stmt, i + 1, values[i], -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1
                 ? CBM_STORE_OK
                 : CBM_STORE_REJECTED;
    sqlite3_finalize(stmt);
    return rc;
}

static char *s12_complete_payload(const cbm_task_complete_input_t *input) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "task_id", input->task_id);
    yyjson_mut_obj_add_str(doc, root, "outcome", input->outcome);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    for (size_t i = 0; i < input->attribution_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "memory_item_id", input->attributions[i].memory_item_id);
        yyjson_mut_obj_add_str(doc, item, "state", input->attributions[i].state);
        if (input->attributions[i].evidence_id)
            yyjson_mut_obj_add_str(doc, item, "evidence_id", input->attributions[i].evidence_id);
        else
            yyjson_mut_obj_add_null(doc, item, "evidence_id");
        if (input->attributions[i].feedback_event_id)
            yyjson_mut_obj_add_str(doc, item, "feedback_event_id",
                                   input->attributions[i].feedback_event_id);
        else
            yyjson_mut_obj_add_null(doc, item, "feedback_event_id");
        yyjson_mut_arr_append(items, item);
    }
    yyjson_mut_obj_add_val(doc, root, "attributions", items);
    return s12_write_json(doc);
}

int cbm_orchestrator_complete(cbm_store_t *store, const cbm_task_complete_input_t *input,
                              char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    bool outcome_valid =
        input && input->outcome &&
        (strcmp(input->outcome, "completed") == 0 || strcmp(input->outcome, "failed") == 0 ||
         strcmp(input->outcome, "cancelled") == 0 || strcmp(input->outcome, "abandoned") == 0);
    if (!db || !input || !out_report_json || !outcome_valid || !s12_text(input->project, 255) ||
        !s12_text(input->task_id, 255) || !s12_text(input->idempotency_key, 512) ||
        input->attribution_count > 16)
        return CBM_STORE_ERR;
    char *payload = s12_complete_payload(input);
    char payload_hash[65], task_buf[80] = {0};
    if (!payload || s12_hash(payload, payload_hash) != CBM_STORE_OK) {
        free(payload);
        return CBM_STORE_ERR;
    }
    int replay = s12_replay(db, input->idempotency_key, payload_hash, task_buf);
    if (replay == CBM_STORE_REPLAYED || replay == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        *out_report_json = s12_result(replay == CBM_STORE_REPLAYED ? "replayed" : "conflict",
                                      replay == CBM_STORE_REPLAYED ? "OK" : "IDEMPOTENCY_CONFLICT",
                                      input->task_id, payload_hash, false);
        free(payload);
        return replay;
    }
    char session_id[256], turn_id[256], prompt_hash[65], retrieval_id[256];
    int prompt_length = 0;
    if (!s12_task_identity(db, input->task_id, input->project, session_id, turn_id, prompt_hash,
                           &prompt_length, retrieval_id)) {
        *out_report_json = s12_result("error", "TASK_NOT_FOUND", NULL, payload_hash, false);
        free(payload);
        return CBM_STORE_NOT_FOUND;
    }
    if (strcmp(input->outcome, "completed") != 0 && input->attribution_count != 0) {
        *out_report_json = s12_result("error", "OUTCOME_ATTRIBUTION_FORBIDDEN", input->task_id,
                                      payload_hash, false);
        free(payload);
        return CBM_STORE_REJECTED;
    }
    if (s12_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK) {
        *out_report_json = s12_result("error", "DB_TIMEOUT", input->task_id, payload_hash, false);
        free(payload);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    char timestamp[40];
    s12_timestamp(timestamp);
    for (size_t i = 0; rc == CBM_STORE_OK && i < input->attribution_count; i++) {
        const cbm_task_attribution_input_t *attribution = &input->attributions[i];
        bool state_valid = attribution->state && (strcmp(attribution->state, "injected") == 0 ||
                                                  strcmp(attribution->state, "used") == 0 ||
                                                  strcmp(attribution->state, "rejected") == 0 ||
                                                  strcmp(attribution->state, "contradicted") == 0);
        if (!s12_text(attribution->memory_item_id, 255) || !state_valid) {
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_stmt *candidate = NULL;
        const char *candidate_sql =
            "SELECT c.id,c.session_id FROM retrieval_candidate c JOIN memory_task_session s ON "
            "s.session_id=c.session_id WHERE s.task_id=?1 AND c.memory_item_id=?2 ORDER BY "
            "c.aggregate_rank,c.id LIMIT 1;";
        if (sqlite3_prepare_v2(db, candidate_sql, -1, &candidate, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(candidate, 1, input->task_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(candidate, 2, attribution->memory_item_id, -1, SQLITE_TRANSIENT);
        bool found = sqlite3_step(candidate) == SQLITE_ROW;
        char candidate_id[256] = {0}, feedback_session[256] = {0};
        if (found) {
            snprintf(candidate_id, sizeof(candidate_id), "%s", sqlite3_column_text(candidate, 0));
            snprintf(feedback_session, sizeof(feedback_session), "%s",
                     sqlite3_column_text(candidate, 1));
        }
        sqlite3_finalize(candidate);
        if (!found) {
            rc = CBM_STORE_NOT_FOUND;
            break;
        }
        bool feedback_state = strcmp(attribution->state, "used") == 0 ||
                              strcmp(attribution->state, "rejected") == 0 ||
                              strcmp(attribution->state, "contradicted") == 0;
        if (feedback_state && !attribution->evidence_id) {
            rc = CBM_STORE_REJECTED;
            break;
        }
        char key[1024], attribution_id[80], attribution_hash[65], feedback_id[256] = {0};
        snprintf(key, sizeof(key), "%s:attribution:%zu", input->idempotency_key, i);
        if (s12_id("attr-", key, attribution_id, sizeof(attribution_id)) != CBM_STORE_OK ||
            s12_hash(key, attribution_hash) != CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        if (feedback_state) {
            if (attribution->feedback_event_id) {
                snprintf(feedback_id, sizeof(feedback_id), "%s", attribution->feedback_event_id);
                rc = s12_existing_feedback_validate(
                    db, feedback_id, input->task_id, feedback_session, candidate_id,
                    attribution->memory_item_id, attribution->state, attribution->evidence_id);
            } else if (s12_id("feedback-", key, feedback_id, sizeof(feedback_id)) != CBM_STORE_OK) {
                rc = CBM_STORE_ERR;
                break;
            } else {
                rc = s12_feedback(db, input->task_id, feedback_session, candidate_id,
                                  attribution->memory_item_id, attribution->state,
                                  attribution->evidence_id, feedback_id, key, timestamp);
            }
        }
        if (rc == CBM_STORE_OK) {
            sqlite3_stmt *stmt = NULL;
            if (sqlite3_prepare_v2(
                    db,
                    "INSERT INTO "
                    "codex_task_attribution(attribution_id,task_id,memory_item_id,attribution_"
                    "state,evidence_id,feedback_event_id,idempotency_key,payload_sha256,created_at)"
                    " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);",
                    -1, &stmt, NULL) != SQLITE_OK) {
                rc = CBM_STORE_ERR;
            } else {
                sqlite3_bind_text(stmt, 1, attribution_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, input->task_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, attribution->memory_item_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, attribution->state, -1, SQLITE_TRANSIENT);
                s12_bind_nullable(stmt, 5, attribution->evidence_id);
                s12_bind_nullable(stmt, 6, feedback_id[0] ? feedback_id : NULL);
                sqlite3_bind_text(stmt, 7, key, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 8, attribution_hash, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 9, timestamp, -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (rc == CBM_STORE_OK) {
        const char *state = strcmp(input->outcome, "completed") == 0 ? "completed" : input->outcome;
        rc = s12_insert_lifecycle(db, input->task_id, session_id, turn_id, prompt_hash,
                                  prompt_length, retrieval_id[0] ? retrieval_id : NULL, state,
                                  input->outcome, input->idempotency_key, payload_hash, timestamp);
    }
    if (rc == CBM_STORE_OK)
        rc = s12_exec(db, "COMMIT;");
    else
        s12_exec(db, "ROLLBACK;");
    const char *code =
        rc == CBM_STORE_OK
            ? "OK"
            : (rc == CBM_STORE_NOT_FOUND
                   ? "ATTRIBUTION_NOT_RETRIEVED"
                   : (rc == CBM_STORE_REJECTED ? "EVIDENCE_NOT_LINKED" : "TASK_COMPLETE_FAILED"));
    *out_report_json = s12_result(rc == CBM_STORE_OK ? "recorded" : "error", code, input->task_id,
                                  payload_hash, rc == CBM_STORE_OK);
    free(payload);
    return rc;
}

int cbm_orchestrator_abandon_open(cbm_store_t *store, const char *project, const char *session_id,
                                  const char *turn_id, const char *idempotency_key,
                                  char **out_report_json) {
    char *status = NULL;
    int rc = cbm_orchestrator_status(store, project, NULL, session_id, turn_id, &status);
    if (rc != CBM_STORE_OK || !status) {
        free(status);
        if (out_report_json)
            *out_report_json = s12_result("error", "TASK_NOT_FOUND", NULL, NULL, false);
        return rc;
    }
    yyjson_doc *doc = yyjson_read(status, strlen(status), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *task = root ? yyjson_obj_get(root, "task_id") : NULL;
    yyjson_val *state = root ? yyjson_obj_get(root, "state") : NULL;
    char task_id[256] = {0}, state_value[64] = {0};
    if (task && yyjson_is_str(task))
        snprintf(task_id, sizeof(task_id), "%s", yyjson_get_str(task));
    if (state && yyjson_is_str(state))
        snprintf(state_value, sizeof(state_value), "%s", yyjson_get_str(state));
    yyjson_doc_free(doc);
    free(status);
    if (strcmp(state_value, "completed") == 0 || strcmp(state_value, "failed") == 0 ||
        strcmp(state_value, "cancelled") == 0 || strcmp(state_value, "abandoned") == 0) {
        if (out_report_json)
            *out_report_json = s12_result("replayed", "OK", task_id, NULL, false);
        return CBM_STORE_REPLAYED;
    }
    cbm_task_complete_input_t input = {
        .project = project,
        .task_id = task_id,
        .outcome = "abandoned",
        .idempotency_key = idempotency_key,
        .attributions = NULL,
        .attribution_count = 0,
    };
    return cbm_orchestrator_complete(store, &input, out_report_json);
}
