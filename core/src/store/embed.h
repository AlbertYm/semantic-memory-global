/*
 * embed.h — Pluggable memory-embedding backend.
 *
 * The memory subsystem turns text into an int8 vector for cosine search. Two
 * backends:
 *   - "static" (default): the in-binary nomic lookup table + sparse-random
 *     fallback (zero runtime deps; see memory_feature_vec in store.c). It cannot
 *     bridge meaning across differing wording — a known ceiling.
 *   - "sidecar": a local child process running a real multilingual sentence
 *     model (bge-m3) that the C process talks to over newline-delimited JSON.
 *     Real semantic recall, at the cost of a Python+model runtime dependency.
 *
 * Selected by env CBM_MEMORY_EMBED_BACKEND ∈ {static, sidecar}; default static.
 * The sidecar is best-effort: any spawn/IO failure degrades to static for the
 * rest of the process so embedding never blocks or fails a caller.
 */
#ifndef CBM_EMBED_H
#define CBM_EMBED_H

#include <stdint.h>

enum {
    CBM_EMBED_STATIC = 0,
    CBM_EMBED_SIDECAR = 1,
};

/* The active backend for this process (cached after first read of the env). */
int cbm_embed_backend(void);

/* Embed `text` into out[0..dim-1] as an int8 (x127) unit vector via the sidecar.
 * Returns 0 on success, non-zero on any failure (caller should fall back to the
 * static embedder). Only meaningful when cbm_embed_backend()==CBM_EMBED_SIDECAR;
 * a failure here also degrades the backend to static for subsequent calls. */
int cbm_embed_text(const char *text, int8_t *out, int dim);

/* Tear down the sidecar process if one was started. Idempotent. */
void cbm_embed_shutdown(void);

/* Test-only: forget the cached backend selection and tear down any sidecar, so
 * a subsequent cbm_embed_backend() re-reads the env. Lets a single test process
 * exercise both backends. Not for production use. */
void cbm_embed_reset_for_test(void);

#endif /* CBM_EMBED_H */
