#ifndef CBM_VERIFIED_LEARNING_H
#define CBM_VERIFIED_LEARNING_H
#include "memory/memory_store.h"

/* A separate bounded controller; never enables the legacy purge hot path. */
int cbm_learning_refresh(cbm_store_t *store, int64_t now_ms);
void cbm_learning_rank(cbm_store_t *store, cbm_memory_result_t *result);
char *cbm_learning_status(cbm_store_t *store, const char *project);
int cbm_learning_control(cbm_store_t *store, const char *project, const char *action,
                         const char *item_id, const char *request_id,
                         int expected_generation, int64_t now_ms);
/* Strict structured command result, never infer success from free-form prose. */
int cbm_learning_tool_outcome(const char *tool_name, const char *output_json);
int cbm_learning_record_receipt(cbm_store_t *store, const char *evidence_id,
                                const char *result_hash, int outcome);
#endif
