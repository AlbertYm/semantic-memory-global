/* standalone_memory_server.c - Minimal MCP server for long-term memory MVP */
#include "store/store.h"
#include "foundation/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static cbm_store_t *g_store = NULL;

/* JSON helpers */
static char *json_str(const char *s) {
    if (!s) return strdup("null");
    int len = (int)strlen(s), cap = len * 2 + 4, j = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[j++] = '"';
    for (int i = 0; i < len && j < cap - 3; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if (c < 0x20) { j += snprintf(out + j, cap - j, "\\u%04x", c); }
        else { out[j++] = c; }
    }
    out[j++] = '"'; out[j] = '\0';
    return out;
}

static char *json_get_str(const char *json, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p = strchr(p + strlen(search), '"');
    if (!p) return NULL;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    int len = (int)(end - p);
    char *out = malloc(len + 1);
    memcpy(out, p, len); out[len] = '\0';
    return out;
}

static double json_get_num(const char *json, const char *key, double def) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return def;
    p = strchr(p + strlen(search), ':');
    if (!p) return def;
    while (*p == ':' || *p == ' ') p++;
    return strtod(p, NULL);
}

static int json_get_int(const char *json, const char *key, int def) {
    return (int)json_get_num(json, key, def);
}

/* MCP protocol */
static void send_msg(const char *json) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "Content-Length: %zu\r\n\r\n%s", strlen(json), json);
    fwrite(buf, 1, strlen(buf), stdout); fflush(stdout);
}

static void send_result(const char *id, const char *result_json) {
    char buf[8192];
    snprintf(buf, sizeof(buf), "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}", id, result_json);
    send_msg(buf);
}

/* Tool definitions */
static const char *tools_json(void) {
    return "["
    "{\"name\":\"events\",\"description\":\"Write a raw long-term memory event\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"type\":{\"type\":\"string\"},\"source\":{\"type\":\"string\"},"
    "\"project\":{\"type\":\"string\"},\"payload\":{\"type\":\"string\"},"
    "\"confidence\":{\"type\":\"number\"}},\"required\":[\"type\",\"project\",\"payload\"]}},"
    "{\"name\":\"memory_consolidate\",\"description\":\"Consolidate events into memory items\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"project\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"project\"]}},"
    "{\"name\":\"admin_decay\",\"description\":\"Run explainable memory decay\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"project\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"project\"]}},"
    "{\"name\":\"memories_retrieve\",\"description\":\"Retrieve memories by structured filter or FTS\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"project\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"},"
    "\"kind\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"project\"]}},"
    "{\"name\":\"memories_inspect\",\"description\":\"List memory items for manual review\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"project\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\"}},\"required\":[\"project\"]}},"
    "{\"name\":\"memory_health\",\"description\":\"Report memory MVP health counters\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
    "\"project\":{\"type\":\"string\"}},\"required\":[\"project\"]}}]";
}

/* Tool handlers */
static char *handle_events(const char *args) {
    char *type = json_get_str(args, "type");
    char *source = json_get_str(args, "source");
    char *project = json_get_str(args, "project");
    char *payload = json_get_str(args, "payload");
    double conf = json_get_num(args, "confidence", 0.5);
    cbm_memory_event_t ev = {0};
    ev.type = type; ev.source = source ? source : "mcp.events";
    ev.project = project; ev.payload = payload; ev.confidence = conf;
    char *id = NULL;
    int rc = cbm_store_memory_append_event(g_store, &ev, &id);
    if (rc != CBM_STORE_OK) {
        free(type); free(source); free(project); free(payload);
        return strdup("{\"ok\":false,\"error\":\"event store error\"}");
    }
    /* Also create a candidate memory_item */
    cbm_memory_item_t item = {0};
    item.kind = type ? type : "event";
    item.layer = "episodic";
    item.content = payload;
    item.scope_project = project;
    item.status = "candidate";
    item.confidence = conf;
    char *item_id = NULL;
    int rc2 = cbm_store_memory_append_candidate(g_store, &item, &item_id);
    free(type); free(source); free(project); free(payload);
    if (rc2 != CBM_STORE_OK) {
        free(id); free(item_id);
        return strdup("{\"ok\":false,\"error\":\"item create error\"}");
    }
    char *jid = json_str(id);
    char buf[512];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"id\":%s}", jid);
    free(jid); free(id); free(item_id);
    return strdup(buf);
}

static char *handle_consolidate(const char *args) {
    char *project = json_get_str(args, "project");
    int limit = json_get_int(args, "limit", 100);
    int processed = 0;
    int rc = cbm_store_memory_consolidate(g_store, project, limit, &processed);
    free(project);
    if (rc != CBM_STORE_OK) return strdup("{\"ok\":false}");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"processed\":%d}", processed);
    return strdup(buf);
}

static char *handle_decay(const char *args) {
    char *project = json_get_str(args, "project");
    int limit = json_get_int(args, "limit", 100);
    int processed = 0;
    int rc = cbm_store_memory_decay(g_store, project, limit, &processed);
    free(project);
    if (rc != CBM_STORE_OK) return strdup("{\"ok\":false}");
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"processed\":%d}", processed);
    return strdup(buf);
}

static char *handle_retrieve(const char *args) {
    char *project = json_get_str(args, "project");
    char *query = json_get_str(args, "query");
    char *kind = json_get_str(args, "kind");
    int limit = json_get_int(args, "limit", 10);
    cbm_memory_query_t q = {0};
    q.project = project; q.query = query; q.kind = kind; q.limit = limit;
    cbm_memory_result_t res = {0};
    int rc = cbm_store_memory_retrieve(g_store, &q, &res);
    free(project); free(query); free(kind);
    if (rc != CBM_STORE_OK) return strdup("{\"ok\":false,\"count\":0,\"items\":[]}");
    int cap = res.count * 1024 + 256;
    char *out = malloc(cap);
    if (!out) { cbm_store_memory_result_free(&res); return strdup("{}"); }
    int pos = snprintf(out, cap, "{\"ok\":true,\"count\":%d,\"items\":[", res.count);
    for (int i = 0; i < res.count && pos < cap - 256; i++) {
        cbm_memory_item_t *it = &res.items[i];
        char *jc = json_str(it->content ? it->content : "");
        char *jt = json_str(it->title ? it->title : "");
        pos += snprintf(out + pos, cap - pos,
            "%s{\"id\":\"%s\",\"kind\":\"%s\",\"title\":%s,\"content\":%s,\"status\":\"%s\",\"retrieval_source\":\"%s\",\"retrieval_score\":%.6f,\"evidence\":%s}",
            i > 0 ? "," : "", it->id ? it->id : "",
            it->kind ? it->kind : "", jt, jc, it->status ? it->status : "",
            it->retrieval_source ? it->retrieval_source : "", it->retrieval_score,
            it->evidence_json ? it->evidence_json : "[]");
        free(jc); free(jt);
    }
    if (pos < cap - 10) snprintf(out + pos, cap - pos, "]}");
    cbm_store_memory_result_free(&res);
    return out;
}

static char *handle_inspect(const char *args) {
    char *project = json_get_str(args, "project");
    int limit = json_get_int(args, "limit", 50);
    cbm_memory_query_t q = {0};
    q.project = project; q.limit = limit; q.include_inactive = true;
    cbm_memory_result_t res = {0};
    int rc = cbm_store_memory_retrieve(g_store, &q, &res);
    free(project);
    if (rc != CBM_STORE_OK) return strdup("{\"ok\":false,\"count\":0,\"items\":[]}");
    int cap = res.count * 512 + 256;
    char *out = malloc(cap);
    if (!out) { cbm_store_memory_result_free(&res); return strdup("{}"); }
    int pos = snprintf(out, cap, "{\"ok\":true,\"count\":%d,\"items\":[", res.count);
    for (int i = 0; i < res.count && pos < cap - 256; i++) {
        cbm_memory_item_t *it = &res.items[i];
        char *jek = json_str(it->entity_key ? it->entity_key : "");
        char *jpr = json_str(it->predicate ? it->predicate : "");
        pos += snprintf(out + pos, cap - pos,
            "%s{\"id\":\"%s\",\"entity_key\":%s,\"predicate\":%s,\"status\":\"%s\"}",
            i > 0 ? "," : "", it->id ? it->id : "", jek, jpr,
            it->status ? it->status : "");
        free(jek); free(jpr);
    }
    if (pos < cap - 10) snprintf(out + pos, cap - pos, "]}");
    cbm_store_memory_result_free(&res);
    return out;
}

static char *handle_health(const char *args) {
    char *project = json_get_str(args, "project");
    cbm_memory_health_t h = {0};
    int rc = cbm_store_memory_health(g_store, project, &h);
    free(project);
    if (rc != CBM_STORE_OK) return strdup("{\"ok\":false}");
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"item_count\":%d,\"event_count\":%d,\"candidate_count\":%d,"
        "\"active_count\":%d,\"edge_count\":%d,\"hit_count\":%lld,\"conflict_count\":%d,\"hit_rate\":%.3f}",
        h.item_count, h.event_count, h.candidate_count,
        h.active_count, h.edge_count, (long long)h.total_hits, h.conflict_count, h.hit_rate);
    return strdup(buf);
}

/* Dispatch */
static void handle_message(const char *msg) {
    char *method = json_get_str(msg, "method");
    char *id = json_get_str(msg, "id");
    if (!method) { free(id); return; }

    if (strcmp(method, "initialize") == 0) {
        send_result(id ? id : "0",
            "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},"
            "\"serverInfo\":{\"name\":\"semantic-memory-mcp\",\"version\":\"0.1.0\"}}");
    }
    else if (strcmp(method, "tools/list") == 0) {
        char buf[4096];
        snprintf(buf, sizeof(buf), "{\"tools\":%s}", tools_json());
        send_result(id ? id : "0", buf);
    }
    else if (strcmp(method, "tools/call") == 0) {
        const char *params = strstr(msg, "\"params\"");
        char *tname = json_get_str(params ? params : "{}", "name");
        char *args_json = NULL;
        const char *ap = strstr(params ? params : "", "\"arguments\"");
        if (ap) {
            ap = strchr(ap, '{');
            if (ap) {
                const char *ae = ap; int depth = 0;
                while (*ae) {
                    if (*ae == '{') depth++;
                    else if (*ae == '}') { depth--; if (depth == 0) { ae++; break; } }
                    ae++;
                }
                int len = (int)(ae - ap);
                args_json = malloc(len + 1);
                memcpy(args_json, ap, len); args_json[len] = '\0';
            }
        }
        char *result = NULL;
        if (strcmp(tname, "events") == 0)
            result = handle_events(args_json ? args_json : "{}");
        else if (strcmp(tname, "memory_consolidate") == 0)
            result = handle_consolidate(args_json ? args_json : "{}");
        else if (strcmp(tname, "admin_consolidate") == 0)
            result = handle_consolidate(args_json ? args_json : "{}");
        else if (strcmp(tname, "admin_decay") == 0 || strcmp(tname, "memory_decay") == 0)
            result = handle_decay(args_json ? args_json : "{}");
        else if (strcmp(tname, "memories_retrieve") == 0)
            result = handle_retrieve(args_json ? args_json : "{}");
        else if (strcmp(tname, "memories_inspect") == 0)
            result = handle_inspect(args_json ? args_json : "{}");
        else if (strcmp(tname, "memory_health") == 0)
            result = handle_health(args_json ? args_json : "{}");
        else
            result = strdup("{\"ok\":false,\"error\":\"unknown tool\"}");
        char buf[32768];
        snprintf(buf, sizeof(buf),
            "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"content\":[{\"type\":\"text\",\"text\":%s}]}}",
            id ? id : "0", result);
        send_msg(buf);
        free(result); free(tname); free(args_json);
    }
    free(method); free(id);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *db_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc)
            db_path = argv[++i];
    }
    if (db_path) g_store = cbm_store_open(db_path);
    else { g_store = cbm_store_open_memory(); fprintf(stderr, "memory-mcp: in-memory\n"); }
    if (!g_store) { fprintf(stderr, "Failed to open store\n"); return 1; }
    fprintf(stderr, "memory-mcp ready\n"); fflush(stderr);
    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == '\r' || line[0] == '\n') continue;
        if (strncmp(line, "Content-Length:", 15) == 0) continue;
        if (strncmp(line, "Content-Type:", 13) == 0) continue;
        handle_message(line);
    }
    cbm_store_close(g_store);
    return 0;
}
