/*
 * store_internal.h — Shared internal definition of struct cbm_store and
 * common helpers. Included by both store/store.c (code graph) and
 * memory/memory_store.c (local-fork memory/ADR layer).
 *
 * This file is NOT part of the public API — external code includes store.h
 * or memory/memory_store.h only.
 */
#ifndef CBM_STORE_INTERNAL_H
#define CBM_STORE_INTERNAL_H

#include "store/store.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Internal store structure ───────────────────────────────────── */

struct cbm_store {
    sqlite3 *db;
    const char *db_path; /* heap-allocated, or NULL for :memory: */
    char errbuf[512];

    /* Prepared statements (lazily initialized, cached for lifetime) */
    sqlite3_stmt *stmt_upsert_node;
    sqlite3_stmt *stmt_find_node_by_id;
    sqlite3_stmt *stmt_find_node_by_qn;
    sqlite3_stmt *stmt_find_node_by_qn_any; /* QN lookup without project filter */
    sqlite3_stmt *stmt_find_nodes_by_name;
    sqlite3_stmt *stmt_find_nodes_by_name_any; /* name lookup without project filter */
    sqlite3_stmt *stmt_find_nodes_by_label;
    sqlite3_stmt *stmt_find_nodes_by_file;
    sqlite3_stmt *stmt_count_nodes;
    sqlite3_stmt *stmt_delete_nodes_by_project;
    sqlite3_stmt *stmt_delete_nodes_by_file;
    sqlite3_stmt *stmt_delete_nodes_by_label;

    sqlite3_stmt *stmt_insert_edge;
    sqlite3_stmt *stmt_find_edges_by_source;
    sqlite3_stmt *stmt_find_edges_by_target;
    sqlite3_stmt *stmt_find_edges_by_source_type;
    sqlite3_stmt *stmt_find_edges_by_target_type;
    sqlite3_stmt *stmt_find_edges_by_type;
    sqlite3_stmt *stmt_count_edges;
    sqlite3_stmt *stmt_count_edges_by_type;
    sqlite3_stmt *stmt_delete_edges_by_project;
    sqlite3_stmt *stmt_delete_edges_by_type;

    sqlite3_stmt *stmt_upsert_project;
    sqlite3_stmt *stmt_get_project;
    sqlite3_stmt *stmt_list_projects;
    sqlite3_stmt *stmt_delete_project;

    sqlite3_stmt *stmt_upsert_file_hash;
    sqlite3_stmt *stmt_get_file_hashes;
    sqlite3_stmt *stmt_delete_file_hash;
    sqlite3_stmt *stmt_delete_file_hashes;
};

/* ── Shared helpers (needed by memory_store.c which knows s->db etc.) ── */

static inline void store_set_error_sqlite(cbm_store_t *s, const char *prefix) {
    snprintf(s->errbuf, sizeof(s->errbuf), "%s: %s", prefix, sqlite3_errmsg(s->db));
}

/* CBM_NOT_FOUND is defined in foundation/constants.h; included transitively
 * via store/store.h. Do NOT redefine here — that causes enum clash. */

/* Convenience macro: sizeof string literal minus NUL. */
#define SLEN(s) (sizeof(s) - 1)

static inline const char *safe_str(const char *s) {
    return s ? s : "";
}

/* Isolate the int-to-ptr cast that SQLITE_TRANSIENT expands to.
 * A union type-pun avoids the performance-no-int-to-ptr diagnostic. */
static inline sqlite3_destructor_type make_transient(void) {
    union {
        uintptr_t i;
        sqlite3_destructor_type fn;
    } u;
    u.i = (uintptr_t)CBM_NOT_FOUND;
    return u.fn;
}
#define BIND_TRANSIENT (make_transient())

static inline int bind_text(sqlite3_stmt *s, int col, const char *v) {
    return sqlite3_bind_text(s, col, v, CBM_NOT_FOUND, BIND_TRANSIENT);
}

/* Duplicate a string onto the heap. */
static inline char *heap_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    if (d) {
        memcpy(d, s, len + 1);
    }
    return d;
}

#ifdef __cplusplus
}
#endif

#endif /* CBM_STORE_INTERNAL_H */
