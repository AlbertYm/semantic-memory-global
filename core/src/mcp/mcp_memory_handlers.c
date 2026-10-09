/*
 * mcp_memory_handlers.c — Local-fork ADR MCP handler functions.
 *
 * Extracted from src/mcp/mcp.c (HEAD, commit 38f8d25) so mcp.c can be
 * replaced with the upstream version independently. Every handler here
 * opens its own memory-store handle via the public cbm_store_* API so it
 * never touches cbm_mcp_server_t internals (the struct definition is
 * static inside mcp.c).
 */

#include "mcp/mcp.h"
#include "store/store.h"
#include "memory/memory_store.h"
#include "memory/verified_learning.h"
#include "memory/memory_security.h"
#include "memory/memory_orchestrator.h"
#include "memory/global_memory.h"
#include "memory/evolution_engine.h"
#include "memory/project_resolver.h"
#include "memory/edge_lifecycle.h"
#include "memory/concept_growth.h"
#include "foundation/log.h"
#include "foundation/constants.h"
#include "foundation/platform.h"
#include "foundation/compat.h"

#include <sqlite3.h>
#include <yyjson/yyjson.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <bcrypt.h>
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

/* ═════════════════════════════════════════════════════════════════════
 *  Internal helpers (all static — no symbols exported)
 * ═════════════════════════════════════════════════════════════════════ */

/* ── yyjson argument extraction ──────────────────────────────────── */

static yyjson_val *memory_arg(yyjson_doc *doc, const char *key) {
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    return root && yyjson_is_obj(root) ? yyjson_obj_get(root, key) : NULL;
}

static char *memory_arg_string_dup(yyjson_doc *doc, const char *key) {
    yyjson_val *v = memory_arg(doc, key);
    return (v && yyjson_is_str(v)) ? cbm_strdup(yyjson_get_str(v)) : NULL;
}

static double memory_arg_double(yyjson_doc *doc, const char *key, double def) {
    yyjson_val *v = memory_arg(doc, key);
    return (v && yyjson_is_num(v)) ? yyjson_get_real(v) : def;
}

static double memory_arg_positive_double(yyjson_doc *doc, const char *key, double def) {
    yyjson_val *v = memory_arg(doc, key);
    if (!v || !yyjson_is_num(v)) {
        return def;
    }
    double value = yyjson_get_real(v);
    return value > 0.0 ? value : def;
}

static char *memory_arg_raw_dup(yyjson_doc *doc, const char *key) {
    yyjson_val *v = memory_arg(doc, key);
    if (!v)
        return NULL;
    if (yyjson_is_str(v))
        return cbm_strdup(yyjson_get_str(v));
    return yyjson_val_write(v, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
}

static char *memory_security_response(const cbm_memory_security_result_t *security, bool is_error) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "ok", security->allowed);
    yyjson_mut_obj_add_str(doc, root, "code", security->code);
    yyjson_mut_obj_add_str(doc, root, "category", security->category);
    yyjson_mut_obj_add_str(doc, root, "action", security->action);
    yyjson_mut_obj_add_str(doc, root, "policy_id", CBM_MEMORY_SECURITY_POLICY_ID);
    yyjson_mut_obj_add_int(doc, root, "policy_version", CBM_MEMORY_SECURITY_POLICY_VERSION);
    yyjson_mut_obj_add_str(doc, root, "detector_version", CBM_MEMORY_SECURITY_DETECTOR_VERSION);
    yyjson_mut_val *reasons = yyjson_mut_arr(doc);
    yyjson_mut_arr_add_str(doc, reasons, security->reason_code);
    yyjson_mut_obj_add_val(doc, root, "reason_codes", reasons);
    yyjson_mut_obj_add_str(doc, root, "content_sha256", security->content_sha256);
    yyjson_mut_obj_add_int(doc, root, "content_length", (int64_t)security->content_length);
    yyjson_mut_obj_add_bool(doc, root, "untrusted_data", true);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, is_error);
    free(json);
    return result;
}

static char *memory_security_scope_response(const char *scope_value) {
    cbm_memory_security_result_t security = {0};
    const char *value = scope_value ? scope_value : "";
    if (cbm_memory_security_scan(value, strlen(value), &security) != 0) {
        return cbm_mcp_text_result("SECURITY_POLICY_MISMATCH", true);
    }
    security.allowed = false;
    security.code = "SECURITY_SCOPE_VIOLATION";
    security.category = "scope";
    security.action = "reject";
    security.reason_code = "scope_mismatch";
    return memory_security_response(&security, true);
}

static char *memory_security_scan_arg(yyjson_doc *doc, const char *key, const char *operation) {
    char *value = memory_arg_raw_dup(doc, key);
    if (!value)
        return NULL;
    cbm_memory_security_result_t security = {0};
    int rc = cbm_memory_security_scan(value, strlen(value), &security);
    free(value);
    if (rc != 0) {
        return cbm_mcp_text_result("SECURITY_POLICY_MISMATCH", true);
    }
    if (security.allowed)
        return NULL;
    cbm_log_security_event(security.code, operation, NULL, security.content_sha256,
                           security.content_length, 0);
    return memory_security_response(&security, true);
}

static char *memory_security_scan_keys(yyjson_doc *doc, const char *const *keys, size_t key_count,
                                       const char *operation) {
    for (size_t i = 0; i < key_count; i++) {
        char *blocked = memory_security_scan_arg(doc, keys[i], operation);
        if (blocked)
            return blocked;
    }
    return NULL;
}

static bool memory_tool_scope_guarded(const char *tool_name) {
    static const char *const tools[] = {
        "events",
        "memories_retrieve",
        "memories_inspect",
        "memory_update_status",
        "memory_feedback",
        "memory_reinforcement_replay",
        "memory_edge_lifecycle_migrate",
        "memory_edge_maintenance",
        "memory_edge_restore",
        "memory_concept_generate",
        "memory_concept_review",
        "memory_concept_inspect",
        "memory_observe_injection",
        "memory_observe_usage",
        "memory_delete",
        "admin_consolidate",
        "admin_decay",
        "memory_health",
        "adr_list",
        "adr_chain",
        "memory_task_begin",
        "memory_task_status",
        "memory_learning_status",
        "memory_learning_control",
        "memory_task_complete",
        "memory_task_migrate",
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (strcmp(tool_name, tools[i]) == 0)
            return true;
    }
    return false;
}

static bool memory_global_default_project(const char *project) {
    return project && project[0] && !cbm_mcp_memory_fixture_project_authorized(project);
}

static cbm_store_t *memory_stage14_store(cbm_mcp_server_t *srv, const char *project, bool create) {
    return memory_global_default_project(project) ? resolve_global_memory_store(srv, create)
                                                  : resolve_memory_store(srv, project, create);
}

static bool memory_stage14_workspace_path(const char *project) {
    if (!project || !project[0])
        return false;
    return !strncmp(project, "file://", 7) || project[0] == '/' || project[0] == '\\' ||
           (isalpha((unsigned char)project[0]) && project[1] == ':' &&
            (project[2] == '/' || project[2] == '\\'));
}

static int memory_stage14_resolve_project(cbm_store_t *store, const char *project,
                                          cbm_project_resolution_t *out) {
    if (!store || !project || !out)
        return CBM_STORE_ERR;
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "c.project_uuid,c.canonical_path,c.path_hash,c.display_name,COALESCE(c.volume_id,''),"
        "COALESCE(c.source_fingerprint,''),c.workspace_state "
        "FROM global_project_catalog c LEFT JOIN global_legacy_alias a "
        "ON a.project_uuid=c.project_uuid AND a.legacy_kind='project' "
        "WHERE c.project_uuid=?1 OR a.legacy_id=?1 "
        "ORDER BY CASE WHEN c.project_uuid=?1 THEN 0 ELSE 1 END LIMIT 1;";
    if (sqlite3_prepare_v2(cbm_store_get_db(store), sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            snprintf(out->project_uuid, sizeof(out->project_uuid), "%s",
                     sqlite3_column_text(stmt, 0));
            snprintf(out->canonical_path, sizeof(out->canonical_path), "%s",
                     sqlite3_column_text(stmt, 1));
            snprintf(out->path_hash, sizeof(out->path_hash), "%s", sqlite3_column_text(stmt, 2));
            snprintf(out->display_name, sizeof(out->display_name), "%s",
                     sqlite3_column_text(stmt, 3));
            snprintf(out->volume_id, sizeof(out->volume_id), "%s", sqlite3_column_text(stmt, 4));
            snprintf(out->source_fingerprint, sizeof(out->source_fingerprint), "%s",
                     sqlite3_column_text(stmt, 5));
            const char *state = (const char *)sqlite3_column_text(stmt, 6);
            out->path_exists = state && strcmp(state, "missing") != 0;
            out->path_writable = state && strcmp(state, "writable") == 0;
            sqlite3_finalize(stmt);
            return CBM_STORE_OK;
        }
    }
    sqlite3_finalize(stmt);
    return memory_stage14_workspace_path(project) &&
                   cbm_project_resolve(project, NULL, NULL, out) == 0
               ? CBM_STORE_OK
               : CBM_STORE_NOT_FOUND;
}

static const char *memory_stage14_project_identity(cbm_store_t *store, const char *project,
                                                   char out[CBM_PROJECT_UUID_SIZE]) {
    if (!memory_global_default_project(project))
        return project;
    cbm_project_resolution_t resolution = {0};
    if (memory_stage14_resolve_project(store, project, &resolution) != CBM_STORE_OK)
        return NULL;
    snprintf(out, CBM_PROJECT_UUID_SIZE, "%s", resolution.project_uuid);
    return out;
}

static bool memory_security_response_hash(const char *value, char out[65]) {
    if (!value || !out)
        return false;
    size_t length = strlen(value);
    bool sha256 = length == 64;
    for (size_t i = 0; sha256 && i < length; i++) {
        if (!isxdigit((unsigned char)value[i]))
            sha256 = false;
    }
    if (sha256) {
        for (size_t i = 0; i < length; i++)
            out[i] = (char)tolower((unsigned char)value[i]);
        out[length] = '\0';
        return true;
    }
    cbm_memory_security_result_t derived = {0};
    if (cbm_memory_security_scan(value, length, &derived) != 0)
        return false;
    snprintf(out, 65, "%s", derived.content_sha256);
    return true;
}

char *cbm_mcp_memory_security_guard(cbm_mcp_server_t *srv, const char *tool_name,
                                    const char *args) {
    if (!tool_name || !args)
        return NULL;
    bool events = strcmp(tool_name, "events") == 0;
    bool feedback = strcmp(tool_name, "memory_feedback") == 0;
    bool concept_review = strcmp(tool_name, "memory_concept_review") == 0;
    bool usage = strcmp(tool_name, "memory_observe_usage") == 0;
    bool injection = strcmp(tool_name, "memory_observe_injection") == 0;
    bool scope_guarded = memory_tool_scope_guarded(tool_name);
    if (!scope_guarded)
        return NULL;

    yyjson_doc *doc = yyjson_read(args, strlen(args), 0);
    if (!doc)
        return NULL;
    char *project = memory_arg_string_dup(doc, "project");
    char *store = memory_arg_string_dup(doc, "store");
    char *scope = memory_arg_string_dup(doc, "scope");
    char *workspace = memory_arg_string_dup(doc, "workspace");
    char *user = memory_arg_string_dup(doc, "user");
    const char *effective_store = store ? store : "project-memory";
    bool fixture_scope = project && cbm_mcp_memory_fixture_project_authorized(project);
    bool global_lifecycle = strcmp(tool_name, "memory_task_begin") == 0 ||
                            strcmp(tool_name, "memory_task_status") == 0 ||
                            strcmp(tool_name, "memory_task_complete") == 0;
    bool global_default = memory_global_default_project(project);
    bool global_identity_valid = false;
    if (global_default) {
        global_identity_valid = memory_stage14_workspace_path(project) ||
                                (global_lifecycle && memory_stage14_workspace_path(workspace));
        if (!global_identity_valid) {
            cbm_store_t *global_store = resolve_global_memory_store(srv, false);
            cbm_project_resolution_t known = {0};
            global_identity_valid =
                global_store &&
                memory_stage14_resolve_project(global_store, project, &known) == CBM_STORE_OK;
        }
    }
    bool scope_invalid = (global_lifecycle || (global_default && global_identity_valid))
                             ? (!project || strcmp(effective_store, "project-memory") != 0 ||
                                (scope && strcmp(scope, "project") != 0 &&
                                 (!events || strcmp(scope, "global") != 0)))
                             : (!project || strcmp(effective_store, "project-memory") != 0 ||
                                (scope && strcmp(scope, "project") != 0) ||
                                ((workspace || user) && !fixture_scope) ||
                                !cbm_mcp_memory_project_authorized(srv, project));
    if ((!strcmp(tool_name,"memory_learning_status") || !strcmp(tool_name,"memory_learning_control")) &&
        global_default && !global_identity_valid)scope_invalid=true;
    if (scope_invalid) {
        const char *scope_value = project ? project : (workspace ? workspace : user);
        char *result = memory_security_scope_response(scope_value);
        free(project);
        free(store);
        free(scope);
        free(workspace);
        free(user);
        yyjson_doc_free(doc);
        return result;
    }

    char *blocked = NULL;
    if (events) {
        static const char *const keys[] = {
            "type",
            "source",
            "task",
            "kind",
            "layer",
            "title",
            "summary",
            "entity_key",
            "predicate",
            "payload",
            "content",
            "supersedes",
            "derived_from_memory_id",
            "context",
            "about_code",
        };
        blocked = memory_security_scan_keys(doc, keys, sizeof(keys) / sizeof(keys[0]), tool_name);
    } else if (feedback) {
        static const char *const keys[] = {
            "task_type",    "result_ref",       "result_payload", "evidence_source",
            "evidence_ref", "evidence_payload", "action",
        };
        blocked = memory_security_scan_keys(doc, keys, sizeof(keys) / sizeof(keys[0]), tool_name);
    } else if (concept_review) {
        static const char *const keys[] = {"content_text", "related_candidate_id"};
        blocked = memory_security_scan_keys(doc, keys, sizeof(keys) / sizeof(keys[0]), tool_name);
    } else if (usage) {
        static const char *const keys[] = {"evidence_type", "evidence_ref"};
        blocked = memory_security_scan_keys(doc, keys, sizeof(keys) / sizeof(keys[0]), tool_name);
    } else if (injection) {
        static const char *const keys[] = {"target"};
        blocked = memory_security_scan_keys(doc, keys, sizeof(keys) / sizeof(keys[0]), tool_name);
    }
    free(project);
    free(store);
    free(scope);
    free(workspace);
    free(user);
    yyjson_doc_free(doc);
    return blocked;
}

/* Confidence/reusability scoring (formerly memory_l1_blend + the P4 reusability
 * floor, both inline here) now lives in one place: cbm_memory_score_item in
 * memory_store.c, which folds L1 graph signal ⊕ L2 kind prior ⊕ L3 declared. */

/* Free the heap-copied about_code anchor list (see handle_events). */
static void free_anchor_qns(char **qns, size_t n) {
    if (!qns) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(qns[i]);
    }
    free(qns);
}

static bool events_derived_from_fail_after(const char *point) {
    char value[32] = {0};
    cbm_safe_getenv("CBM_STAGE8_DERIVED_FROM_FAIL_AFTER", value, sizeof(value), NULL);
    return point && value[0] && strcmp(value, point) == 0;
}

/* P3-d structure dimension (HELPER, advice only — never rejects, never requires
 * exact format). For a decision-class memory, do a deliberately LOOSE check for
 * ADR structure elements (CN or EN keywords, order/format ignored) and return a
 * gentle nudge string, or NULL when no advice is warranted. Two cases earn a
 * nudge: (1) substantial content with NO recognizable structure element at all;
 * (2) has a decision element but is missing the "rejected alternatives" part —
 * the most valuable and most-often-omitted ADR section. Short content is left
 * alone. Returns a static string (not owned by caller). */
static const char *memory_structure_advice(const char *kind, const char *content) {
    if (!kind || !content) {
        return NULL;
    }
    if (strcmp(kind, "decision") != 0 && strcmp(kind, "constraint") != 0) {
        return NULL;
    }
    if (strlen(content) < 80) {
        return NULL; /* too short to expect full structure */
    }
    bool has_decision = strstr(content, "决策") || strstr(content, "Decision") ||
                        strstr(content, "decision") || strstr(content, "[Decision]");
    bool has_context =
        strstr(content, "背景") || strstr(content, "Context") || strstr(content, "context");
    bool has_rejected = strstr(content, "否决") || strstr(content, "替代") ||
                        strstr(content, "Rejected") || strstr(content, "alternative") ||
                        strstr(content, "Alternative");
    bool any_structure = has_decision || has_context || has_rejected;
    if (!any_structure) {
        return "no recognizable ADR structure: consider stating the Decision, its "
               "Context, and the Rejected alternatives so the rationale survives.";
    }
    if (!has_rejected) {
        return "missing 'Rejected alternatives': recording what you DIDN'T choose and "
               "why is the most valuable part of an ADR for a future reader.";
    }
    return NULL;
}

/* ── Phantom project name guard ─────────────────────────────────────
 * An earlier buggy list_projects handed callers the sidecar filename
 * "<project>-memory"; passed back as a project it makes
 * cbm_memory_db_path re-append "-memory.db" → a spurious
 * "<project>-memory-memory.db" orphan, and the SQL filter
 * scope_project="<project>-memory" matches nothing (the rows store the
 * un-suffixed name). If the incoming name ends in "-memory" AND the
 * de-suffixed base already has a real "<base>-memory.db" on disk, the
 * name is a phantom: return the base. Otherwise return a copy of the
 * input unchanged. A genuine project whose own name ends in "-memory"
 * (e.g. "D-semantic-memory-mcp" — its base "D-semantic-memory-" has no
 * "-memory.db") is left intact. Caller frees. */

static char *normalize_phantom_project(const char *project) {
    if (!project) {
        return NULL;
    }
    const char *suf = "-memory";
    size_t plen = strlen(project);
    size_t slen = strlen(suf);
    if (plen > slen && strcmp(project + plen - slen, suf) == 0) {
        char base[CBM_SZ_1K];
        snprintf(base, sizeof(base), "%.*s", (int)(plen - slen), project);
        char base_mem_path[CBM_SZ_1K];
        if (cbm_memory_db_path(base, base_mem_path, sizeof(base_mem_path)) == CBM_STORE_OK &&
            cbm_file_exists(base_mem_path)) {
            return cbm_strdup(base);
        }
    }
    return cbm_strdup(project);
}

/* ── Open memory store (read-only, independent of srv internals) ─── */

static cbm_store_t *open_memory_store_for_project(const char *project) {
    if (!project) {
        return NULL;
    }
    char mem_path[CBM_SZ_1K];
    if (cbm_memory_db_path(project, mem_path, sizeof(mem_path)) != CBM_STORE_OK) {
        return NULL;
    }
    if (!cbm_file_exists(mem_path)) {
        return NULL;
    }
    return cbm_store_open_path_query(mem_path);
}

/* ═════════════════════════════════════════════════════════════════════
 *  Public handlers (declared in mcp.h, called from mcp.c dispatch)
 * ═════════════════════════════════════════════════════════════════════ */

/* ═════════════════════════════════════════════════════════════════════
 *  Memory tool handlers (moved verbatim from mcp.c, 2026-07-09).
 *  Store access goes through resolve_memory_store/resolve_global_memory_store
 *  exported by mcp.c — the only functions touching cbm_mcp_server internals.
 * ═════════════════════════════════════════════════════════════════════ */

static const char *memory_item_str(const char *s) {
    return s ? s : "";
}

static yyjson_mut_val *memory_item_to_json(yyjson_mut_doc *doc, const cbm_memory_item_t *it) {
    yyjson_mut_val *obj = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, obj, "id", memory_item_str(it->id));
    yyjson_mut_obj_add_str(doc, obj, "kind", memory_item_str(it->kind));
    yyjson_mut_obj_add_str(doc, obj, "layer", memory_item_str(it->layer));
    yyjson_mut_obj_add_str(doc, obj, "title", memory_item_str(it->title));
    yyjson_mut_obj_add_str(doc, obj, "summary", memory_item_str(it->summary));
    yyjson_mut_obj_add_str(doc, obj, "content", memory_item_str(it->content));
    yyjson_mut_obj_add_str(doc, obj, "scope_user", memory_item_str(it->scope_user));
    yyjson_mut_obj_add_str(doc, obj, "scope_project", memory_item_str(it->scope_project));
    yyjson_mut_obj_add_str(doc, obj, "scope_task", memory_item_str(it->scope_task));
    yyjson_mut_obj_add_str(doc, obj, "entity_key", memory_item_str(it->entity_key));
    yyjson_mut_obj_add_str(doc, obj, "predicate", memory_item_str(it->predicate));
    yyjson_mut_obj_add_real(doc, obj, "importance", it->importance);
    yyjson_mut_obj_add_real(doc, obj, "confidence", it->confidence);
    yyjson_mut_obj_add_real(doc, obj, "reusability", it->reusability);
    yyjson_mut_obj_add_real(doc, obj, "specificity", it->specificity);
    yyjson_mut_obj_add_int(doc, obj, "hit_count", it->hit_count);
    yyjson_mut_obj_add_int(doc, obj, "last_hit_at", it->last_hit_at);
    yyjson_mut_obj_add_real(doc, obj, "decay", it->decay);
    yyjson_mut_obj_add_str(doc, obj, "status", memory_item_str(it->status));
    yyjson_mut_obj_add_int(doc, obj, "version", it->version);
    yyjson_mut_obj_add_str(doc, obj, "supersedes", memory_item_str(it->supersedes));
    yyjson_mut_obj_add_int(doc, obj, "created_at", it->created_at);
    yyjson_mut_obj_add_int(doc, obj, "updated_at", it->updated_at);
    yyjson_mut_obj_add_str(doc, obj, "source_event_ids", memory_item_str(it->source_event_ids));
    yyjson_mut_obj_add_int(doc, obj, "conflict_count", it->conflict_count);
    yyjson_mut_obj_add_str(doc, obj, "conflict_ids", memory_item_str(it->conflict_ids));
    yyjson_mut_obj_add_str(doc, obj, "conflict_resolution",
                           memory_item_str(it->conflict_resolution));
    yyjson_mut_obj_add_str(doc, obj, "evidence_json", memory_item_str(it->evidence_json));
    yyjson_mut_obj_add_str(doc, obj, "retrieval_source", memory_item_str(it->retrieval_source));
    yyjson_mut_obj_add_real(doc, obj, "retrieval_score", it->retrieval_score);
    yyjson_mut_obj_add_real(doc, obj, "learning_adjustment", it->learning_adjustment);
    return obj;
}

static void memory_activation_add_json(yyjson_mut_doc *doc, yyjson_mut_val *root,
                                       const cbm_memory_activation_report_t *report,
                                       const char *field_name) {
    if (!doc || !root || !report || !report->status || !field_name) {
        return;
    }
    yyjson_mut_val *activation = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, activation, "mode", report->mode ? report->mode : "");
    yyjson_mut_obj_add_str(doc, activation, "status", report->status);
    yyjson_mut_obj_add_str(doc, activation, "session_id",
                           report->session_id ? report->session_id : "");
    yyjson_mut_obj_add_str(doc, activation, "contract_version", "stage6-bounded-activation-v1");
    yyjson_mut_obj_add_str(doc, activation, "termination_reason",
                           report->termination_reason ? report->termination_reason : "");
    yyjson_mut_obj_add_bool(doc, activation, "request_scoped", true);
    yyjson_mut_obj_add_bool(doc, activation, "long_term_state_written", false);
    yyjson_mut_obj_add_bool(doc, activation, "vector_seed_allowed", false);
    yyjson_mut_obj_add_int(doc, activation, "seed_count", report->seed_count);
    yyjson_mut_obj_add_int(doc, activation, "vector_seeds_blocked", report->vector_seeds_blocked);
    yyjson_mut_obj_add_int(doc, activation, "node_count", report->node_count);
    yyjson_mut_obj_add_int(doc, activation, "edge_visits", report->edge_visits);
    yyjson_mut_obj_add_int(doc, activation, "accepted_visits", report->accepted_visits);
    yyjson_mut_obj_add_int(doc, activation, "token_proxy", report->token_proxy);
    yyjson_mut_obj_add_int(doc, activation, "max_hop_observed", report->max_hop_observed);
    yyjson_mut_obj_add_real(doc, activation, "elapsed_ms", report->elapsed_ms);
    yyjson_mut_obj_add_bool(doc, activation, "budget_exhausted", report->budget_exhausted);

    yyjson_mut_val *rejections = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_int(doc, rejections, "cycle", report->cycle_rejections);
    yyjson_mut_obj_add_int(doc, rejections, "duplicate", report->duplicate_rejections);
    yyjson_mut_obj_add_int(doc, rejections, "scope", report->scope_rejections);
    yyjson_mut_obj_add_int(doc, rejections, "version", report->version_rejections);
    yyjson_mut_obj_add_int(doc, rejections, "unsafe", report->unsafe_rejections);
    yyjson_mut_obj_add_int(doc, rejections, "threshold", report->threshold_rejections);
    yyjson_mut_obj_add_val(doc, activation, "rejections", rejections);

    yyjson_mut_val *candidates = yyjson_mut_arr(doc);
    for (int i = 0; i < report->candidate_count; i++) {
        const cbm_memory_activation_candidate_t *candidate = &report->candidates[i];
        yyjson_mut_val *item = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, item, "item_id", candidate->item_id ? candidate->item_id : "");
        yyjson_mut_obj_add_str(doc, item, "candidate_id",
                               candidate->candidate_id ? candidate->candidate_id : "");
        yyjson_mut_obj_add_str(doc, item, "path_id", candidate->path_id ? candidate->path_id : "");
        yyjson_mut_obj_add_str(doc, item, "evidence_id",
                               candidate->evidence_id ? candidate->evidence_id : "");
        yyjson_mut_obj_add_str(doc, item, "seed_id", candidate->seed_id ? candidate->seed_id : "");
        yyjson_mut_obj_add_real(doc, item, "activation_score", candidate->score);
        yyjson_mut_obj_add_int(doc, item, "hop", candidate->hop);
        yyjson_mut_obj_add_int(doc, item, "predecessor_count", candidate->predecessor_count);
        if (candidate->explanation_json) {
            yyjson_doc *explanation_doc =
                yyjson_read(candidate->explanation_json, strlen(candidate->explanation_json), 0);
            if (explanation_doc) {
                yyjson_mut_val *copy =
                    yyjson_val_mut_copy(doc, yyjson_doc_get_root(explanation_doc));
                if (copy) {
                    yyjson_mut_obj_add_val(doc, item, "explanation", copy);
                }
                yyjson_doc_free(explanation_doc);
            }
        }
        yyjson_mut_arr_add_val(candidates, item);
    }
    yyjson_mut_obj_add_val(doc, activation, "candidates", candidates);
    yyjson_mut_obj_add_val(doc, root, field_name, activation);
}

static bool memory_policy_has_signal(const char *s) {
    if (!s)
        return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p > ' ')
            return true;
    }
    return false;
}

static bool memory_policy_contains_i(const char *hay, const char *needle) {
    if (!hay || !needle || !needle[0])
        return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i]) {
            unsigned char a = (unsigned char)p[i], b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z')
                a = (unsigned char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (unsigned char)(b - 'A' + 'a');
            if (a != b)
                break;
            i++;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

/* Skip leading ASCII whitespace. */
static const char *memory_skip_ws(const char *s) {
    if (!s)
        return s;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return s;
}

/* Structured-format gate for high-value kinds (decision/lesson/constraint).
 * Returns a non-NULL reason string when the write is malformed, NULL when OK.
 * Enforces the ADR contract advertised in the events tool schema so the format
 * holds across every window regardless of how the user phrased the request:
 *   - summary and content must both be present and non-empty
 *   - content must be plain text, not a {"content":...} JSON wrapper
 *   - summary must not be a copy of content (it is the independent recall key)
 * Other kinds (fact/todo/reference/raw events) are not constrained here. */
static const char *memory_validate_format(const char *kind, const char *summary,
                                          const char *content) {
    if (!kind)
        return NULL;
    if (strcmp(kind, "decision") != 0 && strcmp(kind, "lesson") != 0 &&
        strcmp(kind, "constraint") != 0)
        return NULL;
    if (!memory_policy_has_signal(summary))
        return "missing_summary";
    if (!memory_policy_has_signal(content))
        return "missing_content";
    /* Reject JSON-wrapped content like {"content":"..."} — a write-side bug that
     * double-encodes the field. Detect a leading '{' followed by a quoted key. */
    const char *c = memory_skip_ws(content);
    if (*c == '{') {
        const char *q = memory_skip_ws(c + 1);
        if (*q == '"')
            return "content_json_wrapped";
    }
    if (strcmp(summary, content) == 0)
        return "summary_equals_content";
    return NULL;
}

static const char *memory_write_policy_decide(const char *text, const char *kind, const char *type,
                                              const char **reason) {
    if (!memory_policy_has_signal(text)) {
        if (reason)
            *reason = "empty_payload";
        return "rejected";
    }
    if ((kind && (strcmp(kind, "debug") == 0 || strcmp(kind, "scratch") == 0)) ||
        (type && (strcmp(type, "debug") == 0 || strcmp(type, "scratch") == 0)) ||
        memory_policy_contains_i(text, "temporary note") ||
        memory_policy_contains_i(text, "scratch note") ||
        memory_policy_contains_i(text, "临时记录") || memory_policy_contains_i(text, "临时笔记") ||
        memory_policy_contains_i(text, "草稿")) {
        if (reason)
            *reason = "low_value_transient";
        return "rejected";
    }
    if ((kind && (strcmp(kind, "preference") == 0 || strcmp(kind, "decision") == 0 ||
                  strcmp(kind, "constraint") == 0 || strcmp(kind, "lesson") == 0)) ||
        memory_policy_contains_i(text, "remember") ||
        memory_policy_contains_i(text, "do not forget") || memory_policy_contains_i(text, "记住") ||
        memory_policy_contains_i(text, "牢记") || memory_policy_contains_i(text, "别忘") ||
        memory_policy_contains_i(text, "不要忘") || memory_policy_contains_i(text, "务必")) {
        if (reason)
            *reason = "explicit_or_high_value";
        return "must_write";
    }
    if (reason)
        *reason = "default_candidate";
    return "candidate";
}
char *handle_events(cbm_mcp_server_t *srv, const char *args) {
    yyjson_doc *adoc = yyjson_read(args ? args : "{}", args ? strlen(args) : 2, 0);
    if (!adoc)
        return cbm_mcp_text_result("invalid JSON arguments", true);
    char *project = memory_arg_string_dup(adoc, "project");
    char *scope = memory_arg_string_dup(adoc, "scope");
    char *type = memory_arg_string_dup(adoc, "type");
    char *source = memory_arg_string_dup(adoc, "source");
    char *user = memory_arg_string_dup(adoc, "user");
    char *task = memory_arg_string_dup(adoc, "task");
    char *kind = memory_arg_string_dup(adoc, "kind");
    char *layer = memory_arg_string_dup(adoc, "layer");
    char *title = memory_arg_string_dup(adoc, "title");
    char *summary = memory_arg_string_dup(adoc, "summary");
    char *entity_key = memory_arg_string_dup(adoc, "entity_key");
    char *predicate = memory_arg_string_dup(adoc, "predicate");
    char *payload = memory_arg_raw_dup(adoc, "payload");
    char *content = memory_arg_string_dup(adoc, "content");
    char *supersedes = memory_arg_string_dup(adoc, "supersedes");
    yyjson_val *derived_from_value = memory_arg(adoc, "derived_from_memory_id");
    bool derived_from_present = derived_from_value != NULL;
    char *derived_from = memory_arg_string_dup(adoc, "derived_from_memory_id");
    char *context_json = memory_arg_raw_dup(adoc, "context");
    double confidence = memory_arg_double(adoc, "confidence", 0.5);
    double importance = memory_arg_positive_double(adoc, "importance", 0.5);
    double reusability = memory_arg_positive_double(adoc, "reusability", 0.5);
    double specificity = memory_arg_positive_double(adoc, "specificity", 0.5);
    /* Extract about_code anchors BEFORE freeing adoc. (Previously this array was
     * read from adoc AFTER yyjson_doc_free — a use-after-free that silently
     * dropped every anchor, which is also why graph scoring had nothing to read.
     * Copy the qualified-name strings onto the heap so they outlive the doc.) */
    char **about_code_qns = NULL;
    size_t about_code_n = 0;
    {
        yyjson_val *ac = yyjson_obj_get(yyjson_doc_get_root(adoc), "about_code");
        if (ac && yyjson_is_arr(ac)) {
            size_t cap = yyjson_arr_size(ac);
            if (cap > 0) {
                about_code_qns = calloc(cap, sizeof(char *));
                if (about_code_qns) {
                    size_t ai, amax;
                    yyjson_val *av;
                    yyjson_arr_foreach(ac, ai, amax, av) {
                        if (yyjson_is_str(av)) {
                            about_code_qns[about_code_n++] = cbm_strdup(yyjson_get_str(av));
                        }
                    }
                }
            }
        }
    }
    yyjson_doc_free(adoc);
    if (!project || !payload) {
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("project and payload are required", true);
    }
    /* Collapse a phantom "<project>-memory" name so writes land in the real
     * store with the canonical scope_project (not a "-memory-memory.db" orphan). */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }
    /* scope routes the write: "global" lands in the cross-project store with
     * scope_project=NULL; anything else (default) is project-scoped. `project`
     * stays required and is still used as anchor/audit context even for global
     * writes — it just isn't the storage key. */
    bool is_global =
        (scope && strcmp(scope, "global") == 0) || memory_global_default_project(project);
    cbm_project_resolution_t global_resolution = {0};
    bool has_global_resolution = false;
    bool materialize_relation = derived_from_present && derived_from && derived_from[0];
    if (derived_from_present &&
        (!materialize_relation || (scope && scope[0] && strcmp(scope, "project") != 0))) {
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result(
            "derived_from_memory_id must be a non-empty string and is project-scope only", true);
    }

    const char *policy_reason = NULL;
    const char *policy_decision =
        memory_write_policy_decide(content ? content : payload, kind, type, &policy_reason);

    /* Structured-format gate: for high-value kinds, malformed writes (missing or
     * duplicated summary, JSON-wrapped content) are downgraded to rejected so the
     * ADR format holds across every window. Only override an otherwise-accepting
     * decision — never flip an already-rejected one. */
    if (strcmp(policy_decision, "rejected") != 0) {
        const char *fmt_reason = memory_validate_format(kind, summary, content);
        if (fmt_reason) {
            policy_decision = "rejected";
            policy_reason = fmt_reason;
        }
    }

    /* Resolve the store early so we can write the audit event even for rejected writes.
     * Write path: create-if-absent so a pure-memory project's first write builds its DB.
     * Global writes go to the cross-project store instead of the per-project one. */
    cbm_store_t *store = NULL;
    if (is_global) {
        store = resolve_global_memory_store(srv, true);
        char ensure_key[96], *ensure_report = NULL;
        if (!store ||
            memory_stage14_resolve_project(store, project, &global_resolution) != CBM_STORE_OK) {
            store = NULL;
        } else {
            has_global_resolution = true;
            snprintf(ensure_key, sizeof(ensure_key), "stage14-mcp-project-%.64s",
                     global_resolution.path_hash);
            int ensure_rc = cbm_global_store_ensure_project(store, &global_resolution, ensure_key,
                                                            &ensure_report);
            free(ensure_report);
            if (ensure_rc != CBM_STORE_OK && ensure_rc != CBM_STORE_REPLAYED)
                store = NULL;
        }
    } else if (materialize_relation) {
        /* Do not create a new empty store for a relation request. First prove
         * the target store exists, then upgrade the cached handle to writable. */
        store = resolve_memory_store(srv, project, false);
        if (store)
            store = resolve_memory_store(srv, project, true);
    } else {
        store = resolve_memory_store(srv, project, true);
    }

    if (strcmp(policy_decision, "rejected") == 0) {
        /* Write a lightweight audit.rejected event so policy decisions are traceable
         * and can be used to tune write policy thresholds (framework §3, §0 principle 8). */
        if (store) {
            cbm_memory_event_t audit_ev = {0};
            audit_ev.type = "audit.rejected";
            audit_ev.source = source ? source : "mcp.events";
            audit_ev.project = project;
            audit_ev.user = user;
            audit_ev.payload = payload;
            audit_ev.confidence = 0.0;
            /* Encode the rejection reason in context so it's queryable. */
            char audit_ctx[256];
            snprintf(audit_ctx, sizeof(audit_ctx),
                     "{\"policy_reason\":\"%s\",\"kind\":\"%s\",\"type\":\"%s\"}",
                     policy_reason ? policy_reason : "", kind ? kind : "", type ? type : "");
            audit_ev.context_json = audit_ctx;
            (void)cbm_store_memory_append_event(store, &audit_ev, NULL);
        }
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(doc);
        yyjson_mut_doc_set_root(doc, root);
        yyjson_mut_obj_add_str(doc, root, "status", "rejected");
        yyjson_mut_obj_add_str(doc, root, "policy_decision", policy_decision);
        yyjson_mut_obj_add_str(doc, root, "policy_reason", policy_reason ? policy_reason : "");
        yyjson_mut_obj_add_str(doc, root, "hot_path",
                               "write policy rejected; audit event written to memory_event");
        char *json = yy_doc_to_str(doc);
        yyjson_mut_doc_free(doc);
        char *result = cbm_mcp_text_result(json, false);
        free(json);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return result;
    }
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return _res;
    }
    cbm_memory_event_t event = {0};
    event.type = type ? type : "memory.event";
    event.source = source ? source : "mcp.events";
    event.project = project;
    event.user = user;
    event.payload = payload;
    event.confidence = confidence;
    event.context_json = context_json ? context_json : "{}";
    char *event_id = NULL;
    /* Event + structured candidate must persist atomically. Wrap both in one
     * transaction so a crash between them can't leave an orphan event row with
     * no corresponding memory_item. The transaction lives at this business-op
     * layer (not inside the store append fns) because those fns are also called
     * standalone elsewhere — nesting a BEGIN inside them would fail. */
    if (cbm_store_begin(store) != CBM_STORE_OK) {
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to begin memory transaction", true);
    }
    if (cbm_store_memory_append_event(store, &event, &event_id) != CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to append memory event", true);
    }
    if (materialize_relation && events_derived_from_fail_after("event")) {
        cbm_store_rollback(store);
        free(event_id);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("injected derived_from failure after memory event", true);
    }
    char source_ids[CBM_SZ_256];
    snprintf(source_ids, sizeof(source_ids), "[\"%s\"]", event_id ? event_id : "");
    cbm_memory_item_t item = {0};
    item.kind = kind ? kind : "event";
    /* P0-a: ADR identity — decision and constraint memories default to layer "adr"
     * instead of "episodic" so they are fetchable, rankable, and decay-tunable as a
     * distinct class. An explicit layer argument always wins. */
    item.layer = layer
                     ? layer
                     : ((kind && (strcmp(kind, "decision") == 0 || strcmp(kind, "constraint") == 0))
                            ? "adr"
                            : "episodic");
    /* P0-b: for decision-class items without a title, derive one from the summary
     * (first sentence, up to CBM_SZ_128 chars). Summary always carries the query-like
     * conclusion so it makes a far better display label than a NULL fallback. */
    char *derived_title = NULL;
    item.title = title;
    if (!item.title && summary && summary[0] && kind &&
        (strcmp(kind, "decision") == 0 || strcmp(kind, "constraint") == 0)) {
        char title_buf[CBM_SZ_128];
        int tl = 0;
        while (summary[tl] && summary[tl] != '\n' && tl < (int)sizeof(title_buf) - 1) {
            title_buf[tl] = summary[tl];
            tl++;
        }
        /* Never cut inside a UTF-8 sequence: locate the lead byte of the final
         * character and drop it if its sequence is incomplete. A mid-character
         * cut leaves invalid UTF-8 in the stored title, which later makes the
         * JSON-RPC envelope's strict re-parse drop the whole result (client
         * hangs waiting for a response that never validates). */
        {
            int lead = tl;
            while (lead > 0 && ((unsigned char)title_buf[lead - 1] & 0xC0) == 0x80) {
                lead--;
            }
            if (lead > 0 && (unsigned char)title_buf[lead - 1] >= 0xC0) {
                unsigned char lb = (unsigned char)title_buf[lead - 1];
                int need = (lb >= 0xF0) ? 4 : (lb >= 0xE0) ? 3 : 2;
                if (tl - (lead - 1) < need) {
                    tl = lead - 1;
                }
            }
        }
        /* Trim trailing punctuation so the label reads cleanly. */
        while (tl > 0 && (title_buf[tl - 1] == '.' || title_buf[tl - 1] == '!')) {
            tl--;
        }
        title_buf[tl] = '\0';
        derived_title = cbm_strdup(title_buf);
        item.title = derived_title;
    }
    item.summary = summary ? summary : (content ? content : payload);
    item.content = content ? content : payload;
    item.scope_user = user;
    /* Global memories carry no project scope (scope_project=NULL) so they read
     * back from every project; memory_infer_entity then classifies them as
     * global/user. Project-scoped writes keep the resolved project name. */
    item.scope_project = is_global ? NULL : project;
    item.scope_task = task;
    item.entity_key = entity_key;
    item.predicate = predicate;
    item.importance = importance;
    item.confidence = confidence;
    item.reusability = reusability;
    item.specificity = specificity;
    item.status = "candidate";
    /* Version increment: when supersedes is set, query the superseded item's version
     * and set this item's version = old_version + 1, giving the ADR timeline a
     * naturally ascending sequence. If the old item is not found (deleted, different
     * scope, etc.), start fresh at version 1 and surface a warning. */
    item.version = 1;
    bool supersedes_found = false;
    const char *supersedes_warning = NULL;
    if (supersedes && supersedes[0]) {
        sqlite3_stmt *ver_stmt = NULL;
        if (sqlite3_prepare_v2(
                cbm_store_get_db(store),
                "SELECT version FROM memory_item WHERE id=?1 AND deleted_at IS NULL;", -1,
                &ver_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ver_stmt, 1, supersedes, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(ver_stmt) == SQLITE_ROW) {
                item.version = sqlite3_column_int(ver_stmt, 0) + 1;
                supersedes_found = true;
            }
            sqlite3_finalize(ver_stmt);
        }
        if (!supersedes_found) {
            supersedes_warning =
                "supersedes target not found: the referenced item_id does not exist "
                "or has been soft-deleted. Version set to 1; supersedes link recorded "
                "but may be dangling.";
        }
    }
    item.supersedes = supersedes; /* P3-d: NULL unless this ADR replaces an earlier one */
    item.source_event_ids = source_ids;
    char *item_id = NULL;
    int item_rc = cbm_store_memory_append_candidate(store, &item, &item_id);

    /* Atomic: if the candidate write fails, roll back the event too so we never
     * persist an orphan event. Otherwise commit both together. */
    if (item_rc != CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(event_id);
        free(item_id);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(derived_title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to append memory candidate", true);
    }

    const char *fts_failure_point = NULL;
    if (events_derived_from_fail_after("fts_prepare")) {
        fts_failure_point = "prepare";
    } else if (events_derived_from_fail_after("fts_insert")) {
        fts_failure_point = "insert";
    }
    if (cbm_store_memory_index_candidate(store, &item, item_id, fts_failure_point) !=
        CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(event_id);
        free(item_id);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(derived_title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to index memory candidate; transaction rolled back",
                                   true);
    }

    if (is_global) {
        char provenance_seed[1024], provenance_hash[65];
        sqlite3_stmt *provenance = NULL;
        int provenance_rc = has_global_resolution ? CBM_STORE_OK : CBM_STORE_ERR;
        if (provenance_rc == CBM_STORE_OK) {
            snprintf(provenance_seed, sizeof(provenance_seed), "%s\n%s\n%s", item_id,
                     global_resolution.project_uuid, event_id ? event_id : "");
            provenance_rc =
                cbm_stage7_sha256_hex(provenance_seed, strlen(provenance_seed), provenance_hash);
        }
        if (provenance_rc == CBM_STORE_OK &&
            sqlite3_prepare_v2(
                cbm_store_get_db(store),
                "INSERT OR IGNORE INTO "
                "global_memory_provenance(memory_item_id,project_uuid,legacy_project_id,source_"
                "kind,payload_sha256,created_at) VALUES(?1,?2,?3,'mcp_events',?4,datetime('now'));",
                -1, &provenance, NULL) == SQLITE_OK) {
            sqlite3_bind_text(provenance, 1, item_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(provenance, 2, global_resolution.project_uuid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(provenance, 3, project, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(provenance, 4, provenance_hash, -1, SQLITE_TRANSIENT);
            provenance_rc = sqlite3_step(provenance) == SQLITE_DONE ? CBM_STORE_OK : CBM_STORE_ERR;
        } else if (provenance_rc == CBM_STORE_OK) {
            provenance_rc = CBM_STORE_ERR;
        }
        sqlite3_finalize(provenance);
        provenance = NULL;
        int provenance_exact = 0;
        if (provenance_rc == CBM_STORE_OK &&
            sqlite3_prepare_v2(cbm_store_get_db(store),
                               "SELECT COUNT(*) FROM global_memory_provenance WHERE "
                               "memory_item_id=?1 AND project_uuid=?2 AND legacy_project_id=?3 AND "
                               "source_kind='mcp_events' AND payload_sha256=?4;",
                               -1, &provenance, NULL) == SQLITE_OK) {
            sqlite3_bind_text(provenance, 1, item_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(provenance, 2, global_resolution.project_uuid, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(provenance, 3, project, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(provenance, 4, provenance_hash, -1, SQLITE_TRANSIENT);
            provenance_exact =
                sqlite3_step(provenance) == SQLITE_ROW && sqlite3_column_int(provenance, 0) == 1;
        }
        sqlite3_finalize(provenance);
        if (provenance_rc == CBM_STORE_OK && !provenance_exact)
            provenance_rc = CBM_STORE_IDEMPOTENCY_CONFLICT;
        if (provenance_rc != CBM_STORE_OK) {
            cbm_store_rollback(store);
            free(event_id);
            free(item_id);
            free(project);
            free(type);
            free(scope);
            free(source);
            free(user);
            free(task);
            free(kind);
            free(layer);
            free(title);
            free(derived_title);
            free(summary);
            free(entity_key);
            free(predicate);
            free(payload);
            free(content);
            free(supersedes);
            free(derived_from);
            free(context_json);
            free_anchor_qns(about_code_qns, about_code_n);
            return cbm_mcp_text_result(
                "failed to append global memory provenance; transaction rolled back", true);
        }
    }

    char *derived_edge_id = NULL;
    if (materialize_relation) {
        int relation_rc =
            events_derived_from_fail_after("item")
                ? CBM_STORE_ERR
                : cbm_store_memory_link_derived_from(store, item_id, derived_from, project,
                                                     event_id, &derived_edge_id);
        if (relation_rc == CBM_STORE_OK && events_derived_from_fail_after("edge")) {
            relation_rc = CBM_STORE_ERR;
        }
        if (relation_rc != CBM_STORE_OK) {
            cbm_store_rollback(store);
            free(event_id);
            free(item_id);
            free(derived_edge_id);
            free(project);
            free(type);
            free(scope);
            free(source);
            free(user);
            free(task);
            free(kind);
            free(layer);
            free(title);
            free(derived_title);
            free(summary);
            free(entity_key);
            free(predicate);
            free(payload);
            free(content);
            free(supersedes);
            free(derived_from);
            free(context_json);
            free_anchor_qns(about_code_qns, about_code_n);
            return cbm_mcp_text_result(
                relation_rc == CBM_STORE_NOT_FOUND
                    ? "derived_from target must exist, be non-deleted, active/candidate, and in "
                      "the same project store"
                    : "failed to materialize derived_from relation; transaction rolled back",
                true);
        }
    }

    /* Explicit code anchoring: an optional "about_code" array of qualified_names
     * links this memory to code symbols (about_code edges). Written inside the
     * same transaction as the candidate so anchoring is atomic with the write.
     * Unknown qns are allowed — the symbol may be indexed later, and recall does
     * a lazy existence check anyway. */
    if (item_id) {
        for (size_t ai = 0; ai < about_code_n; ai++) {
            if (about_code_qns[ai]) {
                (void)cbm_store_memory_link_code(store, item_id, about_code_qns[ai], "user");
            }
        }
    }

    /* Confidence/reusability via the consolidated 3-tier composition
     * (cbm_memory_score_item, memory_store.c): L1 graph signal from about_code
     * anchors ⊕ L2 kind prior ⊕ L3 declared offset. Runs for EVERY write so an
     * unanchored item still gets its kind baseline; the composition is monotonic
     * (a tier only raises), so an anchored low-degree ADR keeps at least its kind
     * prior instead of scoring below an unanchored one and decaying out first.
     * The graph is borrowed only when anchors exist (NULL for pure-memory
     * projects → no L1, never an error). The result stays a LIVE value a later
     * falsification (memory_feedback/supersede/decay) can pull down. */
    if (item_id) {
        int resolved = 0;
        double l1_conf = 0.0;
        double l1_reuse = 0.0;
        if (about_code_n > 0) {
            cbm_store_t *graph = resolve_store(srv, project);
            sqlite3 *graph_db = graph ? cbm_store_get_db(graph) : NULL;
            if (graph_db) {
                resolved = cbm_store_memory_score_from_anchors(store, graph_db, item_id, project,
                                                               &l1_conf, &l1_reuse);
            }
        }
        cbm_memory_score_t sc = cbm_memory_score_item(kind ? kind : "event", resolved, l1_conf,
                                                      l1_reuse, confidence, reusability);
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(cbm_store_get_db(store),
                               "UPDATE memory_item SET confidence=?1,reusability=?2 WHERE id=?3;",
                               -1, &up, NULL) == SQLITE_OK) {
            sqlite3_bind_double(up, 1, sc.confidence);
            sqlite3_bind_double(up, 2, sc.reusability);
            sqlite3_bind_text(up, 3, item_id, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(up);
            sqlite3_finalize(up);
        }
    }

    /* P4 recall=latest-link: an explicit `supersedes` retires the old ADR from
     * the recall mainline. The merge path already archives the item it retires;
     * the explicit-supersede path must do the same, or the superseded ADR stays
     * active and both versions surface. Archive (not delete) the target: it
     * leaves recall but stays on disk as history, still shielded by the purge
     * red line. Scope-guarded; only an active/candidate target is touched.
     * Direct UPDATE (not cbm_store_memory_update_status) because we are already
     * inside this handler's transaction and that helper opens its own. */
    if (supersedes && supersedes[0]) {
        sqlite3_stmt *sup = NULL;
        int arch_changed = 0;
        if (sqlite3_prepare_v2(
                cbm_store_get_db(store),
                "UPDATE memory_item SET status='archived', updated_at=?1 "
                "WHERE id=?2 AND scope_project=?3 AND status IN ('active','candidate');",
                -1, &sup, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(sup, 1, (int64_t)time(NULL) * 1000);
            sqlite3_bind_text(sup, 2, supersedes, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(sup, 3, project, -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(sup);
            arch_changed = sqlite3_changes(cbm_store_get_db(store));
            sqlite3_finalize(sup);
        }
        if (arch_changed == 0 && supersedes_found) {
            /* Target exists but could not be archived — already archived, wrong
             * status, or scope mismatch. Only report when the target was found
             * above; if it was never found, the primary warning already covers it. */
            supersedes_warning =
                "supersedes target found but could not be archived "
                "(status may not be active/candidate — already archived or retracted)";
        }
    }

    if (cbm_store_commit(store) != CBM_STORE_OK) {
        cbm_store_rollback(store);
        free(event_id);
        free(item_id);
        free(derived_edge_id);
        free(project);
        free(type);
        free(scope);
        free(source);
        free(user);
        free(task);
        free(kind);
        free(layer);
        free(title);
        free(derived_title);
        free(summary);
        free(entity_key);
        free(predicate);
        free(payload);
        free(content);
        free(supersedes);
        free(derived_from);
        free(context_json);
        free_anchor_qns(about_code_qns, about_code_n);
        return cbm_mcp_text_result("failed to commit memory transaction", true);
    }

    /* Lazy auto-maintenance: a single-user agent has no operator to call
     * admin_consolidate/admin_decay, so the write hot path opportunistically
     * triggers them when due. Runs AFTER commit (new candidate is visible and
     * no transaction is open). Best-effort — never fails the write. */
    cbm_memory_maintain_report_t maint = {0};
    (void)cbm_store_memory_maintain_if_due(store, project, &maint);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", "accepted");
    yyjson_mut_obj_add_str(doc, root, "policy_decision", policy_decision);
    yyjson_mut_obj_add_str(doc, root, "policy_reason", policy_reason ? policy_reason : "");
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id ? event_id : "");
    yyjson_mut_obj_add_str(doc, root, "item_id", item_id ? item_id : "");
    yyjson_mut_obj_add_str(doc, root, "item_status", "candidate");
    if (materialize_relation) {
        yyjson_mut_obj_add_bool(doc, root, "materialized_relation", true);
        yyjson_mut_obj_add_str(doc, root, "edge_id", derived_edge_id ? derived_edge_id : "");
        yyjson_mut_obj_add_str(doc, root, "source_event_id", event_id ? event_id : "");
    }
    /* P3-b anchoring dimension (HELPER, not gatekeeper): report whether this
     * memory got a code anchor, and for high-value decision-class kinds written
     * WITHOUT one, advise (never block) — an unanchored decision misses L1 graph
     * scoring and anchor-boost at recall, and can't ride the ADR↔code lifecycle.
     * Pure-memory projects legitimately have no graph, so this stays advice. */
    yyjson_mut_obj_add_bool(doc, root, "anchored", about_code_n > 0);
    if (about_code_n == 0 && kind &&
        (strcmp(kind, "decision") == 0 || strcmp(kind, "constraint") == 0 ||
         strcmp(kind, "lesson") == 0)) {
        yyjson_mut_obj_add_str(
            doc, root, "anchoring_advice",
            "no about_code anchor: this decision-class memory won't get graph-derived "
            "confidence/reusability or recall anchor-boost. If it concerns specific code, "
            "pass about_code=[\"<qualified_name>\", ...] so it anchors to the graph.");
    }
    /* P3-d structure dimension: loose ADR-structure nudge (advice only). */
    const char *struct_advice = memory_structure_advice(kind, content);
    if (struct_advice) {
        yyjson_mut_obj_add_str(doc, root, "structure_advice", struct_advice);
    }
    /* Supersedes chain integrity: warn when a supersedes target is missing or
     * could not be archived (see the version-query and archive-UPDATE blocks above). */
    if (supersedes_warning) {
        yyjson_mut_obj_add_str(doc, root, "supersedes_warning", supersedes_warning);
    }
    yyjson_mut_obj_add_bool(doc, root, "maintained", maint.consolidated || maint.decayed);
    if (maint.consolidated) {
        yyjson_mut_obj_add_int(doc, root, "consolidated", maint.consolidate_count);
    }
    if (maint.decayed) {
        yyjson_mut_obj_add_int(doc, root, "decayed", maint.decay_count);
    }
    yyjson_mut_obj_add_str(
        doc, root, "hot_path",
        "event+structured candidate only; consolidation builds dedup, vectors, and evidence edges");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    free(event_id);
    free(item_id);
    free(derived_edge_id);
    free(project);
    free(type);
    free(scope);
    free(source);
    free(user);
    free(task);
    free(kind);
    free(layer);
    free(title);
    free(derived_title);
    free(summary);
    free(entity_key);
    free(predicate);
    free(payload);
    free(content);
    free(supersedes);
    free(derived_from);
    free(context_json);
    free_anchor_qns(about_code_qns, about_code_n);
    return result;
}
/* Scope-aware downweight for global (cross-project) memories in a project-scoped
 * recall: their retrieval_score is multiplied by this before merging with the
 * project store. <1.0 so project-specific hits win the limited top-K; >0 so a
 * strongly-relevant global memory can still surface. Tunable. */
#define MEMORY_GLOBAL_SCOPE_WEIGHT 0.5

/* qsort comparator: order memory item pointers by retrieval_score descending,
 * used to merge project-store and global-store result sets into one ranked list. */
static int memory_item_ptr_score_desc(const void *a, const void *b) {
    const cbm_memory_item_t *ia = *(const cbm_memory_item_t *const *)a;
    const cbm_memory_item_t *ib = *(const cbm_memory_item_t *const *)b;
    if (ia->retrieval_score < ib->retrieval_score)
        return 1;
    if (ia->retrieval_score > ib->retrieval_score)
        return -1;
    return 0;
}

static char *memory_observe_query_canonical(const char *project, const cbm_memory_query_t *query) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project ? project : "");
    yyjson_mut_obj_add_str(doc, root, "user", query->user ? query->user : "");
    yyjson_mut_obj_add_str(doc, root, "task", query->task ? query->task : "");
    yyjson_mut_obj_add_str(doc, root, "entity_key", query->entity_key ? query->entity_key : "");
    yyjson_mut_obj_add_str(doc, root, "kind", query->kind ? query->kind : "");
    yyjson_mut_obj_add_str(doc, root, "query", query->query ? query->query : "");
    yyjson_mut_obj_add_str(doc, root, "code_context",
                           query->code_context ? query->code_context : "");
    yyjson_mut_obj_add_bool(doc, root, "include_inactive", query->include_inactive);
    yyjson_mut_obj_add_int(doc, root, "limit", query->limit);
    char *result = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    return result;
}

static int memory_observe_record_batch(cbm_store_t *store,
                                       const cbm_retrieval_session_input_t *session_input,
                                       const cbm_memory_item_t *const *ranked, const int *positions,
                                       int count, const char *store_kind, const char *store_id,
                                       cbm_retrieval_observation_ref_t *all_refs,
                                       char **out_session_id, char **out_request_id) {
    bool replayed = false;
    int rc = cbm_store_memory_observe_session_begin(store, session_input, out_session_id,
                                                    out_request_id, &replayed);
    if (rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED) {
        return rc;
    }
    if (count == 0) {
        return CBM_STORE_OK;
    }
    cbm_retrieval_candidate_observation_t *observations =
        calloc((size_t)count, sizeof(*observations));
    cbm_retrieval_observation_ref_t *refs = calloc((size_t)count, sizeof(*refs));
    if (!observations || !refs) {
        free(observations);
        free(refs);
        return CBM_STORE_ERR;
    }
    for (int i = 0; i < count; i++) {
        const cbm_memory_item_t *item = ranked[positions[i]];
        observations[i].source_store_kind = store_kind;
        observations[i].source_store_id = store_id;
        observations[i].memory_item_id = item->id;
        observations[i].retrieval_source = item->retrieval_source;
        observations[i].source_rank = positions[i] + 1;
        observations[i].raw_score = item->retrieval_score;
        observations[i].normalized_score = item->retrieval_score;
        observations[i].aggregate_rank = positions[i] + 1;
        observations[i].decision_status = "selected";
        observations[i].evidence_json = item->evidence_json;
    }
    rc = cbm_store_memory_observe_candidates(store, *out_session_id, observations, count, refs);
    if (rc == CBM_STORE_OK) {
        for (int i = 0; i < count; i++) {
            all_refs[positions[i]] = refs[i];
            memset(&refs[i], 0, sizeof(refs[i]));
        }
    }
    cbm_store_memory_observation_refs_free(refs, count);
    free(refs);
    free(observations);
    return rc;
}

static void memory_concepts_add_json(yyjson_mut_doc *doc, yyjson_mut_val *root, cbm_store_t *store,
                                     const char *project, int limit) {
    yyjson_mut_val *concepts = yyjson_mut_arr(doc);
    int count = 0;
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT n.node_id,n.candidate_id,nv.content_text,nv.content_sha256 FROM concept_node n "
        "JOIN concept_node_version nv ON nv.node_id=n.node_id AND nv.version=(SELECT "
        "MAX(v2.version) "
        "FROM concept_node_version v2 WHERE v2.node_id=n.node_id) WHERE n.scope_project=?1 AND "
        "n.scope_store='project-memory' AND (SELECT action FROM concept_review_event r WHERE "
        "r.candidate_id=n.candidate_id ORDER BY r.sequence_no DESC LIMIT 1)='approve' ORDER BY "
        "n.node_id LIMIT ?2;";
    bool migrated = false;
    if (db && sqlite3_prepare_v2(db,
                                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                                 "name='concept_node';",
                                 -1, &stmt, NULL) == SQLITE_OK) {
        migrated = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (migrated && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, limit > 0 ? limit : 10);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *item = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, item, "node_id",
                                      (const char *)sqlite3_column_text(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc, item, "candidate_id",
                                      (const char *)sqlite3_column_text(stmt, 1));
            yyjson_mut_obj_add_strcpy(doc, item, "content",
                                      (const char *)sqlite3_column_text(stmt, 2));
            yyjson_mut_obj_add_strcpy(doc, item, "content_sha256",
                                      (const char *)sqlite3_column_text(stmt, 3));
            yyjson_mut_obj_add_bool(doc, item, "untrusted_data", true);
            yyjson_mut_arr_add_val(concepts, item);
            count++;
        }
    }
    sqlite3_finalize(stmt);
    yyjson_mut_obj_add_int(doc, root, "concept_count", count);
    yyjson_mut_obj_add_val(doc, root, "concepts", concepts);
}

static char *memory_global_retrieval_result(const char *project,
                                            const cbm_project_resolution_t *resolution,
                                            const cbm_global_retrieval_result_t *out, int rc) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return cbm_mcp_text_result("global retrieval serialization failed", true);
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project ? project : "");
    yyjson_mut_obj_add_str(doc, root, "project_uuid", resolution ? resolution->project_uuid : "");
    yyjson_mut_obj_add_str(doc, root, "retrieval_session_id",
                           out && out->session_id ? out->session_id : "");
    yyjson_mut_obj_add_str(doc, root, "request_id", out && out->session_id ? out->session_id : "");
    yyjson_mut_obj_add_str(doc, root, "mode", "observe_only");
    yyjson_mut_obj_add_bool(doc, root, "untrusted_data", true);
    yyjson_mut_obj_add_str(doc, root, "journal_status",
                           rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED ? "completed" : "failed");
    yyjson_mut_obj_add_str(doc, root, "candidate_pool", "global");
    yyjson_mut_obj_add_int(doc, root, "total", out ? out->total : 0);
    yyjson_mut_obj_add_int(doc, root, "count", out ? out->count : 0);
    yyjson_mut_val *memories = yyjson_mut_arr(doc);
    for (int i = 0; out && i < out->count; i++) {
        const cbm_global_candidate_t *candidate = &out->items[i];
        yyjson_mut_val *item = memory_item_to_json(doc, &candidate->item);
        yyjson_mut_obj_add_str(doc, item, "project_uuid",
                               candidate->project_uuid ? candidate->project_uuid : "");
        if (candidate->legacy_project_id)
            yyjson_mut_obj_add_str(doc, item, "legacy_project_id", candidate->legacy_project_id);
        yyjson_mut_obj_add_str(doc, item, "source_kind",
                               candidate->source_kind ? candidate->source_kind : "global_memory");
        yyjson_mut_obj_add_int(doc, item, "project_soft_boost_ppm",
                               candidate->project_soft_boost_ppm);
        yyjson_mut_obj_add_real(doc, item, "global_score", candidate->global_score);
        yyjson_mut_obj_add_str(doc, item, "candidate_id",
                               candidate->candidate_id ? candidate->candidate_id : "");
        yyjson_mut_obj_add_str(doc, item, "provenance_id",
                               candidate->provenance_id ? candidate->provenance_id : "");
        yyjson_mut_obj_add_str(doc, item, "evidence_id",
                               candidate->evidence_id ? candidate->evidence_id : "");
        yyjson_mut_obj_add_str(doc, item, "content_hash",
                               candidate->content_hash ? candidate->content_hash : "");
        yyjson_mut_obj_add_str(doc, item, "provenance_kind", "direct");
        yyjson_mut_obj_add_bool(doc, item, "untrusted_data", true);
        yyjson_mut_val *states = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_str(doc, states, "retrieved");
        yyjson_mut_arr_add_str(doc, states, "selected");
        yyjson_mut_obj_add_val(doc, item, "states", states);
        yyjson_mut_arr_add_val(memories, item);
    }
    yyjson_mut_obj_add_val(doc, root, "memories", memories);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result =
        cbm_mcp_text_result(json ? json : "{}", rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED);
    free(json);
    return result;
}

char *handle_memories_retrieve(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *concept_mode = cbm_mcp_get_string_arg(args, "concept_mode");
    bool concept_enabled = concept_mode && strcmp(concept_mode, "enabled") == 0;
    if (concept_mode && strcmp(concept_mode, "off") != 0 && !concept_enabled) {
        free(project);
        free(concept_mode);
        return cbm_mcp_text_result("invalid concept_mode", true);
    }
    if (concept_enabled) {
        char guard[16] = {0};
        cbm_safe_getenv("CBM_STAGE10_CONCEPT_RECALL", guard, sizeof(guard), NULL);
        if (strcmp(guard, "1") != 0) {
            free(project);
            free(concept_mode);
            return cbm_mcp_text_result(
                "{\"status\":\"error\",\"code\":\"CONCEPT_RECALL_GUARD_MISSING\"}", true);
        }
    }
    char *requested_request_id = cbm_mcp_get_string_arg(args, "request_id");
    /* Collapse a phantom "<project>-memory" name onto the real project so both
     * the store handle and the scope_project SQL filter use the canonical name. */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }
    bool global_default = memory_global_default_project(project);
    cbm_store_t *store = global_default ? resolve_global_memory_store(srv, true)
                                        : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(concept_mode);
        free(requested_request_id);
        return _res;
    }
    /* Lazy auto-maintenance before reading, so a single-user agent sees freshly
     * consolidated/decayed state without ever calling admin endpoints. At the
     * entry point no transaction is open. Best-effort — never fails the read. */
    if (!global_default)
        (void)cbm_store_memory_maintain_if_due(store, project, NULL);
    cbm_memory_query_t query = {0};
    query.project = project;
    query.user = cbm_mcp_get_string_arg(args, "user");
    query.task = cbm_mcp_get_string_arg(args, "task");
    query.entity_key = cbm_mcp_get_string_arg(args, "entity_key");
    query.kind = cbm_mcp_get_string_arg(args, "kind");
    query.query = cbm_mcp_get_string_arg(args, "query");
    query.code_context = cbm_mcp_get_string_arg(args, "code_context");
    query.activation_mode = cbm_mcp_get_string_arg(args, "activation_mode");
    query.activation_session_id = requested_request_id;
    query.activation_max_hops = cbm_mcp_get_int_arg(args, "activation_max_hops", 0);
    query.activation_max_nodes = cbm_mcp_get_int_arg(args, "activation_max_nodes", 0);
    query.activation_max_visits = cbm_mcp_get_int_arg(args, "activation_max_visits", 0);
    query.activation_token_budget = cbm_mcp_get_int_arg(args, "activation_token_budget", 0);
    query.activation_latency_ms = cbm_mcp_get_int_arg(args, "activation_latency_ms", 0);
    query.include_inactive = cbm_mcp_get_bool_arg(args, "include_inactive");
    query.limit = cbm_mcp_get_int_arg(args, "limit", 10);
    if (global_default) {
        cbm_project_resolution_t resolution = {0};
        char ensure_key[96], request_id[128], *ensure_report = NULL;
        int rc = memory_stage14_resolve_project(store, project, &resolution);
        if (rc == CBM_STORE_OK) {
            snprintf(ensure_key, sizeof(ensure_key), "stage14-mcp-project-%.64s",
                     resolution.path_hash);
            int ensure_rc =
                cbm_global_store_ensure_project(store, &resolution, ensure_key, &ensure_report);
            free(ensure_report);
            if (ensure_rc != CBM_STORE_OK && ensure_rc != CBM_STORE_REPLAYED)
                rc = ensure_rc;
        }
        if (requested_request_id && requested_request_id[0]) {
            snprintf(request_id, sizeof(request_id), "%s", requested_request_id);
        } else {
            snprintf(request_id, sizeof(request_id), "stage14-mcp-%llu",
                     (unsigned long long)cbm_now_ns());
        }
        query.project = NULL;
        cbm_global_retrieval_result_t global_out = {0};
        if (rc == CBM_STORE_OK)
            rc = cbm_global_store_retrieve(store, request_id, resolution.project_uuid, 100000,
                                           &query, &global_out);
        char *result = memory_global_retrieval_result(project, &resolution, &global_out, rc);
        cbm_global_retrieval_result_free(&global_out);
        free(project);
        free(concept_mode);
        free(requested_request_id);
        free((char *)query.user);
        free((char *)query.task);
        free((char *)query.entity_key);
        free((char *)query.kind);
        free((char *)query.query);
        free((char *)query.code_context);
        free((char *)query.activation_mode);
        return result;
    }
    /* Anchor-boost (about_code) needs the code graph, which now lives in a
     * separate DB. Borrow the project's graph handle only when a code_context is
     * given; absence/unindexed graph degrades to "no boost", never an error. */
    if (query.code_context && query.code_context[0]) {
        cbm_store_t *graph = resolve_store(srv, project);
        if (graph) {
            query.graph_db = cbm_store_get_db(graph);
        }
    }
    cbm_memory_result_t out = {0};
    int rc = cbm_store_memory_retrieve(store, &query, &out);

    /* Union the global (cross-project) store: scope_project=NULL memories live
     * in __global__-memory.db and must surface from every project. Query it with
     * project=NULL (the global rows have no project scope) and merge by score.
     * Purely additive — only runs if the global store exists; never gates or
     * errors the project read, and the project-required guard above still
     * protects against mistyped project names. */
    cbm_memory_result_t gout = {0};
    bool have_global = false;
    /* Test/isolation switch: CBM_MEMORY_NO_GLOBAL_UNION=1 skips the global union
     * so a project-scoped recall measures ONLY the project store. The recall eval
     * sets this to stay deterministic (its baseline predates the global store);
     * production leaves it unset so global memories surface everywhere. */
    char no_union[8];
    cbm_safe_getenv("CBM_MEMORY_NO_GLOBAL_UNION", no_union, sizeof(no_union), NULL);
    cbm_store_t *gstore = (no_union[0] == '1') ? NULL : resolve_global_memory_store(srv, false);
    if (gstore) {
        (void)cbm_store_memory_maintain_if_due(gstore, CBM_GLOBAL_MEMORY_PROJECT, NULL);
        cbm_memory_query_t gquery = query;
        gquery.project = NULL;         /* global rows carry scope_project=NULL */
        gquery.activation_mode = NULL; /* Stage 6 never crosses physical stores. */
        gquery.activation_session_id = NULL;
        if (cbm_store_memory_retrieve(gstore, &gquery, &gout) == CBM_STORE_OK) {
            have_global = true;
            /* Scope-aware downweight (B): global memories are cross-project,
             * project-agnostic public info, AND are already injected every turn
             * by the recall hook. So in a PROJECT-scoped query they must not
             * compete head-to-head for the limited top-K — multiply their score
             * by MEMORY_GLOBAL_SCOPE_WEIGHT so a project-specific hit wins, while
             * a genuinely dominant global hit can still surface. */
            for (int i = 0; i < gout.count; i++)
                gout.items[i].retrieval_score *= MEMORY_GLOBAL_SCOPE_WEIGHT;
        }
    }

    /* Build one ranked pointer list across both stores, sorted by retrieval
     * score, truncated to the requested limit. Pointers borrow item storage
     * from `out`/`gout`; both are freed after the JSON is serialized. */
    int merged_total = out.total + (have_global ? gout.total : 0);
    int combined = out.count + (have_global ? gout.count : 0);
    const cbm_memory_item_t **ranked =
        combined > 0 ? calloc((size_t)combined, sizeof(*ranked)) : NULL;
    int nranked = 0;
    if (ranked) {
        for (int i = 0; i < out.count; i++)
            ranked[nranked++] = &out.items[i];
        if (have_global)
            for (int i = 0; i < gout.count; i++)
                ranked[nranked++] = &gout.items[i];
        qsort(ranked, (size_t)nranked, sizeof(*ranked), memory_item_ptr_score_desc);
        if (query.limit > 0 && nranked > query.limit)
            nranked = query.limit;
    }

    cbm_retrieval_observation_ref_t *observe_refs =
        nranked > 0 ? calloc((size_t)nranked, sizeof(*observe_refs)) : NULL;
    int observe_ref_count = nranked;
    int *project_positions = nranked > 0 ? calloc((size_t)nranked, sizeof(int)) : NULL;
    int *global_positions = nranked > 0 ? calloc((size_t)nranked, sizeof(int)) : NULL;
    int project_count = 0;
    int global_count = 0;
    for (int i = 0; i < nranked; i++) {
        if (ranked[i]->scope_project) {
            project_positions[project_count++] = i;
        } else {
            global_positions[global_count++] = i;
        }
    }

    char *canonical_query = memory_observe_query_canonical(project, &query);
    char *session_id = NULL;
    char *journal_request_id = NULL;
    char *global_session_id = NULL;
    char *global_request_id = NULL;
    cbm_store_t *journal_store = NULL;
    cbm_store_t *journal_global_store = NULL;
    bool journal_ok = rc == CBM_STORE_OK && canonical_query &&
                      (nranked == 0 || (observe_refs && project_positions && global_positions));
    if (journal_ok) {
        journal_store = resolve_memory_store(srv, project, true);
        cbm_retrieval_session_input_t session_input = {0};
        session_input.request_id = requested_request_id;
        session_input.project_scope = project;
        session_input.memory_scope = global_count > 0 ? "mixed" : "project";
        session_input.algorithm_version = "stage5-observe-only-v1";
        session_input.config_version = 1;
        session_input.query_text = canonical_query;
        journal_ok = journal_store && memory_observe_record_batch(
                                          journal_store, &session_input, ranked, project_positions,
                                          project_count, "project", project, observe_refs,
                                          &session_id, &journal_request_id) == CBM_STORE_OK;
        if (journal_ok && global_count > 0) {
            journal_global_store = resolve_global_memory_store(srv, true);
            session_input.request_id = journal_request_id;
            journal_ok = journal_global_store &&
                         memory_observe_record_batch(
                             journal_global_store, &session_input, ranked, global_positions,
                             global_count, "global", CBM_GLOBAL_MEMORY_PROJECT, observe_refs,
                             &global_session_id, &global_request_id) == CBM_STORE_OK &&
                         global_session_id && strcmp(session_id, global_session_id) == 0;
        }
        if (journal_ok) {
            journal_ok = cbm_store_memory_observe_session_complete(
                             journal_store, session_id, "completed", NULL) == CBM_STORE_OK;
            if (journal_ok && journal_global_store) {
                journal_ok =
                    cbm_store_memory_observe_session_complete(
                        journal_global_store, global_session_id, "completed", NULL) == CBM_STORE_OK;
            }
        }
        if (!journal_ok) {
            if (journal_store && session_id)
                (void)cbm_store_memory_observe_session_complete(journal_store, session_id, "failed",
                                                                "JOURNAL_WRITE_FAILED");
            if (journal_global_store && global_session_id)
                (void)cbm_store_memory_observe_session_complete(
                    journal_global_store, global_session_id, "failed", "JOURNAL_WRITE_FAILED");
        }
    }
    if (!journal_ok) {
        rc = CBM_STORE_ERR;
        nranked = 0;
    }

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "retrieval_session_id", session_id ? session_id : "");
    yyjson_mut_obj_add_str(doc, root, "request_id", journal_request_id ? journal_request_id : "");
    yyjson_mut_obj_add_str(doc, root, "mode", "observe_only");
    yyjson_mut_obj_add_bool(doc, root, "untrusted_data", true);
    yyjson_mut_obj_add_str(doc, root, "journal_status", journal_ok ? "completed" : "failed");
    yyjson_mut_obj_add_int(doc, root, "total", merged_total);
    yyjson_mut_obj_add_int(doc, root, "count", nranked);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    for (int i = 0; i < nranked; i++) {
        yyjson_mut_val *item = memory_item_to_json(doc, ranked[i]);
        yyjson_mut_obj_add_str(doc, item, "candidate_id", observe_refs[i].candidate_id);
        yyjson_mut_obj_add_str(doc, item, "provenance_id", observe_refs[i].provenance_id);
        yyjson_mut_obj_add_str(doc, item, "evidence_id", observe_refs[i].evidence_id);
        yyjson_mut_obj_add_str(doc, item, "content_hash", observe_refs[i].content_hash);
        yyjson_mut_obj_add_str(doc, item, "provenance_kind",
                               ranked[i]->retrieval_source &&
                                       strcmp(ranked[i]->retrieval_source, "graph") == 0
                                   ? "indirect"
                                   : "direct");
        yyjson_mut_obj_add_bool(doc, item, "untrusted_data", true);
        yyjson_mut_val *states = yyjson_mut_arr(doc);
        yyjson_mut_arr_add_str(doc, states, "retrieved");
        yyjson_mut_arr_add_str(doc, states, "selected");
        yyjson_mut_obj_add_val(doc, item, "states", states);
        yyjson_mut_arr_add_val(arr, item);
    }
    yyjson_mut_obj_add_val(doc, root, "memories", arr);
    if (concept_enabled)
        memory_concepts_add_json(doc, root, store, project, query.limit);
    if (out.activation.status) {
        const char *field_name = out.activation.mode && strcmp(out.activation.mode, "active") == 0
                                     ? "activation"
                                     : "activation_shadow";
        memory_activation_add_json(doc, root, &out.activation, field_name);
    }
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    cbm_store_memory_observation_refs_free(observe_refs, observe_ref_count);
    free(observe_refs);
    free(project_positions);
    free(global_positions);
    free(canonical_query);
    free(session_id);
    free(journal_request_id);
    free(global_session_id);
    free(global_request_id);
    free(ranked);
    cbm_store_memory_result_free(&out);
    cbm_store_memory_result_free(&gout);
    free(project);
    free(concept_mode);
    free(requested_request_id);
    free((char *)query.user);
    free((char *)query.task);
    free((char *)query.entity_key);
    free((char *)query.kind);
    free((char *)query.query);
    free((char *)query.code_context);
    free((char *)query.activation_mode);
    return result;
}

char *handle_memories_inspect(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    /* scope='global' inspects the cross-project store (scope_project=NULL rows);
     * otherwise the per-project store. The SQL below binds the matching scope. */
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, false)
                                   : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    char *status = cbm_mcp_get_string_arg(args, "status");
    int limit = cbm_mcp_get_int_arg(args, "limit", 50);

    const char *cols = "m.id,m.entity_key,m.predicate,m.status,m.kind,m.layer,"
                       "m.title,m.hit_count,m.last_hit_at,m.confidence,m.version,m.updated_at";
    char sql[CBM_SZ_2K];
    snprintf(sql, sizeof(sql),
             "SELECT %s FROM memory_item m WHERE (?1 IS NULL OR m.scope_project=?1) "
             "AND (?2 IS NULL OR m.status=?2) "
             "ORDER BY m.updated_at DESC LIMIT ?3;",
             cols);
    sqlite3 *db = cbm_store_get_db(store);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, CBM_NOT_FOUND, &stmt, NULL) != SQLITE_OK) {
        free(project);
        free(status);
        return cbm_mcp_text_result("inspect query failed", true);
    }
    /* Global inspect binds NULL for the project filter (matches scope_project=NULL
     * rows); project inspect binds the project name. */
    if (is_global) {
        sqlite3_bind_null(stmt, 1);
    } else {
        sqlite3_bind_text(stmt, 1, project, -1, SQLITE_TRANSIENT);
    }
    if (status && status[0]) {
        sqlite3_bind_text(stmt, 2, status, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 2);
    }
    sqlite3_bind_int(stmt, 3, limit > 0 ? limit : 50);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_val *arr = yyjson_mut_arr(doc);
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < limit) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, obj, "id", (const char *)sqlite3_column_text(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc, obj, "entity_key",
                                  (const char *)sqlite3_column_text(stmt, 1));
        yyjson_mut_obj_add_strcpy(doc, obj, "predicate",
                                  (const char *)sqlite3_column_text(stmt, 2));
        yyjson_mut_obj_add_strcpy(doc, obj, "status", (const char *)sqlite3_column_text(stmt, 3));
        yyjson_mut_obj_add_strcpy(doc, obj, "kind", (const char *)sqlite3_column_text(stmt, 4));
        yyjson_mut_obj_add_strcpy(doc, obj, "layer", (const char *)sqlite3_column_text(stmt, 5));
        yyjson_mut_obj_add_strcpy(doc, obj, "title", (const char *)sqlite3_column_text(stmt, 6));
        yyjson_mut_obj_add_int(doc, obj, "hit_count", sqlite3_column_int(stmt, 7));
        yyjson_mut_obj_add_int(doc, obj, "last_hit_at", sqlite3_column_int64(stmt, 8));
        yyjson_mut_obj_add_real(doc, obj, "confidence", sqlite3_column_double(stmt, 9));
        yyjson_mut_obj_add_int(doc, obj, "version", sqlite3_column_int(stmt, 10));
        yyjson_mut_obj_add_int(doc, obj, "updated_at", sqlite3_column_int64(stmt, 11));
        yyjson_mut_arr_add_val(arr, obj);
        n++;
    }
    yyjson_mut_obj_add_val(doc, root, "items", arr);
    yyjson_mut_obj_add_int(doc, root, "count", n);
    sqlite3_finalize(stmt);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    free(project);
    free(status);
    return result;
}

char *handle_memory_update_status(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *id = cbm_mcp_get_string_arg(args, "id");
    char *status = cbm_mcp_get_string_arg(args, "status");
    if (!project || !id || !status) {
        free(project);
        free(id);
        free(status);
        return cbm_mcp_text_result("project, id, and status are required", true);
    }
    cbm_store_t *store = resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(id);
        free(status);
        return _res;
    }
    int rc = cbm_store_memory_update_status(store, id, project, status);
    /* By-id ops are scope-guarded on project, so a global memory (scope_project
     * =NULL) is NOT_FOUND in the project store. Fall back to the global store
     * with project=NULL, which its (?4 IS NULL OR scope_project=?4) clause accepts. */
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *gstore = resolve_global_memory_store(srv, false);
        if (gstore)
            rc = cbm_store_memory_update_status(gstore, id, NULL, status);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "status",
                           rc == CBM_STORE_NOT_FOUND ? "not_found"
                                                     : (rc == CBM_STORE_OK ? "updated" : "error"));
    yyjson_mut_obj_add_str(doc, root, "item_status", status);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    free(id);
    free(status);
    return result;
}

typedef struct {
    char *project, *processing_mode, *event_id, *task_id, *task_type;
    char *session_id, *candidate_id, *injection_id, *usage_id;
    char *result_id, *result_type, *result_status, *result_ref, *result_hash, *result_payload;
    char *evidence_id, *evidence_trust, *evidence_state, *evidence_source;
    char *evidence_ref, *evidence_hash, *evidence_payload;
    char *action, *edge_id, *supersedes_event_id, *algorithm_version;
    int config_version;
} mcp_stage7_feedback_args_t;

static void mcp_stage7_feedback_args_free(mcp_stage7_feedback_args_t *a) {
    if (!a)
        return;
    free(a->project);
    free(a->processing_mode);
    free(a->event_id);
    free(a->task_id);
    free(a->task_type);
    free(a->session_id);
    free(a->candidate_id);
    free(a->injection_id);
    free(a->usage_id);
    free(a->result_id);
    free(a->result_type);
    free(a->result_status);
    free(a->result_ref);
    free(a->result_hash);
    free(a->result_payload);
    free(a->evidence_id);
    free(a->evidence_trust);
    free(a->evidence_state);
    free(a->evidence_source);
    free(a->evidence_ref);
    free(a->evidence_hash);
    free(a->evidence_payload);
    free(a->action);
    free(a->edge_id);
    free(a->supersedes_event_id);
    free(a->algorithm_version);
    memset(a, 0, sizeof(*a));
}

static char *mcp_stage7_feedback_error(const char *status, const char *code, const char *event_id,
                                       const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_str(doc, root, "code", code);
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id ? event_id : "");
    yyjson_mut_obj_add_str(doc, root, "processing_mode", "observe_only");
    if (message)
        yyjson_mut_obj_add_str(doc, root, "message", message);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, true);
    free(json);
    return result;
}

char *handle_memory_feedback(cbm_mcp_server_t *srv, const char *args) {
    mcp_stage7_feedback_args_t a = {0};
    a.project = cbm_mcp_get_string_arg(args, "project");
    a.processing_mode = cbm_mcp_get_string_arg(args, "processing_mode");
    a.event_id = cbm_mcp_get_string_arg(args, "event_id");
    a.task_id = cbm_mcp_get_string_arg(args, "task_id");
    a.task_type = cbm_mcp_get_string_arg(args, "task_type");
    a.session_id = cbm_mcp_get_string_arg(args, "session_id");
    a.candidate_id = cbm_mcp_get_string_arg(args, "candidate_id");
    a.injection_id = cbm_mcp_get_string_arg(args, "injection_id");
    a.usage_id = cbm_mcp_get_string_arg(args, "usage_id");
    a.result_id = cbm_mcp_get_string_arg(args, "result_id");
    a.result_type = cbm_mcp_get_string_arg(args, "result_type");
    a.result_status = cbm_mcp_get_string_arg(args, "result_status");
    a.result_ref = cbm_mcp_get_string_arg(args, "result_ref");
    a.result_hash = cbm_mcp_get_string_arg(args, "result_hash");
    a.result_payload = cbm_mcp_get_string_arg(args, "result_payload");
    a.evidence_id = cbm_mcp_get_string_arg(args, "evidence_id");
    a.evidence_trust = cbm_mcp_get_string_arg(args, "evidence_trust");
    a.evidence_state = cbm_mcp_get_string_arg(args, "evidence_state");
    a.evidence_source = cbm_mcp_get_string_arg(args, "evidence_source");
    a.evidence_ref = cbm_mcp_get_string_arg(args, "evidence_ref");
    a.evidence_hash = cbm_mcp_get_string_arg(args, "evidence_hash");
    a.evidence_payload = cbm_mcp_get_string_arg(args, "evidence_payload");
    a.action = cbm_mcp_get_string_arg(args, "action");
    a.edge_id = cbm_mcp_get_string_arg(args, "edge_id");
    a.supersedes_event_id = cbm_mcp_get_string_arg(args, "supersedes_event_id");
    a.algorithm_version = cbm_mcp_get_string_arg(args, "algorithm_version");
    a.config_version = cbm_mcp_get_int_arg(args, "config_version", -1);
    bool complete = a.project && a.processing_mode && a.event_id && a.task_id && a.task_type &&
                    a.session_id && a.candidate_id && a.usage_id && a.result_id && a.result_type &&
                    a.result_status && a.result_ref && a.result_hash && a.result_payload &&
                    a.evidence_id && a.evidence_trust && a.evidence_state && a.evidence_source &&
                    a.evidence_ref && a.evidence_hash && a.evidence_payload && a.action &&
                    a.algorithm_version && a.config_version >= 0;
    if (!complete) {
        char *result =
            mcp_stage7_feedback_error("error", "STAGE7_SCHEMA_REQUIRED", a.event_id,
                                      "complete Stage 7 observe-only feedback fields are required");
        mcp_stage7_feedback_args_free(&a);
        return result;
    }
    cbm_store_t *store = memory_stage14_store(srv, a.project, false);
    if (store)
        store = memory_stage14_store(srv, a.project, true);
    if (!store) {
        char *result = mcp_stage7_feedback_error("error", "PROJECT_NOT_FOUND", a.event_id,
                                                 "project not found or not indexed");
        mcp_stage7_feedback_args_free(&a);
        return result;
    }
    cbm_feedback_observe_input_t input = {0};
    char feedback_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    input.project = memory_stage14_project_identity(store, a.project, feedback_project_uuid);
    input.processing_mode = a.processing_mode;
    input.event_id = a.event_id;
    input.task_id = a.task_id;
    input.task_type = a.task_type;
    input.session_id = a.session_id;
    input.candidate_id = a.candidate_id;
    input.injection_id = a.injection_id;
    input.usage_id = a.usage_id;
    input.result_id = a.result_id;
    input.result_type = a.result_type;
    input.result_status = a.result_status;
    input.result_ref = a.result_ref;
    input.result_hash = a.result_hash;
    input.result_payload = a.result_payload;
    input.evidence_id = a.evidence_id;
    input.evidence_trust = a.evidence_trust;
    input.evidence_state = a.evidence_state;
    input.evidence_source = a.evidence_source;
    input.evidence_ref = a.evidence_ref;
    input.evidence_hash = a.evidence_hash;
    input.evidence_payload = a.evidence_payload;
    input.action = a.action;
    input.edge_id = a.edge_id;
    input.supersedes_event_id = a.supersedes_event_id;
    input.algorithm_version = a.algorithm_version;
    input.config_version = a.config_version;
    cbm_feedback_observe_result_t observed = {0};
    int rc = cbm_store_memory_feedback_observe(store, &input, &observed);
    char *result = NULL;
    if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
        result = cbm_mcp_text_result(observed.result_json ? observed.result_json : "{}", false);
    } else if (rc == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        result = mcp_stage7_feedback_error("conflict", "IDEMPOTENCY_CONFLICT", a.event_id, NULL);
    } else {
        result =
            mcp_stage7_feedback_error("error", "INVALID_ARGUMENT_OR_ATTRIBUTION", a.event_id, NULL);
    }
    cbm_store_memory_feedback_observe_result_free(&observed);
    mcp_stage7_feedback_args_free(&a);
    return result;
}

static char *handle_stage14_task_evolution_control(cbm_mcp_server_t *srv, const char *args);

char *handle_memory_reinforcement_replay(cbm_mcp_server_t *srv, const char *args) {
    yyjson_doc *route_doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    yyjson_val *route_root = route_doc ? yyjson_doc_get_root(route_doc) : NULL;
    bool stage14_request =
        route_root && yyjson_is_obj(route_root) && yyjson_obj_get(route_root, "action") != NULL;
    yyjson_doc_free(route_doc);
    if (stage14_request)
        return handle_stage14_task_evolution_control(srv, args);

    char *project = cbm_mcp_get_string_arg(args, "project");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *algorithm = cbm_mcp_get_string_arg(args, "algorithm_version");
    int config = cbm_mcp_get_int_arg(args, "config_version", -1);
    if (!project || !mode || !algorithm || config < 0) {
        free(project);
        free(mode);
        free(algorithm);
        return cbm_mcp_text_result("complete Stage 8 replay fields are required", true);
    }
    cbm_store_t *store = memory_stage14_store(srv, project, false);
    if (store && strcmp(mode, "active") == 0) {
        store = memory_stage14_store(srv, project, true);
    }
    if (!store) {
        free(project);
        free(mode);
        free(algorithm);
        return cbm_mcp_text_result("project not found or not indexed", true);
    }
    cbm_edge_reinforcement_input_t input = {0};
    char reinforcement_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    input.project = memory_stage14_project_identity(store, project, reinforcement_project_uuid);
    input.mode = mode;
    input.algorithm_version = algorithm;
    input.config_version = config;
    cbm_edge_reinforcement_result_t replay = {0};
    int rc = cbm_store_memory_reinforcement_replay(store, &input, &replay);
    char *result = NULL;
    if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
        result = cbm_mcp_text_result(replay.report_json ? replay.report_json : "{}", false);
    } else if (rc == CBM_STORE_REJECTED) {
        result = cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"ACTIVE_FIXTURE_GUARD\","
                                     "\"production_state_written\":false}",
                                     true);
    } else if (rc == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        result = cbm_mcp_text_result("{\"status\":\"conflict\",\"code\":\"IDEMPOTENCY_CONFLICT\","
                                     "\"production_state_written\":false}",
                                     true);
    } else {
        result =
            cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"REINFORCEMENT_REPLAY_FAILED\","
                                "\"production_state_written\":false}",
                                true);
    }
    cbm_store_memory_reinforcement_result_free(&replay);
    free(project);
    free(mode);
    free(algorithm);
    return result;
}

static char *mcp_stage9_error(const char *code) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"status\":\"error\",\"code\":\"%s\",\"production_state_written\":false}",
             code ? code : "STAGE9_FAILED");
    return cbm_mcp_text_result(json, true);
}

static bool mcp_stage9_migration_guard(const char *project) {
    char value[32] = {0};
    if (project && strcmp(project, "stage9-fixture-edge-lifecycle-v1") == 0) {
        cbm_safe_getenv("CBM_STAGE9_ACTIVE_FIXTURE", value, sizeof(value), NULL);
        return strcmp(value, "1") == 0;
    }
    if (!project || strcmp(project, "H-Codex_H-neuroplastic-main") != 0)
        return false;
    char manifest[1024] = {0};
    char hash[80] = {0};
    cbm_safe_getenv("CBM_STAGE9_PRODUCTION_CANARY", value, sizeof(value), NULL);
    cbm_safe_getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST", manifest, sizeof(manifest), NULL);
    cbm_safe_getenv("CBM_STAGE9_PRODUCTION_CANARY_MANIFEST_SHA256", hash, sizeof(hash), NULL);
    return strcmp(value, "1") == 0 && manifest[0] && strlen(hash) == 64;
}

char *handle_memory_edge_lifecycle_migrate(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *algorithm = cbm_mcp_get_string_arg(args, "algorithm_version");
    char *policy_hash = cbm_mcp_get_string_arg(args, "policy_sha256");
    int policy_version = cbm_mcp_get_int_arg(args, "policy_version", -1);
    int config_version = cbm_mcp_get_int_arg(args, "config_version", -1);
    if (!project || !algorithm || !policy_hash ||
        strcmp(algorithm, CBM_STAGE9_ALGORITHM_VERSION) != 0 ||
        strcmp(policy_hash, CBM_STAGE9_POLICY_SHA256) != 0 ||
        policy_version != CBM_STAGE9_POLICY_VERSION ||
        config_version != CBM_STAGE9_CONFIG_VERSION) {
        free(project);
        free(algorithm);
        free(policy_hash);
        return mcp_stage9_error("INVALID_STAGE9_CONTRACT");
    }
    if (!mcp_stage9_migration_guard(project)) {
        free(project);
        free(algorithm);
        free(policy_hash);
        return mcp_stage9_error("ACTIVE_FIXTURE_GUARD");
    }
    cbm_store_t *store = memory_stage14_store(srv, project, true);
    if (!store) {
        free(project);
        free(algorithm);
        free(policy_hash);
        return mcp_stage9_error("PROJECT_NOT_FOUND");
    }
    int before = cbm_store_memory_stage9_object_count(store);
    int rc = cbm_store_memory_stage9_migrate(store);
    int after = cbm_store_memory_stage9_object_count(store);
    char json[384];
    snprintf(json, sizeof(json),
             "{\"status\":\"%s\",\"stage9_object_count_before\":%d,"
             "\"stage9_object_count_after\":%d,\"production_state_written\":%s}",
             rc == CBM_STORE_OK ? (before == 22 ? "replayed" : "migrated") : "error", before, after,
             rc == CBM_STORE_OK && before == 0 &&
                     strcmp(project, "H-Codex_H-neuroplastic-main") == 0
                 ? "true"
                 : "false");
    char *result = rc == CBM_STORE_OK ? cbm_mcp_text_result(json, false)
                                      : mcp_stage9_error(rc == CBM_STORE_IDEMPOTENCY_CONFLICT
                                                             ? "MIGRATION_LEDGER_CONFLICT"
                                                             : "STAGE9_MIGRATION_FAILED");
    free(project);
    free(algorithm);
    free(policy_hash);
    return result;
}

static int64_t mcp_stage9_sint(yyjson_doc *doc, const char *key, int64_t def) {
    yyjson_val *value = memory_arg(doc, key);
    return value && yyjson_is_int(value) ? yyjson_get_sint(value) : def;
}

char *handle_memory_edge_maintenance(cbm_mcp_server_t *srv, const char *args) {
    yyjson_doc *doc = yyjson_read(args ? args : "{}", args ? strlen(args) : 2, 0);
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *run_id = cbm_mcp_get_string_arg(args, "run_id");
    char *algorithm = cbm_mcp_get_string_arg(args, "algorithm_version");
    char *policy_hash = cbm_mcp_get_string_arg(args, "policy_sha256");
    char *manifest_path = cbm_mcp_get_string_arg(args, "manifest_path");
    char *manifest_sha256 = cbm_mcp_get_string_arg(args, "manifest_sha256");
    int64_t as_of_ms = mcp_stage9_sint(doc, "as_of_ms", -1);
    int policy_version = cbm_mcp_get_int_arg(args, "policy_version", -1);
    int config_version = cbm_mcp_get_int_arg(args, "config_version", -1);
    yyjson_doc_free(doc);
    if (!project || !mode || !run_id || !algorithm || !policy_hash || as_of_ms < 0 ||
        policy_version < 0 || config_version < 0) {
        free(project);
        free(mode);
        free(run_id);
        free(algorithm);
        free(policy_hash);
        free(manifest_path);
        free(manifest_sha256);
        return mcp_stage9_error("INVALID_STAGE9_CONTRACT");
    }
    cbm_store_t *store = memory_stage14_store(srv, project, strcmp(mode, "active") == 0);
    if (!store) {
        free(project);
        free(mode);
        free(run_id);
        free(algorithm);
        free(policy_hash);
        free(manifest_path);
        free(manifest_sha256);
        return mcp_stage9_error("PROJECT_NOT_FOUND");
    }
    cbm_edge_lifecycle_input_t input = {0};
    char maintenance_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    input.project = memory_stage14_project_identity(store, project, maintenance_project_uuid);
    input.mode = mode;
    input.run_id = run_id;
    input.as_of_ms = as_of_ms;
    input.algorithm_version = algorithm;
    input.policy_sha256 = policy_hash;
    input.policy_version = policy_version;
    input.config_version = config_version;
    input.manifest_path = manifest_path;
    input.manifest_sha256 = manifest_sha256;
    cbm_edge_lifecycle_result_t lifecycle = {0};
    int rc = cbm_store_memory_edge_maintenance(store, &input, &lifecycle);
    char *result = NULL;
    if (rc == CBM_STORE_OK) {
        result = cbm_mcp_text_result(lifecycle.report_json ? lifecycle.report_json : "{}", false);
    } else if (rc == CBM_STORE_REJECTED) {
        result = mcp_stage9_error("ACTIVE_FIXTURE_GUARD");
    } else if (rc == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        result = mcp_stage9_error("IDEMPOTENCY_CONFLICT");
    } else {
        result = mcp_stage9_error("EDGE_MAINTENANCE_FAILED");
    }
    cbm_store_memory_edge_lifecycle_result_free(&lifecycle);
    free(project);
    free(mode);
    free(run_id);
    free(algorithm);
    free(policy_hash);
    free(manifest_path);
    free(manifest_sha256);
    return result;
}

char *handle_memory_edge_restore(cbm_mcp_server_t *srv, const char *args) {
    yyjson_doc *doc = yyjson_read(args ? args : "{}", args ? strlen(args) : 2, 0);
    yyjson_val *edge_values = doc ? memory_arg(doc, "edge_ids") : NULL;
    size_t edge_count =
        edge_values && yyjson_is_arr(edge_values) ? yyjson_arr_size(edge_values) : 0;
    char **edge_ids = edge_count > 0 ? calloc(edge_count, sizeof(*edge_ids)) : NULL;
    if (edge_ids) {
        yyjson_arr_iter iter = yyjson_arr_iter_with(edge_values);
        yyjson_val *value = NULL;
        size_t index = 0;
        while ((value = yyjson_arr_iter_next(&iter)) && index < edge_count) {
            if (!yyjson_is_str(value))
                break;
            edge_ids[index++] = cbm_strdup(yyjson_get_str(value));
        }
    }
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *run_id = cbm_mcp_get_string_arg(args, "run_id");
    char *algorithm = cbm_mcp_get_string_arg(args, "algorithm_version");
    char *policy_hash = cbm_mcp_get_string_arg(args, "policy_sha256");
    char *manifest_path = cbm_mcp_get_string_arg(args, "manifest_path");
    char *manifest_sha256 = cbm_mcp_get_string_arg(args, "manifest_sha256");
    int64_t as_of_ms = mcp_stage9_sint(doc, "as_of_ms", -1);
    int policy_version = cbm_mcp_get_int_arg(args, "policy_version", -1);
    int config_version = cbm_mcp_get_int_arg(args, "config_version", -1);
    yyjson_doc_free(doc);
    if (!project || !mode || !run_id || !algorithm || !policy_hash || !edge_ids ||
        edge_count == 0 || as_of_ms < 0 || policy_version < 0 || config_version < 0) {
        for (size_t i = 0; i < edge_count; i++)
            free(edge_ids ? edge_ids[i] : NULL);
        free(edge_ids);
        free(project);
        free(mode);
        free(run_id);
        free(algorithm);
        free(policy_hash);
        free(manifest_path);
        free(manifest_sha256);
        return mcp_stage9_error("INVALID_STAGE9_RESTORE_CONTRACT");
    }
    cbm_store_t *store = memory_stage14_store(srv, project, strcmp(mode, "active") == 0);
    cbm_edge_lifecycle_restore_input_t input = {0};
    input.edge_ids = (const char *const *)edge_ids;
    input.edge_count = (int)edge_count;
    char restore_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    input.lifecycle.project = memory_stage14_project_identity(store, project, restore_project_uuid);
    input.lifecycle.mode = mode;
    input.lifecycle.run_id = run_id;
    input.lifecycle.as_of_ms = as_of_ms;
    input.lifecycle.algorithm_version = algorithm;
    input.lifecycle.policy_sha256 = policy_hash;
    input.lifecycle.policy_version = policy_version;
    input.lifecycle.config_version = config_version;
    input.lifecycle.manifest_path = manifest_path;
    input.lifecycle.manifest_sha256 = manifest_sha256;
    cbm_edge_lifecycle_result_t lifecycle = {0};
    int rc = store ? cbm_store_memory_edge_restore(store, &input, &lifecycle) : CBM_STORE_NOT_FOUND;
    char *result = NULL;
    if (rc == CBM_STORE_OK) {
        result = cbm_mcp_text_result(lifecycle.report_json ? lifecycle.report_json : "{}", false);
    } else if (rc == CBM_STORE_REJECTED) {
        result = mcp_stage9_error("ACTIVE_FIXTURE_GUARD");
    } else if (rc == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        result = mcp_stage9_error("IDEMPOTENCY_CONFLICT");
    } else if (rc == CBM_STORE_NOT_FOUND) {
        result = mcp_stage9_error("EDGE_NOT_FOUND_OR_SCOPE_MISMATCH");
    } else {
        result = mcp_stage9_error("EDGE_RESTORE_FAILED");
    }
    cbm_store_memory_edge_lifecycle_result_free(&lifecycle);
    for (size_t i = 0; i < edge_count; i++)
        free(edge_ids[i]);
    free(edge_ids);
    free(project);
    free(mode);
    free(run_id);
    free(algorithm);
    free(policy_hash);
    free(manifest_path);
    free(manifest_sha256);
    return result;
}

static char *mcp_stage10_error(const char *code) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"status\":\"error\",\"code\":\"%s\","
             "\"production_state_written\":false}",
             code ? code : "STAGE10_FAILED");
    return cbm_mcp_text_result(json, true);
}

static bool mcp_stage10_canary_guard(const char *project) {
    char enabled[16] = {0};
    if (project && strncmp(project, "stage10-fixture-", 16) == 0) {
        cbm_safe_getenv("CBM_STAGE10_ACTIVE_FIXTURE", enabled, sizeof(enabled), NULL);
        return strcmp(enabled, "1") == 0;
    }
    if (!project || strcmp(project, "H-Codex_H-neuroplastic-main") != 0)
        return false;
    char manifest[1024] = {0};
    char hash[80] = {0};
    cbm_safe_getenv("CBM_STAGE10_PRODUCTION_CANARY", enabled, sizeof(enabled), NULL);
    cbm_safe_getenv("CBM_STAGE10_PRODUCTION_CANARY_MANIFEST", manifest, sizeof(manifest), NULL);
    cbm_safe_getenv("CBM_STAGE10_PRODUCTION_CANARY_SHA256", hash, sizeof(hash), NULL);
    return strcmp(enabled, "1") == 0 && manifest[0] && strlen(hash) == 64;
}

char *handle_memory_concept_generate(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *store_name = cbm_mcp_get_string_arg(args, "store");
    char *operation = cbm_mcp_get_string_arg(args, "operation");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *run_id = cbm_mcp_get_string_arg(args, "run_id");
    char *idempotency_key = cbm_mcp_get_string_arg(args, "idempotency_key");
    char *algorithm = cbm_mcp_get_string_arg(args, "algorithm_version");
    char *policy_hash = cbm_mcp_get_string_arg(args, "policy_sha256");
    char *generator = cbm_mcp_get_string_arg(args, "generator_version");
    char *manifest_path = cbm_mcp_get_string_arg(args, "manifest_path");
    char *manifest_sha256 = cbm_mcp_get_string_arg(args, "manifest_sha256");
    int policy_version = cbm_mcp_get_int_arg(args, "policy_version", -1);
    int config_version = cbm_mcp_get_int_arg(args, "config_version", -1);
    bool contract = project && store_name && operation && mode && algorithm && policy_hash &&
                    generator && strcmp(store_name, "project-memory") == 0 &&
                    strcmp(algorithm, CBM_STAGE10_ALGORITHM_VERSION) == 0 &&
                    strcmp(policy_hash, CBM_STAGE10_POLICY_SHA256) == 0 &&
                    strcmp(generator, CBM_STAGE10_GENERATOR_VERSION) == 0 &&
                    policy_version == CBM_STAGE10_POLICY_VERSION &&
                    config_version == CBM_STAGE10_CONFIG_VERSION;
    char *result = NULL;
    if (!contract) {
        result = mcp_stage10_error("INVALID_STAGE10_CONTRACT");
        goto done;
    }
    if (strcmp(operation, "migrate") == 0) {
        if (strcmp(mode, "active") != 0 || !mcp_stage10_canary_guard(project)) {
            result = mcp_stage10_error("ACTIVE_FIXTURE_GUARD");
            goto done;
        }
        cbm_store_t *store = memory_stage14_store(srv, project, true);
        if (!store) {
            result = mcp_stage10_error("PROJECT_NOT_FOUND");
            goto done;
        }
        int before = cbm_store_memory_stage10_object_count(store);
        int rc = cbm_store_memory_stage10_migrate(store);
        int after = cbm_store_memory_stage10_object_count(store);
        if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
            char json[384];
            snprintf(json, sizeof(json),
                     "{\"status\":\"%s\",\"stage10_object_count_before\":%d,"
                     "\"stage10_object_count_after\":%d,\"production_state_written\":%s}",
                     rc == CBM_STORE_REPLAYED ? "replayed" : "migrated", before, after,
                     rc == CBM_STORE_OK ? "true" : "false");
            result = cbm_mcp_text_result(json, false);
        } else {
            result = mcp_stage10_error(rc == CBM_STORE_IDEMPOTENCY_CONFLICT
                                           ? "SCHEMA_HASH_MISMATCH"
                                           : "STAGE10_MIGRATION_FAILED");
        }
        goto done;
    }
    cbm_store_t *store = memory_stage14_store(srv, project, strcmp(mode, "active") == 0);
    if (!store) {
        result = mcp_stage10_error("PROJECT_NOT_FOUND");
        goto done;
    }
    cbm_concept_generate_input_t input = {0};
    char concept_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    input.project = memory_stage14_project_identity(store, project, concept_project_uuid);
    input.store = store_name;
    input.operation = operation;
    input.mode = mode;
    input.run_id = run_id;
    input.idempotency_key = idempotency_key;
    input.algorithm_version = algorithm;
    input.policy_sha256 = policy_hash;
    input.policy_version = policy_version;
    input.config_version = config_version;
    input.generator_version = generator;
    input.manifest_path = manifest_path;
    input.manifest_sha256 = manifest_sha256;
    cbm_concept_result_t generated = {0};
    int rc = cbm_store_memory_concept_generate(store, &input, &generated);
    if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
        result = cbm_mcp_text_result(generated.report_json ? generated.report_json : "{}", false);
    } else {
        result = mcp_stage10_error(generated.failure_code ? generated.failure_code
                                                          : "CONCEPT_GENERATION_FAILED");
    }
    cbm_store_memory_concept_result_free(&generated);
done:
    free(project);
    free(store_name);
    free(operation);
    free(mode);
    free(run_id);
    free(idempotency_key);
    free(algorithm);
    free(policy_hash);
    free(generator);
    free(manifest_path);
    free(manifest_sha256);
    return result;
}

char *handle_memory_concept_review(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *store_name = cbm_mcp_get_string_arg(args, "store");
    char *candidate_id = cbm_mcp_get_string_arg(args, "candidate_id");
    char *action = cbm_mcp_get_string_arg(args, "action");
    char *idempotency_key = cbm_mcp_get_string_arg(args, "idempotency_key");
    char *content_text = cbm_mcp_get_string_arg(args, "content_text");
    char *related_candidate_id = cbm_mcp_get_string_arg(args, "related_candidate_id");
    bool explicit_user = cbm_mcp_get_bool_arg(args, "explicit_user_confirmed");
    char guard[16] = {0};
    cbm_safe_getenv("CBM_STAGE10_REVIEW_EXPLICIT_USER", guard, sizeof(guard), NULL);
    if (!project || !store_name || !candidate_id || !action || !idempotency_key ||
        strcmp(store_name, "project-memory") != 0 || !explicit_user || strcmp(guard, "1") != 0) {
        free(project);
        free(store_name);
        free(candidate_id);
        free(action);
        free(idempotency_key);
        free(content_text);
        free(related_candidate_id);
        return mcp_stage10_error("REVIEW_REQUIRES_EXPLICIT_USER");
    }
    cbm_store_t *store = memory_stage14_store(srv, project, true);
    if (!store) {
        free(project);
        free(store_name);
        free(candidate_id);
        free(action);
        free(idempotency_key);
        free(content_text);
        free(related_candidate_id);
        return mcp_stage10_error("PROJECT_NOT_FOUND");
    }
    cbm_concept_review_input_t input = {0};
    char review_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    input.project = memory_stage14_project_identity(store, project, review_project_uuid);
    input.store = store_name;
    input.candidate_id = candidate_id;
    input.action = action;
    input.idempotency_key = idempotency_key;
    input.content_text = content_text;
    input.related_candidate_id = related_candidate_id;
    input.reviewer_source = "explicit_user";
    cbm_concept_result_t reviewed = {0};
    int rc = cbm_store_memory_concept_review(store, &input, &reviewed);
    char *result = NULL;
    if (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) {
        result = cbm_mcp_text_result(reviewed.report_json ? reviewed.report_json : "{}", false);
    } else {
        result = mcp_stage10_error(reviewed.failure_code ? reviewed.failure_code
                                                         : "CONCEPT_REVIEW_FAILED");
    }
    cbm_store_memory_concept_result_free(&reviewed);
    free(project);
    free(store_name);
    free(candidate_id);
    free(action);
    free(idempotency_key);
    free(content_text);
    free(related_candidate_id);
    return result;
}

char *handle_memory_learning_status(cbm_mcp_server_t *srv, const char *args) {
    char *project=cbm_mcp_get_string_arg(args,"project");
    cbm_store_t *store=project ? memory_stage14_store(srv,project,false):NULL;
    char uuid[CBM_PROJECT_UUID_SIZE]={0};
    const char *identity=store ? memory_stage14_project_identity(store,project,uuid):NULL;
    char *json=identity ? cbm_learning_status(store,identity):NULL;
    char *result=json ? cbm_mcp_text_result(json,false):mcp_stage10_error("LEARNING_UNAVAILABLE");
    free(json);free(project);return result;
}

char *handle_memory_learning_control(cbm_mcp_server_t *srv, const char *args) {
    char *project=cbm_mcp_get_string_arg(args,"project");
    char *action=cbm_mcp_get_string_arg(args,"action");
    char *id=cbm_mcp_get_string_arg(args,"item_id");
    char *key=cbm_mcp_get_string_arg(args,"idempotency_key");
    yyjson_doc *doc=yyjson_read(args,strlen(args),0);
    yyjson_val *generation=doc ? yyjson_obj_get(yyjson_doc_get_root(doc),"expected_generation"):NULL;
    cbm_store_t *store=project ? memory_stage14_store(srv,project,true):NULL;
    char uuid[CBM_PROJECT_UUID_SIZE]={0};
    const char *identity=store ? memory_stage14_project_identity(store,project,uuid):NULL;
    int rc=identity && action && key && yyjson_is_int(generation) && yyjson_get_sint(generation)>=0
        ? cbm_learning_control(store,identity,action,id,key,(int)yyjson_get_sint(generation),0):CBM_STORE_REJECTED;
    char *json=(rc==CBM_STORE_OK || rc==CBM_STORE_REPLAYED) ? cbm_learning_status(store,identity):NULL;
    char *result=json ? cbm_mcp_text_result(json,false):mcp_stage10_error("LEARNING_CONTROL_CONFLICT");
    free(json);free(project);free(action);free(id);free(key);yyjson_doc_free(doc);return result;
}

char *handle_memory_concept_inspect(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *store_name = cbm_mcp_get_string_arg(args, "store");
    char *candidate_id = cbm_mcp_get_string_arg(args, "candidate_id");
    if (!project || !store_name || !candidate_id || strcmp(store_name, "project-memory") != 0) {
        free(project);
        free(store_name);
        free(candidate_id);
        return mcp_stage10_error("INVALID_INSPECT_CONTRACT");
    }
    cbm_store_t *store = memory_stage14_store(srv, project, false);
    cbm_concept_result_t inspected = {0};
    char inspect_project_uuid[CBM_PROJECT_UUID_SIZE] = {0};
    const char *inspect_project =
        memory_stage14_project_identity(store, project, inspect_project_uuid);
    int rc = store ? cbm_store_memory_concept_inspect(store, inspect_project, store_name,
                                                      candidate_id, &inspected)
                   : CBM_STORE_NOT_FOUND;
    char *result =
        rc == CBM_STORE_OK
            ? cbm_mcp_text_result(inspected.report_json ? inspected.report_json : "{}", false)
            : mcp_stage10_error(inspected.failure_code ? inspected.failure_code
                                                       : "CANDIDATE_NOT_FOUND");
    cbm_store_memory_concept_result_free(&inspected);
    free(project);
    free(store_name);
    free(candidate_id);
    return result;
}

char *handle_memory_observe_injection(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *event_id = cbm_mcp_get_string_arg(args, "event_id");
    char *session_id = cbm_mcp_get_string_arg(args, "session_id");
    char *candidate_id = cbm_mcp_get_string_arg(args, "candidate_id");
    char *target = cbm_mcp_get_string_arg(args, "target");
    char *content_hash = cbm_mcp_get_string_arg(args, "content_hash");
    char *classifier_status = cbm_mcp_get_string_arg(args, "classifier_status");
    char *classification = cbm_mcp_get_string_arg(args, "classification");
    int injection_index = cbm_mcp_get_int_arg(args, "injection_index", -1);
    int token_count = cbm_mcp_get_int_arg(args, "token_count", -1);
    if (!project || !event_id || !session_id || !candidate_id || !target || !content_hash ||
        !classifier_status || !classification || injection_index < 0 || token_count < 0) {
        free(project);
        free(event_id);
        free(session_id);
        free(candidate_id);
        free(target);
        free(content_hash);
        free(classifier_status);
        free(classification);
        return cbm_mcp_text_result("missing required observe-only injection field", true);
    }
    if (!cbm_memory_security_injection_allowed(classifier_status, classification)) {
        cbm_memory_security_result_t security = {0};
        security.allowed = false;
        security.action = "reject";
        security.content_length = 0;
        if (!memory_security_response_hash(content_hash, security.content_sha256)) {
            free(project);
            free(event_id);
            free(session_id);
            free(candidate_id);
            free(target);
            free(content_hash);
            free(classifier_status);
            free(classification);
            return cbm_mcp_text_result("SECURITY_POLICY_MISMATCH", true);
        }
        if (strcmp(classification, "secret") == 0) {
            security.code = "SECURITY_SECRET_REJECTED";
            security.category = "credential_secret";
            security.reason_code = "credential_secret";
        } else if (strcmp(classification, "pii") == 0) {
            security.code = "SECURITY_PII_CONFIRMATION_REQUIRED";
            security.category = "direct_pii";
            security.reason_code = "direct_pii";
        } else {
            security.code = "SECURITY_PROMPT_INJECTION_REJECTED";
            security.category = "prompt_control_injection";
            security.reason_code =
                strcmp(classification, "rule_override") == 0 ? "rule_override" : "prompt_injection";
        }
        cbm_log_security_event(security.code, "memory_observe_injection", NULL,
                               security.content_sha256, 0, 0);
        char *result = memory_security_response(&security, true);
        free(project);
        free(event_id);
        free(session_id);
        free(candidate_id);
        free(target);
        free(content_hash);
        free(classifier_status);
        free(classification);
        return result;
    }
    cbm_observe_injection_input_t input = {0};
    input.event_id = event_id;
    input.session_id = session_id;
    input.candidate_id = candidate_id;
    input.injection_index = injection_index;
    input.target = target;
    input.content_hash = content_hash;
    input.token_count = token_count;
    input.classifier_status = classifier_status;
    input.classification = classification;

    cbm_store_t *store = resolve_memory_store(srv, project, false);
    if (store)
        store = resolve_memory_store(srv, project, true);
    int rc = store ? cbm_store_memory_observe_injection(store, &input) : CBM_STORE_NOT_FOUND;
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *global = resolve_global_memory_store(srv, false);
        if (global)
            global = resolve_global_memory_store(srv, true);
        if (global)
            rc = cbm_store_memory_observe_injection(global, &input);
    }
    const char *status =
        rc == CBM_STORE_OK
            ? "injected"
            : (rc == CBM_STORE_REJECTED
                   ? "rejected"
                   : (rc == CBM_STORE_REPLAYED
                          ? "replayed"
                          : (rc == CBM_STORE_IDEMPOTENCY_CONFLICT
                                 ? "conflict"
                                 : (rc == CBM_STORE_NOT_FOUND ? "not_found" : "error"))));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id);
    yyjson_mut_obj_add_str(doc, root, "session_id", session_id);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", candidate_id);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_bool(doc, root, "injected",
                            rc == CBM_STORE_OK || (rc == CBM_STORE_REPLAYED &&
                                                   strcmp(classifier_status, "pass") == 0 &&
                                                   strcmp(classification, "safe") == 0));
    if (rc == CBM_STORE_IDEMPOTENCY_CONFLICT)
        yyjson_mut_obj_add_str(doc, root, "code", "IDEMPOTENCY_CONFLICT");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    bool is_error = rc != CBM_STORE_OK && rc != CBM_STORE_REJECTED && rc != CBM_STORE_REPLAYED;
    char *result = cbm_mcp_text_result(json, is_error);
    free(json);
    free(project);
    free(event_id);
    free(session_id);
    free(candidate_id);
    free(target);
    free(content_hash);
    free(classifier_status);
    free(classification);
    return result;
}

char *handle_memory_security_check(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *store_name = cbm_mcp_get_string_arg(args, "store");
    char *content = cbm_mcp_get_string_arg(args, "content");
    if (!project || !store_name || !content || strcmp(store_name, "project-memory") != 0 ||
        !cbm_mcp_memory_project_authorized(srv, project)) {
        char *result = memory_security_scope_response(project ? project : "");
        free(project);
        free(store_name);
        free(content);
        return result;
    }
    cbm_memory_security_result_t security = {0};
    int rc = cbm_memory_security_scan(content, strlen(content), &security);
    free(project);
    free(store_name);
    free(content);
    if (rc != 0) {
        return cbm_mcp_text_result("SECURITY_POLICY_MISMATCH", true);
    }
    return memory_security_response(&security, false);
}

/* ── Stage 12 task orchestration ────────────────────────────────── */

static bool stage12_args_allowed(yyjson_doc *doc, const char *const *allowed,
                                 size_t allowed_count) {
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!root || !yyjson_is_obj(root))
        return false;
    size_t index, maximum;
    yyjson_val *key, *value;
    yyjson_obj_foreach(root, index, maximum, key, value) {
        (void)value;
        const char *name = yyjson_get_str(key);
        bool found = false;
        for (size_t i = 0; name && i < allowed_count; i++) {
            if (strcmp(name, allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static char *stage12_handler_result(char *report, int rc) {
    char *result = cbm_mcp_text_result(
        report ? report : "{\"status\":\"error\",\"code\":\"ORCHESTRATOR_FAILED\"}",
        rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED);
    free(report);
    return result;
}

static int stage14_evolution_runtime(char mode[32], int *production_gate) {
    char configured[32] = {0};
    snprintf(mode, 32, "shadow");
    if (cbm_safe_getenv("CBM_STAGE14_EVOLUTION_MODE", configured, sizeof(configured), NULL) &&
        configured[0]) {
        if (strcmp(configured, "active") == 0)
            return 0;
        if (strcmp(configured, "shadow") != 0 && strcmp(configured, "dry_run") != 0 &&
            strcmp(configured, "bounded_canary") != 0)
            return 0;
        /*
         * Task completion is a legacy automatic path and has no explicit
         * authorization/manifest arguments. A production canary configuration
         * therefore remains plan-only here; the explicit Stage 14 task-control
         * branch is the sole production write entry point.
         */
        snprintf(mode, 32, "%s",
                 strcmp(configured, "bounded_canary") == 0 ? "dry_run" : configured);
    }
    *production_gate = 0;
    return 1;
}

static bool stage12_file_bytes(const char *path, unsigned char **out, size_t *out_size) {
    *out = NULL;
    *out_size = 0;
    if (!path)
        return false;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length <= 0 || length > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *bytes = malloc((size_t)length + 1);
    bool ok = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length;
    fclose(file);
    if (!ok) {
        free(bytes);
        return false;
    }
    bytes[length] = '\0';
    *out = bytes;
    *out_size = (size_t)length;
    return true;
}

#define STAGE14_CANARY_PATH_CAP 4096
#define STAGE14_CANARY_ID_CAP 512

typedef struct {
    char authorization_id[STAGE14_CANARY_ID_CAP];
    char created_at[40];
    char expires_at[40];
    char release_executable_path[STAGE14_CANARY_PATH_CAP];
    char release_executable_sha256[65];
    char target_data_root[STAGE14_CANARY_PATH_CAP];
    char contract_path[STAGE14_CANARY_PATH_CAP];
    char contract_sha256[65];
    char task_project_uuid[STAGE14_CANARY_ID_CAP];
    char task_run_id[STAGE14_CANARY_ID_CAP];
    char task_id[STAGE14_CANARY_ID_CAP];
    char task_idempotency_key[STAGE14_CANARY_ID_CAP];
    char task_evolution_manifest_path[STAGE14_CANARY_PATH_CAP];
    char task_evolution_manifest_sha256[65];
    char task_request_sha256[65];
    int task_memory_item_count;
    int task_max_evolution_events;
    int task_max_cross_project_edges;
    char maintenance_project_uuid[STAGE14_CANARY_ID_CAP];
    char maintenance_mode[32];
    char maintenance_run_id[STAGE14_CANARY_ID_CAP];
    char maintenance_idempotency_key[STAGE14_CANARY_ID_CAP];
    char maintenance_owner_id[STAGE14_CANARY_ID_CAP];
    int64_t maintenance_frozen_as_of_ms;
    char edge_manifest_path[STAGE14_CANARY_PATH_CAP];
    char edge_manifest_sha256[65];
    char concept_manifest_path[STAGE14_CANARY_PATH_CAP];
    char concept_manifest_sha256[65];
    char authorization_manifest_path[STAGE14_CANARY_PATH_CAP];
    char authorization_manifest_sha256[65];
    int limit;
    int budget_seconds;
} stage14_canary_authorization_t;

static bool stage14_lower_sha256(const char *value) {
    if (!value || strlen(value) != 64)
        return false;
    for (size_t i = 0; i < 64; i++) {
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    }
    return true;
}

static bool stage14_absolute_path(const char *path) {
    if (!path || !path[0])
        return false;
#ifdef _WIN32
    return ((isalpha((unsigned char)path[0]) && path[1] == ':' &&
             (path[2] == '\\' || path[2] == '/')) ||
            ((path[0] == '\\' || path[0] == '/') && (path[1] == '\\' || path[1] == '/')));
#else
    return path[0] == '/';
#endif
}

static bool stage14_paths_equal(const char *left, const char *right) {
    if (!left || !right)
        return false;
#ifdef _WIN32
    char normalized_left[STAGE14_CANARY_PATH_CAP];
    char normalized_right[STAGE14_CANARY_PATH_CAP];
    if (strlen(left) >= sizeof(normalized_left) || strlen(right) >= sizeof(normalized_right))
        return false;
    snprintf(normalized_left, sizeof(normalized_left), "%s", left);
    snprintf(normalized_right, sizeof(normalized_right), "%s", right);
    for (size_t i = 0; normalized_left[i]; i++)
        if (normalized_left[i] == '/')
            normalized_left[i] = '\\';
    for (size_t i = 0; normalized_right[i]; i++)
        if (normalized_right[i] == '/')
            normalized_right[i] = '\\';
    return _stricmp(normalized_left, normalized_right) == 0;
#else
    return strcmp(left, right) == 0;
#endif
}

static bool stage14_object_exact_keys(yyjson_val *object, const char *const *allowed,
                                      size_t allowed_count) {
    if (!object || !yyjson_is_obj(object) || yyjson_obj_size(object) != allowed_count)
        return false;
    size_t index, maximum;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, index, maximum, key, value) {
        (void)value;
        const char *name = yyjson_get_str(key);
        bool found = false;
        for (size_t i = 0; name && i < allowed_count; i++) {
            if (strcmp(name, allowed[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static bool stage14_copy_required_string(yyjson_val *object, const char *key, char *out,
                                         size_t out_cap) {
    yyjson_val *value = object ? yyjson_obj_get(object, key) : NULL;
    const char *text = value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
    size_t length = text ? strlen(text) : 0;
    if (!text || length == 0 || length >= out_cap)
        return false;
    memcpy(out, text, length + 1);
    return true;
}

static bool stage14_current_executable(char out[STAGE14_CANARY_PATH_CAP]) {
#ifdef _WIN32
    DWORD length = GetModuleFileNameA(NULL, out, STAGE14_CANARY_PATH_CAP);
    return length > 0 && length < STAGE14_CANARY_PATH_CAP && stage14_absolute_path(out);
#else
    ssize_t length = readlink("/proc/self/exe", out, STAGE14_CANARY_PATH_CAP - 1);
    if (length <= 0 || length >= STAGE14_CANARY_PATH_CAP - 1)
        return false;
    out[length] = '\0';
    return stage14_absolute_path(out);
#endif
}

static bool stage14_file_sha256(const char *path, char out[65]) {
    if (!stage14_absolute_path(path))
        return false;
#ifdef _WIN32
    bool ok = false;
    HANDLE file =
        CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    PUCHAR buffer = NULL;
    UCHAR digest[32] = {0};
    DWORD object_size = 0, result_size = 0;
    if (file == INVALID_HANDLE_VALUE ||
        BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                          sizeof(object_size), &result_size, 0) != 0 ||
        object_size == 0) {
        goto done;
    }
    object = HeapAlloc(GetProcessHeap(), 0, object_size);
    buffer = HeapAlloc(GetProcessHeap(), 0, 1024 * 1024);
    if (!object || !buffer ||
        BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0) != 0)
        goto done;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, 1024 * 1024, &read, NULL))
            goto done;
        if (read == 0)
            break;
        if (BCryptHashData(hash, buffer, read, 0) != 0)
            goto done;
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) != 0)
        goto done;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
    ok = true;
done:
    if (hash)
        BCryptDestroyHash(hash);
    if (algorithm)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (buffer)
        HeapFree(GetProcessHeap(), 0, buffer);
    if (object)
        HeapFree(GetProcessHeap(), 0, object);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    memset(digest, 0, sizeof(digest));
    return ok;
#else
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long length = ftell(file);
    if (length <= 0 || (uint64_t)length > UINT64_C(1073741824) || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    unsigned char *bytes = malloc((size_t)length);
    bool ok = bytes && fread(bytes, 1, (size_t)length, file) == (size_t)length &&
              cbm_stage7_sha256_hex(bytes, (size_t)length, out) == CBM_STORE_OK;
    free(bytes);
    fclose(file);
    return ok;
#endif
}

static bool stage14_small_file_sha256(const char *path, const char *expected) {
    unsigned char *bytes = NULL;
    size_t size = 0;
    char actual[65] = {0};
    bool ok = stage14_absolute_path(path) && stage14_lower_sha256(expected) &&
              stage12_file_bytes(path, &bytes, &size) &&
              cbm_stage7_sha256_hex(bytes, size, actual) == CBM_STORE_OK &&
              strcmp(actual, expected) == 0;
    free(bytes);
    return ok;
}

static bool stage14_iso_utc_millis_valid(const char *value) {
    if (!value || strlen(value) != 24 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != '.' || value[23] != 'Z')
        return false;
    const int digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 22};
    for (size_t i = 0; i < sizeof(digit_positions) / sizeof(digit_positions[0]); i++) {
        if (!isdigit((unsigned char)value[digit_positions[i]]))
            return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int millis = 0, consumed = 0;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ%n", &year, &month, &day, &hour, &minute,
               &second, &millis, &consumed) != 7 ||
        consumed != 24 || year < 2020 || year > 9999 || month < 1 || month > 12 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 || millis < 0 ||
        millis > 999)
        return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum_day = days[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap)
        maximum_day = 29;
    return day >= 1 && day <= maximum_day;
}

static bool stage14_expiration_is_future(const char *value) {
    if (!stage14_iso_utc_millis_valid(value))
        return false;
    char now_value[40] = {0};
#ifdef _WIN32
    SYSTEMTIME now;
    GetSystemTime(&now);
    snprintf(now_value, sizeof(now_value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
             (unsigned)now.wYear, (unsigned)now.wMonth, (unsigned)now.wDay, (unsigned)now.wHour,
             (unsigned)now.wMinute, (unsigned)now.wSecond, (unsigned)now.wMilliseconds);
#else
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC)
        return false;
    struct tm utc = {0};
    if (!gmtime_r(&now.tv_sec, &utc))
        return false;
    snprintf(now_value, sizeof(now_value), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
             utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
             now.tv_nsec / 1000000L);
#endif
    return strcmp(value, now_value) > 0;
}

static bool stage14_directory_exists(const char *path) {
    struct stat info;
    return stage14_absolute_path(path) && stat(path, &info) == 0 &&
#ifdef _WIN32
           (info.st_mode & _S_IFDIR) != 0;
#else
           S_ISDIR(info.st_mode);
#endif
}

static bool stage14_manifest_schema_and_run(const char *path, const char *expected_sha256,
                                            const char *expected_schema,
                                            const char *expected_run_id) {
    unsigned char *bytes = NULL;
    size_t size = 0;
    if (!stage14_small_file_sha256(path, expected_sha256) ||
        !stage12_file_bytes(path, &bytes, &size))
        return false;
    yyjson_doc *doc = yyjson_read((const char *)bytes, size, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *schema = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "schema") : NULL;
    yyjson_val *run_id = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "run_id") : NULL;
    bool ok =
        schema && yyjson_is_str(schema) && strcmp(yyjson_get_str(schema), expected_schema) == 0 &&
        (!expected_run_id ||
         (run_id && yyjson_is_str(run_id) && strcmp(yyjson_get_str(run_id), expected_run_id) == 0));
    yyjson_doc_free(doc);
    free(bytes);
    return ok;
}

static bool stage14_string_array_valid(yyjson_val *array, size_t expected_count) {
    if (!array || !yyjson_is_arr(array) || yyjson_arr_size(array) != expected_count)
        return false;
    size_t index, maximum;
    yyjson_val *value;
    yyjson_arr_foreach(array, index, maximum, value) {
        const char *text = value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
        if (!text || !text[0])
            return false;
        size_t other_index, other_maximum;
        yyjson_val *other;
        yyjson_arr_foreach(array, other_index, other_maximum, other) {
            if (other_index >= index)
                break;
            if (yyjson_is_str(other) && strcmp(text, yyjson_get_str(other)) == 0)
                return false;
        }
    }
    return true;
}

static bool stage14_task_manifest_matches(const stage14_canary_authorization_t *authorization) {
    static const char *const exact_fields[] = {"schema",
                                               "mode",
                                               "task_id",
                                               "idempotency_key",
                                               "request_sha256",
                                               "memory_item_ids",
                                               "feedback_event_ids",
                                               "max_evolution_events",
                                               "max_cross_project_edges"};
    if (!authorization || !stage14_small_file_sha256(authorization->task_evolution_manifest_path,
                                                     authorization->task_evolution_manifest_sha256))
        return false;
    unsigned char *bytes = NULL;
    size_t size = 0;
    if (!stage12_file_bytes(authorization->task_evolution_manifest_path, &bytes, &size))
        return false;
    yyjson_doc *doc = yyjson_read((const char *)bytes, size, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *schema = root ? yyjson_obj_get(root, "schema") : NULL;
    yyjson_val *mode = root ? yyjson_obj_get(root, "mode") : NULL;
    yyjson_val *task = root ? yyjson_obj_get(root, "task_id") : NULL;
    yyjson_val *key = root ? yyjson_obj_get(root, "idempotency_key") : NULL;
    yyjson_val *request = root ? yyjson_obj_get(root, "request_sha256") : NULL;
    yyjson_val *memories = root ? yyjson_obj_get(root, "memory_item_ids") : NULL;
    yyjson_val *feedback = root ? yyjson_obj_get(root, "feedback_event_ids") : NULL;
    yyjson_val *max_events = root ? yyjson_obj_get(root, "max_evolution_events") : NULL;
    yyjson_val *max_edges = root ? yyjson_obj_get(root, "max_cross_project_edges") : NULL;
    bool ok = stage14_object_exact_keys(root, exact_fields,
                                        sizeof(exact_fields) / sizeof(exact_fields[0])) &&
              schema && yyjson_is_str(schema) &&
              strcmp(yyjson_get_str(schema), "stage14-task-evolution-canary-manifest/v1") == 0 &&
              mode && yyjson_is_str(mode) && strcmp(yyjson_get_str(mode), "bounded_canary") == 0 &&
              task && yyjson_is_str(task) &&
              strcmp(yyjson_get_str(task), authorization->task_id) == 0 && key &&
              yyjson_is_str(key) &&
              strcmp(yyjson_get_str(key), authorization->task_idempotency_key) == 0 && request &&
              yyjson_is_str(request) &&
              strcmp(yyjson_get_str(request), authorization->task_request_sha256) == 0 &&
              max_events && yyjson_is_int(max_events) &&
              yyjson_get_sint(max_events) == authorization->task_max_evolution_events &&
              max_edges && yyjson_is_int(max_edges) &&
              yyjson_get_sint(max_edges) == authorization->task_max_cross_project_edges &&
              stage14_string_array_valid(memories, (size_t)authorization->task_memory_item_count) &&
              stage14_string_array_valid(feedback, (size_t)authorization->task_memory_item_count);
    yyjson_doc_free(doc);
    free(bytes);
    return ok;
}

static bool stage14_load_canary_authorization(const char *request_path, const char *request_sha256,
                                              bool request_binding_required,
                                              stage14_canary_authorization_t *out,
                                              const char **out_code) {
    static const char *const exact_fields[] = {"schema",
                                               "authorization_id",
                                               "created_at",
                                               "expires_at",
                                               "release_executable_path",
                                               "release_executable_sha256",
                                               "target_data_root",
                                               "contract_path",
                                               "contract_sha256",
                                               "task_evolution",
                                               "maintenance",
                                               "call_policy",
                                               "secrets_recorded"};
    static const char *const task_fields[] = {"project_uuid",
                                              "run_id",
                                              "task_id",
                                              "idempotency_key",
                                              "manifest_path",
                                              "manifest_sha256",
                                              "request_sha256",
                                              "memory_item_count",
                                              "max_evolution_events",
                                              "max_cross_project_edges",
                                              "mode",
                                              "apply_maximum",
                                              "exact_replay_allowed"};
    static const char *const maintenance_fields[] = {"project_uuid",
                                                     "mode",
                                                     "run_id",
                                                     "idempotency_key",
                                                     "owner_id",
                                                     "frozen_as_of_ms",
                                                     "limit",
                                                     "budget_seconds",
                                                     "edge_manifest_path",
                                                     "edge_manifest_sha256",
                                                     "concept_manifest_path",
                                                     "concept_manifest_sha256",
                                                     "apply_maximum",
                                                     "exact_replay_allowed",
                                                     "hard_delete_allowed"};
    static const char *const policy_fields[] = {"task_evolution_apply_maximum",
                                                "maintenance_apply_maximum", "exact_replay_allowed",
                                                "drift_fail_closed"};
    static const char *const frozen_contract_sha256 =
        "2571515732703420e1756f4af8fec41c519a0daa0dc656d5a3c96f0bd42f97dd";
    char gate[16] = {0}, mode[32] = {0}, env_path[STAGE14_CANARY_PATH_CAP] = {0};
    char env_sha256[80] = {0};
    char auto_maintain[16] = {0}, embed_backend[32] = {0}, no_global_union[16] = {0};
    if (out)
        memset(out, 0, sizeof(*out));
    if (out_code)
        *out_code = "CANARY_AUTH_INVALID";
    if (!out || !cbm_safe_getenv("CBM_STAGE14_PRODUCTION_GATE", gate, sizeof(gate), NULL) ||
        strcmp(gate, "1") != 0 ||
        !cbm_safe_getenv("CBM_STAGE14_EVOLUTION_MODE", mode, sizeof(mode), NULL) ||
        strcmp(mode, "bounded_canary") != 0) {
        if (out_code)
            *out_code = "PRODUCTION_GATE_NOT_LOADED";
        return false;
    }
    const char *no_global_union_ptr = cbm_safe_getenv("CBM_MEMORY_NO_GLOBAL_UNION",
                                                       no_global_union,
                                                       sizeof(no_global_union), NULL);
    bool no_global_union_present = (no_global_union_ptr != NULL);
    if (!cbm_safe_getenv("CBM_MEMORY_AUTO_MAINTAIN", auto_maintain, sizeof(auto_maintain), NULL) ||
        strcmp(auto_maintain, "0") != 0 ||
        !cbm_safe_getenv("CBM_MEMORY_EMBED_BACKEND", embed_backend, sizeof(embed_backend), NULL) ||
        strcmp(embed_backend, "static") != 0 || no_global_union_present) {
        if (out_code)
            *out_code = "CANARY_RUNTIME_ENV_DRIFT";
        return false;
    }
    if (!cbm_safe_getenv("CBM_STAGE14_CANARY_AUTH_MANIFEST", env_path, sizeof(env_path), NULL) ||
        !cbm_safe_getenv("CBM_STAGE14_CANARY_AUTH_SHA256", env_sha256, sizeof(env_sha256), NULL) ||
        !stage14_absolute_path(env_path) || !stage14_lower_sha256(env_sha256)) {
        if (out_code)
            *out_code = "CANARY_AUTH_ENV_DRIFT";
        return false;
    }
    if (request_binding_required && (!stage14_paths_equal(request_path, env_path) ||
                                     !request_sha256 || strcmp(request_sha256, env_sha256) != 0)) {
        if (out_code)
            *out_code = "CANARY_AUTH_REQUEST_DRIFT";
        return false;
    }

    unsigned char *bytes = NULL;
    size_t size = 0;
    char actual_sha256[65] = {0};
    if (!stage12_file_bytes(env_path, &bytes, &size) ||
        cbm_stage7_sha256_hex(bytes, size, actual_sha256) != CBM_STORE_OK ||
        strcmp(actual_sha256, env_sha256) != 0) {
        free(bytes);
        if (out_code)
            *out_code = "CANARY_AUTH_HASH_MISMATCH";
        return false;
    }
    yyjson_doc *doc = yyjson_read((const char *)bytes, size, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *schema = root ? yyjson_obj_get(root, "schema") : NULL;
    yyjson_val *task = root ? yyjson_obj_get(root, "task_evolution") : NULL;
    yyjson_val *maintenance = root ? yyjson_obj_get(root, "maintenance") : NULL;
    yyjson_val *policy = root ? yyjson_obj_get(root, "call_policy") : NULL;
    yyjson_val *secrets = root ? yyjson_obj_get(root, "secrets_recorded") : NULL;
    yyjson_val *task_count = task ? yyjson_obj_get(task, "memory_item_count") : NULL;
    yyjson_val *task_events = task ? yyjson_obj_get(task, "max_evolution_events") : NULL;
    yyjson_val *task_edges = task ? yyjson_obj_get(task, "max_cross_project_edges") : NULL;
    yyjson_val *task_mode = task ? yyjson_obj_get(task, "mode") : NULL;
    yyjson_val *task_apply = task ? yyjson_obj_get(task, "apply_maximum") : NULL;
    yyjson_val *task_replay = task ? yyjson_obj_get(task, "exact_replay_allowed") : NULL;
    yyjson_val *maintenance_frozen =
        maintenance ? yyjson_obj_get(maintenance, "frozen_as_of_ms") : NULL;
    yyjson_val *maintenance_limit = maintenance ? yyjson_obj_get(maintenance, "limit") : NULL;
    yyjson_val *maintenance_budget =
        maintenance ? yyjson_obj_get(maintenance, "budget_seconds") : NULL;
    yyjson_val *maintenance_apply =
        maintenance ? yyjson_obj_get(maintenance, "apply_maximum") : NULL;
    yyjson_val *maintenance_replay =
        maintenance ? yyjson_obj_get(maintenance, "exact_replay_allowed") : NULL;
    yyjson_val *hard_delete =
        maintenance ? yyjson_obj_get(maintenance, "hard_delete_allowed") : NULL;
    yyjson_val *policy_task =
        policy ? yyjson_obj_get(policy, "task_evolution_apply_maximum") : NULL;
    yyjson_val *policy_maintenance =
        policy ? yyjson_obj_get(policy, "maintenance_apply_maximum") : NULL;
    yyjson_val *policy_replay = policy ? yyjson_obj_get(policy, "exact_replay_allowed") : NULL;
    yyjson_val *policy_drift = policy ? yyjson_obj_get(policy, "drift_fail_closed") : NULL;
    int64_t task_count_raw =
        task_count && yyjson_is_int(task_count) ? yyjson_get_sint(task_count) : -1;
    int64_t task_events_raw =
        task_events && yyjson_is_int(task_events) ? yyjson_get_sint(task_events) : -1;
    int64_t task_edges_raw =
        task_edges && yyjson_is_int(task_edges) ? yyjson_get_sint(task_edges) : -1;
    int64_t maintenance_frozen_raw = maintenance_frozen && yyjson_is_int(maintenance_frozen)
                                         ? yyjson_get_sint(maintenance_frozen)
                                         : -1;
    int64_t maintenance_limit_raw = maintenance_limit && yyjson_is_int(maintenance_limit)
                                        ? yyjson_get_sint(maintenance_limit)
                                        : -1;
    int64_t maintenance_budget_raw = maintenance_budget && yyjson_is_int(maintenance_budget)
                                         ? yyjson_get_sint(maintenance_budget)
                                         : -1;
    bool shape_ok =
        stage14_object_exact_keys(root, exact_fields,
                                  sizeof(exact_fields) / sizeof(exact_fields[0])) &&
        stage14_object_exact_keys(task, task_fields,
                                  sizeof(task_fields) / sizeof(task_fields[0])) &&
        stage14_object_exact_keys(maintenance, maintenance_fields,
                                  sizeof(maintenance_fields) / sizeof(maintenance_fields[0])) &&
        stage14_object_exact_keys(policy, policy_fields,
                                  sizeof(policy_fields) / sizeof(policy_fields[0])) &&
        schema && yyjson_is_str(schema) &&
        strcmp(yyjson_get_str(schema), "stage14-production-canary-authorization/v1") == 0 &&
        stage14_copy_required_string(root, "authorization_id", out->authorization_id,
                                     sizeof(out->authorization_id)) &&
        stage14_copy_required_string(root, "created_at", out->created_at,
                                     sizeof(out->created_at)) &&
        stage14_copy_required_string(root, "expires_at", out->expires_at,
                                     sizeof(out->expires_at)) &&
        stage14_copy_required_string(root, "release_executable_path", out->release_executable_path,
                                     sizeof(out->release_executable_path)) &&
        stage14_copy_required_string(root, "release_executable_sha256",
                                     out->release_executable_sha256,
                                     sizeof(out->release_executable_sha256)) &&
        stage14_copy_required_string(root, "target_data_root", out->target_data_root,
                                     sizeof(out->target_data_root)) &&
        stage14_copy_required_string(root, "contract_path", out->contract_path,
                                     sizeof(out->contract_path)) &&
        stage14_copy_required_string(root, "contract_sha256", out->contract_sha256,
                                     sizeof(out->contract_sha256)) &&
        stage14_copy_required_string(task, "project_uuid", out->task_project_uuid,
                                     sizeof(out->task_project_uuid)) &&
        stage14_copy_required_string(task, "run_id", out->task_run_id, sizeof(out->task_run_id)) &&
        stage14_copy_required_string(task, "task_id", out->task_id, sizeof(out->task_id)) &&
        stage14_copy_required_string(task, "idempotency_key", out->task_idempotency_key,
                                     sizeof(out->task_idempotency_key)) &&
        stage14_copy_required_string(task, "manifest_path", out->task_evolution_manifest_path,
                                     sizeof(out->task_evolution_manifest_path)) &&
        stage14_copy_required_string(task, "manifest_sha256", out->task_evolution_manifest_sha256,
                                     sizeof(out->task_evolution_manifest_sha256)) &&
        stage14_copy_required_string(task, "request_sha256", out->task_request_sha256,
                                     sizeof(out->task_request_sha256)) &&
        stage14_copy_required_string(maintenance, "project_uuid", out->maintenance_project_uuid,
                                     sizeof(out->maintenance_project_uuid)) &&
        stage14_copy_required_string(maintenance, "mode", out->maintenance_mode,
                                     sizeof(out->maintenance_mode)) &&
        stage14_copy_required_string(maintenance, "run_id", out->maintenance_run_id,
                                     sizeof(out->maintenance_run_id)) &&
        stage14_copy_required_string(maintenance, "idempotency_key",
                                     out->maintenance_idempotency_key,
                                     sizeof(out->maintenance_idempotency_key)) &&
        stage14_copy_required_string(maintenance, "owner_id", out->maintenance_owner_id,
                                     sizeof(out->maintenance_owner_id)) &&
        stage14_copy_required_string(maintenance, "edge_manifest_path", out->edge_manifest_path,
                                     sizeof(out->edge_manifest_path)) &&
        stage14_copy_required_string(maintenance, "edge_manifest_sha256", out->edge_manifest_sha256,
                                     sizeof(out->edge_manifest_sha256)) &&
        stage14_copy_required_string(maintenance, "concept_manifest_path",
                                     out->concept_manifest_path,
                                     sizeof(out->concept_manifest_path)) &&
        stage14_copy_required_string(maintenance, "concept_manifest_sha256",
                                     out->concept_manifest_sha256,
                                     sizeof(out->concept_manifest_sha256)) &&
        task_count && yyjson_is_int(task_count) && task_events && yyjson_is_int(task_events) &&
        task_edges && yyjson_is_int(task_edges) && task_mode && yyjson_is_str(task_mode) &&
        task_apply && yyjson_is_int(task_apply) && task_replay && yyjson_is_bool(task_replay) &&
        maintenance_frozen && yyjson_is_int(maintenance_frozen) && maintenance_limit &&
        yyjson_is_int(maintenance_limit) && maintenance_budget &&
        yyjson_is_int(maintenance_budget) && maintenance_apply &&
        yyjson_is_int(maintenance_apply) && maintenance_replay &&
        yyjson_is_bool(maintenance_replay) && hard_delete && yyjson_is_bool(hard_delete) &&
        policy_task && yyjson_is_int(policy_task) && policy_maintenance &&
        yyjson_is_int(policy_maintenance) && policy_replay && yyjson_is_bool(policy_replay) &&
        policy_drift && yyjson_is_bool(policy_drift) && secrets && yyjson_is_bool(secrets);
    if (shape_ok) {
        shape_ok = !strcmp(yyjson_get_str(task_mode), "bounded_canary") &&
                   yyjson_get_sint(task_apply) == 1 && yyjson_get_bool(task_replay) &&
                   !strcmp(out->maintenance_mode, "bounded_canary") &&
                   yyjson_get_sint(maintenance_apply) == 1 && yyjson_get_bool(maintenance_replay) &&
                   !yyjson_get_bool(hard_delete) && yyjson_get_sint(policy_task) == 1 &&
                   yyjson_get_sint(policy_maintenance) == 1 && yyjson_get_bool(policy_replay) &&
                   yyjson_get_bool(policy_drift) && !yyjson_get_bool(secrets) &&
                   task_count_raw >= 0 && task_count_raw <= 16 &&
                   task_events_raw >= task_count_raw + 1 && task_events_raw <= 17 &&
                   task_edges_raw >= 0 && task_edges_raw <= 16 &&
                   task_edges_raw <= task_count_raw && maintenance_frozen_raw > 0 &&
                   maintenance_limit_raw >= 1 && maintenance_limit_raw <= 1000 &&
                   maintenance_budget_raw >= 1 && maintenance_budget_raw <= 30;
    }
    if (shape_ok) {
        out->task_memory_item_count = (int)task_count_raw;
        out->task_max_evolution_events = (int)task_events_raw;
        out->task_max_cross_project_edges = (int)task_edges_raw;
        out->maintenance_frozen_as_of_ms = maintenance_frozen_raw;
        out->limit = (int)maintenance_limit_raw;
        out->budget_seconds = (int)maintenance_budget_raw;
    }
    yyjson_doc_free(doc);
    free(bytes);
    if (!shape_ok || !stage14_absolute_path(out->release_executable_path) ||
        !stage14_absolute_path(out->target_data_root) ||
        !stage14_absolute_path(out->contract_path) ||
        !stage14_absolute_path(out->task_evolution_manifest_path) ||
        !stage14_absolute_path(out->edge_manifest_path) ||
        !stage14_absolute_path(out->concept_manifest_path) ||
        !stage14_lower_sha256(out->release_executable_sha256) ||
        !stage14_lower_sha256(out->contract_sha256) ||
        !stage14_lower_sha256(out->task_evolution_manifest_sha256) ||
        !stage14_lower_sha256(out->task_request_sha256) ||
        !stage14_lower_sha256(out->edge_manifest_sha256) ||
        !stage14_lower_sha256(out->concept_manifest_sha256) ||
        !stage14_iso_utc_millis_valid(out->created_at) || out->task_memory_item_count < 0 ||
        out->task_memory_item_count > 16 ||
        out->task_max_evolution_events < out->task_memory_item_count + 1 ||
        out->task_max_evolution_events > 17 || out->task_max_cross_project_edges < 0 ||
        out->task_max_cross_project_edges > 16 ||
        out->task_max_cross_project_edges > out->task_memory_item_count ||
        out->maintenance_frozen_as_of_ms <= 0 || out->limit < 1 || out->limit > 1000 ||
        out->budget_seconds < 1 || out->budget_seconds > 30) {
        if (out_code)
            *out_code = "CANARY_AUTH_SCHEMA_INVALID";
        return false;
    }
    if (!stage14_expiration_is_future(out->expires_at)) {
        if (out_code)
            *out_code = "CANARY_AUTH_EXPIRED";
        return false;
    }
    char data_root[STAGE14_CANARY_PATH_CAP] = {0};
    if (!cbm_safe_getenv("CBM_DATA_ROOT", data_root, sizeof(data_root), NULL) ||
        !stage14_paths_equal(data_root, out->target_data_root) ||
        !stage14_directory_exists(out->target_data_root)) {
        if (out_code)
            *out_code = "CANARY_DATA_ROOT_MISMATCH";
        return false;
    }
    if (strcmp(out->contract_sha256, frozen_contract_sha256) != 0 ||
        !stage14_manifest_schema_and_run(out->contract_path, out->contract_sha256,
                                         "stage14-rev8-production-closure-contract/v1", NULL)) {
        if (out_code)
            *out_code = "CANARY_CONTRACT_MISMATCH";
        return false;
    }
    char executable_path[STAGE14_CANARY_PATH_CAP] = {0};
    char executable_sha256[65] = {0};
    if (!stage14_current_executable(executable_path) ||
        !stage14_paths_equal(executable_path, out->release_executable_path) ||
        !stage14_file_sha256(executable_path, executable_sha256) ||
        strcmp(executable_sha256, out->release_executable_sha256) != 0) {
        if (out_code)
            *out_code = "CANARY_EXECUTABLE_MISMATCH";
        return false;
    }
    char edge_run_id[STAGE14_CANARY_ID_CAP + 16] = {0};
    char concept_run_id[STAGE14_CANARY_ID_CAP + 16] = {0};
    int edge_length =
        snprintf(edge_run_id, sizeof(edge_run_id), "%s:edge", out->maintenance_run_id);
    int concept_length =
        snprintf(concept_run_id, sizeof(concept_run_id), "%s:concept", out->maintenance_run_id);
    if (!stage14_task_manifest_matches(out) || edge_length <= 0 ||
        edge_length >= (int)sizeof(edge_run_id) || concept_length <= 0 ||
        concept_length >= (int)sizeof(concept_run_id) ||
        !stage14_manifest_schema_and_run(out->edge_manifest_path, out->edge_manifest_sha256,
                                         "stage9-production-canary-manifest/v1", edge_run_id) ||
        !stage14_manifest_schema_and_run(out->concept_manifest_path, out->concept_manifest_sha256,
                                         "stage10-production-canary-manifest/v1", concept_run_id)) {
        if (out_code)
            *out_code = "CANARY_REFERENCED_MANIFEST_MISMATCH";
        return false;
    }
    snprintf(out->authorization_manifest_path, sizeof(out->authorization_manifest_path), "%s",
             env_path);
    snprintf(out->authorization_manifest_sha256, sizeof(out->authorization_manifest_sha256), "%s",
             env_sha256);
    if (out_code)
        *out_code = "CANARY_AUTH_READY";
    return true;
}

static bool stage14_authorizes_task(const stage14_canary_authorization_t *authorization,
                                    const char *project_uuid, const char *run_id,
                                    const char *task_id, const char *idempotency_key,
                                    const char *manifest_path, const char *manifest_sha256,
                                    int max_evolution_events, int max_cross_project_edges) {
    return authorization && project_uuid && run_id && task_id && idempotency_key && manifest_path &&
           manifest_sha256 && strcmp(authorization->task_project_uuid, project_uuid) == 0 &&
           strcmp(authorization->task_run_id, run_id) == 0 &&
           strcmp(authorization->task_id, task_id) == 0 &&
           strcmp(authorization->task_idempotency_key, idempotency_key) == 0 &&
           stage14_paths_equal(authorization->task_evolution_manifest_path, manifest_path) &&
           strcmp(authorization->task_evolution_manifest_sha256, manifest_sha256) == 0 &&
           authorization->task_max_evolution_events == max_evolution_events &&
           authorization->task_max_cross_project_edges == max_cross_project_edges;
}

static char *stage14_task_control_error(const char *status, const char *code) {
    char json[384];
    snprintf(json, sizeof(json),
             "{\"schema\":\"stage14-task-evolution-control/v1\","
             "\"status\":\"%s\",\"code\":\"%s\","
             "\"production_state_written\":false}",
             status ? status : "error", code ? code : "STAGE14_TASK_CONTROL_FAILED");
    return cbm_mcp_text_result(json, true);
}

static char *handle_stage14_task_evolution_control(cbm_mcp_server_t *srv, const char *args) {
    static const char *const preview_fields[] = {"action",
                                                 "mode",
                                                 "project",
                                                 "run_id",
                                                 "task_id",
                                                 "idempotency_key",
                                                 "max_evolution_events",
                                                 "max_cross_project_edges"};
    static const char *const apply_fields[] = {"action",
                                               "mode",
                                               "project",
                                               "run_id",
                                               "task_id",
                                               "idempotency_key",
                                               "max_evolution_events",
                                               "max_cross_project_edges",
                                               "manifest_path",
                                               "manifest_sha256",
                                               "authorization_manifest_path",
                                               "authorization_manifest_sha256"};
    (void)srv;
    yyjson_doc *doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *action_value = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "action") : NULL;
    const char *action =
        action_value && yyjson_is_str(action_value) ? yyjson_get_str(action_value) : NULL;
    bool preview = action && strcmp(action, "preview") == 0;
    bool apply = action && strcmp(action, "apply") == 0;
    bool exact_shape =
        (preview &&
         stage14_object_exact_keys(root, preview_fields,
                                   sizeof(preview_fields) / sizeof(preview_fields[0]))) ||
        (apply && stage14_object_exact_keys(root, apply_fields,
                                            sizeof(apply_fields) / sizeof(apply_fields[0])));
    if (!exact_shape) {
        yyjson_doc_free(doc);
        return stage14_task_control_error("rejected", "INVALID_STAGE14_TASK_CONTROL");
    }

    const char *mode = yyjson_get_str(yyjson_obj_get(root, "mode"));
    const char *project = yyjson_get_str(yyjson_obj_get(root, "project"));
    const char *run_id = yyjson_get_str(yyjson_obj_get(root, "run_id"));
    const char *task_id = yyjson_get_str(yyjson_obj_get(root, "task_id"));
    const char *idempotency_key = yyjson_get_str(yyjson_obj_get(root, "idempotency_key"));
    yyjson_val *events_value = yyjson_obj_get(root, "max_evolution_events");
    yyjson_val *edges_value = yyjson_obj_get(root, "max_cross_project_edges");
    int64_t max_events_raw =
        events_value && yyjson_is_int(events_value) ? yyjson_get_sint(events_value) : -1;
    int64_t max_edges_raw =
        edges_value && yyjson_is_int(edges_value) ? yyjson_get_sint(edges_value) : -1;
    if (!mode || strcmp(mode, "bounded_canary") != 0) {
        yyjson_doc_free(doc);
        return stage14_task_control_error("rejected", mode && strcmp(mode, "active") == 0
                                                          ? "ACTIVE_MODE_FORBIDDEN"
                                                          : "BOUNDED_CANARY_MODE_REQUIRED");
    }
    if (!project || !project[0] || !run_id || !run_id[0] || !task_id || !task_id[0] ||
        !idempotency_key || !idempotency_key[0] || max_events_raw < 1 || max_events_raw > 17 ||
        max_edges_raw < 0 || max_edges_raw > 16) {
        yyjson_doc_free(doc);
        return stage14_task_control_error("rejected", "INVALID_STAGE14_TASK_BINDING");
    }
    int max_events = (int)max_events_raw;
    int max_edges = (int)max_edges_raw;

    const char *manifest_path = NULL;
    const char *manifest_sha256 = NULL;
    const char *authorization_path = NULL;
    const char *authorization_sha256 = NULL;
    if (apply) {
        manifest_path = yyjson_get_str(yyjson_obj_get(root, "manifest_path"));
        manifest_sha256 = yyjson_get_str(yyjson_obj_get(root, "manifest_sha256"));
        authorization_path = yyjson_get_str(yyjson_obj_get(root, "authorization_manifest_path"));
        authorization_sha256 =
            yyjson_get_str(yyjson_obj_get(root, "authorization_manifest_sha256"));
        if (!stage14_absolute_path(manifest_path) || !stage14_lower_sha256(manifest_sha256) ||
            !stage14_absolute_path(authorization_path) ||
            !stage14_lower_sha256(authorization_sha256)) {
            yyjson_doc_free(doc);
            return stage14_task_control_error("rejected", "INVALID_STAGE14_MANIFEST_BINDING");
        }
    }

    cbm_global_memory_t *global = cbm_global_memory_open_default();
    if (!global) {
        yyjson_doc_free(doc);
        return stage14_task_control_error("error", "GLOBAL_MEMORY_UNAVAILABLE");
    }
    cbm_evolution_task_input_t input = {
        .mode = "bounded_canary",
        .task_id = task_id,
        .project_uuid = project,
        .run_id = run_id,
        .idempotency_key = idempotency_key,
        .max_evolution_events = max_events,
        .max_cross_project_edges = max_edges,
        .isolated_write_allowed = 0,
        .production_gate_allowed = 0,
    };
    cbm_evolution_result_t plan = {0};
    int plan_rc = cbm_evolution_plan_completed_task(global, &input, &plan);
    if (plan_rc != CBM_STORE_OK) {
        cbm_evolution_result_free(&plan);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        return stage14_task_control_error(plan_rc == CBM_STORE_REJECTED ? "rejected" : "error",
                                          plan_rc == CBM_STORE_REJECTED
                                              ? "TASK_EVOLUTION_PLAN_REJECTED"
                                              : "TASK_EVOLUTION_PLAN_FAILED");
    }
    if (preview) {
        char *result = cbm_mcp_text_result(plan.report_json ? plan.report_json : "{}", false);
        cbm_evolution_result_free(&plan);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        return result;
    }

    stage14_canary_authorization_t authorization = {0};
    const char *authorization_code = "CANARY_AUTH_INVALID";
    bool authorization_ready = stage14_load_canary_authorization(
        authorization_path, authorization_sha256, true, &authorization, &authorization_code);
    if (!authorization_ready) {
        cbm_evolution_result_free(&plan);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        return stage14_task_control_error("rejected", authorization_code);
    }
    if (!stage14_authorizes_task(&authorization, project, run_id, task_id, idempotency_key,
                                 manifest_path, manifest_sha256, max_events, max_edges) ||
        strcmp(plan.request_sha256, authorization.task_request_sha256) != 0 ||
        plan.eligible != authorization.task_memory_item_count) {
        cbm_evolution_result_free(&plan);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        return stage14_task_control_error("rejected", "CANARY_TASK_ALLOWLIST_MISMATCH");
    }
    cbm_evolution_result_free(&plan);

    input.manifest_path = manifest_path;
    input.manifest_sha256 = manifest_sha256;
    input.production_gate_allowed = 1;
    cbm_evolution_result_t applied = {0};
    int apply_rc = cbm_evolution_apply_completed_task(global, &input, &applied);
    char *result = NULL;
    if (apply_rc == CBM_STORE_OK || apply_rc == CBM_STORE_REPLAYED) {
        result = cbm_mcp_text_result(applied.report_json ? applied.report_json : "{}", false);
    } else if (apply_rc == CBM_STORE_IDEMPOTENCY_CONFLICT) {
        result = stage14_task_control_error("conflict", "IDEMPOTENCY_CONFLICT");
    } else if (apply_rc == CBM_STORE_REJECTED) {
        result = stage14_task_control_error("rejected", "TASK_EVOLUTION_MANIFEST_OR_GATE_REJECTED");
    } else {
        result = stage14_task_control_error("error", "TASK_EVOLUTION_APPLY_FAILED");
    }
    cbm_evolution_result_free(&applied);
    cbm_global_memory_close(global);
    yyjson_doc_free(doc);
    return result;
}

static bool stage14_authorizes_maintenance(
    const stage14_canary_authorization_t *authorization, const char *project_uuid, const char *mode,
    const char *run_id, const char *idempotency_key, const char *owner_id, int64_t frozen_as_of_ms,
    const char *edge_manifest_path, const char *edge_manifest_sha256,
    const char *concept_manifest_path, const char *concept_manifest_sha256, int limit,
    int budget_seconds) {
    return authorization && project_uuid && mode && run_id && idempotency_key && owner_id &&
           edge_manifest_path && edge_manifest_sha256 && concept_manifest_path &&
           concept_manifest_sha256 &&
           strcmp(authorization->maintenance_project_uuid, project_uuid) == 0 &&
           strcmp(authorization->maintenance_mode, mode) == 0 &&
           strcmp(authorization->maintenance_run_id, run_id) == 0 &&
           strcmp(authorization->maintenance_idempotency_key, idempotency_key) == 0 &&
           strcmp(authorization->maintenance_owner_id, owner_id) == 0 &&
           authorization->maintenance_frozen_as_of_ms == frozen_as_of_ms &&
           stage14_paths_equal(authorization->edge_manifest_path, edge_manifest_path) &&
           strcmp(authorization->edge_manifest_sha256, edge_manifest_sha256) == 0 &&
           stage14_paths_equal(authorization->concept_manifest_path, concept_manifest_path) &&
           strcmp(authorization->concept_manifest_sha256, concept_manifest_sha256) == 0 &&
           authorization->limit == limit && authorization->budget_seconds == budget_seconds;
}

static bool stage12_manifest_guard(const char *project, const char *path,
                                   const char *expected_hash) {
    if (!project || !path || !expected_hash)
        return false;
    char fixture[16] = {0};
    char production[16] = {0};
    char env_path[4096] = {0};
    char env_hash[80] = {0};
    cbm_safe_getenv("CBM_STAGE12_ACTIVE_FIXTURE", fixture, sizeof(fixture), NULL);
    cbm_safe_getenv("CBM_STAGE12_PRODUCTION_CANARY", production, sizeof(production), NULL);
    cbm_safe_getenv("CBM_STAGE12_PRODUCTION_CANARY_MANIFEST", env_path, sizeof(env_path), NULL);
    cbm_safe_getenv("CBM_STAGE12_PRODUCTION_CANARY_SHA256", env_hash, sizeof(env_hash), NULL);
    bool fixture_ok = strcmp(fixture, "1") == 0 && strncmp(project, "stage12-fixture-", 16) == 0;
    bool production_ok = strcmp(project, "H-Codex_H-neuroplastic-main") == 0 &&
                         strcmp(production, "1") == 0 && env_path[0] && env_hash[0] &&
                         strcmp(path, env_path) == 0 && strcmp(expected_hash, env_hash) == 0;
    if (!fixture_ok && !production_ok)
        return false;
    unsigned char *bytes = NULL;
    size_t size = 0;
    if (!stage12_file_bytes(path, &bytes, &size))
        return false;
    char actual_hash[65];
    bool hash_ok = cbm_stage7_sha256_hex(bytes, size, actual_hash) == CBM_STORE_OK &&
                   strcmp(actual_hash, expected_hash) == 0;
    yyjson_doc *doc = hash_ok ? yyjson_read((const char *)bytes, size, 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *schema = root ? yyjson_obj_get(root, "schema") : NULL;
    yyjson_val *manifest_project = root ? yyjson_obj_get(root, "project") : NULL;
    bool shape_ok = schema && yyjson_is_str(schema) &&
                    strcmp(yyjson_get_str(schema), "stage12-production-canary/v1") == 0 &&
                    manifest_project && yyjson_is_str(manifest_project) &&
                    strcmp(yyjson_get_str(manifest_project), project) == 0;
    yyjson_doc_free(doc);
    free(bytes);
    return hash_ok && shape_ok;
}

char *handle_memory_task_migrate(cbm_mcp_server_t *srv, const char *args) {
    static const char *const allowed[] = {"project", "manifest_path", "manifest_sha256",
                                          "idempotency_key"};
    yyjson_doc *doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    if (!doc || !stage12_args_allowed(doc, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        yyjson_doc_free(doc);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"INVALID_ARGUMENT\","
                                   "\"production_state_written\":false}",
                                   true);
    }
    char *project = memory_arg_string_dup(doc, "project");
    char *manifest_path = memory_arg_string_dup(doc, "manifest_path");
    char *manifest_hash = memory_arg_string_dup(doc, "manifest_sha256");
    char *idempotency_key = memory_arg_string_dup(doc, "idempotency_key");
    bool complete = project && manifest_path && manifest_hash && idempotency_key;
    bool guard = complete && cbm_mcp_memory_project_authorized(srv, project) &&
                 stage12_manifest_guard(project, manifest_path, manifest_hash);
    yyjson_doc_free(doc);
    if (!guard) {
        free(project);
        free(manifest_path);
        free(manifest_hash);
        free(idempotency_key);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"FEATURE_DISABLED\","
                                   "\"production_state_written\":false}",
                                   true);
    }
    cbm_store_t *store = resolve_memory_store(srv, project, true);
    char *report = NULL;
    bool replayed = false;
    int rc = store ? cbm_orchestrator_migrate(store, &replayed, &report) : CBM_STORE_ERR;
    free(project);
    free(manifest_path);
    free(manifest_hash);
    free(idempotency_key);
    return stage12_handler_result(report, rc);
}

char *handle_memory_task_begin(cbm_mcp_server_t *srv, const char *args) {
    static const char *const allowed[] = {
        "project", "session_id",           "turn_id",         "prompt_sha256", "prompt_length",
        "scope",   "retrieval_session_id", "idempotency_key", "workspace"};
    yyjson_doc *doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    if (!doc || !stage12_args_allowed(doc, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        yyjson_doc_free(doc);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"INVALID_ARGUMENT\"}", true);
    }
    yyjson_val *length_value = memory_arg(doc, "prompt_length");
    char *project = memory_arg_string_dup(doc, "project");
    char *workspace = memory_arg_string_dup(doc, "workspace");
    char *scope = memory_arg_string_dup(doc, "scope");
    cbm_task_begin_input_t input = {
        .project = project,
        .session_id = yyjson_get_str(memory_arg(doc, "session_id")),
        .turn_id = yyjson_get_str(memory_arg(doc, "turn_id")),
        .prompt_sha256 = yyjson_get_str(memory_arg(doc, "prompt_sha256")),
        .prompt_length =
            length_value && yyjson_is_int(length_value) ? (int)yyjson_get_int(length_value) : -1,
        .retrieval_session_id = yyjson_get_str(memory_arg(doc, "retrieval_session_id")),
        .idempotency_key = yyjson_get_str(memory_arg(doc, "idempotency_key")),
    };
    bool valid_scope = scope && strcmp(scope, "project") == 0;
    cbm_global_memory_t *global = project && valid_scope ? cbm_global_memory_open_default() : NULL;
    char *report = NULL;
    int rc = CBM_STORE_ERR;
    if (global) {
        cbm_project_resolution_t resolution = {0};
        int resolved = workspace ? (cbm_project_resolve(workspace, project, NULL, &resolution) == 0
                                        ? CBM_STORE_OK
                                        : CBM_STORE_ERR)
                                 : memory_stage14_resolve_project(cbm_global_memory_store(global),
                                                                  project, &resolution);
        if (resolved == CBM_STORE_OK)
            rc = cbm_global_task_begin(global, &resolution, &input, &report);
    }
    yyjson_doc_free(doc);
    free(project);
    free(workspace);
    free(scope);
    if (global)
        cbm_global_memory_close(global);
    if (!global && !report)
        report = cbm_strdup("{\"status\":\"error\",\"code\":\"TASK_SCHEMA_UNAVAILABLE\"}");
    return stage12_handler_result(report, rc);
}

char *handle_memory_task_status(cbm_mcp_server_t *srv, const char *args) {
    static const char *const allowed[] = {"project", "task_id", "session_id", "turn_id",
                                          "legacy_fallback"};
    yyjson_doc *doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    if (!doc || !stage12_args_allowed(doc, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        yyjson_doc_free(doc);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"INVALID_ARGUMENT\"}", true);
    }
    char *project = memory_arg_string_dup(doc, "project");
    const char *task_id = yyjson_get_str(memory_arg(doc, "task_id"));
    const char *session_id = yyjson_get_str(memory_arg(doc, "session_id"));
    const char *turn_id = yyjson_get_str(memory_arg(doc, "turn_id"));
    bool legacy_fallback = yyjson_get_bool(memory_arg(doc, "legacy_fallback"));
    cbm_store_t *global = project ? resolve_global_memory_store(srv, false) : NULL;
    char *report = NULL;
    int rc = global
                 ? cbm_global_store_task_status(global, task_id, session_id, turn_id, NULL, &report)
                 : CBM_STORE_ERR;
    if (rc == CBM_STORE_NOT_FOUND && legacy_fallback) {
        free(report);
        report = NULL;
        cbm_store_t *legacy = resolve_memory_store(srv, project, false);
        rc = legacy
                 ? cbm_orchestrator_status(legacy, project, task_id, session_id, turn_id, &report)
                 : CBM_STORE_NOT_FOUND;
    }
    yyjson_doc_free(doc);
    free(project);
    if (!global && !report)
        report = cbm_strdup("{\"status\":\"error\",\"code\":\"TASK_SCHEMA_UNAVAILABLE\"}");
    return stage12_handler_result(report, rc);
}

char *handle_memory_task_complete(cbm_mcp_server_t *srv, const char *args) {
    static const char *const allowed[] = {"project",         "task_id",      "outcome",
                                          "idempotency_key", "attributions", "legacy_fallback"};
    static const char *const attribution_allowed[] = {"memory_item_id", "state", "evidence_id",
                                                      "feedback_event_id"};
    yyjson_doc *doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    if (!doc || !stage12_args_allowed(doc, allowed, sizeof(allowed) / sizeof(allowed[0]))) {
        yyjson_doc_free(doc);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"INVALID_ARGUMENT\"}", true);
    }
    yyjson_val *array = memory_arg(doc, "attributions");
    size_t count = array && yyjson_is_arr(array) ? yyjson_arr_size(array) : 0;
    if (!array || !yyjson_is_arr(array) || count > 16) {
        yyjson_doc_free(doc);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"INVALID_ARGUMENT\"}", true);
    }
    cbm_task_attribution_input_t attributions[16];
    memset(attributions, 0, sizeof(attributions));
    size_t index, maximum;
    yyjson_val *item;
    bool items_valid = true;
    yyjson_arr_foreach(array, index, maximum, item) {
        if (!yyjson_is_obj(item)) {
            items_valid = false;
            break;
        }
        size_t ki, km;
        yyjson_val *key, *value;
        yyjson_obj_foreach(item, ki, km, key, value) {
            (void)value;
            const char *name = yyjson_get_str(key);
            bool found = false;
            for (size_t a = 0;
                 name && a < sizeof(attribution_allowed) / sizeof(attribution_allowed[0]); a++) {
                if (strcmp(name, attribution_allowed[a]) == 0)
                    found = true;
            }
            if (!found)
                items_valid = false;
        }
        yyjson_val *memory = yyjson_obj_get(item, "memory_item_id");
        yyjson_val *state = yyjson_obj_get(item, "state");
        yyjson_val *evidence = yyjson_obj_get(item, "evidence_id");
        yyjson_val *feedback = yyjson_obj_get(item, "feedback_event_id");
        attributions[index].memory_item_id =
            memory && yyjson_is_str(memory) ? yyjson_get_str(memory) : NULL;
        attributions[index].state = state && yyjson_is_str(state) ? yyjson_get_str(state) : NULL;
        attributions[index].evidence_id =
            evidence && yyjson_is_str(evidence) ? yyjson_get_str(evidence) : NULL;
        attributions[index].feedback_event_id =
            feedback && yyjson_is_str(feedback) ? yyjson_get_str(feedback) : NULL;
        if (!attributions[index].memory_item_id || !attributions[index].state)
            items_valid = false;
    }
    char *project = memory_arg_string_dup(doc, "project");
    bool legacy_fallback = yyjson_get_bool(memory_arg(doc, "legacy_fallback"));
    cbm_task_complete_input_t input = {
        .project = project,
        .task_id = yyjson_get_str(memory_arg(doc, "task_id")),
        .outcome = yyjson_get_str(memory_arg(doc, "outcome")),
        .idempotency_key = yyjson_get_str(memory_arg(doc, "idempotency_key")),
        .attributions = attributions,
        .attribution_count = count,
    };
    cbm_global_memory_t *global = project && items_valid ? cbm_global_memory_open_default() : NULL;
    char *report = NULL;
    int rc = global ? cbm_global_task_complete(global, &input, &report) : CBM_STORE_ERR;
    if (global && (rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED)) {
        char mode[32], evolution_key[1024];
        int production_gate = 0;
        int key_size = snprintf(evolution_key, sizeof(evolution_key), "%s:stage14-evolution",
                                input.idempotency_key ? input.idempotency_key : "");
        cbm_evolution_result_t evolution = {0};
        int evolution_rc = CBM_STORE_ERR;
        if (key_size > 0 && key_size < (int)sizeof(evolution_key) &&
            stage14_evolution_runtime(mode, &production_gate)) {
            cbm_evolution_task_input_t evolution_input = {
                .mode = mode,
                .task_id = input.task_id,
                .idempotency_key = evolution_key,
                .isolated_write_allowed = 0,
                .production_gate_allowed = production_gate,
            };
            evolution_rc = cbm_evolution_apply_completed_task(global, &evolution_input, &evolution);
        }
        cbm_evolution_result_free(&evolution);
        if (evolution_rc != CBM_STORE_OK && evolution_rc != CBM_STORE_REPLAYED) {
            free(report);
            report = cbm_strdup(
                evolution_rc == CBM_STORE_IDEMPOTENCY_CONFLICT
                    ? "{\"status\":\"conflict\",\"code\":\"IDEMPOTENCY_CONFLICT\"}"
                    : (evolution_rc == CBM_STORE_REJECTED
                           ? "{\"status\":\"error\",\"code\":\"STAGE14_EVOLUTION_GUARD\"}"
                           : "{\"status\":\"error\",\"code\":\"STAGE14_EVOLUTION_FAILED\"}"));
            rc = evolution_rc;
        }
    }
    if (rc == CBM_STORE_NOT_FOUND && legacy_fallback) {
        free(report);
        report = NULL;
        cbm_store_t *legacy = resolve_memory_store(srv, project, false);
        if (legacy)
            legacy = resolve_memory_store(srv, project, true);
        rc = legacy ? cbm_orchestrator_complete(legacy, &input, &report) : CBM_STORE_NOT_FOUND;
    }
    yyjson_doc_free(doc);
    free(project);
    if (global)
        cbm_global_memory_close(global);
    if (!global && !report)
        report = cbm_strdup("{\"status\":\"error\",\"code\":\"INVALID_ARGUMENT\"}");
    return stage12_handler_result(report, rc);
}

char *handle_memory_observe_usage(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *event_id = cbm_mcp_get_string_arg(args, "event_id");
    char *session_id = cbm_mcp_get_string_arg(args, "session_id");
    char *candidate_id = cbm_mcp_get_string_arg(args, "candidate_id");
    char *injection_id = cbm_mcp_get_string_arg(args, "injection_id");
    char *outcome = cbm_mcp_get_string_arg(args, "outcome");
    char *evidence_type = cbm_mcp_get_string_arg(args, "evidence_type");
    char *evidence_ref = cbm_mcp_get_string_arg(args, "evidence_ref");
    char *evidence_hash = cbm_mcp_get_string_arg(args, "evidence_hash");
    if (!project || !event_id || !session_id || !candidate_id || !outcome || !evidence_type ||
        !evidence_ref) {
        free(project);
        free(event_id);
        free(session_id);
        free(candidate_id);
        free(injection_id);
        free(outcome);
        free(evidence_type);
        free(evidence_ref);
        free(evidence_hash);
        return cbm_mcp_text_result("missing required observe-only usage field", true);
    }
    cbm_observe_usage_input_t input = {0};
    input.event_id = event_id;
    input.session_id = session_id;
    input.candidate_id = candidate_id;
    input.injection_id = injection_id;
    input.outcome = outcome;
    input.evidence_type = evidence_type;
    input.evidence_ref = evidence_ref;
    input.evidence_hash = evidence_hash;

    cbm_store_t *store = resolve_memory_store(srv, project, false);
    if (store)
        store = resolve_memory_store(srv, project, true);
    int rc = store ? cbm_store_memory_observe_usage(store, &input) : CBM_STORE_NOT_FOUND;
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *global = resolve_global_memory_store(srv, false);
        if (global)
            global = resolve_global_memory_store(srv, true);
        if (global)
            rc = cbm_store_memory_observe_usage(global, &input);
    }
    const char *status = rc == CBM_STORE_OK
                             ? "recorded"
                             : (rc == CBM_STORE_REPLAYED
                                    ? "replayed"
                                    : (rc == CBM_STORE_IDEMPOTENCY_CONFLICT
                                           ? "conflict"
                                           : (rc == CBM_STORE_NOT_FOUND ? "not_found" : "error")));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "event_id", event_id);
    yyjson_mut_obj_add_str(doc, root, "session_id", session_id);
    yyjson_mut_obj_add_str(doc, root, "candidate_id", candidate_id);
    yyjson_mut_obj_add_str(doc, root, "outcome", outcome);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    if (rc == CBM_STORE_IDEMPOTENCY_CONFLICT)
        yyjson_mut_obj_add_str(doc, root, "code", "IDEMPOTENCY_CONFLICT");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    bool is_error = rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED;
    char *result = cbm_mcp_text_result(json, is_error);
    free(json);
    free(project);
    free(event_id);
    free(session_id);
    free(candidate_id);
    free(injection_id);
    free(outcome);
    free(evidence_type);
    free(evidence_ref);
    free(evidence_hash);
    return result;
}

char *handle_memory_delete(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    char *id = cbm_mcp_get_string_arg(args, "id");
    char *mode = cbm_mcp_get_string_arg(args, "mode");
    char *user = cbm_mcp_get_string_arg(args, "user");
    if (!project || !id) {
        free(project);
        free(id);
        free(mode);
        free(user);
        return cbm_mcp_text_result("project and id are required", true);
    }
    const char *m = (mode && mode[0]) ? mode : "soft";
    if (strcmp(m, "soft") != 0 && strcmp(m, "restore") != 0) {
        free(project);
        free(id);
        free(mode);
        free(user);
        return cbm_mcp_text_result("{\"status\":\"error\",\"code\":\"HARD_DELETE_DISABLED\","
                                   "\"production_state_written\":false}",
                                   true);
    }
    cbm_store_t *store = resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error("project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        free(id);
        free(mode);
        free(user);
        return _res;
    }
    int rc;
    const char *ok_status;
    if (strcmp(m, "restore") == 0) {
        rc = cbm_store_memory_restore(store, id, project, user);
        ok_status = "restored";
    } else if (strcmp(m, "soft") == 0) {
        rc = cbm_store_memory_delete(store, id, project, m, user);
        ok_status = "soft_deleted";
    } else {
        rc = cbm_store_memory_delete(store, id, project, "soft", user);
        ok_status = "soft_deleted";
    }
    /* Global-memory fallback: by-id delete/restore is scope-guarded on project,
     * so retry the global store with project=NULL on NOT_FOUND. */
    if (rc == CBM_STORE_NOT_FOUND) {
        cbm_store_t *gstore = resolve_global_memory_store(srv, false);
        if (gstore) {
            if (strcmp(m, "restore") == 0)
                rc = cbm_store_memory_restore(gstore, id, NULL, user);
            else
                rc = cbm_store_memory_delete(gstore, id, NULL, m, user);
        }
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_str(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "mode", m);
    yyjson_mut_obj_add_str(doc, root, "status",
                           rc == CBM_STORE_NOT_FOUND ? "not_found"
                                                     : (rc == CBM_STORE_OK ? ok_status : "error"));
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK && rc != CBM_STORE_NOT_FOUND);
    free(json);
    free(project);
    free(id);
    free(mode);
    free(user);
    return result;
}

char *handle_admin_consolidate(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    /* scope='global' consolidates the cross-project store; pass project=NULL so
     * the store-level filter covers all of its (scope_project=NULL) rows. */
    const char *scope_arg = is_global ? NULL : project;
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, false)
                                   : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    int processed = 0;
    int rc = cbm_store_memory_consolidate(store, scope_arg, cbm_mcp_get_int_arg(args, "limit", 100),
                                          &processed);
    /* Rebuild the FTS index with current CJK segmentation so memories indexed
     * before bigram segmentation existed become searchable in Chinese. Pass
     * skip_reindex_fts=true to skip on large stores where the rebuild is costly. */
    int reindexed = 0;
    if (!cbm_mcp_get_bool_arg(args, "skip_reindex_fts")) {
        (void)cbm_store_memory_reindex_fts(store, scope_arg, &reindexed);
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "processed", processed);
    yyjson_mut_obj_add_int(doc, root, "fts_reindexed", reindexed);
    yyjson_mut_obj_add_str(doc, root, "mode", "deterministic_mvp_pass");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    return result;
}

char *handle_admin_decay(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    const char *scope_arg = is_global ? NULL : project;
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, false)
                                   : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    int processed = 0;
    int rc = cbm_store_memory_decay(store, scope_arg, cbm_mcp_get_int_arg(args, "limit", 100),
                                    &processed);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "processed", processed);
    yyjson_mut_obj_add_str(doc, root, "formula", "age_days/30 * (1-confidence) * (1-reusability)");
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    return result;
}
char *handle_memory_health(cbm_mcp_server_t *srv, const char *args) {
    char *project = cbm_mcp_get_string_arg(args, "project");
    if (!project)
        return cbm_mcp_text_result("project is required", true);
    char *scope = cbm_mcp_get_string_arg(args, "scope");
    bool is_global = scope && strcmp(scope, "global") == 0;
    free(scope);
    const char *scope_arg = is_global ? NULL : project;
    cbm_store_t *store = is_global ? resolve_global_memory_store(srv, false)
                                   : resolve_memory_store(srv, project, false);
    if (!store) {
        char *_err = build_project_list_error(is_global ? "no global memories yet"
                                                        : "project not found or not indexed");
        char *_res = cbm_mcp_text_result(_err, true);
        free(_err);
        free(project);
        return _res;
    }
    cbm_memory_health_t h = {0};
    int rc = cbm_store_memory_health(store, scope_arg, &h);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "project", project);
    yyjson_mut_obj_add_int(doc, root, "events", h.event_count);
    yyjson_mut_obj_add_int(doc, root, "items", h.item_count);
    yyjson_mut_obj_add_int(doc, root, "edges", h.edge_count);
    yyjson_mut_obj_add_int(doc, root, "candidates", h.candidate_count);
    yyjson_mut_obj_add_int(doc, root, "active", h.active_count);
    yyjson_mut_obj_add_int(doc, root, "deprecated", h.deprecated_count);
    yyjson_mut_obj_add_int(doc, root, "archived", h.archived_count);
    yyjson_mut_obj_add_int(doc, root, "retracted", h.retracted_count);
    yyjson_mut_obj_add_int(doc, root, "deleted", h.deleted_count);
    yyjson_mut_obj_add_int(doc, root, "total_hits", h.total_hits);
    yyjson_mut_obj_add_int(doc, root, "conflicts", h.conflict_count);
    yyjson_mut_obj_add_int(doc, root, "scopes", h.scope_count);
    yyjson_mut_obj_add_real(doc, root, "hit_rate", h.hit_rate);
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json, rc != CBM_STORE_OK);
    free(json);
    free(project);
    return result;
}

/* P1: handle_adr_list — structured ADR index for human browsing and tool use.
 * Mirrors the MEMORY.md concept: a browsable, filterable list of architectural
 * decisions instead of an opaque full-text-retrieval-only store.
 *
 * Takes cbm_mcp_server_t *srv for API compatibility but does NOT access its
 * internals — opens memory stores independently via the public cbm_store_* API. */

char *handle_adr_list(cbm_mcp_server_t *srv, const char *args) {
    (void)srv; /* opaque — never accessed */

    yyjson_doc *adoc = yyjson_read(args ? args : "{}", args ? strlen(args) : CBM_SZ_2, 0);
    if (!adoc)
        return cbm_mcp_text_result("invalid JSON arguments", true);

    char *project = memory_arg_string_dup(adoc, "project");
    char *kind = memory_arg_string_dup(adoc, "kind");
    char *status = memory_arg_string_dup(adoc, "status");
    char *entity_key = memory_arg_string_dup(adoc, "entity_key");
    int limit = 50;
    {
        yyjson_val *lv = yyjson_obj_get(yyjson_doc_get_root(adoc), "limit");
        if (lv && yyjson_is_int(lv)) {
            int v = (int)yyjson_get_int(lv);
            if (v > 0 && v <= 200)
                limit = v;
        }
    }
    yyjson_doc_free(adoc);

    if (!project) {
        free(kind);
        free(status);
        free(entity_key);
        return cbm_mcp_text_result("project is required", true);
    }

    /* Normalize phantom names so adr_list works on "-memory" aliases. */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }

    cbm_store_t *store = open_memory_store_for_project(project);
    if (!store) {
        char *res = cbm_mcp_text_result("project not found or not indexed", true);
        free(project);
        free(kind);
        free(status);
        free(entity_key);
        return res;
    }

    char *json = NULL;
    int rc = cbm_store_memory_adr_list(store, project, kind, status, entity_key, limit, &json);

    /* Union with the global (cross-project) store — same semantics as
     * handle_memories_retrieve. Global ADRs carry scope_project=NULL and
     * should surface from every project's adr_list. Both result sets are
     * parsed, merged by composite score, and truncated to limit. */
    char *gjson = NULL;
    {
        char gmem_path[CBM_SZ_1K];
        cbm_store_t *gstore = NULL;
        if (cbm_memory_db_path(CBM_GLOBAL_MEMORY_PROJECT, gmem_path, sizeof(gmem_path)) ==
                CBM_STORE_OK &&
            cbm_file_exists(gmem_path)) {
            gstore = cbm_store_open_path_query(gmem_path);
        }
        if (gstore && rc == CBM_STORE_OK && json) {
            int grc =
                cbm_store_memory_adr_list_global(gstore, kind, status, entity_key, limit, &gjson);
            if (grc == CBM_STORE_OK && gjson && gjson[0]) {
                yyjson_doc *pdoc = yyjson_read(json, strlen(json), 0);
                yyjson_doc *gdoc = yyjson_read(gjson, strlen(gjson), 0);
                if (pdoc && gdoc) {
                    yyjson_val *proot = yyjson_doc_get_root(pdoc);
                    yyjson_val *groot = yyjson_doc_get_root(gdoc);
                    yyjson_val *pitems = yyjson_obj_get(proot, "items");
                    yyjson_val *gitems = yyjson_obj_get(groot, "items");
                    int ptotal = 0, gtotal = 0;
                    yyjson_val *ptv = yyjson_obj_get(proot, "total");
                    yyjson_val *gtv = yyjson_obj_get(groot, "total");
                    if (ptv && yyjson_is_int(ptv))
                        ptotal = (int)yyjson_get_int(ptv);
                    if (gtv && yyjson_is_int(gtv))
                        gtotal = (int)yyjson_get_int(gtv);

                    /* Collect item pointers (raw immutable values) with their
                     * composite scores, sort, then field-copy the top-N into
                     * a fresh mutable doc. Avoids yyjson deep-copy ownership
                     * complications while preserving correct score ordering. */
                    struct {
                        const yyjson_val *val;
                        double score;
                    } *items = NULL;
                    int nitems = 0, icap = 0;
                    if (pitems && yyjson_is_arr(pitems)) {
                        size_t pi, pmax;
                        yyjson_val *pv;
                        yyjson_arr_foreach(pitems, pi, pmax, pv) {
                            if (nitems == icap) {
                                icap = icap ? icap * 2 : 64;
                                /* Realloc into a temp first: on failure keep the
                                 * existing buffer (memleak-on-realloc); break and
                                 * process what we already collected. */
                                void *grown = realloc(items, (size_t)icap * sizeof(*items));
                                if (!grown) {
                                    break;
                                }
                                items = grown;
                            }
                            if (items) {
                                yyjson_val *imp = yyjson_obj_get(pv, "importance");
                                yyjson_val *con = yyjson_obj_get(pv, "confidence");
                                yyjson_val *reu = yyjson_obj_get(pv, "reusability");
                                yyjson_val *spe = yyjson_obj_get(pv, "specificity");
                                yyjson_val *hc = yyjson_obj_get(pv, "hit_count");
                                yyjson_val *dec = yyjson_obj_get(pv, "decay");
                                items[nitems].val = pv;
                                items[nitems].score =
                                    (imp && yyjson_is_num(imp) ? yyjson_get_real(imp) : 0.0) +
                                    (con && yyjson_is_num(con) ? yyjson_get_real(con) : 0.0) +
                                    (reu && yyjson_is_num(reu) ? yyjson_get_real(reu) : 0.0) +
                                    (spe && yyjson_is_num(spe) ? yyjson_get_real(spe) : 0.0) +
                                    (hc && yyjson_is_int(hc) ? (double)yyjson_get_int(hc) : 0.0) -
                                    (dec && yyjson_is_num(dec) ? yyjson_get_real(dec) : 0.0);
                                nitems++;
                            }
                        }
                    }
                    if (gitems && yyjson_is_arr(gitems)) {
                        size_t gi, gmax;
                        yyjson_val *gv;
                        yyjson_arr_foreach(gitems, gi, gmax, gv) {
                            if (nitems == icap) {
                                icap = icap ? icap * 2 : 64;
                                /* Realloc into a temp first: on failure keep the
                                 * existing buffer (memleak-on-realloc); break and
                                 * process what we already collected. */
                                void *grown = realloc(items, (size_t)icap * sizeof(*items));
                                if (!grown) {
                                    break;
                                }
                                items = grown;
                            }
                            if (items) {
                                yyjson_val *imp = yyjson_obj_get(gv, "importance");
                                yyjson_val *con = yyjson_obj_get(gv, "confidence");
                                yyjson_val *reu = yyjson_obj_get(gv, "reusability");
                                yyjson_val *spe = yyjson_obj_get(gv, "specificity");
                                yyjson_val *hc = yyjson_obj_get(gv, "hit_count");
                                yyjson_val *dec = yyjson_obj_get(gv, "decay");
                                items[nitems].val = gv;
                                items[nitems].score =
                                    (imp && yyjson_is_num(imp) ? yyjson_get_real(imp) : 0.0) +
                                    (con && yyjson_is_num(con) ? yyjson_get_real(con) : 0.0) +
                                    (reu && yyjson_is_num(reu) ? yyjson_get_real(reu) : 0.0) +
                                    (spe && yyjson_is_num(spe) ? yyjson_get_real(spe) : 0.0) +
                                    (hc && yyjson_is_int(hc) ? (double)yyjson_get_int(hc) : 0.0) -
                                    (dec && yyjson_is_num(dec) ? yyjson_get_real(dec) : 0.0);
                                nitems++;
                            }
                        }
                    }
                    /* Bubble sort by score descending (items list is small; limit <= 200). */
                    if (items) {
                        for (int a = 0; a < nitems; a++) {
                            for (int b = a + 1; b < nitems; b++) {
                                if (items[b].score > items[a].score) {
                                    double ts = items[a].score;
                                    items[a].score = items[b].score;
                                    items[b].score = ts;
                                    const yyjson_val *tv = items[a].val;
                                    items[a].val = items[b].val;
                                    items[b].val = tv;
                                }
                            }
                        }
                        int out_n = nitems < limit ? nitems : limit;

                        /* Field-by-field copy from the sorted immutable items into
                         * a single mutable doc — avoids yyjson deep-copy ownership
                         * issues while staying type-safe. */
                        yyjson_mut_doc *mdoc = yyjson_mut_doc_new(NULL);
                        yyjson_mut_val *mroot = yyjson_mut_obj(mdoc);
                        yyjson_mut_doc_set_root(mdoc, mroot);
                        yyjson_mut_obj_add_str(mdoc, mroot, "project", project);
                        yyjson_mut_obj_add_int(mdoc, mroot, "total", ptotal + gtotal);
                        yyjson_mut_val *marr = yyjson_mut_arr(mdoc);
                        for (int k = 0; k < out_n; k++) {
                            const yyjson_val *src = items[k].val;
                            yyjson_mut_val *obj = yyjson_mut_obj(mdoc);
#define CP_STR(mdoc, obj, key, src)                                             \
    do {                                                                        \
        yyjson_val *v = yyjson_obj_get((src), (key));                           \
        if (v && yyjson_is_str(v))                                              \
            yyjson_mut_obj_add_strcpy((mdoc), (obj), (key), yyjson_get_str(v)); \
    } while (0)
#define CP_REAL(mdoc, obj, key, src)                                           \
    do {                                                                       \
        yyjson_val *v = yyjson_obj_get((src), (key));                          \
        if (v && yyjson_is_num(v))                                             \
            yyjson_mut_obj_add_real((mdoc), (obj), (key), yyjson_get_real(v)); \
    } while (0)
#define CP_INT(mdoc, obj, key, src)                                           \
    do {                                                                      \
        yyjson_val *v = yyjson_obj_get((src), (key));                         \
        if (v && yyjson_is_int(v))                                            \
            yyjson_mut_obj_add_int((mdoc), (obj), (key), yyjson_get_sint(v)); \
    } while (0)
                            CP_STR(mdoc, obj, "id", src);
                            CP_STR(mdoc, obj, "kind", src);
                            CP_STR(mdoc, obj, "layer", src);
                            CP_STR(mdoc, obj, "title", src);
                            CP_STR(mdoc, obj, "summary", src);
                            CP_STR(mdoc, obj, "entity_key", src);
                            CP_STR(mdoc, obj, "status", src);
                            CP_REAL(mdoc, obj, "importance", src);
                            CP_REAL(mdoc, obj, "confidence", src);
                            CP_REAL(mdoc, obj, "reusability", src);
                            CP_REAL(mdoc, obj, "specificity", src);
                            CP_INT(mdoc, obj, "hit_count", src);
                            CP_REAL(mdoc, obj, "decay", src);
                            CP_INT(mdoc, obj, "version", src);
                            CP_STR(mdoc, obj, "supersedes", src);
                            CP_INT(mdoc, obj, "created_at", src);
                            CP_INT(mdoc, obj, "updated_at", src);
#undef CP_STR
#undef CP_REAL
#undef CP_INT
                            yyjson_mut_arr_add_val(marr, obj);
                        }
                        yyjson_mut_obj_add_val(mdoc, mroot, "items", marr);
                        size_t mlen = 0;
                        char *ms =
                            yyjson_mut_write(mdoc, YYJSON_WRITE_ALLOW_INVALID_UNICODE, &mlen);
                        free(json);
                        json = ms ? strdup(ms) : NULL;
                        free(ms);
                        yyjson_mut_doc_free(mdoc);
                        free(items);
                        rc = json ? CBM_STORE_OK : CBM_STORE_ERR;
                    }
                }
                if (pdoc)
                    yyjson_doc_free(pdoc);
                if (gdoc)
                    yyjson_doc_free(gdoc);
            }
            if (gjson)
                free(gjson);
        }
        if (gstore)
            cbm_store_close(gstore);
    }

    cbm_store_close(store);

    free(project);
    free(kind);
    free(status);
    free(entity_key);

    if (rc != CBM_STORE_OK || !json) {
        return cbm_mcp_text_result("failed to query ADR list", true);
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

/* P3: handle_adr_chain — walk the supersedes chain for an ADR, showing the
 * full version timeline. Start from item_id (walk backward to root, then
 * forward to newest) or entity_key (find root at version=1). Does NOT union
 * the global store — supersedes is project-scoped by design.
 *
 * Takes cbm_mcp_server_t *srv for API compatibility but does NOT access its
 * internals — opens the memory store independently via the public API. */

char *handle_adr_chain(cbm_mcp_server_t *srv, const char *args) {
    (void)srv; /* opaque — never accessed */

    yyjson_doc *adoc = yyjson_read(args ? args : "{}", args ? strlen(args) : CBM_SZ_2, 0);
    if (!adoc)
        return cbm_mcp_text_result("invalid JSON arguments", true);

    char *project = memory_arg_string_dup(adoc, "project");
    char *entity_key = memory_arg_string_dup(adoc, "entity_key");
    char *item_id = memory_arg_string_dup(adoc, "item_id");
    int max_depth = 50;
    {
        yyjson_val *dv = yyjson_obj_get(yyjson_doc_get_root(adoc), "max_depth");
        if (dv && yyjson_is_int(dv)) {
            int v = (int)yyjson_get_int(dv);
            if (v > 0 && v <= 200)
                max_depth = v;
        }
    }
    yyjson_doc_free(adoc);

    if (!project) {
        free(entity_key);
        free(item_id);
        return cbm_mcp_text_result("project is required", true);
    }
    if (!entity_key && !item_id) {
        free(project);
        free(entity_key);
        free(item_id);
        return cbm_mcp_text_result("entity_key or item_id is required", true);
    }

    /* Normalize phantom names. */
    {
        char *canon = normalize_phantom_project(project);
        if (canon) {
            free(project);
            project = canon;
        }
    }

    cbm_store_t *store = open_memory_store_for_project(project);
    if (!store) {
        char *res = cbm_mcp_text_result("project not found or not indexed", true);
        free(project);
        free(entity_key);
        free(item_id);
        return res;
    }

    char *json = NULL;
    int rc = cbm_store_memory_adr_chain(store, project, entity_key, item_id, max_depth, &json);
    cbm_store_close(store);

    free(project);
    free(entity_key);
    free(item_id);

    if (rc != CBM_STORE_OK || !json) {
        return cbm_mcp_text_result("failed to query ADR chain", true);
    }
    char *result = cbm_mcp_text_result(json, false);
    free(json);
    return result;
}

static int manager_table_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *stmt = NULL;
    int exists = 0;
    if (db &&
        sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
    }
    sqlite3_finalize(stmt);
    return exists;
}

static int manager_count(sqlite3 *db, const char *table) {
    if (!manager_table_exists(db, table))
        return 0;
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", table);
    sqlite3_stmt *stmt = NULL;
    int value = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static char *manager_wrap(yyjson_mut_doc *doc) {
    char *json = yy_doc_to_str(doc);
    yyjson_mut_doc_free(doc);
    char *result = cbm_mcp_text_result(json ? json : "{}", json == NULL);
    free(json);
    return result;
}

static yyjson_mut_doc *manager_detail_doc(const char *schema, const char *status,
                                          yyjson_mut_val **item) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    *item = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_str(doc, root, "schema", schema);
    yyjson_mut_obj_add_str(doc, root, "status", status);
    yyjson_mut_obj_add_val(doc, root, "item", *item);
    return doc;
}

static yyjson_mut_doc *manager_list_doc(const char *schema, const char *status,
                                        yyjson_mut_val **root, yyjson_mut_val **items) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    *root = yyjson_mut_obj(doc);
    *items = yyjson_mut_arr(doc);
    yyjson_mut_doc_set_root(doc, *root);
    yyjson_mut_obj_add_str(doc, *root, "schema", schema);
    yyjson_mut_obj_add_str(doc, *root, "status", status);
    yyjson_mut_obj_add_val(doc, *root, "items", *items);
    return doc;
}

static sqlite3 *manager_global_db(cbm_mcp_server_t *srv) {
    cbm_store_t *store = resolve_global_memory_store(srv, false);
    return store ? cbm_store_get_db(store) : NULL;
}

static sqlite3 *manager_global_graph_db(cbm_mcp_server_t *srv) {
    cbm_store_t *store = resolve_global_graph_store(srv);
    return store ? cbm_store_get_db(store) : NULL;
}

static int manager_count_for(sqlite3 *db, const char *sql, const char *value) {
    sqlite3_stmt *stmt = NULL;
    int count = 0;
    if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, value ? value : "", -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

static const char *manager_text(sqlite3_stmt *stmt, int column) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? (const char *)value : "";
}

static int manager_isolated_mock_enabled(void) {
    char guard[16] = {0}, cache[CBM_SZ_1K] = {0}, temp[CBM_SZ_1K] = {0};
    if (!cbm_safe_getenv("CBM_STAGE14_MANAGER_ISOLATED_MOCK", guard, sizeof(guard), NULL) ||
        strcmp(guard, "1") != 0)
        return 0;
    const char *cache_dir = cbm_resolve_cache_dir(), *temp_dir = cbm_tmpdir();
    if (!cache_dir || !temp_dir)
        return 0;
    snprintf(cache, sizeof(cache), "%s", cache_dir);
    snprintf(temp, sizeof(temp), "%s", temp_dir);
    cbm_normalize_path_sep(cache);
    cbm_normalize_path_sep(temp);
    size_t n = strlen(temp);
    while (n > 1 && temp[n - 1] == '/')
        temp[--n] = '\0';
#ifdef _WIN32
    return _strnicmp(cache, temp, n) == 0 && (cache[n] == '\0' || cache[n] == '/');
#else
    return strncmp(cache, temp, n) == 0 && (cache[n] == '\0' || cache[n] == '/');
#endif
}

static const char *manager_maintenance_state(sqlite3 *db) {
    if (!manager_table_exists(db, "global_maintenance_lease"))
        return "idle";
    sqlite3_stmt *stmt = NULL;
    const char *state = "idle";
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM global_maintenance_lease WHERE "
                           "expires_at>strftime('%Y-%m-%dT%H:%M:%SZ','now');",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0)
        state = "running";
    sqlite3_finalize(stmt);
    return state;
}

char *handle_manager_global_overview(cbm_mcp_server_t *srv, const char *args) {
    (void)args;
    sqlite3 *db = manager_global_db(srv), *graph = manager_global_graph_db(srv);
    yyjson_mut_val *item = NULL;
    yyjson_mut_doc *doc = manager_detail_doc("semantic-memory-manager-global-overview/v1",
                                             db ? "ok" : "unavailable", &item);
    yyjson_mut_obj_add_int(doc, item, "project_count", manager_count(db, "global_project_catalog"));
    yyjson_mut_obj_add_int(doc, item, "memory_count", manager_count(db, "memory_item"));
    yyjson_mut_obj_add_int(doc, item, "cross_project_edge_count",
                           manager_count(graph, "global_cross_project_edge"));
    yyjson_mut_obj_add_int(doc, item, "task_count", manager_count(db, "memory_task"));
    yyjson_mut_obj_add_str(doc, item, "maintenance_state", manager_maintenance_state(db));
    yyjson_mut_obj_add_bool(doc, item, "global_default_candidate_pool", true);
    yyjson_mut_val *projects = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, item, "projects", projects);
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "c.project_uuid,c.display_name,c.workspace_state,c.index_state,c.last_seen_at,(SELECT "
        "COUNT(*) FROM global_memory_provenance p WHERE p.project_uuid=c.project_uuid),(SELECT "
        "COUNT(*) FROM global_task_workspace w WHERE w.project_uuid=c.project_uuid) FROM "
        "global_project_catalog c ORDER BY c.display_name,c.project_uuid;";
    if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *row = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, row, "project_uuid", manager_text(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc, row, "display_name", manager_text(stmt, 1));
            yyjson_mut_obj_add_strcpy(doc, row, "workspace_state", manager_text(stmt, 2));
            yyjson_mut_obj_add_strcpy(doc, row, "index_state", manager_text(stmt, 3));
            yyjson_mut_obj_add_strcpy(doc, row, "last_seen_at", manager_text(stmt, 4));
            yyjson_mut_obj_add_int(doc, row, "memory_count", sqlite3_column_int(stmt, 5));
            yyjson_mut_obj_add_int(doc, row, "task_count", sqlite3_column_int(stmt, 6));
            yyjson_mut_arr_add_val(projects, row);
        }
    }
    sqlite3_finalize(stmt);
    return manager_wrap(doc);
}

char *handle_manager_global_memory(cbm_mcp_server_t *srv, const char *args) {
    sqlite3 *db = manager_global_db(srv);
    int cursor = cbm_mcp_get_int_arg(args, "cursor", 0),
        limit = cbm_mcp_get_int_arg(args, "limit", 50);
    if (cursor < 0)
        cursor = 0;
    if (limit < 1)
        limit = 1;
    if (limit > 200)
        limit = 200;
    yyjson_mut_val *root = NULL, *items = NULL;
    yyjson_mut_doc *doc = manager_list_doc("semantic-memory-manager-global-memory/v1",
                                           db ? "ok" : "unavailable", &root, &items);
    int returned = 0, total = manager_count(db, "memory_item");
    const char *sql =
        "SELECT "
        "m.id,m.kind,COALESCE(m.title,''),COALESCE(m.summary,''),m.status,COALESCE(p.project_uuid,"
        "m.scope_project,'__global__'),COALESCE(c.display_name,COALESCE(p.project_uuid,m.scope_"
        "project,'Global')),COALESCE((SELECT e.evidence_grade FROM global_evolution_event e WHERE "
        "e.object_id=m.id ORDER BY e.sequence_no DESC LIMIT "
        "1),'ungraded'),m.created_at,COALESCE(p.source_kind,'global_memory'),COALESCE(p.legacy_"
        "project_id,'') FROM memory_item m LEFT JOIN global_memory_provenance p ON "
        "p.memory_item_id=m.id LEFT JOIN global_project_catalog c ON c.project_uuid=p.project_uuid "
        "WHERE m.deleted_at IS NULL ORDER BY m.updated_at DESC,m.id LIMIT ?1 OFFSET ?2;";
    sqlite3_stmt *stmt = NULL;
    if (db && sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        sqlite3_bind_int(stmt, 2, cursor);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *row = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, row, "memory_item_id", manager_text(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc, row, "kind", manager_text(stmt, 1));
            yyjson_mut_obj_add_strcpy(doc, row, "title", manager_text(stmt, 2));
            yyjson_mut_obj_add_strcpy(doc, row, "summary", manager_text(stmt, 3));
            yyjson_mut_obj_add_strcpy(doc, row, "status", manager_text(stmt, 4));
            yyjson_mut_obj_add_strcpy(doc, row, "project_uuid", manager_text(stmt, 5));
            yyjson_mut_obj_add_strcpy(doc, row, "project_name", manager_text(stmt, 6));
            yyjson_mut_obj_add_strcpy(doc, row, "evidence_grade", manager_text(stmt, 7));
            yyjson_mut_obj_add_strcpy(doc, row, "created_at", manager_text(stmt, 8));
            yyjson_mut_val *prov = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, prov, "project_uuid", manager_text(stmt, 5));
            yyjson_mut_obj_add_strcpy(doc, prov, "project_name", manager_text(stmt, 6));
            yyjson_mut_obj_add_strcpy(doc, prov, "source_kind", manager_text(stmt, 9));
            yyjson_mut_obj_add_strcpy(doc, prov, "legacy_project_id", manager_text(stmt, 10));
            yyjson_mut_obj_add_val(doc, row, "provenance", prov);
            yyjson_mut_arr_add_val(items, row);
            returned++;
        }
    }
    sqlite3_finalize(stmt);
    if (cursor + returned < total)
        yyjson_mut_obj_add_int(doc, root, "next_cursor", cursor + returned);
    else
        yyjson_mut_obj_add_null(doc, root, "next_cursor");
    yyjson_mut_obj_add_int(doc, root, "total", total);
    return manager_wrap(doc);
}

char *handle_manager_global_topology(cbm_mcp_server_t *srv, const char *args) {
    sqlite3 *db = manager_global_graph_db(srv);
    int cursor = cbm_mcp_get_int_arg(args, "cursor", 0),
        limit = cbm_mcp_get_int_arg(args, "limit", 50);
    if (cursor < 0)
        cursor = 0;
    if (limit < 1)
        limit = 1;
    if (limit > 200)
        limit = 200;
    yyjson_mut_val *root = NULL, *items = NULL;
    yyjson_mut_doc *doc = manager_list_doc("semantic-memory-manager-global-topology/v1",
                                           db ? "ok" : "unavailable", &root, &items);
    int total = manager_count(db, "global_cross_project_edge"), returned = 0;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT "
        "e.edge_id,e.source_project_uuid,e.target_project_uuid,e.relation_type,e.weight_ppm,e."
        "confidence_ppm,e.status,e.version,COALESCE(v.evidence_event_id,''),COALESCE(v.payload_"
        "sha256,'') FROM global_cross_project_edge e LEFT JOIN global_cross_project_edge_version v "
        "ON v.edge_id=e.edge_id AND v.version=e.version ORDER BY e.edge_id LIMIT ?1 OFFSET ?2;";
    if (db && manager_table_exists(db, "global_cross_project_edge") &&
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        sqlite3_bind_int(stmt, 2, cursor);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *row = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, row, "edge_id", manager_text(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc, row, "source_project_uuid", manager_text(stmt, 1));
            yyjson_mut_obj_add_strcpy(doc, row, "target_project_uuid", manager_text(stmt, 2));
            yyjson_mut_obj_add_strcpy(doc, row, "relation_type", manager_text(stmt, 3));
            yyjson_mut_obj_add_int(doc, row, "weight_ppm", sqlite3_column_int(stmt, 4));
            yyjson_mut_obj_add_int(doc, row, "confidence_ppm", sqlite3_column_int(stmt, 5));
            yyjson_mut_obj_add_strcpy(doc, row, "status", manager_text(stmt, 6));
            yyjson_mut_obj_add_int(doc, row, "version", sqlite3_column_int(stmt, 7));
            yyjson_mut_val *prov = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_str(doc, prov, "source_kind", "global_cross_project_graph");
            yyjson_mut_obj_add_strcpy(doc, prov, "evidence_event_id", manager_text(stmt, 8));
            yyjson_mut_obj_add_strcpy(doc, prov, "payload_sha256", manager_text(stmt, 9));
            yyjson_mut_obj_add_val(doc, row, "provenance", prov);
            yyjson_mut_arr_add_val(items, row);
            returned++;
        }
    }
    sqlite3_finalize(stmt);
    if (cursor + returned < total)
        yyjson_mut_obj_add_int(doc, root, "next_cursor", cursor + returned);
    else
        yyjson_mut_obj_add_null(doc, root, "next_cursor");
    yyjson_mut_obj_add_int(doc, root, "total", total);
    return manager_wrap(doc);
}

char *handle_manager_evolution(cbm_mcp_server_t *srv, const char *args) {
    sqlite3 *db = manager_global_db(srv);
    int cursor = cbm_mcp_get_int_arg(args, "cursor", 0),
        limit = cbm_mcp_get_int_arg(args, "limit", 50);
    if (cursor < 0)
        cursor = 0;
    if (limit < 1)
        limit = 1;
    if (limit > 200)
        limit = 200;
    yyjson_mut_val *root = NULL, *items = NULL;
    yyjson_mut_doc *doc = manager_list_doc("semantic-memory-manager-evolution/v1",
                                           db ? "ok" : "unavailable", &root, &items);
    int total = manager_count(db, "global_evolution_event"), returned = 0;
    sqlite3_stmt *stmt = NULL;
    if (db && manager_table_exists(db, "global_evolution_event") &&
        sqlite3_prepare_v2(
            db,
            "SELECT "
            "event_id,sequence_no,COALESCE(task_id,''),project_uuid,object_kind,object_id,"
            "operation,evidence_grade,COALESCE(evidence_id,''),before_sha256,after_sha256,created_"
            "at FROM global_evolution_event ORDER BY sequence_no DESC LIMIT ?1 OFFSET ?2;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, limit);
        sqlite3_bind_int(stmt, 2, cursor);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *row = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, row, "event_id", manager_text(stmt, 0));
            yyjson_mut_obj_add_int(doc, row, "sequence_no", sqlite3_column_int64(stmt, 1));
            if (manager_text(stmt, 2)[0])
                yyjson_mut_obj_add_strcpy(doc, row, "task_id", manager_text(stmt, 2));
            else
                yyjson_mut_obj_add_null(doc, row, "task_id");
            const char *names[] = {"project_uuid",  "object_kind",    "object_id",
                                   "operation",     "evidence_grade", "evidence_id",
                                   "before_sha256", "after_sha256",   "created_at"};
            for (int i = 0; i < 9; i++)
                yyjson_mut_obj_add_strcpy(doc, row, names[i], manager_text(stmt, i + 3));
            yyjson_mut_arr_add_val(items, row);
            returned++;
        }
    }
    sqlite3_finalize(stmt);
    if (cursor + returned < total)
        yyjson_mut_obj_add_int(doc, root, "next_cursor", cursor + returned);
    else
        yyjson_mut_obj_add_null(doc, root, "next_cursor");
    yyjson_mut_obj_add_int(doc, root, "total", total);
    return manager_wrap(doc);
}

char *handle_manager_task_chain(cbm_mcp_server_t *srv, const char *args) {
    sqlite3 *db = manager_global_db(srv);
    char *task_id = cbm_mcp_get_string_arg(args, "task_id");
    int exists =
        manager_count_for(db, "SELECT COUNT(*) FROM memory_task WHERE task_id=?1;", task_id) > 0;
    yyjson_mut_val *item = NULL;
    yyjson_mut_doc *doc = manager_detail_doc("semantic-memory-manager-task-chain/v1",
                                             exists ? "ok" : "not_found", &item);
    yyjson_mut_val *links = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, item, "links", links);
    if (task_id) {
        yyjson_mut_obj_add_strcpy(doc, item, "task_id", task_id);
        sqlite3_stmt *stmt = NULL;
        char project_uuid[128] = {0};
        if (db && sqlite3_prepare_v2(
                      db, "SELECT project,task_type,created_at FROM memory_task WHERE task_id=?1;",
                      -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                snprintf(project_uuid, sizeof(project_uuid), "%s", manager_text(stmt, 0));
                yyjson_mut_obj_add_strcpy(doc, item, "project_uuid", project_uuid);
                yyjson_mut_obj_add_strcpy(doc, item, "task_type", manager_text(stmt, 1));
                yyjson_mut_obj_add_strcpy(doc, item, "created_at", manager_text(stmt, 2));
                yyjson_mut_val *link = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, link, "kind", "task");
                yyjson_mut_obj_add_strcpy(doc, link, "id", task_id);
                yyjson_mut_obj_add_strcpy(doc, link, "label", manager_text(stmt, 1));
                yyjson_mut_obj_add_strcpy(doc, link, "project_uuid", project_uuid);
                yyjson_mut_arr_add_val(links, link);
            }
        }
        sqlite3_finalize(stmt);
        int evidence_count = manager_count_for(
                db, "SELECT COUNT(*) FROM memory_evidence WHERE task_id=?1;", task_id),
            attribution_count = manager_count_for(
                db, "SELECT COUNT(*) FROM codex_task_attribution WHERE task_id=?1;", task_id);
        yyjson_mut_obj_add_int(doc, item, "evidence_count", evidence_count);
        yyjson_mut_obj_add_int(doc, item, "attribution_total", attribution_count);
        const char *grade = "ungraded";
        if (manager_count_for(db,
                              "SELECT COUNT(*) FROM memory_evidence WHERE task_id=?1 AND "
                              "trust_class='external_verified' AND evidence_state='valid';",
                              task_id) > 0)
            grade = "A";
        else if (manager_count_for(db,
                                   "SELECT COUNT(*) FROM memory_evidence WHERE task_id=?1 AND "
                                   "trust_class='explicit_user' AND evidence_state='valid';",
                                   task_id) > 0)
            grade = "B";
        else if (evidence_count > 0)
            grade = "C";
        yyjson_mut_obj_add_str(doc, item, "evidence_grade", grade);
        if (db &&
            sqlite3_prepare_v2(db,
                               "SELECT evidence_id,trust_class,evidence_state,source_type FROM "
                               "memory_evidence WHERE task_id=?1 ORDER BY created_at,evidence_id;",
                               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                yyjson_mut_val *link = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, link, "kind", "evidence");
                yyjson_mut_obj_add_strcpy(doc, link, "id", manager_text(stmt, 0));
                yyjson_mut_obj_add_strcpy(doc, link, "label", manager_text(stmt, 1));
                yyjson_mut_obj_add_strcpy(doc, link, "provenance", manager_text(stmt, 3));
                yyjson_mut_arr_add_val(links, link);
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (db && sqlite3_prepare_v2(
                      db,
                      "SELECT attribution_id,memory_item_id,attribution_state FROM "
                      "codex_task_attribution WHERE task_id=?1 ORDER BY created_at,attribution_id;",
                      -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                yyjson_mut_val *link = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, link, "kind", "attribution");
                yyjson_mut_obj_add_strcpy(doc, link, "id", manager_text(stmt, 0));
                yyjson_mut_obj_add_strcpy(doc, link, "label", manager_text(stmt, 2));
                yyjson_mut_obj_add_strcpy(doc, link, "provenance", manager_text(stmt, 1));
                yyjson_mut_arr_add_val(links, link);
            }
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (db &&
            sqlite3_prepare_v2(db,
                               "SELECT event_id,operation,project_uuid FROM global_evolution_event "
                               "WHERE task_id=?1 ORDER BY sequence_no;",
                               -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, task_id, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                yyjson_mut_val *link = yyjson_mut_obj(doc);
                yyjson_mut_obj_add_str(doc, link, "kind", "evolution");
                yyjson_mut_obj_add_strcpy(doc, link, "id", manager_text(stmt, 0));
                yyjson_mut_obj_add_strcpy(doc, link, "label", manager_text(stmt, 1));
                yyjson_mut_obj_add_strcpy(doc, link, "project_uuid", manager_text(stmt, 2));
                yyjson_mut_arr_add_val(links, link);
            }
        }
        sqlite3_finalize(stmt);
    }
    char *response = manager_wrap(doc);
    free(task_id);
    return response;
}

char *handle_manager_drift_preview(cbm_mcp_server_t *srv, const char *args) {
    (void)args;
    sqlite3 *db = manager_global_db(srv);
    yyjson_mut_val *item = NULL;
    yyjson_mut_doc *doc = manager_detail_doc("semantic-memory-manager-drift-preview/v1",
                                             db ? "ok" : "unavailable", &item);
    int count = manager_count(db, "global_config_drift_event");
    char classification[128] = "none";
    sqlite3_stmt *stmt = NULL;
    if (db &&
        sqlite3_prepare_v2(db,
                           "SELECT classification FROM global_config_drift_event ORDER BY "
                           "created_at DESC,event_id DESC LIMIT 1;",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW)
        snprintf(classification, sizeof(classification), "%s", manager_text(stmt, 0));
    sqlite3_finalize(stmt);
    int blocking = count > 0 && strncmp(classification, "GREEN_", 6) != 0 &&
                   strcmp(classification, "none") != 0;
    yyjson_mut_obj_add_int(doc, item, "event_count", count);
    yyjson_mut_obj_add_bool(doc, item, "third_party_config_body_included", false);
    yyjson_mut_obj_add_str(doc, item, "classification", classification);
    yyjson_mut_obj_add_bool(doc, item, "blocking", blocking != 0);
    yyjson_mut_val *repair = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, repair, "action", blocking ? "restore_managed_fields" : "none");
    yyjson_mut_obj_add_int(doc, repair, "writes_required", 0);
    yyjson_mut_obj_add_bool(doc, repair, "requires_confirmation", blocking != 0);
    yyjson_mut_obj_add_val(doc, item, "repair_preview", repair);
    return manager_wrap(doc);
}

char *handle_manager_maintenance_preview(cbm_mcp_server_t *srv, const char *args) {
    (void)args;
    cbm_store_t *store = resolve_global_memory_store(srv, false);
    sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
    cbm_evolution_maintenance_input_t input = {.mode = "shadow",
                                               .project_uuid = "*",
                                               .owner_id = "manager-preview",
                                               .idempotency_key = "manager-preview-shadow-v1",
                                               .limit = 1000,
                                               .budget_seconds = 30,
                                               .isolated_write_allowed = 0,
                                               .production_gate_allowed = 0};
    cbm_evolution_result_t preview = {0};
    int preview_rc =
        store ? cbm_evolution_maintenance_store(store, &input, &preview) : CBM_STORE_ERR;
    yyjson_mut_val *item = NULL;
    yyjson_mut_doc *doc =
        manager_detail_doc("semantic-memory-manager-maintenance-preview/v1",
                           preview_rc == CBM_STORE_OK ? "ok" : "unavailable", &item);
    yyjson_mut_obj_add_str(doc, item, "state", manager_maintenance_state(db));
    yyjson_mut_obj_add_str(doc, item, "mode",
                           manager_isolated_mock_enabled() ? "isolated_mock" : "production");
    yyjson_mut_obj_add_int(doc, item, "candidate_memories", preview.eligible);
    yyjson_mut_obj_add_str(doc, item, "controller_mode", "shadow");
    yyjson_mut_obj_add_bool(doc, item, "controller_wrote", false);
    yyjson_mut_obj_add_int(doc, item, "budget_seconds_max", 30);
    yyjson_mut_obj_add_bool(doc, item, "hard_delete_allowed", false);
    stage14_canary_authorization_t authorization = {0};
    const char *authorization_code = "PRODUCTION_GATE_NOT_LOADED";
    int authorization_ready =
        stage14_load_canary_authorization(NULL, NULL, false, &authorization, &authorization_code);
    char authorization_path[STAGE14_CANARY_PATH_CAP] = {0};
    char authorization_sha256[80] = {0};
    cbm_safe_getenv("CBM_STAGE14_CANARY_AUTH_MANIFEST", authorization_path,
                    sizeof(authorization_path), NULL);
    cbm_safe_getenv("CBM_STAGE14_CANARY_AUTH_SHA256", authorization_sha256,
                    sizeof(authorization_sha256), NULL);
    yyjson_mut_obj_add_bool(doc, item, "production_gate_loaded", authorization_ready != 0);
    yyjson_mut_obj_add_bool(doc, item, "production_actions_enabled", authorization_ready != 0);
    yyjson_mut_obj_add_str(doc, item, "production_gate_status", authorization_code);
    yyjson_mut_obj_add_strcpy(doc, item, "authorization_manifest_path", authorization_path);
    yyjson_mut_obj_add_strcpy(doc, item, "authorization_manifest_sha256", authorization_sha256);
    yyjson_mut_obj_add_strcpy(doc, item, "task_evolution_manifest_path",
                              authorization.task_evolution_manifest_path);
    yyjson_mut_obj_add_strcpy(doc, item, "task_evolution_manifest_sha256",
                              authorization.task_evolution_manifest_sha256);
    yyjson_mut_obj_add_strcpy(doc, item, "edge_manifest_path", authorization.edge_manifest_path);
    yyjson_mut_obj_add_strcpy(doc, item, "edge_manifest_sha256",
                              authorization.edge_manifest_sha256);
    yyjson_mut_obj_add_strcpy(doc, item, "concept_manifest_path",
                              authorization.concept_manifest_path);
    yyjson_mut_obj_add_strcpy(doc, item, "concept_manifest_sha256",
                              authorization.concept_manifest_sha256);
    yyjson_mut_obj_add_strcpy(doc, item, "authorization_expires_at", authorization.expires_at);
    yyjson_mut_obj_add_bool(doc, item, "production_pause_supported", false);
    yyjson_mut_obj_add_str(doc, item, "production_pause_status", "CORE_PAUSE_API_UNAVAILABLE");
    sqlite3_stmt *stmt = NULL;
    int lease = 0;
    if (db &&
        sqlite3_prepare_v2(db,
                           "SELECT owner_id,checkpoint_json,acquired_at FROM "
                           "global_maintenance_lease ORDER BY acquired_at DESC LIMIT 1;",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        lease = 1;
        yyjson_mut_obj_add_strcpy(doc, item, "lease_owner", manager_text(stmt, 0));
        yyjson_mut_obj_add_strcpy(doc, item, "checkpoint", manager_text(stmt, 1));
        yyjson_mut_obj_add_strcpy(doc, item, "last_run_at", manager_text(stmt, 2));
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (!lease) {
        yyjson_mut_obj_add_null(doc, item, "lease_owner");
        yyjson_mut_obj_add_str(doc, item, "checkpoint", "none");
        yyjson_mut_obj_add_null(doc, item, "last_run_at");
    }
    yyjson_mut_val *history = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, item, "history", history);
    if (db && manager_table_exists(db, "global_maintenance_run") &&
        sqlite3_prepare_v2(
            db,
            "SELECT run_id,mode,status,started_at,COALESCE(completed_at,''),checkpoint_json FROM "
            "global_maintenance_run ORDER BY started_at DESC,run_id DESC LIMIT 50;",
            -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            yyjson_mut_val *row = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_strcpy(doc, row, "run_id", manager_text(stmt, 0));
            yyjson_mut_obj_add_strcpy(doc, row, "mode", manager_text(stmt, 1));
            yyjson_mut_obj_add_strcpy(doc, row, "status", manager_text(stmt, 2));
            yyjson_mut_obj_add_strcpy(doc, row, "created_at", manager_text(stmt, 3));
            yyjson_mut_obj_add_strcpy(doc, row, "completed_at", manager_text(stmt, 4));
            yyjson_mut_obj_add_strcpy(doc, row, "checkpoint", manager_text(stmt, 5));
            yyjson_mut_arr_add_val(history, row);
        }
    }
    sqlite3_finalize(stmt);
    cbm_evolution_result_free(&preview);
    return manager_wrap(doc);
}

static int manager_maintenance_run_state(sqlite3 *db, const char *run_id,
                                         const char *idempotency_key) {
    if (!db || !run_id || !idempotency_key || !manager_table_exists(db, "global_maintenance_run"))
        return 0;
    sqlite3_stmt *stmt = NULL;
    int state = 0;
    if (sqlite3_prepare_v2(
            db, "SELECT run_id,status FROM global_maintenance_run WHERE idempotency_key=?1;", -1,
            &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, idempotency_key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *stored_run = manager_text(stmt, 0), *status = manager_text(stmt, 1);
            if (strcmp(stored_run, run_id) != 0)
                state = -1;
            else if (!strcmp(status, "completed"))
                state = 1;
            else
                state = 2;
        }
    }
    sqlite3_finalize(stmt);
    return state;
}

char *handle_manager_maintenance_control(cbm_mcp_server_t *srv, const char *args) {
    static const char *const allowed[] = {"action",
                                          "scope",
                                          "mode",
                                          "project",
                                          "owner_id",
                                          "run_id",
                                          "idempotency_key",
                                          "frozen_as_of_ms",
                                          "limit",
                                          "budget_seconds",
                                          "edge_manifest_path",
                                          "edge_manifest_sha256",
                                          "concept_manifest_path",
                                          "concept_manifest_sha256",
                                          "authorization_manifest_path",
                                          "authorization_manifest_sha256"};
    static const char *const isolated_fields[] = {"action", "scope"};
    static const char *const preview_fields[] = {
        "action",          "scope",           "mode",  "project",       "owner_id", "run_id",
        "idempotency_key", "frozen_as_of_ms", "limit", "budget_seconds"};
    static const char *const execution_fields[] = {"action",
                                                   "scope",
                                                   "mode",
                                                   "project",
                                                   "owner_id",
                                                   "run_id",
                                                   "idempotency_key",
                                                   "frozen_as_of_ms",
                                                   "limit",
                                                   "budget_seconds",
                                                   "edge_manifest_path",
                                                   "edge_manifest_sha256",
                                                   "concept_manifest_path",
                                                   "concept_manifest_sha256",
                                                   "authorization_manifest_path",
                                                   "authorization_manifest_sha256"};
    yyjson_doc *args_doc = yyjson_read(args ? args : "", args ? strlen(args) : 0, 0);
    int arguments_allowed =
        args_doc && stage12_args_allowed(args_doc, allowed, sizeof(allowed) / sizeof(allowed[0]));
    yyjson_val *args_root = args_doc ? yyjson_doc_get_root(args_doc) : NULL;
    char *action = arguments_allowed ? memory_arg_string_dup(args_doc, "action") : NULL;
    char *scope = arguments_allowed ? memory_arg_string_dup(args_doc, "scope") : NULL;
    int isolated = scope && !strcmp(scope, "isolated_mock") && manager_isolated_mock_enabled();
    cbm_evolution_result_t result = {0};
    int controller_rc = CBM_STORE_REJECTED;
    const char *status = "rejected", *code = "INVALID_ARGUMENT", *controller_mode = "none";
    int production_state_written = 0, authorization_ready = 0;
    stage14_canary_authorization_t authorization = {0};
    const char *authorization_code = "PRODUCTION_GATE_NOT_LOADED";

    if (action && scope && isolated &&
        stage14_object_exact_keys(args_root, isolated_fields,
                                  sizeof(isolated_fields) / sizeof(isolated_fields[0])) &&
        (!strcmp(action, "dry_run") || !strcmp(action, "pause") || !strcmp(action, "resume"))) {
        if (!strcmp(action, "dry_run")) {
            cbm_store_t *store = resolve_global_memory_store(srv, false);
            cbm_evolution_maintenance_input_t input = {.mode = "dry_run",
                                                       .project_uuid = "*",
                                                       .owner_id = "manager-control",
                                                       .idempotency_key =
                                                           "manager-control-dry-run-v1",
                                                       .limit = 1000,
                                                       .budget_seconds = 30,
                                                       .isolated_write_allowed = 1,
                                                       .production_gate_allowed = 0};
            if (store)
                controller_rc = cbm_evolution_maintenance_store(store, &input, &result);
            status = controller_rc == CBM_STORE_OK ? "planned" : "rejected";
            code = controller_rc == CBM_STORE_OK ? "ISOLATED_DRY_RUN_READY"
                                                 : "ISOLATED_DRY_RUN_FAILED";
            controller_mode = "dry_run";
        } else {
            status = "simulated";
            code = "ISOLATED_CONTROL_SIMULATED";
        }
    } else if (action && scope && !strcmp(scope, "production")) {
        char *mode = memory_arg_string_dup(args_doc, "mode");
        char *project = memory_arg_string_dup(args_doc, "project");
        char *owner = memory_arg_string_dup(args_doc, "owner_id");
        char *run_id = memory_arg_string_dup(args_doc, "run_id");
        char *idempotency_key = memory_arg_string_dup(args_doc, "idempotency_key");
        char *edge_path = memory_arg_string_dup(args_doc, "edge_manifest_path");
        char *edge_sha = memory_arg_string_dup(args_doc, "edge_manifest_sha256");
        char *concept_path = memory_arg_string_dup(args_doc, "concept_manifest_path");
        char *concept_sha = memory_arg_string_dup(args_doc, "concept_manifest_sha256");
        char *auth_path = memory_arg_string_dup(args_doc, "authorization_manifest_path");
        char *auth_sha = memory_arg_string_dup(args_doc, "authorization_manifest_sha256");
        yyjson_val *frozen_value = memory_arg(args_doc, "frozen_as_of_ms");
        yyjson_val *limit_value = memory_arg(args_doc, "limit");
        yyjson_val *budget_value = memory_arg(args_doc, "budget_seconds");
        int64_t frozen =
            frozen_value && yyjson_is_int(frozen_value) ? yyjson_get_sint(frozen_value) : 0;
        int64_t limit_raw =
            limit_value && yyjson_is_int(limit_value) ? yyjson_get_sint(limit_value) : 0;
        int64_t budget_raw =
            budget_value && yyjson_is_int(budget_value) ? yyjson_get_sint(budget_value) : 0;
        int limit = (limit_raw >= 1 && limit_raw <= 1000) ? (int)limit_raw : 0;
        int budget = (budget_raw >= 1 && budget_raw <= 30) ? (int)budget_raw : 0;
        int preview_action = !strcmp(action, "preview");
        int execution_action =
            !strcmp(action, "run") || !strcmp(action, "resume") || !strcmp(action, "pause");
        int base_complete = (preview_action || execution_action) && mode &&
                            !strcmp(mode, "bounded_canary") && project && project[0] && owner &&
                            owner[0] && run_id && run_id[0] && idempotency_key &&
                            idempotency_key[0] && frozen > 0 && limit_raw >= 1 &&
                            limit_raw <= 1000 && budget_raw >= 1 && budget_raw <= 30;
        int preview_shape = stage14_object_exact_keys(
            args_root, preview_fields, sizeof(preview_fields) / sizeof(preview_fields[0]));
        int execution_shape = stage14_object_exact_keys(
            args_root, execution_fields, sizeof(execution_fields) / sizeof(execution_fields[0]));
        if (preview_action && base_complete && preview_shape) {
            cbm_store_t *store = resolve_global_memory_store(srv, false);
            cbm_evolution_maintenance_input_t input = {.mode = "dry_run",
                                                       .project_uuid = project,
                                                       .owner_id = owner,
                                                       .idempotency_key = idempotency_key,
                                                       .run_id = run_id,
                                                       .frozen_as_of_ms = frozen,
                                                       .limit = limit,
                                                       .budget_seconds = budget,
                                                       .isolated_write_allowed = 0,
                                                       .production_gate_allowed = 0};
            controller_mode = "dry_run";
            authorization_code = "READ_ONLY_PREVIEW_NO_AUTH_REQUIRED";
            if (store)
                controller_rc = cbm_evolution_maintenance_store(store, &input, &result);
            if (controller_rc == CBM_STORE_OK) {
                status = "planned";
                code = "PRODUCTION_PREVIEW_READY";
            } else {
                code = "PRODUCTION_PREVIEW_FAILED";
            }
            production_state_written = 0;
        } else if (execution_action && base_complete && execution_shape &&
                   stage14_absolute_path(edge_path) && stage14_lower_sha256(edge_sha) &&
                   stage14_absolute_path(concept_path) && stage14_lower_sha256(concept_sha) &&
                   stage14_absolute_path(auth_path) && stage14_lower_sha256(auth_sha)) {
            authorization_ready = stage14_load_canary_authorization(
                auth_path, auth_sha, true, &authorization, &authorization_code);
            if (!authorization_ready) {
                code = authorization_code;
            } else if (!stage14_authorizes_maintenance(
                           &authorization, project, mode, run_id, idempotency_key, owner, frozen,
                           edge_path, edge_sha, concept_path, concept_sha, limit, budget)) {
                code = "CANARY_MAINTENANCE_ALLOWLIST_MISMATCH";
            } else if (!strcmp(action, "pause")) {
                code = "CORE_PAUSE_API_UNAVAILABLE";
            } else {
                cbm_store_t *store = resolve_global_memory_store(srv, false);
                sqlite3 *db = store ? cbm_store_get_db(store) : NULL;
                int run_state = manager_maintenance_run_state(db, run_id, idempotency_key);
                if (!strcmp(action, "resume") && run_state == 0) {
                    code = "MAINTENANCE_CHECKPOINT_NOT_FOUND";
                } else if (!strcmp(action, "run") && run_state == 2) {
                    code = "MAINTENANCE_RESUME_REQUIRED";
                } else if (run_state < 0) {
                    code = "IDEMPOTENCY_CONFLICT";
                } else {
                    cbm_evolution_maintenance_input_t input = {
                        .mode = "bounded_canary",
                        .project_uuid = project,
                        .owner_id = owner,
                        .idempotency_key = idempotency_key,
                        .run_id = run_id,
                        .edge_manifest_path = edge_path,
                        .edge_manifest_sha256 = edge_sha,
                        .concept_manifest_path = concept_path,
                        .concept_manifest_sha256 = concept_sha,
                        .frozen_as_of_ms = frozen,
                        .limit = limit,
                        .budget_seconds = budget,
                        .isolated_write_allowed = 0,
                        .production_gate_allowed = 1};
                    controller_mode = input.mode;
                    if (store)
                        controller_rc = cbm_evolution_maintenance_store(store, &input, &result);
                    if (controller_rc == CBM_STORE_OK) {
                        status = "applied";
                        code = "PRODUCTION_CANARY_APPLIED";
                    } else if (controller_rc == CBM_STORE_REPLAYED) {
                        status = "replayed";
                        code = "EXACT_REPLAY_ZERO_WRITE";
                    } else if (controller_rc == CBM_STORE_CHECKPOINTED) {
                        status = "checkpointed";
                        code = "MAINTENANCE_CHECKPOINTED";
                    } else if (controller_rc == CBM_STORE_IDEMPOTENCY_CONFLICT) {
                        status = "conflict";
                        code = "IDEMPOTENCY_CONFLICT";
                    } else if (controller_rc == CBM_STORE_REJECTED) {
                        code = "MAINTENANCE_LEASE_OR_GATE_REJECTED";
                    } else {
                        code = "MAINTENANCE_CONTROLLER_FAILED";
                    }
                    production_state_written = result.wrote != 0;
                }
            }
        } else if (mode && !strcmp(mode, "active")) {
            code = "ACTIVE_MODE_FORBIDDEN";
        }
        free(mode);
        free(project);
        free(owner);
        free(run_id);
        free(idempotency_key);
        free(edge_path);
        free(edge_sha);
        free(concept_path);
        free(concept_sha);
        free(auth_path);
        free(auth_sha);
    }

    yyjson_mut_val *item = NULL;
    yyjson_mut_doc *doc =
        manager_detail_doc("semantic-memory-manager-maintenance-control/v2", status, &item);
    yyjson_mut_obj_add_strcpy(doc, item, "action", action ? action : "");
    yyjson_mut_obj_add_strcpy(doc, item, "scope", scope ? scope : "");
    yyjson_mut_obj_add_str(doc, item, "status", status);
    yyjson_mut_obj_add_str(doc, item, "code", code);
    yyjson_mut_obj_add_bool(doc, item, "production_state_written", production_state_written != 0);
    yyjson_mut_obj_add_bool(doc, item, "isolated_mock", isolated != 0);
    yyjson_mut_obj_add_bool(doc, item, "authorization_ready", authorization_ready != 0);
    yyjson_mut_obj_add_str(doc, item, "authorization_status", authorization_code);
    yyjson_mut_obj_add_int(doc, item, "eligible", result.eligible);
    yyjson_mut_obj_add_int(doc, item, "evolution_events", result.evolution_events);
    yyjson_mut_obj_add_bool(doc, item, "checkpointed", result.checkpointed != 0);
    yyjson_mut_obj_add_str(doc, item, "controller_mode", controller_mode);
    yyjson_mut_obj_add_bool(doc, item, "hard_delete_allowed", false);
    yyjson_mut_obj_add_str(doc, item, "production_transaction_owner", "core-evolution-controller");
    if (result.report_json) {
        yyjson_doc *report_doc = yyjson_read(result.report_json, strlen(result.report_json), 0);
        yyjson_val *report_root = report_doc ? yyjson_doc_get_root(report_doc) : NULL;
        yyjson_mut_val *report_copy = report_root ? yyjson_val_mut_copy(doc, report_root) : NULL;
        if (report_copy)
            yyjson_mut_obj_add_val(doc, item, "controller_report", report_copy);
        yyjson_doc_free(report_doc);
    }
    cbm_evolution_result_free(&result);
    yyjson_doc_free(args_doc);
    free(action);
    free(scope);
    return manager_wrap(doc);
}
