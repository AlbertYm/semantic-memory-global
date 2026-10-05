#include "cli/cli.h"

#include "foundation/platform.h"
#include "mcp/mcp.h"
#include "memory/global_memory.h"
#include "memory/memory_orchestrator.h"
#include "store/store.h"
#include "yyjson/yyjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STOP_STDIN_CAP (256 * 1024)

static char *stop_read_stdin(void) {
    char *buffer = malloc(STOP_STDIN_CAP + 1);
    if (!buffer)
        return NULL;
    size_t total = 0;
    while (total < STOP_STDIN_CAP) {
        size_t count = fread(buffer + total, 1, STOP_STDIN_CAP - total, stdin);
        if (count == 0)
            break;
        total += count;
    }
    buffer[total] = '\0';
    return buffer;
}

static const char *stop_string(yyjson_val *root, const char *key) {
    yyjson_val *value = root && yyjson_is_obj(root) ? yyjson_obj_get(root, key) : NULL;
    return value && yyjson_is_str(value) ? yyjson_get_str(value) : NULL;
}

static void stop_emit(bool allow, const char *reason, const char *message) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        yyjson_mut_doc_free(doc);
        return;
    }
    yyjson_mut_doc_set_root(doc, root);
    if (allow) {
        yyjson_mut_obj_add_bool(doc, root, "continue", true);
    } else {
        const char *block_reason =
            reason && reason[0] ? reason : "Complete the memory lifecycle before stopping.";
        yyjson_mut_obj_add_str(doc, root, "decision", "block");
        yyjson_mut_obj_add_str(doc, root, "reason", block_reason);
    }
    if (message)
        yyjson_mut_obj_add_str(doc, root, "systemMessage", message);
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    if (json) {
        fputs(json, stdout);
        free(json);
    }
}

int cbm_cmd_memory_stop(void) {
    char *input = stop_read_stdin();
    yyjson_doc *doc = input ? yyjson_read(input, strlen(input), 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    const char *event = stop_string(root, "hook_event_name");
    const char *session_id = stop_string(root, "session_id");
    const char *turn_id = stop_string(root, "turn_id");
    yyjson_val *active_value = root ? yyjson_obj_get(root, "stop_hook_active") : NULL;
    bool active = active_value && yyjson_is_bool(active_value) && yyjson_is_true(active_value);
    if (!root || (event && strcmp(event, "Stop") != 0) || !session_id || !turn_id) {
        yyjson_doc_free(doc);
        free(input);
        stop_emit(true, NULL, "memory lifecycle degraded: invalid Stop payload");
        return 0;
    }
    cbm_global_memory_t *global = cbm_global_memory_open_default();
    char *status = NULL;
    char *project_uuid = NULL;
    int rc = global
                 ? cbm_global_task_status(global, NULL, session_id, turn_id, &project_uuid, &status)
                 : CBM_STORE_ERR;
    if (rc != CBM_STORE_OK || !status) {
        free(status);
        free(project_uuid);
        if (global)
            cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        free(input);
        stop_emit(true, NULL, "memory lifecycle degraded: task unavailable");
        return 0;
    }
    yyjson_doc *status_doc = yyjson_read(status, strlen(status), 0);
    yyjson_val *status_root = status_doc ? yyjson_doc_get_root(status_doc) : NULL;
    const char *state = stop_string(status_root, "state");
    char state_copy[64] = {0};
    if (state)
        snprintf(state_copy, sizeof(state_copy), "%s", state);
    yyjson_doc_free(status_doc);
    free(status);
    bool closed = strcmp(state_copy, "completed") == 0 || strcmp(state_copy, "failed") == 0 ||
                  strcmp(state_copy, "cancelled") == 0 || strcmp(state_copy, "abandoned") == 0;
    if (closed) {
        free(project_uuid);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        free(input);
        stop_emit(true, NULL, NULL);
        return 0;
    }
    if (!active) {
        free(project_uuid);
        cbm_global_memory_close(global);
        yyjson_doc_free(doc);
        free(input);
        stop_emit(false, "Complete the task evidence and feedback lifecycle before stopping.",
                  NULL);
        return 0;
    }
    char key_material[768], key_hash[65], abandon_key[96];
    snprintf(key_material, sizeof(key_material), "%s:%s:abandoned", session_id, turn_id);
    cbm_stage7_sha256_hex(key_material, strlen(key_material), key_hash);
    snprintf(abandon_key, sizeof(abandon_key), "stop-abandon-%s", key_hash);
    char *report = NULL;
    int abandon_rc = cbm_global_task_abandon(global, session_id, turn_id, abandon_key, &report);
    free(report);
    free(project_uuid);
    cbm_global_memory_close(global);
    yyjson_doc_free(doc);
    free(input);
    stop_emit(true, NULL,
              abandon_rc == CBM_STORE_OK || abandon_rc == CBM_STORE_REPLAYED
                  ? "memory lifecycle abandoned after one bounded continuation"
                  : "memory lifecycle degraded: bounded abandon failed");
    return 0;
}
