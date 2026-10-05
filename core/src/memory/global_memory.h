#ifndef CBM_GLOBAL_MEMORY_H
#define CBM_GLOBAL_MEMORY_H

#include "memory/memory_orchestrator.h"
#include "memory/memory_store.h"
#include "memory/project_resolver.h"

#include <stddef.h>

typedef struct cbm_global_memory cbm_global_memory_t;

typedef struct {
    cbm_memory_item_t item;
    char *project_uuid;
    char *legacy_project_id;
    char *source_kind;
    char *candidate_id;
    char *provenance_id;
    char *evidence_id;
    char *content_hash;
    double global_score;
    int project_soft_boost_ppm;
} cbm_global_candidate_t;

typedef struct {
    cbm_global_candidate_t *items;
    int count;
    int total;
    char *session_id;
    char *candidate_pool;
} cbm_global_retrieval_result_t;

cbm_global_memory_t *cbm_global_memory_open(const char *memory_db_path,
                                            const char *global_graph_db_path);
/* Open the shared <cache>/__global__ Memory and graph stores. */
cbm_global_memory_t *cbm_global_memory_open_default(void);
void cbm_global_memory_close(cbm_global_memory_t *global);
cbm_store_t *cbm_global_memory_store(cbm_global_memory_t *global);
struct sqlite3 *cbm_global_memory_db(cbm_global_memory_t *global);
struct sqlite3 *cbm_global_graph_db(cbm_global_memory_t *global);

int cbm_global_memory_migrate(cbm_global_memory_t *global, int *out_replayed);
int cbm_global_store_migrate(cbm_store_t *store, int *out_replayed);

/* Ensures one catalog identity. Path replay is zero-write; a moved workspace
 * with the same source fingerprint reuses the prior UUID and appends an alias. */
int cbm_global_ensure_project(cbm_global_memory_t *global,
                              const cbm_project_resolution_t *resolution,
                              const char *idempotency_key, char **out_report_json);
int cbm_global_store_ensure_project(cbm_store_t *store, const cbm_project_resolution_t *resolution,
                                    const char *idempotency_key, char **out_report_json);

int cbm_global_task_begin(cbm_global_memory_t *global, const cbm_project_resolution_t *resolution,
                          const cbm_task_begin_input_t *input, char **out_report_json);
int cbm_global_task_record_evidence(cbm_global_memory_t *global,
                                    const cbm_task_evidence_input_t *input, char **out_report_json);
/* Project is discovered from the task/session identity in the global store. */
int cbm_global_task_status(cbm_global_memory_t *global, const char *task_id, const char *session_id,
                           const char *turn_id, char **out_project_uuid, char **out_report_json);
int cbm_global_store_task_status(cbm_store_t *store, const char *task_id, const char *session_id,
                                 const char *turn_id, char **out_project_uuid,
                                 char **out_report_json);
int cbm_global_task_complete(cbm_global_memory_t *global, const cbm_task_complete_input_t *input,
                             char **out_report_json);
int cbm_global_task_abandon(cbm_global_memory_t *global, const char *session_id,
                            const char *turn_id, const char *idempotency_key,
                            char **out_report_json);

/* Global candidate pool with project only as a bounded ranking signal. */
int cbm_global_memory_retrieve(cbm_global_memory_t *global, const char *session_id,
                               const char *current_project_uuid, int soft_boost_ppm,
                               const cbm_memory_query_t *query, cbm_global_retrieval_result_t *out);
int cbm_global_store_retrieve(cbm_store_t *store, const char *session_id,
                              const char *current_project_uuid, int soft_boost_ppm,
                              const cbm_memory_query_t *query, cbm_global_retrieval_result_t *out);
void cbm_global_retrieval_result_free(cbm_global_retrieval_result_t *out);

int cbm_global_cross_project_edge(cbm_global_memory_t *global, const char *edge_id,
                                  const char *source_project_uuid, const char *target_project_uuid,
                                  const char *relation_type, int weight_ppm, int confidence_ppm,
                                  const char *status, int version, const char *evidence_event_id,
                                  const char *idempotency_key, char **out_report_json);

typedef struct {
    const char *source_memory_path;
    const char *source_graph_path;
    const char *source_config_path;
    const char *target_root;
    const char *project_path;
    const char *idempotency_key;
    const char *mode; /* plan | apply | verify */
} cbm_global_migration_input_t;

int cbm_global_migration_execute(const cbm_global_migration_input_t *input, char **out_report_json);
/* Read-only verification of an already managed target and its applied ledger. */
int cbm_global_migration_verify_existing(const cbm_global_migration_input_t *input,
                                         char **out_report_json);

#endif
