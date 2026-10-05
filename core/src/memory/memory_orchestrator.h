#ifndef CBM_MEMORY_ORCHESTRATOR_H
#define CBM_MEMORY_ORCHESTRATOR_H

#include "memory/memory_store.h"

#include <stdbool.h>
#include <stddef.h>

#define CBM_STAGE12_SCHEMA_VERSION 1
#define CBM_STAGE12_COMPONENT "stage12_task_orchestrator"
#define CBM_STAGE12_MIGRATION_NAME "codex_task_lifecycle_attribution_v1"
#define CBM_STAGE12_MIGRATION_SHA256 \
    "b10b6dc9d4e25c903c051bb314bd805e95ee4ccb5435822641dbd303958039f5"
#define CBM_STAGE12_POLICY_VERSION "stage12-v1"

typedef struct {
    const char *project;
    const char *session_id;
    const char *turn_id;
    const char *prompt_sha256;
    int prompt_length;
    const char *retrieval_session_id;
    const char *idempotency_key;
} cbm_task_begin_input_t;

typedef struct {
    const char *task_id;
    const char *result_id;
    const char *result_hash;
    const char *evidence_id;
    const char *evidence_hash;
    const char *evidence_trust;
    const char *evidence_source;
    const char *idempotency_key;
} cbm_task_evidence_input_t;

typedef struct {
    const char *memory_item_id;
    const char *state;
    const char *evidence_id;
    const char *feedback_event_id;
} cbm_task_attribution_input_t;

typedef struct {
    const char *project;
    const char *task_id;
    const char *outcome;
    const char *idempotency_key;
    const cbm_task_attribution_input_t *attributions;
    size_t attribution_count;
} cbm_task_complete_input_t;

int cbm_orchestrator_migrate(cbm_store_t *store, bool *out_replayed, char **out_report_json);
int cbm_orchestrator_begin(cbm_store_t *store, const cbm_task_begin_input_t *input,
                           char **out_report_json);
int cbm_orchestrator_status(cbm_store_t *store, const char *project, const char *task_id,
                            const char *session_id, const char *turn_id, char **out_report_json);
int cbm_orchestrator_record_evidence(cbm_store_t *store, const cbm_task_evidence_input_t *input,
                                     char **out_report_json);
int cbm_orchestrator_complete(cbm_store_t *store, const cbm_task_complete_input_t *input,
                              char **out_report_json);
int cbm_orchestrator_abandon_open(cbm_store_t *store, const char *project, const char *session_id,
                                  const char *turn_id, const char *idempotency_key,
                                  char **out_report_json);

#endif
