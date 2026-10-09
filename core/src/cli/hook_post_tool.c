#include "cli/cli.h"

#include "foundation/platform.h"
#include "mcp/mcp.h"
#include "memory/global_memory.h"
#include "memory/memory_orchestrator.h"
#include "memory/memory_store.h"
#include "memory/verified_learning.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POST_TOOL_STDIN_CAP (2 * 1024 * 1024)

static char *post_tool_read_stdin(void) {
    char *buffer = malloc(POST_TOOL_STDIN_CAP + 1);
    if (!buffer)
        return NULL;
    size_t total = 0;
    while (total < POST_TOOL_STDIN_CAP) {
        size_t count = fread(buffer + total, 1, POST_TOOL_STDIN_CAP - total, stdin);
        if (count == 0)
            break;
        total += count;
    }
    buffer[total] = '\0';
    return buffer;
}

static const char *post_tool_string(yyjson_val *root, const char *key) {
    yyjson_val *value = root && yyjson_is_obj(root) ? yyjson_obj_get(root, key) : NULL;
    return value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static void post_tool_emit(bool degraded, const char *code) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "continue", true);
    if (degraded) {
        char message[160];
        snprintf(message, sizeof(message), "memory evidence degraded: %s",
                 code ? code : "unavailable");
        yyjson_mut_obj_add_str(doc, root, "systemMessage", message);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (json) {
        fputs(json, stdout);
        free(json);
    }
}

static bool post_tool_terminal_state(const char *state) {
    return state && (strcmp(state, "completed") == 0 || strcmp(state, "failed") == 0 ||
                     strcmp(state, "cancelled") == 0 || strcmp(state, "abandoned") == 0);
}

int cbm_cmd_memory_post_tool(void) {
    char *input = post_tool_read_stdin();
    yyjson_doc *doc = input ? yyjson_read(input, strlen(input), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *event = post_tool_string(root, "hook_event_name");
    const char *session_id = post_tool_string(root, "session_id");
    const char *turn_id = post_tool_string(root, "turn_id");
    const char *tool_name = post_tool_string(root, "tool_name");
    const char *tool_use_id = post_tool_string(root, "tool_use_id");
    yyjson_val *tool_input = root ? yyjson_obj_get(root, "tool_input") : NULL;
    yyjson_val *tool_output = root ? yyjson_obj_get(root, "tool_response") : NULL;
    if (!root || (event && strcmp(event, "PostToolUse") != 0) || !session_id || !turn_id ||
        !tool_name || !tool_use_id || !tool_input || !tool_output) {
        yyjson_doc_free(doc);
        free(input);
        post_tool_emit(true, "INVALID_HOOK_INPUT");
        return 0;
    }
    cbm_global_memory_t *global = cbm_global_memory_open_default();
    char *status = NULL;
    char *project_uuid = NULL;
    int status_rc =
        global ? cbm_global_task_status(global, NULL, session_id, turn_id, &project_uuid, &status)
               : CBM_STORE_ERR;
    if (status_rc != CBM_STORE_OK || !status) {
        free(status);
        free(project_uuid);
        if (global)
            cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        free(input);
        post_tool_emit(true, "TASK_NOT_FOUND");
        return 0;
    }
    yyjson_doc *status_doc = yyjson_read(status, strlen(status), 0);
    yyjson_val *status_root = status_doc ? yyjson_doc_get_root(status_doc) : NULL;
    const char *task_id_value = post_tool_string(status_root, "task_id");
    const char *state = post_tool_string(status_root, "state");
    char task_id[256] = {0};
    if (task_id_value)
        snprintf(task_id, sizeof(task_id), "%s", task_id_value);
    bool terminal = post_tool_terminal_state(state);
    yyjson_doc_free(status_doc);
    free(status);
    if (terminal) {
        free(project_uuid);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        free(input);
        post_tool_emit(false, NULL);
        return 0;
    }
    char *input_json = yyjson_val_write(tool_input, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    char *output_json = yyjson_val_write(tool_output, YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    char input_hash[65], output_hash[65];
    bool hashed =
        input_json && output_json &&
        cbm_stage7_sha256_hex(input_json, strlen(input_json), input_hash) == CBM_STORE_OK &&
        cbm_stage7_sha256_hex(output_json, strlen(output_json), output_hash) == CBM_STORE_OK;
    char tool_key_hash[65];
    bool key_hashed =
        cbm_stage7_sha256_hex(tool_use_id, strlen(tool_use_id), tool_key_hash) == CBM_STORE_OK;
    char evidence_material[512], evidence_hash[65];
    snprintf(evidence_material, sizeof(evidence_material), "%s:%s:%s", tool_name,
             hashed ? input_hash : "", hashed ? output_hash : "");
    bool evidence_hashed = cbm_stage7_sha256_hex(evidence_material, strlen(evidence_material),
                                                 evidence_hash) == CBM_STORE_OK;
    char result_id[96], evidence_id[96], idempotency_key[128];
    snprintf(result_id, sizeof(result_id), "result-%s", key_hashed ? tool_key_hash : "invalid");
    snprintf(evidence_id, sizeof(evidence_id), "evidence-%s",
             key_hashed ? tool_key_hash : "invalid");
    snprintf(idempotency_key, sizeof(idempotency_key), "post-tool-%s",
             key_hashed ? tool_key_hash : "invalid");
    int outcome = cbm_learning_tool_outcome(tool_name, output_json);
    cbm_task_evidence_input_t evidence = {
        .task_id = task_id,
        .result_id = result_id,
        .result_hash = output_hash,
        .evidence_id = evidence_id,
        .evidence_hash = evidence_hash,
        .evidence_trust = outcome >= 0 ? "external_verified" : "model_self_report",
        .result_status = outcome == 1 ? "failed" : "succeeded",
        .evidence_source = "runtime",
        .idempotency_key = idempotency_key,
    };
    char *report = NULL;
    int rc = hashed && key_hashed && evidence_hashed && task_id[0]
                 ? cbm_global_task_record_evidence(global, &evidence, &report)
                 : CBM_STORE_ERR;
    if ((rc == CBM_STORE_OK || rc == CBM_STORE_REPLAYED) && outcome >= 0) {
        int receipt_rc = cbm_learning_record_receipt(cbm_global_memory_store(global), evidence_id, output_hash, outcome);
        if (receipt_rc != CBM_STORE_OK && receipt_rc != CBM_STORE_REJECTED) rc = receipt_rc;
    }
    free(report);
    free(input_json);
    free(output_json);
    free(project_uuid);
    cbm_global_memory_close(global);
    yyjson_doc_free(doc);
    free(input);
    post_tool_emit(rc != CBM_STORE_OK && rc != CBM_STORE_REPLAYED,
                   rc == CBM_STORE_IDEMPOTENCY_CONFLICT ? "IDEMPOTENCY_CONFLICT"
                                                        : "EVIDENCE_FAILED");
    return 0;
}
