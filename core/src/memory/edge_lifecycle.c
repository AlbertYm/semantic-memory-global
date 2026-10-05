#include "memory/edge_lifecycle.h"

#include "foundation/platform.h"
#include "store/store.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STAGE9_COMPONENT "stage9_edge_lifecycle"
#define STAGE9_COMPONENT_NAME "edge-lifecycle-v1"
#define STAGE9_FIXTURE_PROJECT "stage9-fixture-edge-lifecycle-v1"
#define STAGE14_FIXTURE_PREFIX "stage14-fixture-"
#define STAGE9_PRODUCTION_PROJECT "H-Codex_H-neuroplastic-main"
#define STAGE9_MANIFEST_SCHEMA "stage9-production-canary-manifest/v1"
#define STAGE9_DAY_MS INT64_C(86400000)
#define STAGE9_PPM INT64_C(1000000)

typedef struct {
    const char *type;
    bool protected_relation;
    int grace_days;
    int decay_rate;
    int max_decay;
    int cold_age;
    int cold_conductance;
    int cold_failures;
    int archive_dwell;
    int archive_conductance;
    int archive_failures;
} stage9_relation_policy_t;

static const stage9_relation_policy_t STAGE9_POLICIES[] = {
    {"used_in", false, 30, 4000, 800000, 90, 400000, 2, 90, 200000, 3},
    {"supports", false, 60, 2500, 750000, 180, 425000, 2, 180, 225000, 3},
    {"derived_from", false, 90, 1500, 700000, 270, 450000, 2, 270, 250000, 3},
    {"contradicts", true, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {"supersedes", true, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

typedef struct {
    const char *type;
    const char *name;
    const char *sql;
} stage9_schema_object_t;

static const stage9_schema_object_t STAGE9_OBJECTS[] = {
    {"table", "stage9_component_ledger",
     "CREATE TABLE IF NOT EXISTS stage9_component_ledger(component TEXT NOT NULL,version "
     "INTEGER NOT NULL CHECK(version>=1),name TEXT NOT NULL,checksum TEXT NOT NULL CHECK("
     "length(checksum)=64 AND checksum NOT GLOB '*[^0-9a-f]*'),policy_sha256 TEXT NOT NULL "
     "CHECK(length(policy_sha256)=64 AND policy_sha256 NOT GLOB '*[^0-9a-f]*'),applied_at "
     "TEXT NOT NULL,PRIMARY KEY(component,version));"},
    {"table", "edge_maintenance_run",
     "CREATE TABLE IF NOT EXISTS edge_maintenance_run(run_id TEXT PRIMARY KEY,canonical_request_"
     "sha256 TEXT NOT NULL UNIQUE CHECK(length(canonical_request_sha256)=64 AND canonical_request_"
     "sha256 NOT GLOB '*[^0-9a-f]*'),operation TEXT NOT NULL CHECK(operation IN ('maintenance',"
     "'restore')),mode TEXT NOT NULL CHECK(mode='active'),project TEXT NOT NULL,as_of_ms INTEGER "
     "NOT NULL CHECK(as_of_ms>=0),algorithm_version TEXT NOT NULL,policy_version INTEGER NOT NULL "
     "CHECK(policy_version>=1),config_version INTEGER NOT NULL CHECK(config_version>=1),policy_"
     "sha256 TEXT NOT NULL CHECK(length(policy_sha256)=64 AND policy_sha256 NOT GLOB "
     "'*[^0-9a-f]*'),decision_set_sha256 TEXT NOT NULL CHECK(length(decision_set_sha256)=64 AND "
     "decision_set_sha256 NOT GLOB '*[^0-9a-f]*'),report_sha256 TEXT NOT NULL CHECK(length("
     "report_sha256)=64 AND report_sha256 NOT GLOB '*[^0-9a-f]*'),decision_count INTEGER NOT "
     "NULL CHECK(decision_count>=0),transition_count INTEGER NOT NULL CHECK(transition_count>=0),"
     "created_at TEXT NOT NULL);"},
    {"table", "edge_maintenance_decision",
     "CREATE TABLE IF NOT EXISTS edge_maintenance_decision(decision_id TEXT PRIMARY KEY,run_id "
     "TEXT NOT NULL REFERENCES edge_maintenance_run(run_id) ON DELETE RESTRICT,edge_id TEXT NOT "
     "NULL REFERENCES memory_edge(id) ON DELETE RESTRICT,relation_type TEXT NOT NULL,from_state "
     "TEXT NOT NULL CHECK(from_state IN ('active','cold','archived','disabled')),to_state TEXT NOT "
     "NULL CHECK(to_state IN ('active','cold','archived','disabled')),reason_code TEXT NOT NULL,"
     "protected INTEGER NOT NULL CHECK(protected IN (0,1)),conductance_ppm INTEGER NOT NULL CHECK("
     "conductance_ppm BETWEEN 0 AND 1000000),effective_success_flow_ppm INTEGER NOT NULL CHECK("
     "effective_success_flow_ppm BETWEEN 0 AND 1000000),age_days INTEGER NOT NULL CHECK(age_days"
     ">=0),cold_dwell_days INTEGER NOT NULL CHECK(cold_dwell_days>=0),success_count INTEGER NOT "
     "NULL CHECK(success_count>=0),failure_count INTEGER NOT NULL CHECK(failure_count>=0),decision_"
     "sha256 TEXT NOT NULL CHECK(length(decision_sha256)=64 AND decision_sha256 NOT GLOB "
     "'*[^0-9a-f]*'),created_at TEXT NOT NULL,UNIQUE(run_id,edge_id));"},
    {"table", "edge_lifecycle_audit_event",
     "CREATE TABLE IF NOT EXISTS edge_lifecycle_audit_event(sequence_no INTEGER PRIMARY KEY "
     "AUTOINCREMENT,event_id TEXT NOT NULL UNIQUE,run_id TEXT NOT NULL REFERENCES edge_maintenance_"
     "run(run_id) ON DELETE RESTRICT,edge_id TEXT NOT NULL REFERENCES memory_edge(id) ON DELETE "
     "RESTRICT,operation TEXT NOT NULL CHECK(operation IN ('initialize','transition','restore')),"
     "from_state TEXT NOT NULL CHECK(from_state IN "
     "('active','cold','archived','disabled')),to_state"
     " TEXT NOT NULL CHECK(to_state IN ('active','cold','archived','disabled')),before_state_sha256"
     " TEXT NOT NULL CHECK(length(before_state_sha256)=64 AND before_state_sha256 NOT GLOB "
     "'*[^0-9a-f]*'),after_state_sha256 TEXT NOT NULL CHECK(length(after_state_sha256)=64 AND "
     "after_state_sha256 NOT GLOB '*[^0-9a-f]*'),decision_sha256 TEXT NOT NULL CHECK(length("
     "decision_sha256)=64 AND decision_sha256 NOT GLOB '*[^0-9a-f]*'),algorithm_version TEXT NOT "
     "NULL,policy_version INTEGER NOT NULL CHECK(policy_version>=1),config_version INTEGER NOT NULL"
     " CHECK(config_version>=1),prev_hash TEXT NOT NULL CHECK((sequence_no=1 AND "
     "prev_hash='GENESIS'"
     ") OR (sequence_no>1 AND length(prev_hash)=64 AND prev_hash NOT GLOB '*[^0-9a-f]*')),event_"
     "hash TEXT NOT NULL UNIQUE CHECK(length(event_hash)=64 AND event_hash NOT GLOB '*[^0-9a-f]*'"
     "),created_at TEXT NOT NULL);"},
    {"table", "edge_lifecycle_state",
     "CREATE TABLE IF NOT EXISTS edge_lifecycle_state(edge_id TEXT PRIMARY KEY REFERENCES memory_"
     "edge(id) ON DELETE RESTRICT,lifecycle_state TEXT NOT NULL CHECK(lifecycle_state IN ('active',"
     "'cold','archived','disabled')),conductance_ppm INTEGER NOT NULL CHECK(conductance_ppm BETWEEN"
     " 0 AND 1000000),effective_success_flow_ppm INTEGER NOT NULL CHECK(effective_success_flow_ppm"
     " BETWEEN 0 AND 1000000),last_evaluated_as_of_ms INTEGER NOT NULL CHECK(last_evaluated_as_of_"
     "ms>=0),state_changed_at_ms INTEGER NOT NULL CHECK(state_changed_at_ms>=0),protection_reason "
     "TEXT NOT NULL,version INTEGER NOT NULL CHECK(version>=1),last_audit_event_id TEXT NOT NULL "
     "REFERENCES edge_lifecycle_audit_event(event_id) ON DELETE RESTRICT,algorithm_version TEXT "
     "NOT NULL,policy_version INTEGER NOT NULL CHECK(policy_version>=1),config_version INTEGER NOT"
     " NULL CHECK(config_version>=1),policy_sha256 TEXT NOT NULL CHECK(length(policy_sha256)=64 AND"
     " policy_sha256 NOT GLOB '*[^0-9a-f]*'),state_sha256 TEXT NOT NULL CHECK(length(state_sha256)"
     "=64 AND state_sha256 NOT GLOB '*[^0-9a-f]*'),updated_at TEXT NOT NULL);"},
    {"index", "edge_lifecycle_state_status_idx",
     "CREATE INDEX IF NOT EXISTS edge_lifecycle_state_status_idx ON edge_lifecycle_state("
     "lifecycle_state,edge_id);"},
    {"index", "edge_maintenance_run_as_of_idx",
     "CREATE INDEX IF NOT EXISTS edge_maintenance_run_as_of_idx ON edge_maintenance_run(as_of_ms,"
     "run_id);"},
    {"index", "edge_maintenance_decision_edge_idx",
     "CREATE INDEX IF NOT EXISTS edge_maintenance_decision_edge_idx ON edge_maintenance_decision("
     "edge_id,run_id);"},
    {"index", "edge_lifecycle_audit_edge_idx",
     "CREATE INDEX IF NOT EXISTS edge_lifecycle_audit_edge_idx ON edge_lifecycle_audit_event("
     "edge_id,sequence_no);"},
    {"trigger", "stage9_component_ledger_no_update",
     "CREATE TRIGGER IF NOT EXISTS stage9_component_ledger_no_update BEFORE UPDATE ON stage9_"
     "component_ledger BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "stage9_component_ledger_no_delete",
     "CREATE TRIGGER IF NOT EXISTS stage9_component_ledger_no_delete BEFORE DELETE ON stage9_"
     "component_ledger BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "edge_lifecycle_state_no_delete",
     "CREATE TRIGGER IF NOT EXISTS edge_lifecycle_state_no_delete BEFORE DELETE ON edge_lifecycle_"
     "state BEGIN SELECT RAISE(ABORT,'hard-delete-disabled'); END;"},
    {"trigger", "edge_maintenance_run_no_update",
     "CREATE TRIGGER IF NOT EXISTS edge_maintenance_run_no_update BEFORE UPDATE ON "
     "edge_maintenance_"
     "run BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "edge_maintenance_run_no_delete",
     "CREATE TRIGGER IF NOT EXISTS edge_maintenance_run_no_delete BEFORE DELETE ON "
     "edge_maintenance_"
     "run BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "edge_maintenance_decision_no_update",
     "CREATE TRIGGER IF NOT EXISTS edge_maintenance_decision_no_update BEFORE UPDATE ON edge_"
     "maintenance_decision BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "edge_maintenance_decision_no_delete",
     "CREATE TRIGGER IF NOT EXISTS edge_maintenance_decision_no_delete BEFORE DELETE ON edge_"
     "maintenance_decision BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "edge_lifecycle_audit_no_update",
     "CREATE TRIGGER IF NOT EXISTS edge_lifecycle_audit_no_update BEFORE UPDATE ON edge_lifecycle_"
     "audit_event BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "edge_lifecycle_audit_no_delete",
     "CREATE TRIGGER IF NOT EXISTS edge_lifecycle_audit_no_delete BEFORE DELETE ON edge_lifecycle_"
     "audit_event BEGIN SELECT RAISE(ABORT,'append-only'); END;"},
    {"trigger", "stage9_memory_edge_no_delete",
     "CREATE TRIGGER IF NOT EXISTS stage9_memory_edge_no_delete BEFORE DELETE ON memory_edge BEGIN "
     "SELECT RAISE(ABORT,'hard-delete-disabled'); END;"},
    {"trigger", "stage9_memory_item_no_delete",
     "CREATE TRIGGER IF NOT EXISTS stage9_memory_item_no_delete BEFORE DELETE ON memory_item BEGIN "
     "SELECT RAISE(ABORT,'hard-delete-disabled'); END;"},
    {"trigger", "stage9_memory_event_no_delete",
     "CREATE TRIGGER IF NOT EXISTS stage9_memory_event_no_delete BEFORE DELETE ON memory_event "
     "BEGIN SELECT RAISE(ABORT,'hard-delete-disabled'); END;"},
    {"trigger", "stage9_memory_evidence_no_delete",
     "CREATE TRIGGER IF NOT EXISTS stage9_memory_evidence_no_delete BEFORE DELETE ON memory_"
     "evidence BEGIN SELECT RAISE(ABORT,'hard-delete-disabled'); END;"},
};

typedef struct {
    char *edge_id;
    char *relation_type;
    char *src_kind;
    char *dst_kind;
    char *src_project;
    char *dst_project;
    char *state;
    char *protection_reason;
    int64_t created_at_ms;
    int64_t last_signal_ms;
    int64_t state_changed_at_ms;
    int64_t confidence_ppm;
    int64_t pheromone_ppm;
    int64_t src_importance_ppm;
    int64_t dst_importance_ppm;
    int64_t src_reusability_ppm;
    int64_t dst_reusability_ppm;
    int success_count;
    int failure_count;
} stage9_edge_t;

typedef struct {
    char *edge_id;
    char *relation_type;
    char *from_state;
    char *to_state;
    char *reason_code;
    bool protected_edge;
    int64_t conductance_ppm;
    int64_t success_flow_ppm;
    int64_t age_days;
    int64_t cold_dwell_days;
    int success_count;
    int failure_count;
    char decision_sha256[65];
} stage9_decision_t;

static char *stage9_dup(const char *value) {
    if (!value)
        value = "";
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

static const char *stage9_col_text(sqlite3_stmt *stmt, int index) {
    const unsigned char *value = sqlite3_column_text(stmt, index);
    return value ? (const char *)value : "";
}

static int64_t stage9_clamp(int64_t value, int64_t minimum, int64_t maximum) {
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

static bool stage9_table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *stmt = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            found = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return found == 1;
}

static int stage9_env_int(const char *name) {
    char value[32] = {0};
    cbm_safe_getenv(name, value, sizeof(value), NULL);
    return value[0] ? atoi(value) : 0;
}

static void stage9_iso_from_ms(int64_t value, char out[40]) {
    time_t seconds = (time_t)(value / 1000);
    struct tm utc = {0};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(out + strlen(out), 40 - strlen(out), ".%03lldZ", (long long)(value % 1000));
}

static int stage9_hash_text(const char *text, char out[65]) {
    return cbm_stage7_sha256_hex(text ? text : "", text ? strlen(text) : 0, out);
}

static char *stage9_hash_id(const char *prefix, const char *a, const char *b, const char *c) {
    size_t size = strlen(a ? a : "") + strlen(b ? b : "") + strlen(c ? c : "") + 4;
    char *payload = malloc(size);
    if (!payload)
        return NULL;
    snprintf(payload, size, "%s|%s|%s", a ? a : "", b ? b : "", c ? c : "");
    char hash[65];
    if (stage9_hash_text(payload, hash) != CBM_STORE_OK) {
        free(payload);
        return NULL;
    }
    free(payload);
    size_t out_size = strlen(prefix) + 25;
    char *out = malloc(out_size);
    if (out)
        snprintf(out, out_size, "%s%.24s", prefix, hash);
    return out;
}

static const stage9_relation_policy_t *stage9_policy(const char *type) {
    for (size_t i = 0; i < sizeof(STAGE9_POLICIES) / sizeof(STAGE9_POLICIES[0]); i++) {
        if (type && strcmp(type, STAGE9_POLICIES[i].type) == 0)
            return &STAGE9_POLICIES[i];
    }
    return NULL;
}

static void stage9_edge_clear(stage9_edge_t *edge) {
    if (!edge)
        return;
    free(edge->edge_id);
    free(edge->relation_type);
    free(edge->src_kind);
    free(edge->dst_kind);
    free(edge->src_project);
    free(edge->dst_project);
    free(edge->state);
    free(edge->protection_reason);
    memset(edge, 0, sizeof(*edge));
}

static void stage9_edges_free(stage9_edge_t *edges, int count) {
    for (int i = 0; i < count; i++)
        stage9_edge_clear(&edges[i]);
    free(edges);
}

static void stage9_decision_clear(stage9_decision_t *decision) {
    if (!decision)
        return;
    free(decision->edge_id);
    free(decision->relation_type);
    free(decision->from_state);
    free(decision->to_state);
    free(decision->reason_code);
    memset(decision, 0, sizeof(*decision));
}

static void stage9_decisions_free(stage9_decision_t *decisions, int count) {
    for (int i = 0; i < count; i++)
        stage9_decision_clear(&decisions[i]);
    free(decisions);
}

static int stage9_edge_compare(const void *left, const void *right) {
    const stage9_edge_t *a = left;
    const stage9_edge_t *b = right;
    return strcmp(a->edge_id, b->edge_id);
}

static int stage9_decision_compare(const void *left, const void *right) {
    const stage9_decision_t *a = left;
    const stage9_decision_t *b = right;
    return strcmp(a->edge_id, b->edge_id);
}

int cbm_store_memory_stage9_object_count(cbm_store_t *store) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db)
        return -1;
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'stage9_%' OR name LIKE "
                      "'edge_lifecycle_%' OR name LIKE 'edge_maintenance_%';";
    int count = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

static bool stage9_ledger_valid(sqlite3 *db) {
    if (!stage9_table_exists(db, "stage9_component_ledger"))
        return false;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT COUNT(*) FROM stage9_component_ledger WHERE component=?1 AND version=1 AND "
        "name=?2 AND checksum=?3 AND policy_sha256=?4;";
    int count = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, STAGE9_COMPONENT, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, STAGE9_COMPONENT_NAME, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, CBM_STAGE9_MIGRATION_SHA256, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, CBM_STAGE9_POLICY_SHA256, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count == 1;
}

int cbm_store_memory_stage9_migrate(cbm_store_t *store) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db)
        return CBM_STORE_ERR;
    int count = cbm_store_memory_stage9_object_count(store);
    if (count == (int)(sizeof(STAGE9_OBJECTS) / sizeof(STAGE9_OBJECTS[0])) &&
        stage9_ledger_valid(db)) {
        return CBM_STORE_OK;
    }
    if (count != 0)
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    if (cbm_store_begin(store) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int fail_after = stage9_env_int("CBM_STAGE9_MIGRATION_FAIL_AFTER");
    int rc = CBM_STORE_OK;
    for (size_t i = 0; i < sizeof(STAGE9_OBJECTS) / sizeof(STAGE9_OBJECTS[0]); i++) {
        if (sqlite3_exec(db, STAGE9_OBJECTS[i].sql, NULL, NULL, NULL) != SQLITE_OK ||
            (fail_after > 0 && (int)i + 1 == fail_after)) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    char timestamp[40];
    stage9_iso_from_ms((int64_t)time(NULL) * INT64_C(1000), timestamp);
    if (rc == CBM_STORE_OK) {
        sqlite3_stmt *stmt = NULL;
        const char *sql =
            "INSERT INTO stage9_component_ledger(component,version,name,checksum,policy_sha256,"
            "applied_at) VALUES(?1,1,?2,?3,?4,?5);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
        } else {
            sqlite3_bind_text(stmt, 1, STAGE9_COMPONENT, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, STAGE9_COMPONENT_NAME, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, CBM_STAGE9_MIGRATION_SHA256, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, CBM_STAGE9_POLICY_SHA256, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 5, timestamp, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE)
                rc = CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
    }
    if (rc == CBM_STORE_OK &&
        cbm_store_memory_stage9_object_count(store) ==
            (int)(sizeof(STAGE9_OBJECTS) / sizeof(STAGE9_OBJECTS[0])) &&
        stage9_ledger_valid(db)) {
        rc = cbm_store_commit(store);
    } else {
        rc = CBM_STORE_ERR;
    }
    if (rc != CBM_STORE_OK)
        cbm_store_rollback(store);
    return rc;
}

static int stage9_load_edges(sqlite3 *db, stage9_edge_t **out_edges, int *out_count) {
    *out_edges = NULL;
    *out_count = 0;
    bool has_stage8 = stage9_table_exists(db, "plastic_edge_state");
    bool has_stage9 = stage9_table_exists(db, "edge_lifecycle_state");
    const char *pcols = has_stage8
                            ? "COALESCE(p.pheromone_ppm,1000000),COALESCE(p.success_count,0),"
                              "COALESCE(p.failure_count,0),MAX(e.created_at,COALESCE(CAST(strftime("
                              "'%s',p.rebuilt_at)*1000 AS INTEGER),e.created_at))"
                            : "1000000,0,0,e.created_at";
    const char *pjoin = has_stage8 ? " LEFT JOIN plastic_edge_state p ON p.edge_id=e.id " : " ";
    const char *lcols = has_stage9
                            ? "COALESCE(l.lifecycle_state,'active'),COALESCE(l.state_changed_at_ms,"
                              "e.created_at),COALESCE(l.protection_reason,'normal')"
                            : "'active',e.created_at,'normal'";
    const char *ljoin = has_stage9 ? " LEFT JOIN edge_lifecycle_state l ON l.edge_id=e.id " : " ";
    char sql[4096];
    snprintf(sql, sizeof(sql),
             "SELECT e.id,e.type,e.created_at,COALESCE(e.confidence,0.0),src.kind,dst.kind,"
             "src.scope_project,dst.scope_project,COALESCE(src.importance,0.0),COALESCE(dst."
             "importance,0.0),COALESCE(src.reusability,0.0),COALESCE(dst.reusability,0.0),%s,%s "
             "FROM memory_edge e LEFT JOIN memory_item src ON src.id=e.src_id LEFT JOIN memory_item"
             " dst ON dst.id=e.dst_id%s%sORDER BY e.id;",
             pcols, lcols, pjoin, ljoin);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int capacity = 16;
    int count = 0;
    stage9_edge_t *edges = calloc((size_t)capacity, sizeof(*edges));
    if (!edges) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count == capacity) {
            capacity *= 2;
            stage9_edge_t *next = realloc(edges, (size_t)capacity * sizeof(*edges));
            if (!next) {
                rc = CBM_STORE_ERR;
                break;
            }
            edges = next;
            memset(edges + count, 0, (size_t)(capacity - count) * sizeof(*edges));
        }
        stage9_edge_t *edge = &edges[count];
        edge->edge_id = stage9_dup(stage9_col_text(stmt, 0));
        edge->relation_type = stage9_dup(stage9_col_text(stmt, 1));
        edge->created_at_ms = sqlite3_column_int64(stmt, 2);
        edge->confidence_ppm = (int64_t)(sqlite3_column_double(stmt, 3) * 1000000.0 + 0.5);
        edge->src_kind = stage9_dup(stage9_col_text(stmt, 4));
        edge->dst_kind = stage9_dup(stage9_col_text(stmt, 5));
        edge->src_project = stage9_dup(stage9_col_text(stmt, 6));
        edge->dst_project = stage9_dup(stage9_col_text(stmt, 7));
        edge->src_importance_ppm = (int64_t)(sqlite3_column_double(stmt, 8) * 1000000.0 + 0.5);
        edge->dst_importance_ppm = (int64_t)(sqlite3_column_double(stmt, 9) * 1000000.0 + 0.5);
        edge->src_reusability_ppm = (int64_t)(sqlite3_column_double(stmt, 10) * 1000000.0 + 0.5);
        edge->dst_reusability_ppm = (int64_t)(sqlite3_column_double(stmt, 11) * 1000000.0 + 0.5);
        edge->pheromone_ppm = sqlite3_column_int64(stmt, 12);
        edge->success_count = sqlite3_column_int(stmt, 13);
        edge->failure_count = sqlite3_column_int(stmt, 14);
        edge->last_signal_ms = sqlite3_column_int64(stmt, 15);
        edge->state = stage9_dup(stage9_col_text(stmt, 16));
        edge->state_changed_at_ms = sqlite3_column_int64(stmt, 17);
        edge->protection_reason = stage9_dup(stage9_col_text(stmt, 18));
        if (!edge->edge_id || !edge->relation_type || !edge->src_kind || !edge->dst_kind ||
            !edge->src_project || !edge->dst_project || !edge->state || !edge->protection_reason) {
            rc = CBM_STORE_ERR;
            break;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK) {
        stage9_edges_free(edges, count + 1);
        return rc;
    }
    qsort(edges, (size_t)count, sizeof(*edges), stage9_edge_compare);
    *out_edges = edges;
    *out_count = count;
    return CBM_STORE_OK;
}

static int stage9_decision_hash(stage9_decision_t *decision) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_sint(doc, root, "age_days", decision->age_days);
    yyjson_mut_obj_add_sint(doc, root, "cold_dwell_days", decision->cold_dwell_days);
    yyjson_mut_obj_add_sint(doc, root, "conductance_ppm", decision->conductance_ppm);
    yyjson_mut_obj_add_str(doc, root, "edge_id", decision->edge_id);
    yyjson_mut_obj_add_sint(doc, root, "effective_success_flow_ppm", decision->success_flow_ppm);
    yyjson_mut_obj_add_int(doc, root, "failure_count", decision->failure_count);
    yyjson_mut_obj_add_str(doc, root, "from_state", decision->from_state);
    yyjson_mut_obj_add_bool(doc, root, "protected", decision->protected_edge);
    yyjson_mut_obj_add_str(doc, root, "reason_code", decision->reason_code);
    yyjson_mut_obj_add_str(doc, root, "relation_type", decision->relation_type);
    yyjson_mut_obj_add_int(doc, root, "success_count", decision->success_count);
    yyjson_mut_obj_add_str(doc, root, "to_state", decision->to_state);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = stage9_hash_text(json, decision->decision_sha256);
    free(json);
    return rc;
}

static int stage9_decide(const stage9_edge_t *edge, const char *project, int64_t as_of_ms,
                         stage9_decision_t *decision) {
    memset(decision, 0, sizeof(*decision));
    decision->edge_id = stage9_dup(edge->edge_id);
    decision->relation_type = stage9_dup(edge->relation_type);
    decision->from_state = stage9_dup(edge->state);
    decision->to_state = stage9_dup(edge->state);
    decision->success_count = edge->success_count;
    decision->failure_count = edge->failure_count;
    decision->age_days =
        as_of_ms > edge->last_signal_ms ? (as_of_ms - edge->last_signal_ms) / STAGE9_DAY_MS : 0;
    decision->cold_dwell_days =
        strcmp(edge->state, "cold") == 0 && as_of_ms > edge->state_changed_at_ms
            ? (as_of_ms - edge->state_changed_at_ms) / STAGE9_DAY_MS
            : 0;
    const stage9_relation_policy_t *policy = stage9_policy(edge->relation_type);
    int64_t pheromone_score = stage9_clamp(edge->pheromone_ppm, 0, 2000000) / 2;
    decision->success_flow_ppm =
        (int64_t)edge->success_count * STAGE9_PPM / (edge->success_count + edge->failure_count + 1);
    if (policy && !policy->protected_relation) {
        int64_t base = (pheromone_score * 500000 + decision->success_flow_ppm * 300000 +
                        stage9_clamp(edge->confidence_ppm, 0, STAGE9_PPM) * 200000) /
                       STAGE9_PPM;
        int64_t stale_days =
            decision->age_days > policy->grace_days ? decision->age_days - policy->grace_days : 0;
        int64_t decay = stage9_clamp(stale_days * policy->decay_rate, 0, policy->max_decay);
        decision->conductance_ppm = base * (STAGE9_PPM - decay) / STAGE9_PPM;
    }
    const char *reason = NULL;
    if (!project || strcmp(edge->src_project, project) != 0 ||
        strcmp(edge->dst_project, project) != 0) {
        reason = "CROSS_SCOPE_BLOCKED";
        decision->protected_edge = true;
    } else if (strcmp(edge->state, "disabled") == 0) {
        reason = "DISABLED_MANUAL_BLOCK";
        decision->protected_edge = true;
    } else if (!policy || policy->protected_relation) {
        reason = policy ? "PROTECTED_RELATION_TYPE" : "UNKNOWN_RELATION_PROTECTED";
        decision->protected_edge = true;
    } else if (strcmp(edge->src_kind, "constraint") == 0 ||
               strcmp(edge->dst_kind, "constraint") == 0) {
        reason = "PROTECTED_ENDPOINT_CONSTRAINT";
        decision->protected_edge = true;
    } else if (edge->src_importance_ppm >= 900000 || edge->dst_importance_ppm >= 900000 ||
               edge->src_reusability_ppm >= 900000 || edge->dst_reusability_ppm >= 900000) {
        reason = "PROTECTED_HIGH_VALUE_ENDPOINT";
        decision->protected_edge = true;
    } else if (strcmp(edge->state, "archived") == 0) {
        reason = "KEEP_ARCHIVED_REQUIRES_RESTORE";
    } else if (strcmp(edge->state, "active") == 0) {
        if (decision->age_days < policy->cold_age)
            reason = "KEEP_RECENT";
        else if (edge->failure_count < policy->cold_failures)
            reason = "KEEP_INSUFFICIENT_FAILURE_EVIDENCE";
        else if (edge->failure_count <= edge->success_count)
            reason = "KEEP_SUCCESS_NOT_FAILURE_DOMINANT";
        else if (decision->conductance_ppm > policy->cold_conductance)
            reason = "KEEP_CONDUCTANCE_ABOVE_COLD_THRESHOLD";
        else {
            free(decision->to_state);
            decision->to_state = stage9_dup("cold");
            reason = "TRANSITION_ACTIVE_TO_COLD";
        }
    } else if (strcmp(edge->state, "cold") == 0) {
        if (decision->cold_dwell_days < policy->archive_dwell)
            reason = "KEEP_COLD_DWELL";
        else if (edge->failure_count < policy->archive_failures)
            reason = "KEEP_INSUFFICIENT_ARCHIVE_EVIDENCE";
        else if (edge->failure_count <= edge->success_count)
            reason = "KEEP_SUCCESS_NOT_FAILURE_DOMINANT";
        else if (decision->conductance_ppm > policy->archive_conductance)
            reason = "KEEP_CONDUCTANCE_ABOVE_ARCHIVE_THRESHOLD";
        else {
            free(decision->to_state);
            decision->to_state = stage9_dup("archived");
            reason = "TRANSITION_COLD_TO_ARCHIVED";
        }
    } else {
        reason = "INVALID_STATE_FAIL_CLOSED";
        decision->protected_edge = true;
    }
    decision->reason_code = stage9_dup(reason);
    if (!decision->edge_id || !decision->relation_type || !decision->from_state ||
        !decision->to_state || !decision->reason_code) {
        stage9_decision_clear(decision);
        return CBM_STORE_ERR;
    }
    return stage9_decision_hash(decision);
}

static yyjson_mut_val *stage9_decision_json(yyjson_mut_doc *doc,
                                            const stage9_decision_t *decision) {
    yyjson_mut_val *item = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_sint(doc, item, "age_days", decision->age_days);
    yyjson_mut_obj_add_sint(doc, item, "cold_dwell_days", decision->cold_dwell_days);
    yyjson_mut_obj_add_sint(doc, item, "conductance_ppm", decision->conductance_ppm);
    yyjson_mut_obj_add_str(doc, item, "decision_sha256", decision->decision_sha256);
    yyjson_mut_obj_add_str(doc, item, "edge_id", decision->edge_id);
    yyjson_mut_obj_add_sint(doc, item, "effective_success_flow_ppm", decision->success_flow_ppm);
    yyjson_mut_obj_add_int(doc, item, "failure_count", decision->failure_count);
    yyjson_mut_obj_add_str(doc, item, "from_state", decision->from_state);
    yyjson_mut_obj_add_bool(doc, item, "protected", decision->protected_edge);
    yyjson_mut_obj_add_str(doc, item, "reason_code", decision->reason_code);
    yyjson_mut_obj_add_str(doc, item, "relation_type", decision->relation_type);
    yyjson_mut_obj_add_int(doc, item, "success_count", decision->success_count);
    yyjson_mut_obj_add_str(doc, item, "to_state", decision->to_state);
    return item;
}

static int stage9_decision_set_hash(stage9_decision_t *decisions, int count, char out[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *array = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !array) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, array);
    for (int i = 0; i < count; i++) {
        yyjson_mut_arr_add_val(array, stage9_decision_json(doc, &decisions[i]));
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = stage9_hash_text(json, out);
    free(json);
    return rc;
}

static int stage9_report_hash(const cbm_edge_lifecycle_input_t *input, const char *operation,
                              const char *decision_hash, int decision_count, int transition_count,
                              int protected_count, char out[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_sint(doc, root, "as_of_ms", input->as_of_ms);
    yyjson_mut_obj_add_int(doc, root, "automatic_permanent_delete_count", 0);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    yyjson_mut_obj_add_int(doc, root, "decision_count", decision_count);
    yyjson_mut_obj_add_str(doc, root, "decision_set_sha256", decision_hash);
    yyjson_mut_obj_add_str(doc, root, "mode", input->mode);
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    yyjson_mut_obj_add_str(doc, root, "policy_sha256", input->policy_sha256);
    yyjson_mut_obj_add_int(doc, root, "policy_version", input->policy_version);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_int(doc, root, "protected_count", protected_count);
    yyjson_mut_obj_add_int(doc, root, "transition_count", transition_count);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = stage9_hash_text(json, out);
    free(json);
    return rc;
}

static char *stage9_report_json(const cbm_edge_lifecycle_input_t *input, const char *operation,
                                stage9_decision_t *decisions, int count, const char *decision_hash,
                                const char *report_hash, bool wrote, bool replayed) {
    int transitions = 0;
    int protected_count = 0;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *array = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !array) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < count; i++) {
        if (strcmp(decisions[i].from_state, decisions[i].to_state) != 0)
            transitions++;
        if (decisions[i].protected_edge)
            protected_count++;
        yyjson_mut_arr_add_val(array, stage9_decision_json(doc, &decisions[i]));
    }
    yyjson_mut_obj_add_str(doc, root, "schema", "stage9-edge-maintenance-report/v1");
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    yyjson_mut_obj_add_str(doc, root, "mode", input->mode);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "run_id", input->run_id ? input->run_id : "");
    yyjson_mut_obj_add_sint(doc, root, "as_of_ms", input->as_of_ms);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_int(doc, root, "policy_version", input->policy_version);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    yyjson_mut_obj_add_str(doc, root, "policy_sha256", input->policy_sha256);
    yyjson_mut_obj_add_int(doc, root, "decision_count", count);
    yyjson_mut_obj_add_int(doc, root, "transition_count", transitions);
    yyjson_mut_obj_add_int(doc, root, "protected_count", protected_count);
    yyjson_mut_obj_add_int(doc, root, "automatic_permanent_delete_count", 0);
    yyjson_mut_obj_add_str(doc, root, "decision_set_sha256", decision_hash);
    yyjson_mut_obj_add_str(doc, root, "report_sha256", report_hash);
    yyjson_mut_obj_add_bool(doc, root, "long_term_state_written", wrote);
    yyjson_mut_obj_add_bool(doc, root, "production_state_written",
                            wrote && strcmp(input->project, STAGE9_PRODUCTION_PROJECT) == 0);
    yyjson_mut_obj_add_bool(doc, root, "replayed", replayed);
    yyjson_mut_obj_add_val(doc, root, "decisions", array);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int stage9_input_valid(const cbm_edge_lifecycle_input_t *input) {
    return input && input->project && input->mode && input->algorithm_version &&
           input->policy_sha256 && input->as_of_ms >= 0 &&
           strcmp(input->algorithm_version, CBM_STAGE9_ALGORITHM_VERSION) == 0 &&
           strcmp(input->policy_sha256, CBM_STAGE9_POLICY_SHA256) == 0 &&
           input->policy_version == CBM_STAGE9_POLICY_VERSION &&
           input->config_version == CBM_STAGE9_CONFIG_VERSION &&
           (strcmp(input->mode, "off") == 0 || strcmp(input->mode, "shadow") == 0 ||
            strcmp(input->mode, "dry_run") == 0 || strcmp(input->mode, "active") == 0);
}

static char *stage9_read_file(const char *path, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    if (!path || !path[0])
        return NULL;
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size) {
        free(buffer);
        return NULL;
    }
    buffer[read] = '\0';
    if (out_size)
        *out_size = read;
    return buffer;
}

static bool stage9_manifest_verify(const cbm_edge_lifecycle_input_t *input, const char *operation,
                                   stage9_decision_t *decisions, int count,
                                   const char *decision_hash, bool stage14_parent_authorized) {
    char path[1024] = {0};
    char expected_hash[80] = {0};
    bool stage14_fixture =
        strncmp(input->project, STAGE14_FIXTURE_PREFIX, strlen(STAGE14_FIXTURE_PREFIX)) == 0;
    bool fixture = strcmp(input->project, STAGE9_FIXTURE_PROJECT) == 0 || stage14_fixture;
    if (stage14_parent_authorized) {
        if (!input->manifest_path || !input->manifest_sha256)
            return false;
        snprintf(path, sizeof(path), "%s", input->manifest_path);
        snprintf(expected_hash, sizeof(expected_hash), "%s", input->manifest_sha256);
    } else if (fixture) {
        const char *guard =
            stage14_fixture ? "CBM_STAGE14_ACTIVE_FIXTURE" : "CBM_STAGE9_ACTIVE_FIXTURE";
        if (stage9_env_int(guard) != 1 || !input->manifest_path || !input->manifest_sha256) {
            return false;
        }
        snprintf(path, sizeof(path), "%s", input->manifest_path);
        snprintf(expected_hash, sizeof(expected_hash), "%s", input->manifest_sha256);
    } else if (strcmp(input->project, STAGE9_PRODUCTION_PROJECT) == 0) {
        if (stage9_env_int("CBM_STAGE9_PRODUCTION_CANARY") != 1)
            return false;
        cbm_safe_getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST", path, sizeof(path), NULL);
        cbm_safe_getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST_SHA256", expected_hash,
                        sizeof(expected_hash), NULL);
    } else {
        return false;
    }
    size_t size = 0;
    char *payload = stage9_read_file(path, &size);
    if (!payload)
        return false;
    char actual_hash[65];
    bool ok = cbm_stage7_sha256_hex(payload, size, actual_hash) == CBM_STORE_OK &&
              strcmp(actual_hash, expected_hash) == 0;
    yyjson_doc *doc = ok ? yyjson_read(payload, size, 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *edges = root ? yyjson_obj_get(root, "edge_ids") : NULL;
    yyjson_val *schema = root ? yyjson_obj_get(root, "schema") : NULL;
    yyjson_val *project = root ? yyjson_obj_get(root, "project") : NULL;
    yyjson_val *run_id = root ? yyjson_obj_get(root, "run_id") : NULL;
    yyjson_val *as_of = root ? yyjson_obj_get(root, "as_of_ms") : NULL;
    yyjson_val *algorithm = root ? yyjson_obj_get(root, "algorithm_version") : NULL;
    yyjson_val *policy_hash = root ? yyjson_obj_get(root, "policy_sha256") : NULL;
    yyjson_val *policy_version = root ? yyjson_obj_get(root, "policy_version") : NULL;
    yyjson_val *config_version = root ? yyjson_obj_get(root, "config_version") : NULL;
    yyjson_val *decision_set = root ? yyjson_obj_get(root, "decision_set_sha256") : NULL;
    yyjson_val *manifest_operation = root ? yyjson_obj_get(root, "operation") : NULL;
    ok = ok && root && yyjson_is_obj(root) && schema && yyjson_is_str(schema) &&
         strcmp(yyjson_get_str(schema), STAGE9_MANIFEST_SCHEMA) == 0 && project &&
         yyjson_is_str(project) && strcmp(yyjson_get_str(project), input->project) == 0 && run_id &&
         yyjson_is_str(run_id) && input->run_id &&
         strcmp(yyjson_get_str(run_id), input->run_id) == 0 && as_of && yyjson_is_int(as_of) &&
         yyjson_get_sint(as_of) == input->as_of_ms && algorithm && yyjson_is_str(algorithm) &&
         strcmp(yyjson_get_str(algorithm), input->algorithm_version) == 0 && policy_hash &&
         yyjson_is_str(policy_hash) &&
         strcmp(yyjson_get_str(policy_hash), input->policy_sha256) == 0 && policy_version &&
         yyjson_is_int(policy_version) &&
         yyjson_get_sint(policy_version) == input->policy_version && config_version &&
         yyjson_is_int(config_version) &&
         yyjson_get_sint(config_version) == input->config_version && decision_set &&
         yyjson_is_str(decision_set) && strcmp(yyjson_get_str(decision_set), decision_hash) == 0 &&
         manifest_operation && yyjson_is_str(manifest_operation) &&
         strcmp(yyjson_get_str(manifest_operation), operation) == 0 && edges &&
         yyjson_is_arr(edges) && yyjson_arr_size(edges) == (size_t)count;
    if (ok) {
        yyjson_arr_iter iter = yyjson_arr_iter_with(edges);
        yyjson_val *value = NULL;
        int index = 0;
        while ((value = yyjson_arr_iter_next(&iter))) {
            if (!yyjson_is_str(value) || index >= count ||
                strcmp(yyjson_get_str(value), decisions[index].edge_id) != 0) {
                ok = false;
                break;
            }
            index++;
        }
    }
    yyjson_doc_free(doc);
    free(payload);
    return ok;
}

static int stage9_request_hash(const cbm_edge_lifecycle_input_t *input, const char *operation,
                               stage9_decision_t *decisions, int count, const char *decision_hash,
                               bool stage14_parent_authorized, char out[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *edges = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !edges) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < count; i++)
        yyjson_mut_arr_add_strcpy(doc, edges, decisions[i].edge_id);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_sint(doc, root, "as_of_ms", input->as_of_ms);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    yyjson_mut_obj_add_str(doc, root, "decision_set_sha256", decision_hash);
    yyjson_mut_obj_add_val(doc, root, "edge_ids", edges);
    char production_manifest_sha256[80] = {0};
    const char *manifest_sha256 = input->manifest_sha256 ? input->manifest_sha256 : "";
    if (strcmp(input->project, STAGE9_PRODUCTION_PROJECT) == 0 && !stage14_parent_authorized) {
        cbm_safe_getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST_SHA256", production_manifest_sha256,
                        sizeof(production_manifest_sha256), NULL);
        manifest_sha256 = production_manifest_sha256;
    }
    yyjson_mut_obj_add_str(doc, root, "manifest_sha256", manifest_sha256);
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    yyjson_mut_obj_add_str(doc, root, "policy_sha256", input->policy_sha256);
    yyjson_mut_obj_add_int(doc, root, "policy_version", input->policy_version);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "run_id", input->run_id);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = stage9_hash_text(json, out);
    free(json);
    return rc;
}

static int stage9_existing_run(sqlite3 *db, const char *run_id, const char *request_hash) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT canonical_request_sha256 FROM edge_maintenance_run WHERE run_id=?1;";
    int rc = CBM_STORE_NOT_FOUND;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, run_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *stored = stage9_col_text(stmt, 0);
            rc = strcmp(stored, request_hash) == 0 ? CBM_STORE_REPLAYED
                                                   : CBM_STORE_IDEMPOTENCY_CONFLICT;
        }
    } else {
        rc = CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    return rc;
}

static int stage9_load_existing_run(sqlite3 *db, const char *run_id,
                                    stage9_decision_t **out_decisions, int *out_count,
                                    char out_request_hash[65], char out_report_hash[65]) {
    *out_decisions = NULL;
    *out_count = 0;
    sqlite3_stmt *run_stmt = NULL;
    const char *run_sql =
        "SELECT canonical_request_sha256,report_sha256,decision_count FROM edge_maintenance_run "
        "WHERE run_id=?1;";
    if (sqlite3_prepare_v2(db, run_sql, -1, &run_stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(run_stmt, 1, run_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(run_stmt) != SQLITE_ROW) {
        sqlite3_finalize(run_stmt);
        return CBM_STORE_NOT_FOUND;
    }
    snprintf(out_request_hash, 65, "%s", stage9_col_text(run_stmt, 0));
    snprintf(out_report_hash, 65, "%s", stage9_col_text(run_stmt, 1));
    int expected_count = sqlite3_column_int(run_stmt, 2);
    sqlite3_finalize(run_stmt);

    stage9_decision_t *decisions =
        calloc((size_t)(expected_count > 0 ? expected_count : 1), sizeof(*decisions));
    if (!decisions)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT edge_id,relation_type,from_state,to_state,reason_code,protected,conductance_ppm,"
        "effective_success_flow_ppm,age_days,cold_dwell_days,success_count,failure_count,"
        "decision_sha256 FROM edge_maintenance_decision WHERE run_id=?1 ORDER BY edge_id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(decisions);
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, run_id, -1, SQLITE_TRANSIENT);
    int count = 0;
    int rc = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= expected_count) {
            rc = CBM_STORE_IDEMPOTENCY_CONFLICT;
            break;
        }
        stage9_decision_t *decision = &decisions[count++];
        decision->edge_id = stage9_dup(stage9_col_text(stmt, 0));
        decision->relation_type = stage9_dup(stage9_col_text(stmt, 1));
        decision->from_state = stage9_dup(stage9_col_text(stmt, 2));
        decision->to_state = stage9_dup(stage9_col_text(stmt, 3));
        decision->reason_code = stage9_dup(stage9_col_text(stmt, 4));
        decision->protected_edge = sqlite3_column_int(stmt, 5) != 0;
        decision->conductance_ppm = sqlite3_column_int64(stmt, 6);
        decision->success_flow_ppm = sqlite3_column_int64(stmt, 7);
        decision->age_days = sqlite3_column_int64(stmt, 8);
        decision->cold_dwell_days = sqlite3_column_int64(stmt, 9);
        decision->success_count = sqlite3_column_int(stmt, 10);
        decision->failure_count = sqlite3_column_int(stmt, 11);
        snprintf(decision->decision_sha256, sizeof(decision->decision_sha256), "%s",
                 stage9_col_text(stmt, 12));
        if (!decision->edge_id || !decision->relation_type || !decision->from_state ||
            !decision->to_state || !decision->reason_code) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK && count != expected_count)
        rc = CBM_STORE_IDEMPOTENCY_CONFLICT;
    if (rc != CBM_STORE_OK) {
        stage9_decisions_free(decisions, count);
        return rc;
    }
    *out_decisions = decisions;
    *out_count = count;
    return CBM_STORE_OK;
}

static int stage9_state_hash(const stage9_decision_t *decision,
                             const cbm_edge_lifecycle_input_t *input, int64_t state_changed_at_ms,
                             int version, const char *protection_reason, char out[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    yyjson_mut_obj_add_sint(doc, root, "conductance_ppm", decision->conductance_ppm);
    yyjson_mut_obj_add_str(doc, root, "edge_id", decision->edge_id);
    yyjson_mut_obj_add_sint(doc, root, "effective_success_flow_ppm", decision->success_flow_ppm);
    yyjson_mut_obj_add_sint(doc, root, "last_evaluated_as_of_ms", input->as_of_ms);
    yyjson_mut_obj_add_str(doc, root, "policy_sha256", input->policy_sha256);
    yyjson_mut_obj_add_int(doc, root, "policy_version", input->policy_version);
    yyjson_mut_obj_add_str(doc, root, "protection_reason", protection_reason);
    yyjson_mut_obj_add_str(doc, root, "state", decision->to_state);
    yyjson_mut_obj_add_sint(doc, root, "state_changed_at_ms", state_changed_at_ms);
    yyjson_mut_obj_add_int(doc, root, "version", version);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = stage9_hash_text(json, out);
    free(json);
    return rc;
}

static int stage9_event_hash(const char *event_id, const char *run_id,
                             const stage9_decision_t *decision, const char *operation,
                             const char *before_hash, const char *after_hash,
                             const cbm_edge_lifecycle_input_t *input, const char *prev_hash,
                             const char *created_at, char out[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "after_state_sha256", after_hash);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_str(doc, root, "before_state_sha256", before_hash);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    yyjson_mut_obj_add_str(doc, root, "created_at", created_at);
    yyjson_mut_obj_add_str(doc, root, "decision_sha256", decision->decision_sha256);
    yyjson_mut_obj_add_str(doc, root, "edge_id", decision->edge_id);
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id);
    yyjson_mut_obj_add_str(doc, root, "from_state", decision->from_state);
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    yyjson_mut_obj_add_int(doc, root, "policy_version", input->policy_version);
    yyjson_mut_obj_add_str(doc, root, "prev_hash", prev_hash);
    yyjson_mut_obj_add_str(doc, root, "run_id", run_id);
    yyjson_mut_obj_add_str(doc, root, "to_state", decision->to_state);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = stage9_hash_text(json, out);
    free(json);
    return rc;
}

static int stage9_insert_run(sqlite3 *db, const cbm_edge_lifecycle_input_t *input,
                             const char *operation, const char *request_hash,
                             const char *decision_hash, const char *report_hash, int decision_count,
                             int transition_count, const char *created_at) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO edge_maintenance_run(run_id,canonical_request_sha256,operation,mode,project,"
        "as_of_ms,algorithm_version,policy_version,config_version,policy_sha256,decision_set_"
        "sha256,"
        "report_sha256,decision_count,transition_count,created_at) VALUES(?1,?2,?3,'active',?4,?5,"
        "?6,?7,?8,?9,?10,?11,?12,?13,?14);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->run_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, request_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, operation, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, input->project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, input->as_of_ms);
    sqlite3_bind_text(stmt, 6, input->algorithm_version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, input->policy_version);
    sqlite3_bind_int(stmt, 8, input->config_version);
    sqlite3_bind_text(stmt, 9, input->policy_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, decision_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 11, report_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 12, decision_count);
    sqlite3_bind_int(stmt, 13, transition_count);
    sqlite3_bind_text(stmt, 14, created_at, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int stage9_persist(cbm_store_t *store, const cbm_edge_lifecycle_input_t *input,
                          const char *operation, stage9_decision_t *decisions, int count,
                          const char *decision_hash, const char *report_hash, bool owns_transaction,
                          bool stage14_parent_authorized, int *out_recorded, int *out_replayed) {
    sqlite3 *db = cbm_store_get_db(store);
    char request_hash[65];
    if (!input->run_id || !input->run_id[0] ||
        stage9_request_hash(input, operation, decisions, count, decision_hash,
                            stage14_parent_authorized, request_hash) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    int existing = stage9_existing_run(db, input->run_id, request_hash);
    if (existing == CBM_STORE_REPLAYED) {
        *out_replayed = 1;
        return CBM_STORE_OK;
    }
    if (existing != CBM_STORE_NOT_FOUND)
        return existing;
    if (owns_transaction && cbm_store_begin(store) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char created_at[40];
    stage9_iso_from_ms(input->as_of_ms, created_at);
    int transitions = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(decisions[i].from_state, decisions[i].to_state) != 0)
            transitions++;
    }
    int rc = stage9_insert_run(db, input, operation, request_hash, decision_hash, report_hash,
                               count, transitions, created_at);
    int fail_after = stage9_env_int("CBM_STAGE9_ACTIVE_FAIL_AFTER");
    int step = rc == CBM_STORE_OK ? 1 : 0;
    if (rc == CBM_STORE_OK && fail_after == step)
        rc = CBM_STORE_ERR;
    for (int i = 0; i < count && rc == CBM_STORE_OK; i++) {
        stage9_decision_t *decision = &decisions[i];
        char *decision_id = stage9_hash_id("decision-", input->run_id, decision->edge_id, "");
        sqlite3_stmt *stmt = NULL;
        const char *decision_sql =
            "INSERT INTO edge_maintenance_decision(decision_id,run_id,edge_id,relation_type,from_"
            "state,to_state,reason_code,protected,conductance_ppm,effective_success_flow_ppm,age_"
            "days,cold_dwell_days,success_count,failure_count,decision_sha256,created_at) VALUES("
            "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);";
        if (!decision_id || sqlite3_prepare_v2(db, decision_sql, -1, &stmt, NULL) != SQLITE_OK) {
            free(decision_id);
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(stmt, 1, decision_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, input->run_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, decision->edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, decision->relation_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, decision->from_state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, decision->to_state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, decision->reason_code, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, decision->protected_edge ? 1 : 0);
        sqlite3_bind_int64(stmt, 9, decision->conductance_ppm);
        sqlite3_bind_int64(stmt, 10, decision->success_flow_ppm);
        sqlite3_bind_int64(stmt, 11, decision->age_days);
        sqlite3_bind_int64(stmt, 12, decision->cold_dwell_days);
        sqlite3_bind_int(stmt, 13, decision->success_count);
        sqlite3_bind_int(stmt, 14, decision->failure_count);
        sqlite3_bind_text(stmt, 15, decision->decision_sha256, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 16, created_at, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE)
            rc = CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        free(decision_id);
        step++;
        if (rc == CBM_STORE_OK && fail_after == step)
            rc = CBM_STORE_ERR;
        if (rc != CBM_STORE_OK)
            break;

        const char *state_sql =
            "SELECT lifecycle_state,state_sha256,version,state_changed_at_ms FROM edge_lifecycle_"
            "state WHERE edge_id=?1;";
        sqlite3_stmt *state_stmt = NULL;
        bool has_state = false;
        char before_hash[65];
        int version = 1;
        int64_t changed_at = input->as_of_ms;
        if (sqlite3_prepare_v2(db, state_sql, -1, &state_stmt, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(state_stmt, 1, decision->edge_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(state_stmt) == SQLITE_ROW) {
            has_state = true;
            snprintf(before_hash, sizeof(before_hash), "%s", stage9_col_text(state_stmt, 1));
            version = sqlite3_column_int(state_stmt, 2) + 1;
            changed_at = sqlite3_column_int64(state_stmt, 3);
        } else {
            char seed[1024];
            snprintf(seed, sizeof(seed), "{\"edge_id\":\"%s\",\"state\":\"%s\",\"version\":0}",
                     decision->edge_id, decision->from_state);
            stage9_hash_text(seed, before_hash);
        }
        sqlite3_finalize(state_stmt);
        bool transition = strcmp(decision->from_state, decision->to_state) != 0;
        if (has_state && !transition)
            continue;
        if (transition)
            changed_at = input->as_of_ms;
        const char *protection = decision->protected_edge ? decision->reason_code : "normal";
        char after_hash[65];
        if (stage9_state_hash(decision, input, changed_at, version, protection, after_hash) !=
            CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_stmt *prev_stmt = NULL;
        char prev_hash[65] = "GENESIS";
        if (sqlite3_prepare_v2(db,
                               "SELECT event_hash FROM edge_lifecycle_audit_event ORDER BY "
                               "sequence_no DESC LIMIT 1;",
                               -1, &prev_stmt, NULL) == SQLITE_OK &&
            sqlite3_step(prev_stmt) == SQLITE_ROW) {
            snprintf(prev_hash, sizeof(prev_hash), "%s", stage9_col_text(prev_stmt, 0));
        }
        sqlite3_finalize(prev_stmt);
        const char *audit_operation =
            strcmp(operation, "restore") == 0
                ? "restore"
                : (!has_state && !transition ? "initialize" : "transition");
        char *event_id =
            stage9_hash_id("lifecycle-", input->run_id, decision->edge_id, audit_operation);
        char event_hash[65];
        if (!event_id || stage9_event_hash(event_id, input->run_id, decision, audit_operation,
                                           before_hash, after_hash, input, prev_hash, created_at,
                                           event_hash) != CBM_STORE_OK) {
            free(event_id);
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_stmt *audit_stmt = NULL;
        const char *audit_sql =
            "INSERT INTO edge_lifecycle_audit_event(event_id,run_id,edge_id,operation,from_state,"
            "to_state,before_state_sha256,after_state_sha256,decision_sha256,algorithm_version,"
            "policy_version,config_version,prev_hash,event_hash,created_at) VALUES(?1,?2,?3,?4,"
            "?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);";
        if (sqlite3_prepare_v2(db, audit_sql, -1, &audit_stmt, NULL) != SQLITE_OK) {
            free(event_id);
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(audit_stmt, 1, event_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 2, input->run_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 3, decision->edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 4, audit_operation, -1, SQLITE_STATIC);
        sqlite3_bind_text(audit_stmt, 5, decision->from_state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 6, decision->to_state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 7, before_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 8, after_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 9, decision->decision_sha256, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 10, input->algorithm_version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(audit_stmt, 11, input->policy_version);
        sqlite3_bind_int(audit_stmt, 12, input->config_version);
        sqlite3_bind_text(audit_stmt, 13, prev_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 14, event_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(audit_stmt, 15, created_at, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(audit_stmt) != SQLITE_DONE)
            rc = CBM_STORE_ERR;
        sqlite3_finalize(audit_stmt);
        step++;
        if (rc == CBM_STORE_OK && fail_after == step)
            rc = CBM_STORE_ERR;
        if (rc != CBM_STORE_OK) {
            free(event_id);
            break;
        }
        sqlite3_stmt *write_state = NULL;
        const char *insert_sql =
            "INSERT INTO edge_lifecycle_state(edge_id,lifecycle_state,conductance_ppm,effective_"
            "success_flow_ppm,last_evaluated_as_of_ms,state_changed_at_ms,protection_reason,"
            "version,"
            "last_audit_event_id,algorithm_version,policy_version,config_version,policy_sha256,"
            "state_"
            "sha256,updated_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15);";
        const char *update_sql =
            "UPDATE edge_lifecycle_state SET lifecycle_state=?2,conductance_ppm=?3,effective_"
            "success_flow_ppm=?4,last_evaluated_as_of_ms=?5,state_changed_at_ms=?6,protection_"
            "reason"
            "=?7,version=?8,last_audit_event_id=?9,algorithm_version=?10,policy_version=?11,config_"
            "version=?12,policy_sha256=?13,state_sha256=?14,updated_at=?15 WHERE edge_id=?1;";
        if (sqlite3_prepare_v2(db, has_state ? update_sql : insert_sql, -1, &write_state, NULL) !=
            SQLITE_OK) {
            free(event_id);
            rc = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(write_state, 1, decision->edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(write_state, 2, decision->to_state, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(write_state, 3, decision->conductance_ppm);
        sqlite3_bind_int64(write_state, 4, decision->success_flow_ppm);
        sqlite3_bind_int64(write_state, 5, input->as_of_ms);
        sqlite3_bind_int64(write_state, 6, changed_at);
        sqlite3_bind_text(write_state, 7, protection, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(write_state, 8, version);
        sqlite3_bind_text(write_state, 9, event_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(write_state, 10, input->algorithm_version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(write_state, 11, input->policy_version);
        sqlite3_bind_int(write_state, 12, input->config_version);
        sqlite3_bind_text(write_state, 13, input->policy_sha256, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(write_state, 14, after_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(write_state, 15, created_at, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(write_state) != SQLITE_DONE)
            rc = CBM_STORE_ERR;
        sqlite3_finalize(write_state);
        free(event_id);
        step++;
        if (rc == CBM_STORE_OK && fail_after == step)
            rc = CBM_STORE_ERR;
    }
    if (rc == CBM_STORE_OK && owns_transaction)
        rc = cbm_store_commit(store);
    if (rc != CBM_STORE_OK) {
        if (owns_transaction)
            cbm_store_rollback(store);
        return rc;
    }
    *out_recorded = 1;
    return CBM_STORE_OK;
}

static int stage9_build_maintenance(sqlite3 *db, const cbm_edge_lifecycle_input_t *input,
                                    stage9_decision_t **out_decisions, int *out_count) {
    stage9_edge_t *edges = NULL;
    int edge_count = 0;
    int rc = stage9_load_edges(db, &edges, &edge_count);
    if (rc != CBM_STORE_OK)
        return rc;
    stage9_decision_t *decisions =
        calloc((size_t)(edge_count > 0 ? edge_count : 1), sizeof(*decisions));
    if (!decisions) {
        stage9_edges_free(edges, edge_count);
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < edge_count; i++) {
        rc = stage9_decide(&edges[i], input->project, input->as_of_ms, &decisions[i]);
        if (rc != CBM_STORE_OK) {
            stage9_decisions_free(decisions, i + 1);
            stage9_edges_free(edges, edge_count);
            return rc;
        }
    }
    stage9_edges_free(edges, edge_count);
    qsort(decisions, (size_t)edge_count, sizeof(*decisions), stage9_decision_compare);
    *out_decisions = decisions;
    *out_count = edge_count;
    return CBM_STORE_OK;
}

static bool stage9_edge_requested(const cbm_edge_lifecycle_restore_input_t *input,
                                  const char *edge_id) {
    for (int i = 0; i < input->edge_count; i++) {
        if (input->edge_ids[i] && strcmp(input->edge_ids[i], edge_id) == 0)
            return true;
    }
    return false;
}

static int stage9_build_restore(sqlite3 *db, const cbm_edge_lifecycle_restore_input_t *restore,
                                stage9_decision_t **out_decisions, int *out_count) {
    stage9_edge_t *edges = NULL;
    int edge_count = 0;
    int rc = stage9_load_edges(db, &edges, &edge_count);
    if (rc != CBM_STORE_OK)
        return rc;
    stage9_decision_t *decisions =
        calloc((size_t)(restore->edge_count > 0 ? restore->edge_count : 1), sizeof(*decisions));
    if (!decisions) {
        stage9_edges_free(edges, edge_count);
        return CBM_STORE_ERR;
    }
    int count = 0;
    for (int i = 0; i < edge_count; i++) {
        if (!stage9_edge_requested(restore, edges[i].edge_id))
            continue;
        stage9_decision_t *decision = &decisions[count];
        if (stage9_decide(&edges[i], restore->lifecycle.project, restore->lifecycle.as_of_ms,
                          decision) != CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        free(decision->to_state);
        free(decision->reason_code);
        decision->protected_edge = false;
        if (strcmp(decision->from_state, "disabled") == 0) {
            decision->to_state = stage9_dup("disabled");
            decision->reason_code = stage9_dup("RESTORE_DISABLED_REJECTED");
            decision->protected_edge = true;
        } else if (strcmp(edges[i].src_project, restore->lifecycle.project) != 0 ||
                   strcmp(edges[i].dst_project, restore->lifecycle.project) != 0) {
            decision->to_state = stage9_dup(decision->from_state);
            decision->reason_code = stage9_dup("CROSS_SCOPE_BLOCKED");
            decision->protected_edge = true;
        } else if (strcmp(decision->from_state, "cold") == 0 ||
                   strcmp(decision->from_state, "archived") == 0) {
            decision->to_state = stage9_dup("active");
            decision->reason_code = stage9_dup("RESTORE_TO_ACTIVE");
        } else {
            decision->to_state = stage9_dup(decision->from_state);
            decision->reason_code = stage9_dup("RESTORE_ALREADY_ACTIVE");
        }
        if (!decision->to_state || !decision->reason_code ||
            stage9_decision_hash(decision) != CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        count++;
    }
    if (rc == CBM_STORE_OK && count != restore->edge_count)
        rc = CBM_STORE_NOT_FOUND;
    stage9_edges_free(edges, edge_count);
    if (rc != CBM_STORE_OK) {
        stage9_decisions_free(decisions, count + (count < restore->edge_count ? 1 : 0));
        return rc;
    }
    qsort(decisions, (size_t)count, sizeof(*decisions), stage9_decision_compare);
    *out_decisions = decisions;
    *out_count = count;
    return CBM_STORE_OK;
}

static int stage9_run(cbm_store_t *store, const cbm_edge_lifecycle_input_t *input,
                      const char *operation, stage9_decision_t *decisions, int count,
                      bool owns_transaction, bool stage14_parent_authorized,
                      cbm_edge_lifecycle_result_t *out) {
    sqlite3 *db = cbm_store_get_db(store);
    if (strcmp(input->mode, "active") == 0 && input->run_id && input->run_id[0] &&
        stage9_ledger_valid(db)) {
        stage9_decision_t *stored_decisions = NULL;
        int stored_count = 0;
        char stored_request_hash[65] = {0};
        char stored_report_hash[65] = {0};
        int existing = stage9_load_existing_run(db, input->run_id, &stored_decisions, &stored_count,
                                                stored_request_hash, stored_report_hash);
        if (existing != CBM_STORE_NOT_FOUND) {
            if (existing != CBM_STORE_OK)
                return existing;
            char stored_decision_hash[65];
            char actual_request_hash[65];
            int transitions = 0;
            int protected_count = 0;
            int rc = stage9_decision_set_hash(stored_decisions, stored_count, stored_decision_hash);
            for (int i = 0; i < stored_count; i++) {
                if (strcmp(stored_decisions[i].from_state, stored_decisions[i].to_state) != 0)
                    transitions++;
                if (stored_decisions[i].protected_edge)
                    protected_count++;
            }
            char actual_report_hash[65];
            if (rc == CBM_STORE_OK)
                rc = stage9_request_hash(input, operation, stored_decisions, stored_count,
                                         stored_decision_hash, stage14_parent_authorized,
                                         actual_request_hash);
            if (rc == CBM_STORE_OK)
                rc = stage9_report_hash(input, operation, stored_decision_hash, stored_count,
                                        transitions, protected_count, actual_report_hash);
            if (rc != CBM_STORE_OK || strcmp(actual_request_hash, stored_request_hash) != 0 ||
                strcmp(actual_report_hash, stored_report_hash) != 0 ||
                !stage9_manifest_verify(input, operation, stored_decisions, stored_count,
                                        stored_decision_hash, stage14_parent_authorized)) {
                stage9_decisions_free(stored_decisions, stored_count);
                return CBM_STORE_IDEMPOTENCY_CONFLICT;
            }
            out->decision_count = stored_count;
            out->transition_count = transitions;
            out->replayed_count = 1;
            out->report_json =
                stage9_report_json(input, operation, stored_decisions, stored_count,
                                   stored_decision_hash, stored_report_hash, false, true);
            stage9_decisions_free(stored_decisions, stored_count);
            return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
        }
    }
    char decision_hash[65];
    if (stage9_decision_set_hash(decisions, count, decision_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int transitions = 0;
    int protected_count = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(decisions[i].from_state, decisions[i].to_state) != 0)
            transitions++;
        if (decisions[i].protected_edge)
            protected_count++;
    }
    char report_hash[65];
    if (stage9_report_hash(input, operation, decision_hash, count, transitions, protected_count,
                           report_hash) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    bool wrote = false;
    bool replayed = false;
    if (strcmp(input->mode, "active") == 0) {
        if (!stage9_ledger_valid(db) ||
            !stage9_manifest_verify(input, operation, decisions, count, decision_hash,
                                    stage14_parent_authorized)) {
            rc = CBM_STORE_REJECTED;
        } else {
            rc = stage9_persist(store, input, operation, decisions, count, decision_hash,
                                report_hash, owns_transaction, stage14_parent_authorized,
                                &out->recorded_count, &out->replayed_count);
            wrote = rc == CBM_STORE_OK && out->recorded_count > 0;
            replayed = rc == CBM_STORE_OK && out->replayed_count > 0;
        }
    }
    if (rc == CBM_STORE_OK) {
        out->decision_count = count;
        out->transition_count = transitions;
        out->report_json = stage9_report_json(input, operation, decisions, count, decision_hash,
                                              report_hash, wrote, replayed);
        if (!out->report_json)
            rc = CBM_STORE_ERR;
    }
    return rc;
}

void cbm_store_memory_edge_lifecycle_result_free(cbm_edge_lifecycle_result_t *result) {
    if (!result)
        return;
    free(result->report_json);
    memset(result, 0, sizeof(*result));
}

static bool stage9_controller_transaction(sqlite3 *db, const char *controller_run_id) {
    if (!db || !controller_run_id || !controller_run_id[0] || sqlite3_get_autocommit(db) != 0)
        return false;
    sqlite3_stmt *stmt = NULL;
    bool valid = false;
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM global_maintenance_run WHERE run_id=?1 "
                           "AND status IN ('running','checkpointed') LIMIT 1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, controller_run_id, -1, SQLITE_TRANSIENT);
        valid = sqlite3_step(stmt) == SQLITE_ROW;
    }
    sqlite3_finalize(stmt);
    return valid;
}

static int stage9_maintenance(cbm_store_t *store, const cbm_edge_lifecycle_input_t *input,
                              const char *controller_run_id, bool owns_transaction,
                              bool stage14_parent_authorized, cbm_edge_lifecycle_result_t *out) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || !out || !stage9_input_valid(input) ||
        (!owns_transaction && (strcmp(input->mode, "active") != 0 ||
                               !stage9_controller_transaction(db, controller_run_id))))
        return CBM_STORE_ERR;
    if (strcmp(input->mode, "off") == 0) {
        char empty_hash[65];
        stage9_hash_text("[]", empty_hash);
        char report_hash[65];
        stage9_report_hash(input, "maintenance", empty_hash, 0, 0, 0, report_hash);
        out->report_json = stage9_report_json(input, "maintenance", NULL, 0, empty_hash,
                                              report_hash, false, false);
        return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    stage9_decision_t *decisions = NULL;
    int count = 0;
    int rc = stage9_build_maintenance(db, input, &decisions, &count);
    if (rc == CBM_STORE_OK)
        rc = stage9_run(store, input, "maintenance", decisions, count, owns_transaction,
                        stage14_parent_authorized, out);
    stage9_decisions_free(decisions, count);
    return rc;
}

int cbm_store_memory_edge_maintenance(cbm_store_t *store, const cbm_edge_lifecycle_input_t *input,
                                      cbm_edge_lifecycle_result_t *out) {
    return stage9_maintenance(store, input, NULL, true, false, out);
}

int cbm_store_memory_edge_maintenance_in_transaction(cbm_store_t *store,
                                                     const cbm_edge_lifecycle_input_t *input,
                                                     const char *controller_run_id,
                                                     bool stage14_parent_authorized,
                                                     cbm_edge_lifecycle_result_t *out) {
    return stage9_maintenance(store, input, controller_run_id, false, stage14_parent_authorized,
                              out);
}

int cbm_store_memory_edge_restore(cbm_store_t *store,
                                  const cbm_edge_lifecycle_restore_input_t *input,
                                  cbm_edge_lifecycle_result_t *out) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || !out || !input || input->edge_count <= 0 || !input->edge_ids ||
        !stage9_input_valid(&input->lifecycle) || strcmp(input->lifecycle.mode, "off") == 0) {
        return CBM_STORE_ERR;
    }
    stage9_decision_t *decisions = NULL;
    int count = 0;
    int rc = stage9_build_restore(db, input, &decisions, &count);
    if (rc == CBM_STORE_OK)
        rc = stage9_run(store, &input->lifecycle, "restore", decisions, count, true, false, out);
    stage9_decisions_free(decisions, count);
    return rc;
}

bool cbm_store_memory_edge_allows_propagation(cbm_store_t *store, const char *edge_id) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !edge_id || !edge_id[0] || !stage9_table_exists(db, "edge_lifecycle_state"))
        return true;
    sqlite3_stmt *stmt = NULL;
    bool allowed = true;
    if (sqlite3_prepare_v2(db, "SELECT lifecycle_state FROM edge_lifecycle_state WHERE edge_id=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            allowed = strcmp(stage9_col_text(stmt, 0), "active") == 0;
    }
    sqlite3_finalize(stmt);
    return allowed;
}

int cbm_store_memory_stage9_audit_verify(cbm_store_t *store, int *out_count) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (out_count)
        *out_count = 0;
    if (!db || !stage9_ledger_valid(db))
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT event_id,run_id,edge_id,operation,from_state,to_state,before_state_sha256,after_"
        "state_sha256,decision_sha256,algorithm_version,policy_version,config_version,prev_hash,"
        "event_hash,created_at FROM edge_lifecycle_audit_event ORDER BY sequence_no;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    char expected_prev[65] = "GENESIS";
    int count = 0;
    int rc = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        stage9_decision_t decision = {0};
        decision.edge_id = (char *)stage9_col_text(stmt, 2);
        decision.from_state = (char *)stage9_col_text(stmt, 4);
        decision.to_state = (char *)stage9_col_text(stmt, 5);
        snprintf(decision.decision_sha256, sizeof(decision.decision_sha256), "%s",
                 stage9_col_text(stmt, 8));
        cbm_edge_lifecycle_input_t input = {0};
        input.algorithm_version = stage9_col_text(stmt, 9);
        input.policy_version = sqlite3_column_int(stmt, 10);
        input.config_version = sqlite3_column_int(stmt, 11);
        const char *prev_hash = stage9_col_text(stmt, 12);
        const char *stored_hash = stage9_col_text(stmt, 13);
        char actual_hash[65];
        if (strcmp(prev_hash, expected_prev) != 0 ||
            stage9_event_hash(stage9_col_text(stmt, 0), stage9_col_text(stmt, 1), &decision,
                              stage9_col_text(stmt, 3), stage9_col_text(stmt, 6),
                              stage9_col_text(stmt, 7), &input, prev_hash,
                              stage9_col_text(stmt, 14), actual_hash) != CBM_STORE_OK ||
            strcmp(stored_hash, actual_hash) != 0) {
            rc = CBM_STORE_ERR;
            break;
        }
        snprintf(expected_prev, sizeof(expected_prev), "%s", stored_hash);
        count++;
    }
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK && out_count)
        *out_count = count;
    return rc;
}
