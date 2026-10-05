#include "ui/admin_operations.h"

#include "foundation/compat.h"
#include "foundation/platform.h"
#include "foundation/str_util.h"
#include "memory/memory_store.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define ADMIN_PATH_CAP 2048
#define ADMIN_SIGNATURE_CAP (1024 * 1024)

typedef struct {
    const char *role;
    char path[ADMIN_PATH_CAP];
    char filename[512];
} admin_store_path_t;

static char *admin_write(yyjson_mut_doc *doc) {
    char *json = doc ? yyjson_mut_write(doc, 0, NULL) : NULL;
    yyjson_mut_doc_free(doc);
    return json;
}

static char *admin_error(const char *code) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-manager-result/v1");
    yyjson_mut_obj_add_str(doc, root, "status", "error");
    yyjson_mut_obj_add_str(doc, root, "code", code ? code : "MANAGER_ERROR");
    return admin_write(doc);
}

static bool admin_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool admin_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static bool admin_parent_dir(char *path) {
    if (!path || !path[0])
        return false;
    cbm_normalize_path_sep(path);
    size_t length = strlen(path);
    while (length > 3 && path[length - 1] == '/')
        path[--length] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    if (slash == path + 2 && path[1] == ':') {
        slash[1] = '\0';
        return true;
    }
    *slash = '\0';
    return path[0] != '\0';
}

static bool admin_find_project_cache(const char *start, char output[ADMIN_PATH_CAP]) {
    if (!start || !start[0] || !output)
        return false;
    char cursor[ADMIN_PATH_CAP];
    snprintf(cursor, sizeof(cursor), "%s", start);
    cbm_normalize_path_sep(cursor);
    for (int depth = 0; depth < 12; depth++) {
        char candidate[ADMIN_PATH_CAP];
        int written = snprintf(candidate, sizeof(candidate), "%s/runtime-data/codex-mcp", cursor);
        if (written > 0 && (size_t)written < sizeof(candidate) && admin_is_dir(candidate)) {
            snprintf(output, ADMIN_PATH_CAP, "%s", candidate);
            return true;
        }
        if (!admin_parent_dir(cursor))
            break;
    }
    return false;
}

static const char *admin_resolve_cache(char output[ADMIN_PATH_CAP]) {
    char configured[ADMIN_PATH_CAP] = {0};
    cbm_safe_getenv("CBM_CACHE_DIR", configured, sizeof(configured), NULL);
    if (configured[0]) {
        snprintf(output, ADMIN_PATH_CAP, "%s", configured);
        cbm_normalize_path_sep(output);
        return output;
    }

    char cwd[ADMIN_PATH_CAP] = {0};
#ifdef _WIN32
    DWORD cwd_length = GetCurrentDirectoryA((DWORD)sizeof(cwd), cwd);
    if (cwd_length > 0 && cwd_length < sizeof(cwd) && admin_find_project_cache(cwd, output))
        return output;

    char executable[ADMIN_PATH_CAP] = {0};
    DWORD executable_length = GetModuleFileNameA(NULL, executable, (DWORD)sizeof(executable));
    if (executable_length > 0 && executable_length < sizeof(executable) &&
        admin_parent_dir(executable) && admin_find_project_cache(executable, output))
        return output;
#else
    if (getcwd(cwd, sizeof(cwd)) && admin_find_project_cache(cwd, output))
        return output;

    char executable[ADMIN_PATH_CAP] = {0};
    ssize_t executable_length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (executable_length > 0) {
        executable[executable_length] = '\0';
        if (admin_parent_dir(executable) && admin_find_project_cache(executable, output))
            return output;
    }
#endif

    const char *fallback = cbm_resolve_cache_dir();
    if (!fallback || !fallback[0])
        return NULL;
    snprintf(output, ADMIN_PATH_CAP, "%s", fallback);
    cbm_normalize_path_sep(output);
    return output;
}

static int64_t admin_size(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

static void admin_stamp(char output[32]) {
    time_t shifted = time(NULL) + 8 * 60 * 60;
    struct tm value;
#ifdef _WIN32
    gmtime_s(&value, &shifted);
#else
    gmtime_r(&shifted, &value);
#endif
    strftime(output, 32, "%Y%m%d_%H%M%S", &value);
}

static bool admin_paths(const char *project, admin_store_path_t stores[3]) {
    if (!project || !cbm_validate_project_name(project))
        return false;
    char cache_path[ADMIN_PATH_CAP];
    const char *cache = admin_resolve_cache(cache_path);
    if (!cache || !cache[0])
        return false;
    stores[0].role = "graph";
    stores[1].role = "memory";
    stores[2].role = "config";
    snprintf(stores[0].filename, sizeof(stores[0].filename), "%s.db", project);
    snprintf(stores[1].filename, sizeof(stores[1].filename), "%s-memory.db", project);
    snprintf(stores[2].filename, sizeof(stores[2].filename), "_config.db");
    snprintf(stores[0].path, sizeof(stores[0].path), "%s/%s", cache, stores[0].filename);
    snprintf(stores[1].path, sizeof(stores[1].path), "%s/%s", cache, stores[1].filename);
    snprintf(stores[2].path, sizeof(stores[2].path), "%s/%s", cache, stores[2].filename);
    return true;
}

static void admin_add_sources(yyjson_mut_doc *doc, yyjson_mut_val *candidate, sqlite3 *db,
                              const char *candidate_id) {
    yyjson_mut_val *sources = yyjson_mut_arr(doc);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT source_type,source_rank,normalized_score FROM retrieval_candidate_source "
        "WHERE candidate_id=?1 ORDER BY source_rank,source_type LIMIT 32;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *source = yyjson_mut_obj(doc);
            const char *type = (const char *)sqlite3_column_text(stmt, 0);
            yyjson_mut_obj_add_strcpy(doc, source, "type", type ? type : "unknown");
            if (sqlite3_column_type(stmt, 1) == SQLITE_NULL)
                yyjson_mut_obj_add_null(doc, source, "rank");
            else
                yyjson_mut_obj_add_int(doc, source, "rank", sqlite3_column_int(stmt, 1));
            if (sqlite3_column_type(stmt, 2) == SQLITE_NULL)
                yyjson_mut_obj_add_null(doc, source, "score");
            else
                yyjson_mut_obj_add_real(doc, source, "score", sqlite3_column_double(stmt, 2));
            yyjson_mut_arr_append(sources, source);
        }
    }
    sqlite3_finalize(stmt);
    yyjson_mut_obj_add_val(doc, candidate, "sources", sources);
}

static void admin_add_candidates(yyjson_mut_doc *doc, yyjson_mut_val *task, sqlite3 *db,
                                 const char *retrieval_session_id) {
    yyjson_mut_val *candidates = yyjson_mut_arr(doc);
    yyjson_mut_val *edges = yyjson_mut_arr(doc);
    if (!retrieval_session_id || !retrieval_session_id[0]) {
        yyjson_mut_obj_add_val(doc, task, "candidates", candidates);
        yyjson_mut_obj_add_val(doc, task, "edges", edges);
        return;
    }

    sqlite3_stmt *stmt = NULL;
    const char *candidate_sql =
        "SELECT id,memory_item_id,source_store_kind,aggregate_score,aggregate_rank,decision_status "
        "FROM retrieval_candidate WHERE session_id=?1 ORDER BY aggregate_rank,id LIMIT 100;";
    if (sqlite3_prepare_v2(db, candidate_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, retrieval_session_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *candidate = yyjson_mut_obj(doc);
            const char *id = (const char *)sqlite3_column_text(stmt, 0);
            const char *memory_id = (const char *)sqlite3_column_text(stmt, 1);
            const char *store_kind = (const char *)sqlite3_column_text(stmt, 2);
            const char *decision = (const char *)sqlite3_column_text(stmt, 5);
            yyjson_mut_obj_add_strcpy(doc, candidate, "id", id ? id : "");
            yyjson_mut_obj_add_strcpy(doc, candidate, "memory_item_id", memory_id ? memory_id : "");
            yyjson_mut_obj_add_strcpy(doc, candidate, "store_kind",
                                      store_kind ? store_kind : "unknown");
            if (sqlite3_column_type(stmt, 3) == SQLITE_NULL)
                yyjson_mut_obj_add_null(doc, candidate, "score");
            else
                yyjson_mut_obj_add_real(doc, candidate, "score", sqlite3_column_double(stmt, 3));
            if (sqlite3_column_type(stmt, 4) == SQLITE_NULL)
                yyjson_mut_obj_add_null(doc, candidate, "rank");
            else
                yyjson_mut_obj_add_int(doc, candidate, "rank", sqlite3_column_int(stmt, 4));
            yyjson_mut_obj_add_strcpy(doc, candidate, "decision", decision ? decision : "unknown");
            admin_add_sources(doc, candidate, db, id ? id : "");
            yyjson_mut_arr_append(candidates, candidate);
        }
    }
    sqlite3_finalize(stmt);

    const char *edge_sql =
        "SELECT id,from_candidate_id,to_candidate_id,relation_type,hop_depth,visit_status "
        "FROM retrieval_edge_visit WHERE session_id=?1 ORDER BY hop_depth,id LIMIT 200;";
    if (sqlite3_prepare_v2(db, edge_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, retrieval_session_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *edge = yyjson_mut_obj(doc);
            const char *keys[] = {"id", "from", "to", "relation", "visit_status"};
            const int columns[] = {0, 1, 2, 3, 5};
            for (int index = 0; index < 5; index++) {
                const char *value = (const char *)sqlite3_column_text(stmt, columns[index]);
                if (value)
                    yyjson_mut_obj_add_strcpy(doc, edge, keys[index], value);
                else
                    yyjson_mut_obj_add_null(doc, edge, keys[index]);
            }
            yyjson_mut_obj_add_int(doc, edge, "hop_depth", sqlite3_column_int(stmt, 4));
            yyjson_mut_arr_append(edges, edge);
        }
    }
    sqlite3_finalize(stmt);
    yyjson_mut_obj_add_val(doc, task, "candidates", candidates);
    yyjson_mut_obj_add_val(doc, task, "edges", edges);
}

static void admin_add_memories(yyjson_mut_doc *doc, yyjson_mut_val *task, sqlite3 *db,
                               const char *task_id) {
    yyjson_mut_val *memories = yyjson_mut_arr(doc);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT a.memory_item_id,a.attribution_state,a.evidence_id,a.feedback_event_id,"
        "substr(COALESCE(m.summary,''),1,512),m.kind,m.scope_project,m.version,m.status,"
        "COALESCE(m.entity_key,''),a.created_at FROM codex_task_attribution a "
        "JOIN memory_item m ON m.id=a.memory_item_id WHERE a.task_id=?1 "
        "ORDER BY a.created_at,a.attribution_id LIMIT 100;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *memory = yyjson_mut_obj(doc);
            const char *keys[] = {"id",          "attribution_state",
                                  "evidence_id", "feedback_event_id",
                                  "summary",     "kind",
                                  "scope",       "status",
                                  "entity_key",  "created_at"};
            const int columns[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 10};
            for (int index = 0; index < 10; index++) {
                const char *value = (const char *)sqlite3_column_text(stmt, columns[index]);
                if (value)
                    yyjson_mut_obj_add_strcpy(doc, memory, keys[index], value);
                else
                    yyjson_mut_obj_add_null(doc, memory, keys[index]);
            }
            yyjson_mut_obj_add_int(doc, memory, "version", sqlite3_column_int(stmt, 7));
            yyjson_mut_arr_append(memories, memory);
        }
    }
    sqlite3_finalize(stmt);
    yyjson_mut_obj_add_val(doc, task, "memories", memories);
}

static bool admin_scalar_text(sqlite3 *db, const char *sql, const char *expected) {
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
              sqlite3_step(stmt) == SQLITE_ROW;
    const char *value = ok ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
    ok = ok && value && strcmp(value, expected) == 0;
    sqlite3_finalize(stmt);
    return ok;
}

static int admin_fk_count(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &stmt, NULL) == SQLITE_OK) {
        count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW)
            count++;
    }
    sqlite3_finalize(stmt);
    return count;
}

static char *admin_db_signature(sqlite3 *db) {
    char *signature = malloc(ADMIN_SIGNATURE_CAP);
    if (!signature)
        return NULL;
    signature[0] = '\0';
    size_t used = 0;
    sqlite3_stmt *tables = NULL;
    const char *sql = "SELECT name,sql FROM sqlite_master WHERE type='table' AND name NOT LIKE "
                      "'sqlite_%' ORDER BY name;";
    if (sqlite3_prepare_v2(db, sql, -1, &tables, NULL) != SQLITE_OK) {
        free(signature);
        return NULL;
    }
    while (sqlite3_step(tables) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(tables, 0);
        const char *schema = (const char *)sqlite3_column_text(tables, 1);
        if (!name || !schema)
            continue;
        char query[1024];
        snprintf(query, sizeof(query), "SELECT COUNT(*) FROM \"%s\";", name);
        sqlite3_stmt *count_stmt = NULL;
        int64_t count = -1;
        if (sqlite3_prepare_v2(db, query, -1, &count_stmt, NULL) == SQLITE_OK &&
            sqlite3_step(count_stmt) == SQLITE_ROW)
            count = sqlite3_column_int64(count_stmt, 0);
        sqlite3_finalize(count_stmt);
        int wrote = snprintf(signature + used, ADMIN_SIGNATURE_CAP - used, "%s\n%s\n%lld\n", name,
                             schema, (long long)count);
        if (wrote < 0 || (size_t)wrote >= ADMIN_SIGNATURE_CAP - used) {
            sqlite3_finalize(tables);
            free(signature);
            return NULL;
        }
        used += (size_t)wrote;
    }
    sqlite3_finalize(tables);
    return signature;
}

static bool admin_db_verified(const char *path, char **signature_out, int *fk_out) {
    if (signature_out)
        *signature_out = NULL;
    if (fk_out)
        *fk_out = -1;
    sqlite3 *db = NULL;
    if (!path || sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 3000);
    bool quick = admin_scalar_text(db, "PRAGMA quick_check;", "ok");
    int fk = admin_fk_count(db);
    char *signature = quick && fk == 0 ? admin_db_signature(db) : NULL;
    sqlite3_close(db);
    if (fk_out)
        *fk_out = fk;
    if (signature_out)
        *signature_out = signature;
    else
        free(signature);
    return quick && fk == 0 && signature != NULL;
}

static bool admin_online_copy(const char *source, const char *destination) {
    sqlite3 *src = NULL, *dst = NULL;
    sqlite3_backup *backup = NULL;
    bool ok = false;
    if (sqlite3_open_v2(source, &src, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        goto done;
    if (sqlite3_open_v2(destination, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) !=
        SQLITE_OK)
        goto done;
    sqlite3_busy_timeout(src, 3000);
    sqlite3_busy_timeout(dst, 3000);
    backup = sqlite3_backup_init(dst, "main", src, "main");
    if (!backup)
        goto done;
    int rc;
    do {
        rc = sqlite3_backup_step(backup, 256);
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED)
            sqlite3_sleep(25);
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    ok = rc == SQLITE_DONE && sqlite3_backup_finish(backup) == SQLITE_OK;
    backup = NULL;
done:
    if (backup)
        sqlite3_backup_finish(backup);
    sqlite3_close(dst);
    sqlite3_close(src);
    return ok;
}

static bool admin_copy_verified(const char *source, const char *destination) {
    char *before = NULL, *after = NULL;
    int before_fk = -1, after_fk = -1;
    bool ok = admin_db_verified(source, &before, &before_fk) &&
              admin_online_copy(source, destination) &&
              admin_db_verified(destination, &after, &after_fk) && before && after &&
              strcmp(before, after) == 0 && before_fk == 0 && after_fk == 0;
    free(before);
    free(after);
    return ok;
}

char *cbm_admin_health_json(const char *project, const char *version) {
    admin_store_path_t stores[3] = {0};
    if (!admin_paths(project, stores))
        return admin_error("INVALID_PROJECT");
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root)
        return admin_write(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-manager-health/v1");
    yyjson_mut_obj_add_str(doc, root, "status", "ok");
    yyjson_mut_obj_add_str(doc, root, "version", version ? version : "unknown");
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_val *modes = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, modes, "security", "on");
    yyjson_mut_obj_add_str(doc, modes, "plasticity", "off");
    yyjson_mut_obj_add_bool(doc, modes, "active", false);
    yyjson_mut_obj_add_bool(doc, modes, "automatic_maintenance", false);
    yyjson_mut_obj_add_bool(doc, modes, "global_union", false);
    yyjson_mut_obj_add_val(doc, root, "modes", modes);
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    for (int i = 0; i < 3; i++) {
        char *signature = NULL;
        int fk = -1;
        bool verified =
            admin_exists(stores[i].path) && admin_db_verified(stores[i].path, &signature, &fk);
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "role", stores[i].role);
        yyjson_mut_obj_add_str(doc, item, "filename", stores[i].filename);
        yyjson_mut_obj_add_sint(doc, item, "size_bytes", admin_size(stores[i].path));
        yyjson_mut_obj_add_str(doc, item, "quick_check", verified ? "ok" : "unavailable");
        yyjson_mut_obj_add_int(doc, item, "foreign_key_violations", fk);
        yyjson_mut_obj_add_bool(doc, item, "wal_capable", true);
        yyjson_mut_arr_append(items, item);
        free(signature);
    }
    yyjson_mut_obj_add_val(doc, root, "stores", items);
    return admin_write(doc);
}

char *cbm_admin_tasks_json(const char *project, int limit) {
    admin_store_path_t stores[3] = {0};
    if (!admin_paths(project, stores))
        return admin_error("INVALID_PROJECT");
    if (limit < 1)
        limit = 25;
    if (limit > 100)
        limit = 100;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(stores[1].path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return admin_error("MEMORY_UNAVAILABLE");
    }
    sqlite3_busy_timeout(db, 3000);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        sqlite3_close(db);
        return admin_write(doc);
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-manager-tasks/v1");
    yyjson_mut_obj_add_str(doc, root, "status", "ok");
    yyjson_mut_val *tasks = yyjson_mut_arr(doc);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT l.task_id,l.session_id,l.turn_id,l.state,l.outcome,l.retrieval_session_id,"
        "l.created_at,(SELECT COUNT(*) FROM memory_evidence e WHERE e.task_id=l.task_id),"
        "(SELECT COUNT(*) FROM codex_task_attribution a WHERE a.task_id=l.task_id),"
        "(SELECT COUNT(*) FROM retrieval_candidate c WHERE c.session_id=l.retrieval_session_id),"
        "(SELECT COUNT(*) FROM retrieval_candidate_source s JOIN retrieval_candidate c ON "
        "c.id=s.candidate_id WHERE c.session_id=l.retrieval_session_id),"
        "(SELECT COUNT(*) FROM codex_task_attribution a WHERE a.task_id=l.task_id "
        "AND a.feedback_event_id IS NOT NULL),"
        "CAST(MAX(0,(julianday(l.created_at)-julianday(t.created_at))*86400000) AS INTEGER) "
        "FROM codex_task_lifecycle l JOIN memory_task t ON t.task_id=l.task_id "
        "WHERE t.project=?1 AND l.rowid=(SELECT MAX(x.rowid) FROM codex_task_lifecycle x "
        "WHERE x.task_id=l.task_id) ORDER BY l.rowid DESC LIMIT ?2;";
    bool schema_ready = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK;
    if (schema_ready) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            const char *keys[] = {"task_id", "session_id",           "turn_id",   "state",
                                  "outcome", "retrieval_session_id", "updated_at"};
            for (int col = 0; col < 7; col++) {
                const char *value = (const char *)sqlite3_column_text(stmt, col);
                if (value)
                    yyjson_mut_obj_add_strcpy(doc, item, keys[col], value);
                else
                    yyjson_mut_obj_add_null(doc, item, keys[col]);
            }
            yyjson_mut_obj_add_int(doc, item, "evidence_count", sqlite3_column_int(stmt, 7));
            yyjson_mut_obj_add_int(doc, item, "attribution_count", sqlite3_column_int(stmt, 8));
            yyjson_mut_obj_add_int(doc, item, "candidate_count", sqlite3_column_int(stmt, 9));
            yyjson_mut_obj_add_int(doc, item, "source_count", sqlite3_column_int(stmt, 10));
            int feedback_count = sqlite3_column_int(stmt, 11);
            const char *feedback = feedback_count > 0                ? "recorded"
                                   : sqlite3_column_int(stmt, 8) > 0 ? "pending"
                                                                     : "none";
            yyjson_mut_obj_add_str(doc, item, "feedback_disposition", feedback);
            yyjson_mut_obj_add_sint(doc, item, "duration_ms", sqlite3_column_int64(stmt, 12));
            const char *task_id = (const char *)sqlite3_column_text(stmt, 0);
            const char *retrieval_session = (const char *)sqlite3_column_text(stmt, 5);
            admin_add_candidates(doc, item, db, retrieval_session);
            admin_add_memories(doc, item, db, task_id ? task_id : "");
            yyjson_mut_arr_append(tasks, item);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    yyjson_mut_obj_add_bool(doc, root, "schema_ready", schema_ready);
    yyjson_mut_obj_add_val(doc, root, "tasks", tasks);
    return admin_write(doc);
}

char *cbm_admin_create_backup_json(const char *project, const char *destination_root) {
    admin_store_path_t stores[3] = {0};
    if (!admin_paths(project, stores) || !destination_root || !destination_root[0])
        return admin_error("INVALID_ARGUMENT");
    if (!admin_is_dir(destination_root))
        return admin_error("DESTINATION_NOT_FOUND");
    char stamp[32], directory[ADMIN_PATH_CAP];
    admin_stamp(stamp);
    snprintf(directory, sizeof(directory), "%s/stage12-backup-%s", destination_root, stamp);
    if (admin_exists(directory) || cbm_mkdir(directory) != 0)
        return admin_error("DESTINATION_CONFLICT");
    bool all_ok = true;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root)
        return admin_write(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-manager-backup/v1");
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    for (int i = 0; i < 3; i++) {
        char destination[ADMIN_PATH_CAP];
        snprintf(destination, sizeof(destination), "%s/%s", directory, stores[i].filename);
        bool ok = admin_exists(stores[i].path) && admin_copy_verified(stores[i].path, destination);
        all_ok = all_ok && ok;
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "role", stores[i].role);
        yyjson_mut_obj_add_str(doc, item, "filename", stores[i].filename);
        yyjson_mut_obj_add_bool(doc, item, "verified", ok);
        yyjson_mut_obj_add_sint(doc, item, "size_bytes", admin_size(destination));
        yyjson_mut_arr_append(items, item);
    }
    yyjson_mut_obj_add_str(doc, root, "status", all_ok ? "ok" : "error");
    yyjson_mut_obj_add_str(doc, root, "code", all_ok ? "OK" : "BACKUP_VERIFY_FAILED");
    yyjson_mut_obj_add_str(doc, root, "directory_name",
                           strrchr(directory, '/') ? strrchr(directory, '/') + 1 : directory);
    yyjson_mut_obj_add_val(doc, root, "stores", items);
    return admin_write(doc);
}

char *cbm_admin_verify_restore_json(const char *project, const char *source_directory,
                                    const char *target_directory) {
    admin_store_path_t stores[3] = {0};
    if (!admin_paths(project, stores) || !source_directory || !target_directory ||
        !admin_is_dir(source_directory) || admin_exists(target_directory))
        return admin_error("NEW_DIRECTORY_REQUIRED");
    if (cbm_mkdir(target_directory) != 0)
        return admin_error("TARGET_CREATE_FAILED");
    bool all_ok = true;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root)
        return admin_write(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage12-manager-restore-verification/v1");
    yyjson_mut_val *items = yyjson_mut_arr(doc);
    for (int i = 0; i < 3; i++) {
        char source[ADMIN_PATH_CAP], target[ADMIN_PATH_CAP];
        snprintf(source, sizeof(source), "%s/%s", source_directory, stores[i].filename);
        snprintf(target, sizeof(target), "%s/%s", target_directory, stores[i].filename);
        bool ok = admin_exists(source) && admin_copy_verified(source, target);
        all_ok = all_ok && ok;
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "role", stores[i].role);
        yyjson_mut_obj_add_str(doc, item, "filename", stores[i].filename);
        yyjson_mut_obj_add_bool(doc, item, "verified", ok);
        yyjson_mut_arr_append(items, item);
    }
    yyjson_mut_obj_add_str(doc, root, "status", all_ok ? "ok" : "error");
    yyjson_mut_obj_add_str(doc, root, "code", all_ok ? "OK" : "RESTORE_VERIFY_FAILED");
    yyjson_mut_obj_add_val(doc, root, "stores", items);
    return admin_write(doc);
}
