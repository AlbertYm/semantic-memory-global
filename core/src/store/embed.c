/*
 * embed.c — Pluggable memory-embedding backend (see embed.h).
 *
 * The sidecar backend lazily spawns a long-lived child (default
 * `python scripts/embed_sidecar.py`, override via CBM_MEMORY_EMBED_CMD) and
 * speaks newline-delimited JSON over its stdin/stdout:
 *     -> {"id":N,"text":"..."}
 *     <- {"id":N,"vec":[f0,f1,...]}
 * The returned float vector is L2-normalized then quantized to int8 (x127),
 * matching the storage format the cosine SQL function expects.
 *
 * Robustness: a spawn failure, IO error, malformed reply, or dimension
 * mismatch flips the backend to "degraded" (static) for the rest of the
 * process, so embedding is always best-effort and never blocks a caller
 * indefinitely beyond one request.
 */
#include "store/embed.h"
#include "foundation/compat_proc.h"
#include "foundation/compat_thread.h"
#include "foundation/platform.h"

#include <yyjson/yyjson.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── backend selection ─────────────────────────────────────────── */

static _Atomic int g_backend = -1; /* -1 = unread, else CBM_EMBED_* */

int cbm_embed_backend(void) {
    int b = atomic_load_explicit(&g_backend, memory_order_acquire);
    if (b >= 0) {
        return b;
    }
    char buf[32];
    cbm_safe_getenv("CBM_MEMORY_EMBED_BACKEND", buf, sizeof(buf), NULL);
    int resolved = CBM_EMBED_STATIC;
    if (buf[0] && strcmp(buf, "sidecar") == 0) {
        resolved = CBM_EMBED_SIDECAR;
    }
    atomic_store_explicit(&g_backend, resolved, memory_order_release);
    return resolved;
}

/* ── sidecar lifecycle ─────────────────────────────────────────── */

static char *strdup_safe(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

static cbm_mutex_t g_mtx;
static _Atomic int g_mtx_init = 0;
static cbm_proc_t *g_proc = NULL; /* live sidecar, or NULL */
static int g_degraded = 0;        /* set once a failure forces static */
static int64_t g_req_id = 0;

static void ensure_mtx(void) {
    /* First-caller initializes the mutex. Racy only before any embed call has
     * happened; in practice ensure_pretrained-style init is fine here because
     * the first memory write/retrieve is single-threaded at startup. */
    if (!atomic_load_explicit(&g_mtx_init, memory_order_acquire)) {
        static _Atomic int initializing = 0;
        int expected = 0;
        if (atomic_compare_exchange_strong(&initializing, &expected, 1)) {
            cbm_mutex_init(&g_mtx);
            atomic_store_explicit(&g_mtx_init, 1, memory_order_release);
        } else {
            while (!atomic_load_explicit(&g_mtx_init, memory_order_acquire)) {
                /* spin briefly until the winner finishes init */
            }
        }
    }
}

/* Build argv for the sidecar command. Returns a NULL-terminated array the
 * caller frees with free_argv. Splits CBM_MEMORY_EMBED_CMD on spaces (simple;
 * our default has none in paths). Default: python scripts/embed_sidecar.py */
static char **build_argv(void) {
    char cmd[512];
    cbm_safe_getenv("CBM_MEMORY_EMBED_CMD", cmd, sizeof(cmd), NULL);
    if (!cmd[0]) {
        snprintf(cmd, sizeof(cmd), "python scripts/embed_sidecar.py");
    }
    /* tokenize on spaces */
    int cap = 8, n = 0;
    char **argv = (char **)calloc((size_t)cap, sizeof(char *));
    if (!argv) {
        return NULL;
    }
    char *save = NULL;
    for (char *tok = strtok_r(cmd, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        if (n + 1 >= cap) {
            cap *= 2;
            char **na = (char **)realloc(argv, (size_t)cap * sizeof(char *));
            if (!na) {
                break;
            }
            argv = na;
        }
        argv[n++] = tok ? strdup_safe(tok) : NULL;
    }
    argv[n] = NULL;
    return argv;
}

static void free_argv(char **argv) {
    if (!argv) {
        return;
    }
    for (int i = 0; argv[i]; i++) {
        free(argv[i]);
    }
    free(argv);
}

/* Spawn the sidecar if not running and not degraded. Caller holds g_mtx.
 * Returns 0 on a usable g_proc, non-zero otherwise. */
static int ensure_proc_locked(void) {
    if (g_degraded) {
        return 1;
    }
    if (g_proc) {
        return 0;
    }
    char **argv = build_argv();
    if (!argv || !argv[0]) {
        free_argv(argv);
        g_degraded = 1;
        return 1;
    }
    g_proc = cbm_proc_spawn((const char *const *)argv);
    free_argv(argv);
    if (!g_proc) {
        g_degraded = 1; /* spawn failed: don't retry every call */
        return 1;
    }
    return 0;
}

/* JSON-escape `text` into a request line, send it, read one reply line, parse
 * the "vec" array. Caller holds g_mtx. Returns 0 on success. */
static int sidecar_embed_locked(const char *text, int8_t *out, int dim) {
    int64_t id = ++g_req_id;

    /* Build request {"id":N,"text":"..."} using yyjson for correct escaping. */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return 1;
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_int(doc, root, "id", id);
    yyjson_mut_obj_add_str(doc, root, "text", text ? text : "");
    size_t len = 0;
    char *req = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!req) {
        return 1;
    }

    int rc = cbm_proc_write_line(g_proc, req);
    free(req);
    if (rc != 0) {
        return 1;
    }

    /* Read reply lines until one parses as JSON with our id (skip any stray
     * non-JSON noise the child may emit on stdout). Bounded attempts. */
    char *line = (char *)malloc(1 << 16);
    if (!line) {
        return 1;
    }
    int result = 1;
    for (int attempt = 0; attempt < 4; attempt++) {
        long n = cbm_proc_read_line(g_proc, line, 1 << 16);
        if (n < 0) {
            break; /* EOF/error */
        }
        if (n == 0) {
            continue;
        }
        yyjson_doc *rd = yyjson_read(line, (size_t)n, 0);
        if (!rd) {
            continue;
        }
        yyjson_val *rr = yyjson_doc_get_root(rd);
        yyjson_val *vec = yyjson_obj_get(rr, "vec");
        if (!vec || !yyjson_is_arr(vec)) {
            yyjson_doc_free(rd);
            continue;
        }
        size_t got = yyjson_arr_size(vec);
        if ((int)got != dim) {
            yyjson_doc_free(rd);
            break; /* dimension mismatch: hard fail (degrade) */
        }
        /* Read floats, L2-normalize, quantize to int8 x127. */
        double *fv = (double *)malloc(sizeof(double) * (size_t)dim);
        if (!fv) {
            yyjson_doc_free(rd);
            break;
        }
        size_t idx = 0;
        yyjson_val *v;
        yyjson_arr_iter it = yyjson_arr_iter_with(vec);
        double mag = 0.0;
        while ((v = yyjson_arr_iter_next(&it)) && idx < (size_t)dim) {
            double f = yyjson_get_real(v);
            if (!yyjson_is_real(v) && yyjson_is_int(v)) {
                f = (double)yyjson_get_int(v);
            }
            fv[idx] = f;
            mag += f * f;
            idx++;
        }
        mag = sqrt(mag);
        if (mag < 1e-9) {
            mag = 1.0;
        }
        for (int i = 0; i < dim; i++) {
            double q = (fv[i] / mag) * 127.0;
            if (q > 127.0) {
                q = 127.0;
            }
            if (q < -127.0) {
                q = -127.0;
            }
            out[i] = (int8_t)lround(q);
        }
        free(fv);
        yyjson_doc_free(rd);
        result = 0;
        break;
    }
    free(line);
    return result;
}

int cbm_embed_text(const char *text, int8_t *out, int dim) {
    if (cbm_embed_backend() != CBM_EMBED_SIDECAR || !out || dim <= 0) {
        return 1;
    }
    ensure_mtx();
    cbm_mutex_lock(&g_mtx);
    int rc = ensure_proc_locked();
    if (rc == 0) {
        rc = sidecar_embed_locked(text, out, dim);
        if (rc != 0) {
            /* A live-sidecar failure (broken pipe, bad reply, dim mismatch):
             * tear it down and degrade so we stop trying this process. */
            cbm_proc_close(g_proc);
            g_proc = NULL;
            g_degraded = 1;
        }
    }
    cbm_mutex_unlock(&g_mtx);
    return rc;
}

void cbm_embed_shutdown(void) {
    if (!atomic_load_explicit(&g_mtx_init, memory_order_acquire)) {
        return;
    }
    cbm_mutex_lock(&g_mtx);
    if (g_proc) {
        cbm_proc_close(g_proc);
        g_proc = NULL;
    }
    cbm_mutex_unlock(&g_mtx);
}

void cbm_embed_reset_for_test(void) {
    cbm_embed_shutdown();
    if (atomic_load_explicit(&g_mtx_init, memory_order_acquire)) {
        cbm_mutex_lock(&g_mtx);
        g_degraded = 0;
        g_req_id = 0;
        cbm_mutex_unlock(&g_mtx);
    }
    atomic_store_explicit(&g_backend, -1, memory_order_release);
}
