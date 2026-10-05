#ifndef CBM_MEMORY_SECURITY_H
#define CBM_MEMORY_SECURITY_H

#include <stdbool.h>
#include <stddef.h>

#define CBM_MEMORY_SECURITY_POLICY_ID "stage11-hardening-v1"
#define CBM_MEMORY_SECURITY_POLICY_VERSION 1
#define CBM_MEMORY_SECURITY_DETECTOR_VERSION "stage11-local-deterministic-v1"
#define CBM_MEMORY_SECURITY_SHA256_HEX_SIZE 65
#define CBM_MEMORY_SECURITY_MAX_INPUT_BYTES (1024U * 1024U)

typedef struct {
    bool allowed;
    const char *code;
    const char *category;
    const char *action;
    const char *reason_code;
    char content_sha256[CBM_MEMORY_SECURITY_SHA256_HEX_SIZE];
    size_t content_length;
} cbm_memory_security_result_t;

/* Classify untrusted UTF-8 content without persistence or network access. */
int cbm_memory_security_scan(const char *content, size_t content_length,
                             cbm_memory_security_result_t *out);

/* Scope syntax gate. Store must be project-memory; paths, aliases and empty
 * values are denied. Existence remains the MCP resolver's responsibility. */
bool cbm_memory_security_scope_allowed(const char *project, const char *store);

/* Only a successful safe classification may enter the observe-injection path. */
bool cbm_memory_security_injection_allowed(const char *classifier_status,
                                           const char *classification);

#endif
