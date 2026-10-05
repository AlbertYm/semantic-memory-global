/* Stage 5 observe-only retrieval, injection, and usage journal. */

#include "memory/memory_store.h"
#include "memory/memory_security.h"
#include "foundation/platform.h"
#include "store/store.h"

#include <sqlite3.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <yyjson/yyjson.h>

#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"

static char *obs_dup(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t len = strlen(value) + 1;
    char *copy = malloc(len);
    if (copy) {
        memcpy(copy, value, len);
    }
    return copy;
}

static bool obs_text_equal(const char *a, const char *b) {
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static bool obs_hex_exact(const char *value, size_t length) {
    if (!value || strlen(value) != length)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (!isxdigit((unsigned char)value[i]))
            return false;
    }
    return true;
}

static bool obs_content_hash_valid(const char *value) {
    return obs_hex_exact(value, 64) ||
           (value && strncmp(value, "xxh3-", 5) == 0 && obs_hex_exact(value + 5, 32));
}

static void obs_bind_nullable(sqlite3_stmt *stmt, int index, const char *value) {
    if (value) {
        sqlite3_bind_text(stmt, index, value, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, index);
    }
}

static bool obs_allowed(const char *value, const char *const *allowed, int count) {
    if (!value) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(value, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void obs_timestamp(char out[40]) {
    time_t now = time(NULL);
    struct tm local = {0};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &local);
    strncat(out, ".000+08:00", 39 - strlen(out));
}

static char *obs_stable_id(const char *prefix, const char *const *parts, int count) {
    XXH3_state_t *state = XXH3_createState();
    if (!state || XXH3_128bits_reset(state) == XXH_ERROR) {
        XXH3_freeState(state);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        const char *part = parts[i] ? parts[i] : "";
        uint64_t len = (uint64_t)strlen(part);
        if (XXH3_128bits_update(state, &len, sizeof(len)) == XXH_ERROR ||
            XXH3_128bits_update(state, part, (size_t)len) == XXH_ERROR) {
            XXH3_freeState(state);
            return NULL;
        }
    }
    XXH128_hash_t hash = XXH3_128bits_digest(state);
    XXH3_freeState(state);
    char value[96];
    snprintf(value, sizeof(value), "%s-%016llx%016llx", prefix, (unsigned long long)hash.high64,
             (unsigned long long)hash.low64);
    return obs_dup(value);
}

static const char *obs_source_type(const char *source) {
    static const char *const allowed[] = {"fts", "vector", "code_anchor", "graph", "manual"};
    if (!source || strcmp(source, "structured") == 0) {
        return "manual";
    }
    return obs_allowed(source, allowed, 5) ? source : NULL;
}

int cbm_store_memory_observe_session_begin(cbm_store_t *s,
                                           const cbm_retrieval_session_input_t *input,
                                           char **out_session_id, char **out_request_id,
                                           bool *out_replayed) {
    static const char *const scopes[] = {"project", "global", "mixed"};
    if (out_session_id)
        *out_session_id = NULL;
    if (out_request_id)
        *out_request_id = NULL;
    if (out_replayed)
        *out_replayed = false;
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (!db || !input || !out_session_id || !out_request_id ||
        !obs_allowed(input->memory_scope, scopes, 3) || !input->algorithm_version ||
        input->config_version < 0 || !input->query_text) {
        return CBM_STORE_ERR;
    }

    char generated[96];
    const char *request_id = input->request_id;
    if (!request_id || !request_id[0]) {
        snprintf(generated, sizeof(generated), "rs-%llu", (unsigned long long)cbm_now_ns());
        request_id = generated;
    }
    if (strlen(request_id) > 255) {
        return CBM_STORE_ERR;
    }
    const char *query_parts[] = {input->query_text};
    char *query_hash = obs_stable_id("xxh3", query_parts, 1);
    if (!query_hash) {
        return CBM_STORE_ERR;
    }

    sqlite3_stmt *stmt = NULL;
    const char *select_sql =
        "SELECT id,project_scope,memory_scope,algorithm_version,config_version,query_hash "
        "FROM retrieval_session WHERE request_id=?1;";
    if (sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(query_hash);
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, request_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *existing_id = (const char *)sqlite3_column_text(stmt, 0);
        bool same =
            obs_text_equal((const char *)sqlite3_column_text(stmt, 1), input->project_scope) &&
            obs_text_equal((const char *)sqlite3_column_text(stmt, 2), input->memory_scope) &&
            obs_text_equal((const char *)sqlite3_column_text(stmt, 3), input->algorithm_version) &&
            sqlite3_column_int(stmt, 4) == input->config_version &&
            obs_text_equal((const char *)sqlite3_column_text(stmt, 5), query_hash);
        if (same) {
            *out_session_id = obs_dup(existing_id);
            *out_request_id = obs_dup(request_id);
            if (out_replayed)
                *out_replayed = true;
        }
        sqlite3_finalize(stmt);
        free(query_hash);
        return same && *out_session_id && *out_request_id ? CBM_STORE_REPLAYED
                                                          : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);

    char timestamp[40];
    obs_timestamp(timestamp);
    const char *insert_sql =
        "INSERT INTO retrieval_session(id,request_id,project_scope,memory_scope,mode,status,"
        "algorithm_version,config_version,query_hash,started_at) "
        "VALUES(?1,?1,?2,?3,'observe_only','open',?4,?5,?6,?7);";
    if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(query_hash);
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, request_id, -1, SQLITE_TRANSIENT);
    obs_bind_nullable(stmt, 2, input->project_scope);
    sqlite3_bind_text(stmt, 3, input->memory_scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, input->algorithm_version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, input->config_version);
    sqlite3_bind_text(stmt, 6, query_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, timestamp, -1, SQLITE_TRANSIENT);
    bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    free(query_hash);
    if (!inserted) {
        return CBM_STORE_ERR;
    }
    *out_session_id = obs_dup(request_id);
    *out_request_id = obs_dup(request_id);
    return *out_session_id && *out_request_id ? CBM_STORE_OK : CBM_STORE_ERR;
}

int cbm_store_memory_observe_session_complete(cbm_store_t *s, const char *session_id,
                                              const char *status, const char *error_code) {
    static const char *const statuses[] = {"completed", "cancelled", "failed"};
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (!db || !session_id || !obs_allowed(status, statuses, 3)) {
        return CBM_STORE_ERR;
    }
    char timestamp[40];
    obs_timestamp(timestamp);
    sqlite3_stmt *stmt = NULL;
    const char *sql = "UPDATE retrieval_session SET status=?2,completed_at=?3,error_code=?4 "
                      "WHERE id=?1 AND (status='open' OR status=?2);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_TRANSIENT);
    obs_bind_nullable(stmt, 4, error_code);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(stmt);
    return ok ? CBM_STORE_OK : CBM_STORE_ERR;
}

void cbm_store_memory_observation_refs_free(cbm_retrieval_observation_ref_t *refs, int count) {
    if (!refs) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(refs[i].candidate_id);
        free(refs[i].provenance_id);
        free(refs[i].evidence_id);
        free(refs[i].content_hash);
        memset(&refs[i], 0, sizeof(refs[i]));
    }
}

static int obs_candidate_exact(sqlite3 *db, const char *candidate_id, const char *session_id,
                               const cbm_retrieval_candidate_observation_t *input,
                               const char *content_hash) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM retrieval_candidate WHERE id=?1 AND session_id=?2 "
                      "AND source_store_kind=?3 AND source_store_id=?4 AND memory_item_id=?5 "
                      "AND content_hash=?6 AND aggregate_score=?7 AND aggregate_rank=?8 "
                      "AND decision_status=?9;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->source_store_kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, input->source_store_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, input->memory_item_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, content_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 7, input->normalized_score);
    sqlite3_bind_int(stmt, 8, input->aggregate_rank);
    sqlite3_bind_text(stmt, 9, input->decision_status, -1, SQLITE_TRANSIENT);
    bool exact = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return exact ? CBM_STORE_OK : CBM_STORE_IDEMPOTENCY_CONFLICT;
}

static int obs_source_exact(sqlite3 *db, const char *provenance_id, const char *candidate_id,
                            const char *source_type,
                            const cbm_retrieval_candidate_observation_t *input,
                            const char *detail) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT COUNT(*) FROM retrieval_candidate_source WHERE id=?1 AND candidate_id=?2 "
        "AND source_type=?3 AND source_rank=?4 AND raw_score=?5 AND normalized_score=?6 "
        "AND source_detail_json=?7;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, provenance_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, input->source_rank);
    sqlite3_bind_double(stmt, 5, input->raw_score);
    sqlite3_bind_double(stmt, 6, input->normalized_score);
    sqlite3_bind_text(stmt, 7, detail, -1, SQLITE_TRANSIENT);
    bool exact = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return exact ? CBM_STORE_OK : CBM_STORE_IDEMPOTENCY_CONFLICT;
}

static int obs_edge_visits(sqlite3 *db, const char *session_id, const char *root_memory_id) {
    const char *walk_sql =
        "WITH RECURSIVE walk(previous_id,current_id,edge_id,relation_type,confidence,depth,path) "
        "AS ("
        "SELECT ?2,CASE WHEN e.src_id=?2 THEN e.dst_id ELSE e.src_id "
        "END,e.id,e.type,e.confidence,1,"
        "','||?2||','||CASE WHEN e.src_id=?2 THEN e.dst_id ELSE e.src_id END||',' "
        "FROM memory_edge e WHERE (e.src_id=?2 OR e.dst_id=?2) AND e.type IN "
        "('supports','derived_from','used_in','contradicts','supersedes') UNION ALL "
        "SELECT w.current_id,CASE WHEN e.src_id=w.current_id THEN e.dst_id ELSE e.src_id END,"
        "e.id,e.type,e.confidence,w.depth+1,w.path||"
        "CASE WHEN e.src_id=w.current_id THEN e.dst_id ELSE e.src_id END||',' "
        "FROM walk w JOIN memory_edge e ON (e.src_id=w.current_id OR e.dst_id=w.current_id) "
        "WHERE w.depth<3 AND e.type IN "
        "('supports','derived_from','used_in','contradicts','supersedes') "
        "AND instr(w.path,','||CASE WHEN e.src_id=w.current_id THEN e.dst_id ELSE e.src_id "
        "END||',')=0) "
        "SELECT f.id,t.id,w.edge_id,w.relation_type,w.confidence,w.depth FROM walk w "
        "JOIN retrieval_candidate f ON f.session_id=?1 AND f.memory_item_id=w.previous_id "
        "JOIN retrieval_candidate t ON t.session_id=?1 AND t.memory_item_id=w.current_id "
        "ORDER BY w.depth,w.edge_id;";
    sqlite3_stmt *walk = NULL;
    if (sqlite3_prepare_v2(db, walk_sql, -1, &walk, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(walk, 1, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(walk, 2, root_memory_id, -1, SQLITE_TRANSIENT);
    char timestamp[40];
    obs_timestamp(timestamp);
    int result = CBM_STORE_OK;
    while (sqlite3_step(walk) == SQLITE_ROW) {
        const char *from_id = (const char *)sqlite3_column_text(walk, 0);
        const char *to_id = (const char *)sqlite3_column_text(walk, 1);
        const char *edge_id = (const char *)sqlite3_column_text(walk, 2);
        const char *relation = (const char *)sqlite3_column_text(walk, 3);
        double confidence = sqlite3_column_double(walk, 4);
        int depth = sqlite3_column_int(walk, 5);
        char depth_text[16];
        snprintf(depth_text, sizeof(depth_text), "%d", depth);
        const char *parts[] = {session_id, root_memory_id, edge_id, from_id, to_id, depth_text};
        char *visit_id = obs_stable_id("visit", parts, 6);
        sqlite3_stmt *insert = NULL;
        const char *insert_sql =
            "INSERT OR IGNORE INTO retrieval_edge_visit(id,session_id,from_candidate_id,"
            "to_candidate_id,memory_edge_id,relation_type,hop_depth,activation_in,activation_out,"
            "visit_status,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,1.0,?8,'accepted',?9);";
        if (!visit_id || sqlite3_prepare_v2(db, insert_sql, -1, &insert, NULL) != SQLITE_OK) {
            free(visit_id);
            result = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(insert, 1, visit_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, from_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 4, to_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 5, edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 6, relation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(insert, 7, depth);
        sqlite3_bind_double(insert, 8, confidence);
        sqlite3_bind_text(insert, 9, timestamp, -1, SQLITE_TRANSIENT);
        bool ok = sqlite3_step(insert) == SQLITE_DONE;
        sqlite3_finalize(insert);
        free(visit_id);
        if (!ok) {
            result = CBM_STORE_ERR;
            break;
        }
    }
    sqlite3_finalize(walk);
    return result;
}

int cbm_store_memory_observe_candidates(cbm_store_t *s, const char *session_id,
                                        const cbm_retrieval_candidate_observation_t *candidates,
                                        int count, cbm_retrieval_observation_ref_t *out_refs) {
    static const char *const store_kinds[] = {"project", "global"};
    static const char *const decisions[] = {"retrieved", "selected", "rejected", "contradicted"};
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (!db || !session_id || !candidates || count < 0 || (count > 0 && !out_refs)) {
        return CBM_STORE_ERR;
    }
    if (count == 0) {
        return CBM_STORE_OK;
    }
    bool nested_transaction = sqlite3_get_autocommit(db) == 0;
    if ((nested_transaction
             ? sqlite3_exec(db, "SAVEPOINT cbm_observe_candidates;", NULL, NULL, NULL)
             : cbm_store_begin(s)) != CBM_STORE_OK) {
        return CBM_STORE_ERR;
    }
    int result = CBM_STORE_OK;
    char timestamp[40];
    obs_timestamp(timestamp);
    for (int i = 0; i < count && result == CBM_STORE_OK; i++) {
        const cbm_retrieval_candidate_observation_t *input = &candidates[i];
        const char *source_type = obs_source_type(input->retrieval_source);
        if (!input->source_store_id || !input->memory_item_id || !source_type ||
            !obs_allowed(input->source_store_kind, store_kinds, 2) ||
            !obs_allowed(input->decision_status, decisions, 4) || input->source_rank <= 0 ||
            input->aggregate_rank <= 0) {
            result = CBM_STORE_ERR;
            break;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, "SELECT content FROM memory_item WHERE id=?1;", -1, &stmt,
                               NULL) != SQLITE_OK) {
            result = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(stmt, 1, input->memory_item_id, -1, SQLITE_TRANSIENT);
        const char *content =
            sqlite3_step(stmt) == SQLITE_ROW ? (const char *)sqlite3_column_text(stmt, 0) : NULL;
        char *content_copy = obs_dup(content);
        sqlite3_finalize(stmt);
        if (!content_copy) {
            result = CBM_STORE_NOT_FOUND;
            break;
        }
        const char *candidate_parts[] = {session_id, input->source_store_kind,
                                         input->source_store_id, input->memory_item_id};
        const char *content_parts[] = {content_copy};
        const char *evidence_parts[] = {session_id, input->memory_item_id,
                                        input->evidence_json ? input->evidence_json : "[]"};
        out_refs[i].candidate_id = obs_stable_id("cand", candidate_parts, 4);
        out_refs[i].content_hash = obs_stable_id("xxh3", content_parts, 1);
        const char *source_parts[] = {out_refs[i].candidate_id, source_type};
        out_refs[i].provenance_id = obs_stable_id("prov", source_parts, 2);
        out_refs[i].evidence_id = obs_stable_id("evid", evidence_parts, 3);
        free(content_copy);
        if (!out_refs[i].candidate_id || !out_refs[i].content_hash || !out_refs[i].provenance_id ||
            !out_refs[i].evidence_id) {
            result = CBM_STORE_ERR;
            break;
        }

        const char *candidate_sql =
            "INSERT OR IGNORE INTO retrieval_candidate(id,session_id,source_store_kind,"
            "source_store_id,memory_item_id,content_hash,aggregate_score,aggregate_rank,"
            "decision_status,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
        if (sqlite3_prepare_v2(db, candidate_sql, -1, &stmt, NULL) != SQLITE_OK) {
            result = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(stmt, 1, out_refs[i].candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, input->source_store_kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, input->source_store_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, input->memory_item_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, out_refs[i].content_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 7, input->normalized_score);
        sqlite3_bind_int(stmt, 8, input->aggregate_rank);
        sqlite3_bind_text(stmt, 9, input->decision_status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, timestamp, -1, SQLITE_TRANSIENT);
        bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!inserted || obs_candidate_exact(db, out_refs[i].candidate_id, session_id, input,
                                             out_refs[i].content_hash) != CBM_STORE_OK) {
            result = CBM_STORE_IDEMPOTENCY_CONFLICT;
            break;
        }

        const char *detail = input->source_detail_json ? input->source_detail_json
                                                       : (strcmp(source_type, "graph") == 0
                                                              ? "{\"provenance\":\"indirect\"}"
                                                              : "{\"provenance\":\"direct\"}");
        const char *source_sql =
            "INSERT OR IGNORE INTO retrieval_candidate_source(id,candidate_id,source_type,"
            "source_rank,raw_score,normalized_score,source_detail_json) "
            "VALUES(?1,?2,?3,?4,?5,?6,?7);";
        if (sqlite3_prepare_v2(db, source_sql, -1, &stmt, NULL) != SQLITE_OK) {
            result = CBM_STORE_ERR;
            break;
        }
        sqlite3_bind_text(stmt, 1, out_refs[i].provenance_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, out_refs[i].candidate_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, source_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, input->source_rank);
        sqlite3_bind_double(stmt, 5, input->raw_score);
        sqlite3_bind_double(stmt, 6, input->normalized_score);
        sqlite3_bind_text(stmt, 7, detail, -1, SQLITE_TRANSIENT);
        inserted = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        if (!inserted || obs_source_exact(db, out_refs[i].provenance_id, out_refs[i].candidate_id,
                                          source_type, input, detail) != CBM_STORE_OK) {
            result = CBM_STORE_IDEMPOTENCY_CONFLICT;
        }
    }
    for (int i = 0; i < count && result == CBM_STORE_OK; i++) {
        const char *source_type = obs_source_type(candidates[i].retrieval_source);
        if (source_type && strcmp(source_type, "graph") != 0 &&
            strcmp(source_type, "vector") != 0 &&
            obs_edge_visits(db, session_id, candidates[i].memory_item_id) != CBM_STORE_OK) {
            result = CBM_STORE_ERR;
        }
    }
    if (result == CBM_STORE_OK) {
        result = nested_transaction ? (sqlite3_exec(db, "RELEASE cbm_observe_candidates;", NULL,
                                                    NULL, NULL) == SQLITE_OK
                                           ? CBM_STORE_OK
                                           : CBM_STORE_ERR)
                                    : cbm_store_commit(s);
    } else {
        if (nested_transaction) {
            sqlite3_exec(db, "ROLLBACK TO cbm_observe_candidates;", NULL, NULL, NULL);
            sqlite3_exec(db, "RELEASE cbm_observe_candidates;", NULL, NULL, NULL);
        } else {
            cbm_store_rollback(s);
        }
    }
    if (result != CBM_STORE_OK) {
        cbm_store_memory_observation_refs_free(out_refs, count);
    }
    return result;
}

static int obs_candidate_hash(sqlite3 *db, const char *session_id, const char *candidate_id,
                              const char *content_hash) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM retrieval_candidate WHERE id=?1 AND session_id=?2 AND "
                      "content_hash=?3;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, content_hash, -1, SQLITE_TRANSIENT);
    bool found = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return found ? CBM_STORE_OK : CBM_STORE_NOT_FOUND;
}

static int obs_injection_existing(sqlite3 *db, const cbm_observe_injection_input_t *input) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT session_id,candidate_id,injection_index,target,content_hash,token_count "
        "FROM context_injection WHERE id=?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->event_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CBM_STORE_NOT_FOUND;
    }
    bool exact = obs_text_equal((const char *)sqlite3_column_text(stmt, 0), input->session_id) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 1), input->candidate_id) &&
                 sqlite3_column_int(stmt, 2) == input->injection_index &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 3), input->target) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 4), input->content_hash) &&
                 sqlite3_column_int(stmt, 5) == input->token_count;
    sqlite3_finalize(stmt);
    return exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT;
}

static int obs_usage_existing(sqlite3 *db, const cbm_observe_usage_input_t *input) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "session_id,candidate_id,injection_id,outcome,evidence_type,evidence_ref,evidence_hash "
        "FROM memory_usage_attribution WHERE id=?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->event_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CBM_STORE_NOT_FOUND;
    }
    bool exact = obs_text_equal((const char *)sqlite3_column_text(stmt, 0), input->session_id) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 1), input->candidate_id) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 2), input->injection_id) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 3), input->outcome) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 4), input->evidence_type) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 5), input->evidence_ref) &&
                 obs_text_equal((const char *)sqlite3_column_text(stmt, 6), input->evidence_hash);
    sqlite3_finalize(stmt);
    return exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT;
}

static bool obs_id_exists(sqlite3 *db, const char *table, const char *event_id) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE id=?1;", table);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return exists;
}

int cbm_store_memory_observe_usage(cbm_store_t *s, const cbm_observe_usage_input_t *input) {
    static const char *const outcomes[] = {"used", "ignored", "rejected", "contradicted",
                                           "uncertain"};
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (!db || !input || !input->event_id || !input->session_id || !input->candidate_id ||
        !obs_allowed(input->outcome, outcomes, 5) || !input->evidence_type ||
        !input->evidence_ref) {
        return CBM_STORE_ERR;
    }
    if (obs_id_exists(db, "context_injection", input->event_id)) {
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    int existing = obs_usage_existing(db, input);
    if (existing != CBM_STORE_NOT_FOUND) {
        return existing;
    }
    sqlite3_stmt *stmt = NULL;
    char timestamp[40];
    obs_timestamp(timestamp);
    const char *sql =
        "INSERT INTO memory_usage_attribution(id,session_id,candidate_id,injection_id,outcome,"
        "evidence_type,evidence_ref,evidence_hash,recorded_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->candidate_id, -1, SQLITE_TRANSIENT);
    obs_bind_nullable(stmt, 4, input->injection_id);
    sqlite3_bind_text(stmt, 5, input->outcome, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, input->evidence_type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, input->evidence_ref, -1, SQLITE_TRANSIENT);
    obs_bind_nullable(stmt, 8, input->evidence_hash);
    sqlite3_bind_text(stmt, 9, timestamp, -1, SQLITE_TRANSIENT);
    bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return inserted ? CBM_STORE_OK : CBM_STORE_ERR;
}

int cbm_store_memory_observe_injection(cbm_store_t *s, const cbm_observe_injection_input_t *input) {
    static const char *const statuses[] = {"pass", "error"};
    static const char *const classifications[] = {"safe", "prompt_injection", "secret",
                                                  "pii",  "canary",           "rule_override"};
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (!db || !input || !input->event_id || !input->session_id || !input->candidate_id ||
        input->injection_index < 0 || !input->target ||
        !obs_content_hash_valid(input->content_hash) || input->token_count < 0 ||
        !obs_allowed(input->classifier_status, statuses, 2) ||
        !obs_allowed(input->classification, classifications, 6)) {
        return CBM_STORE_ERR;
    }
    if (!cbm_memory_security_injection_allowed(input->classifier_status, input->classification)) {
        return CBM_STORE_REJECTED;
    }
    int existing = obs_injection_existing(db, input);
    if (existing != CBM_STORE_NOT_FOUND) {
        return existing;
    }
    if (obs_candidate_hash(db, input->session_id, input->candidate_id, input->content_hash) !=
        CBM_STORE_OK) {
        return CBM_STORE_NOT_FOUND;
    }
    if (obs_id_exists(db, "memory_usage_attribution", input->event_id)) {
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_stmt *stmt = NULL;
    char timestamp[40];
    obs_timestamp(timestamp);
    const char *sql =
        "INSERT INTO context_injection(id,session_id,candidate_id,injection_index,target,"
        "content_hash,token_count,injected_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    sqlite3_bind_text(stmt, 1, input->event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, input->injection_index);
    sqlite3_bind_text(stmt, 5, input->target, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, input->content_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, input->token_count);
    sqlite3_bind_text(stmt, 8, timestamp, -1, SQLITE_TRANSIENT);
    bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return inserted ? CBM_STORE_OK : CBM_STORE_ERR;
}

/* Stage 7 uses SHA256 rather than the Stage 5 XXH3 identifiers because feedback
 * payload and audit hashes are evidence-bound external contracts. */
typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} stage7_sha256_ctx_t;

static const uint32_t STAGE7_SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t stage7_rotr(uint32_t value, uint32_t amount) {
    return (value >> amount) | (value << (32U - amount));
}

static void stage7_sha256_transform(stage7_sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t words[64];
    for (int i = 0; i < 16; i++) {
        int offset = i * 4;
        words[i] = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |
                   ((uint32_t)data[offset + 2] << 8) | data[offset + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 =
            stage7_rotr(words[i - 15], 7) ^ stage7_rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32_t s1 =
            stage7_rotr(words[i - 2], 17) ^ stage7_rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = stage7_rotr(e, 6) ^ stage7_rotr(e, 11) ^ stage7_rotr(e, 25);
        uint32_t choose = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + choose + STAGE7_SHA256_K[i] + words[i];
        uint32_t s0 = stage7_rotr(a, 2) ^ stage7_rotr(a, 13) ^ stage7_rotr(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void stage7_sha256_init(stage7_sha256_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
}

static void stage7_sha256_update(stage7_sha256_ctx_t *ctx, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            stage7_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void stage7_sha256_final(stage7_sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64)
            ctx->data[i++] = 0;
        stage7_sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56)
        ctx->data[i++] = 0;
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    for (int byte = 0; byte < 8; byte++) {
        ctx->data[63 - byte] = (uint8_t)(ctx->bitlen >> (byte * 8));
    }
    stage7_sha256_transform(ctx, ctx->data);
    for (i = 0; i < 8; i++) {
        hash[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)ctx->state[i];
    }
}

int cbm_stage7_sha256_hex(const void *data, size_t size, char out_hex[65]) {
    if (!out_hex || (!data && size > 0)) {
        return CBM_STORE_ERR;
    }
    stage7_sha256_ctx_t ctx;
    uint8_t hash[32];
    static const char HEX[] = "0123456789abcdef";
    stage7_sha256_init(&ctx);
    stage7_sha256_update(&ctx, (const uint8_t *)data, size);
    stage7_sha256_final(&ctx, hash);
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2] = HEX[hash[i] >> 4];
        out_hex[i * 2 + 1] = HEX[hash[i] & 0x0f];
    }
    out_hex[64] = '\0';
    return CBM_STORE_OK;
}

static bool stage7_sha256_string(const char *value) {
    if (!value || strlen(value) != 64)
        return false;
    for (int i = 0; i < 64; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool stage7_payload_matches(const char *payload, const char *expected_hash) {
    char actual[65];
    return payload && stage7_sha256_string(expected_hash) &&
           cbm_stage7_sha256_hex(payload, strlen(payload), actual) == CBM_STORE_OK &&
           strcmp(actual, expected_hash) == 0;
}

static bool stage7_nonempty(const char *value) {
    return value && value[0] && strlen(value) <= 255;
}

static bool stage7_trust_source_valid(const char *trust, const char *source) {
    static const char *const external[] = {"build", "test", "static_check", "runtime"};
    if (strcmp(trust, "external_verified") == 0)
        return obs_allowed(source, external, 4);
    if (strcmp(trust, "explicit_user") == 0)
        return strcmp(source, "user") == 0;
    return strcmp(trust, "model_self_report") == 0 && strcmp(source, "model") == 0;
}

static bool stage7_input_valid(const cbm_feedback_observe_input_t *input) {
    static const char *const task_types[] = {"build",   "test",      "static_check",
                                             "runtime", "user_task", "health_check"};
    static const char *const result_types[] = {
        "build", "test", "static_check", "runtime", "user_confirmation", "health_check"};
    static const char *const result_statuses[] = {"succeeded", "failed", "cancelled", "pending"};
    static const char *const trusts[] = {"external_verified", "explicit_user", "model_self_report"};
    static const char *const states[] = {"valid", "invalid", "expired", "withdrawn"};
    static const char *const actions[] = {"confirm", "reject", "correct", "withdraw"};
    if (!input ||
        strcmp(input->processing_mode ? input->processing_mode : "", "observe_only") != 0 ||
        !stage7_nonempty(input->project) || !stage7_nonempty(input->event_id) ||
        !stage7_nonempty(input->task_id) || !stage7_nonempty(input->session_id) ||
        !stage7_nonempty(input->candidate_id) || !stage7_nonempty(input->usage_id) ||
        !stage7_nonempty(input->result_id) || !stage7_nonempty(input->result_ref) ||
        !stage7_nonempty(input->evidence_id) || !stage7_nonempty(input->evidence_ref) ||
        !stage7_nonempty(input->algorithm_version) || input->config_version < 0 ||
        !obs_allowed(input->task_type, task_types, 6) ||
        !obs_allowed(input->result_type, result_types, 6) ||
        !obs_allowed(input->result_status, result_statuses, 4) ||
        !obs_allowed(input->evidence_trust, trusts, 3) ||
        !obs_allowed(input->evidence_state, states, 4) || !obs_allowed(input->action, actions, 4) ||
        !stage7_trust_source_valid(input->evidence_trust, input->evidence_source) ||
        !stage7_payload_matches(input->result_payload, input->result_hash) ||
        !stage7_payload_matches(input->evidence_payload, input->evidence_hash)) {
        return false;
    }
    bool compensating =
        strcmp(input->action, "correct") == 0 || strcmp(input->action, "withdraw") == 0;
    if (compensating != (input->supersedes_event_id && input->supersedes_event_id[0])) {
        return false;
    }
    if (strcmp(input->evidence_state, "valid") != 0 && !compensating) {
        return false;
    }
    return true;
}

static void stage7_json_nullable(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key,
                                 const char *value) {
    if (value) {
        yyjson_mut_obj_add_str(doc, object, key, value);
    } else {
        yyjson_mut_obj_add_null(doc, object, key);
    }
}

static char *stage7_canonical_payload(const cbm_feedback_observe_input_t *input) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", input->project);
    yyjson_mut_obj_add_str(doc, root, "processing_mode", input->processing_mode);
    yyjson_mut_obj_add_str(doc, root, "event_id", input->event_id);
    yyjson_mut_obj_add_str(doc, root, "task_id", input->task_id);
    yyjson_mut_obj_add_str(doc, root, "task_type", input->task_type);
    yyjson_mut_obj_add_str(doc, root, "session_id", input->session_id);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", input->candidate_id);
    stage7_json_nullable(doc, root, "injection_id", input->injection_id);
    yyjson_mut_obj_add_str(doc, root, "usage_id", input->usage_id);
    yyjson_mut_obj_add_str(doc, root, "result_id", input->result_id);
    yyjson_mut_obj_add_str(doc, root, "result_type", input->result_type);
    yyjson_mut_obj_add_str(doc, root, "result_status", input->result_status);
    yyjson_mut_obj_add_str(doc, root, "result_ref", input->result_ref);
    yyjson_mut_obj_add_str(doc, root, "result_hash", input->result_hash);
    yyjson_mut_obj_add_str(doc, root, "evidence_id", input->evidence_id);
    yyjson_mut_obj_add_str(doc, root, "evidence_trust", input->evidence_trust);
    yyjson_mut_obj_add_str(doc, root, "evidence_state", input->evidence_state);
    yyjson_mut_obj_add_str(doc, root, "evidence_source", input->evidence_source);
    yyjson_mut_obj_add_str(doc, root, "evidence_ref", input->evidence_ref);
    yyjson_mut_obj_add_str(doc, root, "evidence_hash", input->evidence_hash);
    yyjson_mut_obj_add_str(doc, root, "action", input->action);
    stage7_json_nullable(doc, root, "edge_id", input->edge_id);
    stage7_json_nullable(doc, root, "supersedes_event_id", input->supersedes_event_id);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int stage7_replay(sqlite3 *db, const char *event_id, const char *canonical_hash,
                         cbm_feedback_observe_result_t *out) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT canonical_payload_sha256,result_json FROM feedback_event WHERE event_id=?1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return CBM_STORE_NOT_FOUND;
    }
    const char *stored_hash = (const char *)sqlite3_column_text(stmt, 0);
    const char *stored_result = (const char *)sqlite3_column_text(stmt, 1);
    out->event_id = obs_dup(event_id);
    out->canonical_payload_sha256 = obs_dup(stored_hash);
    out->result_json = obs_dup(stored_result);
    bool allocated = out->event_id && out->canonical_payload_sha256 && out->result_json;
    bool exact = stored_hash && strcmp(stored_hash, canonical_hash) == 0;
    sqlite3_finalize(stmt);
    return !allocated ? CBM_STORE_ERR
                      : (exact ? CBM_STORE_REPLAYED : CBM_STORE_IDEMPOTENCY_CONFLICT);
}

static int stage7_chain_memory_item(sqlite3 *db, const cbm_feedback_observe_input_t *input,
                                    char **out_memory_item_id) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT c.memory_item_id FROM retrieval_session s JOIN retrieval_candidate c "
        "ON c.session_id=s.id JOIN memory_usage_attribution u ON u.session_id=s.id "
        "AND u.candidate_id=c.id WHERE s.id=?1 AND s.project_scope=?2 AND s.status='completed' "
        "AND c.id=?3 AND u.id=?4 AND u.injection_id IS ?5;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->candidate_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, input->usage_id, -1, SQLITE_TRANSIENT);
    obs_bind_nullable(stmt, 5, input->injection_id);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_memory_item_id = obs_dup((const char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW || !*out_memory_item_id)
        return CBM_STORE_NOT_FOUND;
    if (!input->edge_id)
        return CBM_STORE_OK;
    sql = "SELECT COUNT(*) FROM retrieval_edge_visit WHERE session_id=?1 AND memory_edge_id=?2 "
          "AND (from_candidate_id=?3 OR to_candidate_id=?3);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->edge_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->candidate_id, -1, SQLITE_TRANSIENT);
    bool visited = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return visited ? CBM_STORE_OK : CBM_STORE_NOT_FOUND;
}

static int stage7_superseded_evidence(sqlite3 *db, const cbm_feedback_observe_input_t *input,
                                      char **out_evidence_id) {
    if (!input->supersedes_event_id)
        return CBM_STORE_OK;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT evidence_id FROM feedback_event WHERE event_id=?1 AND task_id=?2 AND session_id=?3 "
        "AND candidate_id=?4;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->supersedes_event_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, input->candidate_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
        *out_evidence_id = obs_dup((const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW && *out_evidence_id ? CBM_STORE_OK : CBM_STORE_NOT_FOUND;
}

static int stage7_exec_bound(sqlite3 *db, const char *sql, const char *const *values, int count) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    for (int i = 0; i < count; i++)
        obs_bind_nullable(stmt, i + 1, values[i]);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int stage7_insert_foundation(sqlite3 *db, const cbm_feedback_observe_input_t *input,
                                    const char *superseded_evidence, const char *timestamp) {
    const char *task_values[] = {input->task_id, input->project, input->task_type, timestamp};
    if (stage7_exec_bound(db,
                          "INSERT OR IGNORE INTO memory_task(task_id,project,task_type,created_at) "
                          "VALUES(?1,?2,?3,?4);",
                          task_values, 4) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM memory_task WHERE task_id=?1 AND project=?2 AND "
                           "task_type=?3;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, input->task_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, input->project, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, input->task_type, -1, SQLITE_TRANSIENT);
    bool exact = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    if (!exact)
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    const char *link_values[] = {input->task_id, input->session_id, timestamp};
    if (stage7_exec_bound(db,
                          "INSERT OR IGNORE INTO memory_task_session(task_id,session_id,linked_at) "
                          "VALUES(?1,?2,?3);",
                          link_values, 3) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    const char *result_values[] = {
        input->result_id,  input->task_id,     input->result_type, input->result_status,
        input->result_ref, input->result_hash, timestamp};
    if (stage7_exec_bound(
            db,
            "INSERT OR IGNORE INTO memory_task_result(result_id,task_id,result_type,"
            "status,result_ref,result_hash,recorded_at) VALUES(?1,?2,?3,?4,?5,?6,?7);",
            result_values, 7) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM memory_task_result WHERE result_id=?1 AND "
                           "task_id=?2 AND result_type=?3 AND status=?4 AND result_ref=?5 AND "
                           "result_hash=?6;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    for (int i = 0; i < 6; i++)
        sqlite3_bind_text(stmt, i + 1, result_values[i], -1, SQLITE_TRANSIENT);
    exact = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    if (!exact)
        return CBM_STORE_IDEMPOTENCY_CONFLICT;
    const char *evidence_values[] = {input->evidence_id,    input->task_id,
                                     input->result_id,      input->evidence_trust,
                                     input->evidence_state, input->evidence_source,
                                     input->evidence_ref,   input->evidence_hash,
                                     superseded_evidence,   timestamp};
    if (stage7_exec_bound(
            db,
            "INSERT OR IGNORE INTO memory_evidence(evidence_id,task_id,result_id,"
            "trust_class,evidence_state,source_type,evidence_ref,evidence_hash,"
            "supersedes_evidence_id,created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);",
            evidence_values, 10) != CBM_STORE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM memory_evidence WHERE evidence_id=?1 AND "
                           "task_id=?2 AND result_id=?3 AND trust_class=?4 AND evidence_state=?5 "
                           "AND source_type=?6 AND evidence_ref=?7 AND evidence_hash=?8 AND "
                           "supersedes_evidence_id IS ?9;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    for (int i = 0; i < 9; i++)
        obs_bind_nullable(stmt, i + 1, evidence_values[i]);
    exact = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return exact ? CBM_STORE_OK : CBM_STORE_IDEMPOTENCY_CONFLICT;
}

static void stage7_reward(const cbm_feedback_observe_input_t *input, double *reward,
                          const char **status) {
    *reward = 0.0;
    *status = "attributed";
    if (strcmp(input->evidence_trust, "model_self_report") == 0) {
        *status = "pending_confirmation";
        return;
    }
    if (strcmp(input->action, "withdraw") == 0 || strcmp(input->evidence_state, "withdrawn") == 0) {
        *status = "withdrawn";
        return;
    }
    if (strcmp(input->evidence_state, "valid") != 0) {
        *status = "invalid";
        return;
    }
    double strength = strcmp(input->evidence_trust, "explicit_user") == 0 ? 1.0 : 0.75;
    if (strcmp(input->action, "confirm") == 0) {
        if (strcmp(input->result_status, "succeeded") == 0) {
            *reward = strength;
        } else {
            *status = "invalid";
        }
    } else if (strcmp(input->action, "reject") == 0) {
        *reward = -strength;
    } else if (strcmp(input->action, "correct") == 0) {
        *reward = strcmp(input->evidence_trust, "explicit_user") == 0 ? -0.75 : -0.50;
    }
}

static char *stage7_reward_report(const cbm_feedback_observe_input_t *input,
                                  const char *memory_item_id, const char *status,
                                  double node_contribution, double edge_contribution,
                                  double final_reward) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage7-reward-report/v1");
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_str(doc, root, "event_id", input->event_id);
    yyjson_mut_val *task = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, task, "task_id", input->task_id);
    yyjson_mut_obj_add_str(doc, task, "task_type", input->task_type);
    yyjson_mut_obj_add_str(doc, task, "result_id", input->result_id);
    yyjson_mut_obj_add_str(doc, task, "result_status", input->result_status);
    yyjson_mut_obj_add_val(doc, root, "task", task);
    yyjson_mut_obj_add_str(doc, root, "session_id", input->session_id);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", input->candidate_id);
    yyjson_mut_obj_add_str(doc, root, "memory_item_id", memory_item_id);
    stage7_json_nullable(doc, root, "injection_id", input->injection_id);
    yyjson_mut_obj_add_str(doc, root, "usage_id", input->usage_id);
    yyjson_mut_val *evidence = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, evidence, "evidence_id", input->evidence_id);
    yyjson_mut_obj_add_str(doc, evidence, "trust_class", input->evidence_trust);
    yyjson_mut_obj_add_str(doc, evidence, "state", input->evidence_state);
    yyjson_mut_obj_add_str(doc, evidence, "source", input->evidence_source);
    yyjson_mut_obj_add_str(doc, evidence, "ref", input->evidence_ref);
    yyjson_mut_obj_add_str(doc, evidence, "sha256", input->evidence_hash);
    yyjson_mut_obj_add_val(doc, root, "evidence", evidence);
    yyjson_mut_obj_add_str(doc, root, "action", input->action);
    yyjson_mut_val *edge = yyjson_mut_obj(doc);
    stage7_json_nullable(doc, edge, "edge_id", input->edge_id);
    yyjson_mut_obj_add_bool(doc, edge, "visited", input->edge_id != NULL);
    yyjson_mut_obj_add_real(doc, edge, "contribution", edge_contribution);
    yyjson_mut_obj_add_val(doc, root, "edge", edge);
    yyjson_mut_val *contributions = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, contributions, "evidence_gate", final_reward == 0.0 ? 0.0 : 1.0);
    yyjson_mut_obj_add_real(doc, contributions, "node", node_contribution);
    yyjson_mut_obj_add_real(doc, contributions, "edge", edge_contribution);
    yyjson_mut_obj_add_val(doc, root, "contributions", contributions);
    yyjson_mut_val *cap = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_real(doc, cap, "min", -1.0);
    yyjson_mut_obj_add_real(doc, cap, "max", 1.0);
    yyjson_mut_obj_add_val(doc, root, "cap", cap);
    yyjson_mut_obj_add_real(doc, root, "uncapped_reward", node_contribution + edge_contribution);
    yyjson_mut_obj_add_real(doc, root, "final_reward", final_reward);
    yyjson_mut_obj_add_str(doc, root, "processing_mode", "observe_only");
    yyjson_mut_obj_add_str(doc, root, "reward_mode", "shadow");
    yyjson_mut_obj_add_bool(doc, root, "long_term_state_written", false);
    yyjson_mut_obj_add_bool(doc, root, "edge_reinforcement_enabled", false);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static char *stage7_prefixed_hash_id(const char *prefix, const char *value) {
    char hash[65];
    if (cbm_stage7_sha256_hex(value, strlen(value), hash) != CBM_STORE_OK)
        return NULL;
    size_t size = strlen(prefix) + 64 + 1;
    char *result = malloc(size);
    if (result)
        snprintf(result, size, "%s%s", prefix, hash);
    return result;
}

static int stage7_audit_hash(int64_t sequence, const char *audit_id, const char *feedback_id,
                             const char *operation, const char *before_json, const char *after_json,
                             const char *algorithm_version, int config_version,
                             const char *prev_hash, const char *created_at, char out_hash[65]) {
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
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", algorithm_version);
    yyjson_mut_obj_add_int(doc, root, "config_version", config_version);
    yyjson_mut_obj_add_str(doc, root, "prev_hash", prev_hash);
    yyjson_mut_obj_add_str(doc, root, "created_at", created_at);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = cbm_stage7_sha256_hex(json, strlen(json), out_hash);
    free(json);
    return rc;
}

static int stage7_feedback_failure_point(void) {
    char value[32] = {0};
    cbm_safe_getenv("CBM_STAGE7_FEEDBACK_FAIL_AFTER", value, sizeof(value), NULL);
    return value[0] ? atoi(value) : 0;
}

static bool stage7_should_fail(int *executed, int failure_point) {
    (*executed)++;
    return failure_point > 0 && *executed == failure_point;
}

void cbm_store_memory_feedback_observe_result_free(cbm_feedback_observe_result_t *result) {
    if (!result)
        return;
    free(result->event_id);
    free(result->canonical_payload_sha256);
    free(result->result_json);
    memset(result, 0, sizeof(*result));
}

int cbm_store_memory_feedback_observe(cbm_store_t *s, const cbm_feedback_observe_input_t *input,
                                      cbm_feedback_observe_result_t *out) {
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || !out || !stage7_input_valid(input))
        return CBM_STORE_ERR;
    char *payload_json = stage7_canonical_payload(input);
    char canonical_hash[65];
    if (!payload_json ||
        cbm_stage7_sha256_hex(payload_json, strlen(payload_json), canonical_hash) != CBM_STORE_OK) {
        free(payload_json);
        return CBM_STORE_ERR;
    }
    if (cbm_store_begin(s) != CBM_STORE_OK) {
        free(payload_json);
        return CBM_STORE_ERR;
    }
    int replay = stage7_replay(db, input->event_id, canonical_hash, out);
    if (replay != CBM_STORE_NOT_FOUND) {
        int commit = cbm_store_commit(s);
        free(payload_json);
        return commit == CBM_STORE_OK ? replay : CBM_STORE_ERR;
    }
    char *memory_item_id = NULL;
    char *superseded_evidence = NULL;
    int rc = stage7_chain_memory_item(db, input, &memory_item_id);
    if (rc == CBM_STORE_OK)
        rc = stage7_superseded_evidence(db, input, &superseded_evidence);
    char timestamp[40];
    obs_timestamp(timestamp);
    if (rc == CBM_STORE_OK) {
        rc = stage7_insert_foundation(db, input, superseded_evidence, timestamp);
    }
    int failure_point = stage7_feedback_failure_point();
    int executed = 0;
    if (rc == CBM_STORE_OK && stage7_should_fail(&executed, failure_point))
        rc = CBM_STORE_ERR;
    double reward = 0.0;
    const char *attribution_status = NULL;
    stage7_reward(input, &reward, &attribution_status);
    if (reward < -1.0)
        reward = -1.0;
    if (reward > 1.0)
        reward = 1.0;
    double node_contribution = input->edge_id ? reward * 0.7 : reward;
    double edge_contribution = input->edge_id ? reward * 0.3 : 0.0;
    char *result_json =
        stage7_reward_report(input, memory_item_id ? memory_item_id : "", attribution_status,
                             node_contribution, edge_contribution, reward);
    if (rc == CBM_STORE_OK && !result_json)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        sqlite3_stmt *stmt = NULL;
        const char *sql =
            "INSERT INTO feedback_event(event_id,task_id,session_id,candidate_id,injection_id,"
            "usage_id,result_id,evidence_id,action,processing_mode,canonical_payload_sha256,"
            "payload_json,result_json,supersedes_event_id,algorithm_version,config_version,"
            "received_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
        } else {
            const char *values[] = {
                input->event_id,
                input->task_id,
                input->session_id,
                input->candidate_id,
                input->injection_id,
                input->usage_id,
                input->result_id,
                input->evidence_id,
                input->action,
                input->processing_mode,
                canonical_hash,
                payload_json,
                result_json,
                input->supersedes_event_id,
                input->algorithm_version,
            };
            for (int i = 0; i < 15; i++)
                obs_bind_nullable(stmt, i + 1, values[i]);
            sqlite3_bind_int(stmt, 16, input->config_version);
            sqlite3_bind_text(stmt, 17, timestamp, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
    }
    if (rc == CBM_STORE_OK && stage7_should_fail(&executed, failure_point))
        rc = CBM_STORE_ERR;
    char *attribution_id = stage7_prefixed_hash_id("attr-", input->event_id);
    if (rc == CBM_STORE_OK && !attribution_id)
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK) {
        sqlite3_stmt *stmt = NULL;
        const char *sql =
            "INSERT INTO feedback_attribution(attribution_id,feedback_event_id,task_id,session_id,"
            "candidate_id,memory_item_id,edge_id,evidence_id,node_contribution,edge_contribution,"
            "cap_min,cap_max,uncapped_reward,final_reward,attribution_status,explanation_json,"
            "created_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,-1.0,1.0,?11,?12,?13,?14,?15);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
        } else {
            sqlite3_bind_text(stmt, 1, attribution_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, input->event_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, input->task_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, input->session_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, input->candidate_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, memory_item_id, -1, SQLITE_TRANSIENT);
            obs_bind_nullable(stmt, 7, input->edge_id);
            sqlite3_bind_text(stmt, 8, input->evidence_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 9, node_contribution);
            sqlite3_bind_double(stmt, 10, edge_contribution);
            sqlite3_bind_double(stmt, 11, node_contribution + edge_contribution);
            sqlite3_bind_double(stmt, 12, reward);
            sqlite3_bind_text(stmt, 13, attribution_status, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 14, result_json, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 15, timestamp, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
    }
    if (rc == CBM_STORE_OK && stage7_should_fail(&executed, failure_point))
        rc = CBM_STORE_ERR;
    char *audit_id = stage7_prefixed_hash_id("audit-", input->event_id);
    int64_t sequence = 1;
    char prev_hash[65];
    memset(prev_hash, '0', 64);
    prev_hash[64] = '\0';
    if (rc == CBM_STORE_OK) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db,
                               "SELECT sequence_no,event_hash FROM plasticity_audit_event "
                               "ORDER BY sequence_no DESC LIMIT 1;",
                               -1, &stmt, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
        } else if (sqlite3_step(stmt) == SQLITE_ROW) {
            sequence = sqlite3_column_int64(stmt, 0) + 1;
            snprintf(prev_hash, sizeof(prev_hash), "%s", sqlite3_column_text(stmt, 1));
        }
        sqlite3_finalize(stmt);
    }
    const char *operation = strcmp(input->action, "withdraw") == 0
                                ? "withdrawal"
                                : (strcmp(input->action, "correct") == 0 ? "compensating_correction"
                                                                         : "observe_feedback");
    char event_hash[65];
    if (rc == CBM_STORE_OK &&
        (!audit_id ||
         stage7_audit_hash(sequence, audit_id, input->event_id, operation, "{}", result_json,
                           input->algorithm_version, input->config_version, prev_hash, timestamp,
                           event_hash) != CBM_STORE_OK)) {
        rc = CBM_STORE_ERR;
    }
    if (rc == CBM_STORE_OK) {
        sqlite3_stmt *stmt = NULL;
        const char *sql =
            "INSERT INTO plasticity_audit_event(sequence_no,event_id,feedback_event_id,operation,"
            "before_json,after_json,algorithm_version,config_version,prev_hash,event_hash,created_"
            "at) "
            "VALUES(?1,?2,?3,?4,'{}',?5,?6,?7,?8,?9,?10);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            rc = CBM_STORE_ERR;
        } else {
            sqlite3_bind_int64(stmt, 1, sequence);
            sqlite3_bind_text(stmt, 2, audit_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, input->event_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, operation, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, result_json, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, input->algorithm_version, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 7, input->config_version);
            sqlite3_bind_text(stmt, 8, prev_hash, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, event_hash, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 10, timestamp, -1, SQLITE_TRANSIENT);
            rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        }
        sqlite3_finalize(stmt);
    }
    if (rc == CBM_STORE_OK && stage7_should_fail(&executed, failure_point))
        rc = CBM_STORE_ERR;
    if (rc == CBM_STORE_OK)
        rc = cbm_store_commit(s);
    if (rc != CBM_STORE_OK) {
        cbm_store_rollback(s);
    } else {
        out->event_id = obs_dup(input->event_id);
        out->canonical_payload_sha256 = obs_dup(canonical_hash);
        out->result_json = obs_dup(result_json);
        if (!out->event_id || !out->canonical_payload_sha256 || !out->result_json) {
            cbm_store_memory_feedback_observe_result_free(out);
            rc = CBM_STORE_ERR;
        }
    }
    free(audit_id);
    free(attribution_id);
    free(result_json);
    free(superseded_evidence);
    free(memory_item_id);
    free(payload_json);
    return rc;
}

int cbm_store_memory_stage7_audit_verify(cbm_store_t *s, int *out_count) {
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (out_count)
        *out_count = 0;
    if (!db)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT sequence_no,event_id,feedback_event_id,operation,before_json,after_json,"
        "algorithm_version,config_version,prev_hash,event_hash,created_at FROM "
        "plasticity_audit_event ORDER BY sequence_no;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    char expected_prev[65];
    memset(expected_prev, '0', 64);
    expected_prev[64] = '\0';
    int count = 0;
    int64_t expected_sequence = 1;
    int result = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t sequence = sqlite3_column_int64(stmt, 0);
        const char *audit_id = (const char *)sqlite3_column_text(stmt, 1);
        const char *feedback_id = (const char *)sqlite3_column_text(stmt, 2);
        const char *operation = (const char *)sqlite3_column_text(stmt, 3);
        const char *before_json = (const char *)sqlite3_column_text(stmt, 4);
        const char *after_json = (const char *)sqlite3_column_text(stmt, 5);
        const char *algorithm = (const char *)sqlite3_column_text(stmt, 6);
        int config = sqlite3_column_int(stmt, 7);
        const char *prev_hash = (const char *)sqlite3_column_text(stmt, 8);
        const char *stored_hash = (const char *)sqlite3_column_text(stmt, 9);
        const char *created_at = (const char *)sqlite3_column_text(stmt, 10);
        char actual_hash[65];
        if (sequence != expected_sequence || !prev_hash || strcmp(prev_hash, expected_prev) != 0 ||
            stage7_audit_hash(sequence, audit_id, feedback_id, operation, before_json, after_json,
                              algorithm, config, prev_hash, created_at,
                              actual_hash) != CBM_STORE_OK ||
            !stored_hash || strcmp(stored_hash, actual_hash) != 0) {
            result = CBM_STORE_ERR;
            break;
        }
        snprintf(expected_prev, sizeof(expected_prev), "%s", stored_hash);
        expected_sequence++;
        count++;
    }
    sqlite3_finalize(stmt);
    if (result == CBM_STORE_OK && out_count)
        *out_count = count;
    return result;
}

/* ── Stage 8-A deterministic edge reinforcement replay ───────────── */

#define STAGE8_ALGORITHM "stage8-edge-reinforcement-v1"
#define STAGE8_CONFIG_VERSION 1
#define STAGE8_FIXTURE_PROJECT "stage6-fixture-stage8-g8-candidate-v1"
#define STAGE8_PRODUCTION_PROJECT "H-Codex_H-neuroplastic-main"
#define STAGE8_CANARY_MANIFEST_SCHEMA "stage8b2-production-canary-manifest/v1"
#define STAGE8_CANARY_MANIFEST_PATH                                                               \
    "H:\\Codex_H\\project\\experiments\\stage8b2-production-canary\\stage8b2-canary-manifest-v1." \
    "json"
#define STAGE8_CANARY_MEMORY_PRE_STATE \
    "9e3c5a916e96fb5754497a1acd64088b1a3613bb391cf7a4a6f8d92e9da2db5d"
#define STAGE8_CANARY_MEMORY_DB \
    "H:\\Codex_H\\runtime-data\\codex-mcp\\H-Codex_H-neuroplastic-main-memory.db"
#define STAGE8_COMPONENT "stage8_edge_reinforcement"
#define STAGE8_MIGRATION_NAME "edge_contribution_event_state_audit_v1"
#define STAGE8_MIGRATION_CHECKSUM "b3684685a4082bfcc80407c36297aa290b44c60075abe1a8c23e160e9f953fd7"
#define STAGE8_BASELINE_PPM 1000000LL
#define STAGE8_MIN_PPM 750000LL
#define STAGE8_MAX_PPM 1250000LL
#define STAGE8_SCALE_PPM 200000.0
#define STAGE8_EVENT_CAP 50000LL
#define STAGE8_EVIDENCE_CAP 50000LL
#define STAGE8_SOURCE_TASK_CAP 75000LL
#define STAGE8_PATH_CAP 75000LL
#define STAGE8_EDGE_TASK_CAP 50000LL
#define STAGE8_TASK_CAP 100000LL
#define STAGE8_EDGE_TOTAL_CAP 250000LL

typedef struct {
    char *id;
    char *feedback_id;
    char *attribution_id;
    char *edge_id;
    char *task_id;
    char *evidence_id;
    char *evidence_source;
    char *session_id;
    char *candidate_id;
    char *path_key;
    char *action;
    char *supersedes_id;
    char *canonical_hash;
    char *algorithm_version;
    char *created_at;
    int config_version;
    int64_t raw_delta_ppm;
    int64_t effective_delta_ppm;
    bool superseded;
    bool newly_recorded;
} stage8_event_t;

typedef struct {
    char *edge_id;
    char *last_event_id;
    char state_hash[65];
    int64_t pheromone_ppm;
    int64_t effective_delta_ppm;
    int success_count;
    int failure_count;
} stage8_state_t;

typedef struct {
    int event;
    int evidence;
    int source_task;
    int path;
    int edge_task;
    int task;
    int edge_total;
} stage8_cap_hits_t;

typedef struct {
    const char *type;
    const char *name;
    const char *sql;
} stage8_object_t;

static const stage8_object_t STAGE8_OBJECTS[] = {
    {"table", "stage8_component_ledger",
     "CREATE TABLE IF NOT EXISTS stage8_component_ledger("
     "component TEXT NOT NULL,version INTEGER NOT NULL CHECK(version > 0),name TEXT NOT NULL,"
     "checksum TEXT NOT NULL,applied_at TEXT NOT NULL,PRIMARY KEY(component,version),"
     "UNIQUE(component,name))"},
    {"table", "edge_contribution_event",
     "CREATE TABLE IF NOT EXISTS edge_contribution_event("
     "contribution_event_id TEXT PRIMARY KEY,feedback_event_id TEXT NOT NULL UNIQUE REFERENCES "
     "feedback_event(event_id) ON DELETE RESTRICT,feedback_attribution_id TEXT NOT NULL UNIQUE "
     "REFERENCES feedback_attribution(attribution_id) ON DELETE RESTRICT,edge_id TEXT NOT NULL "
     "REFERENCES memory_edge(id) ON DELETE RESTRICT,task_id TEXT NOT NULL REFERENCES "
     "memory_task(task_id) ON DELETE RESTRICT,evidence_id TEXT NOT NULL REFERENCES "
     "memory_evidence(evidence_id) ON DELETE RESTRICT,evidence_source TEXT NOT NULL CHECK("
     "evidence_source IN ('build','test','static_check','runtime','user','model')),session_id TEXT "
     "NOT NULL REFERENCES retrieval_session(id) ON DELETE RESTRICT,candidate_id TEXT NOT NULL "
     "REFERENCES retrieval_candidate(id) ON DELETE RESTRICT,path_key TEXT NOT NULL CHECK("
     "length(path_key) = 64 AND path_key NOT GLOB '*[^0-9a-f]*'),action TEXT NOT NULL CHECK("
     "action IN ('confirm','reject','correct','withdraw')),supersedes_contribution_event_id TEXT "
     "UNIQUE REFERENCES edge_contribution_event(contribution_event_id) ON DELETE RESTRICT,"
     "canonical_payload_sha256 TEXT NOT NULL CHECK(length(canonical_payload_sha256) = 64 AND "
     "canonical_payload_sha256 NOT GLOB '*[^0-9a-f]*'),raw_delta_ppm INTEGER NOT NULL CHECK("
     "raw_delta_ppm BETWEEN -1000000 AND 1000000),algorithm_version TEXT NOT NULL,config_version "
     "INTEGER NOT NULL CHECK(config_version >= 0),created_at TEXT NOT NULL,CHECK((action IN "
     "('correct','withdraw') AND supersedes_contribution_event_id IS NOT NULL) OR (action IN "
     "('confirm','reject') AND supersedes_contribution_event_id IS NULL)))"},
    {"table", "plastic_edge_state",
     "CREATE TABLE IF NOT EXISTS plastic_edge_state("
     "edge_id TEXT PRIMARY KEY REFERENCES memory_edge(id) ON DELETE RESTRICT,pheromone_ppm "
     "INTEGER NOT NULL CHECK(pheromone_ppm BETWEEN 750000 AND 1250000),success_count INTEGER NOT "
     "NULL CHECK(success_count >= 0),failure_count INTEGER NOT NULL CHECK(failure_count >= 0),"
     "effective_event_count INTEGER NOT NULL CHECK(effective_event_count = success_count + "
     "failure_count),last_contribution_event_id TEXT NOT NULL REFERENCES edge_contribution_event("
     "contribution_event_id) ON DELETE RESTRICT,algorithm_version TEXT NOT NULL,config_version "
     "INTEGER NOT NULL CHECK(config_version >= 0),state_sha256 TEXT NOT NULL CHECK(length("
     "state_sha256) = 64 AND state_sha256 NOT GLOB '*[^0-9a-f]*'),rebuilt_at TEXT NOT NULL)"},
    {"table", "edge_reinforcement_audit_event",
     "CREATE TABLE IF NOT EXISTS edge_reinforcement_audit_event("
     "sequence_no INTEGER PRIMARY KEY AUTOINCREMENT,audit_event_id TEXT NOT NULL UNIQUE,"
     "contribution_event_id TEXT NOT NULL UNIQUE REFERENCES edge_contribution_event("
     "contribution_event_id) ON DELETE RESTRICT,operation TEXT NOT NULL CHECK(operation IN "
     "('apply','compensate','withdraw')),before_state_sha256 TEXT NOT NULL CHECK(length("
     "before_state_sha256) = 64 AND before_state_sha256 NOT GLOB '*[^0-9a-f]*'),"
     "after_state_sha256 TEXT NOT NULL CHECK(length(after_state_sha256) = 64 AND "
     "after_state_sha256 NOT GLOB '*[^0-9a-f]*'),rebuild_sha256 TEXT NOT NULL CHECK(length("
     "rebuild_sha256) = 64 AND rebuild_sha256 NOT GLOB '*[^0-9a-f]*'),algorithm_version TEXT NOT "
     "NULL,config_version INTEGER NOT NULL CHECK(config_version >= 0),prev_hash TEXT NOT NULL "
     "CHECK(length(prev_hash) = 64 AND prev_hash NOT GLOB '*[^0-9a-f]*'),event_hash TEXT NOT NULL "
     "UNIQUE CHECK(length(event_hash) = 64 AND event_hash NOT GLOB '*[^0-9a-f]*'),created_at TEXT "
     "NOT NULL)"},
    {"index", "edge_contribution_edge_idx",
     "CREATE INDEX IF NOT EXISTS edge_contribution_edge_idx ON "
     "edge_contribution_event(edge_id, contribution_event_id)"},
    {"index", "edge_contribution_task_idx",
     "CREATE INDEX IF NOT EXISTS edge_contribution_task_idx ON "
     "edge_contribution_event(task_id, edge_id)"},
    {"index", "edge_contribution_path_idx",
     "CREATE INDEX IF NOT EXISTS edge_contribution_path_idx ON "
     "edge_contribution_event(path_key, edge_id)"},
    {"trigger", "edge_contribution_no_update",
     "CREATE TRIGGER IF NOT EXISTS edge_contribution_no_update BEFORE UPDATE ON "
     "edge_contribution_event BEGIN SELECT RAISE(ABORT, 'append-only'); END"},
    {"trigger", "edge_contribution_no_delete",
     "CREATE TRIGGER IF NOT EXISTS edge_contribution_no_delete BEFORE DELETE ON "
     "edge_contribution_event BEGIN SELECT RAISE(ABORT, 'append-only'); END"},
    {"trigger", "edge_reinforcement_audit_no_update",
     "CREATE TRIGGER IF NOT EXISTS edge_reinforcement_audit_no_update BEFORE UPDATE ON "
     "edge_reinforcement_audit_event BEGIN SELECT RAISE(ABORT, 'append-only'); END"},
    {"trigger", "edge_reinforcement_audit_no_delete",
     "CREATE TRIGGER IF NOT EXISTS edge_reinforcement_audit_no_delete BEFORE DELETE ON "
     "edge_reinforcement_audit_event BEGIN SELECT RAISE(ABORT, 'append-only'); END"},
};

static void stage8_event_clear(stage8_event_t *event) {
    if (!event)
        return;
    free(event->id);
    free(event->feedback_id);
    free(event->attribution_id);
    free(event->edge_id);
    free(event->task_id);
    free(event->evidence_id);
    free(event->evidence_source);
    free(event->session_id);
    free(event->candidate_id);
    free(event->path_key);
    free(event->action);
    free(event->supersedes_id);
    free(event->canonical_hash);
    free(event->algorithm_version);
    free(event->created_at);
    memset(event, 0, sizeof(*event));
}

static void stage8_events_free(stage8_event_t *events, int count) {
    for (int i = 0; i < count; i++)
        stage8_event_clear(&events[i]);
    free(events);
}

static void stage8_states_free(stage8_state_t *states, int count) {
    for (int i = 0; i < count; i++) {
        free(states[i].edge_id);
        free(states[i].last_event_id);
    }
    free(states);
}

static char *stage8_hash_id(const char *prefix, const char *left, const char *right) {
    size_t size = strlen(left ? left : "") + strlen(right ? right : "") + 2;
    char *canonical = malloc(size);
    if (!canonical)
        return NULL;
    snprintf(canonical, size, "%s\n%s", left ? left : "", right ? right : "");
    char hash[65];
    if (cbm_stage7_sha256_hex(canonical, strlen(canonical), hash) != CBM_STORE_OK) {
        free(canonical);
        return NULL;
    }
    free(canonical);
    size_t result_size = strlen(prefix) + 65;
    char *result = malloc(result_size);
    if (result)
        snprintf(result, result_size, "%s%s", prefix, hash);
    return result;
}

static char *stage8_plain_hash(const char *left, const char *right) {
    char *prefixed = stage8_hash_id("", left, right);
    return prefixed;
}

static int64_t stage8_clamp(int64_t value, int64_t minimum, int64_t maximum) {
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

static int64_t stage8_cap_increment(int64_t current, int64_t delta, int64_t cap) {
    return stage8_clamp(current + delta, -cap, cap) - current;
}

static bool stage8_fixture_active_guard(const cbm_edge_reinforcement_input_t *input) {
    if (!input || !input->project || strcmp(input->project, STAGE8_FIXTURE_PROJECT) != 0) {
        return false;
    }
    char enabled[8] = {0};
    cbm_safe_getenv("CBM_STAGE8_ACTIVE_FIXTURE", enabled, sizeof(enabled), NULL);
    return strcmp(enabled, "1") == 0;
}

static bool stage8_production_canary_guard(sqlite3 *db,
                                           const cbm_edge_reinforcement_input_t *input);

static int stage8_env_int(const char *name) {
    char value[32] = {0};
    cbm_safe_getenv(name, value, sizeof(value), NULL);
    return value[0] ? atoi(value) : 0;
}

static bool stage8_fail_step(int *step, int fail_after) {
    (*step)++;
    int crash_after = stage8_env_int("CBM_STAGE8_REINFORCEMENT_CRASH_AFTER");
    if (crash_after > 0 && *step == crash_after)
        _Exit(86);
    return fail_after > 0 && *step == fail_after;
}

static int stage8_schema_object_count(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT COUNT(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' AND "
                      "(name LIKE 'stage8_%' OR name LIKE 'edge_contribution_%' OR "
                      "name='plastic_edge_state' OR name LIKE 'edge_reinforcement_%');";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int count = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return count;
}

static bool stage8_schema_complete(sqlite3 *db) {
    int expected = (int)(sizeof(STAGE8_OBJECTS) / sizeof(STAGE8_OBJECTS[0]));
    if (stage8_schema_object_count(db) != expected)
        return false;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT COUNT(*) FROM stage8_component_ledger WHERE component=?1 AND version=1 AND "
        "name=?2 AND checksum=?3;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, STAGE8_COMPONENT, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, STAGE8_MIGRATION_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, STAGE8_MIGRATION_CHECKSUM, -1, SQLITE_STATIC);
    bool complete = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    sqlite3_finalize(stmt);
    return complete;
}

static bool stage8_manifest_string(yyjson_val *root, const char *key, const char *expected) {
    yyjson_val *value = yyjson_obj_get(root, key);
    const char *actual = value ? yyjson_get_str(value) : NULL;
    return actual && expected && strcmp(actual, expected) == 0;
}

static bool stage8_manifest_int(yyjson_val *root, const char *key, int expected) {
    yyjson_val *value = yyjson_obj_get(root, key);
    return value && yyjson_is_int(value) && yyjson_get_int(value) == expected;
}

static bool stage8_query_count(sqlite3 *db, const char *sql, int expected,
                               const char *const *values, int value_count) {
    sqlite3_stmt *stmt = NULL;
    if (!db || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return false;
    for (int i = 0; i < value_count; i++) {
        sqlite3_bind_text(stmt, i + 1, values[i], -1, SQLITE_TRANSIENT);
    }
    bool result = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == expected;
    sqlite3_finalize(stmt);
    return result;
}

static bool stage8_user_version_is(sqlite3 *db, int expected) {
    sqlite3_stmt *stmt = NULL;
    if (!db || sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) != SQLITE_OK)
        return false;
    bool result = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == expected;
    sqlite3_finalize(stmt);
    return result;
}

static bool stage8_production_canary_guard(sqlite3 *db,
                                           const cbm_edge_reinforcement_input_t *input) {
    if (!db || !input || !input->project || strcmp(input->project, STAGE8_PRODUCTION_PROJECT) != 0)
        return false;
    char enabled[8] = {0};
    cbm_safe_getenv("CBM_STAGE8_PRODUCTION_CANARY", enabled, sizeof(enabled), NULL);
    if (strcmp(enabled, "1") != 0)
        return false;
    yyjson_doc *manifest_doc = NULL;
    char manifest_env_path[4096] = {0};
    cbm_safe_getenv("CBM_STAGE8_PRODUCTION_CANARY_MANIFEST", manifest_env_path,
                    sizeof(manifest_env_path), NULL);
    if (strcmp(manifest_env_path, STAGE8_CANARY_MANIFEST_PATH) != 0)
        return false;
    char manifest_hash[65] = {0};
    cbm_safe_getenv("CBM_STAGE8_PRODUCTION_CANARY_SHA256", manifest_hash, sizeof(manifest_hash),
                    NULL);
    FILE *file = fopen(STAGE8_CANARY_MANIFEST_PATH, "rb");
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length <= 0 || length > 128 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *bytes = malloc((size_t)length + 1);
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        fclose(file);
        return false;
    }
    fclose(file);
    bytes[length] = '\0';
    char actual_hash[65] = {0};
    if (cbm_stage7_sha256_hex(bytes, (size_t)length, actual_hash) != CBM_STORE_OK ||
        strcmp(actual_hash, manifest_hash) != 0) {
        free(bytes);
        return false;
    }
    manifest_doc = yyjson_read((const char *)bytes, (size_t)length, 0);
    yyjson_val *root = manifest_doc ? yyjson_doc_get_root(manifest_doc) : NULL;
    bool fields_ok =
        root && yyjson_is_obj(root) &&
        stage8_manifest_string(root, "schema", STAGE8_CANARY_MANIFEST_SCHEMA) &&
        stage8_manifest_string(root, "project", STAGE8_PRODUCTION_PROJECT) &&
        stage8_manifest_string(root, "memory_db", STAGE8_CANARY_MEMORY_DB) &&
        stage8_manifest_string(root, "memory_pre_state_sha256", STAGE8_CANARY_MEMORY_PRE_STATE) &&
        stage8_manifest_int(root, "memory_user_version", 6) &&
        stage8_manifest_string(root, "edge_id", "medge-40150413779000-5") &&
        stage8_manifest_string(root, "source_event_id", "evt-40150413481500-3") &&
        stage8_manifest_string(root, "feedback_event_id",
                               "stage8-gate-c-production__20260718_094916122-feedback") &&
        stage8_manifest_string(
            root, "feedback_attribution_id",
            "attr-97a6a8c7fa171d11225dfbf08dcdf99b2b8b3e3274d0ba086f8388c79fbdf088") &&
        stage8_manifest_string(root, "task_id",
                               "stage8-gate-c-production__20260718_094916122-task") &&
        stage8_manifest_string(root, "evidence_id",
                               "stage8-gate-c-production__20260718_094916122-checklist-evidence") &&
        stage8_manifest_string(root, "session_id",
                               "stage8-gate-c-production__20260718_094916122-retrieve-v3") &&
        stage8_manifest_string(root, "candidate_id", "cand-874943e5a86f1e888ccf543d0d26856a") &&
        stage8_manifest_string(root, "visit_id", "visit-7614c2d463819813f8685661e38a3664") &&
        stage8_manifest_string(root, "usage_id",
                               "stage8-gate-c-production__20260718_094916122-c2-usage") &&
        stage8_manifest_string(root, "injection_id",
                               "stage8-gate-c-production__20260718_094916122-c2-injection") &&
        stage8_manifest_string(root, "canary_id", "stage8b2-production-canary-20260718-1") &&
        stage8_manifest_string(root, "algorithm_version", STAGE8_ALGORITHM) &&
        stage8_manifest_int(root, "config_version", STAGE8_CONFIG_VERSION) &&
        stage8_manifest_int(root, "max_edges", 1) &&
        stage8_manifest_string(root, "production_default_mode", "off");
    free(bytes);
    yyjson_doc_free(manifest_doc);
    if (!fields_ok)
        return false;

    const char *edge_values[] = {"medge-40150413779000-5", "evt-40150413481500-3"};
    const char *eligible_values[] = {
        "stage8-gate-c-production__20260718_094916122-feedback",
        "attr-97a6a8c7fa171d11225dfbf08dcdf99b2b8b3e3274d0ba086f8388c79fbdf088",
        "medge-40150413779000-5",
        "stage8-gate-c-production__20260718_094916122-task",
        "stage8-gate-c-production__20260718_094916122-checklist-evidence",
        "stage8-gate-c-production__20260718_094916122-retrieve-v3",
        "cand-874943e5a86f1e888ccf543d0d26856a",
        "stage8-gate-c-production__20260718_094916122-c2-usage",
        "stage8-gate-c-production__20260718_094916122-c2-injection",
        "visit-7614c2d463819813f8685661e38a3664"};
    bool exact_edge =
        stage8_query_count(db, "SELECT COUNT(*) FROM memory_edge;", 1, NULL, 0) &&
        stage8_query_count(
            db,
            "SELECT COUNT(*) FROM memory_edge WHERE id=?1 AND origin=?2 AND type='derived_from';",
            1, edge_values, 2);
    bool eligible = stage8_query_count(
        db,
        "SELECT COUNT(*) FROM feedback_event f JOIN feedback_attribution a ON "
        "a.feedback_event_id=f.event_id "
        "JOIN memory_evidence e ON e.evidence_id=f.evidence_id JOIN memory_usage_attribution u ON "
        "u.id=f.usage_id AND u.session_id=f.session_id AND u.candidate_id=f.candidate_id "
        "JOIN retrieval_edge_visit v ON v.session_id=f.session_id AND v.memory_edge_id=a.edge_id "
        "AND (v.from_candidate_id=f.candidate_id OR v.to_candidate_id=f.candidate_id) "
        "WHERE f.event_id=?1 AND a.attribution_id=?2 AND a.edge_id=?3 AND f.task_id=?4 AND "
        "f.evidence_id=?5 AND f.session_id=?6 AND f.candidate_id=?7 AND f.usage_id=?8 AND "
        "f.injection_id=?9 AND v.id=?10 AND u.outcome='used' AND v.visit_status='accepted' AND "
        "a.attribution_status='attributed' AND e.trust_class='external_verified' AND "
        "f.action='confirm';",
        1, eligible_values, 10);
    bool unique_edge_feedback = stage8_query_count(
        db, "SELECT COUNT(*) FROM feedback_attribution WHERE edge_id IS NOT NULL;", 1, NULL, 0);
    bool schema_present = stage8_schema_object_count(db) > 0;
    bool state_ok = !schema_present ? !schema_present : stage8_schema_complete(db);
    bool stage8_rows_ok = true;
    if (schema_present) {
        stage8_rows_ok =
            stage8_query_count(db, "SELECT COUNT(*) FROM edge_contribution_event;", 1, NULL, 0) &&
            stage8_query_count(db, "SELECT COUNT(*) FROM plastic_edge_state;", 1, NULL, 0) &&
            stage8_query_count(db, "SELECT COUNT(*) FROM edge_reinforcement_audit_event;", 1, NULL,
                               0) &&
            stage8_query_count(db,
                               "SELECT COUNT(*) FROM edge_contribution_event WHERE edge_id=?1 AND "
                               "feedback_event_id=?2 AND feedback_attribution_id=?3 AND task_id=?4 "
                               "AND evidence_id=?5 AND session_id=?6 AND candidate_id=?7 AND "
                               "action='confirm' AND algorithm_version=?8 AND config_version=1;",
                               1,
                               (const char *const[]){edge_values[0], eligible_values[0],
                                                     eligible_values[1], eligible_values[3],
                                                     eligible_values[4], eligible_values[5],
                                                     eligible_values[6], STAGE8_ALGORITHM},
                               8);
    }
    bool user_version = stage8_user_version_is(db, 6);
    return exact_edge && eligible && unique_edge_feedback && state_ok && stage8_rows_ok &&
           user_version;
}

static int stage8_prepare_schema(sqlite3 *db) {
    int existing = stage8_schema_object_count(db);
    if (existing < 0)
        return CBM_STORE_ERR;
    if (existing > 0)
        return stage8_schema_complete(db) ? CBM_STORE_OK : CBM_STORE_ERR;
    int fail_after = stage8_env_int("CBM_STAGE8_MIGRATION_FAIL_AFTER");
    int executed = 0;
    int count = (int)(sizeof(STAGE8_OBJECTS) / sizeof(STAGE8_OBJECTS[0]));
    for (int i = 0; i < count; i++) {
        if (sqlite3_exec(db, STAGE8_OBJECTS[i].sql, NULL, NULL, NULL) != SQLITE_OK ||
            stage8_fail_step(&executed, fail_after)) {
            return CBM_STORE_ERR;
        }
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO stage8_component_ledger(component,version,name,checksum,applied_at) "
        "VALUES(?1,1,?2,?3,strftime('%Y-%m-%dT%H:%M:%fZ','now'));";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, STAGE8_COMPONENT, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, STAGE8_MIGRATION_NAME, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, STAGE8_MIGRATION_CHECKSUM, -1, SQLITE_STATIC);
    bool inserted = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!inserted || stage8_fail_step(&executed, fail_after))
        return CBM_STORE_ERR;
    return stage8_schema_complete(db) ? CBM_STORE_OK : CBM_STORE_ERR;
}

static int stage8_event_compare(const void *left, const void *right) {
    const stage8_event_t *a = left;
    const stage8_event_t *b = right;
    return strcmp(a->id ? a->id : "", b->id ? b->id : "");
}

static int stage8_state_compare(const void *left, const void *right) {
    const stage8_state_t *a = left;
    const stage8_state_t *b = right;
    return strcmp(a->edge_id ? a->edge_id : "", b->edge_id ? b->edge_id : "");
}

static char *stage8_event_canonical(const stage8_event_t *event) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "action", event->action);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", event->algorithm_version);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", event->candidate_id);
    yyjson_mut_obj_add_int(doc, root, "config_version", event->config_version);
    yyjson_mut_obj_add_str(doc, root, "contribution_event_id", event->id);
    yyjson_mut_obj_add_str(doc, root, "edge_id", event->edge_id);
    yyjson_mut_obj_add_str(doc, root, "evidence_id", event->evidence_id);
    yyjson_mut_obj_add_str(doc, root, "evidence_source", event->evidence_source);
    yyjson_mut_obj_add_str(doc, root, "feedback_attribution_id", event->attribution_id);
    yyjson_mut_obj_add_str(doc, root, "feedback_event_id", event->feedback_id);
    yyjson_mut_obj_add_str(doc, root, "path_key", event->path_key);
    yyjson_mut_obj_add_sint(doc, root, "raw_delta_ppm", event->raw_delta_ppm);
    yyjson_mut_obj_add_str(doc, root, "session_id", event->session_id);
    stage7_json_nullable(doc, root, "supersedes_contribution_event_id", event->supersedes_id);
    yyjson_mut_obj_add_str(doc, root, "task_id", event->task_id);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

static int stage8_load_stage7_events(sqlite3 *db, const cbm_edge_reinforcement_input_t *input,
                                     stage8_event_t **out_events, int *out_count,
                                     int *out_ignored) {
    *out_events = NULL;
    *out_count = 0;
    *out_ignored = 0;
    sqlite3_stmt *total_stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM feedback_attribution WHERE edge_id IS NOT NULL;",
                           -1, &total_stmt, NULL) != SQLITE_OK) {
        return CBM_STORE_ERR;
    }
    int total = sqlite3_step(total_stmt) == SQLITE_ROW ? sqlite3_column_int(total_stmt, 0) : 0;
    sqlite3_finalize(total_stmt);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT f.event_id,a.attribution_id,a.edge_id,f.task_id,f.evidence_id,e.source_type,"
        "f.session_id,f.candidate_id,f.action,f.supersedes_event_id,a.edge_contribution,"
        "f.received_at FROM feedback_event f JOIN feedback_attribution a ON "
        "a.feedback_event_id=f.event_id JOIN memory_evidence e ON e.evidence_id=f.evidence_id "
        "JOIN memory_usage_attribution u ON u.id=f.usage_id AND u.session_id=f.session_id AND "
        "u.candidate_id=f.candidate_id WHERE a.edge_id IS NOT NULL AND u.outcome='used' AND "
        "a.attribution_status IN ('attributed','withdrawn') AND "
        "(abs(a.final_reward)>0.000000000001 OR f.action='withdraw') AND EXISTS(SELECT 1 FROM "
        "retrieval_edge_visit v WHERE v.session_id=f.session_id AND v.memory_edge_id=a.edge_id "
        "AND (v.from_candidate_id=f.candidate_id OR v.to_candidate_id=f.candidate_id)) "
        "ORDER BY f.event_id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int capacity = total > 0 ? total : 1;
    stage8_event_t *events = calloc((size_t)capacity, sizeof(*events));
    if (!events) {
        sqlite3_finalize(stmt);
        return CBM_STORE_ERR;
    }
    int count = 0;
    int rc = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        stage8_event_t *event = &events[count];
        const char *feedback = (const char *)sqlite3_column_text(stmt, 0);
        const char *edge = (const char *)sqlite3_column_text(stmt, 2);
        const char *supersedes_feedback = (const char *)sqlite3_column_text(stmt, 9);
        event->id = stage8_hash_id("contrib-", feedback, edge);
        event->feedback_id = obs_dup(feedback);
        event->attribution_id = obs_dup((const char *)sqlite3_column_text(stmt, 1));
        event->edge_id = obs_dup(edge);
        event->task_id = obs_dup((const char *)sqlite3_column_text(stmt, 3));
        event->evidence_id = obs_dup((const char *)sqlite3_column_text(stmt, 4));
        event->evidence_source = obs_dup((const char *)sqlite3_column_text(stmt, 5));
        event->session_id = obs_dup((const char *)sqlite3_column_text(stmt, 6));
        event->candidate_id = obs_dup((const char *)sqlite3_column_text(stmt, 7));
        event->action = obs_dup((const char *)sqlite3_column_text(stmt, 8));
        event->supersedes_id =
            supersedes_feedback ? stage8_hash_id("contrib-", supersedes_feedback, edge) : NULL;
        event->path_key = stage8_plain_hash(event->session_id, event->candidate_id);
        event->raw_delta_ppm =
            strcmp(event->action, "withdraw") == 0
                ? 0
                : (int64_t)llround(sqlite3_column_double(stmt, 10) * STAGE8_SCALE_PPM);
        event->algorithm_version = obs_dup(input->algorithm_version);
        event->config_version = input->config_version;
        event->created_at = obs_dup((const char *)sqlite3_column_text(stmt, 11));
        char *canonical = stage8_event_canonical(event);
        char hash[65];
        if (!event->id || !event->feedback_id || !event->attribution_id || !event->edge_id ||
            !event->task_id || !event->evidence_id || !event->evidence_source ||
            !event->session_id || !event->candidate_id || !event->action || !event->path_key ||
            !event->algorithm_version || !event->created_at || !canonical ||
            cbm_stage7_sha256_hex(canonical, strlen(canonical), hash) != CBM_STORE_OK) {
            free(canonical);
            rc = CBM_STORE_ERR;
            break;
        }
        free(canonical);
        event->canonical_hash = obs_dup(hash);
        if (!event->canonical_hash ||
            ((strcmp(event->action, "correct") == 0 || strcmp(event->action, "withdraw") == 0) !=
             (event->supersedes_id != NULL))) {
            rc = CBM_STORE_ERR;
            break;
        }
        count++;
    }
    sqlite3_finalize(stmt);
    if (rc != CBM_STORE_OK) {
        stage8_events_free(events, count + 1);
        return rc;
    }
    qsort(events, (size_t)count, sizeof(*events), stage8_event_compare);
    *out_events = events;
    *out_count = count;
    *out_ignored = total - count;
    return CBM_STORE_OK;
}

static int stage8_load_ledger(sqlite3 *db, stage8_event_t **out_events, int *out_count) {
    *out_events = NULL;
    *out_count = 0;
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM edge_contribution_event;", -1, &count_stmt,
                           NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    int capacity = sqlite3_step(count_stmt) == SQLITE_ROW ? sqlite3_column_int(count_stmt, 0) : 0;
    sqlite3_finalize(count_stmt);
    stage8_event_t *events = calloc((size_t)(capacity > 0 ? capacity : 1), sizeof(*events));
    if (!events)
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT contribution_event_id,feedback_event_id,feedback_attribution_id,edge_id,task_id,"
        "evidence_id,evidence_source,session_id,candidate_id,path_key,action,"
        "supersedes_contribution_event_id,canonical_payload_sha256,raw_delta_ppm,"
        "algorithm_version,config_version,created_at FROM edge_contribution_event "
        "ORDER BY contribution_event_id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(events);
        return CBM_STORE_ERR;
    }
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && count < capacity) {
        stage8_event_t *event = &events[count++];
        event->id = obs_dup((const char *)sqlite3_column_text(stmt, 0));
        event->feedback_id = obs_dup((const char *)sqlite3_column_text(stmt, 1));
        event->attribution_id = obs_dup((const char *)sqlite3_column_text(stmt, 2));
        event->edge_id = obs_dup((const char *)sqlite3_column_text(stmt, 3));
        event->task_id = obs_dup((const char *)sqlite3_column_text(stmt, 4));
        event->evidence_id = obs_dup((const char *)sqlite3_column_text(stmt, 5));
        event->evidence_source = obs_dup((const char *)sqlite3_column_text(stmt, 6));
        event->session_id = obs_dup((const char *)sqlite3_column_text(stmt, 7));
        event->candidate_id = obs_dup((const char *)sqlite3_column_text(stmt, 8));
        event->path_key = obs_dup((const char *)sqlite3_column_text(stmt, 9));
        event->action = obs_dup((const char *)sqlite3_column_text(stmt, 10));
        event->supersedes_id = sqlite3_column_type(stmt, 11) == SQLITE_NULL
                                   ? NULL
                                   : obs_dup((const char *)sqlite3_column_text(stmt, 11));
        event->canonical_hash = obs_dup((const char *)sqlite3_column_text(stmt, 12));
        event->raw_delta_ppm = sqlite3_column_int64(stmt, 13);
        event->algorithm_version = obs_dup((const char *)sqlite3_column_text(stmt, 14));
        event->config_version = sqlite3_column_int(stmt, 15);
        event->created_at = obs_dup((const char *)sqlite3_column_text(stmt, 16));
    }
    sqlite3_finalize(stmt);
    *out_events = events;
    *out_count = count;
    return CBM_STORE_OK;
}

static int stage8_find_event(stage8_event_t *events, int count, const char *id) {
    for (int i = 0; i < count; i++) {
        if (events[i].id && id && strcmp(events[i].id, id) == 0)
            return i;
    }
    return -1;
}

static int64_t stage8_prior_sum(stage8_event_t *events, int upto, const stage8_event_t *current,
                                int group) {
    int64_t sum = 0;
    for (int i = 0; i < upto; i++) {
        stage8_event_t *prior = &events[i];
        if (prior->superseded)
            continue;
        bool same = false;
        if (group == 0)
            same = strcmp(prior->evidence_id, current->evidence_id) == 0;
        if (group == 1)
            same = strcmp(prior->evidence_source, current->evidence_source) == 0 &&
                   strcmp(prior->task_id, current->task_id) == 0;
        if (group == 2)
            same = strcmp(prior->path_key, current->path_key) == 0;
        if (group == 3)
            same = strcmp(prior->edge_id, current->edge_id) == 0 &&
                   strcmp(prior->task_id, current->task_id) == 0;
        if (group == 4)
            same = strcmp(prior->task_id, current->task_id) == 0;
        if (group == 5)
            same = strcmp(prior->edge_id, current->edge_id) == 0;
        if (same)
            sum += prior->effective_delta_ppm;
    }
    return sum;
}

static int stage8_state_hash(stage8_state_t *state) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "edge_id", state->edge_id);
    yyjson_mut_obj_add_sint(doc, root, "effective_delta_ppm", state->effective_delta_ppm);
    yyjson_mut_obj_add_int(doc, root, "effective_event_count",
                           state->success_count + state->failure_count);
    yyjson_mut_obj_add_int(doc, root, "failure_count", state->failure_count);
    yyjson_mut_obj_add_str(doc, root, "last_contribution_event_id", state->last_event_id);
    yyjson_mut_obj_add_sint(doc, root, "pheromone_ppm", state->pheromone_ppm);
    yyjson_mut_obj_add_int(doc, root, "success_count", state->success_count);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = cbm_stage7_sha256_hex(json, strlen(json), state->state_hash);
    free(json);
    return rc;
}

static int stage8_states_digest(stage8_state_t *states, int count, char out_hash[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *array = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !array) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, array);
    for (int i = 0; i < count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "edge_id", states[i].edge_id);
        yyjson_mut_obj_add_sint(doc, item, "effective_delta_ppm", states[i].effective_delta_ppm);
        yyjson_mut_obj_add_int(doc, item, "failure_count", states[i].failure_count);
        yyjson_mut_obj_add_str(doc, item, "last_event_id", states[i].last_event_id);
        yyjson_mut_obj_add_sint(doc, item, "pheromone_ppm", states[i].pheromone_ppm);
        yyjson_mut_obj_add_int(doc, item, "success_count", states[i].success_count);
        yyjson_mut_obj_add_str(doc, item, "state_sha256", states[i].state_hash);
        yyjson_mut_arr_add_val(array, item);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = cbm_stage7_sha256_hex(json, strlen(json), out_hash);
    free(json);
    return rc;
}

static int stage8_aggregate(stage8_event_t *events, int count, stage8_state_t **out_states,
                            int *out_count, stage8_cap_hits_t *hits, char digest[65]) {
    *out_states = NULL;
    *out_count = 0;
    memset(hits, 0, sizeof(*hits));
    for (int i = 0; i < count; i++) {
        events[i].superseded = false;
        events[i].effective_delta_ppm = 0;
    }
    for (int i = 0; i < count; i++) {
        if (!events[i].supersedes_id)
            continue;
        int parent = stage8_find_event(events, count, events[i].supersedes_id);
        if (parent < 0 || strcmp(events[parent].edge_id, events[i].edge_id) != 0 ||
            events[parent].superseded) {
            return CBM_STORE_ERR;
        }
        events[parent].superseded = true;
    }
    for (int i = 0; i < count; i++) {
        if (events[i].superseded)
            continue;
        int64_t delta = stage8_clamp(events[i].raw_delta_ppm, -STAGE8_EVENT_CAP, STAGE8_EVENT_CAP);
        if (delta != events[i].raw_delta_ppm)
            hits->event++;
        int64_t limited = stage8_cap_increment(stage8_prior_sum(events, i, &events[i], 0), delta,
                                               STAGE8_EVIDENCE_CAP);
        if (limited != delta)
            hits->evidence++;
        delta = limited;
        limited = stage8_cap_increment(stage8_prior_sum(events, i, &events[i], 1), delta,
                                       STAGE8_SOURCE_TASK_CAP);
        if (limited != delta)
            hits->source_task++;
        delta = limited;
        limited = stage8_cap_increment(stage8_prior_sum(events, i, &events[i], 2), delta,
                                       STAGE8_PATH_CAP);
        if (limited != delta)
            hits->path++;
        delta = limited;
        limited = stage8_cap_increment(stage8_prior_sum(events, i, &events[i], 3), delta,
                                       STAGE8_EDGE_TASK_CAP);
        if (limited != delta)
            hits->edge_task++;
        delta = limited;
        limited = stage8_cap_increment(stage8_prior_sum(events, i, &events[i], 4), delta,
                                       STAGE8_TASK_CAP);
        if (limited != delta)
            hits->task++;
        delta = limited;
        limited = stage8_cap_increment(stage8_prior_sum(events, i, &events[i], 5), delta,
                                       STAGE8_EDGE_TOTAL_CAP);
        if (limited != delta)
            hits->edge_total++;
        events[i].effective_delta_ppm = limited;
    }
    stage8_state_t *states = calloc((size_t)(count > 0 ? count : 1), sizeof(*states));
    if (!states)
        return CBM_STORE_ERR;
    int state_count = 0;
    for (int i = 0; i < count; i++) {
        if (events[i].superseded)
            continue;
        int index = -1;
        for (int j = 0; j < state_count; j++) {
            if (strcmp(states[j].edge_id, events[i].edge_id) == 0)
                index = j;
        }
        if (index < 0) {
            index = state_count++;
            states[index].edge_id = obs_dup(events[i].edge_id);
            states[index].last_event_id = obs_dup(events[i].id);
        } else if (strcmp(events[i].id, states[index].last_event_id) > 0) {
            free(states[index].last_event_id);
            states[index].last_event_id = obs_dup(events[i].id);
        }
        if (!states[index].edge_id || !states[index].last_event_id) {
            stage8_states_free(states, state_count);
            return CBM_STORE_ERR;
        }
        states[index].effective_delta_ppm += events[i].effective_delta_ppm;
        states[index].success_count += events[i].effective_delta_ppm > 0;
        states[index].failure_count += events[i].effective_delta_ppm < 0;
    }
    qsort(states, (size_t)state_count, sizeof(*states), stage8_state_compare);
    for (int i = 0; i < state_count; i++) {
        states[i].pheromone_ppm = stage8_clamp(STAGE8_BASELINE_PPM + states[i].effective_delta_ppm,
                                               STAGE8_MIN_PPM, STAGE8_MAX_PPM);
        if (stage8_state_hash(&states[i]) != CBM_STORE_OK) {
            stage8_states_free(states, state_count);
            return CBM_STORE_ERR;
        }
    }
    if (stage8_states_digest(states, state_count, digest) != CBM_STORE_OK) {
        stage8_states_free(states, state_count);
        return CBM_STORE_ERR;
    }
    *out_states = states;
    *out_count = state_count;
    return CBM_STORE_OK;
}

static int stage8_existing_event(sqlite3 *db, const stage8_event_t *event) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT canonical_payload_sha256 FROM edge_contribution_event WHERE "
                           "contribution_event_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    sqlite3_bind_text(stmt, 1, event->id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    int result = CBM_STORE_NOT_FOUND;
    if (step == SQLITE_ROW) {
        const char *stored = (const char *)sqlite3_column_text(stmt, 0);
        result = stored && strcmp(stored, event->canonical_hash) == 0
                     ? CBM_STORE_REPLAYED
                     : CBM_STORE_IDEMPOTENCY_CONFLICT;
    }
    sqlite3_finalize(stmt);
    return result;
}

static bool stage8_parent_exists(sqlite3 *db, const char *id) {
    if (!id)
        return true;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM edge_contribution_event WHERE "
                           "contribution_event_id=?1;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_TRANSIENT);
    bool exists = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return exists;
}

static int stage8_insert_event(sqlite3 *db, const stage8_event_t *event) {
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO edge_contribution_event(contribution_event_id,feedback_event_id,"
        "feedback_attribution_id,edge_id,task_id,evidence_id,evidence_source,session_id,"
        "candidate_id,path_key,action,supersedes_contribution_event_id,"
        "canonical_payload_sha256,raw_delta_ppm,algorithm_version,config_version,created_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    const char *values[] = {event->id,
                            event->feedback_id,
                            event->attribution_id,
                            event->edge_id,
                            event->task_id,
                            event->evidence_id,
                            event->evidence_source,
                            event->session_id,
                            event->candidate_id,
                            event->path_key,
                            event->action,
                            event->supersedes_id,
                            event->canonical_hash};
    for (int i = 0; i < 13; i++)
        obs_bind_nullable(stmt, i + 1, values[i]);
    sqlite3_bind_int64(stmt, 14, event->raw_delta_ppm);
    sqlite3_bind_text(stmt, 15, event->algorithm_version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 16, event->config_version);
    sqlite3_bind_text(stmt, 17, event->created_at, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
    sqlite3_finalize(stmt);
    return rc;
}

static int stage8_materialize(sqlite3 *db, stage8_event_t *events, int count, int *recorded,
                              int *replayed, int *fail_step, int fail_after) {
    bool *done = calloc((size_t)(count > 0 ? count : 1), sizeof(*done));
    if (!done)
        return CBM_STORE_ERR;
    int remaining = count;
    int rc = CBM_STORE_OK;
    while (remaining > 0 && rc == CBM_STORE_OK) {
        bool progressed = false;
        for (int i = 0; i < count; i++) {
            if (done[i])
                continue;
            int existing = stage8_existing_event(db, &events[i]);
            if (existing == CBM_STORE_REPLAYED) {
                (*replayed)++;
                done[i] = true;
                remaining--;
                progressed = true;
                continue;
            }
            if (existing == CBM_STORE_IDEMPOTENCY_CONFLICT || existing == CBM_STORE_ERR) {
                rc = existing;
                break;
            }
            if (!stage8_parent_exists(db, events[i].supersedes_id))
                continue;
            if (stage8_insert_event(db, &events[i]) != CBM_STORE_OK ||
                stage8_fail_step(fail_step, fail_after)) {
                rc = CBM_STORE_ERR;
                break;
            }
            events[i].newly_recorded = true;
            (*recorded)++;
            done[i] = true;
            remaining--;
            progressed = true;
        }
        if (!progressed && rc == CBM_STORE_OK)
            rc = CBM_STORE_ERR;
    }
    free(done);
    return rc;
}

static int stage8_current_state_digest(sqlite3 *db, char out_hash[65]) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT edge_id,pheromone_ppm,success_count,failure_count,"
                           "last_contribution_event_id,state_sha256 FROM plastic_edge_state "
                           "ORDER BY edge_id;",
                           -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *array = doc ? yyjson_mut_arr(doc) : NULL;
    if (!doc || !array) {
        sqlite3_finalize(stmt);
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, array);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        yyjson_mut_val *item = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_strcpy(doc, item, (const char *)sqlite3_column_text(stmt, 0));
        yyjson_mut_arr_add_sint(doc, item, sqlite3_column_int64(stmt, 1));
        yyjson_mut_arr_add_int(doc, item, sqlite3_column_int(stmt, 2));
        yyjson_mut_arr_add_int(doc, item, sqlite3_column_int(stmt, 3));
        yyjson_mut_arr_add_strcpy(doc, item, (const char *)sqlite3_column_text(stmt, 4));
        yyjson_mut_arr_add_strcpy(doc, item, (const char *)sqlite3_column_text(stmt, 5));
        yyjson_mut_arr_add_val(array, item);
    }
    sqlite3_finalize(stmt);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = cbm_stage7_sha256_hex(json, strlen(json), out_hash);
    free(json);
    return rc;
}

static int stage8_replace_states(sqlite3 *db, stage8_state_t *states, int count,
                                 const cbm_edge_reinforcement_input_t *input, const char *timestamp,
                                 int *fail_step, int fail_after) {
    if (sqlite3_exec(db, "DELETE FROM plastic_edge_state;", NULL, NULL, NULL) != SQLITE_OK ||
        stage8_fail_step(fail_step, fail_after))
        return CBM_STORE_ERR;
    const char *sql =
        "INSERT INTO plastic_edge_state(edge_id,pheromone_ppm,success_count,failure_count,"
        "effective_event_count,last_contribution_event_id,algorithm_version,config_version,"
        "state_sha256,rebuilt_at) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
    for (int i = 0; i < count; i++) {
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            return CBM_STORE_ERR;
        sqlite3_bind_text(stmt, 1, states[i].edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, states[i].pheromone_ppm);
        sqlite3_bind_int(stmt, 3, states[i].success_count);
        sqlite3_bind_int(stmt, 4, states[i].failure_count);
        sqlite3_bind_int(stmt, 5, states[i].success_count + states[i].failure_count);
        sqlite3_bind_text(stmt, 6, states[i].last_event_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, input->algorithm_version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, input->config_version);
        sqlite3_bind_text(stmt, 9, states[i].state_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, timestamp, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        if (rc != CBM_STORE_OK || stage8_fail_step(fail_step, fail_after))
            return CBM_STORE_ERR;
    }
    return CBM_STORE_OK;
}

static int stage8_audit_hash(int64_t sequence, const char *audit_id, const char *contribution_id,
                             const char *operation, const char *before_hash, const char *after_hash,
                             const char *rebuild_hash, const char *algorithm, int config,
                             const char *prev_hash, const char *created_at, char out_hash[65]) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return CBM_STORE_ERR;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_sint(doc, root, "sequence_no", sequence);
    yyjson_mut_obj_add_str(doc, root, "audit_event_id", audit_id);
    yyjson_mut_obj_add_str(doc, root, "contribution_event_id", contribution_id);
    yyjson_mut_obj_add_str(doc, root, "operation", operation);
    yyjson_mut_obj_add_str(doc, root, "before_state_sha256", before_hash);
    yyjson_mut_obj_add_str(doc, root, "after_state_sha256", after_hash);
    yyjson_mut_obj_add_str(doc, root, "rebuild_sha256", rebuild_hash);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", algorithm);
    yyjson_mut_obj_add_int(doc, root, "config_version", config);
    yyjson_mut_obj_add_str(doc, root, "prev_hash", prev_hash);
    yyjson_mut_obj_add_str(doc, root, "created_at", created_at);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (!json)
        return CBM_STORE_ERR;
    int rc = cbm_stage7_sha256_hex(json, strlen(json), out_hash);
    free(json);
    return rc;
}

static int stage8_append_audits(sqlite3 *db, stage8_event_t *events, int count,
                                const cbm_edge_reinforcement_input_t *input,
                                const char *before_hash, const char *after_hash, int *fail_step,
                                int fail_after) {
    int64_t sequence = 1;
    char prev_hash[65];
    memset(prev_hash, '0', 64);
    prev_hash[64] = '\0';
    sqlite3_stmt *head = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT sequence_no,event_hash FROM edge_reinforcement_audit_event "
                           "ORDER BY sequence_no DESC LIMIT 1;",
                           -1, &head, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    if (sqlite3_step(head) == SQLITE_ROW) {
        sequence = sqlite3_column_int64(head, 0) + 1;
        snprintf(prev_hash, sizeof(prev_hash), "%s", sqlite3_column_text(head, 1));
    }
    sqlite3_finalize(head);
    const char *sql =
        "INSERT INTO edge_reinforcement_audit_event(sequence_no,audit_event_id,"
        "contribution_event_id,operation,before_state_sha256,after_state_sha256,rebuild_sha256,"
        "algorithm_version,config_version,prev_hash,event_hash,created_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12);";
    for (int i = 0; i < count; i++) {
        if (!events[i].newly_recorded)
            continue;
        char *audit_id = stage8_hash_id("reinforce-audit-", events[i].id, "v1");
        const char *operation = strcmp(events[i].action, "withdraw") == 0
                                    ? "withdraw"
                                    : (events[i].supersedes_id ? "compensate" : "apply");
        char event_hash[65];
        if (!audit_id ||
            stage8_audit_hash(sequence, audit_id, events[i].id, operation, before_hash, after_hash,
                              after_hash, input->algorithm_version, input->config_version,
                              prev_hash, events[i].created_at, event_hash) != CBM_STORE_OK) {
            free(audit_id);
            return CBM_STORE_ERR;
        }
        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            free(audit_id);
            return CBM_STORE_ERR;
        }
        sqlite3_bind_int64(stmt, 1, sequence);
        sqlite3_bind_text(stmt, 2, audit_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, events[i].id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, operation, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, before_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, after_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, after_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, input->algorithm_version, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 9, input->config_version);
        sqlite3_bind_text(stmt, 10, prev_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, event_hash, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 12, events[i].created_at, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        sqlite3_finalize(stmt);
        free(audit_id);
        if (rc != CBM_STORE_OK || stage8_fail_step(fail_step, fail_after))
            return CBM_STORE_ERR;
        snprintf(prev_hash, sizeof(prev_hash), "%s", event_hash);
        sequence++;
    }
    return CBM_STORE_OK;
}

static char *stage8_report(const cbm_edge_reinforcement_input_t *input, stage8_state_t *states,
                           int state_count, int eligible, int ignored, int recorded, int replayed,
                           const stage8_cap_hits_t *hits, const char *digest, bool wrote,
                           bool production_write) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return NULL;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", "stage8-edge-reinforcement-report/v1");
    yyjson_mut_obj_add_str(doc, root, "mode", input->mode);
    yyjson_mut_obj_add_str(doc, root, "algorithm_version", input->algorithm_version);
    yyjson_mut_obj_add_int(doc, root, "config_version", input->config_version);
    yyjson_mut_obj_add_int(doc, root, "eligible_count", eligible);
    yyjson_mut_obj_add_int(doc, root, "ignored_count", ignored);
    yyjson_mut_obj_add_int(doc, root, "recorded_count", recorded);
    yyjson_mut_obj_add_int(doc, root, "replayed_count", replayed);
    yyjson_mut_val *caps = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, caps, "event", hits ? hits->event : 0);
    yyjson_mut_obj_add_int(doc, caps, "evidence_id", hits ? hits->evidence : 0);
    yyjson_mut_obj_add_int(doc, caps, "evidence_source_task", hits ? hits->source_task : 0);
    yyjson_mut_obj_add_int(doc, caps, "path", hits ? hits->path : 0);
    yyjson_mut_obj_add_int(doc, caps, "edge_task", hits ? hits->edge_task : 0);
    yyjson_mut_obj_add_int(doc, caps, "task", hits ? hits->task : 0);
    yyjson_mut_obj_add_int(doc, caps, "edge_total", hits ? hits->edge_total : 0);
    yyjson_mut_obj_add_val(doc, root, "cap_hits", caps);
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    for (int i = 0; i < state_count; i++) {
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "edge_id", states[i].edge_id);
        yyjson_mut_obj_add_sint(doc, item, "effective_delta_ppm", states[i].effective_delta_ppm);
        yyjson_mut_obj_add_sint(doc, item, "pheromone_ppm", states[i].pheromone_ppm);
        yyjson_mut_obj_add_int(doc, item, "success_count", states[i].success_count);
        yyjson_mut_obj_add_int(doc, item, "failure_count", states[i].failure_count);
        yyjson_mut_obj_add_str(doc, item, "state_sha256", states[i].state_hash);
        yyjson_mut_arr_add_val(array, item);
    }
    yyjson_mut_obj_add_val(doc, root, "states", array);
    yyjson_mut_obj_add_str(doc, root, "state_sha256", digest ? digest : "");
    yyjson_mut_obj_add_bool(doc, root, "long_term_state_written", wrote);
    yyjson_mut_obj_add_bool(doc, root, "production_state_written", production_write);
    yyjson_mut_obj_add_bool(doc, root, "production_edge_reinforcement_enabled", production_write);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json;
}

void cbm_store_memory_reinforcement_result_free(cbm_edge_reinforcement_result_t *result) {
    if (!result)
        return;
    free(result->report_json);
    memset(result, 0, sizeof(*result));
}

int cbm_store_memory_reinforcement_replay(cbm_store_t *s,
                                          const cbm_edge_reinforcement_input_t *input,
                                          cbm_edge_reinforcement_result_t *out) {
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (out)
        memset(out, 0, sizeof(*out));
    if (!db || !input || !out || !input->project || !input->mode || !input->algorithm_version ||
        strcmp(input->algorithm_version, STAGE8_ALGORITHM) != 0 ||
        input->config_version != STAGE8_CONFIG_VERSION ||
        (strcmp(input->mode, "off") != 0 && strcmp(input->mode, "shadow") != 0 &&
         strcmp(input->mode, "active") != 0)) {
        return CBM_STORE_ERR;
    }
    if (strcmp(input->mode, "off") == 0) {
        stage8_cap_hits_t hits = {0};
        char empty_hash[65];
        cbm_stage7_sha256_hex("[]", 2, empty_hash);
        out->report_json =
            stage8_report(input, NULL, 0, 0, 0, 0, 0, &hits, empty_hash, false, false);
        return out->report_json ? CBM_STORE_OK : CBM_STORE_ERR;
    }
    if (strcmp(input->mode, "active") == 0 && !stage8_fixture_active_guard(input) &&
        !stage8_production_canary_guard(db, input)) {
        return CBM_STORE_REJECTED;
    }
    stage8_event_t *derived = NULL;
    int derived_count = 0;
    int ignored = 0;
    int rc = stage8_load_stage7_events(db, input, &derived, &derived_count, &ignored);
    if (rc != CBM_STORE_OK)
        return rc;
    stage8_state_t *states = NULL;
    int state_count = 0;
    stage8_cap_hits_t hits = {0};
    char digest[65];
    int recorded = 0;
    int replayed = 0;
    bool wrote = false;
    if (strcmp(input->mode, "shadow") == 0) {
        rc = stage8_aggregate(derived, derived_count, &states, &state_count, &hits, digest);
    } else {
        if (cbm_store_begin(s) != CBM_STORE_OK)
            rc = CBM_STORE_ERR;
        int fail_after = stage8_env_int("CBM_STAGE8_REINFORCEMENT_FAIL_AFTER");
        int fail_step = 0;
        if (rc == CBM_STORE_OK) {
            rc = stage8_prepare_schema(db);
        }
        if (rc == CBM_STORE_OK && stage8_fail_step(&fail_step, fail_after)) {
            rc = CBM_STORE_ERR;
        }
        if (rc == CBM_STORE_OK) {
            rc = stage8_materialize(db, derived, derived_count, &recorded, &replayed, &fail_step,
                                    fail_after);
        }
        stage8_event_t *ledger = NULL;
        int ledger_count = 0;
        char before_hash[65];
        if (rc == CBM_STORE_OK && recorded > 0) {
            rc = stage8_current_state_digest(db, before_hash);
        }
        if (rc == CBM_STORE_OK) {
            rc = stage8_load_ledger(db, &ledger, &ledger_count);
        }
        if (rc == CBM_STORE_OK) {
            rc = stage8_aggregate(ledger, ledger_count, &states, &state_count, &hits, digest);
        }
        char timestamp[40];
        obs_timestamp(timestamp);
        if (rc == CBM_STORE_OK && recorded > 0) {
            rc = stage8_replace_states(db, states, state_count, input, timestamp, &fail_step,
                                       fail_after);
        }
        if (rc == CBM_STORE_OK && recorded > 0) {
            rc = stage8_append_audits(db, derived, derived_count, input, before_hash, digest,
                                      &fail_step, fail_after);
        }
        if (rc == CBM_STORE_OK && recorded > 0 && stage8_fail_step(&fail_step, fail_after)) {
            rc = CBM_STORE_ERR;
        }
        if (rc == CBM_STORE_OK)
            rc = cbm_store_commit(s);
        if (rc != CBM_STORE_OK) {
            cbm_store_rollback(s);
            recorded = 0;
            replayed = 0;
        } else {
            wrote = recorded > 0;
        }
        stage8_events_free(ledger, ledger_count);
    }
    if (rc == CBM_STORE_OK) {
        out->recorded_count = recorded;
        out->replayed_count = replayed;
        out->report_json = stage8_report(
            input, states, state_count, derived_count, ignored, recorded, replayed, &hits, digest,
            wrote, wrote && strcmp(input->project, STAGE8_PRODUCTION_PROJECT) == 0);
        if (!out->report_json)
            rc = CBM_STORE_ERR;
    }
    stage8_states_free(states, state_count);
    stage8_events_free(derived, derived_count);
    return rc;
}

int cbm_store_memory_stage8_audit_verify(cbm_store_t *s, int *out_count) {
    sqlite3 *db = s ? cbm_store_get_db(s) : NULL;
    if (out_count)
        *out_count = 0;
    if (!db || !stage8_schema_complete(db))
        return CBM_STORE_ERR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT sequence_no,audit_event_id,contribution_event_id,operation,before_state_sha256,"
        "after_state_sha256,rebuild_sha256,algorithm_version,config_version,prev_hash,event_hash,"
        "created_at FROM edge_reinforcement_audit_event ORDER BY sequence_no;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return CBM_STORE_ERR;
    char expected_prev[65];
    memset(expected_prev, '0', 64);
    expected_prev[64] = '\0';
    int count = 0;
    int64_t expected_sequence = 1;
    int rc = CBM_STORE_OK;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t sequence = sqlite3_column_int64(stmt, 0);
        const char *audit_id = (const char *)sqlite3_column_text(stmt, 1);
        const char *contribution_id = (const char *)sqlite3_column_text(stmt, 2);
        const char *operation = (const char *)sqlite3_column_text(stmt, 3);
        const char *before_hash = (const char *)sqlite3_column_text(stmt, 4);
        const char *after_hash = (const char *)sqlite3_column_text(stmt, 5);
        const char *rebuild_hash = (const char *)sqlite3_column_text(stmt, 6);
        const char *algorithm = (const char *)sqlite3_column_text(stmt, 7);
        int config = sqlite3_column_int(stmt, 8);
        const char *prev_hash = (const char *)sqlite3_column_text(stmt, 9);
        const char *stored_hash = (const char *)sqlite3_column_text(stmt, 10);
        const char *created_at = (const char *)sqlite3_column_text(stmt, 11);
        char actual_hash[65];
        if (sequence != expected_sequence || strcmp(prev_hash, expected_prev) != 0 ||
            stage8_audit_hash(sequence, audit_id, contribution_id, operation, before_hash,
                              after_hash, rebuild_hash, algorithm, config, prev_hash, created_at,
                              actual_hash) != CBM_STORE_OK ||
            strcmp(stored_hash, actual_hash) != 0) {
            rc = CBM_STORE_ERR;
            break;
        }
        snprintf(expected_prev, sizeof(expected_prev), "%s", stored_hash);
        expected_sequence++;
        count++;
    }
    sqlite3_finalize(stmt);
    if (rc == CBM_STORE_OK && out_count)
        *out_count = count;
    return rc;
}
