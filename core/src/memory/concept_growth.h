#ifndef CBM_CONCEPT_GROWTH_H
#define CBM_CONCEPT_GROWTH_H

#include <stdbool.h>

#include "memory/memory_store.h"

#define CBM_STAGE10_ALGORITHM_VERSION "stage10-concept-growth-v1"
#define CBM_STAGE10_POLICY_VERSION 1
#define CBM_STAGE10_CONFIG_VERSION 1
#define CBM_STAGE10_GENERATOR_VERSION "deterministic-local-v1"
#define CBM_STAGE10_POLICY_SHA256 "38e32e2c0fbe224db8e6ca04fb1aff58b38cbc7255904b70563c3d48018746b4"
#define CBM_STAGE10_MIGRATION_SHA256 \
    "f841199fbdd0db359dede7619f2d71faf3f746e51560f359e41e74fa511f7e2d"

typedef struct {
    const char *project;
    const char *store;
    const char *operation; /* evaluate | generate */
    const char *mode;      /* off | shadow | dry_run | active */
    const char *run_id;
    const char *idempotency_key;
    const char *algorithm_version;
    const char *policy_sha256;
    int policy_version;
    int config_version;
    const char *generator_version;
    const char *manifest_path;
    const char *manifest_sha256;
} cbm_concept_generate_input_t;

typedef struct {
    const char *project;
    const char *store;
    const char *candidate_id;
    const char *action; /* approve | edit | reject | withdraw */
    const char *idempotency_key;
    const char *content_text;
    const char *related_candidate_id;
    const char *reviewer_source; /* explicit_user | fixture */
} cbm_concept_review_input_t;

typedef struct {
    char *report_json;
    int eligible_count;
    int proposed_count;
    bool production_state_written;
    const char *failure_code;
} cbm_concept_result_t;

int cbm_store_memory_stage10_migrate(cbm_store_t *store);
int cbm_store_memory_stage10_object_count(cbm_store_t *store);
int cbm_store_memory_concept_generate(cbm_store_t *store, const cbm_concept_generate_input_t *input,
                                      cbm_concept_result_t *out);
/* Stage 14 controller-only variant. The caller owns the transaction, must provide its running
 * controller run id, and may bridge authorization already verified by the Stage 14 parent. */
int cbm_store_memory_concept_generate_in_transaction(cbm_store_t *store,
                                                     const cbm_concept_generate_input_t *input,
                                                     const char *controller_run_id,
                                                     bool stage14_parent_authorized,
                                                     cbm_concept_result_t *out);
int cbm_store_memory_concept_review(cbm_store_t *store, const cbm_concept_review_input_t *input,
                                    cbm_concept_result_t *out);
int cbm_store_memory_concept_inspect(cbm_store_t *store, const char *project,
                                     const char *store_name, const char *candidate_id,
                                     cbm_concept_result_t *out);
int cbm_store_memory_stage10_audit_verify(cbm_store_t *store, int *growth_count, int *review_count);
void cbm_store_memory_concept_result_free(cbm_concept_result_t *result);

#endif
