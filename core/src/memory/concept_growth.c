#include "memory/concept_growth.h"

#include "foundation/platform.h"
#include "foundation/compat.h"
#include "store/store.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STAGE10_COMPONENT "stage10_concept_growth"
#define STAGE10_SCHEMA_NAME "concept-growth-v1"
#define STAGE10_STORE "project-memory"
#define STAGE10_OBJECT_COUNT 46
#define STAGE10_GENESIS "GENESIS"
#define STAGE10_FIXTURE_PREFIX "stage10-fixture-"
#define STAGE14_FIXTURE_PREFIX "stage14-fixture-"
#define STAGE10_PRODUCTION_PROJECT "H-Codex_H-neuroplastic-main"
#define STAGE10_MANIFEST_SCHEMA "stage10-production-canary-manifest/v1"

static const char STAGE10_SCHEMA_SQL[] =
    "CREATE TABLE IF NOT EXISTS stage10_component_ledger("
    "component TEXT PRIMARY KEY,schema_version INTEGER NOT NULL,schema_name TEXT NOT NULL,"
    "schema_sha256 TEXT NOT NULL,policy_sha256 TEXT NOT NULL,algorithm_version TEXT NOT NULL,"
    "generator_version TEXT NOT NULL,installed_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS concept_growth_run("
    "run_id TEXT PRIMARY KEY,idempotency_key TEXT NOT NULL UNIQUE,request_sha256 TEXT NOT NULL,"
    "scope_project TEXT NOT NULL,scope_store TEXT NOT NULL,mode TEXT NOT NULL CHECK(mode IN "
    "('dry_run','active')),algorithm_version TEXT NOT NULL,policy_version INTEGER NOT NULL,"
    "config_version INTEGER NOT NULL,generator_version TEXT NOT NULL,policy_sha256 TEXT NOT NULL,"
    "schema_sha256 TEXT NOT NULL,manifest_sha256 TEXT,decision_set_sha256 TEXT NOT NULL,"
    "eligible_count INTEGER NOT NULL CHECK(eligible_count>=0),proposed_count INTEGER NOT NULL "
    "CHECK(proposed_count>=0),status TEXT NOT NULL CHECK(status IN ('completed','replayed')),"
    "created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS concept_candidate("
    "candidate_id TEXT PRIMARY KEY,identity_sha256 TEXT NOT NULL,scope_project TEXT NOT NULL,"
    "scope_store TEXT NOT NULL,initial_state TEXT NOT NULL CHECK(initial_state='proposed'),"
    "created_run_id TEXT NOT NULL REFERENCES concept_growth_run(run_id) ON DELETE RESTRICT,"
    "created_at TEXT NOT NULL,UNIQUE(scope_project,scope_store,identity_sha256));"
    "CREATE TABLE IF NOT EXISTS concept_candidate_version("
    "candidate_id TEXT NOT NULL REFERENCES concept_candidate(candidate_id) ON DELETE RESTRICT,"
    "version INTEGER NOT NULL CHECK(version>0),classification TEXT NOT NULL CHECK(classification "
    "IN ('episodic','semantic','rule')),classification_reason_code TEXT NOT NULL,"
    "confidence_ppm INTEGER NOT NULL CHECK(confidence_ppm BETWEEN 0 AND 1000000),"
    "content_text TEXT NOT NULL,content_sha256 TEXT NOT NULL,scope_json TEXT NOT NULL,"
    "policy_version INTEGER NOT NULL,policy_sha256 TEXT NOT NULL,generator_version TEXT NOT NULL,"
    "previous_version INTEGER,created_by_review_event_id TEXT,created_at TEXT NOT NULL,"
    "PRIMARY KEY(candidate_id,version));"
    "CREATE TABLE IF NOT EXISTS concept_candidate_source("
    "candidate_id TEXT NOT NULL REFERENCES concept_candidate(candidate_id) ON DELETE RESTRICT,"
    "source_item_id TEXT NOT NULL REFERENCES memory_item(id) ON DELETE RESTRICT,"
    "source_content_sha256 TEXT NOT NULL,independent_source_key TEXT NOT NULL,"
    "evidence_reason_code TEXT NOT NULL,shared_activation_id TEXT,shared_success_id TEXT,"
    "scope_project TEXT NOT NULL,scope_store TEXT NOT NULL,scope_product TEXT NOT NULL,"
    "scope_api TEXT NOT NULL,scope_version TEXT NOT NULL,contradiction_flag INTEGER NOT NULL "
    "CHECK(contradiction_flag IN (0,1)),created_at TEXT NOT NULL,"
    "PRIMARY KEY(candidate_id,source_item_id));"
    "CREATE TABLE IF NOT EXISTS concept_candidate_relation("
    "relation_id TEXT PRIMARY KEY,candidate_id TEXT NOT NULL REFERENCES concept_candidate("
    "candidate_id) ON DELETE RESTRICT,src_ref TEXT NOT NULL,dst_ref TEXT NOT NULL,"
    "relation_type TEXT NOT NULL CHECK(relation_type IN ('GENERALIZES','SPECIALIZES')),"
    "scope_project TEXT NOT NULL,scope_store TEXT NOT NULL,relation_sha256 TEXT NOT NULL,"
    "created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS concept_review_event("
    "sequence_no INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT NOT NULL UNIQUE,"
    "candidate_id TEXT NOT NULL REFERENCES concept_candidate(candidate_id) ON DELETE RESTRICT,"
    "idempotency_key TEXT NOT NULL UNIQUE,request_sha256 TEXT NOT NULL,action TEXT NOT NULL CHECK("
    "action IN ('approve','edit','reject','withdraw')),reviewer_source TEXT NOT NULL CHECK("
    "reviewer_source IN ('explicit_user','fixture')),from_version INTEGER,to_version INTEGER,"
    "prev_hash TEXT NOT NULL,event_hash TEXT NOT NULL UNIQUE,created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS concept_node("
    "node_id TEXT PRIMARY KEY,candidate_id TEXT NOT NULL UNIQUE REFERENCES concept_candidate("
    "candidate_id) ON DELETE RESTRICT,scope_project TEXT NOT NULL,scope_store TEXT NOT NULL,"
    "created_review_event_id TEXT NOT NULL REFERENCES concept_review_event(event_id) ON DELETE "
    "RESTRICT,created_at TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS concept_node_version("
    "node_id TEXT NOT NULL REFERENCES concept_node(node_id) ON DELETE RESTRICT,version INTEGER "
    "NOT NULL CHECK(version>0),candidate_version INTEGER NOT NULL,content_text TEXT NOT NULL,"
    "content_sha256 TEXT NOT NULL,scope_json TEXT NOT NULL,created_review_event_id TEXT NOT NULL "
    "REFERENCES concept_review_event(event_id) ON DELETE RESTRICT,created_at TEXT NOT NULL,"
    "PRIMARY KEY(node_id,version));"
    "CREATE TABLE IF NOT EXISTS concept_relation("
    "relation_id TEXT PRIMARY KEY,src_node_id TEXT NOT NULL REFERENCES concept_node(node_id) ON "
    "DELETE RESTRICT,dst_node_id TEXT NOT NULL REFERENCES concept_node(node_id) ON DELETE RESTRICT,"
    "relation_type TEXT NOT NULL CHECK(relation_type IN ('GENERALIZES','SPECIALIZES')),"
    "scope_project TEXT NOT NULL,scope_store TEXT NOT NULL,created_review_event_id TEXT NOT NULL "
    "REFERENCES concept_review_event(event_id) ON DELETE RESTRICT,relation_sha256 TEXT NOT NULL,"
    "created_at TEXT NOT NULL,UNIQUE(src_node_id,dst_node_id,relation_type));"
    "CREATE TABLE IF NOT EXISTS concept_growth_audit_event("
    "sequence_no INTEGER PRIMARY KEY AUTOINCREMENT,event_id TEXT NOT NULL UNIQUE,run_id TEXT "
    "REFERENCES concept_growth_run(run_id) ON DELETE RESTRICT,candidate_id TEXT REFERENCES "
    "concept_candidate(candidate_id) ON DELETE RESTRICT,operation TEXT NOT NULL,payload_sha256 "
    "TEXT NOT NULL,prev_hash TEXT NOT NULL,event_hash TEXT NOT NULL UNIQUE,created_at TEXT NOT "
    "NULL);"
    "CREATE INDEX IF NOT EXISTS concept_growth_run_scope_idx ON concept_growth_run("
    "scope_project,scope_store,created_at);"
    "CREATE INDEX IF NOT EXISTS concept_candidate_scope_idx ON concept_candidate("
    "scope_project,scope_store,created_at);"
    "CREATE INDEX IF NOT EXISTS concept_candidate_source_scope_idx ON concept_candidate_source("
    "scope_project,scope_store,source_item_id);"
    "CREATE INDEX IF NOT EXISTS concept_candidate_relation_candidate_idx ON "
    "concept_candidate_relation(candidate_id,relation_type);"
    "CREATE INDEX IF NOT EXISTS concept_review_event_candidate_idx ON concept_review_event("
    "candidate_id,sequence_no);"
    "CREATE INDEX IF NOT EXISTS concept_node_scope_idx ON concept_node("
    "scope_project,scope_store,created_at);"
    "CREATE INDEX IF NOT EXISTS concept_relation_src_idx ON concept_relation("
    "src_node_id,relation_type);"
    "CREATE INDEX IF NOT EXISTS concept_relation_dst_idx ON concept_relation("
    "dst_node_id,relation_type);"
    "CREATE INDEX IF NOT EXISTS concept_growth_audit_candidate_idx ON concept_growth_audit_event("
    "candidate_id,sequence_no);"
    "CREATE TRIGGER IF NOT EXISTS stage10_component_ledger_no_update BEFORE UPDATE ON "
    "stage10_component_ledger BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS stage10_component_ledger_no_delete BEFORE DELETE ON "
    "stage10_component_ledger BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_growth_run_no_update BEFORE UPDATE ON concept_growth_run "
    "BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_growth_run_no_delete BEFORE DELETE ON concept_growth_run "
    "BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_no_update BEFORE UPDATE ON concept_candidate "
    "BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_no_delete BEFORE DELETE ON concept_candidate "
    "BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_version_no_update BEFORE UPDATE ON "
    "concept_candidate_version BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_version_no_delete BEFORE DELETE ON "
    "concept_candidate_version BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_source_no_update BEFORE UPDATE ON "
    "concept_candidate_source BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_source_no_delete BEFORE DELETE ON "
    "concept_candidate_source BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_relation_no_update BEFORE UPDATE ON "
    "concept_candidate_relation BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_candidate_relation_no_delete BEFORE DELETE ON "
    "concept_candidate_relation BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_review_event_no_update BEFORE UPDATE ON "
    "concept_review_event BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_review_event_no_delete BEFORE DELETE ON "
    "concept_review_event BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_node_no_update BEFORE UPDATE ON concept_node BEGIN "
    "SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_node_no_delete BEFORE DELETE ON concept_node BEGIN "
    "SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_node_version_no_update BEFORE UPDATE ON "
    "concept_node_version BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_node_version_no_delete BEFORE DELETE ON "
    "concept_node_version BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_relation_no_update BEFORE UPDATE ON concept_relation "
    "BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_relation_no_delete BEFORE DELETE ON concept_relation "
    "BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_growth_audit_event_no_update BEFORE UPDATE ON "
    "concept_growth_audit_event BEGIN SELECT RAISE(ABORT,'STAGE10_APPEND_ONLY'); END;"
    "CREATE TRIGGER IF NOT EXISTS concept_growth_audit_event_no_delete BEFORE DELETE ON "
    "concept_growth_audit_event BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS stage10_memory_item_no_delete BEFORE DELETE ON memory_item BEGIN "
    "SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS stage10_memory_event_no_delete BEFORE DELETE ON memory_event "
    "BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS stage10_memory_edge_no_delete BEFORE DELETE ON memory_edge BEGIN "
    "SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;"
    "CREATE TRIGGER IF NOT EXISTS stage10_memory_evidence_no_delete BEFORE DELETE ON "
    "memory_evidence BEGIN SELECT RAISE(ABORT,'HARD_DELETE_BLOCKED'); END;";

typedef struct {
    char *item_id;
    char *content;
    char *content_sha256;
    char *source_key;
    char *kind;
    int confidence_ppm;
} stage10_source_t;

typedef struct {
    char *entity_key;
    char *classification;
    char identity_sha256[65];
    char candidate_id[35];
    char decision_sha256[65];
    char content_text[96];
    char content_sha256[65];
    stage10_source_t *sources;
    int source_count;
    int source_capacity;
    int confidence_ppm;
} stage10_candidate_t;

typedef struct {
    stage10_candidate_t *items;
    int count;
    int capacity;
    char decision_set_sha256[65];
} stage10_candidates_t;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} stage10_buffer_t;

static bool stage10_hash(const void *data, size_t size, char out[65]) {
    return cbm_stage7_sha256_hex(data, size, out) == CBM_STORE_OK;
}

static char *stage10_dup_column(sqlite3_stmt *stmt, int column) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return cbm_strdup(value ? (const char *)value : "");
}

static bool stage10_buffer_append(stage10_buffer_t *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    size_t required = buffer->length + (size_t)needed + 1;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 256;
        while (capacity < required)
            capacity *= 2;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) {
            va_end(args);
            return false;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    vsnprintf(buffer->data + buffer->length, buffer->capacity - buffer->length, format, args);
    va_end(args);
    buffer->length += (size_t)needed;
    return true;
}

static bool stage10_table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *stmt = NULL;
    bool exists = false;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    }
    sqlite3_finalize(stmt);
    return exists;
}

int cbm_store_memory_stage10_object_count(cbm_store_t *store) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (!db ||
        sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM sqlite_master WHERE name LIKE 'concept_%' OR "
                           "name LIKE 'stage10_%';",
                           -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static bool stage10_ledger_matches(sqlite3 *db) {
    if (!stage10_table_exists(db, "stage10_component_ledger"))
        return false;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT schema_version,schema_name,schema_sha256,policy_sha256,algorithm_version,"
        "generator_version FROM stage10_component_ledger WHERE component=?1;";
    bool matches = false;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, STAGE10_COMPONENT, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 1);
            const char *schema = (const char *)sqlite3_column_text(stmt, 2);
            const char *policy = (const char *)sqlite3_column_text(stmt, 3);
            const char *algorithm = (const char *)sqlite3_column_text(stmt, 4);
            const char *generator = (const char *)sqlite3_column_text(stmt, 5);
            matches = sqlite3_column_int(stmt, 0) == 1 && name && schema && policy && algorithm &&
                      generator && strcmp(name, STAGE10_SCHEMA_NAME) == 0 &&
                      strcmp(schema, CBM_STAGE10_MIGRATION_SHA256) == 0 &&
                      strcmp(policy, CBM_STAGE10_POLICY_SHA256) == 0 &&
                      strcmp(algorithm, CBM_STAGE10_ALGORITHM_VERSION) == 0 &&
                      strcmp(generator, CBM_STAGE10_GENERATOR_VERSION) == 0;
        }
    }
    sqlite3_finalize(stmt);
    return matches;
}

int cbm_store_memory_stage10_migrate(cbm_store_t *store) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db)
        return CBM_STORE_ERR;
    int before = cbm_store_memory_stage10_object_count(store);
    if (before > 0) {
        return before == STAGE10_OBJECT_COUNT && stage10_ledger_matches(db)
                   ? CBM_STORE_REPLAYED
                   : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_exec(db, STAGE10_SCHEMA_SQL, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return CBM_STORE_ERR;
    }
    char failpoint[16] = {0};
    cbm_safe_getenv("CBM_STAGE10_MIGRATION_FAIL_AFTER", failpoint, sizeof(failpoint), NULL);
    if (strcmp(failpoint, "schema") == 0) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return CBM_STORE_ERR;
    }
    sqlite3_stmt *stmt = NULL;
    const char *insert =
        "INSERT INTO stage10_component_ledger(component,schema_version,schema_name,schema_sha256,"
        "policy_sha256,algorithm_version,generator_version,installed_at) VALUES(?1,1,?2,?3,?4,"
        "?5,?6,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    bool ok = sqlite3_prepare_v2(db, insert, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(stmt, 1, STAGE10_COMPONENT, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, STAGE10_SCHEMA_NAME, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, CBM_STAGE10_MIGRATION_SHA256, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, CBM_STAGE10_POLICY_SHA256, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, CBM_STAGE10_ALGORITHM_VERSION, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, CBM_STAGE10_GENERATOR_VERSION, -1, SQLITE_STATIC);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!ok || cbm_store_memory_stage10_object_count(store) != STAGE10_OBJECT_COUNT ||
        !stage10_ledger_matches(db) || strcmp(failpoint, "ledger") == 0 ||
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static void stage10_candidates_free(stage10_candidates_t *set) {
    if (!set)
        return;
    for (int i = 0; i < set->count; i++) {
        stage10_candidate_t *candidate = &set->items[i];
        free(candidate->entity_key);
        free(candidate->classification);
        for (int j = 0; j < candidate->source_count; j++) {
            free(candidate->sources[j].item_id);
            free(candidate->sources[j].content);
            free(candidate->sources[j].content_sha256);
            free(candidate->sources[j].source_key);
            free(candidate->sources[j].kind);
        }
        free(candidate->sources);
    }
    free(set->items);
    memset(set, 0, sizeof(*set));
}

static stage10_candidate_t *stage10_candidate_get(stage10_candidates_t *set,
                                                  const char *entity_key) {
    if (set->count > 0 && strcmp(set->items[set->count - 1].entity_key, entity_key) == 0)
        return &set->items[set->count - 1];
    if (set->count == set->capacity) {
        int capacity = set->capacity ? set->capacity * 2 : 8;
        stage10_candidate_t *grown = realloc(set->items, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return NULL;
        memset(grown + set->capacity, 0, (size_t)(capacity - set->capacity) * sizeof(*grown));
        set->items = grown;
        set->capacity = capacity;
    }
    stage10_candidate_t *candidate = &set->items[set->count++];
    candidate->entity_key = cbm_strdup(entity_key);
    candidate->confidence_ppm = 1000000;
    return candidate->entity_key ? candidate : NULL;
}

static bool stage10_candidate_add_source(stage10_candidate_t *candidate, sqlite3_stmt *stmt) {
    if (candidate->source_count == candidate->source_capacity) {
        int capacity = candidate->source_capacity ? candidate->source_capacity * 2 : 4;
        stage10_source_t *grown = realloc(candidate->sources, (size_t)capacity * sizeof(*grown));
        if (!grown)
            return false;
        memset(grown + candidate->source_capacity, 0,
               (size_t)(capacity - candidate->source_capacity) * sizeof(*grown));
        candidate->sources = grown;
        candidate->source_capacity = capacity;
    }
    stage10_source_t *source = &candidate->sources[candidate->source_count++];
    source->item_id = stage10_dup_column(stmt, 1);
    source->kind = stage10_dup_column(stmt, 2);
    source->content = stage10_dup_column(stmt, 3);
    source->source_key = stage10_dup_column(stmt, 4);
    source->confidence_ppm = sqlite3_column_int(stmt, 5);
    source->content_sha256 = malloc(65);
    if (!source->item_id || !source->kind || !source->content || !source->source_key ||
        !source->content_sha256 ||
        !stage10_hash(source->content, strlen(source->content), source->content_sha256))
        return false;
    if (source->confidence_ppm < candidate->confidence_ppm)
        candidate->confidence_ppm = source->confidence_ppm;
    return true;
}

static char *stage10_normalize(const char *text) {
    size_t length = text ? strlen(text) : 0;
    char *out = malloc(length + 1);
    if (!out)
        return NULL;
    size_t used = 0;
    bool pending_space = false;
    for (size_t i = 0; i < length; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (isspace(ch)) {
            pending_space = used > 0;
            continue;
        }
        if (pending_space)
            out[used++] = ' ';
        pending_space = false;
        out[used++] = ch < 128 ? (char)tolower(ch) : (char)ch;
    }
    out[used] = '\0';
    return out;
}

static int stage10_tokenize(const char *text, char tokens[64][64]) {
    int count = 0;
    size_t used = 0;
    char token[64] = {0};
    for (const unsigned char *cursor = (const unsigned char *)(text ? text : "");; cursor++) {
        unsigned char ch = *cursor;
        bool token_char = isalnum(ch) || ch == '_' || ch == '.' || ch == '-';
        if (token_char && used + 1 < sizeof(token)) {
            token[used++] = ch < 128 ? (char)tolower(ch) : (char)ch;
        }
        if ((!token_char || ch == '\0') && used > 0) {
            token[used] = '\0';
            bool seen = false;
            for (int i = 0; i < count; i++) {
                if (strcmp(tokens[i], token) == 0)
                    seen = true;
            }
            if (!seen && count < 64) {
                memcpy(tokens[count++], token, used + 1);
            }
            used = 0;
        }
        if (ch == '\0')
            break;
    }
    return count;
}

static int stage10_similarity_ppm(const char *left, const char *right) {
    char left_tokens[64][64] = {{0}};
    char right_tokens[64][64] = {{0}};
    int left_count = stage10_tokenize(left, left_tokens);
    int right_count = stage10_tokenize(right, right_tokens);
    int intersection = 0;
    for (int i = 0; i < left_count; i++) {
        for (int j = 0; j < right_count; j++) {
            if (strcmp(left_tokens[i], right_tokens[j]) == 0) {
                intersection++;
                break;
            }
        }
    }
    int total = left_count + right_count - intersection;
    return total == 0 ? 1000000 : intersection * 1000000 / total;
}

static bool stage10_contains_prompt_control(const char *content) {
    char *normalized = stage10_normalize(content);
    if (!normalized)
        return true;
    const char *needles[] = {"ignore system",          "ignore previous",      "system prompt",
                             "approve this candidate", "regardless of policy", "override policy",
                             "developer message"};
    bool found = false;
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        if (strstr(normalized, needles[i])) {
            found = true;
            break;
        }
    }
    free(normalized);
    return found;
}

static bool stage10_candidate_is_eligible(stage10_candidate_t *candidate) {
    if (!candidate || candidate->source_count < 2)
        return false;
    bool all_rule = true;
    for (int i = 0; i < candidate->source_count; i++) {
        stage10_source_t *source = &candidate->sources[i];
        if (strcmp(source->kind, "constraint") != 0 && strcmp(source->kind, "decision") != 0)
            all_rule = false;
        if (!source->source_key[0] || stage10_contains_prompt_control(source->content))
            return false;
        for (int j = 0; j < i; j++) {
            stage10_source_t *prior = &candidate->sources[j];
            if (strcmp(source->item_id, prior->item_id) == 0 ||
                strcmp(source->source_key, prior->source_key) == 0 ||
                strcmp(source->content_sha256, prior->content_sha256) == 0)
                return false;
            char *normalized = stage10_normalize(source->content);
            char *prior_normalized = stage10_normalize(prior->content);
            bool duplicate = !normalized || !prior_normalized ||
                             strcmp(normalized, prior_normalized) == 0 ||
                             stage10_similarity_ppm(source->content, prior->content) >= 800000;
            free(normalized);
            free(prior_normalized);
            if (duplicate)
                return false;
        }
    }
    candidate->classification = cbm_strdup(all_rule ? "rule" : "semantic");
    int minimum_sources = all_rule ? 3 : 2;
    int minimum_confidence = all_rule ? 850000 : 800000;
    return candidate->classification && candidate->source_count >= minimum_sources &&
           candidate->confidence_ppm >= minimum_confidence;
}

static void stage10_candidate_release(stage10_candidate_t *candidate) {
    if (!candidate)
        return;
    free(candidate->entity_key);
    free(candidate->classification);
    for (int i = 0; i < candidate->source_count; i++) {
        free(candidate->sources[i].item_id);
        free(candidate->sources[i].content);
        free(candidate->sources[i].content_sha256);
        free(candidate->sources[i].source_key);
        free(candidate->sources[i].kind);
    }
    free(candidate->sources);
    memset(candidate, 0, sizeof(*candidate));
}

static bool stage10_candidate_finalize(stage10_candidate_t *candidate, const char *project) {
    if (!stage10_candidate_is_eligible(candidate))
        return false;
    stage10_buffer_t identity = {0};
    bool ok = stage10_buffer_append(&identity, "%s|%s|%s|%s|%s", project, STAGE10_STORE,
                                    candidate->classification, candidate->entity_key,
                                    CBM_STAGE10_POLICY_SHA256);
    for (int i = 0; ok && i < candidate->source_count; i++) {
        ok = stage10_buffer_append(&identity, "|%s:%s", candidate->sources[i].item_id,
                                   candidate->sources[i].content_sha256);
    }
    ok = ok && stage10_hash(identity.data, identity.length, candidate->identity_sha256);
    free(identity.data);
    if (!ok)
        return false;
    snprintf(candidate->candidate_id, sizeof(candidate->candidate_id), "candidate-%.24s",
             candidate->identity_sha256);
    snprintf(candidate->content_text, sizeof(candidate->content_text), "%s candidate %.16s",
             strcmp(candidate->classification, "rule") == 0 ? "Rule" : "Semantic concept",
             candidate->identity_sha256);
    if (!stage10_hash(candidate->content_text, strlen(candidate->content_text),
                      candidate->content_sha256))
        return false;
    stage10_buffer_t decision = {0};
    ok = stage10_buffer_append(&decision, "%s|%s|%s|%d|%s", candidate->candidate_id,
                               candidate->identity_sha256, candidate->classification,
                               candidate->confidence_ppm, candidate->content_sha256);
    for (int i = 0; ok && i < candidate->source_count; i++) {
        ok = stage10_buffer_append(&decision, "|%s:%s", candidate->sources[i].item_id,
                                   candidate->sources[i].content_sha256);
    }
    ok = ok && stage10_hash(decision.data, decision.length, candidate->decision_sha256);
    free(decision.data);
    return ok;
}

static int stage10_collect_candidates(sqlite3 *db, const char *project, stage10_candidates_t *out) {
    const char *required[] = {"memory_item", "feedback_attribution", "feedback_event",
                              "memory_evidence"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!stage10_table_exists(db, required[i]))
            return CBM_STORE_ERR;
    }
    const char *sql =
        "SELECT m.entity_key,m.id,m.kind,m.content,m.source_event_ids,"
        "CAST(m.confidence*1000000 AS INTEGER) FROM memory_item m "
        "WHERE m.scope_project=?1 AND m.deleted_at IS NULL AND m.status='active' "
        "AND m.entity_key IS NOT NULL AND m.entity_key<>'' AND m.supersedes IS NULL "
        "AND NOT EXISTS(SELECT 1 FROM memory_edge ce WHERE ce.type='contradicts' AND "
        "(ce.src_id=m.id OR ce.dst_id=m.id)) "
        "AND EXISTS(SELECT 1 FROM feedback_attribution fa "
        "JOIN feedback_event fe ON fe.event_id=fa.feedback_event_id "
        "JOIN memory_evidence ev ON ev.evidence_id=fa.evidence_id "
        "WHERE fa.memory_item_id=m.id AND fa.attribution_status='attributed' "
        "AND fe.action='confirm' AND ev.evidence_state='valid') "
        "AND NOT EXISTS(SELECT 1 FROM feedback_attribution nfa JOIN feedback_event nfe "
        "ON nfe.event_id=nfa.feedback_event_id WHERE nfa.memory_item_id=m.id "
        "AND nfe.action IN ('reject','correct','withdraw')) ORDER BY m.entity_key,m.id;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    int rc = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *entity = (const char *)sqlite3_column_text(stmt, 0);
        stage10_candidate_t *candidate = stage10_candidate_get(out, entity ? entity : "");
        if (!candidate || !stage10_candidate_add_source(candidate, stmt)) {
            rc = CBM_STORE_ERR;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK)
        return rc;
    int kept = 0;
    for (int i = 0; i < out->count; i++) {
        if (stage10_candidate_finalize(&out->items[i], project)) {
            if (kept != i) {
                out->items[kept] = out->items[i];
                memset(&out->items[i], 0, sizeof(out->items[i]));
            }
            kept++;
        } else {
            stage10_candidate_release(&out->items[i]);
        }
    }
    out->count = kept;
    stage10_buffer_t decisions = {0};
    for (int i = 0; i < out->count; i++) {
        if (!stage10_buffer_append(&decisions, "%s", out->items[i].decision_sha256)) {
            free(decisions.data);
            return CBM_STORE_ERR;
        }
    }
    bool hashed = stage10_hash(decisions.data ? decisions.data : "", decisions.length,
                               out->decision_set_sha256);
    free(decisions.data);
    return hashed ? CBM_STORE_OK : CBM_STORE_ERR;
}

static bool stage10_contract_valid(const cbm_concept_generate_input_t *input) {
    return input && input->project && input->store && input->operation && input->mode &&
           input->algorithm_version && input->policy_sha256 && input->generator_version &&
           strcmp(input->store, STAGE10_STORE) == 0 &&
           strcmp(input->algorithm_version, CBM_STAGE10_ALGORITHM_VERSION) == 0 &&
           strcmp(input->policy_sha256, CBM_STAGE10_POLICY_SHA256) == 0 &&
           strcmp(input->generator_version, CBM_STAGE10_GENERATOR_VERSION) == 0 &&
           input->policy_version == CBM_STAGE10_POLICY_VERSION &&
           input->config_version == CBM_STAGE10_CONFIG_VERSION &&
           (strcmp(input->operation, "evaluate") == 0 ||
            strcmp(input->operation, "generate") == 0) &&
           (strcmp(input->mode, "off") == 0 || strcmp(input->mode, "shadow") == 0 ||
            strcmp(input->mode, "dry_run") == 0 || strcmp(input->mode, "active") == 0);
}

static bool stage10_request_hash(const cbm_concept_generate_input_t *input, char out[65]) {
    stage10_buffer_t request = {0};
    bool ok = stage10_buffer_append(
                  &request, "%s|%s|%s|%s|%s|%s|%d|%d|%s", input->operation, input->project,
                  input->store, input->run_id ? input->run_id : "",
                  input->idempotency_key ? input->idempotency_key : "", input->algorithm_version,
                  input->policy_version, input->config_version, input->generator_version) &&
              stage10_hash(request.data, request.length, out);
    free(request.data);
    return ok;
}

static char *stage10_report_json(const cbm_concept_generate_input_t *input,
                                 const stage10_candidates_t *candidates, const char *request_sha256,
                                 const char *status, bool state_written) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage10-concept-generation-report/v1");
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "store", input->store);
    yyjson_mut_obj_add_str(doc, root, "operation", input->operation);
    yyjson_mut_obj_add_str(doc, root, "mode", input->mode);
    yyjson_mut_obj_add_str(doc, root, "run_id", input->run_id ? input->run_id : "");
    yyjson_mut_obj_add_str(doc, root, "request_sha256", request_sha256);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", CBM_STAGE10_ALGORITHM_VERSION);
    yyjson_mut_obj_add_int(doc, root, "policy_version", CBM_STAGE10_POLICY_VERSION);
    yyjson_mut_obj_add_int(doc, root, "config_version", CBM_STAGE10_CONFIG_VERSION);
    yyjson_mut_obj_add_str(doc, root, "generator_version", CBM_STAGE10_GENERATOR_VERSION);
    yyjson_mut_obj_add_str(doc, root, "policy_sha256", CBM_STAGE10_POLICY_SHA256);
    yyjson_mut_obj_add_str(doc, root, "schema_sha256", CBM_STAGE10_MIGRATION_SHA256);
    yyjson_mut_obj_add_str(doc, root, "decision_set_sha256", candidates->decision_set_sha256);
    yyjson_mut_obj_add_int(doc, root, "eligible_count", candidates->count);
    yyjson_mut_obj_add_int(doc, root, "proposed_count", candidates->count);
    yyjson_mut_obj_add_bool(doc, root, "production_state_written", state_written);
    yyjson_mut_obj_add_bool(doc, root, "untrusted_data", true);
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int i = 0; i < candidates->count; i++) {
        const stage10_candidate_t *candidate = &candidates->items[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "candidate_id", candidate->candidate_id);
        yyjson_mut_obj_add_str(doc, item, "identity_sha256", candidate->identity_sha256);
        yyjson_mut_obj_add_str(doc, item, "decision_sha256", candidate->decision_sha256);
        yyjson_mut_obj_add_str(doc, item, "classification", candidate->classification);
        yyjson_mut_obj_add_str(doc, item, "state", "proposed");
        yyjson_mut_obj_add_int(doc, item, "confidence_ppm", candidate->confidence_ppm);
        yyjson_mut_obj_add_str(doc, item, "content_sha256", candidate->content_sha256);
        yyjson_mut_val *source_ids = yyjson_mut_arr(doc);
        yyjson_mut_val *source_hashes = yyjson_mut_arr(doc);
        for (int j = 0; j < candidate->source_count; j++) {
            yyjson_mut_arr_add_str(doc, source_ids, candidate->sources[j].item_id);
            yyjson_mut_arr_add_str(doc, source_hashes, candidate->sources[j].content_sha256);
        }
        yyjson_mut_obj_add_val(doc, item, "source_ids", source_ids);
        yyjson_mut_obj_add_val(doc, item, "source_content_sha256", source_hashes);
        yyjson_mut_arr_add_val(array, item);
    }
    yyjson_mut_obj_add_val(doc, root, "candidates", array);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static bool stage10_read_file(const char *path, char **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    FILE *handle = fopen(path, "rb");
    if (!handle)
        return false;
    if (fseek(handle, 0, SEEK_END) != 0) {
        fclose(handle);
        return false;
    }
    long size = ftell(handle);
    if (size < 0 || size > 4 * 1024 * 1024 || fseek(handle, 0, SEEK_SET) != 0) {
        fclose(handle);
        return false;
    }
    char *data = malloc((size_t)size + 1);
    bool ok = data && fread(data, 1, (size_t)size, handle) == (size_t)size;
    fclose(handle);
    if (!ok) {
        free(data);
        return false;
    }
    data[size] = '\0';
    *out = data;
    *out_size = (size_t)size;
    return true;
}

static bool stage10_active_guard(const cbm_concept_generate_input_t *input,
                                 bool stage14_parent_authorized) {
    char enabled[16] = {0};
    if (stage14_parent_authorized)
        return input->manifest_path && input->manifest_sha256;
    bool stage10_fixture =
        strncmp(input->project, STAGE10_FIXTURE_PREFIX, strlen(STAGE10_FIXTURE_PREFIX)) == 0;
    bool stage14_fixture =
        strncmp(input->project, STAGE14_FIXTURE_PREFIX, strlen(STAGE14_FIXTURE_PREFIX)) == 0;
    if (stage10_fixture || stage14_fixture) {
        cbm_safe_getenv(stage14_fixture ? "CBM_STAGE14_ACTIVE_FIXTURE"
                                        : "CBM_STAGE10_ACTIVE_FIXTURE",
                        enabled, sizeof(enabled), NULL);
        return strcmp(enabled, "1") == 0;
    }
    if (strcmp(input->project, STAGE10_PRODUCTION_PROJECT) != 0)
        return false;
    char path[1024] = {0};
    char hash[80] = {0};
    cbm_safe_getenv("CBM_STAGE10_PRODUCTION_CANARY", enabled, sizeof(enabled), NULL);
    cbm_safe_getenv("CBM_STAGE10_PRODUCTION_CANARY_MANIFEST", path, sizeof(path), NULL);
    cbm_safe_getenv("CBM_STAGE10_PRODUCTION_CANARY_SHA256", hash, sizeof(hash), NULL);
    return strcmp(enabled, "1") == 0 && input->manifest_path && input->manifest_sha256 &&
           strcmp(path, input->manifest_path) == 0 && strcmp(hash, input->manifest_sha256) == 0;
}

static bool stage10_json_string_equals(yyjson_val *object, const char *key, const char *expected) {
    yyjson_val *value = object ? yyjson_obj_get(object, key) : NULL;
    return value && yyjson_is_str(value) && strcmp(yyjson_get_str(value), expected) == 0;
}

static bool stage10_manifest_verify(const cbm_concept_generate_input_t *input,
                                    const stage10_candidates_t *candidates,
                                    const char *request_sha256) {
    if (!input->manifest_path || !input->manifest_sha256 || strlen(input->manifest_sha256) != 64)
        return false;
    char *data = NULL;
    size_t size = 0;
    char actual_hash[65] = {0};
    if (!stage10_read_file(input->manifest_path, &data, &size) ||
        !stage10_hash(data, size, actual_hash) ||
        strcmp(actual_hash, input->manifest_sha256) != 0) {
        free(data);
        return false;
    }
    yyjson_doc *doc = yyjson_read(data, size, 0);
    free(data);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    bool ok =
        root && yyjson_is_obj(root) &&
        stage10_json_string_equals(root, "schema", STAGE10_MANIFEST_SCHEMA) &&
        stage10_json_string_equals(root, "project", input->project) &&
        stage10_json_string_equals(root, "store", input->store) &&
        stage10_json_string_equals(root, "run_id", input->run_id) &&
        stage10_json_string_equals(root, "request_sha256", request_sha256) &&
        stage10_json_string_equals(root, "algorithm_version", CBM_STAGE10_ALGORITHM_VERSION) &&
        stage10_json_string_equals(root, "generator_version", CBM_STAGE10_GENERATOR_VERSION) &&
        stage10_json_string_equals(root, "policy_sha256", CBM_STAGE10_POLICY_SHA256) &&
        stage10_json_string_equals(root, "schema_sha256", CBM_STAGE10_MIGRATION_SHA256) &&
        stage10_json_string_equals(root, "decision_set_sha256", candidates->decision_set_sha256);
    yyjson_val *eligible = root ? yyjson_obj_get(root, "eligible_count") : NULL;
    yyjson_val *policy = root ? yyjson_obj_get(root, "policy_version") : NULL;
    yyjson_val *config = root ? yyjson_obj_get(root, "config_version") : NULL;
    yyjson_val *array = root ? yyjson_obj_get(root, "candidates") : NULL;
    ok = ok && eligible && yyjson_is_int(eligible) &&
         yyjson_get_sint(eligible) == candidates->count && policy && yyjson_is_int(policy) &&
         yyjson_get_sint(policy) == CBM_STAGE10_POLICY_VERSION && config && yyjson_is_int(config) &&
         yyjson_get_sint(config) == CBM_STAGE10_CONFIG_VERSION && array && yyjson_is_arr(array) &&
         yyjson_arr_size(array) == (size_t)candidates->count;
    if (ok) {
        yyjson_arr_iter iterator = yyjson_arr_iter_with(array);
        yyjson_val *entry = NULL;
        int index = 0;
        while ((entry = yyjson_arr_iter_next(&iterator)) && index < candidates->count) {
            stage10_candidate_t *candidate = &candidates->items[index++];
            if (!stage10_json_string_equals(entry, "candidate_id", candidate->candidate_id) ||
                !stage10_json_string_equals(entry, "identity_sha256", candidate->identity_sha256) ||
                !stage10_json_string_equals(entry, "decision_sha256", candidate->decision_sha256)) {
                ok = false;
                break;
            }
            yyjson_val *ids = yyjson_obj_get(entry, "source_ids");
            yyjson_val *hashes = yyjson_obj_get(entry, "source_content_sha256");
            if (!ids || !hashes || !yyjson_is_arr(ids) || !yyjson_is_arr(hashes) ||
                yyjson_arr_size(ids) != (size_t)candidate->source_count ||
                yyjson_arr_size(hashes) != (size_t)candidate->source_count) {
                ok = false;
                break;
            }
            for (int source = 0; source < candidate->source_count; source++) {
                yyjson_val *id = yyjson_arr_get(ids, (size_t)source);
                yyjson_val *hash = yyjson_arr_get(hashes, (size_t)source);
                if (!id || !hash || !yyjson_is_str(id) || !yyjson_is_str(hash) ||
                    strcmp(yyjson_get_str(id), candidate->sources[source].item_id) != 0 ||
                    strcmp(yyjson_get_str(hash), candidate->sources[source].content_sha256) != 0) {
                    ok = false;
                    break;
                }
            }
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

static bool stage10_audit_insert(sqlite3 *db, const char *run_id, const char *candidate_id,
                                 const char *operation, const char *payload_sha256) {
    sqlite3_stmt *stmt = NULL;
    char previous[65] = STAGE10_GENESIS;
    if (sqlite3_prepare_v2(db,
                           "SELECT event_hash FROM concept_growth_audit_event ORDER BY "
                           "sequence_no DESC LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        if (value)
            snprintf(previous, sizeof(previous), "%s", value);
    }
    sqlite3_finalize(stmt);
    stage10_buffer_t seed = {0};
    char event_seed[65] = {0};
    char event_id[80] = {0};
    char event_hash[65] = {0};
    bool ok = stage10_buffer_append(&seed, "%s|%s|%s|%s", run_id ? run_id : "",
                                    candidate_id ? candidate_id : "", operation, payload_sha256) &&
              stage10_hash(seed.data, seed.length, event_seed);
    free(seed.data);
    if (!ok)
        return false;
    snprintf(event_id, sizeof(event_id), "concept-audit-%.24s", event_seed);
    stage10_buffer_t event = {0};
    ok = stage10_buffer_append(&event, "%s|%s|%s|%s|%s|%s", event_id, run_id ? run_id : "",
                               candidate_id ? candidate_id : "", operation, payload_sha256,
                               previous) &&
         stage10_hash(event.data, event.length, event_hash);
    free(event.data);
    if (!ok ||
        sqlite3_prepare_v2(db,
                           "INSERT INTO concept_growth_audit_event(event_id,run_id,candidate_id,"
                           "operation,payload_sha256,prev_hash,event_hash,created_at) VALUES(?1,?2,"
                           "?3,?4,?5,?6,?7,strftime('%Y-%m-%dT%H:%M:%fZ','now'));",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, run_id, -1, SQLITE_TRANSIENT);
    if (candidate_id)
        sqlite3_bind_text(stmt, 3, candidate_id, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 3);
    sqlite3_bind_text(stmt, 4, operation, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, payload_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, previous, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, event_hash, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool stage10_insert_run(sqlite3 *db, const cbm_concept_generate_input_t *input,
                               const stage10_candidates_t *candidates, const char *request_sha256) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO concept_growth_run(run_id,idempotency_key,request_sha256,scope_project,"
        "scope_store,mode,algorithm_version,policy_version,config_version,generator_version,"
        "policy_sha256,schema_sha256,manifest_sha256,decision_set_sha256,eligible_count,"
        "proposed_count,status,created_at) VALUES(?1,?2,?3,?4,?5,'active',?6,?7,?8,?9,?10,"
        "?11,?12,?13,?14,?14,'completed',strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, input->run_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->idempotency_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, request_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, input->project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, input->store, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, CBM_STAGE10_ALGORITHM_VERSION, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 7, CBM_STAGE10_POLICY_VERSION);
    sqlite3_bind_int(stmt, 8, CBM_STAGE10_CONFIG_VERSION);
    sqlite3_bind_text(stmt, 9, CBM_STAGE10_GENERATOR_VERSION, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 10, CBM_STAGE10_POLICY_SHA256, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, CBM_STAGE10_MIGRATION_SHA256, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 12, input->manifest_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 13, candidates->decision_set_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 14, candidates->count);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool stage10_insert_candidate(sqlite3 *db, const cbm_concept_generate_input_t *input,
                                     const stage10_candidate_t *candidate) {
    sqlite3_stmt *stmt = NULL;
    const char *candidate_sql =
        "INSERT INTO concept_candidate(candidate_id,identity_sha256,scope_project,scope_store,"
        "initial_state,created_run_id,created_at) VALUES(?1,?2,?3,?4,'proposed',?5,"
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    if (sqlite3_prepare_v2(db, candidate_sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, candidate->candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, candidate->identity_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, input->store, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, input->run_id, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok)
        return false;
    char scope_json[512];
    snprintf(scope_json, sizeof(scope_json),
             "{\"api\":\"none\",\"product\":\"generic\",\"project\":\"%s\","
             "\"store\":\"%s\",\"version\":\"v1\"}",
             input->project, input->store);
    const char *version_sql =
        "INSERT INTO concept_candidate_version(candidate_id,version,classification,"
        "classification_reason_code,confidence_ppm,content_text,content_sha256,scope_json,"
        "policy_version,policy_sha256,generator_version,previous_version,"
        "created_by_review_event_id,created_at) VALUES(?1,1,?2,'ELIGIBLE_NOVEL_CANDIDATE',?3,"
        "?4,?5,?6,?7,?8,?9,NULL,NULL,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    if (sqlite3_prepare_v2(db, version_sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, candidate->candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, candidate->classification, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, candidate->confidence_ppm);
    sqlite3_bind_text(stmt, 4, candidate->content_text, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, candidate->content_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, scope_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, CBM_STAGE10_POLICY_VERSION);
    sqlite3_bind_text(stmt, 8, CBM_STAGE10_POLICY_SHA256, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 9, CBM_STAGE10_GENERATOR_VERSION, -1, SQLITE_STATIC);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok)
        return false;
    const char *source_sql =
        "INSERT INTO concept_candidate_source(candidate_id,source_item_id,source_content_sha256,"
        "independent_source_key,evidence_reason_code,shared_activation_id,shared_success_id,"
        "scope_project,scope_store,scope_product,scope_api,scope_version,contradiction_flag,"
        "created_at) VALUES(?1,?2,?3,?4,'SHARED_VERIFIED_SUCCESS',NULL,?5,?6,?7,'generic',"
        "'none','v1',0,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    for (int i = 0; i < candidate->source_count; i++) {
        const stage10_source_t *source = &candidate->sources[i];
        if (sqlite3_prepare_v2(db, source_sql, -1, &stmt, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, candidate->candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, source->item_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, source->content_sha256, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, source->source_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, candidate->entity_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, input->project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, input->store, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok)
            return false;
    }
    const char *first_source = candidate->sources[0].item_id;
    const char *relation_sql =
        "INSERT INTO concept_candidate_relation(relation_id,candidate_id,src_ref,dst_ref,"
        "relation_type,scope_project,scope_store,relation_sha256,created_at) VALUES(?1,?2,?3,"
        "?4,?5,?6,?7,?8,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    const char *types[] = {"GENERALIZES", "SPECIALIZES"};
    for (int i = 0; i < 2; i++) {
        const char *src = i == 0 ? candidate->candidate_id : first_source;
        const char *dst = i == 0 ? first_source : candidate->candidate_id;
        stage10_buffer_t relation = {0};
        char hash[65] = {0};
        char id[64] = {0};
        ok = stage10_buffer_append(&relation, "%s|%s|%s|%s|%s", src, dst, types[i], input->project,
                                   input->store) &&
             stage10_hash(relation.data, relation.length, hash);
        free(relation.data);
        if (!ok)
            return false;
        snprintf(id, sizeof(id), "candidate-relation-%.20s", hash);
        if (sqlite3_prepare_v2(db, relation_sql, -1, &stmt, NULL) != SQLITE_OK)
            return false;
        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, candidate->candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, src, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, dst, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, types[i], -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, input->project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, input->store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, hash, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!ok)
            return false;
    }
    return stage10_audit_insert(db, input->run_id, candidate->candidate_id, "propose",
                                candidate->decision_sha256);
}

static int stage10_existing_run(sqlite3 *db, const char *idempotency_key,
                                const char *request_sha256, const char *decision_set_sha256) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT request_sha256,decision_set_sha256 FROM concept_growth_run "
                           "WHERE idempotency_key=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, idempotency_key, -1, SQLITE_TRANSIENT);
    int result = CBM_STORE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *request = (const char *)sqlite3_column_text(stmt, 0);
        const char *decision = (const char *)sqlite3_column_text(stmt, 1);
        result = request && decision && strcmp(request, request_sha256) == 0 &&
                         strcmp(decision, decision_set_sha256) == 0
                     ? CBM_STORE_REPLAYED
                     : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);
    return result;
}

static bool stage10_controller_transaction(sqlite3 *db, const char *controller_run_id) {
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

static int stage10_concept_generate(cbm_store_t *store, const cbm_concept_generate_input_t *input,
                                    const char *controller_run_id, bool owns_transaction,
                                    bool stage14_parent_authorized, cbm_concept_result_t *out) {
    if (!out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    if (!store || !stage10_contract_valid(input)) {
        out->failure_code = "INVALID_STAGE10_CONTRACT";
        return CBM_STORE_ERR;
    }
    char request_sha256[65] = {0};
    if (!stage10_request_hash(input, request_sha256))
        return CBM_STORE_ERR;
    stage10_candidates_t candidates = {0};
    if (strcmp(input->mode, "off") == 0) {
        stage10_hash("", 0, candidates.decision_set_sha256);
        out->report_json =
            stage10_report_json(input, &candidates, request_sha256, "disabled", false);
        out->failure_code = "STAGE10_DISABLED";
        return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    sqlite3 *db = cbm_store_get_db(store);
    if (!owns_transaction && (strcmp(input->mode, "active") != 0 ||
                              !stage10_controller_transaction(db, controller_run_id))) {
        out->failure_code = "INVALID_STAGE14_CONTROLLER_TRANSACTION";
        return CBM_STORE_ERR;
    }
    int rc = stage10_collect_candidates(db, input->project, &candidates);
    if (rc != CBM_STORE_OK) {
        stage10_candidates_free(&candidates);
        out->failure_code = "CANDIDATE_EVALUATION_FAILED";
        return rc;
    }
    out->eligible_count = candidates.count;
    out->proposed_count = candidates.count;
    if (strcmp(input->mode, "shadow") == 0 || strcmp(input->mode, "dry_run") == 0 ||
        strcmp(input->operation, "evaluate") == 0) {
        out->report_json =
            stage10_report_json(input, &candidates, request_sha256, "completed", false);
        stage10_candidates_free(&candidates);
        return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    if (!input->run_id || !input->run_id[0] || !input->idempotency_key ||
        !input->idempotency_key[0] ||
        cbm_store_memory_stage10_object_count(store) != STAGE10_OBJECT_COUNT ||
        !stage10_ledger_matches(db)) {
        stage10_candidates_free(&candidates);
        out->failure_code = "SCHEMA_HASH_MISMATCH";
        return CBM_STORE_REJECTED;
    }
    int existing = stage10_existing_run(db, input->idempotency_key, request_sha256,
                                        candidates.decision_set_sha256);
    if (existing == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        stage10_candidates_free(&candidates);
        out->failure_code = "IDEMPOTENCY_CONFLICT";
        return existing;
    }
    if (!stage10_active_guard(input, stage14_parent_authorized)) {
        stage10_candidates_free(&candidates);
        out->failure_code = "ACTIVE_FIXTURE_GUARD";
        return CBM_STORE_REJECTED;
    }
    if (!stage10_manifest_verify(input, &candidates, request_sha256)) {
        stage10_candidates_free(&candidates);
        out->failure_code = "MANIFEST_INPUT_MISMATCH";
        return CBM_STORE_REJECTED;
    }
    if (existing == CBM_STORE_REPLAYED) {
        out->report_json =
            stage10_report_json(input, &candidates, request_sha256, "replayed", false);
        stage10_candidates_free(&candidates);
        return out->report_json ? CBM_STORE_REPLAYED : CBM_STORE_ERR;
    }
    if (existing == CBM_STORE_ERR ||
        (owns_transaction && sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)) {
        stage10_candidates_free(&candidates);
        return CBM_STORE_ERR;
    }
    bool ok = stage10_insert_run(db, input, &candidates, request_sha256) &&
              stage10_audit_insert(db, input->run_id, NULL, "generate", request_sha256);
    char failpoint[32] = {0};
    cbm_safe_getenv("CBM_STAGE10_GENERATE_FAIL_AFTER", failpoint, sizeof(failpoint), NULL);
    if (strcmp(failpoint, "run") == 0)
        ok = false;
    for (int i = 0; ok && i < candidates.count; i++) {
        ok = stage10_insert_candidate(db, input, &candidates.items[i]);
        if (strcmp(failpoint, "candidate") == 0)
            ok = false;
    }
    if (!ok || (owns_transaction && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)) {
        if (owns_transaction)
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        stage10_candidates_free(&candidates);
        out->failure_code = "CONCEPT_GENERATION_FAILED";
        return CBM_STORE_ERR;
    }
    out->production_state_written = true;
    out->report_json = stage10_report_json(input, &candidates, request_sha256, "recorded", true);
    stage10_candidates_free(&candidates);
    return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

int cbm_store_memory_concept_generate(cbm_store_t *store, const cbm_concept_generate_input_t *input,
                                      cbm_concept_result_t *out) {
    return stage10_concept_generate(store, input, NULL, true, false, out);
}

int cbm_store_memory_concept_generate_in_transaction(cbm_store_t *store,
                                                     const cbm_concept_generate_input_t *input,
                                                     const char *controller_run_id,
                                                     bool stage14_parent_authorized,
                                                     cbm_concept_result_t *out) {
    return stage10_concept_generate(store, input, controller_run_id, false,
                                    stage14_parent_authorized, out);
}

static const char *stage10_candidate_state(sqlite3 *db, const char *candidate_id) {
    sqlite3_stmt *stmt = NULL;
    static char state[16];
    snprintf(state, sizeof(state), "proposed");
    if (sqlite3_prepare_v2(db,
                           "SELECT action FROM concept_review_event WHERE candidate_id=?1 "
                           "ORDER BY sequence_no DESC LIMIT 1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *action = (const char *)sqlite3_column_text(stmt, 0);
            if (action) {
                if (strcmp(action, "approve") == 0)
                    snprintf(state, sizeof(state), "approved");
                else if (strcmp(action, "reject") == 0)
                    snprintf(state, sizeof(state), "rejected");
                else if (strcmp(action, "withdraw") == 0)
                    snprintf(state, sizeof(state), "withdrawn");
            }
        }
    }
    sqlite3_finalize(stmt);
    return state;
}

static bool stage10_review_request_hash(const cbm_concept_review_input_t *input, char out[65]) {
    stage10_buffer_t buffer = {0};
    bool ok =
        stage10_buffer_append(&buffer, "%s|%s|%s|%s|%s|%s|%s|%s", input->project, input->store,
                              input->candidate_id, input->action, input->idempotency_key,
                              input->content_text ? input->content_text : "",
                              input->related_candidate_id ? input->related_candidate_id : "",
                              input->reviewer_source) &&
        stage10_hash(buffer.data, buffer.length, out);
    free(buffer.data);
    return ok;
}

static int stage10_existing_review(sqlite3 *db, const char *idempotency_key,
                                   const char *request_sha256) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT request_sha256 FROM concept_review_event WHERE "
                           "idempotency_key=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, idempotency_key, -1, SQLITE_TRANSIENT);
    int result = CBM_STORE_NOT_FOUND;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        result = value && strcmp(value, request_sha256) == 0 ? CBM_STORE_REPLAYED
                                                             : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);
    return result;
}

static bool stage10_review_event_insert(sqlite3 *db, const cbm_concept_review_input_t *input,
                                        int from_version, int to_version,
                                        const char *request_sha256, char event_id[80]) {
    sqlite3_stmt *stmt = NULL;
    char previous[65] = STAGE10_GENESIS;
    if (sqlite3_prepare_v2(db,
                           "SELECT event_hash FROM concept_review_event ORDER BY sequence_no "
                           "DESC LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *value = (const char *)sqlite3_column_text(stmt, 0);
        if (value)
            snprintf(previous, sizeof(previous), "%s", value);
    }
    sqlite3_finalize(stmt);
    char id_hash[65] = {0};
    stage10_buffer_t id_seed = {0};
    bool ok = stage10_buffer_append(&id_seed, "%s|%s|%s", input->candidate_id, input->action,
                                    input->idempotency_key) &&
              stage10_hash(id_seed.data, id_seed.length, id_hash);
    free(id_seed.data);
    if (!ok)
        return false;
    snprintf(event_id, 80, "concept-review-%.24s", id_hash);
    char event_hash[65] = {0};
    stage10_buffer_t event = {0};
    ok = stage10_buffer_append(&event, "%s|%s|%s|%s|%s|%s|%d|%d|%s", event_id, input->candidate_id,
                               input->idempotency_key, request_sha256, input->action,
                               input->reviewer_source, from_version, to_version, previous) &&
         stage10_hash(event.data, event.length, event_hash);
    free(event.data);
    if (!ok || sqlite3_prepare_v2(
                   db,
                   "INSERT INTO concept_review_event(event_id,candidate_id,idempotency_key,"
                   "request_sha256,action,reviewer_source,from_version,to_version,prev_hash,"
                   "event_hash,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,"
                   "strftime('%Y-%m-%dT%H:%M:%fZ','now'));",
                   -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->idempotency_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, request_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, input->action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, input->reviewer_source, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, from_version);
    sqlite3_bind_int(stmt, 8, to_version);
    sqlite3_bind_text(stmt, 9, previous, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 10, event_hash, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

static bool stage10_review_insert_version(sqlite3 *db, const char *candidate_id, int from_version,
                                          int to_version, const char *content_text,
                                          const char *event_id) {
    sqlite3_stmt *select = NULL;
    const char *sql =
        "SELECT classification,classification_reason_code,confidence_ppm,scope_json,"
        "policy_version,policy_sha256,generator_version FROM concept_candidate_version WHERE "
        "candidate_id=?1 AND version=?2;";
    if (sqlite3_prepare_v2(db, sql, -1, &select, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(select, 1, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(select, 2, from_version);
    if (sqlite3_step(select) != SQLITE_ROW) {
        sqlite3_finalize(select);
        return false;
    }
    char *classification = stage10_dup_column(select, 0);
    char *reason = stage10_dup_column(select, 1);
    int confidence = sqlite3_column_int(select, 2);
    char *scope = stage10_dup_column(select, 3);
    int policy_version = sqlite3_column_int(select, 4);
    char *policy_hash = stage10_dup_column(select, 5);
    char *generator = stage10_dup_column(select, 6);
    sqlite3_finalize(select);
    char content_hash[65] = {0};
    bool ok = classification && reason && scope && policy_hash && generator && content_text &&
              stage10_hash(content_text, strlen(content_text), content_hash);
    sqlite3_stmt *insert = NULL;
    const char *insert_sql =
        "INSERT INTO concept_candidate_version(candidate_id,version,classification,"
        "classification_reason_code,confidence_ppm,content_text,content_sha256,scope_json,"
        "policy_version,policy_sha256,generator_version,previous_version,"
        "created_by_review_event_id,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,"
        "?12,?13,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    if (ok)
        ok = sqlite3_prepare_v2(db, insert_sql, -1, &insert, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(insert, 1, candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 2, to_version);
        sqlite3_bind_text(insert, 3, classification, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 4, reason, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 5, confidence);
        sqlite3_bind_text(insert, 6, content_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 7, content_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 8, scope, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 9, policy_version);
        sqlite3_bind_text(insert, 10, policy_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 11, generator, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 12, from_version);
        sqlite3_bind_text(insert, 13, event_id, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(insert) == SQLITE_DONE;
    }
    sqlite3_finalize(insert);
    free(classification);
    free(reason);
    free(scope);
    free(policy_hash);
    free(generator);
    return ok;
}

static bool stage10_review_create_node(sqlite3 *db, const char *candidate_id, const char *project,
                                       const char *store_name, int candidate_version,
                                       const char *event_id) {
    char node_hash[65] = {0};
    char node_id[64] = {0};
    if (!stage10_hash(candidate_id, strlen(candidate_id), node_hash))
        return false;
    snprintf(node_id, sizeof(node_id), "concept-node-%.24s", node_hash);
    sqlite3_stmt *stmt = NULL;
    const char *node_sql =
        "INSERT INTO concept_node(node_id,candidate_id,scope_project,scope_store,"
        "created_review_event_id,created_at) VALUES(?1,?2,?3,?4,?5,"
        "strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    if (sqlite3_prepare_v2(db, node_sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, node_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, store_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, event_id, -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok)
        return false;
    const char *version_sql =
        "INSERT INTO concept_node_version(node_id,version,candidate_version,content_text,"
        "content_sha256,scope_json,created_review_event_id,created_at) SELECT ?1,1,version,"
        "content_text,content_sha256,scope_json,?2,strftime('%Y-%m-%dT%H:%M:%fZ','now') FROM "
        "concept_candidate_version WHERE candidate_id=?3 AND version=?4;";
    if (sqlite3_prepare_v2(db, version_sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, node_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, candidate_version);
    ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(stmt);
    return ok;
}

static bool stage10_relation_pair(sqlite3 *db, const char *candidate_id,
                                  const char *related_candidate_id, const char *project,
                                  const char *store_name, const char *event_id) {
    if (!related_candidate_id || !related_candidate_id[0])
        return true;
    sqlite3_stmt *stmt = NULL;
    char *node = NULL;
    char *related = NULL;
    const char *sql =
        "SELECT node_id FROM concept_node WHERE candidate_id=?1 AND scope_project=?2 AND "
        "scope_store=?3;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, store_name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            node = stage10_dup_column(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, related_candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, store_name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            related = stage10_dup_column(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (!node || !related) {
        free(node);
        free(related);
        return false;
    }
    const char *types[] = {"GENERALIZES", "SPECIALIZES"};
    bool ok = true;
    for (int i = 0; ok && i < 2; i++) {
        const char *src = i == 0 ? node : related;
        const char *dst = i == 0 ? related : node;
        stage10_buffer_t relation = {0};
        char hash[65] = {0};
        char id[64] = {0};
        ok = stage10_buffer_append(&relation, "%s|%s|%s|%s|%s", src, dst, types[i], project,
                                   store_name) &&
             stage10_hash(relation.data, relation.length, hash);
        free(relation.data);
        if (!ok)
            break;
        snprintf(id, sizeof(id), "concept-relation-%.20s", hash);
        const char *insert =
            "INSERT INTO concept_relation(relation_id,src_node_id,dst_node_id,relation_type,"
            "scope_project,scope_store,created_review_event_id,relation_sha256,created_at) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
        if (sqlite3_prepare_v2(db, insert, -1, &stmt, NULL) != SQLITE_OK) {
            ok = false;
            break;
        }
        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, src, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, dst, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, types[i], -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, store_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, event_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, hash, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        stmt = NULL;
    }
    sqlite3_finalize(stmt);
    free(node);
    free(related);
    return ok;
}

static char *stage10_review_report(const cbm_concept_review_input_t *input, const char *status,
                                   const char *state, const char *event_id, int version,
                                   bool state_written) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage10-concept-review-report/v1");
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "store", input->store);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", input->candidate_id);
    yyjson_mut_obj_add_str(doc, root, "action", input->action);
    yyjson_mut_obj_add_str(doc, root, "state", state);
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id ? event_id : "");
    yyjson_mut_obj_add_int(doc, root, "version", version);
    yyjson_mut_obj_add_bool(doc, root, "production_state_written", state_written);
    yyjson_mut_obj_add_bool(doc, root, "untrusted_data", true);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

int cbm_store_memory_concept_review(cbm_store_t *store, const cbm_concept_review_input_t *input,
                                    cbm_concept_result_t *out) {
    if (!out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    bool valid_action =
        input && input->action &&
        (strcmp(input->action, "approve") == 0 || strcmp(input->action, "edit") == 0 ||
         strcmp(input->action, "reject") == 0 || strcmp(input->action, "withdraw") == 0);
    if (!store || !input || !input->project || !input->store || !input->candidate_id ||
        !input->idempotency_key || !input->reviewer_source || !valid_action ||
        strcmp(input->store, STAGE10_STORE) != 0 ||
        (strcmp(input->reviewer_source, "explicit_user") != 0 &&
         strcmp(input->reviewer_source, "fixture") != 0) ||
        (strcmp(input->action, "edit") == 0 && (!input->content_text || !input->content_text[0]))) {
        out->failure_code = "INVALID_REVIEW_CONTRACT";
        return CBM_STORE_ERR;
    }
    sqlite3 *db = cbm_store_get_db(store);
    if (cbm_store_memory_stage10_object_count(store) != STAGE10_OBJECT_COUNT ||
        !stage10_ledger_matches(db)) {
        out->failure_code = "SCHEMA_HASH_MISMATCH";
        return CBM_STORE_REJECTED;
    }
    char request_sha256[65] = {0};
    if (!stage10_review_request_hash(input, request_sha256))
        return CBM_STORE_ERR;
    int existing = stage10_existing_review(db, input->idempotency_key, request_sha256);
    if (existing == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        out->failure_code = "IDEMPOTENCY_CONFLICT";
        return existing;
    }
    sqlite3_stmt *stmt = NULL;
    const char *candidate_sql =
        "SELECT scope_project,scope_store,MAX(version) FROM concept_candidate c JOIN "
        "concept_candidate_version v ON v.candidate_id=c.candidate_id WHERE c.candidate_id=?1 "
        "GROUP BY c.scope_project,c.scope_store;";
    if (sqlite3_prepare_v2(db, candidate_sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->candidate_id, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW;
    const char *project = found ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
    const char *store_name = found ? (const char *)sqlite3_column_text(stmt, 1) : NULL;
    int version = found ? sqlite3_column_int(stmt, 2) : 0;
    bool scope_ok = found && project && store_name && strcmp(project, input->project) == 0 &&
                    strcmp(store_name, input->store) == 0;
    sqlite3_finalize(stmt);
    if (!scope_ok) {
        out->failure_code = "CANDIDATE_NOT_FOUND_OR_SCOPE_MISMATCH";
        return CBM_STORE_NOT_FOUND;
    }
    const char *state = stage10_candidate_state(db, input->candidate_id);
    if (existing == CBM_STORE_REPLAYED) {
        out->report_json = stage10_review_report(input, "replayed", state, "", version, false);
        return out->report_json ? CBM_STORE_REPLAYED : CBM_STORE_ERR;
    }
    if ((strcmp(input->action, "withdraw") == 0 && strcmp(state, "approved") != 0) ||
        (strcmp(input->action, "withdraw") != 0 && strcmp(state, "proposed") != 0)) {
        out->failure_code = strcmp(input->action, "withdraw") == 0 ? "CANDIDATE_NOT_APPROVED"
                                                                   : "CANDIDATE_NOT_PROPOSED";
        return CBM_STORE_REJECTED;
    }
    int next_version = strcmp(input->action, "edit") == 0 ? version + 1 : version;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    char event_id[80] = {0};
    bool ok =
        stage10_review_event_insert(db, input, version, next_version, request_sha256, event_id);
    if (ok && strcmp(input->action, "edit") == 0)
        ok = stage10_review_insert_version(db, input->candidate_id, version, next_version,
                                           input->content_text, event_id);
    if (ok && strcmp(input->action, "approve") == 0)
        ok = stage10_review_create_node(db, input->candidate_id, input->project, input->store,
                                        version, event_id) &&
             stage10_relation_pair(db, input->candidate_id, input->related_candidate_id,
                                   input->project, input->store, event_id);
    if (ok)
        ok = stage10_audit_insert(db, NULL, input->candidate_id, input->action, request_sha256);
    char failpoint[32] = {0};
    cbm_safe_getenv("CBM_STAGE10_REVIEW_FAIL_AFTER", failpoint, sizeof(failpoint), NULL);
    if (failpoint[0])
        ok = false;
    if (!ok || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        out->failure_code = "CONCEPT_REVIEW_FAILED";
        return CBM_STORE_ERR;
    }
    state = stage10_candidate_state(db, input->candidate_id);
    out->production_state_written = true;
    out->report_json =
        stage10_review_report(input, "recorded", state, event_id, next_version, true);
    return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

int cbm_store_memory_concept_inspect(cbm_store_t *store, const char *project,
                                     const char *store_name, const char *candidate_id,
                                     cbm_concept_result_t *out) {
    if (!out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    if (!store || !project || !store_name || !candidate_id ||
        strcmp(store_name, STAGE10_STORE) != 0) {
        out->failure_code = "INVALID_INSPECT_CONTRACT";
        return CBM_STORE_ERR;
    }
    sqlite3 *db = cbm_store_get_db(store);
    if (!stage10_table_exists(db, "concept_candidate")) {
        out->failure_code = "STAGE10_NOT_MIGRATED";
        return CBM_STORE_NOT_FOUND;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT c.identity_sha256,c.initial_state,COUNT(DISTINCT v.version),"
        "COUNT(DISTINCT s.source_item_id),(SELECT COUNT(*) FROM concept_review_event r WHERE "
        "r.candidate_id=c.candidate_id),(SELECT COUNT(*) FROM concept_node n WHERE "
        "n.candidate_id=c.candidate_id),(SELECT COUNT(*) FROM concept_relation cr WHERE "
        "cr.src_node_id IN (SELECT node_id FROM concept_node WHERE candidate_id=c.candidate_id) "
        "OR cr.dst_node_id IN (SELECT node_id FROM concept_node WHERE candidate_id=c.candidate_id))"
        " FROM concept_candidate c JOIN concept_candidate_version v ON "
        "v.candidate_id=c.candidate_id"
        " JOIN concept_candidate_source s ON s.candidate_id=c.candidate_id WHERE c.candidate_id=?1"
        " AND c.scope_project=?2 AND c.scope_store=?3 GROUP BY c.candidate_id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, store_name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        out->failure_code = "CANDIDATE_NOT_FOUND_OR_SCOPE_MISMATCH";
        return CBM_STORE_NOT_FOUND;
    }
    char *identity = stage10_dup_column(stmt, 0);
    int version_count = sqlite3_column_int(stmt, 2);
    int source_count = sqlite3_column_int(stmt, 3);
    int review_count = sqlite3_column_int(stmt, 4);
    int node_count = sqlite3_column_int(stmt, 5);
    int relation_count = sqlite3_column_int(stmt, 6);
    sqlite3_finalize(stmt);
    const char *state = stage10_candidate_state(db, candidate_id);
    const char *version_sql =
        "SELECT version,classification,classification_reason_code,confidence_ppm,content_text,"
        "content_sha256 FROM concept_candidate_version WHERE candidate_id=?1 ORDER BY version;";
    if (sqlite3_prepare_v2(db, version_sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(identity);
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage10-concept-inspect/v1");
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "store", store_name);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", candidate_id);
    yyjson_mut_obj_add_str(doc, root, "identity_sha256", identity ? identity : "");
    yyjson_mut_obj_add_str(doc, root, "state", state);
    yyjson_mut_obj_add_int(doc, root, "version_count", version_count);
    yyjson_mut_obj_add_int(doc, root, "source_count", source_count);
    yyjson_mut_obj_add_int(doc, root, "review_count", review_count);
    yyjson_mut_obj_add_int(doc, root, "node_count", node_count);
    yyjson_mut_obj_add_int(doc, root, "relation_count", relation_count);
    yyjson_mut_obj_add_bool(doc, root, "untrusted_data", true);
    yyjson_mut_val *versions = yyjson_mut_arr(doc);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        yyjson_mut_val *version = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_int(doc, version, "version", sqlite3_column_int(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc, version, "classification",
                                  (const char *)sqlite3_column_text(stmt, 1));
        yyjson_mut_obj_add_strcpy(doc, version, "reason_code",
                                  (const char *)sqlite3_column_text(stmt, 2));
        yyjson_mut_obj_add_int(doc, version, "confidence_ppm", sqlite3_column_int(stmt, 3));
        yyjson_mut_obj_add_strcpy(doc, version, "content_text",
                                  (const char *)sqlite3_column_text(stmt, 4));
        yyjson_mut_obj_add_strcpy(doc, version, "content_sha256",
                                  (const char *)sqlite3_column_text(stmt, 5));
        yyjson_mut_obj_add_bool(doc, version, "untrusted_data", true);
        yyjson_mut_arr_add_val(versions, version);
    }
    sqlite3_finalize(stmt);
    yyjson_mut_obj_add_val(doc, root, "versions", versions);
    out->report_json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    free(identity);
    return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
}

static bool stage10_verify_chain(sqlite3 *db, const char *table, bool review, int *out_count) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             review
                 ? "SELECT event_id,candidate_id,idempotency_key,request_sha256,action,"
                   "reviewer_source,from_version,to_version,prev_hash,event_hash FROM %s ORDER BY "
                   "sequence_no;"
                 : "SELECT event_id,COALESCE(run_id,''),COALESCE(candidate_id,''),operation,"
                   "payload_sha256,prev_hash,event_hash FROM %s ORDER BY sequence_no;",
             table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    char previous[65] = STAGE10_GENESIS;
    int count = 0;
    bool ok = true;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        stage10_buffer_t buffer = {0};
        const char *prev = (const char *)sqlite3_column_text(stmt, review ? 8 : 5);
        const char *actual = (const char *)sqlite3_column_text(stmt, review ? 9 : 6);
        if (!prev || !actual || strcmp(prev, previous) != 0) {
            ok = false;
            free(buffer.data);
            break;
        }
        if (review) {
            ok = stage10_buffer_append(&buffer, "%s|%s|%s|%s|%s|%s|%d|%d|%s",
                                       sqlite3_column_text(stmt, 0), sqlite3_column_text(stmt, 1),
                                       sqlite3_column_text(stmt, 2), sqlite3_column_text(stmt, 3),
                                       sqlite3_column_text(stmt, 4), sqlite3_column_text(stmt, 5),
                                       sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 7),
                                       prev);
        } else {
            ok = stage10_buffer_append(&buffer, "%s|%s|%s|%s|%s|%s", sqlite3_column_text(stmt, 0),
                                       sqlite3_column_text(stmt, 1), sqlite3_column_text(stmt, 2),
                                       sqlite3_column_text(stmt, 3), sqlite3_column_text(stmt, 4),
                                       prev);
        }
        char calculated[65] = {0};
        ok = ok && stage10_hash(buffer.data, buffer.length, calculated) &&
             strcmp(calculated, actual) == 0;
        free(buffer.data);
        if (!ok)
            break;
        snprintf(previous, sizeof(previous), "%s", actual);
        count++;
    }
    sqlite3_finalize(stmt);
    if (out_count)
        *out_count = count;
    return ok;
}

int cbm_store_memory_stage10_audit_verify(cbm_store_t *store, int *growth_count,
                                          int *review_count) {
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    if (!db || !stage10_table_exists(db, "concept_growth_audit_event") ||
        !stage10_table_exists(db, "concept_review_event"))
        return CBM_STORE_ERR;
    bool growth_ok = stage10_verify_chain(db, "concept_growth_audit_event", false, growth_count);
    int reviews = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM concept_review_event;", -1, &stmt, NULL) !=
        SQLITE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        reviews = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    bool review_ok =
        reviews == 0 || stage10_verify_chain(db, "concept_review_event", true, review_count);
    if (reviews == 0 && review_count)
        *review_count = 0;
    return growth_ok && review_ok ? CBM_STORE_OK : CBM_STORE_ERR;
}

void cbm_store_memory_concept_result_free(cbm_concept_result_t *result) {
    if (!result)
        return;
    free(result->report_json);
    memset(result, 0, sizeof(*result));
}
