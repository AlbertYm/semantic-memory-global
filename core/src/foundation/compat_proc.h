/*
 * compat_proc.h — Portable bidirectional child-process pipe.
 *
 * Unlike cbm_popen (compat_fs.h), which gives a one-directional pipe (read OR
 * write), this spawns a child with BOTH its stdin and stdout connected back to
 * the parent, so the parent can write a request and read a response over a
 * persistent process. Used for the embedding sidecar (a long-lived helper the
 * memory subsystem talks to over newline-delimited JSON).
 *
 * The child's stderr is left attached to the parent's stderr (diagnostics pass
 * through). Line-oriented: callers write one line, read one line.
 */
#ifndef CBM_COMPAT_PROC_H
#define CBM_COMPAT_PROC_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cbm_proc cbm_proc_t;

/* Spawn argv[0] with argv as its argument vector (NULL-terminated), wiring the
 * child's stdin/stdout to pipes owned by the returned handle. Returns NULL on
 * failure. The child inherits the parent's environment. */
cbm_proc_t *cbm_proc_spawn(const char *const *argv);

/* Write a line to the child's stdin. A trailing '\n' is appended if `line`
 * does not already end in one. Returns 0 on success, non-zero on error
 * (e.g. broken pipe / dead child). */
int cbm_proc_write_line(cbm_proc_t *p, const char *line);

/* Read one '\n'-terminated line from the child's stdout into buf (NUL-
 * terminated, trailing newline stripped). Blocks until a full line, EOF, or
 * error. Returns the byte length on success (>=0), or -1 on EOF/error.
 * Lines longer than buf_sz-1 are truncated to fit. */
long cbm_proc_read_line(cbm_proc_t *p, char *buf, size_t buf_sz);

/* Terminate (if still running) and reap the child, close pipes, free handle.
 * Safe to call with NULL. */
void cbm_proc_close(cbm_proc_t *p);

#endif /* CBM_COMPAT_PROC_H */
