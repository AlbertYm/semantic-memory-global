#ifndef CBM_EDGE_LIFECYCLE_H
#define CBM_EDGE_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "memory/memory_store.h"

#define CBM_STAGE9_ALGORITHM_VERSION "stage9-edge-lifecycle-v1"
#define CBM_STAGE9_POLICY_VERSION 1
#define CBM_STAGE9_CONFIG_VERSION 1
#define CBM_STAGE9_POLICY_SHA256 "8ba8ca0610997d4ffd6a0d2e5e98460a7ec3bf7b2fae2bdfaef9fd41d5802b0c"
#define CBM_STAGE9_MIGRATION_SHA256 \
    "f9f07da56d4001d82f94bfc7310f7b27d7d4bcce04c87dac825434233aa4241f"

typedef struct {
    const char *project;
    const char *mode; /* off | shadow | dry_run | active */
    const char *run_id;
    int64_t as_of_ms;
    const char *algorithm_version;
    const char *policy_sha256;
    int policy_version;
    int config_version;
    const char *manifest_path;
    const char *manifest_sha256;
} cbm_edge_lifecycle_input_t;

typedef struct {
    const char *const *edge_ids;
    int edge_count;
    cbm_edge_lifecycle_input_t lifecycle;
} cbm_edge_lifecycle_restore_input_t;

typedef struct {
    char *report_json;
    int decision_count;
    int transition_count;
    int recorded_count;
    int replayed_count;
} cbm_edge_lifecycle_result_t;

int cbm_store_memory_stage9_migrate(cbm_store_t *store);
int cbm_store_memory_edge_maintenance(cbm_store_t *store, const cbm_edge_lifecycle_input_t *input,
                                      cbm_edge_lifecycle_result_t *out);
/* Stage 14 controller-only variant. The caller owns the transaction, must provide its running
 * controller run id, and may bridge authorization already verified by the Stage 14 parent. */
int cbm_store_memory_edge_maintenance_in_transaction(cbm_store_t *store,
                                                     const cbm_edge_lifecycle_input_t *input,
                                                     const char *controller_run_id,
                                                     bool stage14_parent_authorized,
                                                     cbm_edge_lifecycle_result_t *out);
int cbm_store_memory_edge_restore(cbm_store_t *store,
                                  const cbm_edge_lifecycle_restore_input_t *input,
                                  cbm_edge_lifecycle_result_t *out);
void cbm_store_memory_edge_lifecycle_result_free(cbm_edge_lifecycle_result_t *result);
int cbm_store_memory_stage9_audit_verify(cbm_store_t *store, int *out_count);
int cbm_store_memory_stage9_object_count(cbm_store_t *store);
bool cbm_store_memory_edge_allows_propagation(cbm_store_t *store, const char *edge_id);

#endif
