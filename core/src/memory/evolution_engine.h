#ifndef CBM_EVOLUTION_ENGINE_H
#define CBM_EVOLUTION_ENGINE_H

#include "memory/global_memory.h"

typedef struct {
    const char *mode; /* off | shadow | dry_run | bounded_canary | active */
    const char *task_id;
    const char *project_uuid;
    const char *memory_item_id;
    const char *operation;      /* archive | restore */
    const char *evidence_grade; /* A | B | C | D */
    const char *evidence_id;
    const char *idempotency_key;
    int isolated_write_allowed;
} cbm_evolution_memory_input_t;

typedef struct {
    const char *mode; /* shadow | dry_run | bounded_canary | active */
    const char *task_id;
    const char *project_uuid; /* production: exact global_task_workspace project */
    const char *run_id;       /* production: authorization-scoped execution identity */
    const char *idempotency_key;
    const char *manifest_path;   /* production write: absolute explicit path */
    const char *manifest_sha256; /* production write: lowercase raw-file SHA256 */
    int max_evolution_events;    /* production: 1..17; includes controller event */
    int max_cross_project_edges; /* production: 0..16 */
    int isolated_write_allowed;
    int production_gate_allowed;
    int failure_after_write;              /* isolated fault injection; 0 disables */
    int crash_during_commit;              /* isolated subprocess crash fixture; 0 disables */
    void (*snapshot_hook)(void *context); /* isolated deterministic concurrency fixture */
    void *snapshot_hook_context;
} cbm_evolution_task_input_t;

typedef struct {
    const char *mode; /* shadow | dry_run | bounded_canary | active */
    const char *project_uuid;
    const char *owner_id;
    const char *idempotency_key;
    const char *run_id; /* required for active/canary manifest binding */
    const char *edge_manifest_path;
    const char *edge_manifest_sha256;
    const char *concept_manifest_path;
    const char *concept_manifest_sha256;
    int64_t frozen_as_of_ms;
    int limit;
    int budget_seconds;
    int isolated_write_allowed;
    int production_gate_allowed;
    int failure_after_write;     /* isolated fault injection; 0 disables */
    int failure_before_report;   /* isolated pre-commit report fault; 0 disables */
    int test_budget_after_steps; /* isolated deterministic monotonic-budget fixture */
} cbm_evolution_maintenance_input_t;

typedef struct {
    char *report_json;
    int consolidated;
    int decayed;
    int archived;
    int replayed;
    int wrote;
    int eligible;
    int positive;
    int negative;
    int evolution_events;
    int cross_project_edges;
    int edge_decisions;
    int edge_transitions;
    int concept_eligible;
    int concept_proposed;
    int checkpointed;
    int planned_evolution_events;
    int planned_cross_project_edges;
    char request_sha256[65];
} cbm_evolution_result_t;

int cbm_evolution_plan_completed_task(cbm_global_memory_t *global,
                                      const cbm_evolution_task_input_t *input,
                                      cbm_evolution_result_t *out);
int cbm_evolution_apply_completed_task(cbm_global_memory_t *global,
                                       const cbm_evolution_task_input_t *input,
                                       cbm_evolution_result_t *out);
int cbm_evolution_event_chain_verify_for_test(struct sqlite3 *db, int *out_bad_sequence);

int cbm_evolution_memory_state(cbm_global_memory_t *global,
                               const cbm_evolution_memory_input_t *input,
                               cbm_evolution_result_t *out);
int cbm_evolution_maintenance(cbm_global_memory_t *global,
                              const cbm_evolution_maintenance_input_t *input,
                              cbm_evolution_result_t *out);
int cbm_evolution_maintenance_store(cbm_store_t *store,
                                    const cbm_evolution_maintenance_input_t *input,
                                    cbm_evolution_result_t *out);
void cbm_evolution_result_free(cbm_evolution_result_t *out);

#endif
