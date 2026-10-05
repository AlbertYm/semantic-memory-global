#include "memory/global_memory.h"
#include "memory/concept_growth.h"
#include "memory/edge_lifecycle.h"
#include "foundation/platform.h"
#include "store/store.h"

#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <stdint.h>
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#include <wchar.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

struct cbm_global_memory {
    cbm_store_t *memory;
    sqlite3 *graph;
};

static char *gm_dup(const char *value) {
    if (!value)
        return NULL;
    size_t n = strlen(value) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, value, n);
    return copy;
}

static void gm_timestamp(char out[40]) {
    time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(out, 40, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int gm_exec(sqlite3 *db, const char *sql) {
    return db && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? CBM_STORE_OK
                                                                      : CBM_STORE_ERR;
}

static int gm_bind_text(sqlite3_stmt *stmt, int index, const char *value) {
    return value ? sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT)
                 : sqlite3_bind_null(stmt, index);
}

static int gm_scalar_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static const char *memory_schema_sql =
    "CREATE TABLE IF NOT EXISTS global_project_catalog("
    "project_uuid TEXT PRIMARY KEY,canonical_path TEXT NOT NULL UNIQUE,path_hash TEXT NOT NULL "
    "UNIQUE,"
    "display_name TEXT NOT NULL,volume_id TEXT,source_fingerprint TEXT,workspace_state TEXT NOT "
    "NULL,"
    "index_state TEXT NOT NULL,first_seen_at TEXT NOT NULL,last_seen_at TEXT NOT NULL,"
    "created_by_version TEXT NOT NULL,row_sha256 TEXT NOT NULL);"
    "CREATE UNIQUE INDEX IF NOT EXISTS global_project_source_fingerprint_idx ON "
    "global_project_catalog(source_fingerprint) WHERE source_fingerprint IS NOT NULL AND "
    "source_fingerprint<>'';"
    "CREATE TABLE IF NOT EXISTS global_project_alias(alias_id TEXT PRIMARY KEY,project_uuid TEXT "
    "NOT NULL,"
    "alias_kind TEXT NOT NULL,canonical_alias_path TEXT NOT NULL,path_hash TEXT NOT NULL UNIQUE,"
    "source_fingerprint TEXT,valid_from TEXT NOT NULL,valid_to TEXT,row_sha256 TEXT NOT NULL,"
    "FOREIGN KEY(project_uuid) REFERENCES global_project_catalog(project_uuid) ON DELETE RESTRICT);"
    "CREATE TABLE IF NOT EXISTS global_task_workspace(task_id TEXT PRIMARY KEY,project_uuid TEXT "
    "NOT NULL,"
    "session_id TEXT NOT NULL,turn_id TEXT NOT NULL,resolver_payload_sha256 TEXT NOT "
    "NULL,created_at TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS global_task_workspace_project_idx ON "
    "global_task_workspace(project_uuid,task_id);"
    "CREATE TABLE IF NOT EXISTS global_memory_provenance(memory_item_id TEXT NOT NULL,project_uuid "
    "TEXT NOT NULL,"
    "legacy_project_id TEXT,source_kind TEXT NOT NULL,payload_sha256 TEXT NOT NULL,created_at TEXT "
    "NOT NULL,"
    "PRIMARY KEY(memory_item_id,project_uuid,source_kind));"
    "CREATE INDEX IF NOT EXISTS global_memory_provenance_project_idx ON "
    "global_memory_provenance(project_uuid,memory_item_id);"
    "CREATE TABLE IF NOT EXISTS global_retrieval_project_context(session_id TEXT PRIMARY "
    "KEY,project_uuid TEXT NOT NULL,"
    "soft_boost_ppm INTEGER NOT NULL,candidate_pool TEXT NOT NULL,payload_sha256 TEXT NOT "
    "NULL,created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS global_evolution_event(sequence_no INTEGER PRIMARY KEY "
    "AUTOINCREMENT,"
    "event_id TEXT NOT NULL UNIQUE,task_id TEXT,project_uuid TEXT NOT NULL,object_kind TEXT NOT "
    "NULL,"
    "object_id TEXT NOT NULL,operation TEXT NOT NULL,evidence_grade TEXT NOT NULL,evidence_id TEXT,"
    "before_sha256 TEXT NOT NULL,after_sha256 TEXT NOT NULL,algorithm_version TEXT NOT NULL,"
    "config_version INTEGER NOT NULL,idempotency_key TEXT NOT NULL UNIQUE,payload_sha256 TEXT NOT "
    "NULL,"
    "prev_hash TEXT NOT NULL,event_hash TEXT NOT NULL,created_at TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS global_evolution_event_task_sequence_idx ON "
    "global_evolution_event(task_id,sequence_no);"
    "CREATE TABLE IF NOT EXISTS global_maintenance_lease(lease_name TEXT PRIMARY KEY,owner_id TEXT "
    "NOT NULL,"
    "acquired_at TEXT NOT NULL,expires_at TEXT NOT NULL,checkpoint_json TEXT NOT "
    "NULL,payload_sha256 TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS global_maintenance_run(run_id TEXT PRIMARY KEY,project_uuid TEXT "
    "NOT NULL,"
    "mode TEXT NOT NULL,status TEXT NOT NULL,owner_id TEXT NOT NULL,limit_count INTEGER NOT NULL,"
    "budget_seconds INTEGER NOT NULL,checkpoint_json TEXT NOT NULL,consolidated_count INTEGER NOT "
    "NULL,"
    "decayed_count INTEGER NOT NULL,archived_count INTEGER NOT NULL,idempotency_key TEXT NOT NULL "
    "UNIQUE,"
    "payload_sha256 TEXT NOT NULL,started_at TEXT NOT NULL,completed_at TEXT);"
    "CREATE INDEX IF NOT EXISTS global_maintenance_run_history_idx ON "
    "global_maintenance_run(project_uuid,started_at DESC,run_id);"
    "CREATE TABLE IF NOT EXISTS global_config_drift_event(event_id TEXT PRIMARY KEY,classification "
    "TEXT NOT NULL,"
    "managed_fingerprint_before TEXT,managed_fingerprint_after TEXT,whole_config_sha256_before "
    "TEXT,"
    "whole_config_sha256_after TEXT,payload_sha256 TEXT NOT NULL,created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS global_migration_ledger(migration_id TEXT PRIMARY "
    "KEY,source_store_id TEXT NOT NULL,"
    "source_logical_sha256 TEXT NOT NULL,target_store_id TEXT NOT NULL,target_logical_sha256 TEXT "
    "NOT NULL,"
    "state TEXT NOT NULL,idempotency_key TEXT NOT NULL UNIQUE,payload_sha256 TEXT NOT "
    "NULL,created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS global_legacy_alias(legacy_kind TEXT NOT NULL,legacy_id TEXT NOT "
    "NULL,"
    "global_id TEXT NOT NULL,project_uuid TEXT NOT NULL,payload_sha256 TEXT NOT NULL,created_at "
    "TEXT NOT NULL,"
    "PRIMARY KEY(legacy_kind,legacy_id));"
    "CREATE TABLE IF NOT EXISTS stage14_component_ledger(component TEXT NOT NULL,version INTEGER "
    "NOT NULL,"
    "name TEXT NOT NULL,checksum TEXT NOT NULL,applied_at TEXT NOT NULL,PRIMARY "
    "KEY(component,version));";

static const char *graph_schema_sql =
    "CREATE TABLE IF NOT EXISTS global_project_node(project_uuid TEXT PRIMARY KEY,display_name "
    "TEXT NOT NULL,"
    "summary_sha256 TEXT NOT NULL,updated_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS global_cross_project_edge(edge_id TEXT PRIMARY "
    "KEY,source_project_uuid TEXT NOT NULL,"
    "target_project_uuid TEXT NOT NULL,relation_type TEXT NOT NULL,weight_ppm INTEGER NOT "
    "NULL,confidence_ppm INTEGER NOT NULL,"
    "status TEXT NOT NULL,version INTEGER NOT NULL,updated_at TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS global_cross_project_edge_source_list_idx ON "
    "global_cross_project_edge(source_project_uuid,status,updated_at DESC,edge_id);"
    "CREATE INDEX IF NOT EXISTS global_cross_project_edge_target_list_idx ON "
    "global_cross_project_edge(target_project_uuid,status,updated_at DESC,edge_id);"
    "CREATE TABLE IF NOT EXISTS global_cross_project_edge_version(edge_id TEXT NOT NULL,version "
    "INTEGER NOT NULL,"
    "payload_sha256 TEXT NOT NULL,evidence_event_id TEXT NOT NULL,created_at TEXT NOT NULL,PRIMARY "
    "KEY(edge_id,version));"
    "CREATE INDEX IF NOT EXISTS global_cross_project_edge_version_list_idx ON "
    "global_cross_project_edge_version(created_at DESC,edge_id,version);";

cbm_global_memory_t *cbm_global_memory_open(const char *memory_db_path,
                                            const char *global_graph_db_path) {
    if (!memory_db_path || !global_graph_db_path)
        return NULL;
    cbm_global_memory_t *global = calloc(1, sizeof(*global));
    if (!global)
        return NULL;
    global->memory = strcmp(memory_db_path, ":memory:") == 0 ? cbm_store_open_memory()
                                                             : cbm_store_open_path(memory_db_path);
    if (!global->memory ||
        sqlite3_open_v2(global_graph_db_path, &global->graph,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        cbm_global_memory_close(global);
        return NULL;
    }
    sqlite3_busy_timeout(cbm_store_get_db(global->memory), 5000);
    sqlite3_busy_timeout(global->graph, 5000);
    if (cbm_global_memory_migrate(global, NULL) != CBM_STORE_OK ||
        cbm_store_memory_stage9_migrate(global->memory) < CBM_STORE_OK ||
        cbm_store_memory_stage10_migrate(global->memory) < CBM_STORE_OK) {
        cbm_global_memory_close(global);
        return NULL;
    }
    return global;
}

cbm_global_memory_t *cbm_global_memory_open_default(void) {
    char memory_path[4096], graph_path[4096];
    const char *root = cbm_resolve_cache_dir();
    if (!root || !root[0] ||
        cbm_memory_db_path(CBM_GLOBAL_MEMORY_PROJECT, memory_path, sizeof(memory_path)) !=
            CBM_STORE_OK)
        return NULL;
#ifdef _WIN32
    snprintf(graph_path, sizeof(graph_path), "%s\\__global__-graph.db", root);
#else
    snprintf(graph_path, sizeof(graph_path), "%s/__global__-graph.db", root);
#endif
    return cbm_global_memory_open(memory_path, graph_path);
}

void cbm_global_memory_close(cbm_global_memory_t *global) {
    if (!global)
        return;
    if (global->graph)
        sqlite3_close(global->graph);
    cbm_store_close(global->memory);
    free(global);
}

cbm_store_t *cbm_global_memory_store(cbm_global_memory_t *global) {
    return global ? global->memory : NULL;
}
sqlite3 *cbm_global_memory_db(cbm_global_memory_t *global) {
    return global ? cbm_store_get_db(global->memory) : NULL;
}
sqlite3 *cbm_global_graph_db(cbm_global_memory_t *global) {
    return global ? global->graph : NULL;
}

int cbm_global_memory_migrate(cbm_global_memory_t *global, int *out_replayed) {
    if (out_replayed)
        *out_replayed = 0;
    if (!global || !global->memory || !global->graph)
        return CBM_STORE_ERR;
    sqlite3 *db = cbm_store_get_db(global->memory);
    if (gm_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int existed = gm_scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                                    "name='stage14_component_ledger';");
    int rc = gm_exec(db, memory_schema_sql);
    char now[40];
    gm_timestamp(now);
    sqlite3_stmt *stmt = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO "
                           "stage14_component_ledger(component,version,name,checksum,applied_at) "
                           "VALUES('global_memory_core',1,'stage14_global_schema_v1','"
                           "a34daf768bb3a6fe524e47fbfc83318d813b16425e0dd9fcac8405825e9620f8',?1);",
                           -1, &stmt, NULL) == SQLITE_OK) {
        gm_bind_text(stmt, 1, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        rc = gm_exec(db, "COMMIT;");
    else
        gm_exec(db, "ROLLBACK;");
    if (rc != CBM_STORE_OK)
        return rc;
    if (gm_exec(global->graph, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    rc = gm_exec(global->graph, graph_schema_sql);
    if (rc == CBM_STORE_OK)
        rc = gm_exec(global->graph, "COMMIT;");
    else
        gm_exec(global->graph, "ROLLBACK;");
    if (out_replayed)
        *out_replayed = existed > 0;
    bool orchestrator_replayed = false;
    char *report = NULL;
    int orc = cbm_orchestrator_migrate(global->memory, &orchestrator_replayed, &report);
    free(report);
    return (orc == CBM_STORE_OK || orc == CBM_STORE_REPLAYED) ? rc : CBM_STORE_ERR;
}

int cbm_global_store_migrate(cbm_store_t *store, int *out_replayed) {
    if (out_replayed)
        *out_replayed = 0;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db)
        return CBM_STORE_ERR;
    int replayed = gm_scalar_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                                     "name='stage14_component_ledger';") > 0;
    if (gm_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int rc = gm_exec(db, memory_schema_sql);
    char now[40];
    gm_timestamp(now);
    sqlite3_stmt *stmt = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO "
                           "stage14_component_ledger(component,version,name,checksum,applied_at) "
                           "VALUES('global_memory_core',1,'stage14_global_schema_v1','"
                           "a34daf768bb3a6fe524e47fbfc83318d813b16425e0dd9fcac8405825e9620f8',?1);",
                           -1, &stmt, NULL) == SQLITE_OK) {
        gm_bind_text(stmt, 1, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK) {
        rc = CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        rc = gm_exec(db, "COMMIT;");
    else
        gm_exec(db, "ROLLBACK;");
    if (rc != CBM_STORE_OK)
        return rc;
    bool orchestrator_replayed = false;
    char *report = NULL;
    rc = cbm_orchestrator_migrate(store, &orchestrator_replayed, &report);
    free(report);
    if (out_replayed)
        *out_replayed = replayed && orchestrator_replayed;
    return rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED
               ? (replayed && orchestrator_replayed ? CBM_STORE_REPLAYED : CBM_STORE_OK)
               : CBM_STORE_ERR;
}

static char *gm_project_payload(const cbm_project_resolution_t *r) {
    size_t size = strlen(r->canonical_path) + strlen(r->path_hash) + strlen(r->project_uuid) +
                  strlen(r->display_name) + strlen(r->volume_id) + strlen(r->source_fingerprint) +
                  512;
    char *payload = malloc(size);
    const char *workspace_state =
        r->path_writable ? "writable" : (r->path_exists ? "read_only" : "missing");
    if (payload)
        snprintf(payload, size,
                 "stage14-project-resolver/"
                 "v1\ncanonical_path=%zu:%s\npath_hash=%s\nproject_uuid=%s\ndisplay_name=%zu:%"
                 "s\nvolume_id=%zu:%s\nsource_fingerprint=%s\npath_exists=%d\npath_writable=%"
                 "d\nworkspace_state=%s",
                 strlen(r->canonical_path), r->canonical_path, r->path_hash, r->project_uuid,
                 strlen(r->display_name), r->display_name, strlen(r->volume_id), r->volume_id,
                 r->source_fingerprint, r->path_exists ? 1 : 0, r->path_writable ? 1 : 0,
                 workspace_state);
    return payload;
}

static char *gm_project_identity_payload(const cbm_project_resolution_t *r) {
    size_t size = strlen(r->canonical_path) + strlen(r->path_hash) + strlen(r->project_uuid) +
                  strlen(r->volume_id) + strlen(r->source_fingerprint) + 256;
    char *payload = malloc(size);
    if (payload)
        snprintf(payload, size,
                 "stage14-project-identity/"
                 "v1\ncanonical_path=%zu:%s\npath_hash=%s\nproject_uuid=%s\nvolume_id=%zu:%"
                 "s\nsource_fingerprint=%s",
                 strlen(r->canonical_path), r->canonical_path, r->path_hash, r->project_uuid,
                 strlen(r->volume_id), r->volume_id, r->source_fingerprint);
    return payload;
}

static int gm_existing_project(sqlite3 *db, const cbm_project_resolution_t *r,
                               char uuid[CBM_PROJECT_UUID_SIZE], char path_hash[65],
                               char row_hash[65]) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        r->source_fingerprint[0]
            ? "SELECT project_uuid,path_hash,row_sha256 FROM global_project_catalog WHERE "
              "path_hash=?1 OR source_fingerprint=?2 ORDER BY path_hash=?1 DESC LIMIT 1;"
            : "SELECT project_uuid,path_hash,row_sha256 FROM global_project_catalog WHERE "
              "path_hash=?1 LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(stmt, 1, r->path_hash);
    gm_bind_text(stmt, 2, r->source_fingerprint[0] ? r->source_fingerprint : NULL);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        snprintf(uuid, CBM_PROJECT_UUID_SIZE, "%s", sqlite3_column_text(stmt, 0));
        snprintf(path_hash, 65, "%s", sqlite3_column_text(stmt, 1));
        snprintf(row_hash, 65, "%s", sqlite3_column_text(stmt, 2));
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW ? CBM_STORE_OK : CBM_STORE_NOT_FOUND;
}

static char *gm_report(const char *status, const char *project_uuid, int alias_created) {
    char *json = malloc(320);
    if (json)
        snprintf(json, 320,
                 "{\"schema\":\"semantic-memory-global-project/"
                 "v1\",\"status\":\"%s\",\"project_uuid\":\"%s\",\"alias_created\":%s,\"production_"
                 "state_written\":%s}",
                 status, project_uuid ? project_uuid : "", alias_created ? "true" : "false",
                 strcmp(status, "recorded") == 0 ? "true" : "false");
    return json;
}

static int gm_ensure_project_locked(cbm_store_t *store, const cbm_project_resolution_t *resolution,
                                    const char *idempotency_key, char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    if (!store || !resolution || !resolution->canonical_path[0] || !out_report_json)
        return CBM_STORE_ERR;
    sqlite3 *db = cbm_store_get_db(store);
    char *payload = gm_project_identity_payload(resolution), payload_hash[65];
    if (!payload || cbm_stage7_sha256_hex(payload, strlen(payload), payload_hash) != CBM_STORE_OK) {
        free(payload);
        return CBM_STORE_ERR;
    }
    free(payload);
    if (!idempotency_key || !idempotency_key[0])
        return CBM_STORE_ERR;
    char uuid[CBM_PROJECT_UUID_SIZE] = {0}, prior_path_hash[65] = {0}, prior_row_hash[65] = {0};
    int existing = gm_existing_project(db, resolution, uuid, prior_path_hash, prior_row_hash);
    int alias_created = 0, rc = CBM_STORE_OK;
    char now[40];
    gm_timestamp(now);
    if (existing == CBM_STORE_NOT_FOUND) {
        snprintf(uuid, sizeof(uuid), "%s", resolution->project_uuid);
        sqlite3_stmt *stmt = NULL;
        const char *sql =
            "INSERT INTO "
            "global_project_catalog(project_uuid,canonical_path,path_hash,display_name,volume_id,"
            "source_fingerprint,workspace_state,index_state,first_seen_at,last_seen_at,created_by_"
            "version,row_sha256) VALUES(?1,?2,?3,?4,?5,?6,?7,'queued',?8,?8,'v1.1.0',?9);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK) {
            gm_bind_text(stmt, 1, uuid);
            gm_bind_text(stmt, 2, resolution->canonical_path);
            gm_bind_text(stmt, 3, resolution->path_hash);
            gm_bind_text(stmt, 4, resolution->display_name);
            gm_bind_text(stmt, 5, resolution->volume_id[0] ? resolution->volume_id : NULL);
            gm_bind_text(stmt, 6,
                         resolution->source_fingerprint[0] ? resolution->source_fingerprint : NULL);
            gm_bind_text(stmt, 7,
                         resolution->path_writable
                             ? "writable"
                             : (resolution->path_exists ? "read_only" : "missing"));
            gm_bind_text(stmt, 8, now);
            gm_bind_text(stmt, 9, payload_hash);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
    } else if (existing == CBM_STORE_OK && strcmp(prior_path_hash, resolution->path_hash) == 0 &&
               strcmp(prior_row_hash, payload_hash) != 0) {
        rc = CBM_STORE_IDEMPOTENCY_CONFLICT;
    } else if (existing == CBM_STORE_OK && strcmp(prior_path_hash, resolution->path_hash) != 0) {
        char alias_seed[4224], alias_id[65];
        snprintf(alias_seed, sizeof(alias_seed), "%s\n%s", uuid, resolution->path_hash);
        cbm_stage7_sha256_hex(alias_seed, strlen(alias_seed), alias_id);
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT row_sha256 FROM global_project_alias WHERE path_hash=?1;",
                               -1, &stmt, NULL) == SQLITE_OK) {
            gm_bind_text(stmt, 1, resolution->path_hash);
            int found = sqlite3_step(stmt);
            if (found == SQLITE_ROW) {
                const char *stored = (const char *)sqlite3_column_text(stmt, 0);
                rc = stored && strcmp(stored, payload_hash) == 0 ? CBM_STORE_REPLAYED
                                                                 : CBM_STORE_IDEMPOTENCY_CONFLICT;
            }
            sqlite3_finalize(stmt);
            stmt = NULL;
        } else
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK &&
            sqlite3_prepare_v2(db,
                               "INSERT INTO "
                               "global_project_alias(alias_id,project_uuid,alias_kind,canonical_"
                               "alias_path,path_hash,source_fingerprint,valid_from,valid_to,row_"
                               "sha256) VALUES(?1,?2,'moved_path',?3,?4,?5,?6,NULL,?7);",
                               -1, &stmt, NULL) != SQLITE_OK)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK) {
            gm_bind_text(stmt, 1, alias_id);
            gm_bind_text(stmt, 2, uuid);
            gm_bind_text(stmt, 3, resolution->canonical_path);
            gm_bind_text(stmt, 4, resolution->path_hash);
            gm_bind_text(stmt, 5, resolution->source_fingerprint);
            gm_bind_text(stmt, 6, now);
            gm_bind_text(stmt, 7, payload_hash);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
        if (rc == CBM_STORE_OK)
            alias_created = 1;
    } else if (existing != CBM_STORE_OK)
        rc = existing;
    if (rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED) {
        *out_report_json = gm_report(
            rc == CBM_STORE_IDEMPOTENCY_CONFLICT ? "IDEMPOTENCY_CONFLICT" : "failed", uuid, 0);
        return rc;
    }
    const char *status = existing == CBM_STORE_NOT_FOUND || alias_created ? "recorded" : "replayed";
    *out_report_json = gm_report(status, uuid, alias_created);
    return *out_report_json ? (strcmp(status, "replayed") == 0 ? CBM_STORE_REPLAYED : CBM_STORE_OK)
                            : CBM_STORE_ERR;
}

int cbm_global_ensure_project(cbm_global_memory_t *global,
                              const cbm_project_resolution_t *resolution,
                              const char *idempotency_key, char **out_report_json) {
    if (!global || gm_exec(cbm_global_memory_db(global), "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int rc = gm_ensure_project_locked(global->memory, resolution, idempotency_key, out_report_json);
    if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
        if (gm_exec(cbm_global_memory_db(global), "COMMIT;") != CBM_STORE_OK)
            return CBM_STORE_ERR;
    } else {
        gm_exec(cbm_global_memory_db(global), "ROLLBACK;");
    }
    return rc;
}

int cbm_global_store_ensure_project(cbm_store_t *store, const cbm_project_resolution_t *resolution,
                                    const char *idempotency_key, char **out_report_json) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || gm_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int rc = gm_ensure_project_locked(store, resolution, idempotency_key, out_report_json);
    if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
        if (gm_exec(db, "COMMIT;") != CBM_STORE_OK)
            return CBM_STORE_ERR;
    } else {
        gm_exec(db, "ROLLBACK;");
    }
    return rc;
}

static int gm_lookup_task(sqlite3 *db, const char *session_id, const char *turn_id,
                          char task_id[128]) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT task_id FROM codex_task_lifecycle WHERE session_id=?1 AND "
                           "turn_id=?2 ORDER BY created_at DESC,lifecycle_id DESC LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(stmt, 1, session_id);
    gm_bind_text(stmt, 2, turn_id);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW)
        snprintf(task_id, 128, "%s", sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW ? CBM_STORE_OK : CBM_STORE_NOT_FOUND;
}

int cbm_global_task_begin(cbm_global_memory_t *global, const cbm_project_resolution_t *resolution,
                          const cbm_task_begin_input_t *input, char **out_report_json) {
    if (!global || !resolution || !input || !out_report_json)
        return CBM_STORE_ERR;
    sqlite3 *db = cbm_store_get_db(global->memory);
    if (gm_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char *ensure_report = NULL;
    int erc = gm_ensure_project_locked(global->memory, resolution, input->idempotency_key,
                                       &ensure_report);
    free(ensure_report);
    if (erc != CBM_STORE_OK && erc != CBM_STORE_REPLAYED) {
        gm_exec(db, "ROLLBACK;");
        return erc;
    }
    cbm_task_begin_input_t global_input = *input;
    global_input.project = resolution->project_uuid;
    int rc = cbm_orchestrator_begin(global->memory, &global_input, out_report_json);
    if (rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED) {
        gm_exec(db, "ROLLBACK;");
        return rc;
    }
    char task_id[128], resolver_hash[65];
    char *resolver_payload = gm_project_payload(resolution);
    if (gm_lookup_task(db, input->session_id, input->turn_id, task_id) != CBM_STORE_OK ||
        !resolver_payload ||
        cbm_stage7_sha256_hex(resolver_payload, strlen(resolver_payload), resolver_hash) !=
            CBM_STORE_OK) {
        free(resolver_payload);
        gm_exec(db, "ROLLBACK;");
        free(*out_report_json);
        *out_report_json = NULL;
        return CBM_STORE_ERR;
    }
    free(resolver_payload);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO "
                           "global_task_workspace(task_id,project_uuid,session_id,turn_id,resolver_"
                           "payload_sha256,created_at) VALUES(?1,?2,?3,?4,?5,?6);",
                           -1, &stmt, NULL) != SQLITE_OK) {
        gm_exec(db, "ROLLBACK;");
        free(*out_report_json);
        *out_report_json = NULL;
        return CBM_STORE_ERR;
    }
    char now[40];
    gm_timestamp(now);
    gm_bind_text(stmt, 1, task_id);
    gm_bind_text(stmt, 2, resolution->project_uuid);
    gm_bind_text(stmt, 3, input->session_id);
    gm_bind_text(stmt, 4, input->turn_id);
    gm_bind_text(stmt, 5, resolver_hash);
    gm_bind_text(stmt, 6, now);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (step != SQLITE_DONE) {
        gm_exec(db, "ROLLBACK;");
        free(*out_report_json);
        *out_report_json = gm_report("failed", resolution->project_uuid, 0);
        return CBM_STORE_ERR;
    }
    int exact = 0;
    if (sqlite3_prepare_v2(
            db,
            "SELECT COUNT(*) FROM global_task_workspace w JOIN codex_task_lifecycle l ON "
            "l.task_id=w.task_id WHERE w.task_id=?1 AND w.project_uuid=?2 AND w.session_id=?3 AND "
            "w.turn_id=?4 AND w.resolver_payload_sha256=?5 AND l.idempotency_key=?6;",
            -1, &stmt, NULL) == SQLITE_OK) {
        gm_bind_text(stmt, 1, task_id);
        gm_bind_text(stmt, 2, resolution->project_uuid);
        gm_bind_text(stmt, 3, input->session_id);
        gm_bind_text(stmt, 4, input->turn_id);
        gm_bind_text(stmt, 5, resolver_hash);
        gm_bind_text(stmt, 6, input->idempotency_key);
        exact = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) >= 1;
    }
    sqlite3_finalize(stmt);
    if (!exact) {
        gm_exec(db, "ROLLBACK;");
        free(*out_report_json);
        *out_report_json = gm_report("IDEMPOTENCY_CONFLICT", resolution->project_uuid, 0);
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    if (gm_exec(db, "COMMIT;") != CBM_STORE_OK) {
        gm_exec(db, "ROLLBACK;");
        free(*out_report_json);
        *out_report_json = NULL;
        return CBM_STORE_ERR;
    }
    return rc;
}

int cbm_global_task_record_evidence(cbm_global_memory_t *global,
                                    const cbm_task_evidence_input_t *input,
                                    char **out_report_json) {
    return global ? cbm_orchestrator_record_evidence(global->memory, input, out_report_json)
                  : CBM_STORE_ERR;
}

static int gm_task_project(sqlite3 *db, const char *task_id, const char *session_id,
                           const char *turn_id, char project_uuid[256]) {
    sqlite3_stmt *stmt = NULL;
    const char *sql_by_task = "SELECT project FROM memory_task WHERE task_id=?1 LIMIT 1;";
    const char *sql_by_turn = "SELECT t.project FROM memory_task t JOIN codex_task_lifecycle l "
                              "ON l.task_id=t.task_id WHERE l.session_id=?1 AND l.turn_id=?2 "
                              "ORDER BY l.rowid DESC LIMIT 1;";
    const char *sql = task_id && task_id[0] ? sql_by_task : sql_by_turn;
    if (!db || (!task_id && (!session_id || !turn_id)) ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(stmt, 1, task_id && task_id[0] ? task_id : session_id);
    if (!(task_id && task_id[0]))
        gm_bind_text(stmt, 2, turn_id);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(project_uuid, 256, "%s", value ? value : "");
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW && project_uuid[0] ? CBM_STORE_OK : CBM_STORE_NOT_FOUND;
}

int cbm_global_task_status(cbm_global_memory_t *global, const char *task_id, const char *session_id,
                           const char *turn_id, char **out_project_uuid, char **out_report_json) {
    return global ? cbm_global_store_task_status(global->memory, task_id, session_id, turn_id,
                                                 out_project_uuid, out_report_json)
                  : CBM_STORE_ERR;
}

int cbm_global_store_task_status(cbm_store_t *store, const char *task_id, const char *session_id,
                                 const char *turn_id, char **out_project_uuid,
                                 char **out_report_json) {
    if (out_project_uuid)
        *out_project_uuid = NULL;
    if (out_report_json)
        *out_report_json = NULL;
    if (!store || !out_report_json)
        return CBM_STORE_ERR;
    sqlite3 *db = cbm_store_get_db(store);
    char project[256] = {0};
    int rc = gm_task_project(db, task_id, session_id, turn_id, project);
    if (rc != CBM_STORE_OK)
        return rc;
    rc = cbm_orchestrator_status(store, project, task_id, session_id, turn_id, out_report_json);
    if (rc == CBM_STORE_OK && out_project_uuid)
        *out_project_uuid = gm_dup(project);
    return rc;
}

static char *gm_stable_id(const char *prefix, const char *const *parts, int count) {
    XXH3_state_t *state = XXH3_createState();
    if (!state || XXH3_128bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        const char *part = parts[i] ? parts[i] : "";
        uint64_t length = (uint64_t)strlen(part);
        if (XXH3_128bits_update(state, &length, sizeof(length)) == XXH_ERROR ||
            XXH3_128bits_update(state, part, (size_t)length) == XXH_ERROR) {
            XXH3_freeState(state);
            return NULL;
        }
    }
    XXH128_hash_t hash = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    char value[96];
    snprintf(value, sizeof(value), "%s-%016llx%016llx", prefix, (unsigned long long)hash.high64,
             (unsigned long long)hash.low64);
    return gm_dup(value);
}

static int gm_replay_retrieval(cbm_global_memory_t *global, const char *session_id,
                               int requested_limit, cbm_global_retrieval_result_t *out) {
    sqlite3 *db = cbm_store_get_db(global->memory);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT c.id,c.source_store_id,c.memory_item_id,c.content_hash,c.aggregate_score,"
        "s.id,s.source_type,s.raw_score FROM retrieval_candidate c "
        "LEFT JOIN retrieval_candidate_source s ON s.candidate_id=c.id "
        "WHERE c.session_id=?1 ORDER BY c.aggregate_rank,c.id LIMIT ?2;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(stmt, 1, session_id);
    sqlite3_bind_int(stmt, 2, requested_limit);
    int capacity = requested_limit > 0 ? requested_limit : 10;
    out->items = calloc((size_t)capacity, sizeof(*out->items));
    if (!out->items) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_OK;
    while (out->count < capacity && sqlite3_step(stmt) == SQLITE_ROW) {
        cbm_global_candidate_t *candidate = &out->items[out->count];
        const char *candidate_id = (const char *)sqlite3_column_text(stmt, 0);
        const char *project_uuid = (const char *)sqlite3_column_text(stmt, 1);
        const char *memory_item_id = (const char *)sqlite3_column_text(stmt, 2);
        const char *content_hash = (const char *)sqlite3_column_text(stmt, 3);
        const char *provenance_id = (const char *)sqlite3_column_text(stmt, 5);
        const char *source_type = (const char *)sqlite3_column_text(stmt, 6);
        if (!candidate_id || !project_uuid || !memory_item_id || !content_hash || !provenance_id ||
            cbm_store_memory_get_item(global->memory, memory_item_id, &candidate->item) !=
                CBM_STORE_OK) {
            rc = CBM_STORE_ERR;
            break;
        }
        candidate->candidate_id = gm_dup(candidate_id);
        candidate->project_uuid = gm_dup(project_uuid);
        candidate->source_kind = gm_dup("global_memory");
        candidate->provenance_id = gm_dup(provenance_id);
        candidate->content_hash = gm_dup(content_hash);
        candidate->item.retrieval_source = gm_dup(source_type ? source_type : "manual");
        candidate->item.retrieval_score = sqlite3_column_double(stmt, 7);
        candidate->global_score = sqlite3_column_double(stmt, 4);
        const char *evidence_parts[] = {session_id, memory_item_id,
                                        "{\"project_role\":\"provenance_and_soft_boost\"}"};
        candidate->evidence_id = gm_stable_id("evid", evidence_parts, 3);
        if (!candidate->candidate_id || !candidate->project_uuid || !candidate->source_kind ||
            !candidate->provenance_id || !candidate->content_hash || !candidate->evidence_id) {
            rc = CBM_STORE_ERR;
            break;
        }
        out->count++;
    }
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK) {
        cbm_global_retrieval_result_free(out);
        return rc;
    }
    out->total = out->count;
    out->session_id = gm_dup(session_id);
    out->candidate_pool = gm_dup("global");
    return out->session_id && out->candidate_pool ? CBM_STORE_REPLAYED : CBM_STORE_ERR;
}

int cbm_global_task_complete(cbm_global_memory_t *global, const cbm_task_complete_input_t *input,
                             char **out_report_json) {
    if (!global || !input)
        return CBM_STORE_ERR;
    char project[256] = {0};
    int rc = gm_task_project(cbm_global_memory_db(global), input->task_id, NULL, NULL, project);
    if (rc != CBM_STORE_OK)
        return rc;
    cbm_task_complete_input_t global_input = *input;
    global_input.project = project;
    return cbm_orchestrator_complete(global->memory, &global_input, out_report_json);
}

int cbm_global_task_abandon(cbm_global_memory_t *global, const char *session_id,
                            const char *turn_id, const char *idempotency_key,
                            char **out_report_json) {
    if (!global)
        return CBM_STORE_ERR;
    char project[256] = {0};
    int rc = gm_task_project(cbm_global_memory_db(global), NULL, session_id, turn_id, project);
    if (rc != CBM_STORE_OK)
        return rc;
    return cbm_orchestrator_abandon_open(global->memory, project, session_id, turn_id,
                                         idempotency_key, out_report_json);
}

static int gm_candidate_compare(const void *left, const void *right) {
    const cbm_global_candidate_t *a = left, *b = right;
    if (a->global_score < b->global_score)
        return 1;
    if (a->global_score > b->global_score)
        return -1;
    return strcmp(a->item.id, b->item.id);
}

int cbm_global_memory_retrieve(cbm_global_memory_t *global, const char *session_id,
                               const char *current_project_uuid, int soft_boost_ppm,
                               const cbm_memory_query_t *query,
                               cbm_global_retrieval_result_t *out) {
    if (!global || !session_id || !current_project_uuid || !query || !out || soft_boost_ppm < 0 ||
        soft_boost_ppm > 250000)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    sqlite3 *db = cbm_store_get_db(global->memory);
    char canonical[8192], hash[65], now[40];
    snprintf(canonical, sizeof(canonical),
             "%s\n%s\n%d\nglobal\n%s\n%s\n%s\n%s\n%s\n%s\n%d\n%d\n%s\n%s", session_id,
             current_project_uuid, soft_boost_ppm, query->project ? query->project : "",
             query->user ? query->user : "", query->task ? query->task : "",
             query->entity_key ? query->entity_key : "", query->kind ? query->kind : "",
             query->query ? query->query : "", query->include_inactive ? 1 : 0, query->limit,
             query->code_context ? query->code_context : "",
             query->activation_mode ? query->activation_mode : "");
    cbm_stage7_sha256_hex(canonical, strlen(canonical), hash);
    sqlite3_stmt *context = NULL;
    int replayed = 0;
    if (sqlite3_prepare_v2(
            db, "SELECT payload_sha256 FROM global_retrieval_project_context WHERE session_id=?1;",
            -1, &context, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(context, 1, session_id);
    if (sqlite3_step(context) == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(context, 0);
        if (!stored || strcmp(stored, hash) != 0) {
            sqlite3_finalize(context);
            return CBM_STORE_IDEMPOTENCY_CONFLICT;
        }
        replayed = 1;
    }
    sqlite3_finalize(context);
    int requested_limit = query->limit > 0 ? query->limit : 10;
    if (replayed)
        return gm_replay_retrieval(global, session_id, requested_limit, out);
    cbm_memory_query_t global_query = *query;
    global_query.project = NULL;
    int pool_limit = requested_limit < 64 ? 64 : requested_limit * 4;
    if (pool_limit > 512)
        pool_limit = 512;
    global_query.limit = pool_limit;
    cbm_memory_result_t raw = {0};
    int rc = cbm_store_memory_retrieve(global->memory, &global_query, &raw);
    if (rc != CBM_STORE_OK)
        return rc;
    out->items = calloc((size_t)raw.count, sizeof(*out->items));
    if (raw.count && !out->items) {
        cbm_store_memory_result_free(&raw);
        return CBM_STORE_ERR;
    }
    out->count = raw.count;
    out->total = raw.total;
    out->session_id = gm_dup(session_id);
    out->candidate_pool = gm_dup("global");
    for (int i = 0; i < raw.count; i++) {
        out->items[i].item = raw.items[i];
        memset(&raw.items[i], 0, sizeof(raw.items[i]));
        char legacy[256] = {0}, project[128] = "__global__", source_kind[64] = "global_memory";
        snprintf(legacy, sizeof(legacy), "%s",
                 out->items[i].item.scope_project ? out->items[i].item.scope_project : "");
        sqlite3_stmt *prov = NULL;
        if (sqlite3_prepare_v2(
                db,
                "SELECT project_uuid,COALESCE(legacy_project_id,''),source_kind FROM "
                "global_memory_provenance WHERE memory_item_id=?1 ORDER BY created_at LIMIT 1;",
                -1, &prov, NULL) == SQLITE_OK) {
            gm_bind_text(prov, 1, out->items[i].item.id);
            if (sqlite3_step(prov) == SQLITE_ROW) {
                snprintf(project, sizeof(project), "%s", sqlite3_column_text(prov, 0));
                snprintf(legacy, sizeof(legacy), "%s", sqlite3_column_text(prov, 1));
                snprintf(source_kind, sizeof(source_kind), "%s", sqlite3_column_text(prov, 2));
                out->items[i].legacy_project_id = gm_dup(legacy);
            }
        }
        sqlite3_finalize(prov);
        if (strcmp(project, "__global__") == 0 && legacy[0])
            snprintf(project, sizeof(project), "%s", legacy);
        out->items[i].project_uuid = gm_dup(project);
        out->items[i].source_kind = gm_dup(source_kind);
        out->items[i].project_soft_boost_ppm =
            strcmp(project, current_project_uuid) == 0 ? soft_boost_ppm : 0;
        double evolved_quality =
            (out->items[i].item.confidence + out->items[i].item.reusability) / 2.0 -
            out->items[i].item.decay * 0.25;
        if (evolved_quality < 0.0)
            evolved_quality = 0.0;
        if (evolved_quality > 1.0)
            evolved_quality = 1.0;
        /* Keep lexical/vector relevance primary. Evidence evolution contributes at most +/-0.05,
         * enough to break relevance ties without weakening the FTS overlap gate. */
        double quality_adjustment = (evolved_quality - 0.5) * 0.10;
        out->items[i].global_score = out->items[i].item.retrieval_score + quality_adjustment +
                                     (double)out->items[i].project_soft_boost_ppm / 1000000.0;
    }
    cbm_store_memory_result_free(&raw);
    qsort(out->items, (size_t)out->count, sizeof(*out->items), gm_candidate_compare);
    if (out->count > requested_limit) {
        for (int i = requested_limit; i < out->count; i++) {
            cbm_store_memory_item_free(&out->items[i].item);
            free(out->items[i].project_uuid);
            free(out->items[i].legacy_project_id);
            free(out->items[i].source_kind);
            memset(&out->items[i], 0, sizeof(out->items[i]));
        }
        out->count = requested_limit;
    }
    if (gm_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK) {
        cbm_global_retrieval_result_free(out);
        return CBM_STORE_ERR;
    }
    char *observed_session = NULL, *observed_request = NULL;
    bool observed_replayed = false;
    cbm_retrieval_session_input_t session_input = {
        .request_id = session_id,
        .project_scope = current_project_uuid,
        .memory_scope = "global",
        .algorithm_version = "stage14-global-retrieval-v1",
        .config_version = 1,
        .query_text = canonical,
    };
    rc = cbm_store_memory_observe_session_begin(global->memory, &session_input, &observed_session,
                                                &observed_request, &observed_replayed);
    cbm_retrieval_candidate_observation_t *observations = NULL;
    cbm_retrieval_observation_ref_t *refs = NULL;
    if (rc == CBM_STORE_OK && out->count > 0) {
        observations = calloc((size_t)out->count, sizeof(*observations));
        refs = calloc((size_t)out->count, sizeof(*refs));
        if (!observations || !refs)
            rc = CBM_STORE_ERR;
    }
    for (int i = 0; rc == CBM_STORE_OK && i < out->count; i++) {
        observations[i].source_store_kind = "global";
        observations[i].source_store_id = out->items[i].project_uuid;
        observations[i].memory_item_id = out->items[i].item.id;
        observations[i].retrieval_source = out->items[i].item.retrieval_source;
        observations[i].source_rank = i + 1;
        observations[i].raw_score = out->items[i].item.retrieval_score;
        observations[i].normalized_score = out->items[i].global_score;
        observations[i].aggregate_rank = i + 1;
        observations[i].decision_status = "selected";
        observations[i].source_detail_json = "{\"candidate_pool\":\"global\"}";
        observations[i].evidence_json = "{\"project_role\":\"provenance_and_soft_boost\"}";
    }
    if (rc == CBM_STORE_OK && out->count > 0)
        rc = cbm_store_memory_observe_candidates(global->memory, observed_session, observations,
                                                 out->count, refs);
    if (rc == CBM_STORE_OK)
        rc = cbm_store_memory_observe_session_complete(global->memory, observed_session,
                                                       "completed", NULL);
    if (rc == CBM_STORE_OK) {
        for (int i = 0; i < out->count; i++) {
            out->items[i].candidate_id = gm_dup(refs[i].candidate_id);
            out->items[i].provenance_id = gm_dup(refs[i].provenance_id);
            out->items[i].evidence_id = gm_dup(refs[i].evidence_id);
            out->items[i].content_hash = gm_dup(refs[i].content_hash);
        }
    }
    cbm_store_memory_observation_refs_free(refs, out->count);
    free(refs);
    free(observations);
    free(observed_session);
    free(observed_request);
    if (rc != CBM_STORE_OK) {
        gm_exec(db, "ROLLBACK;");
        cbm_global_retrieval_result_free(out);
        return rc;
    }

    gm_timestamp(now);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            db,
            "INSERT INTO "
            "global_retrieval_project_context(session_id,project_uuid,soft_boost_ppm,candidate_"
            "pool,payload_sha256,created_at) VALUES(?1,?2,?3,'global',?4,?5);",
            -1, &stmt, NULL) != SQLITE_OK) {
        gm_exec(db, "ROLLBACK;");
        cbm_global_retrieval_result_free(out);
        return CBM_STORE_ERR;
    }
    gm_bind_text(stmt, 1, session_id);
    gm_bind_text(stmt, 2, current_project_uuid);
    sqlite3_bind_int(stmt, 3, soft_boost_ppm);
    gm_bind_text(stmt, 4, hash);
    gm_bind_text(stmt, 5, now);
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE || gm_exec(db, "COMMIT;") != CBM_STORE_OK) {
        gm_exec(db, "ROLLBACK;");
        cbm_global_retrieval_result_free(out);
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_global_store_retrieve(cbm_store_t *store, const char *session_id,
                              const char *current_project_uuid, int soft_boost_ppm,
                              const cbm_memory_query_t *query, cbm_global_retrieval_result_t *out) {
    if (!store)
        return CBM_STORE_ERR;
    cbm_global_memory_t view = {.memory = store, .graph = NULL};
    return cbm_global_memory_retrieve(&view, session_id, current_project_uuid, soft_boost_ppm,
                                      query, out);
}

void cbm_global_retrieval_result_free(cbm_global_retrieval_result_t *out) {
    if (!out)
        return;
    for (int i = 0; i < out->count; i++) {
        cbm_store_memory_item_free(&out->items[i].item);
        free(out->items[i].project_uuid);
        free(out->items[i].legacy_project_id);
        free(out->items[i].source_kind);
        free(out->items[i].candidate_id);
        free(out->items[i].provenance_id);
        free(out->items[i].evidence_id);
        free(out->items[i].content_hash);
    }
    free(out->items);
    free(out->session_id);
    free(out->candidate_pool);
    memset(out, 0, sizeof(*out));
}

int cbm_global_cross_project_edge(cbm_global_memory_t *global, const char *edge_id,
                                  const char *source_project_uuid, const char *target_project_uuid,
                                  const char *relation_type, int weight_ppm, int confidence_ppm,
                                  const char *status, int version, const char *evidence_event_id,
                                  const char *idempotency_key, char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    if (!global || !edge_id || !source_project_uuid || !target_project_uuid || !relation_type ||
        !status || version < 1 || !evidence_event_id || !idempotency_key || !out_report_json)
        return CBM_STORE_ERR;
    sqlite3_stmt *evidence_check = NULL;
    sqlite3 *memory_db = cbm_store_get_db(global->memory);
    if (sqlite3_prepare_v2(memory_db, "SELECT 1 FROM global_evolution_event WHERE event_id=?1;", -1,
                           &evidence_check, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(evidence_check, 1, evidence_event_id);
    int evidence_found = sqlite3_step(evidence_check);
    sqlite3_finalize(evidence_check);
    if (evidence_found != SQLITE_ROW) {
        *out_report_json = gm_report("rejected", source_project_uuid, 0);
        return evidence_found == SQLITE_DONE ? CBM_STORE_REJECTED : CBM_STORE_ERR;
    }
    char canonical[1024], hash[65];
    snprintf(canonical, sizeof(canonical), "%s\n%s\n%s\n%s\n%d\n%d\n%s\n%d\n%s", edge_id,
             source_project_uuid, target_project_uuid, relation_type, weight_ppm, confidence_ppm,
             status, version, evidence_event_id);
    cbm_stage7_sha256_hex(canonical, strlen(canonical), hash);
    sqlite3 *db = global->graph;
    sqlite3_stmt *check = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT payload_sha256 FROM global_cross_project_edge_version WHERE "
                           "edge_id=?1 AND version=?2;",
                           -1, &check, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(check, 1, edge_id);
    sqlite3_bind_int(check, 2, version);
    int found = sqlite3_step(check);
    if (found == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(check, 0);
        int exact = stored && strcmp(stored, hash) == 0;
        sqlite3_finalize(check);
        *out_report_json =
            gm_report(exact ? "replayed" : "IDEMPOTENCY_CONFLICT", source_project_uuid, 0);
        return exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(check);
    if (gm_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char now[40];
    gm_timestamp(now);
    sqlite3_stmt *stmt = NULL;
    int rc = CBM_STORE_OK;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO "
                           "global_cross_project_edge(edge_id,source_project_uuid,target_project_"
                           "uuid,relation_type,weight_ppm,confidence_ppm,status,version,updated_at)"
                           " VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON CONFLICT(edge_id) DO UPDATE SET "
                           "source_project_uuid=excluded.source_project_uuid,target_project_uuid="
                           "excluded.target_project_uuid,relation_type=excluded.relation_type,"
                           "weight_ppm=excluded.weight_ppm,confidence_ppm=excluded.confidence_ppm,"
                           "status=excluded.status,version=excluded.version,updated_at=excluded."
                           "updated_at WHERE excluded.version=global_cross_project_edge.version+1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        gm_bind_text(stmt, 1, edge_id);
        gm_bind_text(stmt, 2, source_project_uuid);
        gm_bind_text(stmt, 3, target_project_uuid);
        gm_bind_text(stmt, 4, relation_type);
        sqlite3_bind_int(stmt, 5, weight_ppm);
        sqlite3_bind_int(stmt, 6, confidence_ppm);
        gm_bind_text(stmt, 7, status);
        sqlite3_bind_int(stmt, 8, version);
        gm_bind_text(stmt, 9, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1
                 ? CBM_STORE_OK
                 : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(db,
                           "INSERT INTO "
                           "global_cross_project_edge_version(edge_id,version,payload_sha256,"
                           "evidence_event_id,created_at) VALUES(?1,?2,?3,?4,?5);",
                           -1, &stmt, NULL) == SQLITE_OK) {
        gm_bind_text(stmt, 1, edge_id);
        sqlite3_bind_int(stmt, 2, version);
        gm_bind_text(stmt, 3, hash);
        gm_bind_text(stmt, 4, evidence_event_id);
        gm_bind_text(stmt, 5, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        rc = gm_exec(db, "COMMIT;");
    else
        gm_exec(db, "ROLLBACK;");
    *out_report_json =
        gm_report(rc == CBM_STORE_OK
                      ? "recorded"
                      : (rc == CBM_STORE_IDEMPOTENCY_CONFLICT ? "IDEMPOTENCY_CONFLICT" : "failed"),
                  source_project_uuid, 0);
    return rc;
}

typedef struct {
    char quick_check[16];
    int foreign_key_violations;
    char schema_sha256[65];
    char canonical_logical_sha256[65];
    char row_counts[8192];
} gm_logical_check_t;

static int gm_file_exists(const char *path) {
#ifdef _WIN32
    if (!path)
        return 0;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0)
        return 0;
    wchar_t *wide = calloc((size_t)count, sizeof(wchar_t));
    if (!wide)
        return 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide);
        return 0;
    }
    struct _stat64 st;
    int exists = _wstat64(wide, &st) == 0;
    free(wide);
    return exists;
#else
    struct stat st;
    return path && stat(path, &st) == 0;
#endif
}

static int gm_dir_create(const char *path) {
#ifdef _WIN32
    if (!path)
        return 0;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0)
        return 0;
    wchar_t *wide = calloc((size_t)count, sizeof(wchar_t));
    if (!wide)
        return 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide);
        return 0;
    }
    int created = _wmkdir(wide) == 0;
    free(wide);
    return created;
#else
    return mkdir(path, 0700) == 0;
#endif
}

static int gm_dir_exists(const char *path) {
#ifdef _WIN32
    if (!path)
        return 0;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0)
        return 0;
    wchar_t *wide = calloc((size_t)count, sizeof(wchar_t));
    if (!wide)
        return 0;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide);
        return 0;
    }
    struct _stat64 st;
    int exists = _wstat64(wide, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
    free(wide);
    return exists;
#else
    struct stat st;
    return path && stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
#endif
}

static int gm_dir_ensure(const char *path) {
    return gm_dir_exists(path) || gm_dir_create(path);
}

static int gm_path_is_absolute(const char *path) {
    if (!path || !path[0])
        return 0;
#ifdef _WIN32
    if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
        return 1;
    if ((path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/')) {
        const char *cursor = path + 2, *server = cursor;
        while (*cursor && *cursor != '\\' && *cursor != '/')
            cursor++;
        if (cursor == server || !*cursor)
            return 0;
        cursor++;
        const char *share = cursor;
        while (*cursor && *cursor != '\\' && *cursor != '/')
            cursor++;
        return cursor > share;
    }
    return 0;
#else
    return path[0] == '/';
#endif
}

static int gm_path_format(char *out, size_t out_size, const char *format, ...) {
    if (!out || !out_size || !format)
        return CBM_STORE_ERR;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out, out_size, format, args);
    va_end(args);
    return written >= 0 && (size_t)written < out_size ? CBM_STORE_OK : CBM_STORE_ERR;
}

typedef struct {
    int exists;
    sqlite3_int64 size;
    char content_sha256[65];
#ifdef _WIN32
    HANDLE handle;
    DWORD volume_serial;
    DWORD file_index_high;
    DWORD file_index_low;
    DWORD link_count;
    FILETIME mtime;
    LONGLONG ctime;
#else
    int fd;
    dev_t device;
    ino_t inode;
    nlink_t link_count;
    struct timespec mtime;
    struct timespec ctime;
    int lock_command;
#endif
} gm_held_file_t;

static int gm_vfs_path_exists(const char *path, int *out_exists) {
    if (!path || !out_exists)
        return CBM_STORE_ERR;
    sqlite3_vfs *vfs = sqlite3_vfs_find(NULL);
    int exists = 0;
    if (!vfs || vfs->xAccess(vfs, path, SQLITE_ACCESS_EXISTS, &exists) != SQLITE_OK)
        return CBM_STORE_ERR;
    *out_exists = exists;
    return CBM_STORE_OK;
}

static int gm_held_read_all(gm_held_file_t *file, unsigned char **out_bytes) {
    if (!file || !file->exists || !out_bytes || file->size < 0 ||
        (uint64_t)file->size > (uint64_t)SIZE_MAX)
        return CBM_STORE_ERR;
    *out_bytes = NULL;
    sqlite3_uint64 allocation = (sqlite3_uint64)(file->size ? file->size : 1);
    unsigned char *bytes = sqlite3_malloc64(allocation);
    if (!bytes)
        return CBM_STORE_ERR;
    int status = CBM_STORE_OK;
#ifdef _WIN32
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    if (status == CBM_STORE_OK && !SetFilePointerEx(file->handle, zero, NULL, FILE_BEGIN))
        status = CBM_STORE_ERR;
    sqlite3_int64 offset = 0;
    while (status == CBM_STORE_OK && offset < file->size) {
        DWORD chunk = (DWORD)((file->size - offset) > 65536 ? 65536 : (file->size - offset)),
              read = 0;
        if (!ReadFile(file->handle, bytes + (size_t)offset, chunk, &read, NULL) || read != chunk)
            status = CBM_STORE_ERR;
        else
            offset += (sqlite3_int64)read;
    }
#else
    sqlite3_int64 offset = 0;
    while (status == CBM_STORE_OK && offset < file->size) {
        size_t chunk = (size_t)((file->size - offset) > 65536 ? 65536 : (file->size - offset));
        ssize_t read = pread(file->fd, bytes + (size_t)offset, chunk, (off_t)offset);
        if (read != (ssize_t)chunk)
            status = CBM_STORE_ERR;
        else
            offset += (sqlite3_int64)read;
    }
#endif
    if (status != CBM_STORE_OK) {
        sqlite3_free(bytes);
        return status;
    }
    *out_bytes = bytes;
    return CBM_STORE_OK;
}

static int gm_held_content_sha256(gm_held_file_t *file, char out_sha256[65]) {
    unsigned char *bytes = NULL;
    if (gm_held_read_all(file, &bytes) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int rc = cbm_stage7_sha256_hex(bytes, (size_t)file->size, out_sha256);
    sqlite3_free(bytes);
    return rc;
}

#ifdef _WIN32
static wchar_t *gm_utf8_to_wide(const char *path) {
    if (!path)
        return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0)
        return NULL;
    wchar_t *wide = calloc((size_t)count, sizeof(wchar_t));
    if (!wide || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count)) {
        free(wide);
        return NULL;
    }
    return wide;
}

static int gm_win_handle_matches(const gm_held_file_t *file, HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info;
    FILE_BASIC_INFO basic;
    if (!file || handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info) ||
        !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
        return 0;
    ULARGE_INTEGER size;
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    return !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
           file->volume_serial == info.dwVolumeSerialNumber &&
           file->file_index_high == info.nFileIndexHigh &&
           file->file_index_low == info.nFileIndexLow && file->link_count == info.nNumberOfLinks &&
           file->size == (sqlite3_int64)size.QuadPart &&
           file->mtime.dwLowDateTime == info.ftLastWriteTime.dwLowDateTime &&
           file->mtime.dwHighDateTime == info.ftLastWriteTime.dwHighDateTime &&
           file->ctime == basic.ChangeTime.QuadPart;
}
#else
static struct timespec gm_stat_mtime(const struct stat *st) {
#ifdef __APPLE__
    return st->st_mtimespec;
#else
    return st->st_mtim;
#endif
}

static struct timespec gm_stat_ctime(const struct stat *st) {
#ifdef __APPLE__
    return st->st_ctimespec;
#else
    return st->st_ctim;
#endif
}

static int gm_posix_stat_matches(const gm_held_file_t *file, const struct stat *st) {
    struct timespec mtime = gm_stat_mtime(st), ctime = gm_stat_ctime(st);
    return file && st && S_ISREG(st->st_mode) && file->device == st->st_dev &&
           file->inode == st->st_ino && file->link_count == st->st_nlink &&
           file->size == (sqlite3_int64)st->st_size && file->mtime.tv_sec == mtime.tv_sec &&
           file->mtime.tv_nsec == mtime.tv_nsec && file->ctime.tv_sec == ctime.tv_sec &&
           file->ctime.tv_nsec == ctime.tv_nsec;
}
#endif

static void gm_held_file_release(gm_held_file_t *file) {
    if (!file)
        return;
#ifdef _WIN32
    if (file->handle && file->handle != INVALID_HANDLE_VALUE)
        CloseHandle(file->handle);
    file->handle = INVALID_HANDLE_VALUE;
#else
    if (file->fd >= 0)
        close(file->fd);
    file->fd = -1;
#endif
}

static int gm_held_guard_active(gm_held_file_t *file) {
    if (!file || !file->exists)
        return CBM_STORE_ERR;
#ifdef _WIN32
    return gm_win_handle_matches(file, file->handle) ? CBM_STORE_OK : CBM_STORE_ERR;
#else
    struct flock lock = {0};
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(file->fd, file->lock_command, &lock) != 0)
        return CBM_STORE_ERR;
    struct stat st;
    return fstat(file->fd, &st) == 0 && gm_posix_stat_matches(file, &st) ? CBM_STORE_OK
                                                                         : CBM_STORE_ERR;
#endif
}

static int gm_held_path_matches(const char *path, const gm_held_file_t *file) {
    if (!path || !file)
        return 0;
    if (!file->exists) {
        int exists = 0;
        return gm_vfs_path_exists(path, &exists) == CBM_STORE_OK && !exists;
    }
#ifdef _WIN32
    wchar_t *wide = gm_utf8_to_wide(path);
    if (!wide)
        return 0;
    HANDLE probe = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (probe == INVALID_HANDLE_VALUE)
        return 0;
    int matches = gm_win_handle_matches(file, probe);
    if (!CloseHandle(probe))
        matches = 0;
    return matches;
#else
    struct stat st;
    return lstat(path, &st) == 0 && gm_posix_stat_matches(file, &st);
#endif
}

static int gm_held_file_acquire(const char *path, gm_held_file_t *file) {
    if (!path || !file)
        return CBM_STORE_ERR;
    memset(file, 0, sizeof(*file));
#ifdef _WIN32
    file->handle = INVALID_HANDLE_VALUE;
#else
    file->fd = -1;
#endif
    int exists = 0;
    if (gm_vfs_path_exists(path, &exists) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    file->exists = exists;
    if (!exists)
        return CBM_STORE_OK;
#ifdef _WIN32
    wchar_t *wide = gm_utf8_to_wide(path);
    if (!wide)
        return CBM_STORE_ERR;
    DWORD attributes = GetFileAttributesW(wide);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(wide);
        return CBM_STORE_ERR;
    }
    file->handle = CreateFileW(
        wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (file->handle == INVALID_HANDLE_VALUE)
        return CBM_STORE_ERR;
    BY_HANDLE_FILE_INFORMATION info;
    FILE_BASIC_INFO basic;
    if (!GetFileInformationByHandle(file->handle, &info) ||
        !GetFileInformationByHandleEx(file->handle, FileBasicInfo, &basic, sizeof(basic)) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        gm_held_file_release(file);
        return CBM_STORE_ERR;
    }
    ULARGE_INTEGER size;
    size.HighPart = info.nFileSizeHigh;
    size.LowPart = info.nFileSizeLow;
    if (size.QuadPart > (ULONGLONG)INT64_MAX) {
        gm_held_file_release(file);
        return CBM_STORE_ERR;
    }
    file->size = (sqlite3_int64)size.QuadPart;
    file->volume_serial = info.dwVolumeSerialNumber;
    file->file_index_high = info.nFileIndexHigh;
    file->file_index_low = info.nFileIndexLow;
    file->link_count = info.nNumberOfLinks;
    file->mtime = info.ftLastWriteTime;
    file->ctime = basic.ChangeTime.QuadPart;
#else
    struct stat path_stat;
    if (lstat(path, &path_stat) != 0 || S_ISLNK(path_stat.st_mode) || !S_ISREG(path_stat.st_mode))
        return CBM_STORE_ERR;
    file->fd = open(path, O_RDONLY
#ifdef O_CLOEXEC
                              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                              | O_NOFOLLOW
#endif
    );
    if (file->fd < 0)
        return CBM_STORE_ERR;
    struct stat st;
    if (fstat(file->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        gm_held_file_release(file);
        return CBM_STORE_ERR;
    }
    file->device = st.st_dev;
    file->inode = st.st_ino;
    file->link_count = st.st_nlink;
    file->size = (sqlite3_int64)st.st_size;
    file->mtime = gm_stat_mtime(&st);
    file->ctime = gm_stat_ctime(&st);
#ifdef F_OFD_SETLK
    file->lock_command = F_OFD_SETLK;
#else
    /* Classic POSIX record locks are advisory and process-scoped. The held
       descriptor plus identity/content post-checks remain mandatory. */
    file->lock_command = F_SETLK;
#endif
    struct flock lock = {0};
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(file->fd, file->lock_command, &lock) != 0) {
        gm_held_file_release(file);
        return CBM_STORE_ERR;
    }
#endif
    if (!gm_held_path_matches(path, file) ||
        gm_held_content_sha256(file, file->content_sha256) != CBM_STORE_OK ||
        gm_held_guard_active(file) != CBM_STORE_OK || !gm_held_path_matches(path, file)) {
        gm_held_file_release(file);
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static int gm_held_file_stable(const char *path, gm_held_file_t *file) {
    if (!path || !file)
        return 0;
    if (!file->exists)
        return gm_held_path_matches(path, file);
    if (gm_held_guard_active(file) != CBM_STORE_OK || !gm_held_path_matches(path, file))
        return 0;
    char hash[65];
    if (gm_held_content_sha256(file, hash) != CBM_STORE_OK || strcmp(hash, file->content_sha256))
        return 0;
    return gm_held_guard_active(file) == CBM_STORE_OK && gm_held_path_matches(path, file);
}

static int gm_sqlite_sidecars_absent(const char *path) {
    static const char *suffixes[] = {"-wal", "-shm", "-journal"};
    if (!path)
        return 0;
    size_t length = strlen(path);
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        size_t suffix_length = strlen(suffixes[i]);
        char *sidecar = malloc(length + suffix_length + 1);
        if (!sidecar)
            return 0;
        memcpy(sidecar, path, length);
        memcpy(sidecar + length, suffixes[i], suffix_length + 1);
        int exists = 0, ok = gm_vfs_path_exists(sidecar, &exists) == CBM_STORE_OK;
        free(sidecar);
        if (!ok || exists)
            return 0;
    }
    return 1;
}

static char *gm_immutable_uri_semantics(const char *path, int windows_semantics) {
    static const char suffix[] = "?immutable=1";
    static const char hex[] = "0123456789ABCDEF";
    if (!path)
        return NULL;
    size_t path_length = strlen(path);
    if (path_length > (SIZE_MAX - sizeof(suffix) - 16) / 3)
        return NULL;
    size_t capacity = 16 + path_length * 3 + sizeof(suffix);
    char *uri = malloc(capacity);
    if (!uri)
        return NULL;
    size_t used = 0;
    memcpy(uri, "file:", 5);
    used = 5;
    const unsigned char *cursor = (const unsigned char *)path;
    if (windows_semantics) {
        int unc =
            (cursor[0] == '\\' || cursor[0] == '/') && (cursor[1] == '\\' || cursor[1] == '/');
        int drive =
            ((cursor[0] >= 'A' && cursor[0] <= 'Z') || (cursor[0] >= 'a' && cursor[0] <= 'z')) &&
            cursor[1] == ':';
        if (unc) {
            uri[used++] = '/';
            uri[used++] = '/';
            cursor += 2;
        } else if (drive) {
            uri[used++] = '/';
            uri[used++] = '/';
            uri[used++] = '/';
        } else if (cursor[0] == '/' || cursor[0] == '\\') {
            uri[used++] = '/';
            uri[used++] = '/';
        }
    } else if (cursor[0] == '/') {
        uri[used++] = '/';
        uri[used++] = '/';
    }
    for (; *cursor; cursor++) {
        unsigned char value = *cursor;
        if (windows_semantics && value == '\\')
            value = '/';
        int safe = (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
                   (value >= '0' && value <= '9') || value == '/' || value == '.' || value == '-' ||
                   value == '_' || value == '~' || value == ':';
        if (safe)
            uri[used++] = (char)value;
        else {
            uri[used++] = '%';
            uri[used++] = hex[(value >> 4) & 15];
            uri[used++] = hex[value & 15];
        }
    }
    memcpy(uri + used, suffix, sizeof(suffix));
    return uri;
}

static char *gm_immutable_uri(const char *path) {
#ifdef _WIN32
    return gm_immutable_uri_semantics(path, 1);
#else
    return gm_immutable_uri_semantics(path, 0);
#endif
}

static int gm_open_immutable_readonly(const char *path, sqlite3 **out) {
    if (!path || !out)
        return CBM_STORE_ERR;
    *out = NULL;
    char *uri = gm_immutable_uri(path);
    if (!uri)
        return CBM_STORE_ERR;
    int rc = sqlite3_open_v2(uri, out, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL);
    free(uri);
    if (rc != SQLITE_OK) {
        if (*out)
            sqlite3_close(*out);
        *out = NULL;
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

int cbm_global_migration_immutable_uri_for_test(const char *path, int windows_semantics, char *out,
                                                size_t out_size) {
    char *uri = windows_semantics < 0 ? gm_immutable_uri(path)
                                      : gm_immutable_uri_semantics(path, windows_semantics);
    if (!uri || !out || strlen(uri) + 1 > out_size) {
        free(uri);
        return CBM_STORE_ERR;
    }
    memcpy(out, uri, strlen(uri) + 1);
    free(uri);
    return CBM_STORE_OK;
}

static int gm_open_held_readonly(const char *path, gm_held_file_t *held, sqlite3 **out) {
    if (!path || !held || !held->exists || !out || gm_held_guard_active(held) != CBM_STORE_OK ||
        !gm_held_path_matches(path, held))
        return CBM_STORE_ERR;
    *out = NULL;
    unsigned char *bytes = NULL;
    if (gm_held_read_all(held, &bytes) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char hash[65];
    if (cbm_stage7_sha256_hex(bytes, (size_t)held->size, hash) != CBM_STORE_OK ||
        strcmp(hash, held->content_sha256) || held->size < 100 ||
        memcmp(bytes, "SQLite format 3\000", 16) || (bytes[18] != 1 && bytes[18] != 2) ||
        (bytes[19] != 1 && bytes[19] != 2)) {
        sqlite3_free(bytes);
        return CBM_STORE_ERR;
    }
    /* sqlite3_deserialize rejects a WAL-format header. Alter only the owned
       in-memory copy; the held file bytes remain unchanged and reverified. */
    bytes[18] = 1;
    bytes[19] = 1;
    if (gm_held_guard_active(held) != CBM_STORE_OK || !gm_held_path_matches(path, held)) {
        sqlite3_free(bytes);
        return CBM_STORE_ERR;
    }
    int rc = sqlite3_open_v2(":memory:", out,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_deserialize(*out, "main", bytes, held->size, held->size,
                                 SQLITE_DESERIALIZE_READONLY | SQLITE_DESERIALIZE_FREEONCLOSE);
    else
        sqlite3_free(bytes);
    if (rc != SQLITE_OK || !*out) {
        if (*out)
            sqlite3_close(*out);
        *out = NULL;
        return CBM_STORE_ERR;
    }
    if (sqlite3_exec(*out, "PRAGMA query_only=ON;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(*out);
        *out = NULL;
        return CBM_STORE_ERR;
    }
    if (gm_held_guard_active(held) != CBM_STORE_OK || !gm_held_path_matches(path, held)) {
        sqlite3_close(*out);
        *out = NULL;
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static int gm_paths_sidecars_absent(const char *const *paths, size_t count) {
    for (size_t i = 0; i < count; i++)
        if (!gm_sqlite_sidecars_absent(paths[i]))
            return 0;
    return 1;
}

static void gm_held_set_release(gm_held_file_t *held, size_t count) {
    if (!held)
        return;
    for (size_t i = 0; i < count; i++)
        gm_held_file_release(&held[i]);
}

static int gm_held_set_acquire(const char *const *paths, size_t count, gm_held_file_t *held) {
    if (!paths || !count || !held)
        return CBM_STORE_ERR;
    memset(held, 0, count * sizeof(*held));
#ifdef _WIN32
    for (size_t i = 0; i < count; i++)
        held[i].handle = INVALID_HANDLE_VALUE;
#else
    for (size_t i = 0; i < count; i++)
        held[i].fd = -1;
#endif
    if (!gm_paths_sidecars_absent(paths, count))
        return CBM_STORE_ERR;
    for (size_t i = 0; i < count; i++)
        if (gm_held_file_acquire(paths[i], &held[i]) != CBM_STORE_OK) {
            gm_held_set_release(held, count);
            return CBM_STORE_ERR;
        }
    if (!gm_paths_sidecars_absent(paths, count)) {
        gm_held_set_release(held, count);
        return CBM_STORE_ERR;
    }
    for (size_t i = 0; i < count; i++)
        if (!gm_held_file_stable(paths[i], &held[i])) {
            gm_held_set_release(held, count);
            return CBM_STORE_ERR;
        }
    return CBM_STORE_OK;
}

static int gm_held_set_stable(const char *const *paths, size_t count, gm_held_file_t *held) {
    if (!gm_paths_sidecars_absent(paths, count))
        return 0;
    for (size_t i = 0; i < count; i++)
        if (!gm_held_file_stable(paths[i], &held[i]))
            return 0;
    return gm_paths_sidecars_absent(paths, count);
}

static int gm_held_set_close_databases_and_verify(const char *const *paths, size_t path_count,
                                                  gm_held_file_t *held, sqlite3 **databases,
                                                  size_t database_count) {
    int stable = gm_held_set_stable(paths, path_count, held);
    for (size_t i = 0; i < database_count; i++)
        if (databases[i]) {
            if (sqlite3_close(databases[i]) != SQLITE_OK)
                stable = 0;
            databases[i] = NULL;
        }
    if (!gm_held_set_stable(paths, path_count, held))
        stable = 0;
    gm_held_set_release(held, path_count);
    return stable;
}

static int gm_hash_chain(char state[65], const char *value) {
    size_t size = strlen(state) + strlen(value) + 2;
    char *joined = malloc(size);
    if (!joined)
        return CBM_STORE_ERR;
    snprintf(joined, size, "%s\n%s", state, value);
    int rc = cbm_stage7_sha256_hex(joined, strlen(joined), state);
    free(joined);
    return rc;
}

static int gm_ident_append(char *out, size_t out_size, const char *name) {
    size_t used = strlen(out), need = strlen(name) * 2 + 3;
    if (used + need >= out_size)
        return CBM_STORE_ERR;
    out[used++] = '"';
    for (const char *p = name; *p; p++) {
        if (*p == '"')
            out[used++] = '"';
        out[used++] = *p;
    }
    out[used++] = '"';
    out[used] = 0;
    return CBM_STORE_OK;
}

static int gm_table_digest(sqlite3 *db, const char *table, char logical[65], int *row_count) {
    char pragma[1024] = "PRAGMA table_info(";
    if (gm_ident_append(pragma, sizeof(pragma), table) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    strcat(pragma, ");");
    sqlite3_stmt *columns = NULL;
    if (sqlite3_prepare_v2(db, pragma, -1, &columns, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    char select[8192] = "SELECT ", order[8192] = " ORDER BY ";
    int count = 0;
    while (sqlite3_step(columns) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(columns, 1);
        if (!name)
            continue;
        if (count) {
            strcat(select, ",");
            strcat(order, ",");
        }
        strcat(select, "quote(");
        if (gm_ident_append(select, sizeof(select), name) != CBM_STORE_OK ||
            gm_ident_append(order, sizeof(order), name) != CBM_STORE_OK) {
            sqlite3_finalize(columns);
            return CBM_STORE_ERR;
        }
        strcat(select, ")");
        count++;
    }
    sqlite3_finalize(columns);
    if (count == 0)
        return CBM_STORE_ERR;
    strcat(select, " FROM ");
    if (gm_ident_append(select, sizeof(select), table) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    strcat(select, order);
    strcat(select, ";");
    sqlite3_stmt *rows = NULL;
    if (sqlite3_prepare_v2(db, select, -1, &rows, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    *row_count = 0;
    while (sqlite3_step(rows) == SQLITE_ROW) {
        char row_hash[65];
        cbm_stage7_sha256_hex(table, strlen(table), row_hash);
        for (int i = 0; i < count; i++) {
            const char *v = (const char *)sqlite3_column_text(rows, i);
            if (gm_hash_chain(row_hash, v ? v : "NULL") != CBM_STORE_OK) {
                sqlite3_finalize(rows);
                return CBM_STORE_ERR;
            }
        }
        if (gm_hash_chain(logical, row_hash) != CBM_STORE_OK) {
            sqlite3_finalize(rows);
            return CBM_STORE_ERR;
        }
        (*row_count)++;
    }
    sqlite3_finalize(rows);
    return CBM_STORE_OK;
}

static int gm_collect_db_checks(sqlite3 *db, const char *prefix, gm_logical_check_t *out,
                                int append) {
    if (!db || !out)
        return CBM_STORE_ERR;
    if (!append) {
        memset(out, 0, sizeof(*out));
        snprintf(out->quick_check, sizeof(out->quick_check), "ok");
        snprintf(out->row_counts, sizeof(out->row_counts), "{");
        cbm_stage7_sha256_hex("stage14-schema-v1", 17, out->schema_sha256);
        cbm_stage7_sha256_hex("stage14-logical-v1", 18, out->canonical_logical_sha256);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") != 0)
        snprintf(out->quick_check, sizeof(out->quick_check), "failed");
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out->foreign_key_violations++;
    sqlite3_finalize(stmt);
    stmt = NULL;
    const char *schema_sql = "SELECT type,name,COALESCE(sql,'') FROM sqlite_schema WHERE name NOT "
                             "LIKE 'sqlite_%' ORDER BY type,name;";
    if (sqlite3_prepare_v2(db, schema_sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        char row[16384];
        snprintf(row, sizeof(row), "%s\n%s\n%s\n%s", prefix, sqlite3_column_text(stmt, 0),
                 sqlite3_column_text(stmt, 1), sqlite3_column_text(stmt, 2));
        if (gm_hash_chain(out->schema_sha256, row) != CBM_STORE_OK) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    /* The migration ledger records this digest, so including its own row would
     * make target_logical_sha256 self-referential and unstable after commit. */
    if (sqlite3_prepare_v2(db,
                           "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE "
                           "'sqlite_%' AND name<>'global_migration_ledger' ORDER BY name;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int first = strlen(out->row_counts) == 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *table = (const char *)sqlite3_column_text(stmt, 0);
        int rows = 0;
        if (gm_table_digest(db, table, out->canonical_logical_sha256, &rows) != CBM_STORE_OK) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        size_t used = strlen(out->row_counts);
        int n = snprintf(out->row_counts + used, sizeof(out->row_counts) - used, "%s\"%s.%s\":%d",
                         first ? "" : ",", prefix, table, rows);
        if (n < 0 || (size_t)n >= sizeof(out->row_counts) - used) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        first = 0;
    }
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}

static int gm_collect_triple_checks(sqlite3 *memory, sqlite3 *graph, sqlite3 *config,
                                    gm_logical_check_t *out) {
    if (gm_collect_db_checks(memory, "memory", out, 0) != CBM_STORE_OK ||
        gm_collect_db_checks(graph, "graph", out, 1) != CBM_STORE_OK ||
        gm_collect_db_checks(config, "config", out, 1) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    size_t used = strlen(out->row_counts);
    if (used + 2 > sizeof(out->row_counts))
        return CBM_STORE_ERR;
    strcat(out->row_counts, "}");
    return CBM_STORE_OK;
}

static int gm_collect_db_projection(sqlite3 *candidate, sqlite3 *reference, const char *prefix,
                                    gm_logical_check_t *out, int append) {
    if (!candidate || !reference || !out)
        return CBM_STORE_ERR;
    if (!append) {
        memset(out, 0, sizeof(*out));
        snprintf(out->quick_check, sizeof(out->quick_check), "ok");
        snprintf(out->row_counts, sizeof(out->row_counts), "{");
        cbm_stage7_sha256_hex("stage14-schema-v1", 17, out->schema_sha256);
        cbm_stage7_sha256_hex("stage14-logical-v1", 18, out->canonical_logical_sha256);
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(candidate, "PRAGMA quick_check;", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW ||
        strcmp((const char *)sqlite3_column_text(stmt, 0), "ok") != 0)
        snprintf(out->quick_check, sizeof(out->quick_check), "failed");
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(candidate, "PRAGMA foreign_key_check;", -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out->foreign_key_violations++;
    sqlite3_finalize(stmt);
    stmt = NULL;
    sqlite3_stmt *objects = NULL, *lookup = NULL;
    if (sqlite3_prepare_v2(reference,
                           "SELECT type,name FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%' "
                           "ORDER BY type,name;",
                           -1, &objects, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(
            candidate,
            "SELECT type,name,COALESCE(sql,'') FROM sqlite_schema WHERE type=?1 AND name=?2;", -1,
            &lookup, NULL) != SQLITE_OK) {
        sqlite3_finalize(objects);
        sqlite3_finalize(lookup);
        return CBM_STORE_ERR;
    }
    while (sqlite3_step(objects) == SQLITE_ROW) {
        const char *type = (const char *)sqlite3_column_text(objects, 0),
                   *name = (const char *)sqlite3_column_text(objects, 1);
        sqlite3_reset(lookup);
        sqlite3_clear_bindings(lookup);
        gm_bind_text(lookup, 1, type);
        gm_bind_text(lookup, 2, name);
        if (sqlite3_step(lookup) != SQLITE_ROW) {
            sqlite3_finalize(objects);
            sqlite3_finalize(lookup);
            return CBM_STORE_ERR;
        }
        char row[16384];
        snprintf(row, sizeof(row), "%s\n%s\n%s\n%s", prefix, sqlite3_column_text(lookup, 0),
                 sqlite3_column_text(lookup, 1), sqlite3_column_text(lookup, 2));
        if (gm_hash_chain(out->schema_sha256, row) != CBM_STORE_OK) {
            sqlite3_finalize(objects);
            sqlite3_finalize(lookup);
            return CBM_STORE_ERR;
        }
    }
    sqlite3_finalize(objects);
    sqlite3_finalize(lookup);
    stmt = NULL;
    if (sqlite3_prepare_v2(reference,
                           "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE "
                           "'sqlite_%' ORDER BY name;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int first = strlen(out->row_counts) == 1;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *table = (const char *)sqlite3_column_text(stmt, 0);
        int rows = 0;
        if (gm_table_digest(candidate, table, out->canonical_logical_sha256, &rows) !=
            CBM_STORE_OK) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        size_t used = strlen(out->row_counts);
        int n = snprintf(out->row_counts + used, sizeof(out->row_counts) - used, "%s\"%s.%s\":%d",
                         first ? "" : ",", prefix, table, rows);
        if (n < 0 || (size_t)n >= sizeof(out->row_counts) - used) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        first = 0;
    }
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}

static int gm_collect_triple_projection(sqlite3 *memory, sqlite3 *graph, sqlite3 *config,
                                        sqlite3 *source_memory, sqlite3 *source_graph,
                                        sqlite3 *source_config, gm_logical_check_t *out) {
    if (gm_collect_db_projection(memory, source_memory, "memory", out, 0) != CBM_STORE_OK ||
        gm_collect_db_projection(graph, source_graph, "graph", out, 1) != CBM_STORE_OK ||
        gm_collect_db_projection(config, source_config, "config", out, 1) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    size_t used = strlen(out->row_counts);
    if (used + 2 > sizeof(out->row_counts))
        return CBM_STORE_ERR;
    strcat(out->row_counts, "}");
    return CBM_STORE_OK;
}

#define GM_BACKUP_MAX_STEP_ATTEMPTS 4096
#define GM_BACKUP_MAX_ELAPSED_MS 2000
#define GM_BACKUP_RETRY_SLEEP_MS 10

typedef void (*gm_migration_test_hook_fn)(const char *phase, const char *const *target_paths,
                                          size_t target_path_count, void *context);

static gm_migration_test_hook_fn gm_migration_test_hook = NULL;
static void *gm_migration_test_hook_context = NULL;

void cbm_global_migration_set_test_hook_for_test(gm_migration_test_hook_fn hook, void *context) {
    gm_migration_test_hook = hook;
    gm_migration_test_hook_context = context;
}

int cbm_global_migration_backup_limits_for_test(int *max_attempts, uint64_t *max_elapsed_ms) {
    if (!max_attempts || !max_elapsed_ms)
        return CBM_STORE_ERR;
    *max_attempts = GM_BACKUP_MAX_STEP_ATTEMPTS;
    *max_elapsed_ms = GM_BACKUP_MAX_ELAPSED_MS;
    return CBM_STORE_OK;
}

static void gm_migration_invoke_test_hook(const char *phase, const char *const *target_paths,
                                          size_t target_path_count) {
    gm_migration_test_hook_fn hook = gm_migration_test_hook;
    void *context = gm_migration_test_hook_context;
    if (hook)
        hook(phase, target_paths, target_path_count, context);
}

typedef struct {
    unsigned char *bytes;
    sqlite3_int64 size;
    char sha256[65];
} gm_serialized_image_t;

static void gm_serialized_image_reset(gm_serialized_image_t *image) {
    if (!image)
        return;
    sqlite3_free(image->bytes);
    memset(image, 0, sizeof(*image));
}

static uint64_t gm_monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
#endif
}

static int gm_serialize_database(sqlite3 *db, gm_serialized_image_t *out) {
    if (!db || !out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    sqlite3_int64 size = 0;
    unsigned char *bytes = sqlite3_serialize(db, "main", &size, 0);
    if (!bytes || size < 100 || (uint64_t)size > (uint64_t)SIZE_MAX ||
        memcmp(bytes, "SQLite format 3\000", 16) || (bytes[18] != 1 && bytes[18] != 2) ||
        (bytes[19] != 1 && bytes[19] != 2)) {
        sqlite3_free(bytes);
        return CBM_STORE_ERR;
    }
    /* sqlite3_backup materializes a transactionally complete snapshot,
       including committed WAL pages, into the private in-memory destination.
       sqlite3_serialize(flags=0) returns an owned copy. Convert only that owned
       image's read/write format bytes to rollback mode so the published main
       file never depends on an unpublished -wal sidecar. */
    bytes[18] = 1;
    bytes[19] = 1;
    char sha256[65];
    if (cbm_stage7_sha256_hex(bytes, (size_t)size, sha256) != CBM_STORE_OK) {
        sqlite3_free(bytes);
        return CBM_STORE_ERR;
    }
    out->bytes = bytes;
    out->size = size;
    memcpy(out->sha256, sha256, sizeof(out->sha256));
    return CBM_STORE_OK;
}

static int gm_backup_to_image(sqlite3 *source, gm_serialized_image_t *out) {
    if (!source || !out)
        return CBM_STORE_ERR;
    sqlite3 *target = NULL;
    if (sqlite3_open_v2(":memory:", &target,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY,
                        NULL) != SQLITE_OK) {
        if (target)
            sqlite3_close(target);
        return CBM_STORE_ERR;
    }
    sqlite3_backup *backup = sqlite3_backup_init(target, "main", source, "main");
    int status = CBM_STORE_ERR, step = SQLITE_ERROR;
    uint64_t started = gm_monotonic_ms();
    if (backup) {
        for (int attempt = 0; attempt < GM_BACKUP_MAX_STEP_ATTEMPTS; attempt++) {
            uint64_t now = gm_monotonic_ms();
            if (attempt > 0 && now >= started && now - started >= GM_BACKUP_MAX_ELAPSED_MS)
                break;
            step = sqlite3_backup_step(backup, 256);
            if (step == SQLITE_DONE) {
                status = CBM_STORE_OK;
                break;
            }
            if (step == SQLITE_OK)
                continue;
            if (step == SQLITE_BUSY || step == SQLITE_LOCKED) {
                sqlite3_sleep(GM_BACKUP_RETRY_SLEEP_MS);
                continue;
            }
            break;
        }
        if (sqlite3_backup_finish(backup) != SQLITE_OK)
            status = CBM_STORE_ERR;
    }
    if (status == CBM_STORE_OK)
        status = gm_serialize_database(target, out);
    if (sqlite3_close(target) != SQLITE_OK)
        status = CBM_STORE_ERR;
    if (status != CBM_STORE_OK)
        gm_serialized_image_reset(out);
    return status;
}

static int gm_global_graph_image(gm_serialized_image_t *out) {
    if (!out)
        return CBM_STORE_ERR;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(":memory:", &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY,
                        NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return CBM_STORE_ERR;
    }
    int rc = gm_exec(db, graph_schema_sql);
    if (rc == CBM_STORE_OK)
        rc = gm_serialize_database(db, out);
    if (sqlite3_close(db) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    if (rc != CBM_STORE_OK)
        gm_serialized_image_reset(out);
    return rc;
}

typedef struct {
    int created;
    gm_held_file_t held;
#ifndef _WIN32
    int parent_fd;
    char *leaf;
#endif
} gm_created_file_t;

static void gm_created_file_init(gm_created_file_t *file) {
    if (!file)
        return;
    memset(file, 0, sizeof(*file));
#ifdef _WIN32
    file->held.handle = INVALID_HANDLE_VALUE;
#else
    file->held.fd = -1;
    file->parent_fd = -1;
#endif
}

#ifdef _WIN32
static int gm_win_directory_safe(const wchar_t *path) {
    DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

static int gm_win_directory_chain_safe(const wchar_t *parent) {
    if (!parent || !parent[0])
        return 0;
    size_t length = wcslen(parent), root = 0;
    if (length >= 3 && parent[1] == L':' && parent[2] == L'\\')
        root = 3;
    else if (length >= 5 && parent[0] == L'\\' && parent[1] == L'\\') {
        size_t cursor = 2;
        while (cursor < length && parent[cursor] != L'\\')
            cursor++;
        if (cursor == 2 || cursor >= length)
            return 0;
        cursor++;
        size_t share = cursor;
        while (cursor < length && parent[cursor] != L'\\')
            cursor++;
        if (cursor == share)
            return 0;
        root = cursor < length ? cursor + 1 : cursor;
    } else
        return 0;
    wchar_t *probe = _wcsdup(parent);
    if (!probe)
        return 0;
    int safe = 1;
    for (size_t cursor = root; safe && cursor < length; cursor++) {
        if (probe[cursor] != L'\\')
            continue;
        wchar_t saved = probe[cursor];
        probe[cursor] = 0;
        safe = gm_win_directory_safe(probe);
        probe[cursor] = saved;
    }
    if (safe)
        safe = gm_win_directory_safe(parent);
    free(probe);
    return safe;
}

static int gm_win_handle_same_identity(HANDLE left, HANDLE right, int require_directory) {
    BY_HANDLE_FILE_INFORMATION a, b;
    if (left == INVALID_HANDLE_VALUE || right == INVALID_HANDLE_VALUE ||
        !GetFileInformationByHandle(left, &a) || !GetFileInformationByHandle(right, &b))
        return 0;
    if (require_directory && (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                              !(b.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)))
        return 0;
    if ((a.dwFileAttributes | b.dwFileAttributes) & FILE_ATTRIBUTE_REPARSE_POINT)
        return 0;
    return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
           a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

static int gm_win_prepare_create_path(const char *path, wchar_t **out_full, wchar_t **out_parent,
                                      HANDLE *out_parent_handle) {
    if (!path || !out_full || !out_parent || !out_parent_handle)
        return CBM_STORE_ERR;
    *out_full = NULL;
    *out_parent = NULL;
    *out_parent_handle = INVALID_HANDLE_VALUE;
    wchar_t *input = gm_utf8_to_wide(path);
    if (!input)
        return CBM_STORE_ERR;
    DWORD needed = GetFullPathNameW(input, 0, NULL, NULL);
    if (!needed) {
        free(input);
        return CBM_STORE_ERR;
    }
    wchar_t *full = calloc((size_t)needed + 2, sizeof(wchar_t));
    if (!full) {
        free(input);
        return CBM_STORE_ERR;
    }
    DWORD written = GetFullPathNameW(input, needed + 1, full, NULL);
    free(input);
    if (!written || written > needed) {
        free(full);
        return CBM_STORE_ERR;
    }
    for (wchar_t *cursor = full; *cursor; cursor++)
        if (*cursor == L'/')
            *cursor = L'\\';
    size_t length = wcslen(full), last = length;
    while (last > 0 && full[last - 1] != L'\\')
        last--;
    if (last == 0 || last >= length) {
        free(full);
        return CBM_STORE_ERR;
    }
    size_t leaf_start = last;
    size_t leaf_length = length - leaf_start;
    if ((leaf_length == 1 && full[leaf_start] == L'.') ||
        (leaf_length == 2 && full[leaf_start] == L'.' && full[leaf_start + 1] == L'.')) {
        free(full);
        return CBM_STORE_ERR;
    }
    size_t separator = last - 1, parent_length = separator;
    if (separator == 2 && full[1] == L':')
        parent_length = 3;
    else if (separator > 1 && full[0] == L'\\' && full[1] == L'\\') {
        size_t slash_count = 0;
        for (size_t i = 2; i <= separator; i++)
            if (full[i] == L'\\')
                slash_count++;
        if (slash_count == 1)
            parent_length = separator + 1;
    }
    wchar_t *parent = calloc(parent_length + 1, sizeof(wchar_t));
    if (!parent) {
        free(full);
        return CBM_STORE_ERR;
    }
    memcpy(parent, full, parent_length * sizeof(wchar_t));
    if (!gm_win_directory_chain_safe(parent)) {
        free(parent);
        free(full);
        return CBM_STORE_REJECTED;
    }
    HANDLE parent_handle =
        CreateFileW(parent, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (parent_handle == INVALID_HANDLE_VALUE) {
        free(parent);
        free(full);
        return CBM_STORE_ERR;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(parent_handle, &info) ||
        !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(parent_handle);
        free(parent);
        free(full);
        return CBM_STORE_REJECTED;
    }
    *out_full = full;
    *out_parent = parent;
    *out_parent_handle = parent_handle;
    return CBM_STORE_OK;
}

static int gm_win_parent_still_bound(const wchar_t *parent, HANDLE held_parent) {
    HANDLE probe =
        CreateFileW(parent, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (probe == INVALID_HANDLE_VALUE)
        return 0;
    int same = gm_win_handle_same_identity(held_parent, probe, 1);
    if (!CloseHandle(probe))
        same = 0;
    return same;
}

static int gm_win_created_path_matches(const char *path, const gm_held_file_t *held) {
    wchar_t *wide = gm_utf8_to_wide(path);
    if (!wide)
        return 0;
    HANDLE probe =
        CreateFileW(wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (probe == INVALID_HANDLE_VALUE)
        return 0;
    int matches = gm_win_handle_matches(held, probe);
    if (!CloseHandle(probe))
        matches = 0;
    return matches;
}
#else
static int gm_posix_open_parent(const char *path, int *out_parent_fd, char **out_leaf) {
    if (!path || path[0] != '/' || !out_parent_fd || !out_leaf)
        return CBM_STORE_ERR;
    *out_parent_fd = -1;
    *out_leaf = NULL;
    char *copy = gm_dup(path);
    if (!copy)
        return CBM_STORE_ERR;
    char *last = strrchr(copy, '/');
    if (!last || !last[1]) {
        free(copy);
        return CBM_STORE_ERR;
    }
    char *leaf = gm_dup(last + 1);
    if (!leaf) {
        free(copy);
        return CBM_STORE_ERR;
    }
    if (!strcmp(leaf, ".") || !strcmp(leaf, "..")) {
        free(leaf);
        free(copy);
        return CBM_STORE_REJECTED;
    }
    *last = 0;
    int current = open("/", O_RDONLY
#ifdef O_DIRECTORY
                                | O_DIRECTORY
#endif
#ifdef O_CLOEXEC
                                | O_CLOEXEC
#endif
    );
    if (current < 0) {
        free(leaf);
        free(copy);
        return CBM_STORE_ERR;
    }
    char *cursor = last == copy ? copy : copy + 1;
    while (*cursor) {
        char *separator = strchr(cursor, '/');
        if (separator)
            *separator = 0;
        if (!cursor[0] || !strcmp(cursor, ".") || !strcmp(cursor, "..")) {
            close(current);
            free(leaf);
            free(copy);
            return CBM_STORE_REJECTED;
        }
        int next = openat(current, cursor,
                          O_RDONLY
#ifdef O_DIRECTORY
                              | O_DIRECTORY
#endif
#ifdef O_CLOEXEC
                              | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                              | O_NOFOLLOW
#endif
        );
        close(current);
        if (next < 0) {
            free(leaf);
            free(copy);
            return CBM_STORE_REJECTED;
        }
        current = next;
        if (!separator)
            break;
        *separator = '/';
        cursor = separator + 1;
    }
    free(copy);
    *out_parent_fd = current;
    *out_leaf = leaf;
    return CBM_STORE_OK;
}
#endif

static int gm_atomic_create_image(const char *path, const gm_serialized_image_t *image,
                                  gm_created_file_t *created, int *collision) {
    if (!path || !image || !image->bytes || image->size < 100 || !created || !collision)
        return CBM_STORE_ERR;
    *collision = 0;
    gm_created_file_init(created);
#ifdef _WIN32
    wchar_t *full = NULL, *parent = NULL;
    HANDLE parent_handle = INVALID_HANDLE_VALUE;
    int prepare = gm_win_prepare_create_path(path, &full, &parent, &parent_handle);
    if (prepare != CBM_STORE_OK)
        return prepare;
    HANDLE handle =
        CreateFileW(full, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_WRITE_THROUGH |
                        FILE_FLAG_OPEN_REPARSE_POINT,
                    NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        *collision = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS;
        CloseHandle(parent_handle);
        free(parent);
        free(full);
        return *collision ? CBM_STORE_REJECTED : CBM_STORE_ERR;
    }
    created->created = 1;
    created->held.exists = 1;
    created->held.handle = handle;
    sqlite3_int64 offset = 0;
    int rc = CBM_STORE_OK;
    while (rc == CBM_STORE_OK && offset < image->size) {
        DWORD chunk = (DWORD)((image->size - offset) > 1048576 ? 1048576 : (image->size - offset));
        DWORD written = 0;
        if (!WriteFile(handle, image->bytes + (size_t)offset, chunk, &written, NULL) ||
            written != chunk)
            rc = CBM_STORE_ERR;
        else
            offset += (sqlite3_int64)written;
    }
    if (rc == CBM_STORE_OK && !FlushFileBuffers(handle))
        rc = CBM_STORE_ERR;
    BY_HANDLE_FILE_INFORMATION info;
    FILE_BASIC_INFO basic;
    if (rc == CBM_STORE_OK &&
        (!GetFileInformationByHandle(handle, &info) ||
         !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
         (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))))
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        ULARGE_INTEGER size;
        size.HighPart = info.nFileSizeHigh;
        size.LowPart = info.nFileSizeLow;
        created->held.size = (sqlite3_int64)size.QuadPart;
        created->held.volume_serial = info.dwVolumeSerialNumber;
        created->held.file_index_high = info.nFileIndexHigh;
        created->held.file_index_low = info.nFileIndexLow;
        created->held.link_count = info.nNumberOfLinks;
        created->held.mtime = info.ftLastWriteTime;
        created->held.ctime = basic.ChangeTime.QuadPart;
        if (created->held.size != image->size ||
            gm_held_content_sha256(&created->held, created->held.content_sha256) != CBM_STORE_OK ||
            strcmp(created->held.content_sha256, image->sha256) ||
            !gm_win_handle_matches(&created->held, handle) ||
            !gm_win_created_path_matches(path, &created->held) ||
            !gm_win_parent_still_bound(parent, parent_handle) ||
            !gm_win_directory_chain_safe(parent))
            rc = CBM_STORE_ERR;
    }
    if (!CloseHandle(parent_handle))
        rc = CBM_STORE_ERR;
    free(parent);
    free(full);
    return rc;
#else
    int parent_fd = -1;
    char *leaf = NULL;
    int prepare = gm_posix_open_parent(path, &parent_fd, &leaf);
    if (prepare != CBM_STORE_OK)
        return prepare;
    int fd = openat(parent_fd, leaf,
                    O_RDWR | O_CREAT | O_EXCL
#ifdef O_CLOEXEC
                        | O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
                        | O_NOFOLLOW
#endif
                    ,
                    0600);
    if (fd < 0) {
        *collision = errno == EEXIST;
        close(parent_fd);
        free(leaf);
        return *collision ? CBM_STORE_REJECTED : CBM_STORE_ERR;
    }
    created->created = 1;
    created->held.exists = 1;
    created->held.fd = fd;
    created->parent_fd = parent_fd;
    created->leaf = leaf;
    sqlite3_int64 offset = 0;
    int rc = CBM_STORE_OK;
    while (rc == CBM_STORE_OK && offset < image->size) {
        size_t chunk =
            (size_t)((image->size - offset) > 1048576 ? 1048576 : (image->size - offset));
        ssize_t written = pwrite(fd, image->bytes + (size_t)offset, chunk, (off_t)offset);
        if (written != (ssize_t)chunk)
            rc = CBM_STORE_ERR;
        else
            offset += (sqlite3_int64)written;
    }
    if (rc == CBM_STORE_OK && (fsync(fd) != 0 || fsync(parent_fd) != 0))
        rc = CBM_STORE_ERR;
    struct stat st, path_st;
#ifdef AT_SYMLINK_NOFOLLOW
    int path_stat_flags = AT_SYMLINK_NOFOLLOW;
#else
    int path_stat_flags = 0;
#endif
    if (rc == CBM_STORE_OK &&
        (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != image->size ||
         fstatat(parent_fd, leaf, &path_st, path_stat_flags) != 0 ||
         !gm_posix_stat_matches(&(gm_held_file_t){.exists = 1,
                                                  .size = (sqlite3_int64)st.st_size,
                                                  .device = st.st_dev,
                                                  .inode = st.st_ino,
                                                  .link_count = st.st_nlink,
                                                  .mtime = gm_stat_mtime(&st),
                                                  .ctime = gm_stat_ctime(&st)},
                                &path_st)))
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        created->held.size = (sqlite3_int64)st.st_size;
        created->held.device = st.st_dev;
        created->held.inode = st.st_ino;
        created->held.link_count = st.st_nlink;
        created->held.mtime = gm_stat_mtime(&st);
        created->held.ctime = gm_stat_ctime(&st);
        if (gm_held_content_sha256(&created->held, created->held.content_sha256) != CBM_STORE_OK ||
            strcmp(created->held.content_sha256, image->sha256))
            rc = CBM_STORE_ERR;
    }
    return rc;
#endif
}

static void gm_created_file_release(gm_created_file_t *file) {
    if (!file)
        return;
    gm_held_file_release(&file->held);
#ifndef _WIN32
    if (file->parent_fd >= 0)
        close(file->parent_fd);
    file->parent_fd = -1;
    free(file->leaf);
    file->leaf = NULL;
#endif
}

static int gm_created_file_cleanup(const char *path, gm_created_file_t *file) {
    if (!path || !file || !file->created)
        return CBM_STORE_OK;
#ifdef _WIN32
    if (!gm_win_handle_matches(&file->held, file->held.handle) ||
        !gm_win_created_path_matches(path, &file->held)) {
        gm_created_file_release(file);
        return CBM_STORE_ERR;
    }
    char sha256[65];
    if (gm_held_content_sha256(&file->held, sha256) != CBM_STORE_OK ||
        strcmp(sha256, file->held.content_sha256)) {
        gm_created_file_release(file);
        return CBM_STORE_ERR;
    }
    FILE_DISPOSITION_INFO disposition = {0};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file->held.handle, FileDispositionInfo, &disposition,
                                    sizeof(disposition))) {
        gm_created_file_release(file);
        return CBM_STORE_ERR;
    }
    gm_created_file_release(file);
    int exists = 0;
    return gm_vfs_path_exists(path, &exists) == CBM_STORE_OK && !exists ? CBM_STORE_OK
                                                                        : CBM_STORE_ERR;
#else
    /* POSIX has no portable unlink-by-open-file operation. A pathname unlink
       could remove a concurrently substituted opponent, so preserve the
       partial file and fail closed rather than perform an unsafe cleanup. */
    gm_created_file_release(file);
    return CBM_STORE_ERR;
#endif
}

static int gm_publish_images_atomic(const char *const *paths, gm_serialized_image_t *images,
                                    size_t count) {
    if (!paths || !images || count == 0)
        return CBM_STORE_ERR;
    gm_created_file_t *created = calloc(count, sizeof(*created));
    if (!created)
        return CBM_STORE_ERR;
    for (size_t i = 0; i < count; i++)
        gm_created_file_init(&created[i]);
    int rc = gm_paths_sidecars_absent(paths, count) ? CBM_STORE_OK : CBM_STORE_REJECTED;
    size_t attempted = 0;
    for (size_t i = 0; rc == CBM_STORE_OK && i < count; i++) {
        int collision = 0;
        attempted = i + 1;
        rc = gm_atomic_create_image(paths[i], &images[i], &created[i], &collision);
        if (rc == CBM_STORE_OK && !gm_paths_sidecars_absent(paths, count))
            rc = CBM_STORE_REJECTED;
    }
    if (rc != CBM_STORE_OK) {
        int cleanup = CBM_STORE_OK;
        for (size_t i = attempted; i > 0; i--)
            if (created[i - 1].created &&
                gm_created_file_cleanup(paths[i - 1], &created[i - 1]) != CBM_STORE_OK)
                cleanup = CBM_STORE_ERR;
        for (size_t i = attempted; i < count; i++)
            gm_created_file_release(&created[i]);
        free(created);
        return cleanup == CBM_STORE_OK ? rc : CBM_STORE_ERR;
    }
    for (size_t i = 0; i < count; i++)
        gm_created_file_release(&created[i]);
    free(created);
    return gm_paths_sidecars_absent(paths, count) ? CBM_STORE_OK : CBM_STORE_REJECTED;
}

static int gm_path_move_no_replace(const char *from, const char *to) {
    if (!from || !to)
        return CBM_STORE_ERR;
#ifdef _WIN32
    wchar_t *from_wide = gm_utf8_to_wide(from), *to_wide = gm_utf8_to_wide(to);
    if (!from_wide || !to_wide) {
        free(from_wide);
        free(to_wide);
        return CBM_STORE_ERR;
    }
    int rc = MoveFileExW(from_wide, to_wide, MOVEFILE_WRITE_THROUGH) ? CBM_STORE_OK : CBM_STORE_ERR;
    free(from_wide);
    free(to_wide);
    return rc;
#else
    return rename(from, to) == 0 ? CBM_STORE_OK : CBM_STORE_ERR;
#endif
}

static int gm_path_remove_file(const char *path) {
    if (!path)
        return CBM_STORE_ERR;
#ifdef _WIN32
    wchar_t *wide = gm_utf8_to_wide(path);
    if (!wide)
        return CBM_STORE_ERR;
    int rc = DeleteFileW(wide)
                 ? CBM_STORE_OK
                 : (GetLastError() == ERROR_FILE_NOT_FOUND ? CBM_STORE_OK : CBM_STORE_ERR);
    free(wide);
    return rc;
#else
    return unlink(path) == 0 || errno == ENOENT ? CBM_STORE_OK : CBM_STORE_ERR;
#endif
}

/* Stage14 adoption helpers.
 *
 * gm_migration_adopt_bootstrap() below implements the adoption path only under
 * _WIN32; the POSIX branch is a stub that returns CBM_STORE_REJECTED. These
 * helpers are therefore referenced on Windows only. They are kept compiled on
 * every platform — rather than hidden behind #ifdef _WIN32 — so their POSIX
 * bodies stay syntax-checked and are ready for the day the POSIX adoption path
 * is implemented. CBM_MAYBE_UNUSED keeps -Werror=unused-function quiet in the
 * meantime. */
static CBM_MAYBE_UNUSED int gm_path_remove_empty_directory(const char *path) {
    if (!path)
        return CBM_STORE_ERR;
#ifdef _WIN32
    wchar_t *wide = gm_utf8_to_wide(path);
    if (!wide)
        return CBM_STORE_ERR;
    int rc = RemoveDirectoryW(wide)
                 ? CBM_STORE_OK
                 : (GetLastError() == ERROR_PATH_NOT_FOUND ? CBM_STORE_OK : CBM_STORE_ERR);
    free(wide);
    return rc;
#else
    return rmdir(path) == 0 || errno == ENOENT ? CBM_STORE_OK : CBM_STORE_ERR;
#endif
}

typedef struct {
    char path[4096];
    char backup_path[4096];
    int backup_moved;
} gm_adoption_replacement_t;

static CBM_MAYBE_UNUSED int gm_adoption_replace_existing(const char *path,
                                                         const gm_serialized_image_t *image,
                                                         size_t ordinal,
                                                         gm_adoption_replacement_t *replacement) {
    if (!path || !image || !image->bytes || !replacement)
        return CBM_STORE_ERR;
    memset(replacement, 0, sizeof(*replacement));
    if (gm_path_format(replacement->path, sizeof(replacement->path), "%s", path) != CBM_STORE_OK ||
        gm_path_format(replacement->backup_path, sizeof(replacement->backup_path),
                       "%s.stage14-adoption-backup-%zu", path, ordinal) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char temp_path[4096];
    if (gm_path_format(temp_path, sizeof(temp_path), "%s.stage14-adoption-temp-%zu", path,
                       ordinal) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (gm_file_exists(replacement->backup_path) || gm_file_exists(temp_path))
        return CBM_STORE_REJECTED;
    gm_created_file_t temp;
    gm_created_file_init(&temp);
    int collision = 0;
    int rc = gm_atomic_create_image(temp_path, image, &temp, &collision);
    if (rc != CBM_STORE_OK) {
        gm_created_file_release(&temp);
        return rc;
    }
    gm_created_file_release(&temp);
    if (gm_path_move_no_replace(path, replacement->backup_path) != CBM_STORE_OK) {
        gm_path_remove_file(temp_path);
        return CBM_STORE_ERR;
    }
    replacement->backup_moved = 1;
    if (gm_path_move_no_replace(temp_path, path) != CBM_STORE_OK) {
        gm_path_remove_file(temp_path);
        gm_path_move_no_replace(replacement->backup_path, path);
        replacement->backup_moved = 0;
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static CBM_MAYBE_UNUSED int gm_adoption_restore(gm_adoption_replacement_t *replacements,
                                                size_t count) {
    int rc = CBM_STORE_OK;
    for (size_t i = count; i > 0; i--) {
        gm_adoption_replacement_t *replacement = &replacements[i - 1];
        if (!replacement->backup_moved)
            continue;
        if (gm_path_remove_file(replacement->path) != CBM_STORE_OK ||
            gm_path_move_no_replace(replacement->backup_path, replacement->path) != CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        else
            replacement->backup_moved = 0;
    }
    return rc;
}

static CBM_MAYBE_UNUSED int gm_adoption_commit(gm_adoption_replacement_t *replacements,
                                               size_t count) {
    int rc = CBM_STORE_OK;
    for (size_t i = 0; i < count; i++)
        if (replacements[i].backup_moved) {
            if (gm_path_remove_file(replacements[i].backup_path) != CBM_STORE_OK)
                rc = CBM_STORE_ERR;
            else
                replacements[i].backup_moved = 0;
        }
    return rc;
}

static int gm_table_row_count(sqlite3 *db, const char *table, int *out_count) {
    if (!db || !table || !out_count)
        return CBM_STORE_ERR;
    char sql[1024];
    if (gm_path_format(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\";", table) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        *out_count = sqlite3_column_int(stmt, 0);
        rc = SQLITE_OK;
    } else
        rc = SQLITE_ERROR;
    sqlite3_finalize(stmt);
    return rc == SQLITE_OK ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int gm_bootstrap_memory_is_empty(sqlite3 *db) {
    static const char *required[] = {"global_project_catalog", "global_project_alias",
                                     "global_migration_ledger", "global_legacy_alias",
                                     "stage14_component_ledger"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        char sql[256];
        if (gm_path_format(sql, sizeof(sql),
                           "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%s';",
                           required[i]) != CBM_STORE_OK ||
            gm_scalar_int(db, sql) != 1)
            return 0;
    }
    sqlite3_stmt *tables = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%';",
            -1, &tables, NULL) != SQLITE_OK)
        return 0;
    int valid = 1;
    while (valid && sqlite3_step(tables) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(tables, 0);
        int rows = 0;
        if (gm_table_row_count(db, name, &rows) != CBM_STORE_OK) {
            valid = 0;
            break;
        }
        if (rows == 0)
            continue;
        int allowed = !strcmp(name, "memory_fts_config") || !strcmp(name, "memory_fts_data") ||
                      !strcmp(name, "nodes_fts_config") || !strcmp(name, "nodes_fts_data") ||
                      !strcmp(name, "stage5_schema_migrations") ||
                      (!strncmp(name, "stage", 5) && strstr(name, "component_ledger"));
        if (!allowed)
            valid = 0;
    }
    sqlite3_finalize(tables);
    return valid;
}

static int gm_bootstrap_graph_is_empty(sqlite3 *db) {
    static const char *required[] = {"global_project_node", "global_cross_project_edge",
                                     "global_cross_project_edge_version"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        char sql[256];
        if (gm_path_format(sql, sizeof(sql),
                           "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='%s';",
                           required[i]) != CBM_STORE_OK ||
            gm_scalar_int(db, sql) != 1)
            return 0;
    }
    sqlite3_stmt *tables = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%';",
            -1, &tables, NULL) != SQLITE_OK)
        return 0;
    int valid = 1;
    while (valid && sqlite3_step(tables) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(tables, 0);
        int rows = 0;
        if (gm_table_row_count(db, name, &rows) != CBM_STORE_OK || rows != 0)
            valid = 0;
    }
    sqlite3_finalize(tables);
    return valid;
}

static int gm_bootstrap_config_is_empty(sqlite3 *db) {
    sqlite3_stmt *tables = NULL;
    if (sqlite3_prepare_v2(
            db, "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%';",
            -1, &tables, NULL) != SQLITE_OK)
        return 0;
    int valid = 1;
    while (valid && sqlite3_step(tables) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(tables, 0);
        int rows = 0;
        if (gm_table_row_count(db, name, &rows) != CBM_STORE_OK || rows != 0)
            valid = 0;
    }
    sqlite3_finalize(tables);
    return valid;
}

static int gm_bootstrap_target_is_empty(const char *memory_path, const char *config_path,
                                        const char *global_graph_path) {
    sqlite3 *memory = NULL, *config = NULL, *graph = NULL;
    int rc = gm_open_immutable_readonly(memory_path, &memory);
    if (rc == CBM_STORE_OK)
        rc = gm_open_immutable_readonly(config_path, &config);
    if (rc == CBM_STORE_OK)
        rc = gm_open_immutable_readonly(global_graph_path, &graph);
    int valid = rc == CBM_STORE_OK && gm_bootstrap_memory_is_empty(memory) &&
                gm_bootstrap_config_is_empty(config) && gm_bootstrap_graph_is_empty(graph);
    if (graph)
        sqlite3_close(graph);
    if (config)
        sqlite3_close(config);
    if (memory)
        sqlite3_close(memory);
    return valid ? CBM_STORE_OK : (rc == CBM_STORE_OK ? CBM_STORE_REJECTED : CBM_STORE_ERR);
}

static int gm_checks_equal(const gm_logical_check_t *a, const gm_logical_check_t *b) {
    return a && b && !strcmp(a->quick_check, b->quick_check) &&
           a->foreign_key_violations == b->foreign_key_violations &&
           !strcmp(a->schema_sha256, b->schema_sha256) && !strcmp(a->row_counts, b->row_counts) &&
           !strcmp(a->canonical_logical_sha256, b->canonical_logical_sha256);
}

typedef struct {
    int memory;
    int graph;
    int config;
} gm_projection_equivalence_t;

static int gm_projection_matches(sqlite3 *candidate, sqlite3 *source, const char *prefix) {
    gm_logical_check_t expected = {0}, actual = {0};
    if (gm_collect_db_projection(source, source, prefix, &expected, 0) != CBM_STORE_OK ||
        gm_collect_db_projection(candidate, source, prefix, &actual, 0) != CBM_STORE_OK)
        return 0;
    strcat(expected.row_counts, "}");
    strcat(actual.row_counts, "}");
    return gm_checks_equal(&expected, &actual);
}

static char *gm_migration_report(const char *status, const cbm_project_resolution_t *project,
                                 const gm_logical_check_t *source, const gm_logical_check_t *backup,
                                 const gm_logical_check_t *target,
                                 const gm_projection_equivalence_t *projection, int legacy_projects,
                                 int legacy_memories, const char *payload_hash) {
    size_t cap = 32768;
    char *json = malloc(cap);
    if (!json)
        return NULL;
    const char *tquick = target ? target->quick_check : "not_created",
               *tschema = target ? target->schema_sha256 : "",
               *tlogical = target ? target->canonical_logical_sha256 : "",
               *trows = target ? target->row_counts : "{}";
    const char *bquick = backup ? backup->quick_check : "not_created",
               *bschema = backup ? backup->schema_sha256 : "",
               *blogical = backup ? backup->canonical_logical_sha256 : "",
               *brows = backup ? backup->row_counts : "{}";
    int preserved = (!strcmp(status, "applied") || !strcmp(status, "replayed")) &&
                    gm_checks_equal(source, backup) && gm_checks_equal(source, target);
    int memory_eq = projection && projection->memory, graph_eq = projection && projection->graph,
        config_eq = projection && projection->config;
    snprintf(
        json, cap,
        "{\"schema\":\"semantic-memory-global-migration/"
        "v1\",\"status\":\"%s\",\"project_uuid\":\"%s\",\"source\":{\"quick_check\":\"%s\","
        "\"foreign_key_violations\":%d,\"schema_sha256\":\"%s\",\"row_counts\":%s,\"canonical_"
        "logical_sha256\":\"%s\"},\"backup\":{\"quick_check\":\"%s\",\"foreign_key_violations\":%d,"
        "\"schema_sha256\":\"%s\",\"row_counts\":%s,\"canonical_logical_sha256\":\"%s\"},"
        "\"target\":{\"quick_check\":\"%s\",\"foreign_key_violations\":%d,\"schema_sha256\":\"%s\","
        "\"row_counts\":%s,\"canonical_logical_sha256\":\"%s\"},\"source_to_target_projection\":{"
        "\"memory\":%s,\"config\":%s,\"project_graph\":%s,\"equivalent\":%s},\"preserved_data_"
        "equivalent\":%s,\"source_to_target_mapping\":{\"config\":\"_config.db\",\"memory\":\"__"
        "global__-memory.db\",\"graph\":\"projects/%s/"
        "graph.db\",\"global_graph\":\"__global__-graph.db\"},\"legacy_alias_counts\":{"
        "\"projects\":%d,\"memory_items\":%d},\"payload_sha256\":\"%s\",\"current_pointer_"
        "switched\":false}",
        status, project ? project->project_uuid : "", source ? source->quick_check : "failed",
        source ? source->foreign_key_violations : -1, source ? source->schema_sha256 : "",
        source ? source->row_counts : "{}", source ? source->canonical_logical_sha256 : "", bquick,
        backup ? backup->foreign_key_violations : 0, bschema, brows, blogical, tquick,
        target ? target->foreign_key_violations : 0, tschema, trows, tlogical,
        memory_eq ? "true" : "false", config_eq ? "true" : "false", graph_eq ? "true" : "false",
        memory_eq && config_eq && graph_eq ? "true" : "false",
        preserved && memory_eq && config_eq && graph_eq ? "true" : "false",
        project ? project->project_uuid : "", legacy_projects, legacy_memories,
        payload_hash ? payload_hash : "");
    return json;
}

static int gm_migration_ledger(const char *target_memory, gm_held_file_t *held, const char *key,
                               const char *payload_hash, int *exact, char state[32],
                               char target_hash[65]) {
    *exact = 0;
    state[0] = 0;
    target_hash[0] = 0;
    if (!held || !held->exists)
        return CBM_STORE_NOT_FOUND;
    if (!gm_sqlite_sidecars_absent(target_memory))
        return CBM_STORE_REJECTED;
    sqlite3 *db = NULL;
    if (gm_open_held_readonly(target_memory, held, &db) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    int rc = CBM_STORE_NOT_FOUND;
    int prepare = sqlite3_prepare_v2(db,
                                     "SELECT payload_sha256,state,target_logical_sha256 FROM "
                                     "global_migration_ledger WHERE idempotency_key=?1;",
                                     -1, &stmt, NULL);
    if (prepare != SQLITE_OK)
        rc = CBM_STORE_ERR;
    else if (sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    else {
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            const char *stored = (const char *)sqlite3_column_text(stmt, 0);
            const char *stored_state = (const char *)sqlite3_column_text(stmt, 1);
            const char *stored_hash = (const char *)sqlite3_column_text(stmt, 2);
            if (!stored || !stored_state || !stored_hash)
                rc = CBM_STORE_ERR;
            else {
                *exact = strcmp(stored, payload_hash) == 0;
                snprintf(state, 32, "%s", stored_state);
                snprintf(target_hash, 65, "%s", stored_hash);
                rc = CBM_STORE_OK;
            }
        } else if (step == SQLITE_DONE)
            rc = CBM_STORE_NOT_FOUND;
        else
            rc = CBM_STORE_ERR;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    if (sqlite3_close(db) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    return rc;
}

typedef struct {
    int row_count;
    char state[32];
    char payload_sha256[65];
    char source_logical_sha256[65];
    char target_logical_sha256[65];
} gm_managed_ledger_t;

static int gm_managed_ledger_read(sqlite3 *db, const char *key, gm_managed_ledger_t *out) {
    if (!db || !key || !out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            db,
            "SELECT state,payload_sha256,source_logical_sha256,target_logical_sha256 "
            "FROM global_migration_ledger WHERE idempotency_key=?1;",
            -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    int rc = CBM_STORE_NOT_FOUND;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        out->row_count++;
        if (out->row_count > 1) {
            rc = CBM_STORE_ERR;
            break;
        }
        const char *state = (const char *)sqlite3_column_text(stmt, 0);
        const char *payload = (const char *)sqlite3_column_text(stmt, 1);
        const char *source = (const char *)sqlite3_column_text(stmt, 2);
        const char *target = (const char *)sqlite3_column_text(stmt, 3);
        if (!state || !payload || !source || !target || strlen(state) >= sizeof(out->state) ||
            strlen(payload) != 64 || strlen(source) != 64 || strlen(target) != 64) {
            rc = CBM_STORE_ERR;
            break;
        }
        snprintf(out->state, sizeof(out->state), "%s", state);
        snprintf(out->payload_sha256, sizeof(out->payload_sha256), "%s", payload);
        snprintf(out->source_logical_sha256, sizeof(out->source_logical_sha256), "%s", source);
        snprintf(out->target_logical_sha256, sizeof(out->target_logical_sha256), "%s", target);
        rc = CBM_STORE_OK;
    }
    if (sqlite3_errcode(db) != SQLITE_OK && rc == CBM_STORE_NOT_FOUND)
        rc = CBM_STORE_ERR;
    if (sqlite3_finalize(stmt) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    return rc;
}

static const char *gm_verify_quick(const gm_logical_check_t *check) {
    return check && check->quick_check[0] ? check->quick_check : "not_observed";
}

static const char *gm_verify_schema(const gm_logical_check_t *check) {
    return check && check->schema_sha256[0] ? check->schema_sha256 : "";
}

static const char *gm_verify_logical(const gm_logical_check_t *check) {
    return check && check->canonical_logical_sha256[0] ? check->canonical_logical_sha256 : "";
}

static const char *gm_verify_rows(const gm_logical_check_t *check) {
    return check && check->row_counts[0] ? check->row_counts : "{}";
}

static char *gm_managed_verify_report(const char *status, const cbm_project_resolution_t *project,
                                      const gm_logical_check_t *source,
                                      const gm_logical_check_t *target,
                                      const gm_logical_check_t *global_graph,
                                      const gm_managed_ledger_t *ledger, int source_exists,
                                      int target_exists, int sidecars_absent,
                                      int source_payload_match, int source_logical_match,
                                      int target_logical_match, const char *payload_hash) {
    size_t cap = 65536;
    char *json = malloc(cap);
    if (!json)
        return NULL;
    const char *ledger_state = ledger && ledger->state[0] ? ledger->state : "";
    const char *ledger_payload = ledger && ledger->payload_sha256[0] ? ledger->payload_sha256 : "";
    const char *ledger_source =
        ledger && ledger->source_logical_sha256[0] ? ledger->source_logical_sha256 : "";
    const char *ledger_target =
        ledger && ledger->target_logical_sha256[0] ? ledger->target_logical_sha256 : "";
    int ledger_found = ledger && ledger->row_count == 1;
    int n = snprintf(
        json, cap,
        "{\"schema\":\"semantic-memory-global-managed-target-verify/v1\","
        "\"status\":\"%s\",\"project_uuid\":\"%s\","
        "\"source_exists\":%s,\"target_exists\":%s,\"sidecars_absent\":%s,"
        "\"source\":{\"quick_check\":\"%s\",\"foreign_key_violations\":%d,"
        "\"schema_sha256\":\"%s\",\"row_counts\":%s,\"canonical_logical_sha256\":\"%s\"},"
        "\"target\":{\"quick_check\":\"%s\",\"foreign_key_violations\":%d,"
        "\"schema_sha256\":\"%s\",\"row_counts\":%s,\"canonical_logical_sha256\":\"%s\"},"
        "\"global_graph\":{\"quick_check\":\"%s\",\"foreign_key_violations\":%d,"
        "\"schema_sha256\":\"%s\",\"row_counts\":%s,\"canonical_logical_sha256\":\"%s\"},"
        "\"ledger\":{\"found\":%s,\"row_count\":%d,\"state\":\"%s\","
        "\"payload_sha256\":\"%s\",\"source_logical_sha256\":\"%s\","
        "\"target_logical_sha256\":\"%s\"},"
        "\"comparisons\":{\"source_payload_match\":%s,\"source_logical_match\":%s,"
        "\"target_logical_match\":%s},\"payload_sha256\":\"%s\","
        "\"database_write_performed\":false,\"current_pointer_switched\":false}",
        status, project ? project->project_uuid : "", source_exists ? "true" : "false",
        target_exists ? "true" : "false", sidecars_absent ? "true" : "false",
        gm_verify_quick(source), source ? source->foreign_key_violations : 0,
        gm_verify_schema(source), gm_verify_rows(source), gm_verify_logical(source),
        gm_verify_quick(target), target ? target->foreign_key_violations : 0,
        gm_verify_schema(target), gm_verify_rows(target), gm_verify_logical(target),
        gm_verify_quick(global_graph), global_graph ? global_graph->foreign_key_violations : 0,
        gm_verify_schema(global_graph), gm_verify_rows(global_graph),
        gm_verify_logical(global_graph), ledger_found ? "true" : "false",
        ledger ? ledger->row_count : 0, ledger_state, ledger_payload, ledger_source, ledger_target,
        source_payload_match ? "true" : "false", source_logical_match ? "true" : "false",
        target_logical_match ? "true" : "false", payload_hash ? payload_hash : "");
    if (n < 0 || (size_t)n >= cap) {
        free(json);
        return NULL;
    }
    return json;
}

int cbm_global_migration_verify_existing(const cbm_global_migration_input_t *input,
                                         char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    if (!input || !out_report_json || !input->source_memory_path || !input->source_graph_path ||
        !input->source_config_path || !input->target_root || !input->project_path ||
        !input->idempotency_key || !input->mode || strcmp(input->mode, "verify"))
        return CBM_STORE_ERR;
    if (!gm_path_is_absolute(input->source_memory_path) ||
        !gm_path_is_absolute(input->source_graph_path) ||
        !gm_path_is_absolute(input->source_config_path) ||
        !gm_path_is_absolute(input->target_root) || !gm_path_is_absolute(input->project_path))
        return CBM_STORE_REJECTED;
    cbm_project_resolution_t project = {0};
    if (cbm_project_resolve(input->project_path, NULL, NULL, &project) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char target_memory[4096], target_graph[4096], target_config[4096], target_global_graph[4096];
    if (gm_path_format(target_memory, sizeof(target_memory), "%s/__global__-memory.db",
                       input->target_root) != CBM_STORE_OK ||
        gm_path_format(target_global_graph, sizeof(target_global_graph), "%s/__global__-graph.db",
                       input->target_root) != CBM_STORE_OK ||
        gm_path_format(target_config, sizeof(target_config), "%s/_config.db", input->target_root) !=
            CBM_STORE_OK ||
        gm_path_format(target_graph, sizeof(target_graph), "%s/projects/%s/graph.db",
                       input->target_root, project.project_uuid) != CBM_STORE_OK)
        return CBM_STORE_REJECTED;
    const char *source_paths[3] = {input->source_memory_path, input->source_graph_path,
                                   input->source_config_path};
    const char *target_paths[4] = {target_memory, target_graph, target_config, target_global_graph};
    int source_exists = 1, target_exists = 1;
    for (size_t i = 0; i < 3; i++) {
        int exists = 0;
        if (gm_vfs_path_exists(source_paths[i], &exists) != CBM_STORE_OK)
            return CBM_STORE_REJECTED;
        source_exists = source_exists && exists;
    }
    for (size_t i = 0; i < 4; i++) {
        int exists = 0;
        if (gm_vfs_path_exists(target_paths[i], &exists) != CBM_STORE_OK)
            return CBM_STORE_REJECTED;
        target_exists = target_exists && exists;
    }
    int source_sidecars_absent = gm_paths_sidecars_absent(source_paths, 3);
    int target_sidecars_absent = gm_paths_sidecars_absent(target_paths, 4);
    int sidecars_absent = source_sidecars_absent && target_sidecars_absent;
    gm_logical_check_t source = {0}, target = {0}, target_health = {0}, global_graph = {0};
    gm_managed_ledger_t ledger = {0};
    sqlite3 *source_memory = NULL, *source_graph = NULL, *source_config = NULL;
    sqlite3 *target_memory_db = NULL, *target_graph_db = NULL, *target_config_db = NULL,
            *target_global_graph_db = NULL;
    int source_rc = CBM_STORE_ERR, target_rc = CBM_STORE_ERR, ledger_rc = CBM_STORE_NOT_FOUND;
    char payload_hash[65] = {0};
    if (source_exists && source_sidecars_absent &&
        gm_open_immutable_readonly(input->source_memory_path, &source_memory) == CBM_STORE_OK &&
        gm_open_immutable_readonly(input->source_graph_path, &source_graph) == CBM_STORE_OK &&
        gm_open_immutable_readonly(input->source_config_path, &source_config) == CBM_STORE_OK)
        source_rc = gm_collect_triple_checks(source_memory, source_graph, source_config, &source);
    if (source_rc == CBM_STORE_OK) {
        char payload_seed[32768];
        snprintf(payload_seed, sizeof(payload_seed), "stage14-global-migration-v1\n%s\n%s\n%s",
                 project.project_uuid, source.schema_sha256, source.canonical_logical_sha256);
        if (cbm_stage7_sha256_hex(payload_seed, strlen(payload_seed), payload_hash) != CBM_STORE_OK)
            source_rc = CBM_STORE_ERR;
    }
    if (target_exists && target_sidecars_absent &&
        gm_open_immutable_readonly(target_memory, &target_memory_db) == CBM_STORE_OK &&
        gm_open_immutable_readonly(target_graph, &target_graph_db) == CBM_STORE_OK &&
        gm_open_immutable_readonly(target_config, &target_config_db) == CBM_STORE_OK &&
        gm_open_immutable_readonly(target_global_graph, &target_global_graph_db) == CBM_STORE_OK) {
        target_rc = gm_collect_triple_checks(target_memory_db, target_graph_db, target_config_db,
                                             &target_health);
        if (target_rc == CBM_STORE_OK && source_rc == CBM_STORE_OK)
            target_rc =
                gm_collect_triple_projection(target_memory_db, target_graph_db, target_config_db,
                                             source_memory, source_graph, source_config, &target);
        if (target_rc == CBM_STORE_OK)
            target_rc =
                gm_collect_db_checks(target_global_graph_db, "global_graph", &global_graph, 0);
        if (target_rc == CBM_STORE_OK) {
            size_t used = strlen(global_graph.row_counts);
            if (used + 2 > sizeof(global_graph.row_counts))
                target_rc = CBM_STORE_ERR;
            else
                strcat(global_graph.row_counts, "}");
        }
        if (target_rc == CBM_STORE_OK)
            ledger_rc = gm_managed_ledger_read(target_memory_db, input->idempotency_key, &ledger);
    }
    int source_payload_match = source_rc == CBM_STORE_OK && ledger_rc == CBM_STORE_OK &&
                               payload_hash[0] && !strcmp(payload_hash, ledger.payload_sha256);
    int source_logical_match =
        source_rc == CBM_STORE_OK && ledger_rc == CBM_STORE_OK &&
        !strcmp(source.canonical_logical_sha256, ledger.source_logical_sha256);
    int target_logical_match =
        target_rc == CBM_STORE_OK && ledger_rc == CBM_STORE_OK &&
        !strcmp(target.canonical_logical_sha256, ledger.target_logical_sha256);
    int target_health_ok = target_rc == CBM_STORE_OK && !strcmp(target_health.quick_check, "ok") &&
                           target_health.foreign_key_violations == 0 &&
                           global_graph.quick_check[0] && !strcmp(global_graph.quick_check, "ok") &&
                           global_graph.foreign_key_violations == 0;
    const char *status = "failed";
    if (!source_exists)
        status = "source_missing";
    else if (!source_sidecars_absent || !target_sidecars_absent)
        status = "target_not_quiescent";
    else if (source_rc != CBM_STORE_OK)
        status = "source_unhealthy";
    else if (!target_exists)
        status = "target_missing";
    else if (target_rc != CBM_STORE_OK || !target_health_ok)
        status = "target_unhealthy";
    else if (ledger_rc == CBM_STORE_NOT_FOUND)
        status = "ledger_missing";
    else if (ledger_rc != CBM_STORE_OK)
        status = "ledger_unreadable";
    else if (strcmp(ledger.state, "applied"))
        status = "ledger_not_applied";
    else if (!target_logical_match)
        status = "target_drift";
    else if (!source_payload_match || !source_logical_match)
        status = "source_drift";
    else
        status = "verified";
    *out_report_json = gm_managed_verify_report(
        status, &project, source_rc == CBM_STORE_OK ? &source : NULL,
        target_rc == CBM_STORE_OK ? &target : NULL,
        target_rc == CBM_STORE_OK ? &global_graph : NULL, &ledger, source_exists, target_exists,
        sidecars_absent, source_payload_match, source_logical_match, target_logical_match,
        payload_hash);
    if (source_memory)
        sqlite3_close(source_memory);
    if (source_graph)
        sqlite3_close(source_graph);
    if (source_config)
        sqlite3_close(source_config);
    if (target_memory_db)
        sqlite3_close(target_memory_db);
    if (target_graph_db)
        sqlite3_close(target_graph_db);
    if (target_config_db)
        sqlite3_close(target_config_db);
    if (target_global_graph_db)
        sqlite3_close(target_global_graph_db);
    if (!*out_report_json)
        return CBM_STORE_ERR;
    return !strcmp(status, "verified") ? CBM_STORE_OK : CBM_STORE_REJECTED;
}

static int gm_migration_alias_counts(sqlite3 *db, const char *project_uuid, int *aliases,
                                     int *memories) {
    sqlite3_stmt *stmt = NULL;
    *aliases = 0;
    *memories = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM global_legacy_alias WHERE project_uuid=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(stmt, 1, project_uuid);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    *aliases = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM global_memory_provenance WHERE project_uuid=?1 "
                           "AND source_kind='legacy_migration';",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    gm_bind_text(stmt, 1, project_uuid);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    *memories = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return CBM_STORE_OK;
}

static int gm_migration_adopt_bootstrap(
    sqlite3 **source_memory, sqlite3 **source_graph, sqlite3 **source_config,
    const char *source_memory_path, const char *source_graph_path, const char *source_config_path,
    const char *target_memory, const char *target_graph, const char *target_config,
    const char *target_global_graph, const char *project_root,
    const cbm_project_resolution_t *project, const cbm_global_migration_input_t *input,
    const gm_logical_check_t *source, const char *payload_hash, char **out_report_json) {
#ifndef _WIN32
    (void)source_memory;
    (void)source_graph;
    (void)source_config;
    (void)source_memory_path;
    (void)source_graph_path;
    (void)source_config_path;
    (void)target_memory;
    (void)target_graph;
    (void)target_config;
    (void)target_global_graph;
    (void)project_root;
    (void)project;
    (void)input;
    (void)source;
    *out_report_json =
        gm_migration_report("failed", project, source, NULL, NULL, NULL, 0, 0, payload_hash);
    return CBM_STORE_REJECTED;
#else
    gm_serialized_image_t images[4] = {{0}};
    gm_adoption_replacement_t replacements[3] = {{0}};
    const char *existing_paths[3] = {target_memory, target_config, target_global_graph};
    int rc = CBM_STORE_OK, replaced = 0, project_created = 0, project_root_created = 0;
    sqlite3 *copy_memory = NULL, *copy_graph = NULL, *copy_config = NULL, *target_graph_db = NULL,
            *target_config_db = NULL;
    sqlite3_stmt *stmt = NULL;
    cbm_global_memory_t *global = NULL;
    gm_created_file_t project_file;
    gm_created_file_init(&project_file);
    gm_logical_check_t backup = {0}, target = {0};
    gm_projection_equivalence_t projection = {0};
    int aliases = 0, memories = 0;
    if (gm_backup_to_image(*source_memory, &images[0]) != CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK && gm_backup_to_image(*source_graph, &images[1]) != CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK && gm_backup_to_image(*source_config, &images[2]) != CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK && gm_global_graph_image(&images[3]) != CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (*source_memory)
        sqlite3_close(*source_memory);
    *source_memory = NULL;
    if (*source_graph)
        sqlite3_close(*source_graph);
    *source_graph = NULL;
    if (*source_config)
        sqlite3_close(*source_config);
    *source_config = NULL;
    if (rc != CBM_STORE_OK)
        goto fail;
    for (size_t i = 0; i < 3 && rc == CBM_STORE_OK; i++) {
        rc = gm_adoption_replace_existing(existing_paths[i], &images[i == 0 ? 0 : (i == 1 ? 2 : 3)],
                                          i, &replacements[i]);
        if (rc == CBM_STORE_OK)
            replaced++;
    }
    if (rc != CBM_STORE_OK)
        goto fail;
    if (!gm_dir_exists(project_root)) {
        if (!gm_dir_create(project_root)) {
            rc = CBM_STORE_ERR;
            goto fail;
        }
        project_root_created = 1;
    }
    int collision = 0;
    rc = gm_atomic_create_image(target_graph, &images[1], &project_file, &collision);
    if (rc != CBM_STORE_OK)
        goto fail;
    gm_created_file_release(&project_file);
    project_created = 1;

    if (sqlite3_open_v2(target_memory, &copy_memory, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_open_v2(target_graph, &copy_graph, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_open_v2(target_config, &copy_config, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        gm_collect_triple_checks(copy_memory, copy_graph, copy_config, &backup) != CBM_STORE_OK ||
        !gm_checks_equal(source, &backup)) {
        rc = CBM_STORE_ERR;
        goto fail;
    }
    sqlite3_close(copy_memory);
    copy_memory = NULL;
    sqlite3_close(copy_graph);
    copy_graph = NULL;
    sqlite3_close(copy_config);
    copy_config = NULL;
    global = cbm_global_memory_open(target_memory, target_global_graph);
    if (!global) {
        rc = CBM_STORE_ERR;
        goto fail;
    }
    if (sqlite3_open_v2(target_graph, &target_graph_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        sqlite3_open_v2(target_config, &target_config_db, SQLITE_OPEN_READONLY, NULL) !=
            SQLITE_OK) {
        rc = CBM_STORE_ERR;
        goto fail;
    }
    sqlite3 *db = cbm_global_memory_db(global);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    char now[40];
    gm_timestamp(now);
    char *catalog_report = NULL;
    if (rc == CBM_STORE_OK) {
        int catalog_rc = gm_ensure_project_locked(global->memory, project, input->idempotency_key,
                                                  &catalog_report);
        if (catalog_rc != CBM_STORE_OK && catalog_rc != CBM_STORE_REPLAYED)
            rc = catalog_rc;
    }
    free(catalog_report);
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(
            db,
            "INSERT OR IGNORE INTO "
            "global_legacy_alias(legacy_kind,legacy_id,global_id,project_uuid,payload_sha256,"
            "created_at) SELECT 'project',scope_project,?1,?1,?2,?3 FROM memory_item WHERE "
            "scope_project IS NOT NULL GROUP BY scope_project;",
            -1, &stmt, NULL) == SQLITE_OK) {
        gm_bind_text(stmt, 1, project->project_uuid);
        gm_bind_text(stmt, 2, payload_hash);
        gm_bind_text(stmt, 3, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        aliases = sqlite3_changes(db);
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(db,
                           "INSERT OR IGNORE INTO "
                           "global_memory_provenance(memory_item_id,project_uuid,legacy_project_id,"
                           "source_kind,payload_sha256,created_at) SELECT "
                           "id,?1,scope_project,'legacy_migration',?2,?3 FROM memory_item;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        gm_bind_text(stmt, 1, project->project_uuid);
        gm_bind_text(stmt, 2, payload_hash);
        gm_bind_text(stmt, 3, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        memories = sqlite3_changes(db);
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(
            db,
            "INSERT INTO "
            "global_migration_ledger(migration_id,source_store_id,source_logical_sha256,target_"
            "store_id,target_logical_sha256,state,idempotency_key,payload_sha256,created_at) "
            "VALUES(?1,?2,?3,?4,'pending-post-check','staged',?5,?6,?7);",
            -1, &stmt, NULL) == SQLITE_OK) {
        char migration_id[48];
        snprintf(migration_id, sizeof(migration_id), "mig-%.32s", payload_hash);
        gm_bind_text(stmt, 1, migration_id);
        gm_bind_text(stmt, 2, input->source_memory_path);
        gm_bind_text(stmt, 3, source->canonical_logical_sha256);
        gm_bind_text(stmt, 4, target_memory);
        gm_bind_text(stmt, 5, input->idempotency_key);
        gm_bind_text(stmt, 6, payload_hash);
        gm_bind_text(stmt, 7, now);
        rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc == CBM_STORE_OK) {
        /* The source handles were closed before replacement; reopen them read-only
           from the immutable input paths solely for the projection check. */
        sqlite3 *verify_memory = NULL, *verify_graph = NULL, *verify_config = NULL;
        if (gm_open_immutable_readonly(source_memory_path, &verify_memory) != CBM_STORE_OK ||
            gm_open_immutable_readonly(source_graph_path, &verify_graph) != CBM_STORE_OK ||
            gm_open_immutable_readonly(source_config_path, &verify_config) != CBM_STORE_OK ||
            gm_collect_triple_projection(db, target_graph_db, target_config_db, verify_memory,
                                         verify_graph, verify_config, &target) != CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK) {
            projection.memory = gm_projection_matches(db, verify_memory, "memory");
            projection.graph = gm_projection_matches(target_graph_db, verify_graph, "graph");
            projection.config = gm_projection_matches(target_config_db, verify_config, "config");
            if (!projection.memory || !projection.graph || !projection.config)
                rc = CBM_STORE_ERR;
        }
        if (verify_memory)
            sqlite3_close(verify_memory);
        if (verify_graph)
            sqlite3_close(verify_graph);
        if (verify_config)
            sqlite3_close(verify_config);
    }
    gm_logical_check_t global_health = {0};
    if (rc == CBM_STORE_OK)
        rc = gm_collect_db_checks(cbm_global_graph_db(global), "global_graph", &global_health, 0);
    if (rc == CBM_STORE_OK && gm_checks_equal(source, &backup) &&
        !strcmp(global_health.quick_check, "ok") && global_health.foreign_key_violations == 0) {
        if (sqlite3_prepare_v2(
                db,
                "UPDATE global_migration_ledger SET target_logical_sha256=?1,state='applied' WHERE "
                "idempotency_key=?2 AND state='staged';",
                -1, &stmt, NULL) == SQLITE_OK) {
            gm_bind_text(stmt, 1, target.canonical_logical_sha256);
            gm_bind_text(stmt, 2, input->idempotency_key);
            rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                               : CBM_STORE_ERR;
        } else
            rc = CBM_STORE_ERR;
        sqlite3_finalize(stmt);
    } else if (rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK)
        rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK ? CBM_STORE_OK
                                                                        : CBM_STORE_ERR;
    else
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    if (target_graph_db)
        sqlite3_close(target_graph_db);
    target_graph_db = NULL;
    if (target_config_db)
        sqlite3_close(target_config_db);
    target_config_db = NULL;
    if (global)
        cbm_global_memory_close(global);
    global = NULL;
    if (rc == CBM_STORE_OK && gm_adoption_commit(replacements, 3) == CBM_STORE_OK) {
        for (size_t i = 0; i < 4; i++)
            gm_serialized_image_reset(&images[i]);
        *out_report_json = gm_migration_report("applied", project, source, &backup, &target,
                                               &projection, aliases, memories, payload_hash);
        return *out_report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
fail:
    if (stmt)
        sqlite3_finalize(stmt);
    if (copy_memory)
        sqlite3_close(copy_memory);
    if (copy_graph)
        sqlite3_close(copy_graph);
    if (copy_config)
        sqlite3_close(copy_config);
    if (target_graph_db)
        sqlite3_close(target_graph_db);
    if (target_config_db)
        sqlite3_close(target_config_db);
    if (global)
        cbm_global_memory_close(global);
    if (project_created)
        gm_path_remove_file(target_graph);
    else if (project_file.created)
        gm_created_file_cleanup(target_graph, &project_file);
    if (project_root_created)
        gm_path_remove_empty_directory(project_root);
    if (replaced && gm_adoption_restore(replacements, 3) != CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    for (size_t i = 0; i < 4; i++)
        gm_serialized_image_reset(&images[i]);
    *out_report_json =
        gm_migration_report("failed", project, source, NULL, NULL, NULL, 0, 0, payload_hash);
    return rc == CBM_STORE_REJECTED ? CBM_STORE_REJECTED : CBM_STORE_ERR;
#endif
}

int cbm_global_migration_execute(const cbm_global_migration_input_t *input,
                                 char **out_report_json) {
    if (out_report_json)
        *out_report_json = NULL;
    if (!input || !out_report_json || !input->source_memory_path || !input->source_graph_path ||
        !input->source_config_path || !input->target_root || !input->project_path ||
        !input->idempotency_key || !input->mode)
        return CBM_STORE_ERR;
    if (!strcmp(input->mode, "verify"))
        return cbm_global_migration_verify_existing(input, out_report_json);
    if (strcmp(input->mode, "plan") && strcmp(input->mode, "apply"))
        return CBM_STORE_ERR;
    if (!gm_path_is_absolute(input->source_memory_path) ||
        !gm_path_is_absolute(input->source_graph_path) ||
        !gm_path_is_absolute(input->source_config_path) ||
        !gm_path_is_absolute(input->target_root) || !gm_path_is_absolute(input->project_path))
        return CBM_STORE_REJECTED;
    cbm_project_resolution_t project = {0};
    if (cbm_project_resolve(input->project_path, NULL, NULL, &project) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char target_memory[4096], target_graph[4096], target_config[4096], target_global_graph[4096],
        projects_root[4096], project_root[4096];
    if (gm_path_format(target_memory, sizeof(target_memory), "%s/__global__-memory.db",
                       input->target_root) != CBM_STORE_OK ||
        gm_path_format(target_global_graph, sizeof(target_global_graph), "%s/__global__-graph.db",
                       input->target_root) != CBM_STORE_OK ||
        gm_path_format(target_config, sizeof(target_config), "%s/_config.db", input->target_root) !=
            CBM_STORE_OK ||
        gm_path_format(projects_root, sizeof(projects_root), "%s/projects", input->target_root) !=
            CBM_STORE_OK ||
        gm_path_format(project_root, sizeof(project_root), "%s/projects/%s", input->target_root,
                       project.project_uuid) != CBM_STORE_OK ||
        gm_path_format(target_graph, sizeof(target_graph), "%s/projects/%s/graph.db",
                       input->target_root, project.project_uuid) != CBM_STORE_OK)
        return CBM_STORE_REJECTED;
    const char *target_paths[4] = {target_memory, target_graph, target_config, target_global_graph};
    enum {
        GM_TARGET_ALL_ABSENT = 0,
        GM_TARGET_COMPLETE_REPLAY = 1,
        GM_TARGET_PARTIAL_OR_DIRTY = 2,
        GM_TARGET_EMPTY_BOOTSTRAP = 3
    };
    int apply_mode = !strcmp(input->mode, "apply"), target_state = GM_TARGET_ALL_ABSENT;
    gm_logical_check_t source = {0};
    char payload_seed[32768], payload_hash[65] = {0};
    if (apply_mode) {
        int target_exists_count = 0;
        for (size_t i = 0; i < 4; i++) {
            int exists = 0;
            if (gm_vfs_path_exists(target_paths[i], &exists) != CBM_STORE_OK)
                return CBM_STORE_REJECTED;
            target_exists_count += exists ? 1 : 0;
        }
        int target_sidecars_absent = gm_paths_sidecars_absent(target_paths, 4);
        if (!target_sidecars_absent)
            target_state = GM_TARGET_PARTIAL_OR_DIRTY;
        else if (target_exists_count == 4)
            target_state = GM_TARGET_COMPLETE_REPLAY;
        else if (target_exists_count == 3 && !gm_file_exists(target_graph) &&
                 gm_bootstrap_target_is_empty(target_memory, target_config, target_global_graph) ==
                     CBM_STORE_OK)
            target_state = GM_TARGET_EMPTY_BOOTSTRAP;
        else if (target_exists_count > 0)
            target_state = GM_TARGET_PARTIAL_OR_DIRTY;
        if (target_state == GM_TARGET_PARTIAL_OR_DIRTY) {
            *out_report_json =
                gm_migration_report("failed", &project, NULL, NULL, NULL, NULL, 0, 0, payload_hash);
            return CBM_STORE_REJECTED;
        }
    }
    const char *replay_paths[7] = {input->source_memory_path,
                                   input->source_graph_path,
                                   input->source_config_path,
                                   target_memory,
                                   target_graph,
                                   target_config,
                                   target_global_graph};
    gm_held_file_t replay_held[7];
    memset(replay_held, 0, sizeof(replay_held));
#ifdef _WIN32
    for (size_t i = 0; i < 7; i++)
        replay_held[i].handle = INVALID_HANDLE_VALUE;
#else
    for (size_t i = 0; i < 7; i++)
        replay_held[i].fd = -1;
#endif
    int replay_witnessed = apply_mode && target_state == GM_TARGET_COMPLETE_REPLAY;
    sqlite3 *source_memory = NULL, *source_graph = NULL, *source_config = NULL;
    int source_open_rc = CBM_STORE_OK;
    if (replay_witnessed) {
        if (gm_held_set_acquire(replay_paths, 7, replay_held) != CBM_STORE_OK) {
            *out_report_json =
                gm_migration_report("failed", &project, NULL, NULL, NULL, NULL, 0, 0, payload_hash);
            return CBM_STORE_REJECTED;
        }
        for (size_t i = 0; i < 7; i++)
            if (!replay_held[i].exists)
                source_open_rc = CBM_STORE_ERR;
        if (source_open_rc == CBM_STORE_OK)
            source_open_rc =
                gm_open_held_readonly(input->source_memory_path, &replay_held[0], &source_memory);
        if (source_open_rc == CBM_STORE_OK)
            source_open_rc =
                gm_open_held_readonly(input->source_graph_path, &replay_held[1], &source_graph);
        if (source_open_rc == CBM_STORE_OK)
            source_open_rc =
                gm_open_held_readonly(input->source_config_path, &replay_held[2], &source_config);
    } else {
        if (!apply_mode || target_state == GM_TARGET_EMPTY_BOOTSTRAP) {
            if (gm_open_immutable_readonly(input->source_memory_path, &source_memory) !=
                CBM_STORE_OK)
                source_open_rc = CBM_STORE_ERR;
            if (source_open_rc == CBM_STORE_OK &&
                gm_open_immutable_readonly(input->source_graph_path, &source_graph) != CBM_STORE_OK)
                source_open_rc = CBM_STORE_ERR;
            if (source_open_rc == CBM_STORE_OK &&
                gm_open_immutable_readonly(input->source_config_path, &source_config) !=
                    CBM_STORE_OK)
                source_open_rc = CBM_STORE_ERR;
        } else {
            if (sqlite3_open_v2(input->source_memory_path, &source_memory, SQLITE_OPEN_READONLY,
                                NULL) != SQLITE_OK)
                source_open_rc = CBM_STORE_ERR;
            if (source_open_rc == CBM_STORE_OK &&
                sqlite3_open_v2(input->source_graph_path, &source_graph, SQLITE_OPEN_READONLY,
                                NULL) != SQLITE_OK)
                source_open_rc = CBM_STORE_ERR;
            if (source_open_rc == CBM_STORE_OK &&
                sqlite3_open_v2(input->source_config_path, &source_config, SQLITE_OPEN_READONLY,
                                NULL) != SQLITE_OK)
                source_open_rc = CBM_STORE_ERR;
        }
    }
    if (source_open_rc != CBM_STORE_OK) {
        int stable = 1;
        if (replay_witnessed) {
            sqlite3 *databases[3] = {source_memory, source_graph, source_config};
            stable =
                gm_held_set_close_databases_and_verify(replay_paths, 7, replay_held, databases, 3);
        } else {
            if (source_memory)
                sqlite3_close(source_memory);
            if (source_graph)
                sqlite3_close(source_graph);
            if (source_config)
                sqlite3_close(source_config);
        }
        *out_report_json =
            gm_migration_report("failed", &project, NULL, NULL, NULL, NULL, 0, 0, payload_hash);
        return replay_witnessed ? (stable ? CBM_STORE_REJECTED : CBM_STORE_ERR) : CBM_STORE_ERR;
    }
    int rc = gm_collect_triple_checks(source_memory, source_graph, source_config, &source);
    snprintf(payload_seed, sizeof(payload_seed), "stage14-global-migration-v1\n%s\n%s\n%s",
             project.project_uuid, source.schema_sha256, source.canonical_logical_sha256);
    if (cbm_stage7_sha256_hex(payload_seed, strlen(payload_seed), payload_hash) != CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc != CBM_STORE_OK || strcmp(source.quick_check, "ok") || source.foreign_key_violations) {
        int stable = 1;
        if (replay_witnessed) {
            sqlite3 *databases[3] = {source_memory, source_graph, source_config};
            stable =
                gm_held_set_close_databases_and_verify(replay_paths, 7, replay_held, databases, 3);
        } else {
            sqlite3_close(source_memory);
            sqlite3_close(source_graph);
            sqlite3_close(source_config);
        }
        *out_report_json =
            gm_migration_report("failed", &project, &source, NULL, NULL, NULL, 0, 0, payload_hash);
        return replay_witnessed ? (stable ? CBM_STORE_REJECTED : CBM_STORE_ERR) : CBM_STORE_ERR;
    }
    if (!apply_mode) {
        int target_exists_count = 0;
        for (size_t i = 0; i < 4; i++) {
            int exists = 0;
            if (gm_vfs_path_exists(target_paths[i], &exists) != CBM_STORE_OK) {
                sqlite3_close(source_memory);
                sqlite3_close(source_graph);
                sqlite3_close(source_config);
                return CBM_STORE_REJECTED;
            }
            target_exists_count += exists ? 1 : 0;
        }
        int plan_bootstrap = target_exists_count == 3 && !gm_file_exists(target_graph) &&
                             gm_paths_sidecars_absent(target_paths, 4) &&
                             gm_bootstrap_target_is_empty(target_memory, target_config,
                                                          target_global_graph) == CBM_STORE_OK;
        if (target_exists_count != 0 && !plan_bootstrap) {
            *out_report_json = gm_migration_report("failed", &project, &source, NULL, NULL, NULL, 0,
                                                   0, payload_hash);
            sqlite3_close(source_memory);
            sqlite3_close(source_graph);
            sqlite3_close(source_config);
            return CBM_STORE_REJECTED;
        }
        *out_report_json =
            gm_migration_report("planned", &project, &source, NULL, NULL, NULL, 0, 0, payload_hash);
        sqlite3_close(source_memory);
        sqlite3_close(source_graph);
        sqlite3_close(source_config);
        return *out_report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    if (target_state == GM_TARGET_EMPTY_BOOTSTRAP) {
        return gm_migration_adopt_bootstrap(
            &source_memory, &source_graph, &source_config, input->source_memory_path,
            input->source_graph_path, input->source_config_path, target_memory, target_graph,
            target_config, target_global_graph, project_root, &project, input, &source,
            payload_hash, out_report_json);
    }
    int exact = 0;
    char ledger_state[32], ledger_target_hash[65];
    int replay = replay_witnessed
                     ? gm_migration_ledger(target_memory, &replay_held[3], input->idempotency_key,
                                           payload_hash, &exact, ledger_state, ledger_target_hash)
                     : CBM_STORE_NOT_FOUND;
    if (replay_witnessed && replay != CBM_STORE_OK) {
        sqlite3 *databases[3] = {source_memory, source_graph, source_config};
        int stable =
            gm_held_set_close_databases_and_verify(replay_paths, 7, replay_held, databases, 3);
        *out_report_json =
            gm_migration_report("failed", &project, &source, NULL, NULL, NULL, 0, 0, payload_hash);
        return stable ? CBM_STORE_REJECTED : CBM_STORE_ERR;
    }
    if (replay == CBM_STORE_OK) {
        if (!exact) {
            sqlite3 *databases[3] = {source_memory, source_graph, source_config};
            int stable =
                gm_held_set_close_databases_and_verify(replay_paths, 7, replay_held, databases, 3);
            *out_report_json =
                gm_migration_report(stable ? "IDEMPOTENCY_CONFLICT" : "failed", &project, &source,
                                    NULL, NULL, NULL, 0, 0, payload_hash);
            return stable ? CBM_STORE_IDEMPOTENCY_CONFLICT : CBM_STORE_REJECTED;
        }
        if (strcmp(ledger_state, "applied") != 0) {
            sqlite3 *databases[3] = {source_memory, source_graph, source_config};
            int stable =
                gm_held_set_close_databases_and_verify(replay_paths, 7, replay_held, databases, 3);
            *out_report_json = gm_migration_report(stable ? "staged" : "failed", &project, &source,
                                                   NULL, NULL, NULL, 0, 0, payload_hash);
            return CBM_STORE_REJECTED;
        }
        sqlite3 *target_memory_db = NULL, *target_graph_db = NULL, *target_config_db = NULL,
                *target_global_graph_db = NULL;
        gm_logical_check_t target = {0};
        int aliases = 0, memories = 0;
        int target_rc = gm_open_held_readonly(target_memory, &replay_held[3], &target_memory_db);
        if (target_rc == CBM_STORE_OK)
            target_rc = gm_open_held_readonly(target_graph, &replay_held[4], &target_graph_db);
        if (target_rc == CBM_STORE_OK)
            target_rc = gm_open_held_readonly(target_config, &replay_held[5], &target_config_db);
        if (target_rc == CBM_STORE_OK)
            target_rc = gm_open_held_readonly(target_global_graph, &replay_held[6],
                                              &target_global_graph_db);
        if (target_rc == CBM_STORE_OK)
            target_rc =
                gm_collect_triple_projection(target_memory_db, target_graph_db, target_config_db,
                                             source_memory, source_graph, source_config, &target);
        gm_logical_check_t global_health = {0};
        if (target_rc == CBM_STORE_OK)
            target_rc =
                gm_collect_db_checks(target_global_graph_db, "global_graph", &global_health, 0);
        if (target_rc == CBM_STORE_OK)
            target_rc = gm_migration_alias_counts(target_memory_db, project.project_uuid, &aliases,
                                                  &memories);
        gm_projection_equivalence_t projection = {0};
        if (target_rc == CBM_STORE_OK) {
            projection.memory = gm_projection_matches(target_memory_db, source_memory, "memory");
            projection.graph = gm_projection_matches(target_graph_db, source_graph, "graph");
            projection.config = gm_projection_matches(target_config_db, source_config, "config");
        }
        int logical_verified = target_rc == CBM_STORE_OK && projection.memory && projection.graph &&
                               projection.config && gm_checks_equal(&source, &target) &&
                               !strcmp(global_health.quick_check, "ok") &&
                               global_health.foreign_key_violations == 0 && ledger_target_hash[0] &&
                               !strcmp(ledger_target_hash, target.canonical_logical_sha256);
        sqlite3 *databases[7] = {target_memory_db,       target_graph_db, target_config_db,
                                 target_global_graph_db, source_memory,   source_graph,
                                 source_config};
        int stable =
            gm_held_set_close_databases_and_verify(replay_paths, 7, replay_held, databases, 7);
        int verified = logical_verified && stable;
        *out_report_json = gm_migration_report(
            verified ? "replayed" : (stable ? "target_drift" : "failed"), &project, &source,
            &source, target_rc == CBM_STORE_OK ? &target : NULL, &projection, aliases, memories,
            payload_hash);
        return verified ? CBM_STORE_REPLAYED : CBM_STORE_REJECTED;
    }
    if ((gm_file_exists(input->target_root) && !gm_dir_exists(input->target_root)) ||
        gm_file_exists(target_memory) || gm_file_exists(target_graph) ||
        gm_file_exists(target_config) || gm_file_exists(target_global_graph) ||
        !gm_dir_ensure(input->target_root) || !gm_dir_ensure(projects_root) ||
        !gm_dir_ensure(project_root)) {
        *out_report_json =
            gm_migration_report("failed", &project, &source, NULL, NULL, NULL, 0, 0, payload_hash);
        sqlite3_close(source_memory);
        sqlite3_close(source_graph);
        sqlite3_close(source_config);
        return CBM_STORE_REJECTED;
    }
    gm_migration_invoke_test_hook("after-all-absent-precheck", target_paths, 4);
    gm_serialized_image_t images[4] = {{0}};
    if (rc == CBM_STORE_OK)
        rc = gm_backup_to_image(source_memory, &images[0]);
    if (rc == CBM_STORE_OK)
        rc = gm_backup_to_image(source_graph, &images[1]);
    if (rc == CBM_STORE_OK)
        rc = gm_backup_to_image(source_config, &images[2]);
    if (rc == CBM_STORE_OK)
        rc = gm_global_graph_image(&images[3]);
    if (rc == CBM_STORE_OK)
        rc = gm_publish_images_atomic(target_paths, images, 4);
    for (size_t image_index = 0; image_index < 4; image_index++)
        gm_serialized_image_reset(&images[image_index]);
    if (rc != CBM_STORE_OK) {
        *out_report_json =
            gm_migration_report("failed", &project, &source, NULL, NULL, NULL, 0, 0, payload_hash);
        sqlite3_close(source_memory);
        sqlite3_close(source_graph);
        sqlite3_close(source_config);
        return rc;
    }
    gm_logical_check_t backup = {0};
    if (rc == CBM_STORE_OK) {
        sqlite3 *copy_memory = NULL, *copy_graph = NULL, *copy_config = NULL;
        if (sqlite3_open_v2(target_memory, &copy_memory, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
            sqlite3_open_v2(target_graph, &copy_graph, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
            sqlite3_open_v2(target_config, &copy_config, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
            gm_collect_triple_checks(copy_memory, copy_graph, copy_config, &backup) !=
                CBM_STORE_OK ||
            !gm_checks_equal(&source, &backup))
            rc = CBM_STORE_ERR;
        if (copy_memory)
            sqlite3_close(copy_memory);
        if (copy_graph)
            sqlite3_close(copy_graph);
        if (copy_config)
            sqlite3_close(copy_config);
    }
    cbm_global_memory_t *global =
        rc == CBM_STORE_OK ? cbm_global_memory_open(target_memory, target_global_graph) : NULL;
    if (!global)
        rc = CBM_STORE_ERR;
    int aliases = 0, memories = 0;
    gm_logical_check_t target = {0};
    gm_projection_equivalence_t projection = {0};
    sqlite3 *target_graph_db = NULL, *target_config_db = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_open_v2(target_graph, &target_graph_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK &&
        sqlite3_open_v2(target_config, &target_config_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        sqlite3 *db = cbm_global_memory_db(global);
        if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
            rc = CBM_STORE_ERR;
        char now[40];
        gm_timestamp(now);
        sqlite3_stmt *stmt = NULL;
        char *catalog_report = NULL;
        if (rc == CBM_STORE_OK) {
            int catalog_rc = gm_ensure_project_locked(global->memory, &project,
                                                      input->idempotency_key, &catalog_report);
            if (catalog_rc != CBM_STORE_OK && catalog_rc != CBM_STORE_REPLAYED)
                rc = catalog_rc;
        }
        free(catalog_report);
        if (rc == CBM_STORE_OK &&
            sqlite3_prepare_v2(
                db,
                "INSERT OR IGNORE INTO "
                "global_legacy_alias(legacy_kind,legacy_id,global_id,project_uuid,payload_sha256,"
                "created_at) SELECT 'project',scope_project,?1,?1,?2,?3 FROM memory_item WHERE "
                "scope_project IS NOT NULL GROUP BY scope_project;",
                -1, &stmt, NULL) == SQLITE_OK) {
            gm_bind_text(stmt, 1, project.project_uuid);
            gm_bind_text(stmt, 2, payload_hash);
            gm_bind_text(stmt, 3, now);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
            aliases = sqlite3_changes(db);
        } else if (rc == CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc == CBM_STORE_OK &&
            sqlite3_prepare_v2(db,
                               "INSERT OR IGNORE INTO "
                               "global_memory_provenance(memory_item_id,project_uuid,legacy_"
                               "project_id,source_kind,payload_sha256,created_at) SELECT "
                               "id,?1,scope_project,'legacy_migration',?2,?3 FROM memory_item;",
                               -1, &stmt, NULL) == SQLITE_OK) {
            gm_bind_text(stmt, 1, project.project_uuid);
            gm_bind_text(stmt, 2, payload_hash);
            gm_bind_text(stmt, 3, now);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
            memories = sqlite3_changes(db);
        } else if (rc == CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc == CBM_STORE_OK &&
            sqlite3_prepare_v2(
                db,
                "INSERT INTO "
                "global_migration_ledger(migration_id,source_store_id,source_logical_sha256,target_"
                "store_id,target_logical_sha256,state,idempotency_key,payload_sha256,created_at) "
                "VALUES(?1,?2,?3,?4,'pending-post-check','staged',?5,?6,?7);",
                -1, &stmt, NULL) == SQLITE_OK) {
            char migration_id[48];
            snprintf(migration_id, sizeof(migration_id), "mig-%.32s", payload_hash);
            gm_bind_text(stmt, 1, migration_id);
            gm_bind_text(stmt, 2, input->source_memory_path);
            gm_bind_text(stmt, 3, source.canonical_logical_sha256);
            gm_bind_text(stmt, 4, target_memory);
            gm_bind_text(stmt, 5, input->idempotency_key);
            gm_bind_text(stmt, 6, payload_hash);
            gm_bind_text(stmt, 7, now);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        } else if (rc == CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc == CBM_STORE_OK)
            rc = gm_collect_triple_projection(db, target_graph_db, target_config_db, source_memory,
                                              source_graph, source_config, &target);
        if (rc == CBM_STORE_OK) {
            projection.memory = gm_projection_matches(db, source_memory, "memory");
            projection.graph = gm_projection_matches(target_graph_db, source_graph, "graph");
            projection.config = gm_projection_matches(target_config_db, source_config, "config");
            if (!projection.memory || !projection.graph || !projection.config)
                rc = CBM_STORE_ERR;
        }
        gm_logical_check_t global_health = {0};
        if (rc == CBM_STORE_OK)
            rc = gm_collect_db_checks(cbm_global_graph_db(global), "global_graph", &global_health,
                                      0);
        if (rc == CBM_STORE_OK && gm_checks_equal(&source, &backup) &&
            gm_checks_equal(&source, &target) && !strcmp(global_health.quick_check, "ok") &&
            global_health.foreign_key_violations == 0) {
            if (sqlite3_prepare_v2(
                    db,
                    "UPDATE global_migration_ledger SET target_logical_sha256=?1,state='applied' "
                    "WHERE idempotency_key=?2 AND state='staged';",
                    -1, &stmt, NULL) == SQLITE_OK) {
                gm_bind_text(stmt, 1, target.canonical_logical_sha256);
                gm_bind_text(stmt, 2, input->idempotency_key);
                rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                                   : CBM_STORE_ERR;
            } else
                rc = CBM_STORE_ERR;
            sqlite3_finalize(stmt);
        } else if (rc == CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK)
            rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK ? CBM_STORE_OK
                                                                            : CBM_STORE_ERR;
        else
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    if (target_graph_db)
        sqlite3_close(target_graph_db);
    if (target_config_db)
        sqlite3_close(target_config_db);
    if (global)
        cbm_global_memory_close(global);
    sqlite3_close(source_memory);
    sqlite3_close(source_graph);
    sqlite3_close(source_config);
    *out_report_json = gm_migration_report(rc == CBM_STORE_OK ? "applied" : "failed", &project,
                                           &source, backup.quick_check[0] ? &backup : NULL,
                                           rc == CBM_STORE_OK ? &target : NULL, &projection,
                                           aliases, memories, payload_hash);
    return rc;
}
