/*
 * test_mcp.c — Tests for the MCP server module.
 *
 * Covers: JSON-RPC parsing, MCP protocol, tool dispatch, tool handlers.
 */
#include "../src/foundation/compat.h"
#include "../src/foundation/compat_fs.h" /* cbm_unlink / cbm_rmdir */
#include "../src/foundation/constants.h"
#include "../src/foundation/log.h"
#include "test_framework.h"
#include "test_helpers.h"
#include <cli/cli.h>
#include <mcp/index_supervisor.h> /* spawn-count hook — #845 in-process guard */
#include <mcp/mcp.h>
#include <memory/memory_orchestrator.h>
#include <memory/memory_store.h>
#include <pipeline/pipeline.h>
#include <store/store.h>
#include <watcher/watcher.h>
#include <yyjson/yyjson.h>
#include <sqlite3.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h> /* chmod / stat for read-only query reproductions */
#ifdef _WIN32
#include <direct.h>
#define cbm_chdir _chdir
#define cbm_getcwd _getcwd
#else
#include <sys/wait.h> /* waitpid — #845 fork+alarm harness */
#include <unistd.h>
#define cbm_chdir chdir
#define cbm_getcwd getcwd
#endif

static char mcp_log_buf[4096];

static void mcp_capture_log(const char *line) {
    snprintf(mcp_log_buf, sizeof(mcp_log_buf), "%s", line ? line : "");
}

static bool response_contains_json_fragment(const char *response, const char *fragment) {
    if (!response || !fragment) {
        return false;
    }
    if (strstr(response, fragment)) {
        return true;
    }

    char escaped[512];
    size_t out = 0;
    for (size_t i = 0; fragment[i] && out + 2 < sizeof(escaped); i++) {
        if (fragment[i] == '"') {
            escaped[out++] = '\\';
        }
        escaped[out++] = fragment[i];
    }
    escaped[out] = '\0';
    return strstr(response, escaped) != NULL;
}

static void restore_cache_dir(const char *saved_copy) {
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
}

static void cleanup_project_db(const char *cache, const char *project) {
    if (!cache || !project) {
        return;
    }

    char path[CBM_SZ_4K];
    snprintf(path, sizeof(path), "%s/%s.db", cache, project);
    cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/%s.db-wal", cache, project);
    cbm_unlink(path);
    snprintf(path, sizeof(path), "%s/%s.db-shm", cache, project);
    cbm_unlink(path);
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_request) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"capabilities\":{}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_EQ(req.id, 1);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_notification) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "notifications/initialized");
    ASSERT_FALSE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_invalid) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("not json", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_tools_call) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
                       "\"params\":{\"name\":\"search_graph\","
                       "\"arguments\":{\"label\":\"Function\",\"limit\":5}}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.method, "tools/call");
    ASSERT_EQ(req.id, 42);
    ASSERT_NOT_NULL(req.params_raw);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* issue #253: JSON-RPC 2.0 §4 permits string ids (Claude Desktop sends them
 * for "initialize"). Previously strtol-coerced to 0; must be preserved. */
TEST(jsonrpc_parse_string_id_issue253) {
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"init-abc\",\"method\":\"initialize\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "init-abc");
    cbm_jsonrpc_request_free(&req);

    /* A purely non-numeric string would have become 0 under strtol. */
    const char *line2 = "{\"jsonrpc\":\"2.0\",\"id\":\"xyz\",\"method\":\"ping\"}";
    cbm_jsonrpc_request_t req2 = {0};
    ASSERT_EQ(cbm_jsonrpc_parse(line2, &req2), 0);
    ASSERT_NOT_NULL(req2.id_str);
    ASSERT_STR_EQ(req2.id_str, "xyz");
    cbm_jsonrpc_request_free(&req2);
    PASS();
}

/* issue #253: the response must echo the string id verbatim, not as a number. */
TEST(jsonrpc_format_response_string_id_issue253) {
    cbm_jsonrpc_response_t resp = {
        .id_str = "init-abc",
        .result_json = "{\"ok\":true}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":\"init-abc\""));
    /* Must NOT have coerced to a numeric id. */
    ASSERT_NULL(strstr(json, "\"id\":0"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC FORMATTING
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_format_response) {
    cbm_jsonrpc_response_t resp = {
        .id = 1,
        .result_json = "{\"name\":\"semantic-memory-mcp\"}",
    };
    char *json = cbm_jsonrpc_format_response(&resp);
    ASSERT_NOT_NULL(json);
    /* Should contain jsonrpc, id, and result */
    ASSERT_NOT_NULL(strstr(json, "\"jsonrpc\":\"2.0\""));
    ASSERT_NOT_NULL(strstr(json, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(json, "\"result\""));
    free(json);
    PASS();
}

TEST(jsonrpc_format_error) {
    char *json = cbm_jsonrpc_format_error(5, -32600, "Invalid Request");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"id\":5"));
    ASSERT_NOT_NULL(strstr(json, "\"error\""));
    ASSERT_NOT_NULL(strstr(json, "-32600"));
    ASSERT_NOT_NULL(strstr(json, "Invalid Request"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  MCP PROTOCOL HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_initialize_response) {
    cbm_cli_set_version("9.8.7-test");

    /* Default (no params): returns latest supported version */
    char *json = cbm_mcp_initialize_response(NULL);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "semantic-memory-mcp"));
    ASSERT_NULL(strstr(json, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(json, "\"version\":\"9.8.7-test\""));
    ASSERT_NOT_NULL(strstr(json, "capabilities"));
    ASSERT_NOT_NULL(strstr(json, "tools"));
    ASSERT_NOT_NULL(strstr(json, "\"listChanged\":false"));
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);

    /* Client requests a supported version: server echoes it */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2024-11-05\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2024-11-05"));
    free(json);

    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"2025-06-18\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-06-18"));
    free(json);

    /* Client requests unknown version: server returns its latest */
    json = cbm_mcp_initialize_response("{\"protocolVersion\":\"9999-01-01\"}");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "2025-11-25"));
    free(json);
    cbm_cli_set_version("dev");
    PASS();
}

TEST(mcp_tools_list) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    /* Should contain all 14 tools */
    ASSERT_NOT_NULL(strstr(json, "index_repository"));
    ASSERT_NOT_NULL(strstr(json, "search_graph"));
    ASSERT_NOT_NULL(strstr(json, "query_graph"));
    ASSERT_NOT_NULL(strstr(json, "trace_path"));
    ASSERT_NOT_NULL(strstr(json, "get_code_snippet"));
    ASSERT_NOT_NULL(strstr(json, "get_graph_schema"));
    ASSERT_NOT_NULL(strstr(json, "get_architecture"));
    ASSERT_NOT_NULL(strstr(json, "search_code"));
    ASSERT_NOT_NULL(strstr(json, "list_projects"));
    ASSERT_NOT_NULL(strstr(json, "delete_project"));
    ASSERT_NOT_NULL(strstr(json, "index_status"));
    ASSERT_NOT_NULL(strstr(json, "detect_changes"));
    ASSERT_NOT_NULL(strstr(json, "manage_adr"));
    ASSERT_NOT_NULL(strstr(json, "ingest_traces"));
    free(json);
    PASS();
}

TEST(mcp_tools_list_latest_metadata) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"title\":\"Search graph\""));
    ASSERT_NOT_NULL(strstr(json, "\"title\":\"Index repository\""));
    ASSERT_NOT_NULL(strstr(json, "\"outputSchema\":{\"type\":\"object\""));
    ASSERT_NOT_NULL(strstr(json, "\"additionalProperties\":true"));
    free(json);
    PASS();
}

TEST(mcp_all_tools_expose_valid_required_schema_fields) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_TRUE(yyjson_is_obj(root));
    yyjson_val *tools = yyjson_obj_get(root, "tools");
    ASSERT_TRUE(yyjson_is_arr(tools));
    ASSERT_TRUE(yyjson_arr_size(tools) > 0);

    bool found_memory_task_complete = false;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(tools, &iter);
    yyjson_val *tool = NULL;
    while ((tool = yyjson_arr_iter_next(&iter)) != NULL) {
        ASSERT_TRUE(yyjson_is_obj(tool));
        yyjson_val *name = yyjson_obj_get(tool, "name");
        yyjson_val *input_schema = yyjson_obj_get(tool, "inputSchema");
        yyjson_val *output_schema = yyjson_obj_get(tool, "outputSchema");
        ASSERT_TRUE(yyjson_is_str(name));
        ASSERT_TRUE(yyjson_get_len(name) > 0);
        ASSERT_TRUE(yyjson_is_obj(input_schema));
        ASSERT_TRUE(yyjson_is_obj(output_schema));
        ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(input_schema, "type")), "object");

        if (strcmp(yyjson_get_str(name), "memory_task_complete") == 0) {
            found_memory_task_complete = true;
            yyjson_val *properties = yyjson_obj_get(input_schema, "properties");
            ASSERT_TRUE(yyjson_is_obj(properties));
            ASSERT_TRUE(yyjson_is_obj(yyjson_obj_get(properties, "legacy_fallback")));
        }
    }
    ASSERT_TRUE(found_memory_task_complete);

    yyjson_doc_free(doc);
    free(json);
    PASS();
}

TEST(mcp_index_repository_declares_name_override_issue571) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"index_repository\""));
    ASSERT_NOT_NULL(strstr(json, "\"name\":{\"type\":\"string\""));
    ASSERT_NOT_NULL(strstr(json, "Non-ASCII bytes are encoded"));
    free(json);
    PASS();
}

TEST(mcp_tools_array_schemas_have_items) {
    /* VS Code 1.112+ rejects array schemas without "items" (see
     * https://github.com/microsoft/vscode/issues/248810).
     * Walk every tool's inputSchema and verify that every "type":"array"
     * property also contains "items". */
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    /* Scan for all occurrences of "type":"array" — each must be followed
     * by "items" before the next closing brace of that property. */
    const char *p = json;
    while ((p = strstr(p, "\"type\":\"array\"")) != NULL) {
        /* Find the enclosing '}' for this property object */
        const char *end = strchr(p, '}');
        ASSERT_NOT_NULL(end);
        /* "items" must appear between p and end */
        size_t span = (size_t)(end - p);
        char *segment = malloc(span + 1);
        memcpy(segment, p, span);
        segment[span] = '\0';
        ASSERT_NOT_NULL(strstr(segment, "\"items\"")); /* array missing items */
        free(segment);
        p = end;
    }

    free(json);
    PASS();
}

TEST(mcp_stage8_reinforcement_schema_is_fixed) {
    const char *schema = cbm_mcp_tool_input_schema("memory_reinforcement_replay");
    ASSERT_NOT_NULL(schema);
    ASSERT_NOT_NULL(strstr(schema, "\"oneOf\""));
    ASSERT_NOT_NULL(strstr(schema, "\"mode\""));
    ASSERT_NOT_NULL(strstr(schema, "\"off\""));
    ASSERT_NOT_NULL(strstr(schema, "\"shadow\""));
    ASSERT_NOT_NULL(strstr(schema, "\"active\""));
    ASSERT_NOT_NULL(strstr(schema, "stage8-edge-reinforcement-v1"));
    ASSERT_NOT_NULL(strstr(schema, "\"const\":1"));
    ASSERT_NOT_NULL(strstr(schema, "\"preview\""));
    ASSERT_NOT_NULL(strstr(schema, "\"apply\""));
    ASSERT_NOT_NULL(strstr(schema, "\"bounded_canary\""));
    ASSERT_NOT_NULL(strstr(schema, "\"run_id\""));
    ASSERT_NOT_NULL(strstr(schema, "\"task_id\""));
    ASSERT_NOT_NULL(strstr(schema, "\"manifest_path\""));
    ASSERT_NOT_NULL(strstr(schema, "\"authorization_manifest_path\""));
    PASS();
}

TEST(mcp_stage9_edge_lifecycle_schemas_are_fixed) {
    const char *migrate = cbm_mcp_tool_input_schema("memory_edge_lifecycle_migrate");
    const char *maintain = cbm_mcp_tool_input_schema("memory_edge_maintenance");
    const char *restore = cbm_mcp_tool_input_schema("memory_edge_restore");
    const char *memory_delete = cbm_mcp_tool_input_schema("memory_delete");
    ASSERT_NOT_NULL(migrate);
    ASSERT_NOT_NULL(maintain);
    ASSERT_NOT_NULL(restore);
    ASSERT_NOT_NULL(memory_delete);
    ASSERT_NOT_NULL(strstr(migrate, "stage9-edge-lifecycle-v1"));
    ASSERT_NOT_NULL(strstr(migrate,
                           "8ba8ca0610997d4ffd6a0d2e5e98460a7ec3bf7b2fae2bdfaef9fd41d5802b0c"));
    ASSERT_NOT_NULL(strstr(maintain, "\"off\""));
    ASSERT_NOT_NULL(strstr(maintain, "\"shadow\""));
    ASSERT_NOT_NULL(strstr(maintain, "\"dry_run\""));
    ASSERT_NOT_NULL(strstr(maintain, "\"active\""));
    ASSERT_NOT_NULL(strstr(maintain, "\"as_of_ms\""));
    ASSERT_NOT_NULL(strstr(maintain, "\"manifest_sha256\""));
    ASSERT_NOT_NULL(strstr(restore, "\"edge_ids\""));
    ASSERT_NOT_NULL(strstr(restore, "\"uniqueItems\":true"));
    ASSERT_NOT_NULL(strstr(restore, "\"maxItems\":100"));
    ASSERT_NULL(strstr(memory_delete, "\"hard\""));
    ASSERT_NULL(strstr(memory_delete, "\"purge\""));
    PASS();
}

TEST(mcp_stage10_concept_schemas_are_fixed) {
    const char *generate = cbm_mcp_tool_input_schema("memory_concept_generate");
    const char *review = cbm_mcp_tool_input_schema("memory_concept_review");
    const char *inspect = cbm_mcp_tool_input_schema("memory_concept_inspect");
    ASSERT_NOT_NULL(generate);
    ASSERT_NOT_NULL(review);
    ASSERT_NOT_NULL(inspect);
    ASSERT_NOT_NULL(strstr(generate, "stage10-concept-growth-v1"));
    ASSERT_NOT_NULL(strstr(generate,
                           "38e32e2c0fbe224db8e6ca04fb1aff58b38cbc7255904b70563c3d48018746b4"));
    ASSERT_NOT_NULL(strstr(generate, "deterministic-local-v1"));
    ASSERT_NOT_NULL(strstr(generate, "\"migrate\""));
    ASSERT_NOT_NULL(strstr(generate, "\"off\""));
    ASSERT_NOT_NULL(strstr(generate, "\"shadow\""));
    ASSERT_NOT_NULL(strstr(generate, "\"dry_run\""));
    ASSERT_NOT_NULL(strstr(generate, "\"active\""));
    ASSERT_NOT_NULL(strstr(review, "explicit_user_confirmed"));
    ASSERT_NOT_NULL(strstr(review, "\"approve\""));
    ASSERT_NOT_NULL(strstr(review, "\"edit\""));
    ASSERT_NOT_NULL(strstr(review, "\"reject\""));
    ASSERT_NOT_NULL(strstr(review, "\"withdraw\""));
    ASSERT_NOT_NULL(strstr(inspect, "candidate_id"));
    const char *retrieve = cbm_mcp_tool_input_schema("memories_retrieve");
    ASSERT_NOT_NULL(retrieve);
    ASSERT_NOT_NULL(strstr(retrieve, "concept_mode"));
    ASSERT_NOT_NULL(strstr(retrieve, "\"enabled\""));
    PASS();
}

TEST(mcp_stage14_manager_schemas_are_fixed) {
    const char *names[] = {"manager_global_overview","manager_global_memory",
        "manager_global_topology","manager_evolution","manager_task_chain",
        "manager_drift_preview","manager_maintenance_preview","manager_maintenance_control"};
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);i++) ASSERT_NOT_NULL(cbm_mcp_tool_input_schema(names[i]));
    const char *control=cbm_mcp_tool_input_schema("manager_maintenance_control");
    ASSERT_NOT_NULL(strstr(control,"\"pause\""));
    ASSERT_NOT_NULL(strstr(control,"\"resume\""));
    ASSERT_NOT_NULL(strstr(control,"\"dry_run\""));
    ASSERT_NOT_NULL(strstr(control,"\"preview\""));
    ASSERT_NOT_NULL(strstr(control,"\"bounded_canary\""));
    ASSERT_NOT_NULL(strstr(control,"\"edge_manifest_path\""));
    ASSERT_NOT_NULL(strstr(control,"\"concept_manifest_path\""));
    ASSERT_NOT_NULL(strstr(control,"\"authorization_manifest_path\""));
    PASS();
}

TEST(mcp_stage14_production_controls_fail_closed_without_explicit_authorization) {
    const char *saved_gate = getenv("CBM_STAGE14_PRODUCTION_GATE");
    const char *saved_mode = getenv("CBM_STAGE14_EVOLUTION_MODE");
    const char *saved_auth_path = getenv("CBM_STAGE14_CANARY_AUTH_MANIFEST");
    const char *saved_auth_sha = getenv("CBM_STAGE14_CANARY_AUTH_SHA256");
    const char *saved_auto_maintain = getenv("CBM_MEMORY_AUTO_MAINTAIN");
    const char *saved_embed_backend = getenv("CBM_MEMORY_EMBED_BACKEND");
    const char *saved_no_union = getenv("CBM_MEMORY_NO_GLOBAL_UNION");
    char *gate_copy = saved_gate ? cbm_strdup(saved_gate) : NULL;
    char *mode_copy = saved_mode ? cbm_strdup(saved_mode) : NULL;
    char *auth_path_copy = saved_auth_path ? cbm_strdup(saved_auth_path) : NULL;
    char *auth_sha_copy = saved_auth_sha ? cbm_strdup(saved_auth_sha) : NULL;
    char *auto_maintain_copy =
        saved_auto_maintain ? cbm_strdup(saved_auto_maintain) : NULL;
    char *embed_backend_copy =
        saved_embed_backend ? cbm_strdup(saved_embed_backend) : NULL;
    char *no_union_copy = saved_no_union ? cbm_strdup(saved_no_union) : NULL;

    cbm_setenv("CBM_STAGE14_PRODUCTION_GATE", "1", 1);
    cbm_setenv("CBM_STAGE14_EVOLUTION_MODE", "active", 1);
    cbm_setenv("CBM_MEMORY_AUTO_MAINTAIN", "0", 1);
    cbm_setenv("CBM_MEMORY_EMBED_BACKEND", "static", 1);
    cbm_unsetenv("CBM_MEMORY_NO_GLOBAL_UNION");
    cbm_unsetenv("CBM_STAGE14_CANARY_AUTH_MANIFEST");
    cbm_unsetenv("CBM_STAGE14_CANARY_AUTH_SHA256");
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    const char *active_task =
        "{\"action\":\"apply\",\"mode\":\"active\",\"project\":\"project-v1\","
        "\"run_id\":\"run-v1\",\"task_id\":\"task-v1\","
        "\"idempotency_key\":\"task-key-v1\",\"max_evolution_events\":17,"
        "\"max_cross_project_edges\":16,\"manifest_path\":\"C:\\\\stage14\\\\task.json\","
        "\"manifest_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"authorization_manifest_path\":\"C:\\\\stage14\\\\auth.json\","
        "\"authorization_manifest_sha256\":"
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}";
    char *active = handle_memory_reinforcement_replay(srv, active_task);
    ASSERT_NOT_NULL(active);
    ASSERT_TRUE(response_contains_json_fragment(
        active, "\"code\":\"ACTIVE_MODE_FORBIDDEN\""));
    ASSERT_TRUE(response_contains_json_fragment(
        active, "\"production_state_written\":false"));
    free(active);

    const char *overflow_task =
        "{\"action\":\"preview\",\"mode\":\"bounded_canary\","
        "\"project\":\"project-v1\",\"run_id\":\"run-v1\","
        "\"task_id\":\"task-v1\",\"idempotency_key\":\"task-key-v1\","
        "\"max_evolution_events\":4294967297,\"max_cross_project_edges\":16}";
    char *overflow = handle_memory_reinforcement_replay(srv, overflow_task);
    ASSERT_NOT_NULL(overflow);
    ASSERT_TRUE(response_contains_json_fragment(
        overflow, "\"code\":\"INVALID_STAGE14_TASK_BINDING\""));
    ASSERT_TRUE(response_contains_json_fragment(
        overflow, "\"production_state_written\":false"));
    free(overflow);

    const char *zero_cap_task =
        "{\"action\":\"preview\",\"mode\":\"bounded_canary\","
        "\"project\":\"project-v1\",\"run_id\":\"run-v1\","
        "\"task_id\":\"task-v1\",\"idempotency_key\":\"task-key-v1\","
        "\"max_evolution_events\":0,\"max_cross_project_edges\":0}";
    char *zero_cap = handle_memory_reinforcement_replay(srv, zero_cap_task);
    ASSERT_NOT_NULL(zero_cap);
    ASSERT_TRUE(response_contains_json_fragment(
        zero_cap, "\"code\":\"INVALID_STAGE14_TASK_BINDING\""));
    ASSERT_TRUE(response_contains_json_fragment(
        zero_cap, "\"production_state_written\":false"));
    free(zero_cap);

    cbm_setenv("CBM_STAGE14_EVOLUTION_MODE", "bounded_canary", 1);
    const char *maintenance =
        "{\"action\":\"run\",\"scope\":\"production\",\"mode\":\"bounded_canary\","
        "\"project\":\"project-v1\",\"owner_id\":\"owner-v1\","
        "\"run_id\":\"maintenance-v1\",\"idempotency_key\":\"maintenance-key-v1\","
        "\"frozen_as_of_ms\":1,\"limit\":1,\"budget_seconds\":1,"
        "\"edge_manifest_path\":\"C:\\\\stage14\\\\edge.json\","
        "\"edge_manifest_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"concept_manifest_path\":\"C:\\\\stage14\\\\concept.json\","
        "\"concept_manifest_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"authorization_manifest_path\":\"C:\\\\stage14\\\\auth.json\","
        "\"authorization_manifest_sha256\":"
        "\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}";
    cbm_setenv("CBM_MEMORY_NO_GLOBAL_UNION", "1", 1);
    char *runtime_drift = handle_manager_maintenance_control(srv, maintenance);
    ASSERT_NOT_NULL(runtime_drift);
    ASSERT_TRUE(response_contains_json_fragment(
        runtime_drift, "\"code\":\"CANARY_RUNTIME_ENV_DRIFT\""));
    ASSERT_TRUE(response_contains_json_fragment(
        runtime_drift, "\"production_state_written\":false"));
    free(runtime_drift);
    cbm_unsetenv("CBM_MEMORY_NO_GLOBAL_UNION");
    char *blocked = handle_manager_maintenance_control(srv, maintenance);
    ASSERT_NOT_NULL(blocked);
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"code\":\"CANARY_AUTH_ENV_DRIFT\""));
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"production_state_written\":false"));
    free(blocked);

    const char *extra_field =
        "{\"action\":\"preview\",\"scope\":\"production\","
        "\"mode\":\"bounded_canary\",\"project\":\"project-v1\","
        "\"owner_id\":\"owner-v1\",\"run_id\":\"maintenance-v1\","
        "\"idempotency_key\":\"maintenance-key-v1\",\"frozen_as_of_ms\":1,"
        "\"limit\":1,\"budget_seconds\":1,\"unexpected\":true}";
    char *extra = handle_manager_maintenance_control(srv, extra_field);
    ASSERT_NOT_NULL(extra);
    ASSERT_TRUE(response_contains_json_fragment(
        extra, "\"code\":\"INVALID_ARGUMENT\""));
    ASSERT_TRUE(response_contains_json_fragment(
        extra, "\"production_state_written\":false"));
    free(extra);
    cbm_mcp_server_free(srv);

    if (gate_copy) cbm_setenv("CBM_STAGE14_PRODUCTION_GATE", gate_copy, 1);
    else cbm_unsetenv("CBM_STAGE14_PRODUCTION_GATE");
    if (mode_copy) cbm_setenv("CBM_STAGE14_EVOLUTION_MODE", mode_copy, 1);
    else cbm_unsetenv("CBM_STAGE14_EVOLUTION_MODE");
    if (auth_path_copy)
        cbm_setenv("CBM_STAGE14_CANARY_AUTH_MANIFEST", auth_path_copy, 1);
    else cbm_unsetenv("CBM_STAGE14_CANARY_AUTH_MANIFEST");
    if (auth_sha_copy)
        cbm_setenv("CBM_STAGE14_CANARY_AUTH_SHA256", auth_sha_copy, 1);
    else cbm_unsetenv("CBM_STAGE14_CANARY_AUTH_SHA256");
    if (auto_maintain_copy)
        cbm_setenv("CBM_MEMORY_AUTO_MAINTAIN", auto_maintain_copy, 1);
    else cbm_unsetenv("CBM_MEMORY_AUTO_MAINTAIN");
    if (embed_backend_copy)
        cbm_setenv("CBM_MEMORY_EMBED_BACKEND", embed_backend_copy, 1);
    else cbm_unsetenv("CBM_MEMORY_EMBED_BACKEND");
    if (no_union_copy)
        cbm_setenv("CBM_MEMORY_NO_GLOBAL_UNION", no_union_copy, 1);
    else cbm_unsetenv("CBM_MEMORY_NO_GLOBAL_UNION");
    free(no_union_copy);
    free(embed_backend_copy);
    free(auto_maintain_copy);
    free(auth_sha_copy);
    free(auth_path_copy);
    free(mode_copy);
    free(gate_copy);
    PASS();
}

TEST(mcp_stage14_manager_response_shapes_are_fixed) {
    cbm_mcp_server_t *srv=cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *detail=handle_manager_global_overview(srv,"{}");
    ASSERT_NOT_NULL(detail);ASSERT_NOT_NULL(strstr(detail,"semantic-memory-manager-global-overview/v1"));ASSERT_TRUE(response_contains_json_fragment(detail,"\"project_count\":"));ASSERT_TRUE(response_contains_json_fragment(detail,"\"projects\":["));free(detail);
    char *list=handle_manager_global_memory(srv,"{\"limit\":10}");
    ASSERT_NOT_NULL(list);ASSERT_NOT_NULL(strstr(list,"\"items\""));ASSERT_NOT_NULL(strstr(list,"\"next_cursor\""));ASSERT_NOT_NULL(strstr(list,"\"total\""));free(list);
    char *drift=handle_manager_drift_preview(srv,"{}");
    ASSERT_NOT_NULL(drift);ASSERT_TRUE(response_contains_json_fragment(drift,"\"blocking\":"));ASSERT_TRUE(response_contains_json_fragment(drift,"\"repair_preview\":{"));free(drift);
    char *maintenance=handle_manager_maintenance_preview(srv,"{}");
    ASSERT_NOT_NULL(maintenance);ASSERT_TRUE(response_contains_json_fragment(maintenance,"\"checkpoint\":"));ASSERT_TRUE(response_contains_json_fragment(maintenance,"\"history\":["));free(maintenance);
    char *control=handle_manager_maintenance_control(srv,"{\"action\":\"dry_run\",\"scope\":\"isolated_mock\"}");
    ASSERT_NOT_NULL(control);ASSERT_TRUE(response_contains_json_fragment(control,"\"status\":\"rejected\""));ASSERT_TRUE(response_contains_json_fragment(control,"\"production_state_written\":false"));free(control);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(mcp_stage10_concept_recall_and_review_require_guards) {
    cbm_unsetenv("CBM_STAGE10_CONCEPT_RECALL");
    cbm_unsetenv("CBM_STAGE10_REVIEW_EXPLICIT_USER");
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage10-guard-XXXXXX");
    if (!cbm_mkdtemp(cache)) FAIL("could not create Stage 10 guard fixture cache");
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *recall = cbm_mcp_handle_tool(
        srv, "memories_retrieve",
        "{\"project\":\"stage10-fixture-guard\",\"concept_mode\":\"enabled\"}");
    ASSERT_NOT_NULL(recall);
    ASSERT_NOT_NULL(strstr(recall, "CONCEPT_RECALL_GUARD_MISSING"));
    free(recall);
    char *review = cbm_mcp_handle_tool(
        srv, "memory_concept_review",
        "{\"project\":\"stage10-fixture-guard\",\"store\":\"project-memory\","
        "\"candidate_id\":\"candidate-test\",\"action\":\"approve\","
        "\"idempotency_key\":\"review-test\",\"explicit_user_confirmed\":true}");
    ASSERT_NOT_NULL(review);
    ASSERT_NOT_NULL(strstr(review, "REVIEW_REQUIRES_EXPLICIT_USER"));
    free(review);
    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(mcp_stage9_hard_delete_modes_fail_closed) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage9-delete-guard-XXXXXX");
    if (!cbm_mkdtemp(cache)) FAIL("could not create Stage 9 delete guard fixture cache");
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    const char *modes[] = {"hard", "purge", "physical"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"project\":\"stage9-delete-guard\",\"id\":\"edge-or-node\","
                 "\"mode\":\"%s\"}", modes[i]);
        char *response = cbm_mcp_handle_tool(srv, "memory_delete", args);
        ASSERT_NOT_NULL(response);
        ASSERT_TRUE(response_contains_json_fragment(response,
                                                     "\"code\":\"HARD_DELETE_DISABLED\""));
        ASSERT_TRUE(response_contains_json_fragment(response,
                                                     "\"production_state_written\":false"));
        free(response);
    }
    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(mcp_events_schema_exposes_optional_derived_from) {
    const char *schema = cbm_mcp_tool_input_schema("events");
    ASSERT_NOT_NULL(schema);
    ASSERT_NOT_NULL(strstr(schema, "\"derived_from_memory_id\""));
    ASSERT_NOT_NULL(strstr(schema, "new memory item"));
    ASSERT_NOT_NULL(strstr(schema, "existing active or candidate memory"));
    PASS();
}

TEST(mcp_ingest_traces_items_disallow_additional_properties_issue731) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_NOT_NULL(root);
    yyjson_val *tools = yyjson_obj_get(root, "tools");
    ASSERT_NOT_NULL(tools);
    ASSERT_TRUE(yyjson_is_arr(tools));

    yyjson_val *tool;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(tools, &iter);
    yyjson_val *ingest_traces = NULL;
    while ((tool = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *name = yyjson_obj_get(tool, "name");
        if (name && yyjson_is_str(name) && strcmp(yyjson_get_str(name), "ingest_traces") == 0) {
            ingest_traces = tool;
            break;
        }
    }
    ASSERT_NOT_NULL(ingest_traces);

    yyjson_val *input_schema = yyjson_obj_get(ingest_traces, "inputSchema");
    ASSERT_NOT_NULL(input_schema);
    yyjson_val *properties = yyjson_obj_get(input_schema, "properties");
    ASSERT_NOT_NULL(properties);
    yyjson_val *traces = yyjson_obj_get(properties, "traces");
    ASSERT_NOT_NULL(traces);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(traces, "type")), "array");
    yyjson_val *items = yyjson_obj_get(traces, "items");
    ASSERT_NOT_NULL(items);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(items, "type")), "object");
    yyjson_val *item_properties = yyjson_obj_get(items, "properties");
    ASSERT_NOT_NULL(item_properties);
    yyjson_val *caller = yyjson_obj_get(item_properties, "caller");
    ASSERT_NOT_NULL(caller);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(caller, "type")), "string");
    yyjson_val *callee = yyjson_obj_get(item_properties, "callee");
    ASSERT_NOT_NULL(callee);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(callee, "type")), "string");
    yyjson_val *count = yyjson_obj_get(item_properties, "count");
    ASSERT_NOT_NULL(count);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(count, "type")), "integer");
    yyjson_val *additional_properties = yyjson_obj_get(items, "additionalProperties");
    ASSERT_NOT_NULL(additional_properties);
    ASSERT_TRUE(yyjson_is_bool(additional_properties));
    ASSERT_FALSE(yyjson_get_bool(additional_properties));

    yyjson_doc_free(doc);
    free(json);
    PASS();
}

/* Guard for PR #560 (schema enum): the get_architecture aspects items schema
 * must carry an enum of the valid tokens — including the new "overview" —
 * mirroring VALID_ASPECTS in mcp.c. Parsed structurally like
 * mcp_ingest_traces_items_disallow_additional_properties_issue731. */
TEST(mcp_get_architecture_aspects_schema_enum_pr560) {
    char *json = cbm_mcp_tools_list();
    ASSERT_NOT_NULL(json);

    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    ASSERT_NOT_NULL(doc);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_NOT_NULL(root);
    yyjson_val *tools = yyjson_obj_get(root, "tools");
    ASSERT_NOT_NULL(tools);
    ASSERT_TRUE(yyjson_is_arr(tools));

    yyjson_val *tool;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(tools, &iter);
    yyjson_val *get_arch = NULL;
    while ((tool = yyjson_arr_iter_next(&iter)) != NULL) {
        yyjson_val *name = yyjson_obj_get(tool, "name");
        if (name && yyjson_is_str(name) && strcmp(yyjson_get_str(name), "get_architecture") == 0) {
            get_arch = tool;
            break;
        }
    }
    ASSERT_NOT_NULL(get_arch);

    yyjson_val *input_schema = yyjson_obj_get(get_arch, "inputSchema");
    ASSERT_NOT_NULL(input_schema);
    yyjson_val *properties = yyjson_obj_get(input_schema, "properties");
    ASSERT_NOT_NULL(properties);
    yyjson_val *aspects = yyjson_obj_get(properties, "aspects");
    ASSERT_NOT_NULL(aspects);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(aspects, "type")), "array");
    yyjson_val *items = yyjson_obj_get(aspects, "items");
    ASSERT_NOT_NULL(items);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(items, "type")), "string");
    yyjson_val *enum_arr = yyjson_obj_get(items, "enum");
    ASSERT_NOT_NULL(enum_arr);
    ASSERT_TRUE(yyjson_is_arr(enum_arr));

    /* The enum must be exactly the valid-token set — no more, no less. */
    static const char *expected[] = {"all",      "overview",   "structure", "dependencies",
                                     "routes",   "languages",  "packages",  "entry_points",
                                     "hotspots", "boundaries", "layers",    "file_tree",
                                     "clusters"};
    size_t expected_count = sizeof(expected) / sizeof(expected[0]);
    ASSERT_EQ(yyjson_arr_size(enum_arr), expected_count);
    for (size_t i = 0; i < expected_count; i++) {
        bool found = false;
        yyjson_val *ev;
        yyjson_arr_iter eiter;
        yyjson_arr_iter_init(enum_arr, &eiter);
        while ((ev = yyjson_arr_iter_next(&eiter)) != NULL) {
            if (yyjson_is_str(ev) && strcmp(yyjson_get_str(ev), expected[i]) == 0) {
                found = true;
                break;
            }
        }
        ASSERT_TRUE(found);
    }

    yyjson_doc_free(doc);
    free(json);
    PASS();
}

TEST(mcp_text_result) {
    char *json = cbm_mcp_text_result("{\"total\":5}", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"type\":\"text\""));
    /* The text value is JSON-escaped inside the "text" field */
    ASSERT_NOT_NULL(strstr(json, "total"));
    ASSERT_NOT_NULL(strstr(json, "\"structuredContent\":{\"total\":5}"));
    ASSERT_NOT_NULL(strstr(json, "\"isError\":false"));
    ASSERT_NULL(strstr(json, "\"isError\":true"));
    free(json);
    PASS();
}

TEST(mcp_text_result_skips_structured_content_for_plain_text) {
    char *json = cbm_mcp_text_result("plain text", false);
    ASSERT_NOT_NULL(json);
    ASSERT_NULL(strstr(json, "\"structuredContent\""));
    ASSERT_NOT_NULL(strstr(json, "\"isError\":false"));
    free(json);
    PASS();
}

TEST(mcp_cancel_matches_request_id) {
    ASSERT_TRUE(cbm_mcp_cancel_request_matches("{\"requestId\":7}", 7, NULL));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{\"requestId\":8}", 7, NULL));
    ASSERT_TRUE(cbm_mcp_cancel_request_matches("{\"requestId\":\"call-1\"}", -1, "call-1"));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{\"requestId\":\"call-2\"}", -1, "call-1"));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{\"requestId\":7}", -1, "7"));
    ASSERT_FALSE(cbm_mcp_cancel_request_matches("{}", 7, NULL));
    PASS();
}

TEST(mcp_text_result_error) {
    char *json = cbm_mcp_text_result("something failed", true);
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(json, "something failed"));
    free(json);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_tool_name) {
    const char *params = "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\"}}";
    char *name = cbm_mcp_get_tool_name(params);
    ASSERT_NOT_NULL(name);
    ASSERT_STR_EQ(name, "search_graph");
    free(name);
    PASS();
}

TEST(mcp_get_arguments) {
    const char *params =
        "{\"name\":\"search_graph\",\"arguments\":{\"label\":\"Function\",\"limit\":5}}";
    char *args = cbm_mcp_get_arguments(params);
    ASSERT_NOT_NULL(args);
    ASSERT_NOT_NULL(strstr(args, "\"label\":\"Function\""));
    ASSERT_NOT_NULL(strstr(args, "\"limit\":5"));
    free(args);
    PASS();
}

TEST(mcp_get_string_arg) {
    const char *args = "{\"label\":\"Function\",\"name_pattern\":\".*Order.*\"}";
    char *val = cbm_mcp_get_string_arg(args, "label");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "Function");
    free(val);

    val = cbm_mcp_get_string_arg(args, "name_pattern");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, ".*Order.*");
    free(val);

    val = cbm_mcp_get_string_arg(args, "nonexistent");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg) {
    const char *args = "{\"limit\":10,\"offset\":5}";
    int val = cbm_mcp_get_int_arg(args, "limit", 0);
    ASSERT_EQ(val, 10);
    val = cbm_mcp_get_int_arg(args, "offset", 0);
    ASSERT_EQ(val, 5);
    val = cbm_mcp_get_int_arg(args, "missing", 42);
    ASSERT_EQ(val, 42);
    PASS();
}

TEST(mcp_get_bool_arg) {
    const char *args = "{\"include_connected\":true,\"regex\":false}";
    bool val = cbm_mcp_get_bool_arg(args, "include_connected");
    ASSERT_TRUE(val);
    val = cbm_mcp_get_bool_arg(args, "regex");
    ASSERT_FALSE(val);
    val = cbm_mcp_get_bool_arg(args, "missing");
    ASSERT_FALSE(val);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — PROTOCOL FLOW
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_initialize) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                                   "\"params\":{\"capabilities\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(resp, "semantic-memory-mcp"));
    ASSERT_NULL(strstr(resp, "codebase-memory-mcp"));
    ASSERT_NOT_NULL(strstr(resp, "capabilities"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_initialized_notification) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Notification has no id → no response */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    ASSERT_NULL(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    ASSERT_NOT_NULL(strstr(resp, "query_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list_null_cursor_is_first_page) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* RMCP serializes the initial optional cursor as JSON null. It must be
     * equivalent to an omitted cursor, not an invalid terminal cursor. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\","
             "\"params\":{\"cursor\":null}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":3"));
    ASSERT_NOT_NULL(strstr(resp, "\"nextCursor\":\"15\""));
    ASSERT_NOT_NULL(strstr(resp, "memories_retrieve"));
    ASSERT_NOT_NULL(strstr(resp, "memory_task_status"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list_meta_only_is_first_page) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Codex app-server sends startup metadata without a cursor on its first
     * tools/list request. Unrelated _meta must not change pagination. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/list\","
             "\"params\":{\"_meta\":{\"progressToken\":\"appserver-startup\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":4"));
    ASSERT_NOT_NULL(strstr(resp, "\"nextCursor\":\"15\""));
    ASSERT_NOT_NULL(strstr(resp, "memory_task_complete"));
    ASSERT_NOT_NULL(strstr(resp, "memory_task_status"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list_paginates) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":200,\"method\":\"tools/list\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":200"));
    ASSERT_NOT_NULL(strstr(resp, "\"nextCursor\":\"15\""));
    ASSERT_NOT_NULL(strstr(resp, "index_repository"));
    ASSERT_NOT_NULL(strstr(resp, "events"));
    ASSERT_NULL(strstr(resp, "manage_adr"));
    free(resp);

    /* Walk the remaining pages via nextCursor until exhausted. The total tool
     * count changes as tools are added, so the test is page-count-agnostic:
     * manage_adr must appear on SOME later page, and the cursor chain must
     * terminate (no nextCursor on the last page). */
    bool found_manage_adr = false;
    int cursor = 15;
    for (int page = 0; page < 16; page++) {
        char req[160];
        snprintf(req, sizeof(req),
                 "{\"jsonrpc\":\"2.0\",\"id\":201,\"method\":\"tools/list\","
                 "\"params\":{\"cursor\":\"%d\"}}",
                 cursor);
        resp = cbm_mcp_server_handle(srv, req);
        ASSERT_NOT_NULL(resp);
        ASSERT_NOT_NULL(strstr(resp, "\"id\":201"));
        if (strstr(resp, "manage_adr")) {
            found_manage_adr = true;
        }
        const char *nc = strstr(resp, "\"nextCursor\":\"");
        if (!nc) {
            free(resp);
            break;
        }
        cursor = atoi(nc + strlen("\"nextCursor\":\""));
        free(resp);
    }
    ASSERT_TRUE(found_manage_adr);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_list_first_page_memory_compat_inventory) {
    static const char *expected_tools[] = {
        "index_repository", "search_graph",      "query_graph",       "trace_path",
        "get_code_snippet", "get_graph_schema",  "get_architecture",  "search_code",
        "list_projects",    "delete_project",    "index_status",      "detect_changes",
        "manage_adr",       "ingest_traces",     "adr_list",          "adr_chain",
        "events",           "memories_retrieve", "memories_inspect",  "memory_update_status",
        "memory_feedback",  "memory_reinforcement_replay", "memory_edge_lifecycle_migrate",
        "memory_edge_maintenance", "memory_edge_restore", "memory_observe_injection",
        "memory_observe_usage", "memory_concept_generate", "memory_concept_review",
        "memory_concept_inspect", "memory_delete",
        "admin_consolidate", "admin_decay",       "memory_health",     "describe_tool",
        "memory_security_check", "memory_task_begin", "memory_task_status",
        "memory_task_complete", "memory_task_migrate",
        "manager_global_overview", "manager_global_memory", "manager_global_topology",
        "manager_evolution", "manager_task_chain", "manager_drift_preview",
        "manager_maintenance_preview", "manager_maintenance_control",
    };
    const int expected_count = (int)(sizeof(expected_tools) / sizeof(expected_tools[0]));
    bool seen[sizeof(expected_tools) / sizeof(expected_tools[0])] = {false};
    bool first_page_has_index = false;
    bool first_page_has_query = false;
    bool first_page_has_search = false;
    bool first_page_has_retrieve = false;
    bool first_page_has_task_status = false;
    bool first_page_has_task_complete = false;
    bool first_page_has_events = false;
    bool first_page_has_feedback = false;
    bool first_page_has_reinforcement = false;
    bool first_page_has_lifecycle_migrate = false;
    bool first_page_has_maintenance = false;
    bool first_page_has_restore = false;
    bool first_page_has_injection = false;
    bool first_page_has_usage = false;
    bool first_page_has_task_begin = false;
    int first_page_count = 0;
    int total_seen = 0;
    int cursor = 0;
    bool first_page = true;
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    for (int page = 0; page < 8; page++) {
        char request[256];
        if (first_page) {
            snprintf(request, sizeof(request),
                     "{\"jsonrpc\":\"2.0\",\"id\":220,\"method\":\"tools/list\"}");
        } else {
            snprintf(request, sizeof(request),
                     "{\"jsonrpc\":\"2.0\",\"id\":220,\"method\":\"tools/list\","
                     "\"params\":{\"cursor\":\"%d\"}}",
                     cursor);
        }

        char *response = cbm_mcp_server_handle(srv, request);
        ASSERT_NOT_NULL(response);
        yyjson_doc *doc = yyjson_read(response, strlen(response), 0);
        ASSERT_NOT_NULL(doc);
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *result = root ? yyjson_obj_get(root, "result") : NULL;
        yyjson_val *tools = result ? yyjson_obj_get(result, "tools") : NULL;
        ASSERT_NOT_NULL(result);
        ASSERT_NOT_NULL(tools);
        ASSERT_TRUE(yyjson_is_arr(tools));
        ASSERT_GT(yyjson_arr_size(tools), 0);

        size_t index, max;
        yyjson_val *tool;
        yyjson_arr_foreach(tools, index, max, tool) {
            if (first_page) first_page_count++;
            yyjson_val *name_value = yyjson_obj_get(tool, "name");
            ASSERT_NOT_NULL(name_value);
            ASSERT_TRUE(yyjson_is_str(name_value));
            const char *name = yyjson_get_str(name_value);
            int match = -1;
            for (int i = 0; i < expected_count; i++) {
                if (strcmp(name, expected_tools[i]) == 0) {
                    match = i;
                    break;
                }
            }
            ASSERT_GTE(match, 0);
            ASSERT_FALSE(seen[match]);
            seen[match] = true;
            total_seen++;
            if (first_page && strcmp(name, "memories_retrieve") == 0) {
                first_page_has_retrieve = true;
            }
            if (first_page && strcmp(name, "events") == 0) {
                first_page_has_events = true;
            }
            if (first_page && strcmp(name, "memory_task_status") == 0) {
                first_page_has_task_status = true;
            }
            if (first_page && strcmp(name, "memory_task_complete") == 0) {
                first_page_has_task_complete = true;
            }
            if (first_page && strcmp(name, "memory_feedback") == 0) {
                first_page_has_feedback = true;
            }
            if (first_page && strcmp(name, "memory_reinforcement_replay") == 0) {
                first_page_has_reinforcement = true;
            }
            if (first_page && strcmp(name, "memory_edge_lifecycle_migrate") == 0) {
                first_page_has_lifecycle_migrate = true;
            }
            if (first_page && strcmp(name, "memory_edge_maintenance") == 0) {
                first_page_has_maintenance = true;
            }
            if (first_page && strcmp(name, "memory_edge_restore") == 0) {
                first_page_has_restore = true;
            }
            if (first_page && strcmp(name, "memory_observe_injection") == 0) {
                first_page_has_injection = true;
            }
            if (first_page && strcmp(name, "memory_observe_usage") == 0) {
                first_page_has_usage = true;
            }
            if (first_page && strcmp(name, "index_repository") == 0) {
                first_page_has_index = true;
            }
            if (first_page && strcmp(name, "query_graph") == 0) {
                first_page_has_query = true;
            }
            if (first_page && strcmp(name, "search_graph") == 0) {
                first_page_has_search = true;
            }
            if (first_page && strcmp(name, "memory_task_begin") == 0) {
                first_page_has_task_begin = true;
            }
        }

        yyjson_val *next_cursor = result ? yyjson_obj_get(result, "nextCursor") : NULL;
        bool has_next = next_cursor != NULL;
        int next = cursor;
        if (has_next) {
            ASSERT_TRUE(yyjson_is_str(next_cursor));
            next = atoi(yyjson_get_str(next_cursor));
            ASSERT_GT(next, cursor);
        }
        yyjson_doc_free(doc);
        free(response);

        if (!has_next) {
            break;
        }
        cursor = next;
        first_page = false;
    }

    ASSERT_TRUE(first_page_has_retrieve);
    ASSERT_TRUE(first_page_has_task_status);
    ASSERT_TRUE(first_page_has_task_complete);
    ASSERT_TRUE(first_page_has_events);
    ASSERT_TRUE(first_page_has_feedback);
    ASSERT_TRUE(first_page_has_reinforcement);
    ASSERT_TRUE(first_page_has_lifecycle_migrate);
    ASSERT_TRUE(first_page_has_maintenance);
    ASSERT_TRUE(first_page_has_restore);
    ASSERT_TRUE(first_page_has_injection);
    ASSERT_TRUE(first_page_has_usage);
    ASSERT_TRUE(first_page_has_index);
    ASSERT_TRUE(first_page_has_query);
    ASSERT_TRUE(first_page_has_search);
    ASSERT_TRUE(first_page_has_task_begin);
    ASSERT_EQ(first_page_count, 15);
    ASSERT_EQ(total_seen, expected_count);
    for (int i = 0; i < expected_count; i++) {
        ASSERT_TRUE(seen[i]);
    }

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_logs_request_without_params) {
    mcp_log_buf[0] = '\0';
    CBMLogLevel prev_level = cbm_log_get_level();
    cbm_log_set_level(CBM_LOG_DEBUG);
    cbm_log_set_format(CBM_LOG_FORMAT_TEXT);
    cbm_log_set_sink_ex(mcp_capture_log, CBM_LOG_SINK_REPLACE);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":210,\"method\":\"tools/list\","
                                   "\"params\":{\"token\":\"secret\"}}");
    ASSERT_NOT_NULL(resp);
    free(resp);
    cbm_mcp_server_free(srv);

    cbm_log_set_sink(NULL);
    cbm_log_set_level(prev_level);

    ASSERT_NOT_NULL(strstr(mcp_log_buf, "msg=mcp.request"));
    ASSERT_NOT_NULL(strstr(mcp_log_buf, "protocol=jsonrpc"));
    ASSERT_NOT_NULL(strstr(mcp_log_buf, "method=tools/list"));
    ASSERT_NOT_NULL(strstr(mcp_log_buf, "status=ok"));
    ASSERT_NULL(strstr(mcp_log_buf, "token"));
    ASSERT_NULL(strstr(mcp_log_buf, "secret"));
    PASS();
}

TEST(server_handle_unknown_method) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"unknown/method\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32601")); /* Method not found */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS (via server_handle)
 * ══════════════════════════════════════════════════════════════════ */

/* Helper: create a server with an in-memory store populated with test data */
static cbm_mcp_server_t *setup_mcp_with_data(void) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL); /* NULL = in-memory */
    return srv;
}

TEST(tool_list_projects_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"id\":10"));
    /* Should return a result (possibly empty list) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_graph_schema_empty) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_graph_schema\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_unknown_tool) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"nonexistent_tool\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return result with isError */
    ASSERT_NOT_NULL(strstr(resp, "isError"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    /* search_graph with no project → should work on empty store */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"label\":\"Function\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Forward declarations for helpers defined later in this file */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz);
static void cleanup_snippet_dir(const char *tmp_dir);
static char *extract_text_content(const char *mcp_result);

TEST(tool_search_graph_includes_node_properties) {
    /* search_graph results must surface each node's properties_json
     * payload so callers don't have to round-trip through get_code_snippet
     * just to read them. The setup_snippet_server inserts HandleRequest
     * with a signature/return_type/is_exported property blob; this test
     * pins that those keys reach the MCP response. */
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project\":\"test-project\",\"label\":\"Function\","
             "\"name_pattern\":\"HandleRequest\",\"limit\":5}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    /* Properties from HandleRequest's properties_json must appear. */
    ASSERT_NOT_NULL(strstr(inner, "signature"));
    ASSERT_NOT_NULL(strstr(inner, "func HandleRequest"));
    ASSERT_NOT_NULL(strstr(inner, "is_exported"));
    free(inner);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

TEST(tool_search_graph_query_honors_file_pattern_issue552) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "issue-552";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/issue-552");

    cbm_node_t lib_status = {0};
    lib_status.project = proj;
    lib_status.label = "Function";
    lib_status.name = "status";
    lib_status.qualified_name = "issue-552.src.lib.status";
    lib_status.file_path = "src/lib/status.c";
    lib_status.start_line = 1;
    lib_status.end_line = 3;
    ASSERT_GT(cbm_store_upsert_node(st, &lib_status), 0);

    cbm_node_t component_status = {0};
    component_status.project = proj;
    component_status.label = "Function";
    component_status.name = "status";
    component_status.qualified_name = "issue-552.src.components.status";
    component_status.file_path = "src/components/status.c";
    component_status.start_line = 1;
    component_status.end_line = 3;
    ASSERT_GT(cbm_store_upsert_node(st, &component_status), 0);

    cbm_store_exec(st, "INSERT INTO nodes_fts(nodes_fts) VALUES('delete-all');");
    ASSERT_EQ(cbm_store_exec(st,
                             "INSERT INTO nodes_fts(rowid, name, qualified_name, label, "
                             "file_path) "
                             "SELECT id, cbm_camel_split(name), qualified_name, label, file_path "
                             "FROM nodes;"),
              CBM_STORE_OK);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":552,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_graph\","
                                   "\"arguments\":{\"project\":\"issue-552\",\"query\":\"status\","
                                   "\"file_pattern\":\"src/lib/*\",\"limit\":10}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"search_mode\":\"bm25\""));
    ASSERT_NOT_NULL(strstr(inner, "\"file_path\":\"src/lib/status.c\""));
    ASSERT_NULL(strstr(inner, "src/components/status.c"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_basic) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"query_graph\","
             "\"arguments\":{\"query\":\"MATCH (f:Function) RETURN f.name\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_no_project) {
    cbm_mcp_server_t *srv = setup_mcp_with_data();

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or empty status */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_status_includes_git_metadata) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_status\","
                                   "\"arguments\":{\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"root_path\""));
    ASSERT_NOT_NULL(strstr(inner, "\"git\""));
    ASSERT_NOT_NULL(strstr(inner, "\"is_git\":false"));
    ASSERT_NOT_NULL(strstr(inner, "\"root_exists\":true"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  TOOL HANDLERS WITH DATA
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_trace_call_path_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{\"function_name\":\"NonExistent\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about project not found */
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_trace_missing_function_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"trace_call_path\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression: two same-named definitions with equal rank must be reported
 * ambiguous, not silently traced (trace_path previously took nodes[0]). */
TEST(tool_trace_call_path_ambiguous) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "amb-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/amb");
    cbm_node_t a = {.project = proj,
                    .label = "Function",
                    .name = "amb",
                    .qualified_name = "amb-proj.a.amb",
                    .file_path = "a.c",
                    .start_line = 10,
                    .end_line = 20};
    cbm_node_t b = {.project = proj,
                    .label = "Function",
                    .name = "amb",
                    .qualified_name = "amb-proj.b.amb",
                    .file_path = "b.c",
                    .start_line = 10,
                    .end_line = 20}; /* equal span -> genuine tie */
    ASSERT_GT(cbm_store_upsert_node(st, &a), 0);
    ASSERT_GT(cbm_store_upsert_node(st, &b), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\","
             "\"arguments\":{\"function_name\":\"amb\",\"project\":\"amb-proj\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "suggestions"));
    ASSERT_NULL(strstr(inner, "\"callees\""));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression: when same-named nodes differ in rank, trace must pick the real
 * definition (callable, larger body) — NOT nodes[0]. The Module is inserted
 * first; if trace took nodes[0] the outbound trace would be empty. */
TEST(tool_trace_call_path_prefers_definition) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "pref-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/pref");
    /* nodes[0]: the WRONG match (a Module, tiny span), inserted first. */
    cbm_node_t wrong = {.project = proj,
                        .label = "Module",
                        .name = "dup",
                        .qualified_name = "pref-proj.dup",
                        .file_path = "dup.x",
                        .start_line = 1,
                        .end_line = 1};
    /* the real definition: a Function with a body. */
    cbm_node_t def = {.project = proj,
                      .label = "Function",
                      .name = "dup",
                      .qualified_name = "pref-proj.src.dup",
                      .file_path = "src/dup.c",
                      .start_line = 10,
                      .end_line = 50};
    cbm_node_t callee = {.project = proj,
                         .label = "Function",
                         .name = "callee",
                         .qualified_name = "pref-proj.src.callee",
                         .file_path = "src/dup.c",
                         .start_line = 60,
                         .end_line = 70};
    ASSERT_GT(cbm_store_upsert_node(st, &wrong), 0);
    int64_t id_def = cbm_store_upsert_node(st, &def);
    int64_t id_callee = cbm_store_upsert_node(st, &callee);
    ASSERT_GT(id_def, 0);
    ASSERT_GT(id_callee, 0);
    cbm_edge_t e = {.project = proj, .source_id = id_def, .target_id = id_callee, .type = "CALLS"};
    cbm_store_insert_edge(st, &e);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"dup\","
             "\"project\":\"pref-proj\",\"direction\":\"outbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    /* picked the Function definition -> its outbound CALLS edge to "callee" shows */
    ASSERT_NOT_NULL(strstr(inner, "callee"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#887): the client-supplied `depth` on trace_call_path must be
 * clamped to the MCP ceiling (cbm_mcp_max_depth(), default 15). On origin/main
 * an MCP_MAX_DEPTH=15 constant was defined but never applied — `depth` flowed
 * straight into bfs_union_same_name, so an unbounded value drives the shared
 * cbm_store_bfs to arbitrary depth. Over an 18-node call chain, depth=1000
 * reaches n16/n17 (RED); with the clamp the walk stops at hop 15, so n15 is
 * reached but n16 is not (GREEN). Quoted tokens ("n15"/"n16") match only the
 * node-name field, never the qualified_name (preceded by '.'), so the boundary
 * check is exact. */
TEST(tool_trace_call_path_depth_clamped) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "depth-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/depth");

    /* Linear call chain n00 -CALLS-> n01 -> ... -> n17 (18 nodes). */
    int64_t ids[18];
    for (int i = 0; i < 18; i++) {
        char name[8];
        char qn[32];
        snprintf(name, sizeof(name), "n%02d", i);
        snprintf(qn, sizeof(qn), "depth-proj.n%02d", i);
        cbm_node_t n = {.project = proj,
                        .label = "Function",
                        .name = name,
                        .qualified_name = qn,
                        .file_path = "chain.c",
                        .start_line = 1,
                        .end_line = 2};
        ids[i] = cbm_store_upsert_node(st, &n);
    }
    for (int i = 0; i < 17; i++) {
        cbm_edge_t e = {
            .project = proj, .source_id = ids[i], .target_id = ids[i + 1], .type = "CALLS"};
        cbm_store_insert_edge(st, &e);
    }

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":71,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"n00\","
             "\"project\":\"depth-proj\",\"direction\":\"outbound\",\"depth\":1000}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* Reached within the ceiling (proves the traversal ran) but clamped at 15. */
    ASSERT_NOT_NULL(strstr(inner, "\"n15\""));
    ASSERT_NULL(strstr(inner, "\"n16\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#650, distilled): two GENUINELY-DIFFERENT same-named functions
 * whose bodies differ in length score differently, so the old exact-tie check did
 * not flag them ambiguous — and bfs_union_same_name (#546) then merged the caller
 * sets of both into one confidently-conflated answer (the mirror of #546's under-
 * report). The fix: 2+ real callable defs => ambiguous (disambiguate), never union
 * distinct symbols. RED before the pick_resolved_node real_def_count rule (response
 * merged callerA+callerB), GREEN after (response is ambiguous, no "callers"). */
TEST(tool_trace_call_path_distinct_defs_not_over_unioned) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "ou-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/ou");
    /* two unrelated real definitions of "dupreal", DIFFERENT body spans */
    cbm_node_t da = {.project = proj,
                     .label = "Function",
                     .name = "dupreal",
                     .qualified_name = "ou-proj.a.dupreal",
                     .file_path = "a.c",
                     .start_line = 10,
                     .end_line = 20}; /* span 10 */
    cbm_node_t db = {.project = proj,
                     .label = "Function",
                     .name = "dupreal",
                     .qualified_name = "ou-proj.b.dupreal",
                     .file_path = "b.c",
                     .start_line = 10,
                     .end_line = 40}; /* span 30 (no tie) */
    cbm_node_t ca = {.project = proj,
                     .label = "Function",
                     .name = "callerA",
                     .qualified_name = "ou-proj.a.callerA",
                     .file_path = "a.c",
                     .start_line = 30,
                     .end_line = 40};
    cbm_node_t cb = {.project = proj,
                     .label = "Function",
                     .name = "callerB",
                     .qualified_name = "ou-proj.b.callerB",
                     .file_path = "b.c",
                     .start_line = 50,
                     .end_line = 60};
    int64_t id_da = cbm_store_upsert_node(st, &da);
    int64_t id_db = cbm_store_upsert_node(st, &db);
    int64_t id_ca = cbm_store_upsert_node(st, &ca);
    int64_t id_cb = cbm_store_upsert_node(st, &cb);
    ASSERT_GT(id_da, 0);
    ASSERT_GT(id_db, 0);
    ASSERT_GT(id_ca, 0);
    ASSERT_GT(id_cb, 0);
    cbm_edge_t ea = {.project = proj, .source_id = id_ca, .target_id = id_da, .type = "CALLS"};
    cbm_edge_t eb = {.project = proj, .source_id = id_cb, .target_id = id_db, .type = "CALLS"};
    cbm_store_insert_edge(st, &ea);
    cbm_store_insert_edge(st, &eb);

    char *resp = cbm_mcp_server_handle(
        srv,
        "{\"jsonrpc\":\"2.0\",\"id\":63,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"dupreal\","
        "\"project\":\"ou-proj\",\"direction\":\"inbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    /* distinct symbols must be disambiguated, not merged into one caller set */
    ASSERT_NOT_NULL(strstr(inner, "ambiguous"));
    ASSERT_NOT_NULL(strstr(inner, "suggestions"));
    ASSERT_NULL(strstr(inner, "\"callers\""));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Guard that the ambiguity gate does NOT regress the #546 fix: a real .ts
 * implementation plus a body-less ambient .d.ts stub is ONE logical symbol
 * (one real callable def + a fragment), so it must stay non-ambiguous and the
 * caller sets from both nodes must be unioned. */
TEST(tool_trace_call_path_dts_stub_unions_with_impl) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "dts-proj";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/dts");
    cbm_node_t impl = {.project = proj,
                       .label = "Function",
                       .name = "sym546",
                       .qualified_name = "dts-proj.impl.sym546",
                       .file_path = "src/sym.ts",
                       .start_line = 10,
                       .end_line = 30}; /* real body */
    cbm_node_t stub = {.project = proj,
                       .label = "Function",
                       .name = "sym546",
                       .qualified_name = "dts-proj.stub.sym546",
                       .file_path = "types/sym.d.ts",
                       .start_line = 5,
                       .end_line = 5}; /* body-less ambient decl */
    cbm_node_t crel = {.project = proj,
                       .label = "Function",
                       .name = "callerRel",
                       .qualified_name = "dts-proj.callerRel",
                       .file_path = "src/rel.ts",
                       .start_line = 1,
                       .end_line = 8};
    cbm_node_t cali = {.project = proj,
                       .label = "Function",
                       .name = "callerAlias",
                       .qualified_name = "dts-proj.callerAlias",
                       .file_path = "src/ali.ts",
                       .start_line = 1,
                       .end_line = 8};
    int64_t id_impl = cbm_store_upsert_node(st, &impl);
    int64_t id_stub = cbm_store_upsert_node(st, &stub);
    int64_t id_crel = cbm_store_upsert_node(st, &crel);
    int64_t id_cali = cbm_store_upsert_node(st, &cali);
    ASSERT_GT(id_impl, 0);
    ASSERT_GT(id_stub, 0);
    ASSERT_GT(id_crel, 0);
    ASSERT_GT(id_cali, 0);
    /* callers split by import style: relative -> impl, path-alias -> stub */
    cbm_edge_t er = {.project = proj, .source_id = id_crel, .target_id = id_impl, .type = "CALLS"};
    cbm_edge_t el = {.project = proj, .source_id = id_cali, .target_id = id_stub, .type = "CALLS"};
    cbm_store_insert_edge(st, &er);
    cbm_store_insert_edge(st, &el);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":64,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"trace_call_path\",\"arguments\":{\"function_name\":\"sym546\","
             "\"project\":\"dts-proj\",\"direction\":\"inbound\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NULL(strstr(inner, "ambiguous"));
    /* union across impl + stub: BOTH callers appear (this is the #546 fix) */
    ASSERT_NOT_NULL(strstr(inner, "callerRel"));
    ASSERT_NOT_NULL(strstr(inner, "callerAlias"));
    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_delete_project_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"delete_project\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not_found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_architecture_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No store for nonexistent project — should return project error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression for #281: handle_get_architecture must actually call
 * cbm_store_get_architecture and surface its sections. Before the fix
 * only label/edge histograms were emitted regardless of which aspects
 * were requested. The store-side arch_entry_points query reads
 * properties.is_entry_point on Function nodes, so we tag one node and
 * assert the resulting JSON surfaces an "entry_points" array containing
 * the tagged function — which is impossible without the wiring. */
TEST(tool_get_architecture_emits_populated_sections) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-test";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-test");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "arch-test.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-test\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The handler always emits node/edge counts and schema histograms;
     * those existed before #281. The "entry_points" array only appears
     * when cbm_store_get_architecture is actually called and its result
     * is serialized — which is exactly what #281 wires up. */
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner, "main"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Distills PR #560 (overview subset): "overview" must expand to a compact
 * subset — every aspect EXCEPT file_tree. Before the fix, "overview" was not
 * registered in either aspect gate (want_aspect in store.c, aspect_wanted in
 * mcp.c), so aspects=["overview"] silently degraded to just
 * {total_nodes,total_edges}. RED on unfixed code: no "entry_points" key. */
TEST(tool_get_architecture_overview_compact_subset_pr560) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch560";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch560");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "arch560.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    /* A File node so the file_tree aspect has real content — makes the
     * "overview drops file_tree" assertion below non-vacuous. */
    cbm_node_t file_node = {.project = proj,
                            .label = "File",
                            .name = "main.go",
                            .qualified_name = "arch560.cmd.main.go",
                            .file_path = "cmd/main.go"};
    ASSERT_GT(cbm_store_upsert_node(st, &file_node), 0);

    /* Sanity: with "all", both entry_points and file_tree surface. */
    char *resp_all = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":560,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch560\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp_all);
    char *inner_all = extract_text_content(resp_all);
    ASSERT_NOT_NULL(inner_all);
    ASSERT_NOT_NULL(strstr(inner_all, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner_all, "\"file_tree\""));
    free(inner_all);
    free(resp_all);

    /* "overview": substantive content (entry_points, node_labels) but NO
     * file_tree section. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":561,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch560\",\"aspects\":[\"overview\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));
    ASSERT_NOT_NULL(strstr(inner, "\"node_labels\""));
    ASSERT_NULL(strstr(inner, "\"file_tree\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Distills PR #560 (server-side validation): unknown aspect tokens must be
 * rejected with an isError result listing the valid values. Before the fix
 * the JSON-Schema accepted any string and both aspect gates simply never
 * matched, so a typo like "bogus_aspect" produced a silent near-empty payload
 * with isError:false. RED on unfixed code: no isError, no "Unknown aspect". */
TEST(tool_get_architecture_rejects_unknown_aspect_pr560) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch560v";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch560v");

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":562,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch560v\",\"aspects\":[\"bogus_aspect\"]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(resp, "Unknown aspect 'bogus_aspect'"));
    /* The error must teach the valid vocabulary, including the new token. */
    ASSERT_NOT_NULL(strstr(resp, "overview"));
    ASSERT_NOT_NULL(strstr(resp, "file_tree"));

    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first for #640: query handlers must accept the `project_name`
 * alias, not only the canonical `project` key. list_projects surfaces the field
 * as "name" and the error hint says "pass the project name", so a caller
 * naturally passes `project_name`. With no alias, the handler reads key
 * "project" -> NULL -> resolve_store bails before opening any .db -> "project
 * not found or not indexed" even though the project is indexed. Mirrors
 * tool_get_architecture_emits_populated_sections but with the alias key. */
TEST(tool_get_architecture_accepts_project_name_alias_issue640) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "alias640";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/alias640");

    cbm_node_t main_fn = {0};
    main_fn.project = proj;
    main_fn.label = "Function";
    main_fn.name = "main";
    main_fn.qualified_name = "alias640.cmd.main";
    main_fn.file_path = "cmd/main.go";
    main_fn.start_line = 1;
    main_fn.end_line = 3;
    main_fn.properties_json = "{\"is_entry_point\":true}";
    ASSERT_GT(cbm_store_upsert_node(st, &main_fn), 0);

    /* Caller passes `project_name` (the natural guess) instead of `project`. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":640,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project_name\":\"alias640\",\"aspects\":[\"all\"]}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* RED before the alias: inner is the "project not found" error.
     * GREEN after: the alias resolves and architecture sections surface. */
    ASSERT_NULL(strstr(inner, "project not found"));
    ASSERT_NOT_NULL(strstr(inner, "\"entry_points\""));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first for #640: the alias must apply across query handlers, not
 * just get_architecture. search_graph with `project_name` must resolve too. */
TEST(tool_search_graph_accepts_project_name_alias_issue640) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "alias640b";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/alias640b");

    cbm_node_t fn = {0};
    fn.project = proj;
    fn.label = "Function";
    fn.name = "WidgetHandler";
    fn.qualified_name = "alias640b.svc.WidgetHandler";
    fn.file_path = "svc/widget.go";
    fn.start_line = 1;
    fn.end_line = 2;
    ASSERT_GT(cbm_store_upsert_node(st, &fn), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":641,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\","
             "\"arguments\":{\"project_name\":\"alias640b\",\"name_pattern\":\"Widget.*\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    ASSERT_NULL(strstr(inner, "project not found"));
    ASSERT_NOT_NULL(strstr(inner, "WidgetHandler"));

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression for #604: path scopes architecture totals and content. */
TEST(tool_get_architecture_path_scoping) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);

    const char *proj = "arch-path";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, "/tmp/arch-path");

    cbm_node_t pkg_global = {.project = proj,
                             .label = "Package",
                             .name = "Django",
                             .qualified_name = "arch-path.Django",
                             .file_path = "vendor/django/__init__.py"};
    cbm_store_upsert_node(st, &pkg_global);

    cbm_node_t pkg_local = {.project = proj,
                            .label = "Package",
                            .name = "hoa",
                            .qualified_name = "arch-path.hoa",
                            .file_path = "apps/hoa/main.go"};
    cbm_store_upsert_node(st, &pkg_local);

    cbm_node_t f_hoa = {.project = proj,
                        .label = "File",
                        .name = "main.go",
                        .qualified_name = "arch-path.apps.hoa.main.go",
                        .file_path = "apps/hoa/main.go"};
    cbm_store_upsert_node(st, &f_hoa);

    cbm_node_t f_other = {.project = proj,
                          .label = "File",
                          .name = "other.go",
                          .qualified_name = "arch-path.other.go",
                          .file_path = "lib/other.go"};
    cbm_store_upsert_node(st, &f_other);

    char *resp_root = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"get_architecture\","
             "\"arguments\":{\"project\":\"arch-path\",\"aspects\":[\"packages\"]}}}");
    ASSERT_NOT_NULL(resp_root);
    char *inner_root = extract_text_content(resp_root);
    ASSERT_NOT_NULL(inner_root);
    ASSERT_NOT_NULL(strstr(inner_root, "Django"));

    char *resp_scoped =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_architecture\","
                                   "\"arguments\":{\"project\":\"arch-path\",\"path\":\"apps/hoa\","
                                   "\"aspects\":[\"packages\"]}}}");
    ASSERT_NOT_NULL(resp_scoped);
    char *inner_scoped = extract_text_content(resp_scoped);
    ASSERT_NOT_NULL(inner_scoped);

    ASSERT_NOT_NULL(strstr(inner_scoped, "root_total_nodes"));
    ASSERT_NOT_NULL(strstr(inner_scoped, "scoped_total_nodes"));
    ASSERT_NOT_NULL(strstr(inner_scoped, "\"path\""));
    ASSERT_NOT_NULL(strstr(inner_scoped, "hoa"));
    ASSERT_NULL(strstr(inner_scoped, "Django"));

    int root_nodes = 0;
    int scoped_nodes = 0;
    const char *rt = strstr(inner_scoped, "\"root_total_nodes\":");
    const char *stn = strstr(inner_scoped, "\"scoped_total_nodes\":");
    if (rt) {
        sscanf(rt, "\"root_total_nodes\":%d", &root_nodes);
    }
    if (stn) {
        sscanf(stn, "\"scoped_total_nodes\":%d", &scoped_nodes);
    }
    ASSERT_TRUE(root_nodes > scoped_nodes);
    ASSERT_TRUE(scoped_nodes > 0);

    free(inner_scoped);
    free(resp_scoped);
    free(inner_root);
    free(resp_root);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_query_graph_missing_query) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"query_graph\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about missing query */
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  PIPELINE-DEPENDENT TOOL HANDLERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(tool_index_repository_missing_path) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"index_repository\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_missing_qn) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_get_code_snippet_not_found) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"get_code_snippet\","
                                   "\"arguments\":{\"qualified_name\":\"nonexistent.func\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_missing_pattern) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_search_code_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func main\","
                                   "\"project\":\"nonexistent\"}}}");
    ASSERT_NOT_NULL(resp);
    /* No project indexed → error */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "not indexed") ||
                strstr(resp, "required"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(search_code_multi_word) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Multi-word query "HandleRequest error" — should find the line
     * "func HandleRequest() error {" via regex conversion. */
    char req[512];
    snprintf(req, sizeof(req),
             "{\"jsonrpc\":\"2.0\",\"id\":90,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest error\","
             "\"project\":\"test-project\"}}}");

    char *resp = cbm_mcp_server_handle(srv, req);
    ASSERT_NOT_NULL(resp);
    /* Should find at least one result (not zero) */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL);
    /* Should NOT contain an error about "not found" */
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* Reproduce-first (#687): scoped content search over a repo whose ROOT PATH
 * contains a space. write_scoped_filelist emits "<root>/<file>" records that the
 * Unix pipeline pipes to grep via xargs. With plain `xargs` (newline-split) the
 * space splits one path into several bogus args -> grep finds nothing ->
 * total_grep_matches == 0 (RED on the unfixed code). The fix writes NUL-separated
 * records + uses `xargs -0`, so the path stays a single argument -> match found
 * (GREEN). On Windows the scoped path uses PowerShell Get-Content -LiteralPath,
 * which already handles spaces, so this asserts correct behavior there too. */
TEST(search_code_scoped_path_with_spaces_issue687) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "/tmp/cbm_srch_space_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        FAIL("cbm_mkdtemp failed");
    }

    /* Project root deliberately contains a space. */
    char proj_dir[640];
    snprintf(proj_dir, sizeof(proj_dir), "%s/my project", tmp);
    cbm_mkdir(proj_dir);

    char src_path[768];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        rmdir(proj_dir);
        rmdir(tmp);
        FAIL("cannot write source file under spaced path");
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    const char *proj = "space-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, proj_dir);

    /* A node so the file is "indexed" (cbm_store_list_files -> scoped grep path)
     * and the grep hit classifies to a result. */
    cbm_node_t n = {.project = proj,
                    .label = "Function",
                    .name = "HandleRequest",
                    .qualified_name = "space-search.main.HandleRequest",
                    .file_path = "main.go",
                    .start_line = 3,
                    .end_line = 5};
    ASSERT_GT(cbm_store_upsert_node(st, &n), 0);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"space-search\"}}}");
    ASSERT_NOT_NULL(resp);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* grep must have found the match despite the space in the root path. */
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_TRUE(grep_matches > 0);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    unlink(src_path);
    rmdir(proj_dir);
    rmdir(tmp);
    PASS();
}

/* Shared fixture for the path_filter prefilter tests (PR #756 distilled):
 * a project with two indexed files that both contain the search pattern —
 * src/handler.go (inside the filter) and vendor/other.go (outside it). */
static cbm_mcp_server_t *setup_prefilter_server(char *tmp, size_t tmp_sz, char *src_path,
                                                size_t src_sz, char *vendor_path,
                                                size_t vendor_sz) {
    snprintf(tmp, tmp_sz, "/tmp/cbm_srch_pref_XXXXXX");
    if (!cbm_mkdtemp(tmp)) {
        return NULL;
    }
    char dir[640];
    snprintf(dir, sizeof(dir), "%s/src", tmp);
    cbm_mkdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor", tmp);
    cbm_mkdir(dir);

    snprintf(src_path, src_sz, "%s/src/handler.go", tmp);
    snprintf(vendor_path, vendor_sz, "%s/vendor/other.go", tmp);
    FILE *fp = fopen(src_path, "w");
    if (!fp) {
        return NULL;
    }
    fprintf(fp, "package main\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);
    fp = fopen(vendor_path, "w");
    if (!fp) {
        return NULL;
    }
    fprintf(fp, "package vendored\n\nfunc HandleRequest() error {\n\treturn nil\n}\n");
    fclose(fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        return NULL;
    }
    cbm_store_t *st = cbm_mcp_server_store(srv);
    const char *proj = "prefilter-search";
    cbm_mcp_server_set_project(srv, proj);
    cbm_store_upsert_project(st, proj, tmp);

    cbm_node_t n1 = {.project = proj,
                     .label = "Function",
                     .name = "HandleRequest",
                     .qualified_name = "prefilter-search.main.HandleRequest",
                     .file_path = "src/handler.go",
                     .start_line = 3,
                     .end_line = 5};
    cbm_node_t n2 = {.project = proj,
                     .label = "Function",
                     .name = "HandleRequest",
                     .qualified_name = "prefilter-search.vendored.HandleRequest",
                     .file_path = "vendor/other.go",
                     .start_line = 3,
                     .end_line = 5};
    if (cbm_store_upsert_node(st, &n1) <= 0 || cbm_store_upsert_node(st, &n2) <= 0) {
        cbm_mcp_server_free(srv);
        return NULL;
    }
    return srv;
}

static void cleanup_prefilter_dir(const char *tmp, const char *src_path, const char *vendor_path) {
    char dir[640];
    unlink(src_path);
    unlink(vendor_path);
    snprintf(dir, sizeof(dir), "%s/src", tmp);
    rmdir(dir);
    snprintf(dir, sizeof(dir), "%s/vendor", tmp);
    rmdir(dir);
    rmdir(tmp);
}

/* PR #756 (distilled): scoped search_code prefilters the indexed filelist by
 * path_filter before grep runs. POSITIVE invariant guard: a path_filter that
 * matches the file containing the hit must still return that hit (guards
 * against over-filtering — the prefilter predicate must stay IDENTICAL to the
 * post-grep filter in collect_grep_matches), and files outside the filter
 * stay excluded. Green on pre-prefilter main too (the post-grep filter alone
 * produced the same results): the change is results-preserving perf-only. */
TEST(search_code_path_filter_prefilter_keeps_matches) {
    char tmp[512], src_path[768], vendor_path[768];
    cbm_mcp_server_t *srv = setup_prefilter_server(tmp, sizeof(tmp), src_path, sizeof(src_path),
                                                   vendor_path, sizeof(vendor_path));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":95,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"prefilter-search\","
             "\"path_filter\":\"^src/\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    /* The in-filter hit is returned; the out-of-filter file is not. */
    ASSERT_NOT_NULL(strstr(inner, "src/handler.go"));
    ASSERT_TRUE(strstr(inner, "vendor/other.go") == NULL);

    /* Exactly the one in-filter grep match survives (same count before and
     * after the prefilter — predicate identity). */
    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 1);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_prefilter_dir(tmp, src_path, vendor_path);
    PASS();
}

/* PR #756 (distilled): path_filter matching ZERO indexed files. With the
 * prefilter the scoped filelist has 0 records, and handle_search_code now
 * skips the grep subprocess entirely (xargs on an empty filelist is
 * platform-dependent: GNU execs grep once with no operands, BSD skips) and
 * returns the empty result directly. Must be a clean zero-result response —
 * no error. Green on pre-prefilter main too (there the full filelist is
 * grepped and the post-grep filter drops every hit — an empty filelist is
 * unreachable on main): guards the edge the prefilter introduces. */
TEST(search_code_path_filter_matches_nothing) {
    char tmp[512], src_path[768], vendor_path[768];
    cbm_mcp_server_t *srv = setup_prefilter_server(tmp, sizeof(tmp), src_path, sizeof(src_path),
                                                   vendor_path, sizeof(vendor_path));
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":96,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_code\","
             "\"arguments\":{\"pattern\":\"HandleRequest\",\"project\":\"prefilter-search\","
             "\"path_filter\":\"^no_such_dir/\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "\"isError\":true") == NULL);
    char *inner = extract_text_content(resp);
    ASSERT_NOT_NULL(inner);

    int grep_matches = -1;
    const char *g = strstr(inner, "\"total_grep_matches\":");
    if (g) {
        sscanf(g, "\"total_grep_matches\":%d", &grep_matches);
    }
    ASSERT_EQ(grep_matches, 0);
    int results = -1;
    const char *r = strstr(inner, "\"total_results\":");
    if (r) {
        sscanf(r, "\"total_results\":%d", &results);
    }
    ASSERT_EQ(results, 0);
    ASSERT_TRUE(strstr(inner, "handler.go") == NULL);
    ASSERT_TRUE(strstr(inner, "other.go") == NULL);

    free(inner);
    free(resp);
    cbm_mcp_server_free(srv);
    cleanup_prefilter_dir(tmp, src_path, vendor_path);
    PASS();
}

/* issue #283: search_code with regex=true and a syntactically invalid pattern
 * must return an explicit error, not an empty result indistinguishable from a
 * legitimate no-match. */
TEST(search_code_invalid_regex_errors_issue283) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* Unclosed group under regex=true → must be flagged as an error. */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":91,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"func(\",\"regex\":true,"
                                   "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"isError\":true"));
    ASSERT_NOT_NULL(strstr(resp, "invalid regex"));
    free(resp);

    /* Same pattern as a literal (regex=false) must NOT error. */
    resp = cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":92,\"method\":\"tools/call\","
                                      "\"params\":{\"name\":\"search_code\","
                                      "\"arguments\":{\"pattern\":\"func(\",\"regex\":false,"
                                      "\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid regex") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #282: a literal '|' under regex=false is a silent 0-match trap. It must
 * now be surfaced as a warning (and the result carries elapsed_ms). */
TEST(search_code_literal_pipe_warns_issue282) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":93,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest|Nope\","
                                   "\"regex\":false,\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "warnings"));   /* surfaced, not silent */
    ASSERT_NOT_NULL(strstr(resp, "regex=true")); /* the hint names the fix */
    ASSERT_NOT_NULL(strstr(resp, "elapsed_ms")); /* timing is reported */
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

/* issue #272: '&' in a path / file_pattern is neutralised by the command's
 * quoting and must no longer be rejected as "invalid characters". */
TEST(search_code_ampersand_accepted_issue272) {
    char tmp[512];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":94,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"search_code\","
                                   "\"arguments\":{\"pattern\":\"HandleRequest\","
                                   "\"file_pattern\":\"*R&D*.go\",\"project\":\"test-project\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(strstr(resp, "invalid characters") == NULL);
    free(resp);

    cleanup_snippet_dir(tmp);
    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_detect_changes_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"detect_changes\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_manage_adr_no_project) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":36,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"manage_adr\","
                                   "\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "missing required argument: project"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* Regression test for use-after-free in handle_manage_adr (get path).
 * MUST FAIL before fix: free(buf) is called before yy_doc_to_str serializes doc,
 * so result field is missing or contains garbage. MUST PASS after fix. */
TEST(tool_manage_adr_get_with_existing_adr) {
    /* Create a temp directory with .codebase-memory/adr.md */
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS(); /* skip if mkdtemp fails */
    }

    char adr_dir[512];
    snprintf(adr_dir, sizeof(adr_dir), "%s/.codebase-memory", tmp_dir);
    cbm_mkdir(adr_dir);

    char adr_path[512];
    snprintf(adr_path, sizeof(adr_path), "%s/adr.md", adr_dir);
    FILE *fp = fopen(adr_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("## PURPOSE\nTest ADR content for regression test.\n\n"
          "## STACK\nC, SQLite.\n\n"
          "## ARCHITECTURE\nMCP server.\n",
          fp);
    fclose(fp);

    /* Create server and register the project */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "test-adr-uaf", tmp_dir);
    cbm_mcp_server_set_project(srv, "test-adr-uaf");

    /* Call manage_adr via full JSON-RPC path to exercise cbm_jsonrpc_format_response.
     * The bug: free(buf) before yy_doc_to_str causes garbage JSON; format_response
     * then fails to parse the result and omits the "result" field entirely. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":99,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\","
             "\"arguments\":{\"project\":\"test-adr-uaf\",\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    /* JSON-RPC response must include a "result" field (absent when use-after-free) */
    ASSERT_NOT_NULL(strstr(resp, "\"result\""));
    /* ADR content must appear in response */
    ASSERT_NOT_NULL(strstr(resp, "PURPOSE"));
    /* Must not be an error */
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    /* Clean up */
    cbm_mcp_server_free(srv);
    remove(adr_path);
    rmdir(adr_dir);
    rmdir(tmp_dir);
    PASS();
}

/* issue #256: manage_adr (MCP) and the UI /api/adr endpoints must share ONE
 * backend. A manage_adr(update) write must be readable via cbm_store_adr_get
 * (the exact API the UI's /api/adr GET uses). */
TEST(tool_manage_adr_unified_backend_issue256) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    cbm_store_t *st = cbm_mcp_server_store(srv);
    ASSERT_NOT_NULL(st);
    cbm_store_upsert_project(st, "adr-unify", "/tmp/adr-unify");
    cbm_mcp_server_set_project(srv, "adr-unify");

    /* Write via the MCP tool. */
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":120,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"update\",\"content\":\"## PURPOSE\\nUnified ADR backend.\\n\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    /* Read DIRECTLY via the store API the UI /api/adr uses — must see it. */
    cbm_adr_t adr;
    memset(&adr, 0, sizeof(adr));
    ASSERT_EQ(cbm_store_adr_get(st, "adr-unify", &adr), CBM_STORE_OK);
    ASSERT_NOT_NULL(adr.content);
    ASSERT_NOT_NULL(strstr(adr.content, "Unified ADR backend."));
    cbm_store_adr_free(&adr);

    /* And manage_adr(get) round-trips the same content. */
    resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":121,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"manage_adr\",\"arguments\":{\"project\":\"adr-unify\","
             "\"mode\":\"get\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Unified ADR backend."));
    ASSERT_NULL(strstr(resp, "\"isError\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_index_repository_reports_store_backed_adr) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-adr-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Store-backed ADR metadata.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    ASSERT(response_contains_json_fragment(resp, "\"adr_present\":true"));
    ASSERT_NULL(strstr(resp, "adr_hint"));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Store-backed ADR metadata."));
    ASSERT_NULL(strstr(resp, "no_adr"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_index_repository_dot_uses_absolute_project_key_and_preserves_adr) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-index-dot-adr-test-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-index-dot-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return helper()\n\ndef helper():\n    return 1\n", fp);
    fclose(fp);

    char old_cwd[CBM_SZ_4K];
    ASSERT_NOT_NULL(cbm_getcwd(old_cwd, sizeof(old_cwd)));

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    ASSERT_EQ(cbm_chdir(tmp_dir), 0);
    char *resp =
        cbm_mcp_handle_tool(srv, "index_repository", "{\"repo_path\":\".\",\"mode\":\"fast\"}");
    ASSERT_EQ(cbm_chdir(old_cwd), 0);
    ASSERT_NOT_NULL(resp);
    if (!response_contains_json_fragment(resp, "\"status\":\"indexed\"")) {
        free(resp);
        cbm_mcp_server_free(srv);
        cleanup_project_db(cache, project);
        restore_cache_dir(saved_copy);
        free(saved_copy);
        free(project);
        remove(src_path);
        cbm_rmdir(cache);
        cbm_rmdir(tmp_dir);
        PASS();
    }
    ASSERT_NOT_NULL(strstr(resp, project));
    ASSERT(!response_contains_json_fragment(resp, "\"project\":\"root\""));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Dot-path ADR marker.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    ASSERT_EQ(cbm_chdir(tmp_dir), 0);
    resp = cbm_mcp_handle_tool(srv, "index_repository", "{\"repo_path\":\".\",\"mode\":\"fast\"}");
    ASSERT_EQ(cbm_chdir(old_cwd), 0);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    ASSERT_NOT_NULL(strstr(resp, project));
    ASSERT(response_contains_json_fragment(resp, "\"adr_present\":true"));
    ASSERT(!response_contains_json_fragment(resp, "\"project\":\"root\""));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Dot-path ADR marker."));
    ASSERT_NULL(strstr(resp, "no_adr"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_manage_adr_not_found_rich_error) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-adr-missing-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp = cbm_mcp_handle_tool(srv, "manage_adr",
                                     "{\"project\":\"cbm-no-such-project-zzz\",\"mode\":\"get\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NOT_NULL(strstr(resp, "hint"));
    free(resp);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(tool_manage_adr_get_accepts_abs_path) {
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-abspath-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-adr-abspath-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", tmp_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Abs-path normalization test.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", tmp_dir);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Abs-path normalization test."));
    ASSERT_NULL(strstr(resp, "or not indexed"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
}

TEST(tool_manage_adr_get_accepts_symlink_path) {
#ifdef _WIN32
    PASS();
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-adr-realpath-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-adr-realpath-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    char link_path[320];
    snprintf(link_path, sizeof(link_path), "%s-link", tmp_dir);
    (void)unlink(link_path);
    if (symlink(tmp_dir, link_path) != 0) {
        cbm_rmdir(cache);
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    char *project = cbm_project_name_from_path(tmp_dir);
    ASSERT_NOT_NULL(project);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", link_path);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);
    ASSERT_NOT_NULL(resp);
    ASSERT(response_contains_json_fragment(resp, "\"status\":\"indexed\""));
    ASSERT_NOT_NULL(strstr(resp, project));
    free(resp);

    char update_args[2048];
    snprintf(update_args, sizeof(update_args),
             "{\"project\":\"%s\",\"mode\":\"update\",\"content\":\"## PURPOSE\\n"
             "Symlink-path normalization test.\\n\"}",
             project);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", update_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "updated"));
    free(resp);

    char get_args[512];
    snprintf(get_args, sizeof(get_args), "{\"project\":\"%s\",\"mode\":\"get\"}", link_path);
    resp = cbm_mcp_handle_tool(srv, "manage_adr", get_args);
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "Symlink-path normalization test."));
    ASSERT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NULL(strstr(resp, "no_adr"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    free(project);
    remove(src_path);
    unlink(link_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);
    PASS();
#endif
}

TEST(tool_detect_changes_not_found_rich_error) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-detect-missing-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS();
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    char *resp =
        cbm_mcp_handle_tool(srv, "detect_changes", "{\"project\":\"cbm-no-such-project-zzz\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "or not indexed"));
    ASSERT_NOT_NULL(strstr(resp, "hint"));
    free(resp);

    cbm_mcp_server_free(srv);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(tool_ingest_traces_basic) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":37,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"ingest_traces\","
             "\"arguments\":{\"traces\":[{\"caller\":\"a\",\"callee\":\"b\"}]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    ASSERT_NOT_NULL(strstr(resp, "traces_received"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(tool_ingest_traces_empty) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":38,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"ingest_traces\","
                                   "\"arguments\":{\"traces\":[]}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "accepted"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  IDLE STORE EVICTION
 * ══════════════════════════════════════════════════════════════════ */

TEST(store_idle_eviction) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* Trigger resolve_store via a tool call to set store_last_used */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with 0s timeout → should evict immediately */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_no_eviction_within_timeout) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* Evict with large timeout → should NOT evict */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_protects_initial_store) {
    /* Evicting with NULL server should not crash */
    cbm_mcp_server_evict_idle(NULL, 0);

    /* Evicting server whose store was never accessed via a named project
     * should NOT evict the initial in-memory store (store_last_used == 0). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(store_idle_evict_access_resets_timer) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    cbm_mcp_server_set_project(srv, "test-evict");

    /* First access */
    char *resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    /* Second access (resets timer) */
    resp = cbm_mcp_handle_tool(srv, "get_graph_schema", "{\"project\":\"test-evict\"}");
    free(resp);

    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With large timeout, store should survive */
    cbm_mcp_server_evict_idle(srv, 99999);
    ASSERT_TRUE(cbm_mcp_server_has_cached_store(srv));

    /* With 0 timeout, store should be evicted */
    cbm_mcp_server_evict_idle(srv, 0);
    ASSERT_FALSE(cbm_mcp_server_has_cached_store(srv));

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  URI HELPERS
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_unix) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/home/user/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///tmp/test", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/tmp/test");

    ASSERT_TRUE(cbm_parse_file_uri("file:///", path, sizeof(path)));
    ASSERT_STR_EQ(path, "/");
    PASS();
}

TEST(parse_file_uri_windows) {
    char path[256];
    /* Windows drive letter — leading / stripped */
    ASSERT_TRUE(cbm_parse_file_uri("file:///C:/Users/project", path, sizeof(path)));
    ASSERT_STR_EQ(path, "C:/Users/project");

    ASSERT_TRUE(cbm_parse_file_uri("file:///D:/Projects/myapp", path, sizeof(path)));
    ASSERT_STR_EQ(path, "D:/Projects/myapp");
    PASS();
}

TEST(parse_file_uri_invalid) {
    char path[256];
    /* Non-file URI */
    ASSERT_FALSE(cbm_parse_file_uri("https://example.com", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* Empty string */
    ASSERT_FALSE(cbm_parse_file_uri("", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");

    /* NULL */
    ASSERT_FALSE(cbm_parse_file_uri(NULL, path, sizeof(path)));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SNIPPET TESTS — Port of internal/tools/snippet_test.go
 * ══════════════════════════════════════════════════════════════════ */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/* Create an MCP server pre-populated with nodes/edges matching Go testSnippetServer.
 * Writes a source file to tmp_dir/project/main.go.
 * Caller must free the server with cbm_mcp_server_free and
 * unlink the source file + rmdir manually. */
static cbm_mcp_server_t *setup_snippet_server(char *tmp_dir, size_t tmp_sz) {
    /* Create temp dir */
    snprintf(tmp_dir, tmp_sz, "/tmp/cbm_snippet_test_XXXXXX");
    if (!cbm_mkdtemp(tmp_dir))
        return NULL;

    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "%s/project", tmp_dir);
    cbm_mkdir(proj_dir);

    /* Write sample source file */
    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.go", proj_dir);
    FILE *fp = fopen(src_path, "w");
    if (!fp)
        return NULL;
    fprintf(fp, "package main\n"
                "\n"
                "func HandleRequest() error {\n"
                "\treturn nil\n"
                "}\n"
                "\n"
                "func ProcessOrder(id int) {\n"
                "\t// process\n"
                "}\n"
                "\n"
                "func Run() {\n"
                "\t// server\n"
                "}\n");
    fclose(fp);

    /* Create server with in-memory store */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv)
        return NULL;

    cbm_store_t *st = cbm_mcp_server_store(srv);
    if (!st) {
        cbm_mcp_server_free(srv);
        return NULL;
    }

    const char *proj_name = "test-project";
    cbm_mcp_server_set_project(srv, proj_name);
    cbm_store_upsert_project(st, proj_name, proj_dir);

    /* Create nodes */
    cbm_node_t n_hr = {0};
    n_hr.project = proj_name;
    n_hr.label = "Function";
    n_hr.name = "HandleRequest";
    n_hr.qualified_name = "test-project.cmd.server.main.HandleRequest";
    n_hr.file_path = "main.go";
    n_hr.start_line = 3;
    n_hr.end_line = 5;
    n_hr.properties_json = "{\"signature\":\"func HandleRequest() error\","
                           "\"return_type\":\"error\","
                           "\"is_exported\":true}";
    int64_t id_hr = cbm_store_upsert_node(st, &n_hr);

    cbm_node_t n_po = {0};
    n_po.project = proj_name;
    n_po.label = "Function";
    n_po.name = "ProcessOrder";
    n_po.qualified_name = "test-project.cmd.server.main.ProcessOrder";
    n_po.file_path = "main.go";
    n_po.start_line = 7;
    n_po.end_line = 9;
    n_po.properties_json = "{\"signature\":\"func ProcessOrder(id int)\"}";
    int64_t id_po = cbm_store_upsert_node(st, &n_po);

    cbm_node_t n_run1 = {0};
    n_run1.project = proj_name;
    n_run1.label = "Function";
    n_run1.name = "Run";
    n_run1.qualified_name = "test-project.cmd.server.Run";
    n_run1.file_path = "main.go";
    n_run1.start_line = 11;
    n_run1.end_line = 13;
    int64_t id_run1 = cbm_store_upsert_node(st, &n_run1);

    cbm_node_t n_run2 = {0};
    n_run2.project = proj_name;
    n_run2.label = "Function";
    n_run2.name = "Run";
    n_run2.qualified_name = "test-project.cmd.worker.Run";
    n_run2.file_path = "main.go";
    n_run2.start_line = 11;
    n_run2.end_line = 13;
    cbm_store_upsert_node(st, &n_run2);

    /* Create edges: HandleRequest -> ProcessOrder, HandleRequest -> Run1 */
    cbm_edge_t e1 = {.project = proj_name, .source_id = id_hr, .target_id = id_po, .type = "CALLS"};
    cbm_store_insert_edge(st, &e1);

    cbm_edge_t e2 = {
        .project = proj_name, .source_id = id_hr, .target_id = id_run1, .type = "CALLS"};
    cbm_store_insert_edge(st, &e2);
    (void)id_run1; /* run1 used for edge above */

    return srv;
}

/* Cleanup temp files created by setup_snippet_server */
static void cleanup_snippet_dir(const char *tmp_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/project/main.go", tmp_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/project", tmp_dir);
    rmdir(path);
    rmdir(tmp_dir);
}

/* Extract the inner "text" value from an MCP tool result JSON.
 * The MCP envelope is: {"content":[{"type":"text","text":"<inner json>"}]}
 * This returns the unescaped inner JSON. Caller must free. */
static char *extract_text_content(const char *mcp_result) {
    if (!mcp_result)
        return NULL;
    yyjson_doc *doc = yyjson_read(mcp_result, strlen(mcp_result), 0);
    if (!doc)
        return strdup(mcp_result); /* fallback */
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *content = yyjson_obj_get(root, "content");
    if (!content) {
        /* Handle JSON-RPC wrapper: {"jsonrpc":...,"result":{"content":[...]}} */
        yyjson_val *rpc_result = yyjson_obj_get(root, "result");
        if (rpc_result) {
            content = yyjson_obj_get(rpc_result, "content");
        }
    }
    if (!content || !yyjson_is_arr(content)) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *item = yyjson_arr_get(content, 0);
    if (!item) {
        yyjson_doc_free(doc);
        return strdup(mcp_result);
    }
    yyjson_val *text = yyjson_obj_get(item, "text");
    const char *str = yyjson_get_str(text);
    char *result = str ? strdup(str) : strdup(mcp_result);
    yyjson_doc_free(doc);
    return result;
}

/* Call get_code_snippet and extract inner text content.
 * Caller must free returned string. */
static char *call_snippet(cbm_mcp_server_t *srv, const char *args_json) {
    char *raw = cbm_mcp_handle_tool(srv, "get_code_snippet", args_json);
    char *text = extract_text_content(raw);
    free(raw);
    return text;
}

static bool is_valid_json_response(const char *json) {
    if (!json) {
        return false;
    }
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_doc_free(doc);
    return true;
}

static bool snippet_source_has_replacement(const char *json) {
    yyjson_doc *doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *source = yyjson_obj_get(root, "source");
    const char *source_str = yyjson_get_str(source);
    bool found = source_str && strstr(source_str, "\xEF\xBF\xBD");
    yyjson_doc_free(doc);
    return found;
}

/* Extract a string field from the inner JSON carried by an MCP text result.
 * Caller owns the returned copy. */
static char *mcp_text_field_dup(const char *mcp_result, const char *field) {
    char *text = extract_text_content(mcp_result);
    if (!text) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    free(text);
    if (!doc) {
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *value = root && yyjson_is_obj(root) ? yyjson_obj_get(root, field) : NULL;
    const char *str = value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
    char *copy = str ? strdup(str) : NULL;
    yyjson_doc_free(doc);
    return copy;
}

static char *mcp_first_memory_field_dup(const char *mcp_result, const char *field) {
    char *text = extract_text_content(mcp_result);
    if (!text) return NULL;
    yyjson_doc *doc = yyjson_read(text, strlen(text), 0);
    free(text);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *memories = root && yyjson_is_obj(root) ? yyjson_obj_get(root, "memories") : NULL;
    yyjson_val *first = memories && yyjson_is_arr(memories) ? yyjson_arr_get(memories, 0) : NULL;
    yyjson_val *value = first && yyjson_is_obj(first) ? yyjson_obj_get(first, field) : NULL;
    const char *str = value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
    char *copy = str ? strdup(str) : NULL;
    yyjson_doc_free(doc);
    return copy;
}

static int count_feedback_event_rows(const char *db_path, const char *event_id) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        return -1;
    }
    const char *sql = "SELECT COUNT(*) FROM memory_event "
                      "WHERE id=?1 AND type='feedback' AND source='mcp.memory_feedback';";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

static int count_sql_rows(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return -1;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

static int count_exact_derived_relation(const char *db_path, const char *edge_id,
                                        const char *source_id, const char *target_id,
                                        const char *event_id) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int count = -1;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *sql =
        "SELECT COUNT(*) FROM memory_edge WHERE id=?1 AND src_id=?2 AND dst_id=?3 "
        "AND type='derived_from' AND weight=1.0 AND confidence=1.0 AND origin=?4;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, target_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, event_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

static char *sqlite_text_dup(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    char *value = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        value = text ? strdup(text) : NULL;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return value;
}

static bool sqlite_table_exists(const char *db_path, const char *table_name) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    bool exists = false;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return false;
    }
    if (sqlite3_prepare_v2(db,
                           "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_TRANSIENT);
        exists = sqlite3_step(stmt) == SQLITE_ROW;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return exists;
}

static bool insert_stage6_test_edge(const char *db_path, const char *edge_id,
                                    const char *src_id, const char *dst_id,
                                    const char *type) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO memory_edge(id,src_id,dst_id,type,weight,origin,confidence,created_at) "
        "VALUES(?1,?2,?3,?4,1.0,'stage6_mcp_test',1.0,1);";
    bool ok = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(stmt, 1, edge_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, src_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, dst_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, type, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return ok;
}

static bool sqlite_exec_ok(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    bool ok = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
    sqlite3_close(db);
    return ok;
}

static void cleanup_memory_project_db(const char *cache, const char *project) {
    static const char *suffixes[] = {"", "-wal", "-shm", "-journal"};
    char path[CBM_SZ_4K];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s-memory.db%s", cache, project, suffixes[i]);
        cbm_unlink(path);
    }
}

TEST(stage11_security_check_and_events_reject_before_transaction) {
    const char *project = "stage11-security-mcp-fixture";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage11-security-XXXXXX");
    if (!cbm_mkdtemp(cache)) FAIL("could not create Stage 11 security fixture cache");
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[CBM_SZ_4K];
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"Stage 11 ordinary fixture fact\",\"payload\":{}}",
             project);
    char *created = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(created, "\"status\":\"accepted\""));
    free(created);

    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    int events_before = count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event");
    int items_before = count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item");
    int fts_before = count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts");
    int edges_before = count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge");

    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"store\":\"project-memory\","
             "\"content\":\"G10 seal c26de83abdc9496cd1301470918ec39ecca1cf389ef0ae1c6504da1800d1c431\"}",
             project);
    char *safe = cbm_mcp_handle_tool(srv, "memory_security_check", args);
    ASSERT_TRUE(response_contains_json_fragment(safe, "\"code\":\"OK\""));
    ASSERT_TRUE(response_contains_json_fragment(safe, "\"untrusted_data\":true"));
    free(safe);

    char marker[96];
    snprintf(marker, sizeof(marker), "%s%s%s", "SYNTHETIC_", "STAGE11_",
             "MCP_SECRET_0123456789");
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"store\":\"project-memory\","
             "\"content\":\"api_key=%s\"}", project, marker);
    char *dry_run = cbm_mcp_handle_tool(srv, "memory_security_check", args);
    ASSERT_TRUE(response_contains_json_fragment(
        dry_run, "\"code\":\"SECURITY_SECRET_REJECTED\""));
    ASSERT_NULL(strstr(dry_run, marker));
    free(dry_run);

    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"password=%s\",\"payload\":{}}",
             project, marker);
    char *blocked = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"code\":\"SECURITY_SECRET_REJECTED\""));
    ASSERT_NOT_NULL(strstr(blocked, "\"isError\":true"));
    ASSERT_NULL(strstr(blocked, marker));
    free(blocked);

    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"ordinary\",\"summary\":\"api_key=%s\",\"payload\":{}}",
             project, marker);
    blocked = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"code\":\"SECURITY_SECRET_REJECTED\""));
    ASSERT_NULL(strstr(blocked, marker));
    free(blocked);

    const char *invalid_hash = "caller-controlled-non-sha256-hash";
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"event_id\":\"stage11-reject-injection\","
             "\"session_id\":\"session-1\",\"candidate_id\":\"candidate-1\","
             "\"injection_index\":0,\"target\":\"context\",\"content_hash\":\"%s\","
             "\"token_count\":1,\"classifier_status\":\"pass\","
             "\"classification\":\"secret\"}", project, invalid_hash);
    blocked = cbm_mcp_handle_tool(srv, "memory_observe_injection", args);
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"code\":\"SECURITY_SECRET_REJECTED\""));
    ASSERT_NULL(strstr(blocked, invalid_hash));
    char *response_hash = mcp_text_field_dup(blocked, "content_sha256");
    ASSERT_NOT_NULL(response_hash);
    ASSERT_EQ(strlen(response_hash), 64);
    free(response_hash);
    free(blocked);

    snprintf(args, sizeof(args),
             "{\"project\":\"%s-memory\",\"kind\":\"fact\","
             "\"content\":\"ordinary\",\"payload\":{}}",
             project);
    char *scope = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(
        scope, "\"code\":\"SECURITY_SCOPE_VIOLATION\""));
    free(scope);

    snprintf(args, sizeof(args),
             "{\"project\":\"stage11-unknown-project\",\"kind\":\"fact\","
             "\"content\":\"ordinary\",\"payload\":{}}" );
    scope = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(
        scope, "\"code\":\"SECURITY_SCOPE_VIOLATION\""));
    free(scope);
    char unknown_db[CBM_SZ_4K];
    snprintf(unknown_db, sizeof(unknown_db), "%s/stage11-unknown-project-memory.db", cache);
    struct stat unknown_stat;
    ASSERT_TRUE(stat(unknown_db, &unknown_stat) != 0);

    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), events_before);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), items_before);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts"), fts_before);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), edges_before);

    cbm_mcp_server_free(srv);
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(events_derived_from_is_project_scoped_atomic_and_compatible) {
    const char *project = "stage6-fixture-stage8-derived-v2-mcp";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-derived-events-XXXXXX");
    if (!cbm_mkdtemp(cache)) FAIL("could not create derived_from fixture cache");
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    const char *saved_maintain = getenv("CBM_MEMORY_AUTO_MAINTAIN");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    char *saved_maintain_copy = saved_maintain ? strdup(saved_maintain) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_setenv("CBM_MEMORY_AUTO_MAINTAIN", "0", 1);
    cbm_unsetenv("CBM_STAGE8_DERIVED_FROM_FAIL_AFTER");

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[CBM_SZ_4K];
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"DERIVED_BASE_MEMORY\",\"entity_key\":\"derived-base\","
             "\"payload\":{\"schema\":\"derived-events-v2\"}}",
             project);
    char *base_response = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(base_response, "\"status\":\"accepted\""));
    ASSERT_NULL(strstr(base_response, "materialized_relation"));
    ASSERT_NULL(strstr(base_response, "source_event_id"));
    char *base_id = mcp_text_field_dup(base_response, "item_id");
    ASSERT_NOT_NULL(base_id);
    free(base_response);

    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), 0);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts"), 1);

    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"MISSING_TARGET\",\"payload\":{},"
             "\"derived_from_memory_id\":\"missing-memory\"}", project);
    char *missing = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_NOT_NULL(strstr(missing, "\"isError\":true"));
    free(missing);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), 0);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts"), 1);

    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"scope\":\"global\",\"kind\":\"fact\","
             "\"content\":\"GLOBAL_REJECTED\",\"payload\":{},"
             "\"derived_from_memory_id\":\"%s\"}", project, base_id);
    char *global = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_NOT_NULL(strstr(global, "\"isError\":true"));
    free(global);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 1);

    ASSERT_TRUE(sqlite_exec_ok(db_path,
                               "UPDATE memory_item SET scope_project='other-project' "
                               "WHERE entity_key='derived-base'"));
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"CROSS_SCOPE\",\"payload\":{},"
             "\"derived_from_memory_id\":\"%s\"}", project, base_id);
    char *cross_scope = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_NOT_NULL(strstr(cross_scope, "\"isError\":true"));
    free(cross_scope);
    ASSERT_TRUE(sqlite_exec_ok(db_path,
                               "UPDATE memory_item SET scope_project='stage6-fixture-stage8-derived-v2-mcp' "
                               "WHERE entity_key='derived-base'"));

    const char *invalid_states[] = {"status='archived'", "deleted_at=1"};
    const char *restore_states[] = {"status='candidate'", "deleted_at=NULL"};
    for (size_t i = 0; i < 2; i++) {
        char update[512];
        snprintf(update, sizeof(update), "UPDATE memory_item SET %s WHERE id='%s'",
                 invalid_states[i], base_id);
        ASSERT_TRUE(sqlite_exec_ok(db_path, update));
        snprintf(args, sizeof(args),
                 "{\"project\":\"%s\",\"kind\":\"fact\","
                 "\"content\":\"INVALID_TARGET_STATE\",\"payload\":{},"
                 "\"derived_from_memory_id\":\"%s\"}", project, base_id);
        char *invalid = cbm_mcp_handle_tool(srv, "events", args);
        ASSERT_NOT_NULL(strstr(invalid, "\"isError\":true"));
        free(invalid);
        snprintf(update, sizeof(update), "UPDATE memory_item SET %s WHERE id='%s'",
                 restore_states[i], base_id);
        ASSERT_TRUE(sqlite_exec_ok(db_path, update));
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 1);
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), 1);
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), 0);
    }

    const char *failpoints[] = {"event", "item", "fts_prepare", "fts_insert", "edge"};
    for (size_t i = 0; i < sizeof(failpoints) / sizeof(failpoints[0]); i++) {
        cbm_setenv("CBM_STAGE8_DERIVED_FROM_FAIL_AFTER", failpoints[i], 1);
        snprintf(args, sizeof(args),
                 "{\"project\":\"%s\",\"kind\":\"fact\","
                 "\"content\":\"TRANSACTION_FAILPOINT\",\"payload\":{},"
                 "\"derived_from_memory_id\":\"%s\"}", project, base_id);
        char *failed = cbm_mcp_handle_tool(srv, "events", args);
        ASSERT_NOT_NULL(strstr(failed, "\"isError\":true"));
        free(failed);
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 1);
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), 1);
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), 0);
        ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts"), 1);
    }
    cbm_unsetenv("CBM_STAGE8_DERIVED_FROM_FAIL_AFTER");

    ASSERT_TRUE(sqlite_exec_ok(
        db_path,
        "CREATE TRIGGER fail_required_edge BEFORE INSERT ON memory_edge "
        "WHEN NEW.type='derived_from' BEGIN SELECT RAISE(ABORT,'edge failure'); END"));
    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"kind\":\"fact\","
             "\"content\":\"EDGE_INSERT_FAILURE\",\"payload\":{},"
             "\"derived_from_memory_id\":\"%s\"}", project, base_id);
    char *edge_failure = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_NOT_NULL(strstr(edge_failure, "\"isError\":true"));
    free(edge_failure);
    ASSERT_TRUE(sqlite_exec_ok(db_path, "DROP TRIGGER fail_required_edge"));
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), 0);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts"), 1);

    snprintf(args, sizeof(args),
             "{\"project\":\"%s\",\"scope\":\"project\",\"kind\":\"fact\","
             "\"content\":\"VALID_DERIVED_MEMORY\",\"payload\":{},"
             "\"derived_from_memory_id\":\"%s\"}", project, base_id);
    char *valid = cbm_mcp_handle_tool(srv, "events", args);
    ASSERT_TRUE(response_contains_json_fragment(valid, "\"materialized_relation\":true"));
    char *source_id = mcp_text_field_dup(valid, "item_id");
    char *event_id = mcp_text_field_dup(valid, "event_id");
    char *source_event_id = mcp_text_field_dup(valid, "source_event_id");
    char *edge_id = mcp_text_field_dup(valid, "edge_id");
    ASSERT_NOT_NULL(source_id);
    ASSERT_NOT_NULL(event_id);
    ASSERT_NOT_NULL(source_event_id);
    ASSERT_NOT_NULL(edge_id);
    ASSERT_STR_EQ(event_id, source_event_id);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event"), 2);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_item"), 2);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_fts"), 2);
    ASSERT_EQ(count_exact_derived_relation(db_path, edge_id, source_id, base_id, event_id), 1);

    free(valid);
    free(source_id);
    free(event_id);
    free(source_event_id);
    free(edge_id);
    free(base_id);
    cbm_mcp_server_free(srv);
    cbm_unsetenv("CBM_STAGE8_DERIVED_FROM_FAIL_AFTER");
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    if (saved_maintain_copy) cbm_setenv("CBM_MEMORY_AUTO_MAINTAIN", saved_maintain_copy, 1);
    else cbm_unsetenv("CBM_MEMORY_AUTO_MAINTAIN");
    free(saved_cache_copy);
    free(saved_maintain_copy);
    cbm_rmdir(cache);
    PASS();
}

/* Stage 7 must not let the legacy three-field feedback path mutate memory_item,
 * including after a cold retrieve has cached the project store. */
TEST(memory_feedback_legacy_three_field_fails_closed_after_cold_retrieve) {
    const char *project = "mcp-feedback-cold-reopen";
    const char *feedback_event_id = "feedback-cold-reopen-event";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-feedback-cold-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        FAIL("could not create isolated CBM_CACHE_DIR fixture");
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    bool seed_accepted = false;
    bool item_id_present = false;
    bool retrieve_found = false;
    bool feedback_recorded = false;
    bool feedback_event_matches = false;
    bool replayed = false;
    bool replay_event_matches = false;
    int feedback_rows = -1;
    char *item_id = NULL;
    char *seed_response = NULL;
    char *retrieve_response = NULL;
    char *feedback_response = NULL;
    char *replay_response = NULL;

    cbm_mcp_server_t *seed_srv = cbm_mcp_server_new(NULL);
    if (seed_srv) {
        char seed_args[CBM_SZ_4K];
        snprintf(seed_args, sizeof(seed_args),
                 "{\"project\":\"%s\",\"type\":\"r2_cold_feedback_test\"," 
                 "\"source\":\"test.mcp.cold_feedback\",\"user\":\"r2-test\"," 
                 "\"task\":\"r2-cold-feedback-regression\",\"kind\":\"fact\"," 
                 "\"layer\":\"semantic\",\"title\":\"Synthetic cold feedback fixture\"," 
                 "\"summary\":\"Synthetic-only fixture for writable handle upgrade.\"," 
                 "\"content\":\"SYNTHETIC_TEST_ONLY cold retrieve followed by useful feedback "
                 "must reopen the cached project memory database writable.\"," 
                 "\"entity_key\":\"r2-cold-feedback\",\"predicate\":\"returns_answer\"," 
                 "\"payload\":{\"schema\":\"r2-cold-feedback-test-v1\"},"
                 "\"confidence\":0.99,\"importance\":0.5,\"reusability\":0.5,"
                 "\"specificity\":0.99}",
                 project);
        seed_response = cbm_mcp_handle_tool(seed_srv, "events", seed_args);
        seed_accepted = response_contains_json_fragment(seed_response, "\"status\":\"accepted\"");
        item_id = mcp_text_field_dup(seed_response, "item_id");
        item_id_present = item_id && item_id[0] != '\0';
        cbm_mcp_server_free(seed_srv);
    }

    cbm_mcp_server_t *reader_srv = cbm_mcp_server_new(NULL);
    if (reader_srv && item_id_present) {
        char retrieve_args[1024];
        snprintf(retrieve_args, sizeof(retrieve_args),
                 "{\"project\":\"%s\",\"entity_key\":\"r2-cold-feedback\",\"limit\":5}",
                 project);
        retrieve_response = cbm_mcp_handle_tool(reader_srv, "memories_retrieve", retrieve_args);
        retrieve_found = retrieve_response && strstr(retrieve_response, item_id) != NULL;

        char feedback_args[CBM_SZ_4K];
        snprintf(feedback_args, sizeof(feedback_args),
                 "{\"project\":\"%s\",\"id\":\"%s\",\"feedback\":\"useful\"," 
                 "\"note\":\"synthetic cold-process feedback regression\"," 
                 "\"user\":\"r2-test\",\"event_id\":\"%s\"}",
                 project, item_id, feedback_event_id);
        feedback_response = cbm_mcp_handle_tool(reader_srv, "memory_feedback", feedback_args);
        feedback_recorded = response_contains_json_fragment(
            feedback_response, "\"code\":\"STAGE7_SCHEMA_REQUIRED\"");
        feedback_event_matches = feedback_response && strstr(feedback_response, feedback_event_id);

        replay_response = cbm_mcp_handle_tool(reader_srv, "memory_feedback", feedback_args);
        replayed = response_contains_json_fragment(
            replay_response, "\"code\":\"STAGE7_SCHEMA_REQUIRED\"");
        replay_event_matches = replay_response && strstr(replay_response, feedback_event_id);
    }
    if (reader_srv) {
        cbm_mcp_server_free(reader_srv);
    }

    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    feedback_rows = count_feedback_event_rows(db_path, feedback_event_id);

    if (!feedback_recorded || !replayed || feedback_rows != 0) {
        printf("\n    seed=%s\n    retrieve=%s\n    feedback=%s\n    replay=%s\n    rows=%d\n",
               seed_response ? seed_response : "<null>",
               retrieve_response ? retrieve_response : "<null>",
               feedback_response ? feedback_response : "<null>",
               replay_response ? replay_response : "<null>", feedback_rows);
    }

    free(seed_response);
    free(retrieve_response);
    free(feedback_response);
    free(replay_response);
    free(item_id);
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);

    ASSERT_TRUE(seed_accepted);
    ASSERT_TRUE(item_id_present);
    ASSERT_TRUE(retrieve_found);
    ASSERT_TRUE(feedback_recorded);
    ASSERT_TRUE(feedback_event_matches);
    ASSERT_TRUE(replayed);
    ASSERT_TRUE(replay_event_matches);
    ASSERT_EQ(feedback_rows, 0);
    PASS();
}

TEST(stage7_mcp_feedback_model_report_replay_and_conflict) {
    const char *project = "mcp-stage7-feedback";
    const char *request_id = "stage7-mcp-request-v1";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage7-feedback-XXXXXX");
    if (!cbm_mkdtemp(cache)) FAIL("could not create Stage 7 cache fixture");
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char seed_args[CBM_SZ_4K];
    snprintf(seed_args, sizeof(seed_args),
             "{\"project\":\"%s\",\"type\":\"stage7_mcp_test\","
             "\"source\":\"test.mcp.stage7\",\"kind\":\"fact\","
             "\"content\":\"SYNTHETIC_STAGE7_FEEDBACK_MARKER\","
             "\"entity_key\":\"stage7-feedback\",\"predicate\":\"verifies_health\","
             "\"payload\":{\"schema\":\"stage7-mcp-test-v1\"},\"confidence\":0.99}",
             project);
    char *seed = cbm_mcp_handle_tool(srv, "events", seed_args);
    ASSERT_TRUE(response_contains_json_fragment(seed, "\"status\":\"accepted\""));
    free(seed);
    char retrieve_args[1024];
    snprintf(retrieve_args, sizeof(retrieve_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage7-feedback\","
             "\"request_id\":\"%s\",\"limit\":5}", project, request_id);
    char *retrieve = cbm_mcp_handle_tool(srv, "memories_retrieve", retrieve_args);
    ASSERT_TRUE(response_contains_json_fragment(retrieve, "\"journal_status\":\"completed\""));
    free(retrieve);
    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    char candidate_sql[512];
    snprintf(candidate_sql, sizeof(candidate_sql),
             "SELECT id FROM retrieval_candidate WHERE session_id='%s' LIMIT 1", request_id);
    char hash_sql[512];
    snprintf(hash_sql, sizeof(hash_sql),
             "SELECT content_hash FROM retrieval_candidate WHERE session_id='%s' LIMIT 1",
             request_id);
    char *candidate_id = sqlite_text_dup(db_path, candidate_sql);
    char *content_hash = sqlite_text_dup(db_path, hash_sql);
    ASSERT_NOT_NULL(candidate_id);
    ASSERT_NOT_NULL(content_hash);
    char injection_args[CBM_SZ_4K];
    snprintf(injection_args, sizeof(injection_args),
             "{\"project\":\"%s\",\"event_id\":\"stage7-mcp-injection-v1\","
             "\"session_id\":\"%s\",\"candidate_id\":\"%s\",\"injection_index\":0,"
             "\"target\":\"context\",\"content_hash\":\"%s\",\"token_count\":8,"
             "\"classifier_status\":\"pass\",\"classification\":\"safe\"}",
             project, request_id, candidate_id, content_hash);
    char *injection = cbm_mcp_handle_tool(srv, "memory_observe_injection", injection_args);
    ASSERT_TRUE(response_contains_json_fragment(injection, "\"status\":\"injected\""));
    free(injection);
    char usage_args[CBM_SZ_4K];
    snprintf(usage_args, sizeof(usage_args),
             "{\"project\":\"%s\",\"event_id\":\"stage7-mcp-usage-v1\","
             "\"session_id\":\"%s\",\"candidate_id\":\"%s\","
             "\"injection_id\":\"stage7-mcp-injection-v1\",\"outcome\":\"uncertain\","
             "\"evidence_type\":\"model_self_report\",\"evidence_ref\":\"model:unit\","
             "\"evidence_hash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
             project, request_id, candidate_id);
    char *usage = cbm_mcp_handle_tool(srv, "memory_observe_usage", usage_args);
    ASSERT_TRUE(response_contains_json_fragment(usage, "\"status\":\"recorded\""));
    free(usage);
    const char *result_payload = "health task completed";
    const char *evidence_payload = "model reports success";
    char result_hash[65], evidence_hash[65];
    ASSERT_EQ(cbm_stage7_sha256_hex(result_payload, strlen(result_payload), result_hash),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_stage7_sha256_hex(evidence_payload, strlen(evidence_payload), evidence_hash),
              CBM_STORE_OK);
    char feedback_args[CBM_SZ_8K];
    snprintf(feedback_args, sizeof(feedback_args),
             "{\"project\":\"%s\",\"processing_mode\":\"observe_only\","
             "\"event_id\":\"stage7-mcp-feedback-v1\",\"task_id\":\"stage7-mcp-task-v1\","
             "\"task_type\":\"health_check\",\"session_id\":\"%s\","
             "\"candidate_id\":\"%s\",\"injection_id\":\"stage7-mcp-injection-v1\","
             "\"usage_id\":\"stage7-mcp-usage-v1\",\"result_id\":\"stage7-mcp-result-v1\","
             "\"result_type\":\"health_check\",\"result_status\":\"succeeded\","
             "\"result_ref\":\"model:health\",\"result_hash\":\"%s\","
             "\"result_payload\":\"%s\",\"evidence_id\":\"stage7-mcp-evidence-v1\","
             "\"evidence_trust\":\"model_self_report\",\"evidence_state\":\"valid\","
             "\"evidence_source\":\"model\",\"evidence_ref\":\"model:health\","
             "\"evidence_hash\":\"%s\",\"evidence_payload\":\"%s\","
             "\"action\":\"confirm\",\"algorithm_version\":\"stage7-reward-v1\","
             "\"config_version\":7}",
             project, request_id, candidate_id, result_hash, result_payload, evidence_hash,
             evidence_payload);
    char *feedback = cbm_mcp_handle_tool(srv, "memory_feedback", feedback_args);
    ASSERT_TRUE(response_contains_json_fragment(feedback, "\"status\":\"pending_confirmation\""));
    ASSERT_TRUE(response_contains_json_fragment(feedback, "\"final_reward\":0.0"));
    ASSERT_TRUE(response_contains_json_fragment(feedback, "\"long_term_state_written\":false"));
    char *replay = cbm_mcp_handle_tool(srv, "memory_feedback", feedback_args);
    ASSERT_TRUE(response_contains_json_fragment(replay, "\"status\":\"pending_confirmation\""));
    char conflict_args[CBM_SZ_8K];
    snprintf(conflict_args, sizeof(conflict_args), "%s", feedback_args);
    char *changed_status = strstr(conflict_args, "\"result_status\":\"succeeded\"");
    ASSERT_NOT_NULL(changed_status);
    memcpy(changed_status + strlen("\"result_status\":\""), "cancelled", 9);
    char *conflict = cbm_mcp_handle_tool(srv, "memory_feedback", conflict_args);
    ASSERT_TRUE(response_contains_json_fragment(conflict, "\"code\":\"IDEMPOTENCY_CONFLICT\""));
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM feedback_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM plasticity_audit_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_event WHERE type='feedback'"), 0);
    free(feedback); free(replay); free(conflict); free(candidate_id); free(content_hash);
    cbm_mcp_server_free(srv);
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(stage8_mcp_off_shadow_active_replay_and_conflict_fixture) {
    const char *project = "stage6-fixture-stage8-g8-candidate-v1";
    const char *request_id = "stage8-mcp-request-v1";
    const char *edge_id = "stage8-mcp-edge-v1";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage8-reinforcement-XXXXXX");
    if (!cbm_mkdtemp(cache)) FAIL("could not create Stage 8 MCP cache fixture");
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    const char *saved_guard = getenv("CBM_STAGE8_ACTIVE_FIXTURE");
    const char *saved_stage14_gate = getenv("CBM_STAGE14_PRODUCTION_GATE");
    const char *saved_stage14_mode = getenv("CBM_STAGE14_EVOLUTION_MODE");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    char *saved_guard_copy = saved_guard ? strdup(saved_guard) : NULL;
    char *saved_stage14_gate_copy =
        saved_stage14_gate ? strdup(saved_stage14_gate) : NULL;
    char *saved_stage14_mode_copy =
        saved_stage14_mode ? strdup(saved_stage14_mode) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_unsetenv("CBM_STAGE8_ACTIVE_FIXTURE");

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char seed_args[CBM_SZ_4K];
    snprintf(seed_args, sizeof(seed_args),
             "{\"project\":\"%s\",\"type\":\"stage8_seed\"," 
             "\"source\":\"test.mcp.stage8\",\"kind\":\"fact\"," 
             "\"content\":\"SYNTHETIC_STAGE8_REINFORCEMENT_SEED\"," 
             "\"entity_key\":\"stage8-reinforcement\",\"predicate\":\"anchors\"," 
             "\"payload\":{\"schema\":\"stage8-mcp-v1\"},\"confidence\":0.99}", project);
    char *seed_response = cbm_mcp_handle_tool(srv, "events", seed_args);
    ASSERT_TRUE(response_contains_json_fragment(seed_response, "\"status\":\"accepted\""));
    char *seed_id = mcp_text_field_dup(seed_response, "item_id");
    ASSERT_NOT_NULL(seed_id);
    free(seed_response);

    char peer_args[CBM_SZ_4K];
    snprintf(peer_args, sizeof(peer_args),
             "{\"project\":\"%s\",\"type\":\"stage8_peer\"," 
             "\"source\":\"test.mcp.stage8\",\"kind\":\"fact\"," 
             "\"content\":\"SYNTHETIC_STAGE8_REINFORCEMENT_PEER\"," 
             "\"entity_key\":\"stage8-peer\",\"predicate\":\"supports\"," 
             "\"payload\":{\"schema\":\"stage8-mcp-v1\"},\"confidence\":0.99}", project);
    char *peer_response = cbm_mcp_handle_tool(srv, "events", peer_args);
    ASSERT_TRUE(response_contains_json_fragment(peer_response, "\"status\":\"accepted\""));
    char *peer_id = mcp_text_field_dup(peer_response, "item_id");
    ASSERT_NOT_NULL(peer_id);
    free(peer_response);

    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    ASSERT_TRUE(insert_stage6_test_edge(db_path, edge_id, seed_id, peer_id, "supports"));

    char retrieve_args[CBM_SZ_1K];
    snprintf(retrieve_args, sizeof(retrieve_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage8-reinforcement\"," 
             "\"request_id\":\"%s\",\"limit\":5,\"activation_mode\":\"shadow\"}",
             project, request_id);
    char *retrieve = cbm_mcp_handle_tool(srv, "memories_retrieve", retrieve_args);
    ASSERT_TRUE(response_contains_json_fragment(retrieve, "\"journal_status\":\"completed\""));
    free(retrieve);

    char candidate_sql[CBM_SZ_1K];
    snprintf(candidate_sql, sizeof(candidate_sql),
             "SELECT id FROM retrieval_candidate WHERE session_id='%s' AND memory_item_id='%s'",
             request_id, seed_id);
    char hash_sql[CBM_SZ_1K];
    snprintf(hash_sql, sizeof(hash_sql),
             "SELECT content_hash FROM retrieval_candidate WHERE session_id='%s' AND memory_item_id='%s'",
             request_id, seed_id);
    char *candidate_id = sqlite_text_dup(db_path, candidate_sql);
    char *content_hash = sqlite_text_dup(db_path, hash_sql);
    ASSERT_NOT_NULL(candidate_id);
    ASSERT_NOT_NULL(content_hash);
    char peer_content_hash[65];
    ASSERT_EQ(cbm_stage7_sha256_hex("SYNTHETIC_STAGE8_REINFORCEMENT_PEER",
                                    strlen("SYNTHETIC_STAGE8_REINFORCEMENT_PEER"),
                                    peer_content_hash),
              CBM_STORE_OK);
    char visit_sql[CBM_SZ_8K];
    snprintf(visit_sql, sizeof(visit_sql),
             "INSERT INTO retrieval_candidate(id,session_id,source_store_kind,source_store_id,"
             "memory_item_id,content_hash,aggregate_score,aggregate_rank,decision_status,"
             "decision_reason,created_at) VALUES('stage8-mcp-peer-candidate-v1','%s','project',"
             "'%s','%s','%s',0.8,2,'selected','synthetic_stage8_visit',"
             "'2026-07-16T00:00:00.000+08:00');"
             "INSERT INTO retrieval_edge_visit(id,session_id,from_candidate_id,to_candidate_id,"
             "memory_edge_id,relation_type,hop_depth,activation_in,activation_out,visit_status,"
             "created_at) VALUES('stage8-mcp-edge-visit-v1','%s','%s',"
             "'stage8-mcp-peer-candidate-v1','%s','supports',1,1.0,0.8,'visited',"
             "'2026-07-16T00:00:00.000+08:00');",
             request_id, project, peer_id, peer_content_hash, request_id, candidate_id, edge_id);
    ASSERT_TRUE(sqlite_exec_ok(db_path, visit_sql));
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM retrieval_edge_visit"), 1);

    char injection_args[CBM_SZ_4K];
    snprintf(injection_args, sizeof(injection_args),
             "{\"project\":\"%s\",\"event_id\":\"stage8-mcp-injection-v1\"," 
             "\"session_id\":\"%s\",\"candidate_id\":\"%s\",\"injection_index\":0," 
             "\"target\":\"context\",\"content_hash\":\"%s\",\"token_count\":8," 
             "\"classifier_status\":\"pass\",\"classification\":\"safe\"}",
             project, request_id, candidate_id, content_hash);
    char *injection = cbm_mcp_handle_tool(srv, "memory_observe_injection", injection_args);
    ASSERT_TRUE(response_contains_json_fragment(injection, "\"status\":\"injected\""));
    free(injection);

    char usage_args[CBM_SZ_4K];
    snprintf(usage_args, sizeof(usage_args),
             "{\"project\":\"%s\",\"event_id\":\"stage8-mcp-usage-v1\"," 
             "\"session_id\":\"%s\",\"candidate_id\":\"%s\"," 
             "\"injection_id\":\"stage8-mcp-injection-v1\",\"outcome\":\"used\"," 
             "\"evidence_type\":\"external_verified\",\"evidence_ref\":\"test:stage8\"," 
             "\"evidence_hash\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}",
             project, request_id, candidate_id);
    char *usage = cbm_mcp_handle_tool(srv, "memory_observe_usage", usage_args);
    ASSERT_TRUE(response_contains_json_fragment(usage, "\"status\":\"recorded\""));
    free(usage);

    const char *result_payload = "stage8 fixture passed";
    const char *evidence_payload = "stage8 deterministic evidence";
    char result_hash[65], evidence_hash[65];
    ASSERT_EQ(cbm_stage7_sha256_hex(result_payload, strlen(result_payload), result_hash),
              CBM_STORE_OK);
    ASSERT_EQ(cbm_stage7_sha256_hex(evidence_payload, strlen(evidence_payload), evidence_hash),
              CBM_STORE_OK);
    char feedback_args[CBM_SZ_8K];
    snprintf(feedback_args, sizeof(feedback_args),
             "{\"project\":\"%s\",\"processing_mode\":\"observe_only\"," 
             "\"event_id\":\"stage8-mcp-feedback-v1\",\"task_id\":\"stage8-mcp-task-v1\"," 
             "\"task_type\":\"test\",\"session_id\":\"%s\",\"candidate_id\":\"%s\"," 
             "\"injection_id\":\"stage8-mcp-injection-v1\",\"usage_id\":\"stage8-mcp-usage-v1\"," 
             "\"result_id\":\"stage8-mcp-result-v1\",\"result_type\":\"test\"," 
             "\"result_status\":\"succeeded\",\"result_ref\":\"test-results/stage8-mcp.log\"," 
             "\"result_hash\":\"%s\",\"result_payload\":\"%s\"," 
             "\"evidence_id\":\"stage8-mcp-evidence-v1\",\"evidence_trust\":\"external_verified\"," 
             "\"evidence_state\":\"valid\",\"evidence_source\":\"test\"," 
             "\"evidence_ref\":\"test-results/stage8-mcp.log\",\"evidence_hash\":\"%s\"," 
             "\"evidence_payload\":\"%s\",\"action\":\"confirm\",\"edge_id\":\"%s\"," 
             "\"algorithm_version\":\"stage7-reward-v1\",\"config_version\":7}",
             project, request_id, candidate_id, result_hash, result_payload, evidence_hash,
             evidence_payload, edge_id);
    char *feedback = cbm_mcp_handle_tool(srv, "memory_feedback", feedback_args);
    ASSERT_TRUE(response_contains_json_fragment(feedback, "\"status\":\"attributed\""));
    ASSERT_TRUE(response_contains_json_fragment(feedback, "\"final_reward\":0.75"));
    free(feedback);

    char off_args[CBM_SZ_1K];
    snprintf(off_args, sizeof(off_args),
             "{\"project\":\"%s\",\"mode\":\"off\"," 
             "\"algorithm_version\":\"stage8-edge-reinforcement-v1\",\"config_version\":1}",
             project);
    char *off = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", off_args);
    ASSERT_TRUE(response_contains_json_fragment(off, "\"mode\":\"off\""));
    ASSERT_FALSE(sqlite_table_exists(db_path, "edge_contribution_event"));

    char shadow_args[CBM_SZ_1K];
    snprintf(shadow_args, sizeof(shadow_args),
             "{\"project\":\"%s\",\"mode\":\"shadow\"," 
             "\"algorithm_version\":\"stage8-edge-reinforcement-v1\",\"config_version\":1}",
             project);
    char *shadow1 = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", shadow_args);
    char *shadow2 = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", shadow_args);
    char *shadow3 = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", shadow_args);
    ASSERT_STR_EQ(shadow1, shadow2);
    ASSERT_STR_EQ(shadow2, shadow3);
    ASSERT_TRUE(response_contains_json_fragment(shadow1, "\"pheromone_ppm\":1045000"));
    ASSERT_TRUE(response_contains_json_fragment(shadow1, "\"long_term_state_written\":false"));
    ASSERT_FALSE(sqlite_table_exists(db_path, "edge_contribution_event"));

    char active_args[CBM_SZ_1K];
    snprintf(active_args, sizeof(active_args),
             "{\"project\":\"%s\",\"mode\":\"active\"," 
             "\"algorithm_version\":\"stage8-edge-reinforcement-v1\",\"config_version\":1}",
             project);
    cbm_setenv("CBM_STAGE14_PRODUCTION_GATE", "1", 1);
    cbm_setenv("CBM_STAGE14_EVOLUTION_MODE", "active", 1);
    char *blocked = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", active_args);
    ASSERT_TRUE(response_contains_json_fragment(blocked, "\"code\":\"ACTIVE_FIXTURE_GUARD\""));
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"production_state_written\":false"));
    ASSERT_FALSE(sqlite_table_exists(db_path, "edge_contribution_event"));
    if (saved_stage14_gate_copy)
        cbm_setenv("CBM_STAGE14_PRODUCTION_GATE", saved_stage14_gate_copy, 1);
    else cbm_unsetenv("CBM_STAGE14_PRODUCTION_GATE");
    if (saved_stage14_mode_copy)
        cbm_setenv("CBM_STAGE14_EVOLUTION_MODE", saved_stage14_mode_copy, 1);
    else cbm_unsetenv("CBM_STAGE14_EVOLUTION_MODE");
    free(saved_stage14_gate_copy);
    free(saved_stage14_mode_copy);

    cbm_setenv("CBM_STAGE8_ACTIVE_FIXTURE", "1", 1);
    char *active = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", active_args);
    ASSERT_TRUE(response_contains_json_fragment(active, "\"recorded_count\":1"));
    ASSERT_TRUE(response_contains_json_fragment(active, "\"long_term_state_written\":true"));
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM edge_contribution_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM plastic_edge_state"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM edge_reinforcement_audit_event"), 1);
    char *replay = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", active_args);
    ASSERT_TRUE(response_contains_json_fragment(replay, "\"recorded_count\":0"));
    ASSERT_TRUE(response_contains_json_fragment(replay, "\"replayed_count\":1"));
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM edge_contribution_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM edge_reinforcement_audit_event"), 1);

    ASSERT_TRUE(sqlite_exec_ok(
        db_path,
        "DROP TRIGGER edge_contribution_no_update;"
        "CREATE TRIGGER edge_contribution_no_update BEFORE UPDATE ON edge_contribution_event "
        "WHEN 0 BEGIN SELECT 1; END;"
        "UPDATE edge_contribution_event SET canonical_payload_sha256="
        "'0000000000000000000000000000000000000000000000000000000000000000';"));
    char *conflict = cbm_mcp_handle_tool(srv, "memory_reinforcement_replay", active_args);
    ASSERT_TRUE(response_contains_json_fragment(conflict, "\"code\":\"IDEMPOTENCY_CONFLICT\""));
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM edge_contribution_event"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM plastic_edge_state"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM edge_reinforcement_audit_event"), 1);

    free(conflict); free(replay); free(active); free(blocked);
    free(shadow3); free(shadow2); free(shadow1); free(off);
    free(content_hash); free(candidate_id); free(peer_id); free(seed_id);
    cbm_mcp_server_free(srv);
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_cache_copy);
    if (saved_guard_copy) cbm_setenv("CBM_STAGE8_ACTIVE_FIXTURE", saved_guard_copy, 1);
    else cbm_unsetenv("CBM_STAGE8_ACTIVE_FIXTURE");
    free(saved_guard_copy);
    free(saved_cache_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(stage5_mcp_observe_only_candidate_injection_usage_loop) {
    const char *project = "mcp-stage5-observe-loop";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage5-observe-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        FAIL("could not create isolated Stage 5 cache fixture");
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char seed_args[CBM_SZ_4K];
    snprintf(seed_args, sizeof(seed_args),
             "{\"project\":\"%s\",\"type\":\"stage5_observe_test\","
             "\"source\":\"test.mcp.stage5\",\"kind\":\"fact\","
             "\"summary\":\"Synthetic observe-only recall marker.\","
             "\"content\":\"SYNTHETIC_STAGE5_OBSERVE_ONLY_MARKER\","
             "\"entity_key\":\"stage5-observe-loop\",\"predicate\":\"returns_answer\","
             "\"payload\":{\"schema\":\"stage5-observe-test-v1\"},"
             "\"confidence\":0.99,\"importance\":0.5,\"reusability\":0.5,"
             "\"specificity\":0.99}", project);
    char *seed = cbm_mcp_handle_tool(srv, "events", seed_args);
    ASSERT_TRUE(response_contains_json_fragment(seed, "\"status\":\"accepted\""));
    free(seed);

    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    int edges_before = count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge;");

    char retrieve_args[1024];
    snprintf(retrieve_args, sizeof(retrieve_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage5-observe-loop\","
             "\"request_id\":\"stage5-mcp-session-1\",\"limit\":5}", project);
    char *retrieve = cbm_mcp_handle_tool(srv, "memories_retrieve", retrieve_args);
    ASSERT_NOT_NULL(retrieve);
    char *retrieve_text = extract_text_content(retrieve);
    ASSERT_NOT_NULL(retrieve_text);
    yyjson_doc *retrieve_doc = yyjson_read(retrieve_text, strlen(retrieve_text), 0);
    ASSERT_NOT_NULL(retrieve_doc);
    yyjson_val *root = yyjson_doc_get_root(retrieve_doc);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(root, "retrieval_session_id")),
                  "stage5-mcp-session-1");
    ASSERT_TRUE(yyjson_get_bool(yyjson_obj_get(root, "untrusted_data")));
    yyjson_val *memories = yyjson_obj_get(root, "memories");
    ASSERT_TRUE(yyjson_is_arr(memories));
    ASSERT_GT(yyjson_arr_size(memories), 0);
    yyjson_val *memory = yyjson_arr_get_first(memories);
    const char *candidate = yyjson_get_str(yyjson_obj_get(memory, "candidate_id"));
    const char *provenance = yyjson_get_str(yyjson_obj_get(memory, "provenance_id"));
    const char *evidence = yyjson_get_str(yyjson_obj_get(memory, "evidence_id"));
    const char *content_hash = yyjson_get_str(yyjson_obj_get(memory, "content_hash"));
    ASSERT_NOT_NULL(candidate);
    ASSERT_NOT_NULL(provenance);
    ASSERT_NOT_NULL(evidence);
    ASSERT_NOT_NULL(content_hash);
    char *candidate_copy = strdup(candidate);
    char *content_hash_copy = strdup(content_hash);
    yyjson_doc_free(retrieve_doc);
    free(retrieve_text);
    free(retrieve);

    char injection_args[CBM_SZ_4K];
    snprintf(injection_args, sizeof(injection_args),
             "{\"project\":\"%s\",\"event_id\":\"stage5-mcp-injection-1\","
             "\"session_id\":\"stage5-mcp-session-1\",\"candidate_id\":\"%s\","
             "\"injection_index\":0,\"target\":\"assistant_context\","
             "\"content_hash\":\"%s\",\"token_count\":4,"
             "\"classifier_status\":\"pass\",\"classification\":\"safe\"}",
             project, candidate_copy, content_hash_copy);
    char *injection = cbm_mcp_handle_tool(srv, "memory_observe_injection", injection_args);
    ASSERT_TRUE(response_contains_json_fragment(injection, "\"status\":\"injected\""));
    free(injection);
    injection = cbm_mcp_handle_tool(srv, "memory_observe_injection", injection_args);
    ASSERT_TRUE(response_contains_json_fragment(injection, "\"status\":\"replayed\""));
    free(injection);

    char usage_args[CBM_SZ_4K];
    snprintf(usage_args, sizeof(usage_args),
             "{\"project\":\"%s\",\"event_id\":\"stage5-mcp-usage-1\","
             "\"session_id\":\"stage5-mcp-session-1\",\"candidate_id\":\"%s\","
             "\"injection_id\":\"stage5-mcp-injection-1\",\"outcome\":\"used\","
             "\"evidence_type\":\"deterministic_stub\","
             "\"evidence_ref\":\"synthetic-answer-match\","
             "\"evidence_hash\":\"xxh3-stage5-mcp-evidence\"}", project, candidate_copy);
    char *usage = cbm_mcp_handle_tool(srv, "memory_observe_usage", usage_args);
    ASSERT_TRUE(response_contains_json_fragment(usage, "\"status\":\"recorded\""));
    free(usage);
    usage = cbm_mcp_handle_tool(srv, "memory_observe_usage", usage_args);
    ASSERT_TRUE(response_contains_json_fragment(usage, "\"status\":\"replayed\""));
    free(usage);

    char blocked_args[CBM_SZ_4K];
    snprintf(blocked_args, sizeof(blocked_args),
             "{\"project\":\"%s\",\"event_id\":\"stage5-mcp-injection-blocked\","
             "\"session_id\":\"stage5-mcp-session-1\",\"candidate_id\":\"%s\","
             "\"injection_index\":1,\"target\":\"assistant_context\","
             "\"content_hash\":\"%s\",\"token_count\":4,"
             "\"classifier_status\":\"error\","
             "\"classification\":\"prompt_injection\"}",
             project, candidate_copy, content_hash_copy);
    char *blocked = cbm_mcp_handle_tool(srv, "memory_observe_injection", blocked_args);
    ASSERT_TRUE(response_contains_json_fragment(
        blocked, "\"code\":\"SECURITY_PROMPT_INJECTION_REJECTED\""));
    ASSERT_NOT_NULL(strstr(blocked, "\"isError\":true"));
    ASSERT_NULL(strstr(blocked, "stage5-mcp-injection-blocked"));
    free(blocked);

    cbm_mcp_server_free(srv);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM retrieval_session;"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM retrieval_candidate;"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM context_injection;"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_usage_attribution;"), 1);
    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge;"), edges_before);

    free(candidate_copy);
    free(content_hash_copy);
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

TEST(stage6_mcp_off_shadow_and_active_fixture_guards) {
    const char *project = "stage6-fixture-mcp-shadow";
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-stage6-shadow-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        FAIL("could not create isolated Stage 6 cache fixture");
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);
    cbm_unsetenv("CBM_STAGE6_ACTIVE_FIXTURE");

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char seed_args[CBM_SZ_4K];
    snprintf(seed_args, sizeof(seed_args),
             "{\"project\":\"%s\",\"type\":\"stage6_seed\"," 
             "\"source\":\"test.mcp.stage6\",\"kind\":\"fact\"," 
             "\"summary\":\"Stage 6 direct structured seed.\"," 
             "\"content\":\"STAGE6_MCP_DIRECT_SEED\"," 
             "\"entity_key\":\"stage6-mcp-seed\",\"predicate\":\"anchors\"," 
             "\"payload\":{\"schema\":\"stage6-shadow-v1\"}}", project);
    char *seed_response = cbm_mcp_handle_tool(srv, "events", seed_args);
    ASSERT_TRUE(response_contains_json_fragment(seed_response, "\"status\":\"accepted\""));
    char *seed_id = mcp_text_field_dup(seed_response, "item_id");
    ASSERT_NOT_NULL(seed_id);
    free(seed_response);

    char target_args[CBM_SZ_4K];
    snprintf(target_args, sizeof(target_args),
             "{\"project\":\"%s\",\"type\":\"stage6_target\"," 
             "\"source\":\"test.mcp.stage6\",\"kind\":\"decision\"," 
             "\"summary\":\"Indirect target without direct seed terms.\"," 
             "\"content\":\"ISOLATED_INDIRECT_RESULT_6A\"," 
             "\"entity_key\":\"stage6-mcp-target\",\"predicate\":\"decides\"," 
             "\"payload\":{\"schema\":\"stage6-shadow-v1\"}}", project);
    char *target_response = cbm_mcp_handle_tool(srv, "events", target_args);
    ASSERT_TRUE(response_contains_json_fragment(target_response, "\"status\":\"accepted\""));
    char *target_id = mcp_text_field_dup(target_response, "item_id");
    ASSERT_NOT_NULL(target_id);
    free(target_response);

    char db_path[CBM_SZ_4K];
    snprintf(db_path, sizeof(db_path), "%s/%s-memory.db", cache, project);
    ASSERT_TRUE(insert_stage6_test_edge(db_path, "stage6-mcp-edge", seed_id, target_id,
                                        "supports"));
    int edges_before = count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge;");

    char off_args[CBM_SZ_1K];
    snprintf(off_args, sizeof(off_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage6-mcp-seed\"," 
             "\"request_id\":\"stage6-mcp-off\",\"limit\":5}", project);
    char *off = cbm_mcp_handle_tool(srv, "memories_retrieve", off_args);
    ASSERT_NOT_NULL(off);
    char explicit_off_args[CBM_SZ_1K];
    snprintf(explicit_off_args, sizeof(explicit_off_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage6-mcp-seed\"," 
             "\"request_id\":\"stage6-mcp-off\",\"limit\":5," 
             "\"activation_mode\":\"off\"}", project);
    char *explicit_off = cbm_mcp_handle_tool(srv, "memories_retrieve", explicit_off_args);
    ASSERT_NOT_NULL(explicit_off);
    ASSERT_STR_EQ(off, explicit_off);
    ASSERT_NULL(strstr(off, "activation_shadow"));

    char shadow_args[CBM_SZ_1K];
    snprintf(shadow_args, sizeof(shadow_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage6-mcp-seed\"," 
             "\"request_id\":\"stage6-mcp-shadow\",\"limit\":5," 
             "\"activation_mode\":\"shadow\"}", project);
    char *shadow = cbm_mcp_handle_tool(srv, "memories_retrieve", shadow_args);
    ASSERT_NOT_NULL(shadow);
    char *shadow_text = extract_text_content(shadow);
    ASSERT_NOT_NULL(shadow_text);
    yyjson_doc *shadow_doc = yyjson_read(shadow_text, strlen(shadow_text), 0);
    ASSERT_NOT_NULL(shadow_doc);
    yyjson_val *root = yyjson_doc_get_root(shadow_doc);
    yyjson_val *activation = yyjson_obj_get(root, "activation_shadow");
    ASSERT_TRUE(yyjson_is_obj(activation));
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(activation, "status")), "completed");
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(activation, "session_id")),
                  "stage6-mcp-shadow");
    yyjson_val *candidates = yyjson_obj_get(activation, "candidates");
    ASSERT_TRUE(yyjson_is_arr(candidates));
    ASSERT_EQ(yyjson_arr_size(candidates), 1);
    yyjson_val *candidate = yyjson_arr_get_first(candidates);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(candidate, "item_id")), target_id);
    ASSERT_NOT_NULL(yyjson_get_str(yyjson_obj_get(candidate, "candidate_id")));
    ASSERT_NOT_NULL(yyjson_get_str(yyjson_obj_get(candidate, "path_id")));
    ASSERT_NOT_NULL(yyjson_get_str(yyjson_obj_get(candidate, "evidence_id")));
    yyjson_val *memories = yyjson_obj_get(root, "memories");
    ASSERT_EQ(yyjson_arr_size(memories), 1);
    ASSERT_STR_EQ(yyjson_get_str(yyjson_obj_get(yyjson_arr_get_first(memories), "id")), seed_id);
    yyjson_doc_free(shadow_doc);
    free(shadow_text);

    char active_args[CBM_SZ_1K];
    snprintf(active_args, sizeof(active_args),
             "{\"project\":\"%s\",\"entity_key\":\"stage6-mcp-seed\"," 
             "\"request_id\":\"stage6-mcp-active-blocked\",\"limit\":5," 
             "\"activation_mode\":\"active\"}", project);
    char *blocked = cbm_mcp_handle_tool(srv, "memories_retrieve", active_args);
    ASSERT_TRUE(response_contains_json_fragment(blocked, "\"isError\":true"));

    ASSERT_EQ(count_sql_rows(db_path, "SELECT COUNT(*) FROM memory_edge;"), edges_before);
    ASSERT_TRUE(!sqlite_table_exists(db_path, "plastic_edge_state") ||
                count_sql_rows(db_path, "SELECT COUNT(*) FROM plastic_edge_state;") == 0);
    free(blocked);
    free(shadow);
    free(off);
    free(explicit_off);
    free(seed_id);
    free(target_id);
    cbm_mcp_server_free(srv);
    cleanup_memory_project_db(cache, project);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    cbm_rmdir(cache);
    PASS();
}

/* ── TestSnippet_ExactQN ──────────────────────────────────────── */

TEST(snippet_exact_qn) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* Exact match should NOT have match_method */
    ASSERT_NULL(strstr(resp, "\"match_method\""));
    /* Enriched properties */
    ASSERT_NOT_NULL(strstr(resp, "\"signature\":\"func HandleRequest() error\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\":\"error\""));
    /* Caller/callee counts: 0 callers, 2 callees */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\":0"));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\":2"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_QNSuffix ─────────────────────────────────────── */

TEST(snippet_qn_suffix) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"main.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_UniqueShortName ──────────────────────────────── */

TEST(snippet_unique_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "ProcessOrder" is unique — suffix tier matches (QN ends with .ProcessOrder) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"ProcessOrder\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"ProcessOrder\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NameTier ─────────────────────────────────────── */

TEST(snippet_name_tier) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "HandleRequest" — suffix tier finds it (QN ends with .HandleRequest) */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"name\":\"HandleRequest\""));
    ASSERT_NOT_NULL(strstr(resp, "\"match_method\":\"suffix\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AmbiguousShortName ───────────────────────────── */

TEST(snippet_ambiguous_short_name) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" matches 2 nodes — should return suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NOT_NULL(strstr(resp, "\"message\""));
    ASSERT_NOT_NULL(strstr(resp, "\"suggestions\""));
    /* Must NOT have "error" key */
    ASSERT_NULL(strstr(resp, "\"error\""));
    /* Must NOT have "source" */
    ASSERT_NULL(strstr(resp, "\"source\""));
    /* Should have at least 2 suggestions with qualified_name */
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.server.Run"));
    ASSERT_NOT_NULL(strstr(resp, "test-project.cmd.worker.Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_NotFound ─────────────────────────────────────── */

TEST(snippet_not_found) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp = call_snippet(srv, "{\"qualified_name\":\"CompletelyNonexistentFunctionXYZ123\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should return error or suggestions */
    ASSERT_TRUE(strstr(resp, "not found") || strstr(resp, "suggestions"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzySuggestions ─────────────────────────────── */

TEST(snippet_fuzzy_suggestions) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Handle" is not an exact QN or suffix — should get not-found guidance */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Handle\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should guide user to search_graph */
    ASSERT_NOT_NULL(strstr(resp, "search_graph"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_EnrichedProperties ───────────────────────────── */

TEST(snippet_enriched_properties) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"signature\""));
    ASSERT_NOT_NULL(strstr(resp, "\"return_type\""));
    ASSERT_NOT_NULL(strstr(resp, "\"is_exported\":true"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_FuzzyLastSegment ─────────────────────────────── */

TEST(snippet_fuzzy_last_segment) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "auth.handlers.HandleRequest" — suffix match should find HandleRequest */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"auth.handlers.HandleRequest\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Should either find it via suffix or guide to search_graph */
    ASSERT_TRUE(strstr(resp, "HandleRequest") != NULL || strstr(resp, "search_graph") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Default ──────────────────────────── */

TEST(snippet_auto_resolve_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" is ambiguous (2 candidates). Without auto_resolve → suggestions */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"status\":\"ambiguous\""));
    ASSERT_NULL(strstr(resp, "\"source\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_AutoResolve_Enabled ──────────────────────────── */

TEST(snippet_auto_resolve_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    /* "Run" — suffix match should find candidates or guide to search */
    char *resp = call_snippet(srv, "{\"qualified_name\":\"Run\","
                                   "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* "Run" matches multiple nodes via suffix → should get suggestions or source */
    ASSERT_TRUE(strstr(resp, "Run") != NULL);
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Default ─────────────────────── */

TEST(snippet_include_neighbors_default) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    /* Without include_neighbors → NO caller_names/callee_names */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    ASSERT_NULL(strstr(resp, "\"callee_names\""));
    /* But should still have counts */
    ASSERT_NOT_NULL(strstr(resp, "\"callers\""));
    ASSERT_NOT_NULL(strstr(resp, "\"callees\""));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_IncludeNeighbors_Enabled ─────────────────────── */

TEST(snippet_include_neighbors_enabled) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char *resp =
        call_snippet(srv, "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                          "\"include_neighbors\":true,\"project\":\"test-project\"}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"source\""));
    /* HandleRequest has 0 callers → no caller_names array */
    ASSERT_NULL(strstr(resp, "\"caller_names\""));
    /* HandleRequest has 2 callees: ProcessOrder and Run */
    ASSERT_NOT_NULL(strstr(resp, "\"callee_names\""));
    ASSERT_NOT_NULL(strstr(resp, "ProcessOrder"));
    ASSERT_NOT_NULL(strstr(resp, "Run"));
    free(resp);

    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ── TestSnippet_SourceInvalidUtf8 ────────────────────────────── */

TEST(snippet_source_invalid_utf8) {
    char tmp[256];
    cbm_mcp_server_t *srv = setup_snippet_server(tmp, sizeof(tmp));
    ASSERT_NOT_NULL(srv);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/project/main.go", tmp);
    FILE *fp = fopen(src_path, "wb");
    ASSERT_NOT_NULL(fp);
    const unsigned char source[] = {
        'p',  'a',  'c', 'k', 'a', 'g',  'e',  ' ',  'm',  'a',  'i',  'n', '\n', '\n',
        'f',  'u',  'n', 'c', ' ', 'H',  'a',  'n',  'd',  'l',  'e',  'R', 'e',  'q',
        'u',  'e',  's', 't', '(', ')',  ' ',  'e',  'r',  'r',  'o',  'r', ' ',  '{',
        '\n', '\t', '/', '/', ' ', 0xC0, 0xD4, 0xB7, 0xC2, '\n', '\t', 'r', 'e',  't',
        'u',  'r',  'n', ' ', 'n', 'i',  'l',  '\n', '}',  '\n'};
    ASSERT_EQ(fwrite(source, 1, sizeof(source), fp), sizeof(source));
    ASSERT_EQ(fclose(fp), 0);

    char *raw =
        cbm_mcp_handle_tool(srv, "get_code_snippet",
                            "{\"qualified_name\":\"test-project.cmd.server.main.HandleRequest\","
                            "\"project\":\"test-project\"}");
    ASSERT_TRUE(is_valid_json_response(raw));
    char *resp = extract_text_content(raw);
    ASSERT_NOT_NULL(resp);
    ASSERT_TRUE(is_valid_json_response(resp));
    ASSERT_NULL(strstr(resp, "\xC0\xD4"));
    ASSERT_NOT_NULL(strstr(resp, "HandleRequest"));
    ASSERT_NOT_NULL(strstr(resp, "return nil"));
    ASSERT_TRUE(snippet_source_has_replacement(resp));

    free(resp);
    free(raw);
    cbm_mcp_server_free(srv);
    cleanup_snippet_dir(tmp);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  JSON-RPC PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(jsonrpc_parse_empty_string) {
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_jsonrpc_field) {
    /* jsonrpc field absent — parser defaults to "2.0" if method present */
    const char *line = "{\"id\":1,\"method\":\"initialize\",\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(req.jsonrpc, "2.0");
    ASSERT_STR_EQ(req.method, "initialize");
    ASSERT_TRUE(req.has_id);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_missing_method) {
    /* method is required — should fail */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":1,\"params\":{}}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_string_id) {
    /* JSON-RPC §4: string and numeric ids are distinct. A string id is
     * preserved verbatim (issue #253), never coerced to a number. */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":\"99\",\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(req.has_id);
    ASSERT_NOT_NULL(req.id_str);
    ASSERT_STR_EQ(req.id_str, "99");
    ASSERT_STR_EQ(req.method, "tools/list");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_no_params) {
    /* Request with no params field — params_raw should be NULL */
    const char *line = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/list\"}";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_NULL(req.params_raw);
    ASSERT_EQ(req.id, 5);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_extra_whitespace) {
    /* Leading/trailing whitespace and internal spacing in JSON */
    const char *line = "  { \"jsonrpc\" : \"2.0\" , \"id\" : 7 , \"method\" : \"ping\" }  ";
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse(line, &req);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(req.id, 7);
    ASSERT_STR_EQ(req.method, "ping");
    cbm_jsonrpc_request_free(&req);
    PASS();
}

TEST(jsonrpc_parse_array_not_object) {
    /* JSON array at root — not a valid JSON-RPC request */
    cbm_jsonrpc_request_t req = {0};
    int rc = cbm_jsonrpc_parse("[1,2,3]", &req);
    ASSERT_EQ(rc, -1);
    cbm_jsonrpc_request_free(&req);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  ARGUMENT EXTRACTION — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(mcp_get_string_arg_empty_json) {
    /* Empty JSON string — yyjson_read fails → NULL */
    char *val = cbm_mcp_get_string_arg("", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_empty_object) {
    /* Valid JSON with no keys → NULL for any key */
    char *val = cbm_mcp_get_string_arg("{}", "key");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_string_arg_nested_value) {
    /* Value is an object, not a string → should return NULL */
    const char *args = "{\"config\":{\"nested\":true},\"name\":\"hello\"}";
    char *val = cbm_mcp_get_string_arg(args, "config");
    ASSERT_NULL(val); /* not a string type */
    val = cbm_mcp_get_string_arg(args, "name");
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "hello");
    free(val);
    PASS();
}

TEST(mcp_get_string_arg_int_value) {
    /* Value is an integer, not a string → NULL */
    char *val = cbm_mcp_get_string_arg("{\"count\":42}", "count");
    ASSERT_NULL(val);
    PASS();
}

TEST(mcp_get_int_arg_empty_json) {
    int val = cbm_mcp_get_int_arg("", "key", 99);
    ASSERT_EQ(val, 99);
    PASS();
}

TEST(mcp_get_int_arg_string_value) {
    /* Value is a string, not int → should return default */
    int val = cbm_mcp_get_int_arg("{\"limit\":\"ten\"}", "limit", 5);
    ASSERT_EQ(val, 5);
    PASS();
}

TEST(mcp_get_int_arg_bool_value) {
    /* Value is a bool, not int → default */
    int val = cbm_mcp_get_int_arg("{\"flag\":true}", "flag", -1);
    ASSERT_EQ(val, -1);
    PASS();
}

TEST(mcp_get_bool_arg_empty_json) {
    bool val = cbm_mcp_get_bool_arg("", "key");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_bool_arg_int_value) {
    /* Value is int 1, not bool → should return false */
    bool val = cbm_mcp_get_bool_arg("{\"flag\":1}", "flag");
    ASSERT_FALSE(val);
    PASS();
}

TEST(mcp_get_tool_name_empty_json) {
    char *name = cbm_mcp_get_tool_name("");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_tool_name_missing_name) {
    char *name = cbm_mcp_get_tool_name("{\"arguments\":{}}");
    ASSERT_NULL(name);
    PASS();
}

TEST(mcp_get_arguments_empty_json) {
    char *args = cbm_mcp_get_arguments("");
    ASSERT_NULL(args);
    PASS();
}

TEST(mcp_get_arguments_no_arguments_key) {
    /* No "arguments" key → returns "{}" */
    char *args = cbm_mcp_get_arguments("{\"name\":\"tool\"}");
    ASSERT_NOT_NULL(args);
    ASSERT_STR_EQ(args, "{}");
    free(args);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  FILE URI PARSING — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(parse_file_uri_http_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("http://example.com/path", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_ftp_scheme) {
    char path[256];
    ASSERT_FALSE(cbm_parse_file_uri("ftp://server/file.txt", path, sizeof(path)));
    ASSERT_STR_EQ(path, "");
    PASS();
}

TEST(parse_file_uri_buffer_too_small) {
    char path[5]; /* only 5 bytes — path gets truncated */
    ASSERT_TRUE(cbm_parse_file_uri("file:///usr/local/bin", path, sizeof(path)));
    /* snprintf truncates to 4 chars + NUL */
    ASSERT_EQ(strlen(path), 4);
    ASSERT_STR_EQ(path, "/usr");
    PASS();
}

TEST(parse_file_uri_spaces_in_path) {
    char path[256];
    ASSERT_TRUE(cbm_parse_file_uri("file:///home/user/my%20project", path, sizeof(path)));
    /* Raw percent-encoding is preserved (not decoded) */
    ASSERT_STR_EQ(path, "/home/user/my%20project");
    PASS();
}

TEST(parse_file_uri_null_out_path) {
    /* NULL out_path — should not crash */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", NULL, 256));
    PASS();
}

TEST(parse_file_uri_zero_size) {
    char path[256] = "garbage";
    /* out_size=0 → should fail safely */
    ASSERT_FALSE(cbm_parse_file_uri("file:///tmp", path, 0));
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  SERVER HANDLE — EDGE CASES
 * ══════════════════════════════════════════════════════════════════ */

TEST(server_handle_invalid_json) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    char *resp = cbm_mcp_server_handle(srv, "this is not json at all");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    ASSERT_NOT_NULL(strstr(resp, "-32700")); /* Parse error */
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_empty_object) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* Valid JSON but no method field → parse error */
    char *resp = cbm_mcp_server_handle(srv, "{}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "\"error\""));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

TEST(server_handle_tools_call_missing_name) {
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);

    /* tools/call with no tool name in params */
    char *resp =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"tools/call\","
                                   "\"params\":{\"arguments\":{}}}");
    ASSERT_NOT_NULL(resp);
    /* Should return error about unknown/missing tool */
    ASSERT_NOT_NULL(strstr(resp, "\"id\":50"));
    ASSERT_TRUE(strstr(resp, "error") || strstr(resp, "isError") || strstr(resp, "unknown"));
    free(resp);

    cbm_mcp_server_free(srv);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  POLL/GETLINE FILE* BUFFERING FIX
 * ══════════════════════════════════════════════════════════════════ */

#ifndef _WIN32
#include <unistd.h>
#include <signal.h>

/* Signal handler used by alarm() to abort the test if it hangs */
static void alarm_handler(int sig) {
    (void)sig;
    /* Writing to stderr is async-signal-safe */
    const char msg[] = "FAIL: mcp_server_run_rapid_messages timed out (>5s)\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

TEST(mcp_server_run_rapid_messages) {
    /* Simulate a client sending initialize + notifications/initialized +
     * tools/list all at once (no delays), which exercises the FILE*
     * buffering fix: the first getline() over-reads kernel data into the
     * libc buffer; without the fix, subsequent poll() calls block for 60s.
     *
     * We use alarm(5) to abort the test process if the server hangs. */
    int fds[2];
    ASSERT_EQ(pipe(fds), 0);

    /* Write all 3 messages to the write end in one shot */
    const char *msgs = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                       "\"params\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{}}}\n"
                       "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}\n";
    ssize_t written = write(fds[1], msgs, strlen(msgs));
    ASSERT_TRUE(written > 0);
    close(fds[1]); /* EOF signals end of input to the server */

    FILE *in_fp = fdopen(fds[0], "r");
    ASSERT_NOT_NULL(in_fp);

    FILE *out_fp = tmpfile();
    ASSERT_NOT_NULL(out_fp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* Install alarm to fail the test if cbm_mcp_server_run blocks */
    signal(SIGALRM, alarm_handler);
    alarm(5);

    int rc = cbm_mcp_server_run(srv, in_fp, out_fp);

    alarm(0); /* cancel alarm */
    signal(SIGALRM, SIG_DFL);

    ASSERT_EQ(rc, 0);

    /* Verify both responses are present:
     *   id:1 — initialize response
     *   id:2 — tools/list response (notifications/initialized produces none)
     * and that the tools list payload is included. */
    rewind(out_fp);
    char buf[4096] = {0};
    size_t nread = fread(buf, 1, sizeof(buf) - 1, out_fp);
    ASSERT_TRUE(nread > 0);
    ASSERT_NOT_NULL(strstr(buf, "\"id\":1"));
    ASSERT_NOT_NULL(strstr(buf, "\"id\":2"));
    ASSERT_NOT_NULL(strstr(buf, "tools"));

    cbm_mcp_server_free(srv);
    fclose(out_fp);
    /* in_fp already EOF; fclose cleans up */
    fclose(in_fp);
    PASS();
}
#endif /* !_WIN32 */

/* Issue #235: passing an unrecognised project name to a tool crashed the
 * binary with a buffer overflow while building the "available_projects"
 * error list — collect_db_project_names overflowed projects[CBM_SZ_4K] via
 * an unsigned underflow on (out_sz - offset) once the listed names exceeded
 * the buffer. Fill a temp cache dir with enough long-named .db files to
 * exceed 4 KB, then hit the bad-project path. Under ASan a regression aborts
 * here; the fixed bounds-check keeps it clean and returns a normal error. */
#define ISSUE235_DBNAME(buf, dir, i)                                                         \
    snprintf((buf), sizeof(buf),                                                             \
             "%s/proj_%02d_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.db",                      \
             (dir), (i))
TEST(tool_bad_project_name_no_overflow_issue235) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* 40 * ~120-char names overflows the 4 KB available-projects buffer.
     * collect_db_project_names advertises each db's INTERNAL project name
     * (#704), so the fixture must hold valid dbs with long internal names —
     * not stub files — for the bounds-check path to actually be exercised. */
    enum { ISSUE235_N = 40 };
    for (int i = 0; i < ISSUE235_N; i++) {
        char name[512];
        ISSUE235_DBNAME(name, cache, i);
        char iname[256];
        snprintf(iname, sizeof(iname),
                 "proj_%02d_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 i);
        cbm_store_t *st = cbm_store_open_path(name);
        if (st) {
            cbm_store_upsert_project(st, iname, cache);
            cbm_store_close(st);
        }
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
             "\"search_graph\",\"arguments\":{\"label\":\"Function\","
             "\"project\":\"definitely-not-a-real-project-xyz\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));
    free(resp);
    cbm_mcp_server_free(srv);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    for (int i = 0; i < ISSUE235_N; i++) {
        char name[512];
        ISSUE235_DBNAME(name, cache, i);
        cbm_unlink(name);
        char side[540];
        snprintf(side, sizeof(side), "%s-wal", name);
        cbm_unlink(side);
        snprintf(side, sizeof(side), "%s-shm", name);
        cbm_unlink(side);
    }
    cbm_rmdir(cache);
    PASS();
}
#undef ISSUE235_DBNAME

/* Issue #235 (follow-up): with many long-named projects indexed,
 * collect_db_project_names overflowed projects[CBM_SZ_4K] and truncated the
 * LAST name MID-TOKEN, then clamped offset to out_sz-1 — emitting malformed,
 * unterminated JSON like
 *   ...,"available_projects":["a",...,"vjson_49_bbb],"count":50}
 * (unclosed string + unclosed array). build_project_list_error wrapped that
 * invalid body into the tool error, so a "project not found" reply was NOT
 * valid JSON once enough projects were indexed.
 *
 * Reproduce-first: fill an isolated cache dir with enough long INTERNAL-named
 * dbs to overflow the 4 KB buffer, hit the bad-project path, then assert the
 * ERROR BODY (the inner MCP text content) parses as valid JSON and that
 * available_projects is a JSON array whose length == count. RED on the
 * truncating code (yyjson_read returns NULL on the mid-token cut); GREEN after
 * the element-boundary fix, which only ever writes whole "name" tokens. */
#define BADPROJ_JSON_DBNAME(buf, dir, i)                                                      \
    snprintf((buf), sizeof(buf),                                                              \
             "%s/vjson_%02d_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" \
             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.db",                       \
             (dir), (i))
TEST(tool_bad_project_error_valid_json_issue235) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-badproj-vjson-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* 50 * ~120-char INTERNAL names >> 4 KB → the available_projects buffer
     * overflows and the last name is cut mid-token on the unfixed code. */
    enum { BADPROJ_N = 50 };
    for (int i = 0; i < BADPROJ_N; i++) {
        char name[512];
        BADPROJ_JSON_DBNAME(name, cache, i);
        char iname[256];
        snprintf(iname, sizeof(iname),
                 "vjson_%02d_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
                 "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                 i);
        cbm_store_t *st = cbm_store_open_path(name);
        if (st) {
            cbm_store_upsert_project(st, iname, cache);
            cbm_store_close(st);
        }
    }

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char *resp = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":"
             "\"search_graph\",\"arguments\":{\"label\":\"Function\","
             "\"project\":\"definitely-not-a-real-project-xyz\"}}}");
    ASSERT_NOT_NULL(resp);
    ASSERT_NOT_NULL(strstr(resp, "not found"));

    /* The inner MCP text content is the error body built by
     * build_project_list_error. Capture its validity BEFORE cleanup so a RED
     * failure still restores the environment. */
    char *body = extract_text_content(resp);
    bool body_valid = false;
    bool aps_ok = false; /* available_projects is an array whose len == count */
    if (body) {
        yyjson_doc *bdoc = yyjson_read(body, strlen(body), 0);
        if (bdoc) {
            body_valid = true;
            yyjson_val *broot = yyjson_doc_get_root(bdoc);
            yyjson_val *aps = yyjson_obj_get(broot, "available_projects");
            yyjson_val *cnt = yyjson_obj_get(broot, "count");
            if (aps && yyjson_is_arr(aps) && cnt && yyjson_is_int(cnt)) {
                aps_ok = (yyjson_arr_size(aps) == (size_t)yyjson_get_int(cnt));
            }
            yyjson_doc_free(bdoc);
        }
    }
    free(body);
    free(resp);
    cbm_mcp_server_free(srv);

    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    for (int i = 0; i < BADPROJ_N; i++) {
        char name[512];
        BADPROJ_JSON_DBNAME(name, cache, i);
        cbm_unlink(name);
        char side[540];
        snprintf(side, sizeof(side), "%s-wal", name);
        cbm_unlink(side);
        snprintf(side, sizeof(side), "%s-shm", name);
        cbm_unlink(side);
    }
    cbm_rmdir(cache);

    /* RED on the unfixed code: mid-token truncation → invalid JSON body. */
    ASSERT_TRUE(body_valid);
    ASSERT_TRUE(aps_ok);
    PASS();
}
#undef BADPROJ_JSON_DBNAME

/* ── #704: project resolution must key on the db's INTERNAL project name ──
 *
 * Issue #704: project resolution is registry-less and filename-addressed.
 * resolve_store() opens <cache>/<passed>.db and then requires the internal
 * `projects.name` row to equal the passed name; list_projects /
 * collect_db_project_names derive the advertised name from the .db FILENAME.
 * When a db's filename != its internal name (a legacy '.'-vs-'-' username
 * twin, or a copied/renamed file) it shows up in list_projects under the
 * filename, but every query returns "project not found" — node rows are
 * tagged with the INTERNAL name, so neither the filename nor the resolve
 * path lines up. The fix makes list + resolve both key on the INTERNAL name.
 *
 * Reproduce-first fixture in an isolated CBM_CACHE_DIR:
 *   - alpha704.db  : filename == internal name "alpha704"   (control / fast path)
 *   - gamma704.db  : internal name "beta704"                (DRIFT: built as
 *                    beta704.db then renamed → filename != internal name)
 *   - ghost704.db  : 0-byte file                            (ghost / unresolvable)
 *
 * RED on buggy code / GREEN on the fix:
 *   A. list_projects advertises "beta704" (internal), NOT "gamma704" (filename),
 *      and NOT "ghost704" (0-byte filtered).
 *   B. search_graph(project="beta704") resolves via the cache-dir scan and
 *      returns the node — not the "project not found" error.
 *   C. control project "alpha704" still resolves on the fast path.
 *   D. the 0-byte ghost is not resolvable.
 *   E. addressing the drifted db by its FILENAME ("gamma704") stays not-found
 *      (we key on the internal name, never the file on disk).
 */

/* Create a file-backed project db at <dir>/<filename> whose INTERNAL project
 * name is `internal` (which may differ from the filename), holding one
 * Function node named `fn`. Returns true on success. */
static bool issue704_make_db(const char *dir, const char *filename, const char *internal,
                             const char *fn) {
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    cbm_store_t *st = cbm_store_open_path(path);
    if (!st) {
        return false;
    }
    bool ok = (cbm_store_upsert_project(st, internal, dir) == CBM_STORE_OK);
    if (ok) {
        char qn[256];
        snprintf(qn, sizeof(qn), "%s.%s", internal, fn);
        cbm_node_t n = {0};
        n.project = internal;
        n.label = "Function";
        n.name = fn;
        n.qualified_name = qn;
        n.file_path = "main.go";
        n.start_line = 1;
        n.end_line = 2;
        ok = (cbm_store_upsert_node(st, &n) > 0);
    }
    cbm_store_close(st);
    return ok;
}

TEST(tool_resolve_store_by_internal_name_issue704) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-issue704-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        PASS(); /* skip if mkdtemp fails — not a #704 signal */
    }

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* (1) control: filename == internal name */
    ASSERT_TRUE(issue704_make_db(cache, "alpha704.db", "alpha704", "alphaFunc704"));

    /* (2) DRIFT: build beta704.db (internal "beta704") then rename the file to
     *     gamma704.db, so filename "gamma704" != internal "beta704". */
    ASSERT_TRUE(issue704_make_db(cache, "beta704.db", "beta704", "betaFunc704"));
    char beta_path[700];
    char gamma_path[700];
    snprintf(beta_path, sizeof(beta_path), "%s/beta704.db", cache);
    snprintf(gamma_path, sizeof(gamma_path), "%s/gamma704.db", cache);
    ASSERT_EQ(rename(beta_path, gamma_path), 0);

    /* (3) ghost: 0-byte db file */
    char ghost_path[700];
    snprintf(ghost_path, sizeof(ghost_path), "%s/ghost704.db", cache);
    FILE *gp = fopen(ghost_path, "w");
    ASSERT_NOT_NULL(gp);
    fclose(gp);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);

    /* ── A: list_projects reports INTERNAL names; filters the ghost ── */
    char *list =
        cbm_mcp_server_handle(srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                                   "\"params\":{\"name\":\"list_projects\",\"arguments\":{}}}");
    ASSERT_NOT_NULL(list);
    ASSERT_NOT_NULL(strstr(list, "alpha704")); /* control */
    ASSERT_NOT_NULL(strstr(list, "beta704"));  /* internal name of drifted db (RED before) */
    ASSERT_NULL(strstr(list, "gamma704"));     /* filename must NOT be advertised (RED before) */
    ASSERT_NULL(strstr(list, "ghost704"));     /* 0-byte ghost filtered (RED before) */
    free(list);

    /* ── B: the drifted project resolves by its INTERNAL name ──────── */
    char *q_beta = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"beta704\",\"name_pattern\":\"betaFunc704\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_beta);
    ASSERT_NOT_NULL(strstr(q_beta, "betaFunc704")); /* resolved + returned node (RED before) */
    ASSERT_NULL(strstr(q_beta, "not found"));       /* not the not-found error */
    free(q_beta);

    /* ── C: control project still resolves on the fast path ────────── */
    char *q_alpha = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"alpha704\",\"name_pattern\":\"alphaFunc704\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_alpha);
    ASSERT_NOT_NULL(strstr(q_alpha, "alphaFunc704"));
    free(q_alpha);

    /* ── D: the 0-byte ghost is NOT resolvable ─────────────────────── */
    char *q_ghost = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"ghost704\",\"name_pattern\":\".*\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_ghost);
    ASSERT_NOT_NULL(strstr(q_ghost, "not found"));
    free(q_ghost);

    /* ── E: addressing the drifted db by its FILENAME stays not-found ── */
    char *q_gamma = cbm_mcp_server_handle(
        srv, "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
             "\"params\":{\"name\":\"search_graph\",\"arguments\":{"
             "\"project\":\"gamma704\",\"name_pattern\":\".*\",\"limit\":5}}}");
    ASSERT_NOT_NULL(q_gamma);
    ASSERT_NOT_NULL(strstr(q_gamma, "not found"));
    free(q_gamma);

    cbm_mcp_server_free(srv);

    /* ── cleanup ───────────────────────────────────────────────────── */
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }
    char a_path[700];
    snprintf(a_path, sizeof(a_path), "%s/alpha704.db", cache);
    char corrupt_path[720];
    snprintf(corrupt_path, sizeof(corrupt_path), "%s.corrupt", ghost_path);
    cbm_unlink(a_path);
    cbm_unlink(gamma_path);
    cbm_unlink(ghost_path);
    cbm_unlink(corrupt_path); /* ghost may be quarantined by resolve_store */
    char side[740];
    snprintf(side, sizeof(side), "%s-wal", a_path);
    cbm_unlink(side);
    snprintf(side, sizeof(side), "%s-shm", a_path);
    cbm_unlink(side);
    snprintf(side, sizeof(side), "%s-wal", gamma_path);
    cbm_unlink(side);
    snprintf(side, sizeof(side), "%s-shm", gamma_path);
    cbm_unlink(side);
    cbm_rmdir(cache);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  QUERY STORE READ-ONLY  (data-integrity reproductions)
 *
 *  Bug: query tools resolve the project store via resolve_store() ->
 *  cbm_store_open_path_query(), which opens the DB SQLITE_OPEN_READWRITE
 *  and runs configure_pragmas() with the WRITE pragmas
 *  (journal_mode=WAL + wal_checkpoint + synchronous). Two consequences:
 *    (a) read-only query tools MUTATE the on-disk DB (write pragmas), and
 *    (b) query tools FAIL outright on a read-only DB file / filesystem
 *        (the READWRITE open returns CANTOPEN -> resolve_store NULL ->
 *        "project not found").
 *  Both tests below are written reproduce-first and are RED on the
 *  unfixed code, GREEN once query opens are READONLY with read-only
 *  pragmas.
 * ══════════════════════════════════════════════════════════════════ */

#define ROQ_PROJECT "cbm-roq-test"

/* Whole-file byte snapshot. Returns malloc'd buffer (caller frees) and
 * writes the length to *out_len. Returns NULL on failure. */
static unsigned char *roq_read_file_bytes(const char *path, long *out_len) {
    *out_len = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);
    unsigned char *buf = malloc((size_t)sz > 0 ? (size_t)sz : 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = sz;
    return buf;
}

static int roq_file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* ── (a) NO-MUTATION ──────────────────────────────────────────────────
 *
 * readonly_query_does_not_mutate_db
 *
 * Create a real project DB, convert it to rollback (DELETE) journal mode
 * on disk, snapshot its exact bytes, run search_graph through the server,
 * then re-snapshot. The buggy query path runs `PRAGMA journal_mode=WAL`,
 * which rewrites the file header (1,1 -> 2,2) and spawns a -wal sidecar —
 * so the snapshots differ. The fixed READONLY path runs no write pragma,
 * so the file is byte-identical.
 *
 * The DELETE-mode fixture is what makes the mutation OBSERVABLE: on an
 * already-WAL file `journal_mode=WAL` is a silent no-op, so we deliberately
 * stage the DB in rollback mode (the same technique repro_issue557 uses to
 * plant a deterministic trigger).
 *
 * WHY RED on unfixed code:
 *   journal_mode=WAL rewrites the header -> memcmp(before, after) != 0 and
 *   a -wal file is created while the cached store is open. Both assertions
 *   that demand "unchanged" fire.
 * ─────────────────────────────────────────────────────────────────── */
TEST(readonly_query_does_not_mutate_db) {
    char tmp_cache[512];
    snprintf(tmp_cache, sizeof(tmp_cache), "%s/cbm_roq_a_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp_cache)) {
        ASSERT_NOT_NULL(NULL); /* setup failure */
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", tmp_cache, 1);

    char db_path[700];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", tmp_cache, ROQ_PROJECT);
    char wal_path[730];
    char shm_path[730];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);

    /* Build the DB and flip it to rollback journal mode on disk. */
    cbm_store_t *setup = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(setup);
    ASSERT_EQ(cbm_store_upsert_project(setup, ROQ_PROJECT, "/tmp/roq"), CBM_STORE_OK);
    cbm_node_t node = {.project = ROQ_PROJECT,
                       .label = "Function",
                       .name = "ReadOnlyProbe",
                       .qualified_name = "roq.mod.ReadOnlyProbe",
                       .file_path = "mod.c"};
    ASSERT_TRUE(cbm_store_upsert_node(setup, &node) > 0);
    ASSERT_EQ(cbm_store_exec(setup, "PRAGMA journal_mode=DELETE;"), 0);
    cbm_store_close(setup);

    /* Snapshot BEFORE any query. */
    long before_len = 0;
    unsigned char *before = roq_read_file_bytes(db_path, &before_len);
    ASSERT_NOT_NULL(before);

    /* Run a query tool through the server (the resolve_store path). */
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\".*ReadOnlyProbe.*\"}",
             ROQ_PROJECT);
    char *resp = cbm_mcp_handle_tool(srv, "search_graph", args);

    /* Capture sidecar state WHILE the cached store is still open (the buggy
     * RW+WAL open creates -wal here; on close it would be removed again). */
    int wal_while_open = roq_file_exists(wal_path);
    int query_ok = (resp && strstr(resp, "ReadOnlyProbe") != NULL);
    int query_failed = (resp && (strstr(resp, "not found") || strstr(resp, "not indexed")));

    cbm_mcp_server_free(srv); /* closes the store; header change is persisted */

    long after_len = 0;
    unsigned char *after = roq_read_file_bytes(db_path, &after_len);

    int identical = (before && after && before_len == after_len &&
                     memcmp(before, after, (size_t)before_len) == 0);

    if (resp) {
        free(resp);
    }
    free(before);
    free(after);
    cbm_unlink(db_path);
    cbm_unlink(wal_path);
    cbm_unlink(shm_path);
    cbm_rmdir(tmp_cache);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }

    ASSERT_TRUE(query_ok);        /* read path ran and returned the node */
    ASSERT_FALSE(query_failed);   /* not the "project not found" path */
    ASSERT_TRUE(identical);       /* RED on buggy code: WAL pragma rewrote header */
    ASSERT_FALSE(wal_while_open); /* RED on buggy code: RW+WAL open spawned -wal */
    PASS();
}

/* ── (b) READ-ONLY FILESYSTEM ─────────────────────────────────────────
 *
 * readonly_query_succeeds_on_readonly_fs
 *
 * Create a real project DB (left in WAL journal mode, as the indexer
 * writes it), then chmod the CONTAINING DIRECTORY to 0555 (read-only) to
 * simulate a read-only mount / immutable media, then run search_graph.
 *
 * Note on why the directory (not just the file) must be read-only: SQLite's
 * unix VFS auto-downgrades a failed O_RDWR main-db open to O_RDONLY, so a
 * 0444 *file* alone does NOT surface the bug — the connection silently
 * becomes read-only and, with a writable dir, still creates the WAL -shm
 * and reads. The genuine read-only-FS symptom is the WAL write-pragma
 * (journal_mode=WAL) being unable to create the -shm/-wal sidecars in a
 * read-only directory.
 *
 * WHY RED on unfixed code:
 *   cbm_store_open_path_query() runs configure_pragmas(.., false) which
 *   executes `PRAGMA journal_mode = WAL`. In a read-only directory the WAL
 *   wal-index (-shm) cannot be created, so the pragma errors ->
 *   configure_pragmas fails -> the open returns NULL -> resolve_store()
 *   returns NULL -> the handler emits "project not found or not indexed".
 *
 * GREEN on fixed code:
 *   the READONLY open skips the WAL write-pragma; the plain READONLY open
 *   of a WAL-mode DB in a read-only dir still needs -shm, so it fails and
 *   the immutable-URI fallback (file:..?immutable=1) reads the main DB
 *   file directly and the query returns the node. (This is the test that
 *   exercises the immutable fallback path.)
 * ─────────────────────────────────────────────────────────────────── */
TEST(readonly_query_succeeds_on_readonly_fs) {
    char tmp_cache[512];
    snprintf(tmp_cache, sizeof(tmp_cache), "%s/cbm_roq_b_XXXXXX", cbm_tmpdir());
    if (!cbm_mkdtemp(tmp_cache)) {
        ASSERT_NOT_NULL(NULL); /* setup failure */
    }
    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", tmp_cache, 1);

    char db_path[700];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", tmp_cache, ROQ_PROJECT);
    char wal_path[730];
    char shm_path[730];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);

    /* Build the DB in its natural WAL journal mode and ensure it is cleanly
     * checkpointed (no -wal frames) so the immutable fallback can read all
     * data from the main file. */
    cbm_store_t *setup = cbm_store_open_path(db_path);
    ASSERT_NOT_NULL(setup);
    ASSERT_EQ(cbm_store_upsert_project(setup, ROQ_PROJECT, "/tmp/roq"), CBM_STORE_OK);
    cbm_node_t node = {.project = ROQ_PROJECT,
                       .label = "Function",
                       .name = "ReadOnlyProbe",
                       .qualified_name = "roq.mod.ReadOnlyProbe",
                       .file_path = "mod.c"};
    ASSERT_TRUE(cbm_store_upsert_node(setup, &node) > 0);
    (void)cbm_store_checkpoint(setup); /* fold WAL frames into the main file */
    cbm_store_close(setup);            /* clean close removes -wal/-shm */

    /* Make the containing directory read-only (simulate a read-only mount).
     * SQLite can still traverse + read files, but cannot create -shm/-wal. */
    ASSERT_EQ(chmod(tmp_cache, 0555), 0);

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    ASSERT_NOT_NULL(srv);
    char args[512];
    snprintf(args, sizeof(args), "{\"project\":\"%s\",\"name_pattern\":\".*ReadOnlyProbe.*\"}",
             ROQ_PROJECT);
    char *resp = cbm_mcp_handle_tool(srv, "search_graph", args);

    int query_ok = (resp && strstr(resp, "ReadOnlyProbe") != NULL);
    int query_failed = (resp && (strstr(resp, "not found") || strstr(resp, "not indexed")));

    if (resp) {
        free(resp);
    }
    cbm_mcp_server_free(srv);

    /* Restore write permission on the dir BEFORE unlink (cannot remove dir
     * entries while the directory is read-only). */
    chmod(tmp_cache, 0755);
    cbm_unlink(db_path);
    cbm_unlink(wal_path);
    cbm_unlink(shm_path);
    cbm_rmdir(tmp_cache);
    if (saved_copy) {
        cbm_setenv("CBM_CACHE_DIR", saved_copy, 1);
        free(saved_copy);
    } else {
        cbm_unsetenv("CBM_CACHE_DIR");
    }

    ASSERT_FALSE(query_failed); /* RED on buggy code: WAL pragma fails on RO dir */
    ASSERT_TRUE(query_ok);      /* RED on buggy code: no node returned */
    PASS();
}

#undef ROQ_PROJECT

/* ══════════════════════════════════════════════════════════════════
 *  #845 — supervisor gate must not wrap embedders of cbm_mcp_handle_tool
 * ══════════════════════════════════════════════════════════════════ */

/* Child-side check: index a tiny fixture and verify it ran IN-PROCESS.
 * Distinct exit codes so the parent can report the exact failure mode. */
enum {
    IDX845_OK = 0,
    IDX845_SPAWNED = 41,     /* a worker subprocess was spawned — the #845 bug */
    IDX845_NO_RESULT = 42,   /* handle_tool returned NULL */
    IDX845_NOT_INDEXED = 43, /* response lacks status=indexed */
};

static int idx845_index_inprocess_check(const char *repo_dir) {
    int spawns_before = cbm_index_supervisor_spawn_count();

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        return IDX845_NO_RESULT;
    }
    char args[1024];
    snprintf(args, sizeof(args), "{\"repo_path\":\"%s\",\"mode\":\"fast\"}", repo_dir);
    char *resp = cbm_mcp_handle_tool(srv, "index_repository", args);

    int code = IDX845_OK;
    if (cbm_index_supervisor_spawn_count() != spawns_before) {
        code = IDX845_SPAWNED;
    } else if (!resp) {
        code = IDX845_NO_RESULT;
    } else if (!response_contains_json_fragment(resp, "\"status\":\"indexed\"")) {
        code = IDX845_NOT_INDEXED;
    }
    free(resp);
    cbm_mcp_server_free(srv);
    return code;
}

TEST(index_supervisor_gate_requires_marked_host_issue845) {
    /* #845: index_repository via cbm_mcp_handle_tool from an EMBEDDER (this test
     * binary) must index IN-PROCESS even with CBM_INDEX_SUPERVISOR unset. The
     * supervisor gate may only wrap a process that called
     * cbm_index_supervisor_mark_host() — i.e. the real binary's main(). Before
     * the fix, should_wrap() was true for ANY embedder: the gate resolved the
     * CURRENT binary (this test runner!) and spawned
     * '<test-runner> cli --index-worker index_repository …', which a test binary
     * interprets as suite-filter args → it re-runs test suites in the child →
     * recursive spawn chains (observed 11-min hangs; kernel VM-map load during
     * the 2026-07-04 host panics).
     *
     * POSIX: run the call in a forked child under alarm(20) so the pre-fix
     * recursive behaviour cannot hang the runner; the child reports via exit
     * code. Windows: no fork — run in-process (safe once the gate is fixed; the
     * pre-fix redness is demonstrated on POSIX). */
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idx845-repo-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idx845-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    /* The point of the guard: NO kill switch. The gate itself must keep an
     * unmarked host in-process. Save + restore the ambient value. */
    const char *saved_sv = getenv("CBM_INDEX_SUPERVISOR");
    char *saved_sv_copy = saved_sv ? strdup(saved_sv) : NULL;
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def main():\n    return 'ok'\n", fp);
    fclose(fp);

    int code = -1;
    bool signalled = false;
    int sig = 0;
#ifdef _WIN32
    code = idx845_index_inprocess_check(tmp_dir);
#else
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(20); /* pre-fix spawn chain must die here, not hang the runner */
        _exit(idx845_index_inprocess_check(tmp_dir));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }
#endif

    /* Restore env BEFORE asserting so a red run doesn't leak state. */
    if (saved_sv_copy) {
        cbm_setenv("CBM_INDEX_SUPERVISOR", saved_sv_copy, 1);
        free(saved_sv_copy);
    } else {
        cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    }
    char *project = cbm_project_name_from_path(tmp_dir);
    cleanup_project_db(cache, project);
    free(project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => recursive spawn chain hang)\n", sig);
    } else if (code != IDX845_OK) {
        printf("    child exit code %d (41=worker spawned, 42=no result, 43=not indexed)\n", code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDX845_OK);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  #832 — background auto-index + watcher re-index must run in the
 *         supervised worker SUBPROCESS (RSS isolation)
 * ══════════════════════════════════════════════════════════════════ */

/* The long-lived server ran the full index pipeline in-process on two background
 * paths (session auto-index in mcp.c, watcher re-index in main.c). Worker-thread
 * mimalloc heaps abandon pages at thread exit and mimalloc v3
 * (page_reclaim_on_free=0) does not reclaim them when the main thread later frees
 * their blocks, so RSS ratchets across re-index cycles (#832). The fix routes both
 * paths through cbm_mcp_index_run_supervised_path() — the SAME supervised worker
 * subprocess the index_repository tool uses — so the child hands 100%% of its RSS
 * back to the OS on exit.
 *
 * This guard proves the ROUTING: on a supervisor-marked host with the kill switch
 * OFF, the shared entry the watcher/auto-index now call must (a) spawn a worker
 * child (cbm_index_supervisor_spawn_count() increases) and (b) actually index the
 * fixture (the worker child writes the Function node). RED on the unfixed
 * in-process routing: it calls cbm_pipeline_run directly, so spawn_count is
 * unchanged → IDX832_NO_SPAWN. */
enum {
    IDX832_OK = 0,
    IDX832_NO_SPAWN = 51,    /* spawn_count unchanged — routed in-process (RED) */
    IDX832_NULL_RESP = 52,   /* supervised entry degraded to NULL */
    IDX832_NOT_INDEXED = 53, /* response/store lacks the indexed Function node */
    IDX832_SERVER_FAIL = 54,
};

#ifndef _WIN32 /* helper used only by the POSIX fork harness below */
static int idx832_supervised_route_check(const char *repo_dir) {
    /* Become a supervisor host with the kill switch OFF — exactly the real MCP
     * server's state. Done in the FORKED CHILD only (see the harness) so the
     * parent test-runner's process-wide host mark stays clear and the #845
     * unmarked-embedder guard is unaffected. Bound the recovery loop + worker
     * quiet-timeout so a stuck child cannot run long under the fork+alarm net. */
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "1", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);

    int spawns_before = cbm_index_supervisor_spawn_count();
    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    int spawns_after = cbm_index_supervisor_spawn_count();

    if (spawns_after == spawns_before) {
        free(resp);
        return IDX832_NO_SPAWN; /* the discriminating assertion: RED in-process */
    }
    if (!resp) {
        return IDX832_NULL_RESP;
    }
    bool indexed = response_contains_json_fragment(resp, "\"status\":\"indexed\"");
    free(resp);
    if (!indexed) {
        return IDX832_NOT_INDEXED;
    }

    /* Store-level proof the worker child did real work: the Function node it wrote
     * must be queryable from a fresh server reading the DB the child produced. */
    char *project = cbm_project_name_from_path(repo_dir);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (!srv) {
        free(project);
        return IDX832_SERVER_FAIL;
    }
    int code = IDX832_OK;
    if (project) {
        char q[512];
        snprintf(q, sizeof(q),
                 "{\"project\":\"%s\",\"name_pattern\":\"idx832_fn\",\"label\":\"Function\"}",
                 project);
        char *sr = cbm_mcp_handle_tool(srv, "search_graph", q);
        if (!sr || !strstr(sr, "idx832_fn")) {
            code = IDX832_NOT_INDEXED;
        }
        free(sr);
    }
    cbm_mcp_server_free(srv);
    free(project);
    return code;
}
#endif /* !_WIN32 */

TEST(index_bg_paths_route_through_supervisor_issue832) {
#ifdef _WIN32
    /* The guard marks the process as a supervisor host, which cannot be undone.
     * POSIX isolates that in a forked child; without fork we would pollute the
     * shared test-runner (breaking the #845 unmarked-embedder guard). The routing
     * logic is platform-independent and covered on POSIX CI; Windows containment
     * is covered by the end-to-end crash-containment test. */
    SKIP_PLATFORM("supervisor-host guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idx832-repo-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idx832-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1); /* inherited by the worker child */

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def idx832_fn():\n    return 'ok'\n", fp);
    fclose(fp);

    int code = -1;
    bool signalled = false;
    int sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60); /* a stuck worker dies here instead of hanging the runner */
        _exit(idx832_supervised_route_check(tmp_dir));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }

    char *project = cbm_project_name_from_path(tmp_dir);
    cleanup_project_db(cache, project);
    free(project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => worker hang)\n", sig);
    } else if (code != IDX832_OK) {
        printf("    child exit code %d (51=no spawn/in-process=RED, 52=null resp, "
               "53=not indexed, 54=server fail)\n",
               code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDX832_OK);
    PASS();
#endif
}

TEST(stage14_mcp_lifecycle_defaults_global_with_explicit_legacy_fallback) {
    char cache[256];
    snprintf(cache,sizeof(cache),"/tmp/cbm-stage14-global-lifecycle-XXXXXX");
    if(!cbm_mkdtemp(cache)) FAIL("could not create Stage 14 lifecycle fixture");
    const char *saved=getenv("CBM_CACHE_DIR");char *saved_copy=saved?strdup(saved):NULL;
    cbm_setenv("CBM_CACHE_DIR",cache,1);
    cbm_mcp_server_t *srv=cbm_mcp_server_new(NULL);ASSERT_NOT_NULL(srv);
    const char *begin_args="{\"project\":\"legacy-display-only\",\"workspace\":\"H:/Stage14 Unknown/记忆 工程\",\"session_id\":\"mcp-global-session\",\"turn_id\":\"mcp-global-turn\",\"prompt_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"prompt_length\":17,\"scope\":\"project\",\"idempotency_key\":\"mcp-global-begin\"}";
    char *begin=cbm_mcp_handle_tool(srv,"memory_task_begin",begin_args);
    ASSERT_TRUE(response_contains_json_fragment(begin,"\"status\":\"recorded\""));
    char *task_id=mcp_text_field_dup(begin,"task_id");ASSERT_NOT_NULL(task_id);
    char global_path[512],project_path[512];
    snprintf(global_path,sizeof(global_path),"%s/__global__-memory.db",cache);
    snprintf(project_path,sizeof(project_path),"%s/legacy-display-only-memory.db",cache);
    ASSERT_TRUE(roq_file_exists(global_path));ASSERT_FALSE(roq_file_exists(project_path));
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM memory_task"),1);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_project_catalog"),1);
    char *replay=cbm_mcp_handle_tool(srv,"memory_task_begin",begin_args);
    ASSERT_TRUE(response_contains_json_fragment(replay,"\"status\":\"replayed\""));
    const char *conflict_args="{\"project\":\"legacy-display-only\",\"workspace\":\"H:/Stage14 Unknown/记忆 工程\",\"session_id\":\"mcp-global-session\",\"turn_id\":\"mcp-global-turn\",\"prompt_sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"prompt_length\":17,\"scope\":\"project\",\"idempotency_key\":\"mcp-global-begin\"}";
    char *conflict=cbm_mcp_handle_tool(srv,"memory_task_begin",conflict_args);
    ASSERT_TRUE(response_contains_json_fragment(conflict,"\"code\":\"IDEMPOTENCY_CONFLICT\""));
    char status_args[1024];snprintf(status_args,sizeof(status_args),"{\"project\":\"ignored-provenance\",\"task_id\":\"%s\"}",task_id);
    char *status=cbm_mcp_handle_tool(srv,"memory_task_status",status_args);
    ASSERT_TRUE(response_contains_json_fragment(status,"\"state\":\"recall_completed\""));
    cbm_store_t *status_store=resolve_global_memory_store(srv,false);ASSERT_NOT_NULL(status_store);
    ASSERT_EQ(sqlite3_db_readonly(cbm_store_get_db(status_store),"main"),1);
    char complete_args[1536];snprintf(complete_args,sizeof(complete_args),"{\"project\":\"ignored-provenance\",\"task_id\":\"%s\",\"outcome\":\"completed\",\"idempotency_key\":\"mcp-global-complete\",\"attributions\":[]}",task_id);
    char *complete=cbm_mcp_handle_tool(srv,"memory_task_complete",complete_args);
    ASSERT_TRUE(response_contains_json_fragment(complete,"\"status\":\"recorded\""));
    char *complete_replay=cbm_mcp_handle_tool(srv,"memory_task_complete",complete_args);
    ASSERT_TRUE(response_contains_json_fragment(complete_replay,"\"status\":\"replayed\""));
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM codex_task_lifecycle WHERE state='completed'"),1);

    const char *legacy_project="stage14-legacy-fallback";
    snprintf(project_path,sizeof(project_path),"%s/%s-memory.db",cache,legacy_project);
    cbm_store_t *legacy=cbm_store_open_path(project_path);ASSERT_NOT_NULL(legacy);
    bool migrated_replay=false;char *legacy_report=NULL;
    ASSERT_EQ(cbm_orchestrator_migrate(legacy,&migrated_replay,&legacy_report),CBM_STORE_OK);free(legacy_report);legacy_report=NULL;
    cbm_task_begin_input_t legacy_begin={.project=legacy_project,.session_id="legacy-session",.turn_id="legacy-turn",
        .prompt_sha256="cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",.prompt_length=9,.idempotency_key="legacy-begin"};
    ASSERT_EQ(cbm_orchestrator_begin(legacy,&legacy_begin,&legacy_report),CBM_STORE_OK);
    char *legacy_envelope=cbm_mcp_text_result(legacy_report,false);
    char *legacy_task=mcp_text_field_dup(legacy_envelope,"task_id");free(legacy_envelope);
    if(!legacy_task){yyjson_doc *legacy_doc=yyjson_read(legacy_report,strlen(legacy_report),0);yyjson_val *legacy_root=legacy_doc?yyjson_doc_get_root(legacy_doc):NULL;const char *value=legacy_root?yyjson_get_str(yyjson_obj_get(legacy_root,"task_id")):NULL;legacy_task=value?strdup(value):NULL;yyjson_doc_free(legacy_doc);}
    free(legacy_report);legacy_report=NULL;cbm_store_close(legacy);ASSERT_NOT_NULL(legacy_task);
    char legacy_status[1024];snprintf(legacy_status,sizeof(legacy_status),"{\"project\":\"%s\",\"task_id\":\"%s\"}",legacy_project,legacy_task);
    char *without_fallback=cbm_mcp_handle_tool(srv,"memory_task_status",legacy_status);
    ASSERT_FALSE(without_fallback&&strstr(without_fallback,"\"state\":\"recall_completed\""));
    char legacy_status_fallback[1100];snprintf(legacy_status_fallback,sizeof(legacy_status_fallback),"{\"project\":\"%s\",\"task_id\":\"%s\",\"legacy_fallback\":true}",legacy_project,legacy_task);
    char *with_fallback=cbm_mcp_handle_tool(srv,"memory_task_status",legacy_status_fallback);
    ASSERT_TRUE(response_contains_json_fragment(with_fallback,"\"state\":\"recall_completed\""));
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM memory_task"),1);

    free(begin);free(replay);free(conflict);free(status);free(complete);free(complete_replay);
    free(without_fallback);free(with_fallback);free(task_id);free(legacy_task);
    cbm_mcp_server_free(srv);restore_cache_dir(saved_copy);free(saved_copy);th_rmtree(cache);
    PASS();
}

TEST(stage14_public_mcp_three_projects_share_global_store_and_provenance) {
    char cache[256];
    snprintf(cache,sizeof(cache),"/tmp/cbm-stage14-global-public-XXXXXX");
    if(!cbm_mkdtemp(cache)) FAIL("could not create Stage 14 public MCP fixture");
    const char *saved=getenv("CBM_CACHE_DIR");char *saved_copy=saved?strdup(saved):NULL;
    cbm_setenv("CBM_CACHE_DIR",cache,1);
    cbm_mcp_server_t *srv=cbm_mcp_server_new(NULL);ASSERT_NOT_NULL(srv);
    const char *project_a="H:/Stage14 MCP/A Source";
    const char *project_b="H:/Stage14 MCP/B Consumer";
    const char *project_c="H:/Stage14 MCP/C è®°å¿";
    char event_args[CBM_SZ_4K];
    snprintf(event_args,sizeof(event_args),
        "{\"project\":\"%s\",\"scope\":\"project\",\"type\":\"stage14_global_seed\","
        "\"source\":\"test.mcp.stage14\",\"kind\":\"fact\","
        "\"content\":\"STAGE14_GLOBAL_ROUTE_MARKER\",\"entity_key\":\"stage14-global-route\","
        "\"payload\":{\"schema\":\"stage14-global-route-v1\"},\"confidence\":0.99}",project_a);
    char *event=cbm_mcp_handle_tool(srv,"events",event_args);
    ASSERT_TRUE(response_contains_json_fragment(event,"\"status\":\"accepted\""));
    char *item_id=mcp_text_field_dup(event,"item_id");ASSERT_NOT_NULL(item_id);free(event);
    char global_path[512],global_graph_path[512];
    snprintf(global_path,sizeof(global_path),"%s/__global__-memory.db",cache);
    snprintf(global_graph_path,sizeof(global_graph_path),"%s/__global__-graph.db",cache);
    ASSERT_TRUE(roq_file_exists(global_path));
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM memory_item WHERE scope_project IS NULL"),1);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_memory_provenance WHERE source_kind='mcp_events'"),1);

    char retrieve_b_args[CBM_SZ_4K],retrieve_c_args[CBM_SZ_4K];
    snprintf(retrieve_b_args,sizeof(retrieve_b_args),
        "{\"project\":\"%s\",\"query\":\"STAGE14_GLOBAL_ROUTE_MARKER\",\"request_id\":\"stage14-public-b\",\"limit\":5}",project_b);
    snprintf(retrieve_c_args,sizeof(retrieve_c_args),
        "{\"project\":\"%s\",\"query\":\"STAGE14_GLOBAL_ROUTE_MARKER\",\"request_id\":\"stage14-public-c\",\"limit\":5}",project_c);
    char *retrieve_b=cbm_mcp_handle_tool(srv,"memories_retrieve",retrieve_b_args);
    char *retrieve_c=cbm_mcp_handle_tool(srv,"memories_retrieve",retrieve_c_args);
    ASSERT_TRUE(response_contains_json_fragment(retrieve_b,"\"candidate_pool\":\"global\""));
    ASSERT_TRUE(response_contains_json_fragment(retrieve_c,"\"candidate_pool\":\"global\""));
    ASSERT_NOT_NULL(strstr(retrieve_b,item_id));ASSERT_NOT_NULL(strstr(retrieve_c,item_id));
    ASSERT_TRUE(response_contains_json_fragment(retrieve_b,"\"source_kind\":\"mcp_events\""));
    ASSERT_TRUE(response_contains_json_fragment(retrieve_c,"\"source_kind\":\"mcp_events\""));
    /* Retrieval records candidate/provenance only. retrieved/selected is not
     * used evidence and must never create an active cross-project edge. */
    ASSERT_FALSE(roq_file_exists(global_graph_path));
    char *candidate_b=mcp_first_memory_field_dup(retrieve_b,"candidate_id");ASSERT_NOT_NULL(candidate_b);
    int sessions=count_sql_rows(global_path,"SELECT COUNT(*) FROM retrieval_session");
    int candidates=count_sql_rows(global_path,"SELECT COUNT(*) FROM retrieval_candidate");
    int contexts=count_sql_rows(global_path,"SELECT COUNT(*) FROM global_retrieval_project_context");
    char *replay_b=cbm_mcp_handle_tool(srv,"memories_retrieve",retrieve_b_args);
    char *candidate_b_replay=mcp_first_memory_field_dup(replay_b,"candidate_id");ASSERT_NOT_NULL(candidate_b_replay);
    ASSERT_STR_EQ(candidate_b,candidate_b_replay);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM retrieval_session"),sessions);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM retrieval_candidate"),candidates);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_retrieval_project_context"),contexts);
    ASSERT_FALSE(roq_file_exists(global_graph_path));
    char conflict_b_args[CBM_SZ_4K];
    snprintf(conflict_b_args,sizeof(conflict_b_args),
        "{\"project\":\"%s\",\"query\":\"ALTERED_STAGE14_GLOBAL_ROUTE_MARKER\",\"request_id\":\"stage14-public-b\",\"limit\":5}",project_b);
    char *conflict_b=cbm_mcp_handle_tool(srv,"memories_retrieve",conflict_b_args);
    ASSERT_TRUE(response_contains_json_fragment(conflict_b,"\"journal_status\":\"failed\""));
    ASSERT_FALSE(roq_file_exists(global_graph_path));
    char zero_args[CBM_SZ_4K];
    snprintf(zero_args,sizeof(zero_args),
        "{\"project\":\"%s\",\"query\":\"STAGE14_DEFINITE_ZERO_HIT_TOKEN\",\"request_id\":\"stage14-public-zero\",\"limit\":5}",project_b);
    char *zero=cbm_mcp_handle_tool(srv,"memories_retrieve",zero_args);
    ASSERT_TRUE(response_contains_json_fragment(zero,"\"journal_status\":\"completed\""));
    ASSERT_TRUE(response_contains_json_fragment(zero,"\"count\":0"));
    ASSERT_FALSE(roq_file_exists(global_graph_path));
    char *failed=cbm_mcp_handle_tool(srv,"memories_retrieve",
        "{\"project\":\"stage14-unknown-project\",\"query\":\"marker\",\"request_id\":\"stage14-public-failed\"}");
    ASSERT_NOT_NULL(failed);
    ASSERT_FALSE(roq_file_exists(global_graph_path));
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_project_catalog"),3);
    char legacy_memory_path[512];
    snprintf(legacy_memory_path,sizeof(legacy_memory_path),"%s/H-Stage14-MCP-A-Source-memory.db",cache);
    ASSERT_FALSE(roq_file_exists(legacy_memory_path));
    snprintf(legacy_memory_path,sizeof(legacy_memory_path),"%s/H-Stage14-MCP-B-Consumer-memory.db",cache);
    ASSERT_FALSE(roq_file_exists(legacy_memory_path));
    snprintf(legacy_memory_path,sizeof(legacy_memory_path),"%s/H-Stage14-MCP-C-memory.db",cache);
    ASSERT_FALSE(roq_file_exists(legacy_memory_path));

    free(retrieve_b);free(retrieve_c);free(replay_b);free(conflict_b);free(zero);free(failed);
    free(candidate_b);free(candidate_b_replay);free(item_id);
    cbm_mcp_server_free(srv);restore_cache_dir(saved_copy);free(saved_copy);th_rmtree(cache);
    PASS();
}

TEST(stage14_legacy_project_alias_matches_canonical_workspace_uuid) {
    char cache[256];snprintf(cache,sizeof(cache),"/tmp/cbm-stage14-legacy-alias-XXXXXX");
    if(!cbm_mkdtemp(cache)) FAIL("could not create Stage 14 legacy alias fixture");
    const char *saved=getenv("CBM_CACHE_DIR");char *saved_copy=saved?strdup(saved):NULL;
    cbm_setenv("CBM_CACHE_DIR",cache,1);cbm_mcp_server_t *srv=cbm_mcp_server_new(NULL);ASSERT_NOT_NULL(srv);
    const char *canonical="H:/Codex_H",*legacy="H-Codex_H-neuroplastic-main";
    char event_args[CBM_SZ_4K];snprintf(event_args,sizeof(event_args),
        "{\"project\":\"%s\",\"scope\":\"project\",\"kind\":\"fact\","
        "\"content\":\"STAGE14_CANONICAL_H_MARKER\",\"payload\":{\"schema\":\"stage14-alias-v1\"}}",canonical);
    char *canonical_event=cbm_mcp_handle_tool(srv,"events",event_args);
    ASSERT_TRUE(response_contains_json_fragment(canonical_event,"\"status\":\"accepted\""));free(canonical_event);
    char global_path[512];snprintf(global_path,sizeof(global_path),"%s/__global__-memory.db",cache);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_project_catalog WHERE project_uuid='2fb874ff-b9b3-5d31-997e-793aed30ce00'"),1);
    const char *forbidden_active=
        "{\"action\":\"apply\",\"mode\":\"active\","
        "\"project\":\"2fb874ff-b9b3-5d31-997e-793aed30ce00\","
        "\"run_id\":\"stage14-public-active-run\",\"task_id\":\"stage14-public-active-task\","
        "\"idempotency_key\":\"stage14-public-active-key\",\"max_evolution_events\":17,"
        "\"max_cross_project_edges\":16,\"manifest_path\":\"C:\\\\stage14\\\\task.json\","
        "\"manifest_sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
        "\"authorization_manifest_path\":\"C:\\\\stage14\\\\auth.json\","
        "\"authorization_manifest_sha256\":"
        "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}";
    char *forbidden=cbm_mcp_handle_tool(
        srv,"memory_reinforcement_replay",forbidden_active);
    ASSERT_TRUE(response_contains_json_fragment(
        forbidden,"\"code\":\"ACTIVE_MODE_FORBIDDEN\""));
    ASSERT_TRUE(response_contains_json_fragment(
        forbidden,"\"production_state_written\":false"));
    free(forbidden);
    ASSERT_TRUE(sqlite_exec_ok(global_path,
        "INSERT INTO global_legacy_alias(legacy_kind,legacy_id,global_id,project_uuid,payload_sha256,created_at) VALUES('project','H-Codex_H-neuroplastic-main','2fb874ff-b9b3-5d31-997e-793aed30ce00','2fb874ff-b9b3-5d31-997e-793aed30ce00','aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','2026-07-27T00:00:00Z')"));
    snprintf(event_args,sizeof(event_args),
        "{\"project\":\"%s\",\"scope\":\"project\",\"kind\":\"fact\","
        "\"content\":\"STAGE14_LEGACY_H_MARKER\",\"payload\":{\"schema\":\"stage14-alias-v1\"}}",legacy);
    char *legacy_event=cbm_mcp_handle_tool(srv,"events",event_args);
    ASSERT_TRUE(response_contains_json_fragment(legacy_event,"\"status\":\"accepted\""));free(legacy_event);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_project_catalog"),1);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_memory_provenance WHERE project_uuid='2fb874ff-b9b3-5d31-997e-793aed30ce00'"),2);
    ASSERT_EQ(count_sql_rows(global_path,"SELECT COUNT(*) FROM global_memory_provenance WHERE legacy_project_id='H-Codex_H-neuroplastic-main'"),1);
    char retrieve_args[CBM_SZ_4K];snprintf(retrieve_args,sizeof(retrieve_args),
        "{\"project\":\"%s\",\"query\":\"STAGE14_CANONICAL_H_MARKER\",\"request_id\":\"stage14-legacy-retrieve\",\"limit\":5}",legacy);
    char *retrieve=cbm_mcp_handle_tool(srv,"memories_retrieve",retrieve_args);
    ASSERT_TRUE(response_contains_json_fragment(retrieve,"\"project_uuid\":\"2fb874ff-b9b3-5d31-997e-793aed30ce00\""));
    ASSERT_TRUE(response_contains_json_fragment(retrieve,"\"candidate_pool\":\"global\""));free(retrieve);
    cbm_mcp_server_free(srv);restore_cache_dir(saved_copy);free(saved_copy);th_rmtree(cache);PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  Parallel-only crash recovery (ms-typescript cascade fix)
 * ══════════════════════════════════════════════════════════════════ */

/* The old recovery loop re-ran the worker SINGLE-THREADED to keep one exact
 * crash marker. At scale that fell into the sequential crawl, was killed as
 * a hang mid-pass, and the stale marker quarantined FOUR innocent
 * ms-typescript fixtures, one 15-minute retry at a time. The reworked loop
 * re-runs PARALLEL with a marker journal; a file is quarantined only when
 * it is in-flight across two consecutive failed runs.
 *
 * This guard proves the CONTRACT: with an injected crasher among good
 * files, the supervised index must (a) never spawn a single-threaded worker
 * (cbm_index_supervisor_spawn_st_count stays 0 — RED on the old loop),
 * (b) quarantine exactly the crasher, (c) leave the innocents indexed and
 * NOT quarantined. */
enum {
    IDXPAR_OK = 0,
    IDXPAR_ST_SPAWN = 61,      /* single-threaded recovery spawn happened (RED) */
    IDXPAR_NULL_RESP = 62,     /* supervised entry degraded to NULL */
    IDXPAR_NOT_INDEXED = 63,   /* response lacks status indexed */
    IDXPAR_NO_QUARANTINE = 64, /* crasher missing from skipped[] */
    IDXPAR_INNOCENT_HIT = 65,  /* a good file was quarantined/skipped */
    IDXPAR_GOOD_MISSING = 66,  /* good file's Function absent from the store */
};

#ifndef _WIN32
static int idxpar_recovery_check(const char *repo_dir) {
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    /* Rounds needed: fail+record, fail+quarantine, clean. Generous cap. */
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "5", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);
    cbm_setenv("CBM_TEST_CRASH_ON", "idxpar_crasher", 1);

    int st_before = cbm_index_supervisor_spawn_st_count();
    char *resp = cbm_mcp_index_run_supervised_path(repo_dir);
    int st_after = cbm_index_supervisor_spawn_st_count();
    cbm_unsetenv("CBM_TEST_CRASH_ON");

    if (st_after != st_before) {
        free(resp);
        return IDXPAR_ST_SPAWN; /* discriminating assertion: RED on the old loop */
    }
    if (!resp) {
        return IDXPAR_NULL_RESP;
    }
    bool indexed = response_contains_json_fragment(resp, "\"status\":\"indexed\"");
    bool crasher_skipped = strstr(resp, "idxpar_crasher.py") != NULL;
    bool innocent_hit =
        strstr(resp, "idxpar_good_a.py") != NULL || strstr(resp, "idxpar_good_b.py") != NULL;
    free(resp);
    if (!indexed) {
        return IDXPAR_NOT_INDEXED;
    }
    if (!crasher_skipped) {
        return IDXPAR_NO_QUARANTINE;
    }
    if (innocent_hit) {
        return IDXPAR_INNOCENT_HIT;
    }

    /* Store proof: an innocent's Function node exists. */
    char *project = cbm_project_name_from_path(repo_dir);
    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    int code = IDXPAR_OK;
    if (srv && project) {
        char q[512];
        snprintf(q, sizeof(q),
                 "{\"project\":\"%s\",\"name_pattern\":\"idxpar_good_fn\",\"label\":\"Function\"}",
                 project);
        char *sr = cbm_mcp_handle_tool(srv, "search_graph", q);
        if (!sr || !strstr(sr, "idxpar_good_fn")) {
            code = IDXPAR_GOOD_MISSING;
        }
        free(sr);
    }
    if (srv) {
        cbm_mcp_server_free(srv);
    }
    free(project);
    return code;
}
#endif /* !_WIN32 */

TEST(index_recovery_parallel_quarantines_crasher) {
#ifdef _WIN32
    SKIP_PLATFORM("parallel-recovery guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[CBM_SZ_256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idxpar-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        FAIL("mkdtemp failed");
    }
    char cache[CBM_SZ_256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idxpar-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        FAIL("mkdtemp cache failed");
    }
    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? cbm_strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char p1[CBM_SZ_512];
    char p2[CBM_SZ_512];
    char pc[CBM_SZ_512];
    snprintf(p1, sizeof(p1), "%s/idxpar_good_a.py", tmp_dir);
    snprintf(p2, sizeof(p2), "%s/idxpar_good_b.py", tmp_dir);
    snprintf(pc, sizeof(pc), "%s/idxpar_crasher.py", tmp_dir);
    FILE *f = fopen(p1, "w");
    ASSERT_NOT_NULL(f);
    fputs("def idxpar_good_fn():\n    return 'ok'\n", f);
    fclose(f);
    f = fopen(p2, "w");
    ASSERT_NOT_NULL(f);
    fputs("def idxpar_good_fn_b():\n    return 'ok'\n", f);
    fclose(f);
    f = fopen(pc, "w");
    ASSERT_NOT_NULL(f);
    fputs("def idxpar_crash_fn():\n    return 'boom'\n", f);
    fclose(f);

    int code = -1;
    bool signalled = false;
    int sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(120); /* generous: three supervised rounds + clean run */
        _exit(idxpar_recovery_check(tmp_dir));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }

    char *project = cbm_project_name_from_path(tmp_dir);
    cleanup_project_db(cache, project);
    free(project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(p1);
    remove(p2);
    remove(pc);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => recovery loop hang)\n", sig);
    } else if (code != IDXPAR_OK) {
        printf("    child exit code %d (61=ST spawn/RED, 62=null resp, 63=not indexed, "
               "64=no quarantine, 65=innocent hit, 66=good missing)\n",
               code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDXPAR_OK);
    PASS();
#endif
}

/* ══════════════════════════════════════════════════════════════════
 *  AUTO_WATCH GATE  (distilled from PR #625)
 *
 *  Background watcher registration on session connect is gated by the
 *  `auto_watch` config key (default TRUE = existing behavior).
 * ══════════════════════════════════════════════════════════════════ */

/* Drive the already-indexed connect path (initialize → maybe_auto_index →
 * watcher registration) and return the resulting watch count.
 * auto_watch_value: NULL leaves the key unset (exercises the default),
 * otherwise the key is set to that value before initialize.
 * Returns a negative code on fixture setup failure. */
static int auto_watch_connect_watch_count(const char *auto_watch_value) {
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-autowatch-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        return -1;
    }

    char repodir[512];
    snprintf(repodir, sizeof(repodir), "%s/repo", cache);
    if (th_mkdir_p(repodir) != 0) {
        th_rmtree(cache);
        return -2;
    }

    /* Same derivation detect_session uses on the cwd — realpath-based, so
     * the name matches even where /tmp is a symlink (macOS). */
    char *project = cbm_project_name_from_path(repodir);
    if (!project) {
        th_rmtree(cache);
        return -3;
    }

    /* Pre-create <cache>/<project>.db so maybe_auto_index takes the
     * "already indexed" branch — the watcher-registration site under test. */
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", cache, project);
    if (th_write_file(db_path, "") != 0) {
        free(project);
        th_rmtree(cache);
        return -4;
    }
    free(project);

    const char *saved = getenv("CBM_CACHE_DIR");
    char *saved_copy = saved ? strdup(saved) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1);

    char old_cwd[1024];
    if (!cbm_getcwd(old_cwd, sizeof(old_cwd)) || cbm_chdir(repodir) != 0) {
        restore_cache_dir(saved_copy);
        free(saved_copy);
        th_rmtree(cache);
        return -5;
    }

    int count = -6;
    cbm_config_t *cfg = cbm_config_open(cache);
    cbm_store_t *wstore = cbm_store_open_memory();
    cbm_watcher_t *watcher = wstore ? cbm_watcher_new(wstore, NULL, NULL) : NULL;
    if (cfg && watcher) {
        if (auto_watch_value) {
            cbm_config_set(cfg, CBM_CONFIG_AUTO_WATCH, auto_watch_value);
        }

        cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
        if (srv) {
            cbm_mcp_server_set_watcher(srv, watcher);
            cbm_mcp_server_set_config(srv, cfg);
            char *resp = cbm_mcp_server_handle(
                srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
            free(resp);
            count = cbm_watcher_watch_count(watcher);
            cbm_mcp_server_free(srv);
        }
    }

    if (watcher) {
        cbm_watcher_free(watcher);
    }
    if (wstore) {
        cbm_store_close(wstore);
    }
    if (cfg) {
        cbm_config_close(cfg);
    }

    (void)cbm_chdir(old_cwd);
    restore_cache_dir(saved_copy);
    free(saved_copy);
    th_rmtree(cache);
    return count;
}

/* Default (key unset) → watcher registered on connect. Guards the
 * no-behavior-change promise of the auto_watch gate: existing users keep
 * background auto-sync without touching config. */
TEST(mcp_auto_watch_default_registers_watcher_on_connect) {
    int count = auto_watch_connect_watch_count(NULL);
    if (count < 0) {
        PASS(); /* fixture setup failed (tmpdir/cwd unavailable) — skip */
    }
    ASSERT_EQ(count, 1);
    PASS();
}

/* auto_watch=false → NO watcher registered on connect. RED on pre-gate code
 * (registration was unconditional and the key did not exist). */
TEST(mcp_auto_watch_false_skips_watcher_on_connect) {
    int count = auto_watch_connect_watch_count("false");
    if (count < 0) {
        PASS(); /* fixture setup failed (tmpdir/cwd unavailable) — skip */
    }
    ASSERT_EQ(count, 0);
    PASS();
}

/* ══════════════════════════════════════════════════════════════════
 *  #853 — auto_watch=false must ALSO gate the SUPERVISED fresh-index
 *          watcher registration (keystone × #849 merge interaction)
 * ══════════════════════════════════════════════════════════════════ */

/* #849 routed ALL watcher registration through register_watcher_if_enabled()
 * (auto_watch gate). The #832 keystone then added a SECOND registration site in
 * autoindex_thread's supervised-success branch, but wired it as a DIRECT
 * cbm_watcher_watch() guarded only by `if (srv->watcher)` — srv->watcher is set
 * unconditionally, so that guard does NOT honour `config set auto_watch false`.
 * The above tests only cover the already-indexed on-connect path
 * (register_watcher_if_enabled); this guard covers the fresh-index SUPERVISED
 * autoindex_thread branch that #832 introduced.
 *
 * Drive the real public entry initialize → maybe_auto_index → autoindex_thread on
 * a supervisor-marked host (kill switch off) with a FRESH project (no prior .db)
 * and auto_watch=false. cbm_mcp_server_free() joins the autoindex thread, so the
 * (buggy or gated) registration decision has run before we read the watch count.
 *
 * RED on the unfixed ungated block: the supervised success branch calls
 * cbm_watcher_watch() unconditionally → watch_count == 1 → IDX853_WATCHER_REGISTERED.
 * GREEN once it calls register_watcher_if_enabled() → auto_watch_off skip → 0.
 * spawn_count is asserted to have advanced so the assertion cannot pass vacuously
 * (i.e. green only because the supervised branch was never entered). */
enum {
    IDX853_OK = 0,                  /* watch_count==0, supervised branch ran → GREEN */
    IDX853_WATCHER_REGISTERED = 61, /* watch_count==1 → RED: ungated cbm_watcher_watch */
    IDX853_NO_SPAWN = 62,           /* spawn_count unchanged → supervised path not exercised */
    IDX853_SETUP_FAIL = 63,         /* config/watcher/server/cwd setup failed */
    IDX853_BAD_COUNT = 64,          /* unexpected watch_count (<0 or >1) */
};

#ifndef _WIN32 /* helper used only by the POSIX fork harness below */
static int idx853_supervised_autowatch_check(const char *repo_dir, const char *cache_dir) {
    /* Become a supervisor host with the kill switch OFF — the real prod MCP
     * server's state. Done in the FORKED CHILD only (see harness) so the parent
     * test-runner's process-wide host mark stays clear (#845 invariant). Bound the
     * worker so a stuck spawn cannot run long under the fork+alarm net. */
    cbm_index_supervisor_mark_host();
    cbm_unsetenv("CBM_INDEX_SUPERVISOR");
    cbm_setenv("CBM_INDEX_MAX_RESTARTS", "1", 1);
    cbm_setenv("CBM_INDEX_WORKER_TIMEOUT_S", "30", 1);

    cbm_config_t *cfg = cbm_config_open(cache_dir);
    cbm_store_t *wstore = cbm_store_open_memory();
    cbm_watcher_t *watcher = wstore ? cbm_watcher_new(wstore, NULL, NULL) : NULL;
    if (!cfg || !watcher) {
        if (watcher) {
            cbm_watcher_free(watcher);
        }
        if (wstore) {
            cbm_store_close(wstore);
        }
        if (cfg) {
            cbm_config_close(cfg);
        }
        return IDX853_SETUP_FAIL;
    }
    /* auto_index=true → maybe_auto_index launches autoindex_thread for the fresh
     * project; auto_watch=false → the gate this guard exercises. */
    cbm_config_set(cfg, CBM_CONFIG_AUTO_INDEX, "true");
    cbm_config_set(cfg, CBM_CONFIG_AUTO_WATCH, "false");

    /* detect_session derives session_root/session_project from the cwd. */
    char old_cwd[1024];
    if (!cbm_getcwd(old_cwd, sizeof(old_cwd)) || cbm_chdir(repo_dir) != 0) {
        cbm_watcher_free(watcher);
        cbm_store_close(wstore);
        cbm_config_close(cfg);
        return IDX853_SETUP_FAIL;
    }

    int spawns_before = cbm_index_supervisor_spawn_count();
    int code = IDX853_SETUP_FAIL;

    cbm_mcp_server_t *srv = cbm_mcp_server_new(NULL);
    if (srv) {
        cbm_mcp_server_set_watcher(srv, watcher);
        cbm_mcp_server_set_config(srv, cfg);
        char *resp = cbm_mcp_server_handle(
            srv, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
        free(resp);
        /* free() joins the autoindex thread → the supervised worker has finished
         * and the registration decision (buggy or gated) has executed. */
        cbm_mcp_server_free(srv);

        int spawns_after = cbm_index_supervisor_spawn_count();
        int watch_count = cbm_watcher_watch_count(watcher);

        if (spawns_after == spawns_before) {
            code = IDX853_NO_SPAWN; /* supervised branch never ran — not a valid probe */
        } else if (watch_count == 1) {
            code = IDX853_WATCHER_REGISTERED; /* the discriminating RED assertion */
        } else if (watch_count == 0) {
            code = IDX853_OK;
        } else {
            code = IDX853_BAD_COUNT;
        }
    }

    (void)cbm_chdir(old_cwd);
    cbm_watcher_free(watcher);
    cbm_store_close(wstore);
    cbm_config_close(cfg);
    return code;
}
#endif /* !_WIN32 */

TEST(mcp_auto_watch_false_skips_supervised_autoindex_issue853) {
#ifdef _WIN32
    /* Marks the process as a supervisor host (irreversible); POSIX isolates that
     * in a forked child. The gate logic is platform-independent and covered on
     * POSIX CI. */
    SKIP_PLATFORM("supervisor-host guard needs fork isolation (POSIX-only)");
#else
    char tmp_dir[256];
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/cbm-idx853-repo-XXXXXX");
    if (!cbm_mkdtemp(tmp_dir)) {
        PASS();
    }
    char cache[256];
    snprintf(cache, sizeof(cache), "/tmp/cbm-idx853-cache-XXXXXX");
    if (!cbm_mkdtemp(cache)) {
        cbm_rmdir(tmp_dir);
        PASS();
    }

    const char *saved_cache = getenv("CBM_CACHE_DIR");
    char *saved_cache_copy = saved_cache ? strdup(saved_cache) : NULL;
    cbm_setenv("CBM_CACHE_DIR", cache, 1); /* inherited by the worker child */

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/main.py", tmp_dir);
    FILE *fp = fopen(src_path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("def idx853_fn():\n    return 'ok'\n", fp);
    fclose(fp);

    int code = -1;
    bool signalled = false;
    int sig = 0;
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        alarm(60); /* a stuck worker dies here instead of hanging the runner */
        _exit(idx853_supervised_autowatch_check(tmp_dir, cache));
    }
    ASSERT_TRUE(pid > 0);
    int status = 0;
    (void)waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        signalled = true;
        sig = WTERMSIG(status);
    }

    char *project = cbm_project_name_from_path(tmp_dir);
    cleanup_project_db(cache, project);
    free(project);
    restore_cache_dir(saved_cache_copy);
    free(saved_cache_copy);
    remove(src_path);
    cbm_rmdir(cache);
    cbm_rmdir(tmp_dir);

    if (signalled) {
        printf("    child killed by signal %d (alarm => worker hang)\n", sig);
    } else if (code != IDX853_OK) {
        printf("    child exit code %d (61=watcher registered under auto_watch=false=RED, "
               "62=no spawn, 63=setup fail, 64=bad count)\n",
               code);
    }
    ASSERT_FALSE(signalled);
    ASSERT_EQ(code, IDX853_OK);
    PASS();
#endif
}

/* ══════════════════════════════════════════════════════════════════
 *  SUITE
 * ══════════════════════════════════════════════════════════════════ */

SUITE(mcp) {
    const char *saved_fixture_allowlist = getenv("CBM_STAGE11_TEST_PROJECT_ALLOWLIST");
    char *saved_fixture_allowlist_copy =
        saved_fixture_allowlist ? strdup(saved_fixture_allowlist) : NULL;
    cbm_setenv(
        "CBM_STAGE11_TEST_PROJECT_ALLOWLIST",
        "stage11-security-mcp-fixture;stage6-fixture-stage8-derived-v2-mcp;"
        "stage10-fixture-guard;stage9-delete-guard;"
        "mcp-feedback-cold-reopen;mcp-stage7-feedback;"
        "stage6-fixture-stage8-g8-candidate-v1;mcp-stage5-observe-loop;"
        "stage6-fixture-mcp-shadow",
        1);
    /* JSON-RPC parsing */
    RUN_TEST(jsonrpc_parse_request);
    RUN_TEST(jsonrpc_parse_notification);
    RUN_TEST(jsonrpc_parse_invalid);
    RUN_TEST(jsonrpc_parse_tools_call);
    RUN_TEST(jsonrpc_parse_string_id_issue253);
    RUN_TEST(jsonrpc_format_response_string_id_issue253);

    /* JSON-RPC parsing — edge cases */
    RUN_TEST(jsonrpc_parse_empty_string);
    RUN_TEST(jsonrpc_parse_missing_jsonrpc_field);
    RUN_TEST(jsonrpc_parse_missing_method);
    RUN_TEST(jsonrpc_parse_string_id);
    RUN_TEST(jsonrpc_parse_no_params);
    RUN_TEST(jsonrpc_parse_extra_whitespace);
    RUN_TEST(jsonrpc_parse_array_not_object);

    /* JSON-RPC formatting */
    RUN_TEST(jsonrpc_format_response);
    RUN_TEST(jsonrpc_format_error);

    /* MCP protocol helpers */
    RUN_TEST(mcp_initialize_response);
    RUN_TEST(mcp_tools_list);
    RUN_TEST(mcp_tools_list_latest_metadata);
    RUN_TEST(mcp_all_tools_expose_valid_required_schema_fields);
    RUN_TEST(mcp_index_repository_declares_name_override_issue571);
    RUN_TEST(mcp_tools_array_schemas_have_items);
    RUN_TEST(mcp_stage8_reinforcement_schema_is_fixed);
    RUN_TEST(mcp_stage9_edge_lifecycle_schemas_are_fixed);
    RUN_TEST(mcp_stage10_concept_schemas_are_fixed);
    RUN_TEST(mcp_stage14_manager_schemas_are_fixed);
    RUN_TEST(mcp_stage14_production_controls_fail_closed_without_explicit_authorization);
    RUN_TEST(mcp_stage14_manager_response_shapes_are_fixed);
    RUN_TEST(mcp_stage10_concept_recall_and_review_require_guards);
    RUN_TEST(mcp_stage9_hard_delete_modes_fail_closed);
    RUN_TEST(mcp_events_schema_exposes_optional_derived_from);
    RUN_TEST(mcp_ingest_traces_items_disallow_additional_properties_issue731);
    RUN_TEST(mcp_get_architecture_aspects_schema_enum_pr560);
    RUN_TEST(mcp_text_result);
    RUN_TEST(mcp_text_result_skips_structured_content_for_plain_text);
    RUN_TEST(mcp_cancel_matches_request_id);
    RUN_TEST(mcp_text_result_error);

    /* Argument extraction */
    RUN_TEST(mcp_get_tool_name);
    RUN_TEST(mcp_get_arguments);
    RUN_TEST(mcp_get_string_arg);
    RUN_TEST(mcp_get_int_arg);
    RUN_TEST(mcp_get_bool_arg);

    /* Argument extraction — edge cases */
    RUN_TEST(mcp_get_string_arg_empty_json);
    RUN_TEST(mcp_get_string_arg_empty_object);
    RUN_TEST(mcp_get_string_arg_nested_value);
    RUN_TEST(mcp_get_string_arg_int_value);
    RUN_TEST(mcp_get_int_arg_empty_json);
    RUN_TEST(mcp_get_int_arg_string_value);
    RUN_TEST(mcp_get_int_arg_bool_value);
    RUN_TEST(mcp_get_bool_arg_empty_json);
    RUN_TEST(mcp_get_bool_arg_int_value);
    RUN_TEST(mcp_get_tool_name_empty_json);
    RUN_TEST(mcp_get_tool_name_missing_name);
    RUN_TEST(mcp_get_arguments_empty_json);
    RUN_TEST(mcp_get_arguments_no_arguments_key);

    /* Server protocol handling */
    RUN_TEST(server_handle_initialize);
    RUN_TEST(server_handle_initialized_notification);
    RUN_TEST(server_handle_tools_list);
    RUN_TEST(server_handle_tools_list_null_cursor_is_first_page);
    RUN_TEST(server_handle_tools_list_meta_only_is_first_page);
    RUN_TEST(server_handle_tools_list_paginates);
    RUN_TEST(server_handle_tools_list_first_page_memory_compat_inventory);
    RUN_TEST(events_derived_from_is_project_scoped_atomic_and_compatible);
    RUN_TEST(stage11_security_check_and_events_reject_before_transaction);
    RUN_TEST(memory_feedback_legacy_three_field_fails_closed_after_cold_retrieve);
    RUN_TEST(stage7_mcp_feedback_model_report_replay_and_conflict);
    RUN_TEST(stage8_mcp_off_shadow_active_replay_and_conflict_fixture);
    RUN_TEST(stage5_mcp_observe_only_candidate_injection_usage_loop);
    RUN_TEST(stage6_mcp_off_shadow_and_active_fixture_guards);
    RUN_TEST(stage14_mcp_lifecycle_defaults_global_with_explicit_legacy_fallback);
    RUN_TEST(stage14_public_mcp_three_projects_share_global_store_and_provenance);
    RUN_TEST(stage14_legacy_project_alias_matches_canonical_workspace_uuid);
    RUN_TEST(server_handle_logs_request_without_params);
    RUN_TEST(server_handle_unknown_method);

    /* Server handle — edge cases */
    RUN_TEST(server_handle_invalid_json);
    RUN_TEST(server_handle_empty_object);
    RUN_TEST(server_handle_tools_call_missing_name);

    /* Tool handlers */
    RUN_TEST(tool_list_projects_empty);
    RUN_TEST(tool_get_graph_schema_empty);
    RUN_TEST(tool_unknown_tool);
    RUN_TEST(tool_search_graph_basic);
    RUN_TEST(tool_search_graph_includes_node_properties);
    RUN_TEST(tool_search_graph_query_honors_file_pattern_issue552);
    RUN_TEST(tool_query_graph_basic);
    RUN_TEST(tool_index_status_no_project);
    RUN_TEST(tool_index_status_includes_git_metadata);

    /* Tool handlers with validation */
    RUN_TEST(tool_trace_call_path_not_found);
    RUN_TEST(tool_trace_missing_function_name);
    RUN_TEST(tool_trace_call_path_ambiguous);
    RUN_TEST(tool_trace_call_path_prefers_definition);
    RUN_TEST(tool_trace_call_path_depth_clamped);
    RUN_TEST(tool_trace_call_path_distinct_defs_not_over_unioned);
    RUN_TEST(tool_trace_call_path_dts_stub_unions_with_impl);
    RUN_TEST(tool_delete_project_not_found);
    RUN_TEST(tool_get_architecture_empty);
    RUN_TEST(tool_get_architecture_emits_populated_sections);
    RUN_TEST(tool_get_architecture_overview_compact_subset_pr560);
    RUN_TEST(tool_get_architecture_rejects_unknown_aspect_pr560);
    RUN_TEST(tool_get_architecture_accepts_project_name_alias_issue640);
    RUN_TEST(tool_search_graph_accepts_project_name_alias_issue640);
    RUN_TEST(tool_get_architecture_path_scoping);
    RUN_TEST(tool_query_graph_missing_query);

    /* Pipeline-dependent tool handlers */
    RUN_TEST(tool_index_repository_missing_path);
    RUN_TEST(tool_get_code_snippet_missing_qn);
    RUN_TEST(tool_get_code_snippet_not_found);
    RUN_TEST(tool_search_code_missing_pattern);
    RUN_TEST(tool_search_code_no_project);
    RUN_TEST(search_code_multi_word);
    RUN_TEST(search_code_scoped_path_with_spaces_issue687);
    RUN_TEST(search_code_path_filter_prefilter_keeps_matches);
    RUN_TEST(search_code_path_filter_matches_nothing);
    RUN_TEST(search_code_invalid_regex_errors_issue283);
    RUN_TEST(search_code_literal_pipe_warns_issue282);
    RUN_TEST(search_code_ampersand_accepted_issue272);
    RUN_TEST(tool_detect_changes_no_project);
    RUN_TEST(tool_manage_adr_no_project);
    RUN_TEST(tool_manage_adr_get_with_existing_adr);
    RUN_TEST(tool_manage_adr_unified_backend_issue256);
    RUN_TEST(tool_index_repository_reports_store_backed_adr);
    RUN_TEST(tool_index_repository_dot_uses_absolute_project_key_and_preserves_adr);
    RUN_TEST(index_supervisor_gate_requires_marked_host_issue845);
    RUN_TEST(index_bg_paths_route_through_supervisor_issue832);
    RUN_TEST(index_recovery_parallel_quarantines_crasher);
    RUN_TEST(tool_manage_adr_not_found_rich_error);
    RUN_TEST(tool_manage_adr_get_accepts_abs_path);
    RUN_TEST(tool_manage_adr_get_accepts_symlink_path);
    RUN_TEST(tool_detect_changes_not_found_rich_error);
    RUN_TEST(tool_ingest_traces_basic);
    RUN_TEST(tool_ingest_traces_empty);

    /* Query store read-only (data integrity) */
    RUN_TEST(readonly_query_does_not_mutate_db);
    RUN_TEST(readonly_query_succeeds_on_readonly_fs);

    /* Idle store eviction */
    RUN_TEST(store_idle_eviction);
    RUN_TEST(store_idle_no_eviction_within_timeout);
    RUN_TEST(store_idle_evict_protects_initial_store);
    RUN_TEST(store_idle_evict_access_resets_timer);

    /* URI helpers */
    RUN_TEST(parse_file_uri_unix);
    RUN_TEST(parse_file_uri_windows);
    RUN_TEST(parse_file_uri_invalid);

    /* URI helpers — edge cases */
    RUN_TEST(parse_file_uri_http_scheme);
    RUN_TEST(parse_file_uri_ftp_scheme);
    RUN_TEST(parse_file_uri_buffer_too_small);
    RUN_TEST(parse_file_uri_spaces_in_path);
    RUN_TEST(parse_file_uri_null_out_path);
    RUN_TEST(parse_file_uri_zero_size);

    /* Poll/getline FILE* buffering fix */
#ifndef _WIN32
    RUN_TEST(mcp_server_run_rapid_messages);
#endif

    /* Snippet resolution (port of snippet_test.go) */
    RUN_TEST(snippet_exact_qn);
    RUN_TEST(snippet_qn_suffix);
    RUN_TEST(snippet_unique_short_name);
    RUN_TEST(snippet_name_tier);
    RUN_TEST(snippet_ambiguous_short_name);
    RUN_TEST(snippet_not_found);
    RUN_TEST(snippet_fuzzy_suggestions);
    RUN_TEST(snippet_enriched_properties);
    RUN_TEST(snippet_fuzzy_last_segment);
    RUN_TEST(snippet_auto_resolve_default);
    RUN_TEST(snippet_auto_resolve_enabled);
    RUN_TEST(snippet_include_neighbors_default);
    RUN_TEST(snippet_include_neighbors_enabled);
    RUN_TEST(snippet_source_invalid_utf8);
    RUN_TEST(tool_bad_project_name_no_overflow_issue235);
    RUN_TEST(tool_bad_project_error_valid_json_issue235);
    RUN_TEST(tool_resolve_store_by_internal_name_issue704);

    /* auto_watch gate (distilled from PR #625) */
    RUN_TEST(mcp_auto_watch_default_registers_watcher_on_connect);
    RUN_TEST(mcp_auto_watch_false_skips_watcher_on_connect);
    RUN_TEST(mcp_auto_watch_false_skips_supervised_autoindex_issue853);
    if (saved_fixture_allowlist_copy) {
        cbm_setenv("CBM_STAGE11_TEST_PROJECT_ALLOWLIST", saved_fixture_allowlist_copy, 1);
    } else {
        cbm_unsetenv("CBM_STAGE11_TEST_PROJECT_ALLOWLIST");
    }
    free(saved_fixture_allowlist_copy);
}
