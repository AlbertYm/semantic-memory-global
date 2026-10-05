#include "memory/evolution_engine.h"
#include "memory/concept_growth.h"
#include "memory/edge_lifecycle.h"
#include "store/store.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#define EVO_MAX_ATTRIBUTIONS 16
#define EVO_MAX_TASK_EVENTS (EVO_MAX_ATTRIBUTIONS + 1)
#define EVO_MAX_TASK_MANIFEST_BYTES (1024 * 1024)
#define EVO_MAX_EDGE_MANIFEST_BYTES (1024 * 1024)
#define EVO_MAX_CONCEPT_MANIFEST_BYTES (4 * 1024 * 1024)
#define EVO_TASK_MANIFEST_SCHEMA "stage14-task-evolution-canary-manifest/v1"
#define EVO_TASK_PLAN_SCHEMA "stage14-task-evolution-plan/v1"
#define EVO_TASK_REQUEST_SCHEMA "stage14-task-evolution-request/v1"
#define EVO_EVENT_ALGORITHM_V2 "stage14-evolution-v2"
#define EVO_EVENT_ALGORITHM_V3 "stage14-evolution-v3"
#define EVO_EVENT_CONFIG_VERSION 2
#define EVO_ATOMIC_LOCK_TIMEOUT_MS 5000

typedef struct {
    char memory_item_id[256];
    char attribution_state[32];
    char evidence_id[256];
    char evidence_grade[2];
    char feedback_event_id[256];
    char operation[32];
    char source_project_uuid[128];
    char target_project_uuid[128];
    char status[32];
    double confidence;
    double reusability;
    double decay;
    double reward;
    double delta;
} evo_attribution_t;

typedef struct {
    evo_attribution_t rows[EVO_MAX_ATTRIBUTIONS];
    int count;
    int positive;
    int negative;
    int planned_evolution_events;
    int planned_cross_project_edges;
    int max_evolution_events;
    int max_cross_project_edges;
    char project_uuid[128];
    char request_sha256[65];
} evo_task_plan_t;

typedef struct {
#ifdef _WIN32
    HANDLE handle;
#else
    int memory_fd;
    int graph_fd;
#endif
} evo_cross_process_guard_t;

typedef struct {
    char main_journal[16];
    char graph_journal[16];
    int main_synchronous;
    int graph_synchronous;
    int captured;
    int attached_by_us;
    int prepared;
} evo_journal_guard_t;

static void evo_timestamp(time_t value, char out[40]) {
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &value);
#else
    gmtime_r(&value, &tmv);
#endif
    strftime(out, 40, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int evo_exec(sqlite3 *db, const char *sql) {
    return db && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? CBM_STORE_OK
                                                                      : CBM_STORE_ERR;
}

static void evo_bind_text(sqlite3_stmt *stmt, int index, const char *value) {
    if (value)
        sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, index);
}

static char *evo_dup(const char *value) {
    if (!value)
        return NULL;
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy)
        memcpy(copy, value, size);
    return copy;
}

static double evo_clamp(double value, double minimum, double maximum) {
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int evo_mode_valid(const char *mode) {
    return mode && (!strcmp(mode, "off") || !strcmp(mode, "shadow") || !strcmp(mode, "dry_run") ||
                    !strcmp(mode, "bounded_canary") || !strcmp(mode, "active"));
}

static int evo_mode_writes(const char *mode) {
    return mode && (!strcmp(mode, "bounded_canary") || !strcmp(mode, "active"));
}

static int evo_write_guard(const char *mode, int isolated, int production_gate) {
    if (!evo_mode_writes(mode))
        return 1;
    if (!strcmp(mode, "active"))
        return production_gate || isolated;
    return isolated || production_gate;
}

static char *evo_report(const char *status, const char *mode, int wrote, int eligible, int positive,
                        int negative, int events, int edges, int consolidated, int decayed,
                        int archived, const char *reason) {
    char *json = malloc(1280);
    if (json) {
        snprintf(json, 1280,
                 "{\"schema\":\"semantic-memory-evolution/v2\",\"status\":\"%s\","
                 "\"mode\":\"%s\",\"eligible\":%d,\"positive\":%d,\"negative\":%d,"
                 "\"wrote\":%s,\"evolution_events\":%d,\"cross_project_edges\":%d,"
                 "\"consolidated\":%d,\"decayed\":%d,\"archived\":%d,"
                 "\"hard_delete\":false,\"reason\":\"%s\"}",
                 status ? status : "error", mode ? mode : "", eligible, positive, negative,
                 wrote ? "true" : "false", events, edges, consolidated, decayed, archived,
                 reason ? reason : "");
    }
    return json;
}

static char *evo_maintenance_report(const char *status, const char *mode,
                                    const cbm_evolution_result_t *out, const char *phase,
                                    const char *reason) {
    char *json = malloc(2048);
    if (json) {
        snprintf(json, 2048,
                 "{\"schema\":\"semantic-memory-maintenance/v3\",\"status\":\"%s\","
                 "\"mode\":\"%s\",\"phase\":\"%s\",\"wrote\":%s,\"checkpointed\":%s,"
                 "\"eligible\":%d,\"consolidated\":%d,\"decayed\":%d,\"archived\":%d,"
                 "\"edge_decisions\":%d,\"edge_transitions\":%d,\"concept_eligible\":%d,"
                 "\"concept_proposed\":%d,\"evolution_events\":%d,\"hard_delete\":false,"
                 "\"reason\":\"%s\"}",
                 status ? status : "error", mode ? mode : "", phase ? phase : "",
                 out && out->wrote ? "true" : "false", out && out->checkpointed ? "true" : "false",
                 out ? out->eligible : 0, out ? out->consolidated : 0, out ? out->decayed : 0,
                 out ? out->archived : 0, out ? out->edge_decisions : 0,
                 out ? out->edge_transitions : 0, out ? out->concept_eligible : 0,
                 out ? out->concept_proposed : 0, out ? out->evolution_events : 0,
                 reason ? reason : "");
    }
    return json;
}

static int evo_hash_text(const char *text, char hash[65]) {
    return cbm_stage7_sha256_hex(text ? text : "", text ? strlen(text) : 0, hash);
}

static int evo_lower_sha256(const char *value) {
    if (!value || strlen(value) != 64)
        return 0;
    for (size_t i = 0; i < 64; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return 0;
    }
    return 1;
}

static int evo_absolute_path(const char *path) {
    if (!path || !path[0])
        return 0;
#ifdef _WIN32
    size_t length = strlen(path);
    if (length >= 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
        return 1;
    return length >= 2 &&
           ((path[0] == '\\' && path[1] == '\\') || (path[0] == '/' && path[1] == '/'));
#else
    return path[0] == '/';
#endif
}

static FILE *evo_open_manifest_file(const char *path) {
#ifdef _WIN32
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (count <= 0)
        return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (!wide)
        return NULL;
    FILE *file = NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) == count)
        file = _wfopen(wide, L"rb");
    free(wide);
    return file;
#else
    return fopen(path, "rb");
#endif
}

static char *evo_read_task_manifest(const char *path, size_t *out_size) {
    if (out_size)
        *out_size = 0;
    FILE *file = evo_open_manifest_file(path);
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length <= 0 || length > EVO_MAX_TASK_MANIFEST_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)length + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)length, file);
    int exact = read == (size_t)length && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0)
        exact = 0;
    if (!exact) {
        free(data);
        return NULL;
    }
    data[read] = '\0';
    if (out_size)
        *out_size = read;
    return data;
}

static int evo_manifest_raw_matches(const char *path, const char *expected_sha256,
                                    long maximum_bytes) {
    FILE *file = evo_open_manifest_file(path);
    if (!file)
        return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    long length = ftell(file);
    if (length <= 0 || length > maximum_bytes || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    unsigned char *bytes = malloc((size_t)length);
    int ok = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length &&
             fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0)
        ok = 0;
    char actual_sha256[65] = {0};
    if (ok)
        ok = cbm_stage7_sha256_hex(bytes, (size_t)length, actual_sha256) == CBM_STORE_OK &&
             strcmp(actual_sha256, expected_sha256) == 0;
    free(bytes);
    return ok;
}

static int evo_canonical_field(char *buffer, size_t capacity, size_t *used, const char *name,
                               const char *value) {
    if (!buffer || !used || !name || *used >= capacity)
        return 0;
    if (!value) {
        int null_used = snprintf(buffer + *used, capacity - *used, "%s:null\n", name);
        if (null_used < 0 || (size_t)null_used >= capacity - *used)
            return 0;
        *used += (size_t)null_used;
        return 1;
    }
    size_t value_size = strlen(value);
    int prefix = snprintf(buffer + *used, capacity - *used, "%s:%zu:", name, value_size);
    if (prefix < 0 || (size_t)prefix >= capacity - *used)
        return 0;
    *used += (size_t)prefix;
    if (value_size + 2 > capacity - *used)
        return 0;
    memcpy(buffer + *used, value, value_size);
    *used += value_size;
    buffer[(*used)++] = '\n';
    buffer[*used] = '\0';
    return 1;
}

static int evo_event_hash_v2(const char *previous, const char *payload_hash,
                             const char *before_hash, const char *after_hash,
                             const char *object_kind, const char *object_id, char out[65]) {
    char seed[1024];
    snprintf(seed, sizeof(seed), "%s\n%s\n%s\n%s\n%s\n%s", previous ? previous : "",
             payload_hash ? payload_hash : "", before_hash ? before_hash : "",
             after_hash ? after_hash : "", object_kind ? object_kind : "",
             object_id ? object_id : "");
    return evo_hash_text(seed, out);
}

static int evo_event_hash_v3(sqlite3_int64 sequence_no, const char *previous, const char *task_id,
                             const char *project_uuid, const char *object_kind,
                             const char *object_id, const char *operation,
                             const char *evidence_grade, const char *evidence_id,
                             const char *before_hash, const char *after_hash,
                             const char *algorithm_version, int config_version,
                             const char *idempotency_key, const char *payload_hash,
                             const char *created_at, char out[65]) {
    static const char *names[] = {
        "schema",         "sequence_no",  "prev_hash",         "task_id",        "project_uuid",
        "object_kind",    "object_id",    "operation",         "evidence_grade", "evidence_id",
        "before_sha256",  "after_sha256", "algorithm_version", "config_version", "idempotency_key",
        "payload_sha256", "created_at"};
    char sequence[32], config[32];
    snprintf(sequence, sizeof(sequence), "%lld", (long long)sequence_no);
    snprintf(config, sizeof(config), "%d", config_version);
    const char *values[] = {"stage14-global-evolution-event/v3",
                            sequence,
                            previous,
                            task_id,
                            project_uuid,
                            object_kind,
                            object_id,
                            operation,
                            evidence_grade,
                            evidence_id,
                            before_hash,
                            after_hash,
                            algorithm_version,
                            config,
                            idempotency_key,
                            payload_hash,
                            created_at};
    size_t capacity = 1;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        size_t name_size = strlen(names[i]);
        size_t value_size = values[i] ? strlen(values[i]) : 0;
        if (name_size > SIZE_MAX - value_size - 96 ||
            capacity > SIZE_MAX - name_size - value_size - 96)
            return CBM_STORE_ERR;
        capacity += name_size + value_size + 96;
    }
    char *canonical = calloc(capacity, 1);
    if (!canonical)
        return CBM_STORE_ERR;
    size_t used = 0;
    int rc = CBM_STORE_OK;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (!evo_canonical_field(canonical, capacity, &used, names[i], values[i])) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    if (rc == CBM_STORE_OK)
        rc = cbm_stage7_sha256_hex(canonical, used, out);
    free(canonical);
    return rc;
}

static int evo_event_lookup(sqlite3 *db, const char *key, const char *payload_hash,
                            int *out_exact) {
    sqlite3_stmt *stmt = NULL;
    *out_exact = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT payload_sha256 FROM global_evolution_event "
                           "WHERE idempotency_key=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(stmt, 0);
        *out_exact = stored && strcmp(stored, payload_hash) == 0;
    }
    sqlite3_finalize(stmt);
    if (step == SQLITE_ROW)
        return CBM_STORE_OK;
    if (step == SQLITE_DONE)
        return CBM_STORE_NOT_FOUND;
    return CBM_STORE_ERR;
}

int cbm_evolution_event_lookup_for_test(sqlite3 *db, const char *key, const char *payload_hash,
                                        int *out_exact) {
    return db && key && payload_hash && out_exact
               ? evo_event_lookup(db, key, payload_hash, out_exact)
               : CBM_STORE_ERR;
}

static int evo_append_event(sqlite3 *db, const char *task_id, const char *project_uuid,
                            const char *object_kind, const char *object_id, const char *operation,
                            const char *evidence_grade, const char *evidence_id,
                            const char *before_hash, const char *after_hash,
                            const char *idempotency_key, const char *payload_hash,
                            char out_event_id[48]) {
    char previous[65];
    memset(previous, '0', 64);
    previous[64] = '\0';
    sqlite3_int64 sequence_no = 1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT sequence_no,event_hash FROM global_evolution_event "
                           "ORDER BY sequence_no DESC LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        sqlite3_int64 stored_sequence = sqlite3_column_int64(stmt, 0);
        const char *stored = (const char *)sqlite3_column_text(stmt, 1);
        if (stored_sequence < 1 || stored_sequence == LLONG_MAX || !stored ||
            strlen(stored) != 64) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        sequence_no = stored_sequence + 1;
        snprintf(previous, sizeof(previous), "%s", stored);
    } else if (step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);

    char event_hash[65], now[40];
    evo_timestamp(time(NULL), now);
    if (evo_event_hash_v3(sequence_no, previous, task_id, project_uuid, object_kind, object_id,
                          operation, evidence_grade, evidence_id, before_hash, after_hash,
                          EVO_EVENT_ALGORITHM_V3, EVO_EVENT_CONFIG_VERSION, idempotency_key,
                          payload_hash, now, event_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    snprintf(out_event_id, 48, "evo-%.32s", event_hash);
    const char *sql =
        "INSERT INTO global_evolution_event(sequence_no,event_id,task_id,project_uuid,object_kind,"
        "object_id,operation,evidence_grade,evidence_id,before_sha256,after_sha256,"
        "algorithm_version,config_version,idempotency_key,payload_sha256,prev_hash,event_hash,"
        "created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,"
        "?14,?15,?16,?17,?18);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_int64(stmt, 1, sequence_no);
    const char *values[] = {out_event_id, task_id,        project_uuid, object_kind, object_id,
                            operation,    evidence_grade, evidence_id,  before_hash, after_hash};
    for (int i = 0; i < 10; i++)
        evo_bind_text(stmt, i + 2, values[i]);
    sqlite3_bind_text(stmt, 12, EVO_EVENT_ALGORITHM_V3, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 13, EVO_EVENT_CONFIG_VERSION);
    const char *tail[] = {idempotency_key, payload_hash, previous, event_hash, now};
    for (int i = 0; i < 5; i++)
        evo_bind_text(stmt, i + 14, tail[i]);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

int cbm_evolution_event_chain_verify_for_test(sqlite3 *db, int *out_bad_sequence) {
    if (out_bad_sequence)
        *out_bad_sequence = 0;
    if (!db || !out_bad_sequence)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT sequence_no,event_id,task_id,project_uuid,object_kind,object_id,operation,"
        "evidence_grade,evidence_id,before_sha256,after_sha256,algorithm_version,"
        "config_version,idempotency_key,payload_sha256,prev_hash,event_hash,created_at "
        "FROM global_evolution_event ORDER BY sequence_no;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    char expected_previous[65];
    memset(expected_previous, '0', 64);
    expected_previous[64] = '\0';
    int rc = CBM_STORE_OK;
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        sqlite3_int64 sequence = sqlite3_column_int64(stmt, 0);
        const char *event_id = (const char *)sqlite3_column_text(stmt, 1);
        const char *task_id = (const char *)sqlite3_column_text(stmt, 2);
        const char *project_uuid = (const char *)sqlite3_column_text(stmt, 3);
        const char *object_kind = (const char *)sqlite3_column_text(stmt, 4);
        const char *object_id = (const char *)sqlite3_column_text(stmt, 5);
        const char *operation = (const char *)sqlite3_column_text(stmt, 6);
        const char *evidence_grade = (const char *)sqlite3_column_text(stmt, 7);
        const char *evidence_id = (const char *)sqlite3_column_text(stmt, 8);
        const char *before_hash = (const char *)sqlite3_column_text(stmt, 9);
        const char *after_hash = (const char *)sqlite3_column_text(stmt, 10);
        const char *algorithm = (const char *)sqlite3_column_text(stmt, 11);
        int config_version = sqlite3_column_int(stmt, 12);
        const char *idempotency_key = (const char *)sqlite3_column_text(stmt, 13);
        const char *payload_hash = (const char *)sqlite3_column_text(stmt, 14);
        const char *previous = (const char *)sqlite3_column_text(stmt, 15);
        const char *stored_hash = (const char *)sqlite3_column_text(stmt, 16);
        const char *created_at = (const char *)sqlite3_column_text(stmt, 17);
        char computed[65] = {0};
        if (sequence < 1 || !event_id || !project_uuid || !object_kind || !object_id ||
            !operation || !evidence_grade || !before_hash || !after_hash || !algorithm ||
            !idempotency_key || !payload_hash || !previous || !stored_hash || !created_at ||
            strcmp(previous, expected_previous) != 0) {
            rc = CBM_STORE_ERR;
        } else if (!strcmp(algorithm, EVO_EVENT_ALGORITHM_V2)) {
            rc = evo_event_hash_v2(previous, payload_hash, before_hash, after_hash, object_kind,
                                   object_id, computed);
        } else if (!strcmp(algorithm, EVO_EVENT_ALGORITHM_V3)) {
            rc = evo_event_hash_v3(sequence, previous, task_id, project_uuid, object_kind,
                                   object_id, operation, evidence_grade, evidence_id, before_hash,
                                   after_hash, algorithm, config_version, idempotency_key,
                                   payload_hash, created_at, computed);
        } else {
            rc = CBM_STORE_ERR;
        }
        if (rc == CBM_STORE_OK && strcmp(computed, stored_hash) != 0)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK && !strcmp(algorithm, EVO_EVENT_ALGORITHM_V3)) {
            char expected_event_id[48];
            snprintf(expected_event_id, sizeof(expected_event_id), "evo-%.32s", stored_hash);
            if (strcmp(event_id, expected_event_id) != 0)
                rc = CBM_STORE_ERR;
        }
        if (rc != CBM_STORE_OK) {
            *out_bad_sequence = sequence > INT_MAX ? INT_MAX : (int)sequence;
            break;
        }
        snprintf(expected_previous, sizeof(expected_previous), "%s", stored_hash);
    }
    if (rc == CBM_STORE_OK && step != SQLITE_DONE)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static const char *evo_grade(const char *trust) {
    if (trust && !strcmp(trust, "external_verified"))
        return "A";
    if (trust && !strcmp(trust, "explicit_user"))
        return "B";
    return "C";
}

static void evo_classify(evo_attribution_t *row, const char *action, const char *usage_outcome,
                         const char *trust) {
    snprintf(row->evidence_grade, sizeof(row->evidence_grade), "%s", evo_grade(trust));
    if (action && !strcmp(action, "withdraw"))
        snprintf(row->operation, sizeof(row->operation), "withdraw");
    else if (action && !strcmp(action, "correct"))
        snprintf(row->operation, sizeof(row->operation), "correct");
    else if (!strcmp(row->attribution_state, "contradicted") ||
             (usage_outcome && !strcmp(usage_outcome, "contradicted")))
        snprintf(row->operation, sizeof(row->operation), "contradict");
    else if ((action && !strcmp(action, "reject")) || !strcmp(row->attribution_state, "rejected"))
        snprintf(row->operation, sizeof(row->operation), "reject");
    else if (row->reward < 0.0)
        snprintf(row->operation, sizeof(row->operation), "negative");
    else
        snprintf(row->operation, sizeof(row->operation), "positive");

    if (!strcmp(row->operation, "positive")) {
        row->delta = !strcmp(row->evidence_grade, "A")
                         ? 0.04
                         : (!strcmp(row->evidence_grade, "B") ? 0.02 : 0.0);
    } else if (!strcmp(row->operation, "negative"))
        row->delta = -0.02;
    else if (!strcmp(row->operation, "reject"))
        row->delta = -0.03;
    else if (!strcmp(row->operation, "contradict"))
        row->delta = -0.05;
    else if (!strcmp(row->operation, "withdraw"))
        row->delta = -0.02;
    else {
        row->delta = evo_clamp(row->reward * 0.04, -0.04, 0.04);
        if (row->delta > 0.0 && !strcmp(row->evidence_grade, "C"))
            row->delta = 0.0;
    }
}

static int evo_collect_task(sqlite3 *db, const char *task_id, evo_attribution_t rows[],
                            int *out_count) {
    *out_count = 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "WITH RECURSIVE feedback_chain(root_event_id,event_id,depth) AS ("
        " SELECT a.feedback_event_id,a.feedback_event_id,0 FROM codex_task_attribution a "
        " WHERE a.task_id=?1 AND a.feedback_event_id IS NOT NULL "
        " UNION ALL SELECT chain.root_event_id,child.event_id,chain.depth+1 "
        " FROM feedback_chain chain JOIN feedback_event child "
        " ON child.supersedes_event_id=chain.event_id "
        " WHERE child.task_id=?1 AND chain.depth<64),"
        "terminal_feedback AS ("
        " SELECT chain.root_event_id,chain.event_id,"
        " ROW_NUMBER() OVER (PARTITION BY chain.root_event_id "
        " ORDER BY f.received_at DESC,f.event_id DESC) AS choice "
        " FROM feedback_chain chain JOIN feedback_event f ON f.event_id=chain.event_id "
        " WHERE NOT EXISTS(SELECT 1 FROM feedback_event child "
        " WHERE child.supersedes_event_id=chain.event_id)) "
        "SELECT a.memory_item_id,a.attribution_state,e.evidence_id,e.trust_class,e.source_type,"
        "f.event_id,f.action,u.outcome,fa.final_reward,c.source_store_id,w.project_uuid,"
        "m.status,m.confidence,m.reusability,m.decay "
        "FROM codex_task_attribution a "
        "JOIN feedback_event base ON base.event_id=a.feedback_event_id AND base.task_id=a.task_id "
        "JOIN terminal_feedback terminal ON terminal.root_event_id=base.event_id "
        " AND terminal.choice=1 "
        "JOIN feedback_event f ON f.event_id=terminal.event_id "
        "JOIN feedback_attribution fa ON fa.feedback_event_id=f.event_id AND fa.task_id=a.task_id "
        " AND fa.memory_item_id=a.memory_item_id "
        " AND fa.attribution_status IN ('attributed','withdrawn') "
        "JOIN memory_usage_attribution u ON u.id=f.usage_id AND u.session_id=f.session_id "
        " AND u.candidate_id=f.candidate_id "
        "JOIN retrieval_candidate c ON c.id=f.candidate_id "
        "JOIN memory_evidence e ON e.evidence_id=f.evidence_id AND e.task_id=a.task_id "
        "JOIN memory_task_result r ON r.result_id=e.result_id AND r.task_id=a.task_id "
        "JOIN global_task_workspace w ON w.task_id=a.task_id "
        "JOIN memory_item m ON m.id=a.memory_item_id AND m.deleted_at IS NULL "
        "WHERE a.task_id=?1 AND a.attribution_state IN ('used','rejected','contradicted') "
        "AND ((f.action='withdraw' AND e.evidence_state='withdrawn') "
        " OR (f.action<>'withdraw' AND e.evidence_state='valid')) "
        "AND r.status='succeeded' "
        "AND u.outcome IN ('used','rejected','contradicted') "
        "AND EXISTS(SELECT 1 FROM codex_task_lifecycle l WHERE l.task_id=a.task_id "
        " AND l.rowid=(SELECT latest.rowid FROM codex_task_lifecycle latest "
        " WHERE latest.task_id=a.task_id ORDER BY latest.rowid DESC LIMIT 1) "
        " AND l.state='completed' AND l.outcome='completed') "
        "ORDER BY a.attribution_id LIMIT 16;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*out_count >= EVO_MAX_ATTRIBUTIONS) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        evo_attribution_t *row = &rows[*out_count];
        memset(row, 0, sizeof(*row));
        const char *memory_id = (const char *)sqlite3_column_text(stmt, 0);
        const char *state = (const char *)sqlite3_column_text(stmt, 1);
        const char *evidence_id = (const char *)sqlite3_column_text(stmt, 2);
        const char *trust = (const char *)sqlite3_column_text(stmt, 3);
        const char *source = (const char *)sqlite3_column_text(stmt, 4);
        const char *feedback_id = (const char *)sqlite3_column_text(stmt, 5);
        const char *action = (const char *)sqlite3_column_text(stmt, 6);
        const char *usage = (const char *)sqlite3_column_text(stmt, 7);
        const char *source_project = (const char *)sqlite3_column_text(stmt, 9);
        const char *target_project = (const char *)sqlite3_column_text(stmt, 10);
        const char *status = (const char *)sqlite3_column_text(stmt, 11);
        if (!memory_id || !state || !evidence_id || !feedback_id || !source_project ||
            !target_project || !status || strlen(memory_id) >= sizeof(row->memory_item_id) ||
            strlen(state) >= sizeof(row->attribution_state) ||
            strlen(evidence_id) >= sizeof(row->evidence_id) ||
            strlen(feedback_id) >= sizeof(row->feedback_event_id) ||
            strlen(source_project) >= sizeof(row->source_project_uuid) ||
            strlen(target_project) >= sizeof(row->target_project_uuid) ||
            strlen(status) >= sizeof(row->status)) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        snprintf(row->memory_item_id, sizeof(row->memory_item_id), "%s", memory_id);
        snprintf(row->attribution_state, sizeof(row->attribution_state), "%s", state);
        snprintf(row->evidence_id, sizeof(row->evidence_id), "%s", evidence_id);
        snprintf(row->feedback_event_id, sizeof(row->feedback_event_id), "%s", feedback_id);
        snprintf(row->source_project_uuid, sizeof(row->source_project_uuid), "%s", source_project);
        snprintf(row->target_project_uuid, sizeof(row->target_project_uuid), "%s", target_project);
        snprintf(row->status, sizeof(row->status), "%s", status);
        row->reward = sqlite3_column_double(stmt, 8);
        row->confidence = sqlite3_column_double(stmt, 12);
        row->reusability = sqlite3_column_double(stmt, 13);
        row->decay = sqlite3_column_double(stmt, 14);
        (void)source;
        evo_classify(row, action, usage, trust);
        (*out_count)++;
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int evo_store_authorizer(void *context, int action, const char *p3, const char *p4,
                                const char *p5, const char *p6) {
    (void)context;
    (void)p3;
    (void)p4;
    (void)p5;
    (void)p6;
    return action == SQLITE_ATTACH || action == SQLITE_DETACH ? SQLITE_DENY : SQLITE_OK;
}

static int evo_test_fail_authorizer_restore;

static int evo_force_store_authorizer(sqlite3 *db) {
    return db ? sqlite3_set_authorizer(db, evo_store_authorizer, NULL) : SQLITE_MISUSE;
}

static int evo_restore_store_authorizer(sqlite3 *db) {
    if (evo_test_fail_authorizer_restore) {
        evo_test_fail_authorizer_restore = 0;
        return SQLITE_ERROR;
    }
    return evo_force_store_authorizer(db);
}

static int evo_graph_is_attached(sqlite3 *memory_db, int *out_attached) {
    if (!memory_db || !out_attached)
        return CBM_STORE_ERR;
    *out_attached = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(memory_db, "PRAGMA database_list;", -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if (name && !strcmp(name, "stage14_graph"))
            *out_attached = 1;
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int evo_detach_graph(sqlite3 *memory_db);

static int evo_attach_graph(sqlite3 *memory_db, sqlite3 *graph_db, int *out_attached_by_call) {
    if (out_attached_by_call)
        *out_attached_by_call = 0;
    int attached_by_this_call = 0;
    int attached = 0;
    if (evo_graph_is_attached(memory_db, &attached) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (attached)
        return CBM_STORE_OK;
    const char *path = graph_db ? sqlite3_db_filename(graph_db, "main") : NULL;
    if (!path || !path[0])
        return CBM_STORE_REJECTED;
    if (sqlite3_set_authorizer(memory_db, NULL, NULL) != SQLITE_OK) {
        (void)evo_force_store_authorizer(memory_db);
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = NULL;
    int prepare =
        sqlite3_prepare_v2(memory_db, "ATTACH DATABASE ?1 AS stage14_graph;", -1, &stmt, NULL);
    int rc = CBM_STORE_ERR;
    if (prepare == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            rc = CBM_STORE_OK;
            attached_by_this_call = 1;
            if (out_attached_by_call)
                *out_attached_by_call = 1;
        }
    }
    sqlite3_finalize(stmt);
    int authorizer_rc = evo_restore_store_authorizer(memory_db);
    if (rc != CBM_STORE_OK || authorizer_rc != SQLITE_OK) {
        if (attached_by_this_call)
            (void)evo_detach_graph(memory_db);
        int still_attached = 0;
        if (evo_graph_is_attached(memory_db, &still_attached) != CBM_STORE_OK)
            still_attached = 1;
        if (out_attached_by_call)
            *out_attached_by_call = still_attached;
        (void)evo_force_store_authorizer(memory_db);
        return CBM_STORE_ERR;
    }
    return rc;
}

static int evo_detach_graph(sqlite3 *memory_db) {
    if (!memory_db)
        return CBM_STORE_ERR;
    int attached = 0;
    int rc = evo_graph_is_attached(memory_db, &attached);
    if (rc == CBM_STORE_OK && attached) {
        if (sqlite3_set_authorizer(memory_db, NULL, NULL) != SQLITE_OK)
            rc = CBM_STORE_ERR;
        else
            rc = evo_exec(memory_db, "DETACH DATABASE stage14_graph;");
    }
    if (evo_force_store_authorizer(memory_db) != SQLITE_OK)
        rc = CBM_STORE_ERR;
    return rc;
}

int cbm_evolution_attach_authorizer_failure_for_test(sqlite3 *memory_db, sqlite3 *graph_db) {
    if (!memory_db || !graph_db)
        return CBM_STORE_ERR;
    evo_test_fail_authorizer_restore = 1;
    int attached_by_call = 0;
    int rc = evo_attach_graph(memory_db, graph_db, &attached_by_call);
    evo_test_fail_authorizer_restore = 0;
    return rc;
}

static int evo_enable_foreign_keys(sqlite3 *db) {
    if (evo_exec(db, "PRAGMA foreign_keys=ON;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_keys;", -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int step = sqlite3_step(stmt);
    int enabled = step == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return enabled ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int evo_same_volume(const char *memory_path, const char *graph_path) {
    if (!memory_path || !memory_path[0] || !graph_path || !graph_path[0])
        return 0;
#ifdef _WIN32
    int memory_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, memory_path, -1, NULL, 0);
    int graph_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, graph_path, -1, NULL, 0);
    if (memory_count <= 0 || graph_count <= 0)
        return 0;
    wchar_t *memory_wide = malloc((size_t)memory_count * sizeof(*memory_wide));
    wchar_t *graph_wide = malloc((size_t)graph_count * sizeof(*graph_wide));
    if (!memory_wide || !graph_wide) {
        free(memory_wide);
        free(graph_wide);
        return 0;
    }
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, memory_path, -1, memory_wide,
                                        memory_count) == memory_count &&
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, graph_path, -1, graph_wide,
                                        graph_count) == graph_count;
    HANDLE memory_file = converted
                             ? CreateFileW(memory_wide, FILE_READ_ATTRIBUTES,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                             : INVALID_HANDLE_VALUE;
    HANDLE graph_file = converted
                            ? CreateFileW(graph_wide, FILE_READ_ATTRIBUTES,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                            : INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION memory_info = {0}, graph_info = {0};
    int same = memory_file != INVALID_HANDLE_VALUE && graph_file != INVALID_HANDLE_VALUE &&
               GetFileInformationByHandle(memory_file, &memory_info) &&
               GetFileInformationByHandle(graph_file, &graph_info) &&
               memory_info.dwVolumeSerialNumber == graph_info.dwVolumeSerialNumber;
    if (memory_file != INVALID_HANDLE_VALUE)
        CloseHandle(memory_file);
    if (graph_file != INVALID_HANDLE_VALUE)
        CloseHandle(graph_file);
    free(memory_wide);
    free(graph_wide);
    return same;
#else
    struct stat memory_stat, graph_stat;
    return stat(memory_path, &memory_stat) == 0 && stat(graph_path, &graph_stat) == 0 &&
           memory_stat.st_dev == graph_stat.st_dev;
#endif
}

static int evo_cross_process_guard_acquire(const char *memory_path, const char *graph_path,
                                           evo_cross_process_guard_t *guard) {
    if (!memory_path || !graph_path || !guard)
        return CBM_STORE_ERR;
    memset(guard, 0, sizeof(*guard));
#ifdef _WIN32
    char seed[8192], hash[65];
    int used = snprintf(seed, sizeof(seed), "stage14-atomic-v1\n%s\n%s", memory_path, graph_path);
    if (used < 0 || used >= (int)sizeof(seed) || evo_hash_text(seed, hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    wchar_t name[128];
    if (swprintf(name, sizeof(name) / sizeof(name[0]), L"Local\\CBMStage14Evolution_%hs", hash) < 0)
        return CBM_STORE_ERR;
    guard->handle = CreateMutexW(NULL, FALSE, name);
    if (!guard->handle)
        return CBM_STORE_ERR;
    DWORD waited = WaitForSingleObject(guard->handle, EVO_ATOMIC_LOCK_TIMEOUT_MS);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        CloseHandle(guard->handle);
        guard->handle = NULL;
        return waited == WAIT_TIMEOUT ? CBM_STORE_REJECTED : CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
#else
    guard->memory_fd = -1;
    guard->graph_fd = -1;
    guard->memory_fd = open(memory_path, O_RDWR);
    guard->graph_fd = open(graph_path, O_RDWR);
    if (guard->memory_fd < 0 || guard->graph_fd < 0) {
        if (guard->memory_fd >= 0)
            close(guard->memory_fd);
        if (guard->graph_fd >= 0)
            close(guard->graph_fd);
        guard->memory_fd = guard->graph_fd = -1;
        return CBM_STORE_ERR;
    }
    for (int attempt = 0; attempt < 500; attempt++) {
        if (flock(guard->memory_fd, LOCK_EX | LOCK_NB) == 0) {
            if (flock(guard->graph_fd, LOCK_EX | LOCK_NB) == 0)
                return CBM_STORE_OK;
            flock(guard->memory_fd, LOCK_UN);
        }
        usleep(10000);
    }
    close(guard->memory_fd);
    close(guard->graph_fd);
    guard->memory_fd = guard->graph_fd = -1;
    return CBM_STORE_REJECTED;
#endif
}

static void evo_cross_process_guard_release(evo_cross_process_guard_t *guard) {
    if (!guard)
        return;
#ifdef _WIN32
    if (guard->handle) {
        ReleaseMutex(guard->handle);
        CloseHandle(guard->handle);
        guard->handle = NULL;
    }
#else
    if (guard->graph_fd >= 0) {
        flock(guard->graph_fd, LOCK_UN);
        close(guard->graph_fd);
    }
    if (guard->memory_fd >= 0) {
        flock(guard->memory_fd, LOCK_UN);
        close(guard->memory_fd);
    }
    guard->memory_fd = guard->graph_fd = -1;
#endif
}

static int evo_pragma_text(sqlite3 *db, const char *schema, const char *name, char *out,
                           size_t out_size) {
    if (!db || !schema || !name || !out || out_size < 2)
        return CBM_STORE_ERR;
    char sql[128];
    int used = snprintf(sql, sizeof(sql), "PRAGMA %s.%s;", schema, name);
    if (used < 0 || used >= (int)sizeof(sql))
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int step = sqlite3_step(stmt);
    const char *value = step == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
    int rc = value && value[0] && strlen(value) < out_size ? CBM_STORE_OK : CBM_STORE_ERR;
    if (rc == CBM_STORE_OK)
        snprintf(out, out_size, "%s", value);
    if (rc == CBM_STORE_OK && sqlite3_step(stmt) != SQLITE_DONE)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int evo_set_pragma_text(sqlite3 *db, const char *schema, const char *name, const char *value,
                               char *out, size_t out_size) {
    if (!db || !schema || !name || !value || !out || out_size < 2)
        return CBM_STORE_ERR;
    char sql[160];
    int used = snprintf(sql, sizeof(sql), "PRAGMA %s.%s=%s;", schema, name, value);
    if (used < 0 || used >= (int)sizeof(sql))
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int step = sqlite3_step(stmt);
    const char *result = step == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
    int rc = result && result[0] && strlen(result) < out_size ? CBM_STORE_OK : CBM_STORE_ERR;
    if (rc == CBM_STORE_OK)
        snprintf(out, out_size, "%s", result);
    if (rc == CBM_STORE_OK && sqlite3_step(stmt) != SQLITE_DONE)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int evo_pragma_int(sqlite3 *db, const char *schema, const char *name, int *out_value) {
    if (!db || !schema || !name || !out_value)
        return CBM_STORE_ERR;
    char sql[128];
    int used = snprintf(sql, sizeof(sql), "PRAGMA %s.%s;", schema, name);
    if (used < 0 || used >= (int)sizeof(sql))
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int step = sqlite3_step(stmt);
    int rc = step == SQLITE_ROW ? CBM_STORE_OK : CBM_STORE_ERR;
    if (rc == CBM_STORE_OK)
        *out_value = sqlite3_column_int(stmt, 0);
    if (rc == CBM_STORE_OK && sqlite3_step(stmt) != SQLITE_DONE)
        rc = CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int evo_synchronous_set(sqlite3 *db, const char *schema, int value) {
    char sql[128];
    int used = snprintf(sql, sizeof(sql), "PRAGMA %s.synchronous=%d;", schema, value);
    if (used < 0 || used >= (int)sizeof(sql) || evo_exec(db, sql) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int actual = -1;
    return evo_pragma_int(db, schema, "synchronous", &actual) == CBM_STORE_OK && actual == value
               ? CBM_STORE_OK
               : CBM_STORE_ERR;
}

static int evo_prepare_super_journal(sqlite3 *memory_db, sqlite3 *graph_db,
                                     evo_journal_guard_t *guard) {
    if (!memory_db || !graph_db || !guard)
        return CBM_STORE_ERR;
    memset(guard, 0, sizeof(*guard));
    const char *memory_path = sqlite3_db_filename(memory_db, "main");
    const char *graph_path = sqlite3_db_filename(graph_db, "main");
    if (!evo_same_volume(memory_path, graph_path))
        return CBM_STORE_REJECTED;
    int attached = 0;
    if (evo_graph_is_attached(memory_db, &attached) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (attached)
        return CBM_STORE_REJECTED;
    if (evo_pragma_text(memory_db, "main", "journal_mode", guard->main_journal,
                        sizeof(guard->main_journal)) != CBM_STORE_OK ||
        evo_pragma_text(graph_db, "main", "journal_mode", guard->graph_journal,
                        sizeof(guard->graph_journal)) != CBM_STORE_OK ||
        evo_pragma_int(memory_db, "main", "synchronous", &guard->main_synchronous) !=
            CBM_STORE_OK ||
        evo_pragma_int(graph_db, "main", "synchronous", &guard->graph_synchronous) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    guard->captured = 1;
    if (!strcmp(guard->main_journal, "memory") || !strcmp(guard->main_journal, "off") ||
        !strcmp(guard->graph_journal, "memory") || !strcmp(guard->graph_journal, "off"))
        return CBM_STORE_REJECTED;
    char value[32];
    if (evo_set_pragma_text(graph_db, "main", "journal_mode", "DELETE", value, sizeof(value)) !=
            CBM_STORE_OK ||
        strcmp(value, "delete") != 0 ||
        evo_set_pragma_text(memory_db, "main", "journal_mode", "DELETE", value, sizeof(value)) !=
            CBM_STORE_OK ||
        strcmp(value, "delete") != 0)
        return CBM_STORE_REJECTED;
    int attached_by_call = 0;
    int attach_rc = evo_attach_graph(memory_db, graph_db, &attached_by_call);
    guard->attached_by_us = attached_by_call;
    if (attach_rc != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (evo_set_pragma_text(memory_db, "main", "locking_mode", "EXCLUSIVE", value, sizeof(value)) !=
            CBM_STORE_OK ||
        strcmp(value, "exclusive") != 0 ||
        evo_set_pragma_text(memory_db, "stage14_graph", "locking_mode", "EXCLUSIVE", value,
                            sizeof(value)) != CBM_STORE_OK ||
        strcmp(value, "exclusive") != 0)
        return CBM_STORE_REJECTED;
    if (evo_synchronous_set(memory_db, "main", 2) != CBM_STORE_OK ||
        evo_synchronous_set(memory_db, "stage14_graph", 2) != CBM_STORE_OK)
        return CBM_STORE_REJECTED;
    char main_check[16], graph_check[16];
    if (evo_pragma_text(memory_db, "main", "journal_mode", main_check, sizeof(main_check)) !=
            CBM_STORE_OK ||
        evo_pragma_text(memory_db, "stage14_graph", "journal_mode", graph_check,
                        sizeof(graph_check)) != CBM_STORE_OK ||
        strcmp(main_check, "delete") != 0 || strcmp(graph_check, "delete") != 0)
        return CBM_STORE_REJECTED;
    guard->prepared = 1;
    return CBM_STORE_OK;
}

static void evo_restore_journal_modes(sqlite3 *memory_db, sqlite3 *graph_db,
                                      evo_journal_guard_t *guard) {
    if (!memory_db || !graph_db || !guard)
        return;
    char value[32];
    int attached = 0;
    if (evo_graph_is_attached(memory_db, &attached) != CBM_STORE_OK)
        attached = 0;
    if (attached) {
        if (guard->graph_journal[0] && strcmp(guard->graph_journal, "delete") != 0)
            (void)evo_set_pragma_text(memory_db, "stage14_graph", "journal_mode",
                                      guard->graph_journal, value, sizeof(value));
        if (guard->captured)
            (void)evo_synchronous_set(memory_db, "stage14_graph", guard->graph_synchronous);
        (void)evo_set_pragma_text(memory_db, "stage14_graph", "locking_mode", "NORMAL", value,
                                  sizeof(value));
    }
    if (guard->main_journal[0] && strcmp(guard->main_journal, "delete") != 0)
        (void)evo_set_pragma_text(memory_db, "main", "journal_mode", guard->main_journal, value,
                                  sizeof(value));
    if (guard->captured)
        (void)evo_synchronous_set(memory_db, "main", guard->main_synchronous);
    (void)evo_set_pragma_text(memory_db, "main", "locking_mode", "NORMAL", value, sizeof(value));
    if (guard->attached_by_us)
        (void)evo_detach_graph(memory_db);
    if (guard->graph_journal[0] && strcmp(guard->graph_journal, "delete") != 0)
        (void)evo_set_pragma_text(graph_db, "main", "journal_mode", guard->graph_journal, value,
                                  sizeof(value));
    if (guard->captured)
        (void)evo_synchronous_set(graph_db, "main", guard->graph_synchronous);
    (void)evo_force_store_authorizer(memory_db);
    memset(guard, 0, sizeof(*guard));
}

static int evo_crash_commit_hook(void *context) {
    (void)context;
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 86);
#else
    _exit(86);
#endif
    return 1;
}

static int evo_open_atomic_db(sqlite3 *memory_db, sqlite3 *graph_db, sqlite3 **out_db) {
    if (!out_db)
        return CBM_STORE_ERR;
    *out_db = NULL;
    const char *memory_path = memory_db ? sqlite3_db_filename(memory_db, "main") : NULL;
    if (!memory_path || !memory_path[0])
        return CBM_STORE_REJECTED;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(memory_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return CBM_STORE_ERR;
    }
    sqlite3_busy_timeout(db, 5000);
    int rc = evo_enable_foreign_keys(db);
    int attached_by_call = 0;
    if (rc == CBM_STORE_OK)
        rc = evo_attach_graph(db, graph_db, &attached_by_call);
    if (rc != CBM_STORE_OK) {
        sqlite3_close(db);
        return rc;
    }
    *out_db = db;
    return CBM_STORE_OK;
}

int cbm_evolution_open_atomic_db_for_test(sqlite3 *memory_db, sqlite3 *graph_db, sqlite3 **out_db) {
    return evo_open_atomic_db(memory_db, graph_db, out_db);
}

static int evo_write_cross_project_edge(sqlite3 *db, const evo_attribution_t *row,
                                        const char *event_id, int *out_written) {
    *out_written = 0;
    if (!strcmp(row->source_project_uuid, row->target_project_uuid) || row->delta == 0.0)
        return CBM_STORE_OK;
    const char *relation =
        !strcmp(row->operation, "positive")
            ? "reuse"
            : (!strcmp(row->operation, "contradict") ? "contradicts" : "deprioritizes");
    char seed[1024], hash[65], edge_id[48], now[40];
    snprintf(seed, sizeof(seed), "stage14-evolution-edge/v2\n%s\n%s\n%s\n%s",
             row->source_project_uuid, row->target_project_uuid, row->memory_item_id, relation);
    if (evo_hash_text(seed, hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    snprintf(edge_id, sizeof(edge_id), "xproj-%.32s", hash);
    sqlite3_stmt *stmt = NULL;
    int version = 1, weight = 500000;
    if (sqlite3_prepare_v2(db,
                           "SELECT version,weight_ppm FROM stage14_graph.global_cross_project_edge "
                           "WHERE edge_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0) + 1;
        weight = sqlite3_column_int(stmt, 1);
    } else if (step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    int delta_ppm = (int)(row->delta * 1000000.0);
    weight += delta_ppm;
    if (weight < 0)
        weight = 0;
    if (weight > 1000000)
        weight = 1000000;
    evo_timestamp(time(NULL), now);
    const char *upsert =
        "INSERT INTO stage14_graph.global_cross_project_edge(edge_id,source_project_uuid,"
        "target_project_uuid,relation_type,weight_ppm,confidence_ppm,status,version,updated_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,'active',?7,?8) ON CONFLICT(edge_id) DO UPDATE SET "
        "weight_ppm=excluded.weight_ppm,confidence_ppm=excluded.confidence_ppm,"
        "status='active',version=excluded.version,updated_at=excluded.updated_at "
        "WHERE excluded.version=global_cross_project_edge.version+1;";
    if (sqlite3_prepare_v2(db, upsert, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, row->source_project_uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, row->target_project_uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, relation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, weight);
    sqlite3_bind_int(stmt, 6, !strcmp(row->evidence_grade, "A") ? 900000 : 750000);
    sqlite3_bind_int(stmt, 7, version);
    sqlite3_bind_text(stmt, 8, now, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                           : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK)
        return rc;
    char payload[1024], payload_hash[65];
    snprintf(payload, sizeof(payload), "%s\n%d\n%d\n%s", edge_id, version, weight, event_id);
    if (evo_hash_text(payload, payload_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO stage14_graph.global_cross_project_edge_version("
                           "edge_id,version,payload_sha256,evidence_event_id,created_at) "
                           "VALUES(?1,?2,?3,?4,?5);",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, version);
    sqlite3_bind_text(stmt, 3, payload_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, now, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK)
        *out_written = 1;
    return rc;
}

static int evo_task_project(sqlite3 *db, const char *task_id, char project_uuid[128],
                            int *out_found) {
    if (!db || !task_id || !project_uuid || !out_found)
        return CBM_STORE_ERR;
    project_uuid[0] = '\0';
    *out_found = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT project_uuid FROM global_task_workspace WHERE task_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        const char *project = (const char *)sqlite3_column_text(stmt, 0);
        if (!project || !project[0] || strlen(project) >= 128) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
        snprintf(project_uuid, 128, "%s", project);
        *out_found = 1;
        step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return CBM_STORE_ERR;
        }
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int evo_task_effective_caps(const cbm_evolution_task_input_t *input, int production_intent,
                                   int *max_events, int *max_edges) {
    if (!input || !max_events || !max_edges)
        return CBM_STORE_ERR;
    if (input->max_evolution_events == 0 && !production_intent) {
        if (input->max_cross_project_edges != 0)
            return CBM_STORE_ERR;
        *max_events = EVO_MAX_TASK_EVENTS;
        *max_edges = EVO_MAX_ATTRIBUTIONS;
        return CBM_STORE_OK;
    }
    if (input->max_evolution_events < 1 || input->max_evolution_events > EVO_MAX_TASK_EVENTS ||
        input->max_cross_project_edges < 0 || input->max_cross_project_edges > EVO_MAX_ATTRIBUTIONS)
        return CBM_STORE_REJECTED;
    *max_events = input->max_evolution_events;
    *max_edges = input->max_cross_project_edges;
    return CBM_STORE_OK;
}

static int evo_task_request_hash(const cbm_evolution_task_input_t *input,
                                 const evo_task_plan_t *plan, char out[65]) {
    char canonical[65536] = {0};
    size_t used = 0;
    char number[64];
    if (!evo_canonical_field(canonical, sizeof(canonical), &used, "schema",
                             EVO_TASK_REQUEST_SCHEMA) ||
        !evo_canonical_field(canonical, sizeof(canonical), &used, "mode", input->mode) ||
        !evo_canonical_field(canonical, sizeof(canonical), &used, "task_id", input->task_id) ||
        !evo_canonical_field(canonical, sizeof(canonical), &used, "project_uuid",
                             plan->project_uuid) ||
        !evo_canonical_field(canonical, sizeof(canonical), &used, "run_id",
                             input->run_id ? input->run_id : "") ||
        !evo_canonical_field(canonical, sizeof(canonical), &used, "idempotency_key",
                             input->idempotency_key))
        return CBM_STORE_ERR;
    snprintf(number, sizeof(number), "%d", plan->max_evolution_events);
    if (!evo_canonical_field(canonical, sizeof(canonical), &used, "max_evolution_events", number))
        return CBM_STORE_ERR;
    snprintf(number, sizeof(number), "%d", plan->max_cross_project_edges);
    if (!evo_canonical_field(canonical, sizeof(canonical), &used, "max_cross_project_edges",
                             number))
        return CBM_STORE_ERR;
    snprintf(number, sizeof(number), "%d", plan->count);
    if (!evo_canonical_field(canonical, sizeof(canonical), &used, "attribution_count", number))
        return CBM_STORE_ERR;
    for (int i = 0; i < plan->count; i++) {
        const evo_attribution_t *row = &plan->rows[i];
        snprintf(number, sizeof(number), "%d", i);
        if (!evo_canonical_field(canonical, sizeof(canonical), &used, "row_index", number) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "memory_item_id",
                                 row->memory_item_id) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "feedback_event_id",
                                 row->feedback_event_id) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "evidence_id",
                                 row->evidence_id) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "evidence_grade",
                                 row->evidence_grade) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "operation",
                                 row->operation) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "source_project_uuid",
                                 row->source_project_uuid) ||
            !evo_canonical_field(canonical, sizeof(canonical), &used, "target_project_uuid",
                                 row->target_project_uuid))
            return CBM_STORE_ERR;
        snprintf(number, sizeof(number), "%.9f", row->delta);
        if (!evo_canonical_field(canonical, sizeof(canonical), &used, "delta", number))
            return CBM_STORE_ERR;
    }
    return cbm_stage7_sha256_hex(canonical, used, out);
}

static int evo_prepare_task_plan(sqlite3 *db, const cbm_evolution_task_input_t *input,
                                 evo_task_plan_t *plan) {
    if (!db || !input || !plan)
        return CBM_STORE_ERR;
    memset(plan, 0, sizeof(*plan));
    int production_intent = evo_mode_writes(input->mode) && !input->isolated_write_allowed;
    if (production_intent &&
        (!input->project_uuid || !input->project_uuid[0] || !input->run_id || !input->run_id[0]))
        return CBM_STORE_REJECTED;
    int found = 0;
    int rc = evo_task_project(db, input->task_id, plan->project_uuid, &found);
    if (rc != CBM_STORE_OK)
        return rc;
    if (production_intent && !found)
        return CBM_STORE_REJECTED;
    if (input->project_uuid && input->project_uuid[0]) {
        if (found && strcmp(input->project_uuid, plan->project_uuid) != 0)
            return CBM_STORE_REJECTED;
        if (!found) {
            if (strlen(input->project_uuid) >= sizeof(plan->project_uuid))
                return CBM_STORE_REJECTED;
            snprintf(plan->project_uuid, sizeof(plan->project_uuid), "%s", input->project_uuid);
        }
    }
    rc = evo_task_effective_caps(input, production_intent, &plan->max_evolution_events,
                                 &plan->max_cross_project_edges);
    if (rc != CBM_STORE_OK)
        return rc;
    rc = evo_collect_task(db, input->task_id, plan->rows, &plan->count);
    if (rc != CBM_STORE_OK)
        return rc;
    for (int i = 0; i < plan->count; i++) {
        evo_attribution_t *row = &plan->rows[i];
        if (found && strcmp(row->target_project_uuid, plan->project_uuid) != 0)
            return CBM_STORE_REJECTED;
        if (!strcmp(row->operation, "positive"))
            plan->positive++;
        else if (row->delta < 0.0)
            plan->negative++;
        if (strcmp(row->source_project_uuid, row->target_project_uuid) != 0 && row->delta != 0.0)
            plan->planned_cross_project_edges++;
    }
    plan->planned_evolution_events = plan->count + 1;
    if (plan->planned_evolution_events > plan->max_evolution_events ||
        plan->planned_cross_project_edges > plan->max_cross_project_edges)
        return CBM_STORE_REJECTED;
    return evo_task_request_hash(input, plan, plan->request_sha256);
}

static void evo_task_result_from_plan(cbm_evolution_result_t *out, const evo_task_plan_t *plan) {
    out->eligible = plan->count;
    out->positive = plan->positive;
    out->negative = plan->negative;
    out->planned_evolution_events = plan->planned_evolution_events;
    out->planned_cross_project_edges = plan->planned_cross_project_edges;
    snprintf(out->request_sha256, sizeof(out->request_sha256), "%s", plan->request_sha256);
}

static char *evo_task_plan_report(const cbm_evolution_task_input_t *input,
                                  const evo_task_plan_t *plan) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    yyjson_mut_val *memory_ids = doc ? yyjson_mut_arr(doc) : NULL;
    yyjson_mut_val *feedback_ids = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !root || !memory_ids || !feedback_ids) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < plan->count; i++) {
        yyjson_mut_arr_add_strcpy(doc, memory_ids, plan->rows[i].memory_item_id);
        yyjson_mut_arr_add_strcpy(doc, feedback_ids, plan->rows[i].feedback_event_id);
    }
    yyjson_mut_obj_add_str(doc, root, "schema", EVO_TASK_PLAN_SCHEMA);
    yyjson_mut_obj_add_str(doc, root, "status", plan->count ? "planned" : "no_eligible");
    yyjson_mut_obj_add_str(doc, root, "mode", input->mode);
    yyjson_mut_obj_add_str(doc, root, "task_id", input->task_id);
    yyjson_mut_obj_add_str(doc, root, "project_uuid", plan->project_uuid);
    yyjson_mut_obj_add_str(doc, root, "run_id", input->run_id ? input->run_id : "");
    yyjson_mut_obj_add_str(doc, root, "idempotency_key", input->idempotency_key);
    yyjson_mut_obj_add_str(doc, root, "request_sha256", plan->request_sha256);
    yyjson_mut_obj_add_val(doc, root, "memory_item_ids", memory_ids);
    yyjson_mut_obj_add_val(doc, root, "feedback_event_ids", feedback_ids);
    yyjson_mut_obj_add_int(doc, root, "max_evolution_events", plan->max_evolution_events);
    yyjson_mut_obj_add_int(doc, root, "max_cross_project_edges", plan->max_cross_project_edges);
    yyjson_mut_obj_add_int(doc, root, "planned_evolution_events", plan->planned_evolution_events);
    yyjson_mut_obj_add_int(doc, root, "planned_cross_project_edges",
                           plan->planned_cross_project_edges);
    yyjson_mut_obj_add_int(doc, root, "eligible", plan->count);
    yyjson_mut_obj_add_int(doc, root, "positive", plan->positive);
    yyjson_mut_obj_add_int(doc, root, "negative", plan->negative);
    yyjson_mut_obj_add_bool(doc, root, "wrote", false);
    yyjson_mut_obj_add_bool(doc, root, "hard_delete", false);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int evo_json_string_equal(yyjson_val *object, const char *name, const char *expected) {
    yyjson_val *value = object ? yyjson_obj_get(object, name) : NULL;
    return value && yyjson_is_str(value) && strcmp(yyjson_get_str(value), expected) == 0;
}

static int evo_task_manifest_matches(const cbm_evolution_task_input_t *input,
                                     const evo_task_plan_t *plan) {
    if (!input->manifest_path || !evo_absolute_path(input->manifest_path) ||
        !evo_lower_sha256(input->manifest_sha256))
        return 0;
    size_t size = 0;
    char *data = evo_read_task_manifest(input->manifest_path, &size);
    if (!data)
        return 0;
    char actual_sha256[65];
    int ok = cbm_stage7_sha256_hex(data, size, actual_sha256) == CBM_STORE_OK &&
             strcmp(actual_sha256, input->manifest_sha256) == 0;
    yyjson_doc *doc = ok ? yyjson_read(data, size, 0) : NULL;
    free(data);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *memory_ids = root ? yyjson_obj_get(root, "memory_item_ids") : NULL;
    yyjson_val *feedback_ids = root ? yyjson_obj_get(root, "feedback_event_ids") : NULL;
    yyjson_val *max_events = root ? yyjson_obj_get(root, "max_evolution_events") : NULL;
    yyjson_val *max_edges = root ? yyjson_obj_get(root, "max_cross_project_edges") : NULL;
    ok = ok && root && yyjson_is_obj(root) && yyjson_obj_size(root) == 9 &&
         evo_json_string_equal(root, "schema", EVO_TASK_MANIFEST_SCHEMA) &&
         evo_json_string_equal(root, "mode", input->mode) &&
         evo_json_string_equal(root, "task_id", input->task_id) &&
         evo_json_string_equal(root, "idempotency_key", input->idempotency_key) &&
         evo_json_string_equal(root, "request_sha256", plan->request_sha256) && memory_ids &&
         yyjson_is_arr(memory_ids) && yyjson_arr_size(memory_ids) == (size_t)plan->count &&
         feedback_ids && yyjson_is_arr(feedback_ids) &&
         yyjson_arr_size(feedback_ids) == (size_t)plan->count && max_events &&
         yyjson_is_int(max_events) && yyjson_get_sint(max_events) == plan->max_evolution_events &&
         max_edges && yyjson_is_int(max_edges) &&
         yyjson_get_sint(max_edges) == plan->max_cross_project_edges;
    for (int i = 0; ok && i < plan->count; i++) {
        yyjson_val *memory = yyjson_arr_get(memory_ids, (size_t)i);
        yyjson_val *feedback = yyjson_arr_get(feedback_ids, (size_t)i);
        ok = memory && yyjson_is_str(memory) &&
             strcmp(yyjson_get_str(memory), plan->rows[i].memory_item_id) == 0 && feedback &&
             yyjson_is_str(feedback) &&
             strcmp(yyjson_get_str(feedback), plan->rows[i].feedback_event_id) == 0;
    }
    yyjson_doc_free(doc);
    return ok;
}

int cbm_evolution_plan_completed_task(cbm_global_memory_t *global,
                                      const cbm_evolution_task_input_t *input,
                                      cbm_evolution_result_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!global || !input || !out || !evo_mode_valid(input->mode) || !input->task_id ||
        !input->task_id[0] || !input->idempotency_key || !input->idempotency_key[0] ||
        (input->isolated_write_allowed != 0 && input->isolated_write_allowed != 1) ||
        (input->production_gate_allowed != 0 && input->production_gate_allowed != 1) ||
        (input->crash_during_commit != 0 && input->crash_during_commit != 1))
        return CBM_STORE_ERR;
    sqlite3 *db = cbm_global_memory_db(global);
    if (!db || evo_exec(db, "BEGIN;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    evo_task_plan_t plan;
    int rc = evo_prepare_task_plan(db, input, &plan);
    if (evo_exec(db, "ROLLBACK;") != CBM_STORE_OK && rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc != CBM_STORE_OK)
        return rc;
    evo_task_result_from_plan(out, &plan);
    out->report_json = evo_task_plan_report(input, &plan);
    return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

int cbm_evolution_apply_completed_task(cbm_global_memory_t *global,
                                       const cbm_evolution_task_input_t *input,
                                       cbm_evolution_result_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!global || !input || !out || !evo_mode_valid(input->mode) || !input->task_id ||
        !input->task_id[0] || !input->idempotency_key || !input->idempotency_key[0] ||
        (input->isolated_write_allowed != 0 && input->isolated_write_allowed != 1) ||
        (input->production_gate_allowed != 0 && input->production_gate_allowed != 1) ||
        (input->crash_during_commit != 0 && input->crash_during_commit != 1))
        return CBM_STORE_ERR;
    if (!evo_mode_writes(input->mode))
        return cbm_evolution_plan_completed_task(global, input, out);
    int production = !input->isolated_write_allowed;
    if (production && strcmp(input->mode, "bounded_canary") != 0)
        return CBM_STORE_REJECTED;
    if (production &&
        (!input->production_gate_allowed || !input->manifest_path || !input->manifest_sha256 ||
         input->failure_after_write != 0 || input->crash_during_commit != 0 ||
         input->snapshot_hook || input->snapshot_hook_context))
        return CBM_STORE_REJECTED;
    if (!evo_write_guard(input->mode, input->isolated_write_allowed,
                         input->production_gate_allowed))
        return CBM_STORE_REJECTED;

    sqlite3 *source_db = cbm_global_memory_db(global);
    sqlite3 *source_graph = cbm_global_graph_db(global);
    const char *memory_path = source_db ? sqlite3_db_filename(source_db, "main") : NULL;
    const char *graph_path = source_graph ? sqlite3_db_filename(source_graph, "main") : NULL;
    if (!memory_path || !memory_path[0] || !graph_path || !graph_path[0] ||
        !evo_same_volume(memory_path, graph_path))
        return CBM_STORE_REJECTED;
    evo_cross_process_guard_t process_guard;
    int rc = evo_cross_process_guard_acquire(memory_path, graph_path, &process_guard);
    if (rc != CBM_STORE_OK)
        return rc;

    if (evo_exec(source_db, "BEGIN;") != CBM_STORE_OK) {
        evo_cross_process_guard_release(&process_guard);
        return CBM_STORE_ERR;
    }
    evo_task_plan_t preflight;
    rc = evo_prepare_task_plan(source_db, input, &preflight);
    if (rc == CBM_STORE_OK && production && !evo_task_manifest_matches(input, &preflight))
        rc = CBM_STORE_REJECTED;
    int preflight_exact = 0;
    int preflight_existing = rc == CBM_STORE_OK
                                 ? evo_event_lookup(source_db, input->idempotency_key,
                                                    preflight.request_sha256, &preflight_exact)
                                 : CBM_STORE_ERR;
    int preflight_rollback = evo_exec(source_db, "ROLLBACK;");
    if (preflight_rollback != CBM_STORE_OK && rc == CBM_STORE_OK)
        rc = CBM_STORE_ERR;
    if (rc != CBM_STORE_OK) {
        evo_cross_process_guard_release(&process_guard);
        return rc;
    }
    evo_task_result_from_plan(out, &preflight);
    if (preflight_existing == CBM_STORE_OK) {
        out->replayed = preflight_exact;
        out->report_json = evo_report(preflight_exact ? "replayed" : "IDEMPOTENCY_CONFLICT",
                                      input->mode, 0, preflight.count, out->positive, out->negative,
                                      0, 0, 0, 0, 0, "existing_controller_key");
        evo_cross_process_guard_release(&process_guard);
        if (!out->report_json) {
            memset(out, 0, sizeof(*out));
            return CBM_STORE_ERR;
        }
        return preflight_exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    if (preflight_existing != CBM_STORE_NOT_FOUND) {
        evo_cross_process_guard_release(&process_guard);
        memset(out, 0, sizeof(*out));
        return preflight_existing;
    }

    sqlite3 *atomic_db = source_db;
    evo_journal_guard_t journal_guard;
    rc = evo_prepare_super_journal(atomic_db, source_graph, &journal_guard);
    if (rc != CBM_STORE_OK) {
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        memset(out, 0, sizeof(*out));
        return rc;
    }
    sqlite3 *db = atomic_db;
    if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK) {
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        memset(out, 0, sizeof(*out));
        return CBM_STORE_ERR;
    }
    evo_task_plan_t plan;
    rc = evo_prepare_task_plan(db, input, &plan);
    if (rc != CBM_STORE_OK) {
        evo_exec(db, "ROLLBACK;");
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        memset(out, 0, sizeof(*out));
        return rc;
    }
    memset(out, 0, sizeof(*out));
    evo_task_result_from_plan(out, &plan);
    if (production && !evo_task_manifest_matches(input, &plan)) {
        evo_exec(db, "ROLLBACK;");
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        memset(out, 0, sizeof(*out));
        return CBM_STORE_REJECTED;
    }
    if (input->snapshot_hook && input->isolated_write_allowed)
        input->snapshot_hook(input->snapshot_hook_context);
    const char *payload_hash = plan.request_sha256;
    int exact = 0;
    int existing = evo_event_lookup(db, input->idempotency_key, payload_hash, &exact);
    if (existing == CBM_STORE_OK) {
        out->replayed = exact;
        out->report_json =
            evo_report(exact ? "replayed" : "IDEMPOTENCY_CONFLICT", input->mode, 0, plan.count,
                       out->positive, out->negative, 0, 0, 0, 0, 0, "existing_controller_key");
        evo_exec(db, "ROLLBACK;");
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        if (!out->report_json) {
            memset(out, 0, sizeof(*out));
            return CBM_STORE_ERR;
        }
        return exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    if (existing != CBM_STORE_NOT_FOUND) {
        evo_exec(db, "ROLLBACK;");
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        memset(out, 0, sizeof(*out));
        return existing;
    }
    if (plan.count == 0) {
        char noop_event[48];
        rc = evo_append_event(db, input->task_id, plan.project_uuid, "task", input->task_id, "noop",
                              "system", NULL, payload_hash, payload_hash, input->idempotency_key,
                              payload_hash, noop_event);
        if (rc == CBM_STORE_OK && input->failure_after_write > 0 && 1 >= input->failure_after_write)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK) {
            out->evolution_events = 1;
            out->report_json = evo_report("no_op", input->mode, 0, 0, 0, 0, 1, 0, 0, 0, 0,
                                          "terminal_or_evidence_gate");
            if (!out->report_json)
                rc = CBM_STORE_ERR;
        }
        if (rc == CBM_STORE_OK) {
            if (input->crash_during_commit)
                sqlite3_commit_hook(db, evo_crash_commit_hook, NULL);
            rc = evo_exec(db, "COMMIT;");
            sqlite3_commit_hook(db, NULL, NULL);
            if (rc != CBM_STORE_OK)
                evo_exec(db, "ROLLBACK;");
        } else {
            evo_exec(db, "ROLLBACK;");
        }
        evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
        evo_cross_process_guard_release(&process_guard);
        if (rc != CBM_STORE_OK) {
            free(out->report_json);
            memset(out, 0, sizeof(*out));
            return rc;
        }
        return CBM_STORE_OK;
    }
    int writes = 0;
    int task_events_written = 0;
    char sentinel_event[48];
    rc = evo_append_event(db, input->task_id, plan.project_uuid, "task", input->task_id,
                          "task_evolution", "system", NULL, payload_hash, payload_hash,
                          input->idempotency_key, payload_hash, sentinel_event);
    if (rc == CBM_STORE_OK) {
        writes++;
        task_events_written++;
    }
    if (rc == CBM_STORE_OK && input->failure_after_write > 0 &&
        writes >= input->failure_after_write)
        rc = CBM_STORE_ERR;

    for (int i = 0; rc == CBM_STORE_OK && i < plan.count; i++) {
        evo_attribution_t *row = &plan.rows[i];
        char before[256], after[256], before_hash[65], after_hash[65];
        double next_confidence = evo_clamp(row->confidence + row->delta, 0.0, 1.0);
        double next_reusability = evo_clamp(row->reusability + row->delta, 0.0, 1.0);
        double next_decay = evo_clamp(row->decay - row->delta, 0.0, 1.0);
        snprintf(before, sizeof(before), "%s|%.9f|%.9f|%.9f", row->status, row->confidence,
                 row->reusability, row->decay);
        snprintf(after, sizeof(after), "%s|%.9f|%.9f|%.9f", row->status, next_confidence,
                 next_reusability, next_decay);
        if (evo_hash_text(before, before_hash) != CBM_STORE_OK ||
            evo_hash_text(after, after_hash) != CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        sqlite3_stmt *stmt = NULL;
        if (rc == CBM_STORE_OK && row->delta != 0.0) {
            if (sqlite3_prepare_v2(db,
                                   "UPDATE memory_item SET confidence=?1,reusability=?2,decay=?3,"
                                   "updated_at=strftime('%s','now')*1000 WHERE id=?4 "
                                   "AND deleted_at IS NULL;",
                                   -1, &stmt, NULL) != SQLITE_OK)
                rc = CBM_STORE_ERR;
            if (rc == CBM_STORE_OK) {
                sqlite3_bind_double(stmt, 1, next_confidence);
                sqlite3_bind_double(stmt, 2, next_reusability);
                sqlite3_bind_double(stmt, 3, next_decay);
                sqlite3_bind_text(stmt, 4, row->memory_item_id, -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                                   : CBM_STORE_ERR;
            }
            sqlite3_finalize(stmt);
        }
        char event_key[1024], event_payload[2048], event_payload_hash[65], event_id[48];
        int key_used = snprintf(event_key, sizeof(event_key), "%s:%s", input->idempotency_key,
                                row->feedback_event_id);
        int payload_used =
            snprintf(event_payload, sizeof(event_payload), "%s\n%s\n%s\n%s\n%.9f", payload_hash,
                     row->memory_item_id, row->operation, row->evidence_id, row->delta);
        if (key_used < 0 || key_used >= (int)sizeof(event_key) || payload_used < 0 ||
            payload_used >= (int)sizeof(event_payload))
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK && evo_hash_text(event_payload, event_payload_hash) != CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        if (rc == CBM_STORE_OK)
            rc = evo_append_event(db, input->task_id, row->target_project_uuid, "memory_item",
                                  row->memory_item_id, row->operation, row->evidence_grade,
                                  row->evidence_id, before_hash, after_hash, event_key,
                                  event_payload_hash, event_id);
        if (rc == CBM_STORE_OK) {
            task_events_written++;
            writes++;
            if (task_events_written > plan.max_evolution_events)
                rc = CBM_STORE_REJECTED;
        }
        if (rc == CBM_STORE_OK) {
            int edge_written = 0;
            rc = evo_write_cross_project_edge(db, row, event_id, &edge_written);
            out->cross_project_edges += edge_written;
            writes += edge_written;
            if (out->cross_project_edges > plan.max_cross_project_edges)
                rc = CBM_STORE_REJECTED;
        }
        if (rc == CBM_STORE_OK && input->failure_after_write > 0 &&
            writes >= input->failure_after_write)
            rc = CBM_STORE_ERR;
    }
    if (rc == CBM_STORE_OK && (task_events_written != plan.planned_evolution_events ||
                               out->cross_project_edges != plan.planned_cross_project_edges))
        rc = CBM_STORE_ERR;
    char *committed_report = NULL;
    if (rc == CBM_STORE_OK) {
        committed_report = evo_report("applied", input->mode, 1, plan.count, out->positive,
                                      out->negative, task_events_written, out->cross_project_edges,
                                      0, 0, 0, "completed_task_feedback_evidence_gated");
        if (!committed_report)
            rc = CBM_STORE_ERR;
    }
    if (rc == CBM_STORE_OK) {
        if (input->crash_during_commit)
            sqlite3_commit_hook(db, evo_crash_commit_hook, NULL);
        rc = evo_exec(db, "COMMIT;");
        sqlite3_commit_hook(db, NULL, NULL);
        if (rc != CBM_STORE_OK)
            evo_exec(db, "ROLLBACK;");
    } else {
        evo_exec(db, "ROLLBACK;");
    }
    evo_restore_journal_modes(atomic_db, source_graph, &journal_guard);
    if (rc != CBM_STORE_OK) {
        free(committed_report);
        memset(out, 0, sizeof(*out));
        evo_cross_process_guard_release(&process_guard);
        return rc;
    }
    evo_cross_process_guard_release(&process_guard);
    out->wrote = 1;
    out->evolution_events = task_events_written;
    out->report_json = committed_report;
    return CBM_STORE_OK;
}

static int evo_task_evidence_eligible(sqlite3 *db, const cbm_evolution_memory_input_t *input,
                                      int *out_eligible) {
    if (!db || !input || !out_eligible)
        return CBM_STORE_ERR;
    *out_eligible = 0;
    if (!input->task_id || !input->evidence_id || !input->evidence_grade ||
        (strcmp(input->evidence_grade, "A") && strcmp(input->evidence_grade, "B")))
        return CBM_STORE_OK;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT COUNT(*) FROM codex_task_attribution a "
        "JOIN memory_evidence e ON e.evidence_id=a.evidence_id AND e.task_id=a.task_id "
        "JOIN memory_task_result r ON r.result_id=e.result_id AND r.task_id=a.task_id "
        "JOIN feedback_event f ON f.event_id=a.feedback_event_id AND f.task_id=a.task_id "
        "WHERE a.task_id=?1 AND a.memory_item_id=?2 AND a.evidence_id=?3 "
        "AND a.attribution_state IN ('used','rejected','contradicted') "
        "AND e.evidence_state='valid' AND r.status='succeeded' "
        "AND EXISTS(SELECT 1 FROM codex_task_lifecycle l WHERE l.task_id=a.task_id "
        "AND l.rowid=(SELECT latest.rowid FROM codex_task_lifecycle latest "
        "WHERE latest.task_id=a.task_id ORDER BY latest.rowid DESC LIMIT 1) "
        "AND l.state='completed' AND l.outcome='completed');";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->memory_item_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->evidence_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW)
        *out_eligible = sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW || step == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
}

typedef struct {
    char phase[24];
    char last_id[256];
    int attempt;
    int consolidated;
    int decayed;
    int archived;
    int edge_decisions;
    int edge_transitions;
    int concept_eligible;
    int concept_proposed;
} evo_checkpoint_t;

typedef struct {
    uint64_t deadline_ns;
    int completed_steps;
    int test_after_steps;
} evo_budget_t;

static uint64_t evo_monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) ||
        counter.QuadPart < 0 || frequency.QuadPart <= 0)
        return 0;
    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;
    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remainder = ticks % ticks_per_second;
    if (seconds > UINT64_MAX / UINT64_C(1000000000))
        return UINT64_MAX;
    uint64_t fraction =
        remainder <= UINT64_MAX / UINT64_C(1000000000)
            ? remainder * UINT64_C(1000000000) / ticks_per_second
            : (uint64_t)((long double)remainder * 1000000000.0L / (long double)ticks_per_second);
    return seconds * UINT64_C(1000000000) + fraction;
#else
    struct timespec value;
    return clock_gettime(CLOCK_MONOTONIC, &value) == 0
               ? (uint64_t)value.tv_sec * UINT64_C(1000000000) + (uint64_t)value.tv_nsec
               : 0;
#endif
}

static int evo_budget_expired(const evo_budget_t *budget) {
    if (!budget)
        return 0;
    if (budget->test_after_steps > 0 && budget->completed_steps >= budget->test_after_steps)
        return 1;
    uint64_t now = evo_monotonic_ns();
    return now && budget->deadline_ns && now >= budget->deadline_ns;
}

static int evo_budget_progress(void *context) {
    return evo_budget_expired((const evo_budget_t *)context);
}

static char *evo_checkpoint_json(const evo_checkpoint_t *checkpoint) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "phase", checkpoint->phase);
    yyjson_mut_obj_add_str(doc, root, "last_id", checkpoint->last_id);
    yyjson_mut_obj_add_int(doc, root, "attempt", checkpoint->attempt);
    yyjson_mut_obj_add_int(doc, root, "consolidated", checkpoint->consolidated);
    yyjson_mut_obj_add_int(doc, root, "decayed", checkpoint->decayed);
    yyjson_mut_obj_add_int(doc, root, "archived", checkpoint->archived);
    yyjson_mut_obj_add_int(doc, root, "edge_decisions", checkpoint->edge_decisions);
    yyjson_mut_obj_add_int(doc, root, "edge_transitions", checkpoint->edge_transitions);
    yyjson_mut_obj_add_int(doc, root, "concept_eligible", checkpoint->concept_eligible);
    yyjson_mut_obj_add_int(doc, root, "concept_proposed", checkpoint->concept_proposed);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int evo_checkpoint_parse(const char *json, evo_checkpoint_t *checkpoint) {
    memset(checkpoint, 0, sizeof(*checkpoint));
    snprintf(checkpoint->phase, sizeof(checkpoint->phase), "consolidate");
    if (!json || !json[0])
        return CBM_STORE_OK;
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *phase = root ? yyjson_obj_get(root, "phase") : NULL;
    yyjson_val *last_id = root ? yyjson_obj_get(root, "last_id") : NULL;
    if (!root || !yyjson_is_obj(root) || !phase || !yyjson_is_str(phase)) {
        yyjson_doc_free(doc);
        return CBM_STORE_ERR;
    }
    snprintf(checkpoint->phase, sizeof(checkpoint->phase), "%s", yyjson_get_str(phase));
    if (last_id && yyjson_is_str(last_id))
        snprintf(checkpoint->last_id, sizeof(checkpoint->last_id), "%s", yyjson_get_str(last_id));
#define EVO_CHECKPOINT_INT(field)                            \
    do {                                                     \
        yyjson_val *value = yyjson_obj_get(root, #field);    \
        if (value && yyjson_is_int(value))                   \
            checkpoint->field = (int)yyjson_get_sint(value); \
    } while (0)
    EVO_CHECKPOINT_INT(attempt);
    EVO_CHECKPOINT_INT(consolidated);
    EVO_CHECKPOINT_INT(decayed);
    EVO_CHECKPOINT_INT(archived);
    EVO_CHECKPOINT_INT(edge_decisions);
    EVO_CHECKPOINT_INT(edge_transitions);
    EVO_CHECKPOINT_INT(concept_eligible);
    EVO_CHECKPOINT_INT(concept_proposed);
#undef EVO_CHECKPOINT_INT
    yyjson_doc_free(doc);
    return CBM_STORE_OK;
}

static void evo_result_from_checkpoint(cbm_evolution_result_t *out,
                                       const evo_checkpoint_t *checkpoint) {
    out->consolidated = checkpoint->consolidated;
    out->decayed = checkpoint->decayed;
    out->archived = checkpoint->archived;
    out->edge_decisions = checkpoint->edge_decisions;
    out->edge_transitions = checkpoint->edge_transitions;
    out->concept_eligible = checkpoint->concept_eligible;
    out->concept_proposed = checkpoint->concept_proposed;
}

static int evo_count_query(sqlite3 *db, const char *sql, const char *scope, int limit) {
    sqlite3_stmt *stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        evo_bind_text(stmt, 1, scope);
        if (limit >= 0)
            sqlite3_bind_int(stmt, 2, limit);
        int step = sqlite3_step(stmt);
        if (step == SQLITE_ROW)
            value = sqlite3_column_int(stmt, 0);
        else if (step != SQLITE_DONE)
            value = -1;
    }
    sqlite3_finalize(stmt);
    return value;
}

static int evo_maintenance_event_count(sqlite3 *db, const char *run_id) {
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM global_evolution_event "
                           "WHERE object_kind='maintenance_run' AND object_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, run_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    else if (step != SQLITE_DONE)
        count = -1;
    sqlite3_finalize(stmt);
    return count;
}

static int evo_store_checkpoint(sqlite3 *db, const cbm_evolution_maintenance_input_t *input,
                                const char *payload_hash, const evo_checkpoint_t *checkpoint,
                                const char *status, const char *lease_expires,
                                int expected_run_attempt, int expected_lease_attempt) {
    if (expected_run_attempt < 0 || expected_lease_attempt < 1)
        return CBM_STORE_ERR;
    char *json = evo_checkpoint_json(checkpoint);
    if (!json)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "UPDATE global_maintenance_run SET status=?1,checkpoint_json=?2,consolidated_count=?3,"
        "decayed_count=?4,archived_count=?5,completed_at=CASE WHEN ?1='completed' THEN "
        "strftime('%Y-%m-%dT%H:%M:%SZ','now') ELSE NULL END WHERE run_id=?6 "
        "AND payload_sha256=?7 AND COALESCE(CAST(json_extract(checkpoint_json,'$.attempt') "
        "AS INTEGER),0)=?8;";
    int rc =
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK ? CBM_STORE_OK : CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        sqlite3_bind_text(stmt, 1, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, checkpoint->consolidated);
        sqlite3_bind_int(stmt, 4, checkpoint->decayed);
        sqlite3_bind_int(stmt, 5, checkpoint->archived);
        sqlite3_bind_text(stmt, 6, input->run_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, payload_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, expected_run_attempt);
        rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                           : CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rc == CBM_STORE_OK &&
        sqlite3_prepare_v2(
            db,
            "UPDATE global_maintenance_lease SET expires_at=?1,checkpoint_json=?2,"
            "payload_sha256=?3 WHERE lease_name='stage14-global-maintenance' "
            "AND owner_id=?4 AND payload_sha256=?3 "
            "AND COALESCE(CAST(json_extract(checkpoint_json,'$.attempt') AS INTEGER),0)=?5;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, lease_expires, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, payload_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, input->owner_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, expected_lease_attempt);
        rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                           : CBM_STORE_ERR;
    } else if (rc == CBM_STORE_OK) {
        rc = CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    free(json);
    return rc;
}

static int evo_checkpoint_event(sqlite3 *db, const cbm_evolution_maintenance_input_t *input,
                                const char *payload_hash, const evo_checkpoint_t *checkpoint,
                                const char *operation, const char *suffix) {
    char *json = evo_checkpoint_json(checkpoint);
    if (!json)
        return CBM_STORE_ERR;
    char checkpoint_hash[65], key[1024], event_id[48];
    int rc = evo_hash_text(json, checkpoint_hash);
    free(json);
    if (rc != CBM_STORE_OK)
        return rc;
    snprintf(key, sizeof(key), "%s:%s", input->idempotency_key, suffix);
    return evo_append_event(db, NULL, input->project_uuid, "maintenance_run", input->run_id,
                            operation, "system", NULL, payload_hash, checkpoint_hash, key,
                            checkpoint_hash, event_id);
}

static int evo_pause_maintenance(sqlite3 *db, const cbm_evolution_maintenance_input_t *input,
                                 const char *payload_hash, const evo_checkpoint_t *checkpoint,
                                 cbm_evolution_result_t *out, const char *reason, int return_code) {
    if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    char now[40], suffix[128];
    evo_timestamp(time(NULL), now);
    snprintf(suffix, sizeof(suffix), "checkpoint:%d", checkpoint->attempt);
    int rc = evo_store_checkpoint(db, input, payload_hash, checkpoint, "checkpointed", now,
                                  checkpoint->attempt, checkpoint->attempt);
    if (rc == CBM_STORE_OK)
        rc = evo_checkpoint_event(db, input, payload_hash, checkpoint, "checkpoint", suffix);
    cbm_evolution_result_t prepared = {0};
    char *report = NULL;
    if (rc == CBM_STORE_OK) {
        evo_result_from_checkpoint(&prepared, checkpoint);
        prepared.checkpointed = 1;
        prepared.wrote = 1;
        prepared.evolution_events = evo_maintenance_event_count(db, input->run_id);
        if (prepared.evolution_events < 0)
            rc = CBM_STORE_ERR;
    }
    if (rc == CBM_STORE_OK && input->failure_before_report)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        report = evo_maintenance_report("checkpointed", input->mode, &prepared, checkpoint->phase,
                                        reason);
        if (!report)
            rc = CBM_STORE_ERR;
    }
    if (rc == CBM_STORE_OK) {
        rc = evo_exec(db, "COMMIT;");
        if (rc != CBM_STORE_OK)
            evo_exec(db, "ROLLBACK;");
    } else {
        evo_exec(db, "ROLLBACK;");
    }
    if (rc != CBM_STORE_OK) {
        free(report);
        return rc;
    }
    *out = prepared;
    out->report_json = report;
    return return_code;
}

int cbm_evolution_memory_state(cbm_global_memory_t *global,
                               const cbm_evolution_memory_input_t *input,
                               cbm_evolution_result_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!global || !input || !out || !evo_mode_valid(input->mode) || !input->project_uuid ||
        !input->memory_item_id || !input->operation ||
        (strcmp(input->operation, "archive") && strcmp(input->operation, "restore")) ||
        !input->idempotency_key)
        return CBM_STORE_ERR;
    sqlite3 *db = cbm_global_memory_db(global);
    int eligible = 0;
    int rc = evo_task_evidence_eligible(db, input, &eligible);
    if (rc != CBM_STORE_OK)
        return rc;
    if (!evo_mode_writes(input->mode)) {
        out->report_json =
            evo_report(!strcmp(input->mode, "off") ? "off" : "planned", input->mode, 0, eligible, 0,
                       0, 0, 0, 0, 0, 0, eligible ? "evidence_eligible" : "evidence_ineligible");
        return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    if (!input->isolated_write_allowed || !eligible)
        return CBM_STORE_REJECTED;
    char payload[1024], payload_hash[65];
    snprintf(payload, sizeof(payload), "%s\n%s\n%s\n%s\n%s\n%s\n%s", input->mode, input->task_id,
             input->project_uuid, input->memory_item_id, input->operation, input->evidence_grade,
             input->evidence_id);
    if (evo_hash_text(payload, payload_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    int exact = 0;
    int existing = evo_event_lookup(db, input->idempotency_key, payload_hash, &exact);
    if (existing == CBM_STORE_OK)
        return exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT;
    if (existing != CBM_STORE_NOT_FOUND)
        return existing;
    if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    char before[32] = {0};
    if (sqlite3_prepare_v2(db, "SELECT status FROM memory_item WHERE id=?1 AND deleted_at IS NULL;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->memory_item_id, -1, SQLITE_TRANSIENT);
    int found = sqlite3_step(stmt);
    if (found == SQLITE_ROW)
        snprintf(before, sizeof(before), "%s", sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    if (found != SQLITE_ROW) {
        evo_exec(db, "ROLLBACK;");
        return found == SQLITE_DONE ? CBM_STORE_NOT_FOUND : CBM_STORE_ERR;
    }
    const char *after = !strcmp(input->operation, "archive") ? "archived" : "active";
    if (sqlite3_prepare_v2(db,
                           "UPDATE memory_item SET status=?1,updated_at=strftime('%s','now')*1000 "
                           "WHERE id=?2;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, after, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->memory_item_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                       : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    char before_hash[65], after_hash[65], event_id[48];
    evo_hash_text(before, before_hash);
    evo_hash_text(after, after_hash);
    if (rc == CBM_STORE_OK)
        rc = evo_append_event(db, input->task_id, input->project_uuid, "memory_item",
                              input->memory_item_id, input->operation, input->evidence_grade,
                              input->evidence_id, before_hash, after_hash, input->idempotency_key,
                              payload_hash, event_id);
    if (rc == CBM_STORE_OK)
        rc = evo_exec(db, "COMMIT;");
    else
        evo_exec(db, "ROLLBACK;");
    if (rc != CBM_STORE_OK)
        return rc;
    out->wrote = 1;
    out->archived = !strcmp(after, "archived");
    out->evolution_events = 1;
    out->report_json = evo_report("applied", input->mode, 1, 1, 0, 0, 1, 0, 0, 0, out->archived,
                                  "evidence_gated_reversible_state");
    return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int evo_maintenance_preview(cbm_store_t *store,
                                   const cbm_evolution_maintenance_input_t *input,
                                   cbm_evolution_result_t *out) {
    sqlite3 *db = cbm_store_get_db(store);
    const char *candidate_sql =
        "SELECT COUNT(*) FROM (SELECT id FROM memory_item WHERE status='candidate' "
        "AND deleted_at IS NULL AND (?1='*' OR scope_project=?1) ORDER BY id LIMIT ?2);";
    const char *active_sql =
        "SELECT COUNT(*) FROM (SELECT id FROM memory_item WHERE status='active' "
        "AND deleted_at IS NULL AND (?1='*' OR scope_project=?1) ORDER BY id LIMIT ?2);";
    int candidates = evo_count_query(db, candidate_sql, input->project_uuid, input->limit);
    int active = evo_count_query(db, active_sql, input->project_uuid, input->limit);
    if (candidates < 0 || active < 0)
        return CBM_STORE_ERR;
    out->eligible = active;
    out->consolidated = candidates;
    out->decayed = active;
    const char *preview_mode = !strcmp(input->mode, "shadow") ? "shadow" : "dry_run";
    cbm_edge_lifecycle_input_t edge = {.project = input->project_uuid,
                                       .mode = preview_mode,
                                       .run_id = input->run_id ? input->run_id
                                                               : "stage14-maintenance-preview-edge",
                                       .as_of_ms = input->frozen_as_of_ms,
                                       .algorithm_version = CBM_STAGE9_ALGORITHM_VERSION,
                                       .policy_sha256 = CBM_STAGE9_POLICY_SHA256,
                                       .policy_version = CBM_STAGE9_POLICY_VERSION,
                                       .config_version = CBM_STAGE9_CONFIG_VERSION};
    cbm_edge_lifecycle_result_t edge_result = {0};
    int rc = cbm_store_memory_edge_maintenance(store, &edge, &edge_result);
    if (rc == CBM_STORE_OK) {
        out->edge_decisions = edge_result.decision_count;
        out->edge_transitions = edge_result.transition_count;
    }
    cbm_store_memory_edge_lifecycle_result_free(&edge_result);
    if (rc != CBM_STORE_OK)
        return rc;
    cbm_concept_generate_input_t
        concept = {.project = input->project_uuid,
                   .store = "project-memory",
                   .operation = "evaluate",
                   .mode = preview_mode,
                   .run_id = input->run_id ? input->run_id : "stage14-maintenance-preview-concept",
                   .idempotency_key = input->idempotency_key,
                   .algorithm_version = CBM_STAGE10_ALGORITHM_VERSION,
                   .policy_sha256 = CBM_STAGE10_POLICY_SHA256,
                   .policy_version = CBM_STAGE10_POLICY_VERSION,
                   .config_version = CBM_STAGE10_CONFIG_VERSION,
                   .generator_version = CBM_STAGE10_GENERATOR_VERSION};
    cbm_concept_result_t concept_result = {0};
    rc = cbm_store_memory_concept_generate(store, &concept, &concept_result);
    if (rc == CBM_STORE_OK) {
        out->concept_eligible = concept_result.eligible_count;
        out->concept_proposed = concept_result.proposed_count;
    }
    cbm_store_memory_concept_result_free(&concept_result);
    if (rc != CBM_STORE_OK)
        return rc;
    out->report_json = evo_maintenance_report("planned", input->mode, out, "preview",
                                              "byte_stable_full_pipeline_preview");
    return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int evo_initialize_maintenance(sqlite3 *db, const cbm_evolution_maintenance_input_t *input,
                                      const char *payload_hash, const char *lease_expires,
                                      evo_checkpoint_t *checkpoint, cbm_evolution_result_t *out) {
    if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    char stored_status[24] = {0}, stored_payload[65] = {0};
    char *stored_checkpoint = NULL;
    int existing = 0;
    if (sqlite3_prepare_v2(db,
                           "SELECT status,payload_sha256,checkpoint_json FROM "
                           "global_maintenance_run WHERE idempotency_key=?1;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->idempotency_key, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        existing = 1;
        snprintf(stored_status, sizeof(stored_status), "%s", sqlite3_column_text(stmt, 0));
        snprintf(stored_payload, sizeof(stored_payload), "%s", sqlite3_column_text(stmt, 1));
        const char *value = (const char *)sqlite3_column_text(stmt, 2);
        stored_checkpoint = evo_dup(value);
    } else if (step != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (existing && strcmp(stored_payload, payload_hash) != 0) {
        free(stored_checkpoint);
        evo_exec(db, "ROLLBACK;");
        out->report_json = evo_maintenance_report("IDEMPOTENCY_CONFLICT", input->mode, out,
                                                  "lookup", "altered_maintenance_payload");
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    if (existing && evo_checkpoint_parse(stored_checkpoint, checkpoint) != CBM_STORE_OK) {
        free(stored_checkpoint);
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    free(stored_checkpoint);
    if (existing && !strcmp(stored_status, "completed")) {
        evo_exec(db, "ROLLBACK;");
        evo_result_from_checkpoint(out, checkpoint);
        out->replayed = 1;
        out->evolution_events = evo_maintenance_event_count(db, input->run_id);
        if (out->evolution_events < 0)
            return CBM_STORE_ERR;
        out->report_json = evo_maintenance_report("replayed", input->mode, out, "completed",
                                                  "exact_completed_replay_zero_write");
        return out->report_json ? CBM_STORE_REPLAYED : CBM_STORE_ERR;
    }
    int run_attempt_before_acquire = 0;
    if (!existing) {
        memset(checkpoint, 0, sizeof(*checkpoint));
        snprintf(checkpoint->phase, sizeof(checkpoint->phase), "consolidate");
        checkpoint->attempt = 1;
        run_attempt_before_acquire = checkpoint->attempt;
        char *json = evo_checkpoint_json(checkpoint);
        const char *sql =
            "INSERT INTO global_maintenance_run(run_id,project_uuid,mode,status,owner_id,"
            "limit_count,budget_seconds,checkpoint_json,consolidated_count,decayed_count,"
            "archived_count,idempotency_key,payload_sha256,started_at,completed_at) VALUES("
            "?1,?2,?3,'running',?4,?5,?6,?7,0,0,0,?8,?9,"
            "strftime('%Y-%m-%dT%H:%M:%SZ','now'),NULL);";
        if (!json || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            free(json);
            evo_exec(db, "ROLLBACK;");
            return CBM_STORE_ERR;
        }
        sqlite3_bind_text(stmt, 1, input->run_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, input->project_uuid, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, input->mode, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, input->owner_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, input->limit);
        sqlite3_bind_int(stmt, 6, input->budget_seconds);
        sqlite3_bind_text(stmt, 7, json, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, input->idempotency_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, payload_hash, -1, SQLITE_TRANSIENT);
        int inserted = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        stmt = NULL;
        free(json);
        if (!inserted) {
            evo_exec(db, "ROLLBACK;");
            return CBM_STORE_ERR;
        }
    } else {
        if (checkpoint->attempt == INT_MAX) {
            evo_exec(db, "ROLLBACK;");
            return CBM_STORE_ERR;
        }
        run_attempt_before_acquire = checkpoint->attempt;
        checkpoint->attempt++;
    }
    char now[40];
    evo_timestamp(time(NULL), now);
    char *checkpoint_json = evo_checkpoint_json(checkpoint);
    const char *lease_sql =
        "INSERT INTO global_maintenance_lease(lease_name,owner_id,acquired_at,expires_at,"
        "checkpoint_json,payload_sha256) VALUES('stage14-global-maintenance',?1,?2,?3,?4,?5) "
        "ON CONFLICT(lease_name) DO UPDATE SET owner_id=excluded.owner_id,"
        "acquired_at=excluded.acquired_at,expires_at=excluded.expires_at,"
        "checkpoint_json=excluded.checkpoint_json,payload_sha256=excluded.payload_sha256 "
        "WHERE global_maintenance_lease.expires_at<=excluded.acquired_at;";
    if (!checkpoint_json || sqlite3_prepare_v2(db, lease_sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(checkpoint_json);
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->owner_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, now, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, lease_expires, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, checkpoint_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, payload_hash, -1, SQLITE_TRANSIENT);
    int lease_step = sqlite3_step(stmt);
    int lease_ok = lease_step == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(stmt);
    stmt = NULL;
    free(checkpoint_json);
    if (!lease_ok ||
        evo_store_checkpoint(db, input, payload_hash, checkpoint, "running", lease_expires,
                             run_attempt_before_acquire, checkpoint->attempt) != CBM_STORE_OK) {
        evo_exec(db, "ROLLBACK;");
        if (lease_step != SQLITE_DONE)
            return CBM_STORE_ERR;
        return lease_ok ? CBM_STORE_ERR : CBM_STORE_REJECTED;
    }
    char suffix[64];
    snprintf(suffix, sizeof(suffix), "%s:%d", existing ? "resume" : "begin", checkpoint->attempt);
    if (evo_checkpoint_event(db, input, payload_hash, checkpoint, existing ? "resume" : "begin",
                             suffix) != CBM_STORE_OK ||
        evo_exec(db, "COMMIT;") != CBM_STORE_OK) {
        evo_exec(db, "ROLLBACK;");
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static int evo_phase_commit(sqlite3 *db, const cbm_evolution_maintenance_input_t *input,
                            const char *payload_hash, const char *lease_expires,
                            const evo_checkpoint_t *checkpoint, const char *operation,
                            const char *suffix) {
    int rc = evo_store_checkpoint(db, input, payload_hash, checkpoint, "running", lease_expires,
                                  checkpoint->attempt, checkpoint->attempt);
    if (rc == CBM_STORE_OK)
        rc = evo_checkpoint_event(db, input, payload_hash, checkpoint, operation, suffix);
    if (rc == CBM_STORE_OK)
        rc = evo_exec(db, "COMMIT;");
    else
        evo_exec(db, "ROLLBACK;");
    return rc;
}

static int evo_fault_due(const cbm_evolution_maintenance_input_t *input,
                         const evo_budget_t *budget) {
    return input->failure_after_write > 0 &&
           budget->completed_steps + 1 >= input->failure_after_write;
}

static int evo_maintenance_controller(cbm_store_t *store,
                                      const cbm_evolution_maintenance_input_t *input,
                                      cbm_evolution_result_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    if (!store || !input || !out || !evo_mode_valid(input->mode) || !input->project_uuid ||
        !input->owner_id || !input->idempotency_key || input->limit < 1 || input->limit > 1000 ||
        input->budget_seconds < 1 || input->budget_seconds > 30 || input->failure_after_write < 0 ||
        (input->failure_before_report != 0 && input->failure_before_report != 1) ||
        input->test_budget_after_steps < 0 ||
        (input->isolated_write_allowed != 0 && input->isolated_write_allowed != 1) ||
        (input->production_gate_allowed != 0 && input->production_gate_allowed != 1))
        return CBM_STORE_ERR;
    if (!evo_mode_writes(input->mode))
        return evo_maintenance_preview(store, input, out);
    int production = !input->isolated_write_allowed;
    if (production && strcmp(input->mode, "bounded_canary") != 0)
        return CBM_STORE_REJECTED;
    if (production && (input->failure_after_write != 0 || input->failure_before_report != 0 ||
                       input->test_budget_after_steps != 0))
        return CBM_STORE_REJECTED;
    if (!evo_write_guard(input->mode, input->isolated_write_allowed,
                         input->production_gate_allowed))
        return CBM_STORE_REJECTED;
    if (!input->run_id || !input->run_id[0] || input->frozen_as_of_ms <= 0 ||
        !evo_absolute_path(input->edge_manifest_path) ||
        !evo_lower_sha256(input->edge_manifest_sha256) ||
        !evo_absolute_path(input->concept_manifest_path) ||
        !evo_lower_sha256(input->concept_manifest_sha256))
        return CBM_STORE_ERR;

    char canonical[8192], payload_hash[65];
    int used = snprintf(canonical, sizeof(canonical),
                        "stage14-maintenance/v3\n%s\n%s\n%s\n%s\n%d\n%d\n%lld\n%s\n%s\n%s\n%s",
                        input->mode, input->project_uuid, input->owner_id, input->run_id,
                        input->limit, input->budget_seconds, (long long)input->frozen_as_of_ms,
                        input->edge_manifest_path, input->edge_manifest_sha256,
                        input->concept_manifest_path, input->concept_manifest_sha256);
    if (used < 0 || used >= (int)sizeof(canonical) ||
        evo_hash_text(canonical, payload_hash) != CBM_STORE_OK)
        return CBM_STORE_ERR;

    if (!evo_manifest_raw_matches(input->edge_manifest_path, input->edge_manifest_sha256,
                                  EVO_MAX_EDGE_MANIFEST_BYTES) ||
        !evo_manifest_raw_matches(input->concept_manifest_path, input->concept_manifest_sha256,
                                  EVO_MAX_CONCEPT_MANIFEST_BYTES))
        return CBM_STORE_REJECTED;

    sqlite3 *db = cbm_store_get_db(store);
    char edge_run_id[512], concept_run_id[512], concept_key[1024];
    snprintf(edge_run_id, sizeof(edge_run_id), "%s:edge", input->run_id);
    snprintf(concept_run_id, sizeof(concept_run_id), "%s:concept", input->run_id);
    snprintf(concept_key, sizeof(concept_key), "%s:concept", input->idempotency_key);
    cbm_edge_lifecycle_input_t edge = {.project = input->project_uuid,
                                       .mode = "active",
                                       .run_id = edge_run_id,
                                       .as_of_ms = input->frozen_as_of_ms,
                                       .algorithm_version = CBM_STAGE9_ALGORITHM_VERSION,
                                       .policy_sha256 = CBM_STAGE9_POLICY_SHA256,
                                       .policy_version = CBM_STAGE9_POLICY_VERSION,
                                       .config_version = CBM_STAGE9_CONFIG_VERSION,
                                       .manifest_path = input->edge_manifest_path,
                                       .manifest_sha256 = input->edge_manifest_sha256};
    cbm_concept_generate_input_t concept = {.project = input->project_uuid,
                                            .store = "project-memory",
                                            .operation = "generate",
                                            .mode = "active",
                                            .run_id = concept_run_id,
                                            .idempotency_key = concept_key,
                                            .algorithm_version = CBM_STAGE10_ALGORITHM_VERSION,
                                            .policy_sha256 = CBM_STAGE10_POLICY_SHA256,
                                            .policy_version = CBM_STAGE10_POLICY_VERSION,
                                            .config_version = CBM_STAGE10_CONFIG_VERSION,
                                            .generator_version = CBM_STAGE10_GENERATOR_VERSION,
                                            .manifest_path = input->concept_manifest_path,
                                            .manifest_sha256 = input->concept_manifest_sha256};

    uint64_t started_ns = evo_monotonic_ns();
    if (!started_ns)
        return CBM_STORE_ERR;
    uint64_t duration_ns = (uint64_t)input->budget_seconds * UINT64_C(1000000000);
    uint64_t deadline_ns = started_ns && UINT64_MAX - started_ns >= duration_ns
                               ? started_ns + duration_ns
                               : (started_ns ? UINT64_MAX : 0);
    evo_budget_t budget = {.deadline_ns = deadline_ns,
                           .completed_steps = 0,
                           .test_after_steps = input->test_budget_after_steps};
    time_t wall_started = time(NULL);
    char lease_expires[40];
    evo_timestamp(wall_started + input->budget_seconds, lease_expires);
    int eligible = evo_count_query(
        db,
        "SELECT COUNT(*) FROM (SELECT id FROM memory_item WHERE status='active' AND "
        "deleted_at IS NULL AND (?1='*' OR scope_project=?1) ORDER BY id LIMIT ?2);",
        input->project_uuid, input->limit);
    if (eligible < 0)
        return CBM_STORE_ERR;
    evo_checkpoint_t checkpoint;
    int rc = evo_initialize_maintenance(db, input, payload_hash, lease_expires, &checkpoint, out);
    if (rc != CBM_STORE_OK)
        return rc;
    out->eligible = eligible;
    const char *consolidate_scope = !strcmp(input->project_uuid, "*") ? NULL : input->project_uuid;

    while (strcmp(checkpoint.phase, "completed") != 0) {
        if (evo_budget_expired(&budget))
            return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                         "monotonic_budget_exhausted", CBM_STORE_CHECKPOINTED);

        if (!strcmp(checkpoint.phase, "consolidate")) {
            int remaining = input->limit - checkpoint.consolidated;
            int batch = remaining > 16 ? 16 : remaining;
            evo_checkpoint_t next = checkpoint;
            int processed = 0;
            if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
                return CBM_STORE_ERR;
            sqlite3_progress_handler(db, 1000, evo_budget_progress, &budget);
            rc = batch > 0 ? cbm_store_memory_consolidate_in_transaction(
                                 store, consolidate_scope, batch, input->run_id, &processed)
                           : CBM_STORE_OK;
            sqlite3_progress_handler(db, 0, NULL, NULL);
            if (rc != CBM_STORE_OK || evo_budget_expired(&budget) ||
                evo_fault_due(input, &budget)) {
                evo_exec(db, "ROLLBACK;");
                if (evo_budget_expired(&budget))
                    return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                                 "monotonic_budget_exhausted",
                                                 CBM_STORE_CHECKPOINTED);
                return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                             rc == CBM_STORE_OK ? "injected_consolidate_fault"
                                                                : "consolidate_failed",
                                             rc == CBM_STORE_OK ? CBM_STORE_ERR : rc);
            }
            next.consolidated += processed;
            if (processed < batch || next.consolidated >= input->limit)
                snprintf(next.phase, sizeof(next.phase), "decay");
            char suffix[96];
            snprintf(suffix, sizeof(suffix), "consolidate:%d", next.consolidated);
            rc = evo_phase_commit(db, input, payload_hash, lease_expires, &next,
                                  processed ? "consolidate" : "consolidate_complete", suffix);
            if (rc != CBM_STORE_OK)
                return rc;
            checkpoint = next;
            budget.completed_steps++;
            continue;
        }

        if (!strcmp(checkpoint.phase, "decay")) {
            int remaining = input->limit - checkpoint.decayed;
            int batch = remaining > 16 ? 16 : remaining;
            evo_checkpoint_t next = checkpoint;
            char ids[16][256];
            int count = 0;
            if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
                return CBM_STORE_ERR;
            sqlite3_progress_handler(db, 1000, evo_budget_progress, &budget);
            sqlite3_stmt *stmt = NULL;
            if (batch > 0 &&
                sqlite3_prepare_v2(
                    db,
                    "SELECT id FROM memory_item WHERE status='active' AND deleted_at IS NULL "
                    "AND (?1='*' OR scope_project=?1) AND id>?2 ORDER BY id LIMIT ?3;",
                    -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, input->project_uuid, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, checkpoint.last_id, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 3, batch);
                int select_step = SQLITE_DONE;
                while ((select_step = sqlite3_step(stmt)) == SQLITE_ROW) {
                    const char *id = (const char *)sqlite3_column_text(stmt, 0);
                    if (count >= batch || !id || strlen(id) >= sizeof(ids[count])) {
                        rc = CBM_STORE_ERR;
                        break;
                    }
                    snprintf(ids[count], sizeof(ids[count]), "%s", id);
                    count++;
                }
                if (rc == CBM_STORE_OK && select_step != SQLITE_DONE)
                    rc = CBM_STORE_ERR;
            } else if (batch > 0) {
                rc = CBM_STORE_ERR;
            }
            sqlite3_finalize(stmt);
            double increment = !strcmp(input->mode, "active") ? 0.02 : 0.01;
            for (int i = 0; rc == CBM_STORE_OK && i < count; i++) {
                if (sqlite3_prepare_v2(
                        db,
                        "UPDATE memory_item SET decay=MIN(1.0,decay+?1),"
                        "status=CASE WHEN decay+?1>=1.0 THEN 'archived' ELSE 'active' END,"
                        "updated_at=strftime('%s','now')*1000 WHERE id=?2 AND status='active';",
                        -1, &stmt, NULL) != SQLITE_OK) {
                    rc = CBM_STORE_ERR;
                    break;
                }
                sqlite3_bind_double(stmt, 1, increment);
                sqlite3_bind_text(stmt, 2, ids[i], -1, SQLITE_TRANSIENT);
                rc = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1 ? CBM_STORE_OK
                                                                                   : CBM_STORE_ERR;
                sqlite3_finalize(stmt);
                stmt = NULL;
                if (rc == CBM_STORE_OK) {
                    sqlite3_stmt *state = NULL;
                    if (sqlite3_prepare_v2(db, "SELECT status FROM memory_item WHERE id=?1;", -1,
                                           &state, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(state, 1, ids[i], -1, SQLITE_TRANSIENT);
                        int state_step = sqlite3_step(state);
                        const char *status = state_step == SQLITE_ROW
                                                 ? (const char *)sqlite3_column_text(state, 0)
                                                 : NULL;
                        if (state_step == SQLITE_ROW && status) {
                            if (!strcmp(status, "archived"))
                                next.archived++;
                        } else {
                            rc = CBM_STORE_ERR;
                        }
                    } else
                        rc = CBM_STORE_ERR;
                    sqlite3_finalize(state);
                    if (rc == CBM_STORE_OK) {
                        next.decayed++;
                        snprintf(next.last_id, sizeof(next.last_id), "%s", ids[i]);
                    }
                }
            }
            sqlite3_progress_handler(db, 0, NULL, NULL);
            if (rc != CBM_STORE_OK || evo_budget_expired(&budget) ||
                evo_fault_due(input, &budget)) {
                evo_exec(db, "ROLLBACK;");
                if (evo_budget_expired(&budget))
                    return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                                 "monotonic_budget_exhausted",
                                                 CBM_STORE_CHECKPOINTED);
                return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                             rc == CBM_STORE_OK ? "injected_decay_fault"
                                                                : "decay_failed",
                                             rc == CBM_STORE_OK ? CBM_STORE_ERR : rc);
            }
            if (count < batch || next.decayed >= input->limit)
                snprintf(next.phase, sizeof(next.phase), "edge");
            char suffix[96];
            snprintf(suffix, sizeof(suffix), "decay:%d", next.decayed);
            rc = evo_phase_commit(db, input, payload_hash, lease_expires, &next,
                                  count ? "decay" : "decay_complete", suffix);
            if (rc != CBM_STORE_OK)
                return rc;
            checkpoint = next;
            budget.completed_steps++;
            continue;
        }

        if (!strcmp(checkpoint.phase, "edge")) {
            cbm_edge_lifecycle_result_t edge_result = {0};
            if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
                return CBM_STORE_ERR;
            sqlite3_progress_handler(db, 1000, evo_budget_progress, &budget);
            rc = cbm_store_memory_edge_maintenance_in_transaction(
                store, &edge, input->run_id, input->production_gate_allowed != 0, &edge_result);
            sqlite3_progress_handler(db, 0, NULL, NULL);
            if (rc != CBM_STORE_OK || evo_budget_expired(&budget) ||
                evo_fault_due(input, &budget)) {
                cbm_store_memory_edge_lifecycle_result_free(&edge_result);
                evo_exec(db, "ROLLBACK;");
                if (evo_budget_expired(&budget))
                    return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                                 "monotonic_budget_exhausted",
                                                 CBM_STORE_CHECKPOINTED);
                return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                             rc == CBM_STORE_OK ? "injected_edge_fault"
                                                                : "edge_lifecycle_failed",
                                             rc == CBM_STORE_OK ? CBM_STORE_ERR : rc);
            }
            evo_checkpoint_t next = checkpoint;
            next.edge_decisions = edge_result.decision_count;
            next.edge_transitions = edge_result.transition_count;
            snprintf(next.phase, sizeof(next.phase), "concept");
            cbm_store_memory_edge_lifecycle_result_free(&edge_result);
            rc = evo_phase_commit(db, input, payload_hash, lease_expires, &next, "edge_lifecycle",
                                  "edge");
            if (rc != CBM_STORE_OK)
                return rc;
            checkpoint = next;
            budget.completed_steps++;
            continue;
        }

        if (!strcmp(checkpoint.phase, "concept")) {
            cbm_concept_result_t concept_result = {0};
            if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
                return CBM_STORE_ERR;
            sqlite3_progress_handler(db, 1000, evo_budget_progress, &budget);
            rc = cbm_store_memory_concept_generate_in_transaction(
                store, &concept, input->run_id, input->production_gate_allowed != 0,
                &concept_result);
            sqlite3_progress_handler(db, 0, NULL, NULL);
            if ((rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED) || evo_budget_expired(&budget) ||
                evo_fault_due(input, &budget)) {
                cbm_store_memory_concept_result_free(&concept_result);
                evo_exec(db, "ROLLBACK;");
                if (evo_budget_expired(&budget))
                    return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                                 "monotonic_budget_exhausted",
                                                 CBM_STORE_CHECKPOINTED);
                return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                             rc == CBM_STORE_OK ? "injected_concept_fault"
                                                                : "concept_growth_failed",
                                             rc == CBM_STORE_OK ? CBM_STORE_ERR : rc);
            }
            evo_checkpoint_t next = checkpoint;
            next.concept_eligible = concept_result.eligible_count;
            next.concept_proposed = concept_result.proposed_count;
            snprintf(next.phase, sizeof(next.phase), "finalize");
            cbm_store_memory_concept_result_free(&concept_result);
            rc = evo_phase_commit(db, input, payload_hash, lease_expires, &next, "concept_growth",
                                  "concept");
            if (rc != CBM_STORE_OK)
                return rc;
            checkpoint = next;
            budget.completed_steps++;
            continue;
        }

        if (!strcmp(checkpoint.phase, "finalize")) {
            evo_checkpoint_t next = checkpoint;
            snprintf(next.phase, sizeof(next.phase), "completed");
            if (evo_exec(db, "BEGIN IMMEDIATE;") != CBM_STORE_OK)
                return CBM_STORE_ERR;
            char now[40];
            evo_timestamp(time(NULL), now);
            rc = evo_store_checkpoint(db, input, payload_hash, &next, "completed", now,
                                      next.attempt, next.attempt);
            if (rc == CBM_STORE_OK)
                rc = evo_checkpoint_event(db, input, payload_hash, &next, "completed", "completed");
            cbm_evolution_result_t prepared = {0};
            char *report = NULL;
            if (rc == CBM_STORE_OK) {
                evo_result_from_checkpoint(&prepared, &next);
                prepared.eligible = out->eligible;
                prepared.wrote = 1;
                prepared.evolution_events = evo_maintenance_event_count(db, input->run_id);
                if (prepared.evolution_events < 0)
                    rc = CBM_STORE_ERR;
            }
            if (rc == CBM_STORE_OK && input->failure_before_report)
                rc = CBM_STORE_ERR;
            if (rc == CBM_STORE_OK) {
                report = evo_maintenance_report("applied", input->mode, &prepared, "completed",
                                                "single_controller_atomic_phase_pipeline");
                if (!report)
                    rc = CBM_STORE_ERR;
            }
            if (rc == CBM_STORE_OK) {
                rc = evo_exec(db, "COMMIT;");
                if (rc != CBM_STORE_OK)
                    evo_exec(db, "ROLLBACK;");
            } else {
                evo_exec(db, "ROLLBACK;");
            }
            if (rc != CBM_STORE_OK) {
                free(report);
                return rc;
            }
            *out = prepared;
            out->report_json = report;
            return CBM_STORE_OK;
        }

        return evo_pause_maintenance(db, input, payload_hash, &checkpoint, out,
                                     "invalid_checkpoint_phase", CBM_STORE_ERR);
    }

    return CBM_STORE_ERR;
}

int cbm_evolution_maintenance_store(cbm_store_t *store,
                                    const cbm_evolution_maintenance_input_t *input,
                                    cbm_evolution_result_t *out) {
    return evo_maintenance_controller(store, input, out);
}

int cbm_evolution_maintenance(cbm_global_memory_t *global,
                              const cbm_evolution_maintenance_input_t *input,
                              cbm_evolution_result_t *out) {
    return global ? cbm_evolution_maintenance_store(cbm_global_memory_store(global), input, out)
                  : CBM_STORE_ERR;
}

void cbm_evolution_result_free(cbm_evolution_result_t *out) {
    if (out) {
        free(out->report_json);
        memset(out, 0, sizeof(*out));
    }
}
