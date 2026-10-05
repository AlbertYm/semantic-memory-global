/*
 * memory_store.h — Local-fork memory/ADR extensions for the semantic-memory-mcp
 * project. These types and functions were originally embedded in store/store.h
 * alongside the code-graph layer. They are extracted here so memory-layer code
 * can evolve independently while the upstream store.h stays aligned with the
 * graph-only reference implementation.
 *
 * All memory data lives in a separate SQLite file (<cache>/<project>-memory.db)
 * so rebuilding the code graph never destroys memory.
 */

#ifndef CBM_MEMORY_STORE_H
#define CBM_MEMORY_STORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Forward declarations ──────────────────────────────────────────── */

typedef struct cbm_store cbm_store_t;
struct sqlite3;

/* Memory-specific result codes layered on top of CBM_STORE_OK/ERR/NOT_FOUND. */
#define CBM_STORE_REPLAYED 1
#define CBM_STORE_REJECTED 2
#define CBM_STORE_CHECKPOINTED 3
#define CBM_STORE_IDEMPOTENCY_CONFLICT (-3)

/* ── Memory event ──────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *type;
    const char *source;
    int64_t timestamp_ms;
    const char *project;
    const char *user;
    const char *payload;
    double confidence;
    const char *context_json;
} cbm_memory_event_t;

/* ── Memory item ───────────────────────────────────────────────────── */

typedef struct {
    const char *id;
    const char *kind;
    const char *layer;
    const char *title;
    const char *summary;
    const char *content;
    const char *scope_user;
    const char *scope_project;
    const char *scope_task;
    const char *entity_key;
    const char *predicate;
    double importance;
    double confidence;
    double reusability;
    double specificity;
    int hit_count;
    int64_t last_hit_at;
    double decay;
    const char *status;
    int version;
    const char *supersedes;
    int64_t created_at;
    int64_t updated_at;
    const char *source_event_ids;
    int conflict_count;
    const char *conflict_ids;
    const char *conflict_resolution;
    const char *evidence_json;
    const char *retrieval_source;
    double retrieval_score;
} cbm_memory_item_t;

/* ── Stage 6 request-scoped graph activation ──────────────────────── */

typedef struct {
    char *item_id;
    char *candidate_id;
    char *path_id;
    char *evidence_id;
    char *seed_id;
    char *explanation_json;
    double score;
    int hop;
    int predecessor_count;
} cbm_memory_activation_candidate_t;

typedef struct {
    char *mode;   /* shadow | active */
    char *status; /* completed | failed */
    char *session_id;
    char *termination_reason; /* completed | *_budget | timeout | failpoint | guard */
    cbm_memory_activation_candidate_t *candidates;
    int candidate_count;
    int seed_count;
    int vector_seeds_blocked;
    int node_count;
    int edge_visits;
    int accepted_visits;
    int cycle_rejections;
    int duplicate_rejections;
    int scope_rejections;
    int version_rejections;
    int unsafe_rejections;
    int threshold_rejections;
    int token_proxy;
    int max_hop_observed;
    double elapsed_ms;
    bool budget_exhausted;
} cbm_memory_activation_report_t;

/* ── Memory query / result ─────────────────────────────────────────── */

typedef struct {
    const char *project;
    const char *user;
    const char *task;
    const char *entity_key;
    const char *kind;
    const char *query;
    bool include_inactive;
    int limit;
    /* Optional: qualified_name of the code symbol the agent is currently looking
     * at. When set, memories anchored to it (or to a symbol in the same file) via
     * an about_code edge get a retrieval boost. Pure ranking signal — never
     * changes the candidate set. NULL = no code context. */
    const char *code_context;
    /* Optional borrowed handle to the project's code-graph DB (NOT owned; valid
     * only for the duration of the call). Since memory and graph now live in
     * separate files, the anchor-boost pass reads `nodes` through this handle.
     * NULL disables anchor boosting (memories still returned, just unboosted). */
    struct sqlite3 *graph_db;
    /* Stage 6-A is off unless explicitly requested. Shadow computes a
     * request-scoped report without changing Stage 5 items or ranking. Active
     * is accepted only for isolated stage6-fixture-* projects while
     * CBM_STAGE6_ACTIVE_FIXTURE=1. Zero budgets select frozen defaults. */
    const char *activation_mode; /* NULL/off | shadow | active */
    const char *activation_session_id;
    const char *activation_failpoint;
    int activation_max_hops;
    int activation_max_nodes;
    int activation_max_visits;
    int activation_token_budget;
    int activation_latency_ms;
} cbm_memory_query_t;

typedef struct {
    cbm_memory_item_t *items;
    int count;
    int total;
    cbm_memory_activation_report_t activation;
} cbm_memory_result_t;

/* Stage 5 observe-only retrieval journal. Memory content remains untrusted and
 * is represented in injection/usage APIs only by the hash recorded at recall. */
typedef struct {
    const char *request_id;
    const char *project_scope;
    const char *memory_scope; /* project | global | mixed */
    const char *algorithm_version;
    int config_version;
    const char *query_text; /* hashed before persistence */
} cbm_retrieval_session_input_t;

typedef struct {
    const char *source_store_kind; /* project | global */
    const char *source_store_id;
    const char *memory_item_id;
    const char *retrieval_source; /* structured | fts | vector | code_anchor | graph */
    int source_rank;
    double raw_score;
    double normalized_score;
    int aggregate_rank;
    const char *decision_status; /* retrieved | selected | rejected | contradicted */
    const char *source_detail_json;
    const char *evidence_json;
} cbm_retrieval_candidate_observation_t;

typedef struct {
    char *candidate_id;
    char *provenance_id;
    char *evidence_id;
    char *content_hash;
} cbm_retrieval_observation_ref_t;

typedef struct {
    const char *event_id;
    const char *session_id;
    const char *candidate_id;
    int injection_index;
    const char *target;
    const char *content_hash;
    int token_count;
    const char *classifier_status; /* pass | error */
    const char *classification;    /* safe | prompt_injection | secret | pii | canary */
} cbm_observe_injection_input_t;

typedef struct {
    const char *event_id;
    const char *session_id;
    const char *candidate_id;
    const char *injection_id;
    const char *outcome; /* used | ignored | rejected | contradicted | uncertain */
    const char *evidence_type;
    const char *evidence_ref;
    const char *evidence_hash;
} cbm_observe_usage_input_t;

/* Stage 7 observe-only task evidence and shadow reward. Payload text is used
 * only to verify the caller-provided SHA256 and is never persisted. */
typedef struct {
    const char *project;
    const char *processing_mode; /* must be observe_only */
    const char *event_id;
    const char *task_id;
    const char *task_type;
    const char *session_id;
    const char *candidate_id;
    const char *injection_id;
    const char *usage_id;
    const char *result_id;
    const char *result_type;
    const char *result_status;
    const char *result_ref;
    const char *result_hash;
    const char *result_payload;
    const char *evidence_id;
    const char *evidence_trust;
    const char *evidence_state;
    const char *evidence_source;
    const char *evidence_ref;
    const char *evidence_hash;
    const char *evidence_payload;
    const char *action;
    const char *edge_id;
    const char *supersedes_event_id;
    const char *algorithm_version;
    int config_version;
} cbm_feedback_observe_input_t;

typedef struct {
    char *event_id;
    char *canonical_payload_sha256;
    char *result_json;
} cbm_feedback_observe_result_t;

/* Stage 8-A derives edge contribution events exclusively from immutable
 * Stage 7 feedback. Callers choose execution mode but cannot submit rewards,
 * deltas, or edge identities. */
typedef struct {
    const char *project;
    const char *mode; /* off | shadow | active */
    const char *algorithm_version;
    int config_version;
} cbm_edge_reinforcement_input_t;

typedef struct {
    char *report_json;
    int recorded_count;
    int replayed_count;
} cbm_edge_reinforcement_result_t;

/* ── Memory health ─────────────────────────────────────────────────── */

typedef struct {
    int event_count;
    int item_count;
    int edge_count;
    int candidate_count;
    int active_count;
    int deprecated_count;
    int archived_count;
    int retracted_count;
    int deleted_count; /* soft-deleted, awaiting retention-sweep physical purge */
    int conflict_count;
    int scope_count;
    double hit_rate;
    int64_t total_hits;
} cbm_memory_health_t;

/* Report from cbm_store_memory_maintain_if_due: what (if anything) the lazy
 * auto-maintenance pass actually did this call. */
typedef struct {
    bool consolidated;     /* consolidate pass ran */
    int consolidate_count; /* candidates processed by it */
    bool decayed;          /* decay pass ran */
    int decay_count;       /* items decayed/archived by it */
    bool swept;            /* retention sweep ran */
    int sweep_count;       /* expired soft-deletes physically purged by it */
} cbm_memory_maintain_report_t;

/* ── Global memory project ─────────────────────────────────────────── */

/* Sentinel project name for the global (cross-project) memory store. Memories
 * with scope_project=NULL — user profile, preferences, cross-project lessons —
 * live in <cache>/__global__-memory.db, opened in addition to the per-project
 * store and union-merged on retrieval so they are visible from every project.
 * Chosen so cbm_validate_project_name accepts it (alphanumerics + underscore,
 * no leading dot) and cli_is_rebuildable_index spares it (the "-memory.db"
 * suffix). It is NOT a real indexable project: list_projects filters it out. */
#define CBM_GLOBAL_MEMORY_PROJECT "__global__"

/* ── Path utility ──────────────────────────────────────────────────── */

/* Derive the per-project memory DB path: <cache>/<project>-memory.db.
 * Memory lives in its own file so rebuilding the code graph never destroys it.
 * Returns CBM_STORE_OK and fills buf, or CBM_STORE_ERR on bad input/overflow. */
int cbm_memory_db_path(const char *project, char *buf, size_t bufsz);

/* One-time, idempotent migration: lift memory_* rows from a legacy merged graph
 * DB (where memory and graph shared one file) into this freshly opened memory
 * store. No-op once the memory DB has been migrated, or if graph_db_path is
 * absent / carries no memory rows. Best-effort: failure leaves the memory store
 * usable. Returns CBM_STORE_OK on success/no-op, CBM_STORE_ERR on copy failure. */
int cbm_store_migrate_memory_from_graph(cbm_store_t *mem, const char *graph_db_path);

/* Versioned memory-schema migration runner. Creates the memory_* tables (v1
 * baseline, all IF NOT EXISTS) and applies later steps (memory_meta, vector
 * re-embeds, deleted_at soft-delete column), stamping PRAGMA user_version
 * after each atomic step. Called from the store open path right after graph
 * schema init; idempotent — a DB already at the current version is a no-op. */
int cbm_memory_run_migrations(cbm_store_t *s);

/* ── Memory CRUD ────────────────────────────────────────────────────── */

int cbm_store_memory_append_event(cbm_store_t *s, const cbm_memory_event_t *event,
                                  char **out_event_id);
int cbm_store_memory_append_candidate(cbm_store_t *s, const cbm_memory_item_t *item,
                                      char **out_item_id);
/* Add or replace the searchable FTS row for an item. The caller owns the
 * surrounding business transaction. failure_point is test-only: "prepare" or
 * "insert" returns an error at that phase so atomic rollback can be proven. */
int cbm_store_memory_index_candidate(cbm_store_t *s, const cbm_memory_item_t *item,
                                     const char *item_id, const char *failure_point);
int cbm_store_memory_get_item(cbm_store_t *s, const char *id, cbm_memory_item_t *out);
/* Anchor a memory to a code symbol: creates an about_code edge from the memory
 * item to a code node, addressed by its stable qualified_name (NOT the volatile
 * integer node id — qn survives re-indexing). dst is stored as "code:<qn>".
 * One-directional (memory -> code); the code graph never references memories, so
 * re-indexing the code graph is unaffected. Idempotent (dedup on src,dst,type).
 * origin is "user" (explicit) or "auto". about_code edges are deliberately kept
 * out of the evidence-subgraph walk — they are a recall signal, not graph algo. */
int cbm_store_memory_link_code(cbm_store_t *s, const char *item_id, const char *qualified_name,
                               const char *origin);
/* Materialize the one allowed production semantic relation while `events`
 * creates a new project-scoped memory. The caller must already be inside the
 * event+item transaction. This function verifies that source_item_id is the
 * candidate produced by source_event_id, and that target_item_id is an
 * active/candidate, non-deleted item in the same project/store. It then inserts
 * exactly new -> existing, type=derived_from, confidence=1.0, origin=event ID.
 * This is intentionally not a general existing-to-existing edge API. */
int cbm_store_memory_link_derived_from(cbm_store_t *s, const char *source_item_id,
                                       const char *target_item_id, const char *project,
                                       const char *source_event_id, char **out_edge_id);
/* P3-a: derive confidence/reusability from a memory's about_code anchors using
 * the borrowed code-graph handle. Returns the count of anchors that resolve to a
 * real graph symbol (0 = no usable signal → caller keeps declared values). */
int cbm_store_memory_score_from_anchors(cbm_store_t *s, struct sqlite3 *graph_db,
                                        const char *item_id, const char *project, double *out_conf,
                                        double *out_reuse);
/* Write-time confidence/reusability produced by the 3-tier composition. */
typedef struct {
    double confidence;
    double reusability;
} cbm_memory_score_t;
/* Compose confidence/reusability from all three signal tiers in one place:
 *   L1 graph signal (l1_conf/l1_reuse from score_from_anchors; only when
 *      l1_resolved > 0), L2 kind/type prior (baseline by memory kind), and
 *   L3 the caller's declared values (applied as an OFFSET from the 0.5 default).
 * Monotonic: a tier can only RAISE the baseline, never sink it — an anchored but
 * low-degree ADR keeps at least its kind prior instead of scoring below an
 * unanchored one (which would let it decay out). Confidence carries NO kind
 * prior (kind != correctness); only reusability does. Pass l1_resolved == 0 when
 * there is no graph signal (l1_conf/l1_reuse ignored). */
cbm_memory_score_t cbm_memory_score_item(const char *kind, int l1_resolved, double l1_conf,
                                         double l1_reuse, double declared_conf,
                                         double declared_reuse);
int cbm_store_memory_retrieve(cbm_store_t *s, const cbm_memory_query_t *query,
                              cbm_memory_result_t *out);
int cbm_store_memory_observe_session_begin(cbm_store_t *s,
                                           const cbm_retrieval_session_input_t *input,
                                           char **out_session_id, char **out_request_id,
                                           bool *out_replayed);
int cbm_store_memory_observe_session_complete(cbm_store_t *s, const char *session_id,
                                              const char *status, const char *error_code);
int cbm_store_memory_observe_candidates(cbm_store_t *s, const char *session_id,
                                        const cbm_retrieval_candidate_observation_t *candidates,
                                        int count, cbm_retrieval_observation_ref_t *out_refs);
void cbm_store_memory_observation_refs_free(cbm_retrieval_observation_ref_t *refs, int count);
int cbm_store_memory_observe_injection(cbm_store_t *s, const cbm_observe_injection_input_t *input);
int cbm_store_memory_observe_usage(cbm_store_t *s, const cbm_observe_usage_input_t *input);
int cbm_stage7_sha256_hex(const void *data, size_t size, char out_hex[65]);
int cbm_store_memory_feedback_observe(cbm_store_t *s, const cbm_feedback_observe_input_t *input,
                                      cbm_feedback_observe_result_t *out);
void cbm_store_memory_feedback_observe_result_free(cbm_feedback_observe_result_t *result);
int cbm_store_memory_stage7_audit_verify(cbm_store_t *s, int *out_count);
int cbm_store_memory_reinforcement_replay(cbm_store_t *s,
                                          const cbm_edge_reinforcement_input_t *input,
                                          cbm_edge_reinforcement_result_t *out);
void cbm_store_memory_reinforcement_result_free(cbm_edge_reinforcement_result_t *result);
int cbm_store_memory_stage8_audit_verify(cbm_store_t *s, int *out_count);
int cbm_store_memory_mark_hits(cbm_store_t *s, const char **ids, int count, int64_t now_ms);
int cbm_store_memory_update_status(cbm_store_t *s, const char *id, const char *project,
                                   const char *status);
/* Retry-safe feedback. When requested_event_id is non-empty, the event id is an
 * idempotency key: an exact canonical replay returns CBM_STORE_REPLAYED without
 * side effects; reuse with a different payload returns
 * CBM_STORE_IDEMPOTENCY_CONFLICT. */
int cbm_store_memory_feedback_idempotent(cbm_store_t *s, const char *id, const char *project,
                                         const char *feedback, const char *note, const char *user,
                                         const char *requested_event_id, char **out_event_id);
/* Backward-compatible wrapper that auto-generates an event id. */
int cbm_store_memory_feedback(cbm_store_t *s, const char *id, const char *project,
                              const char *feedback, const char *note, const char *user,
                              char **out_event_id);
/* Delete a memory item (P0-2). mode (default "soft" when NULL/empty):
 *   "soft"  — mark deleted_at; hidden from retrieval, undoable via restore until
 *             the retention sweep physically purges it past the grace window.
 *   "hard"  — delete item + vec + fts + edges in one transaction; source events
 *             are KEPT as an audit trail.
 *   "purge" — hard, plus delete the item's own source events (GDPR erasure).
 * Every mode writes a tombstone audit event (the tombstone survives purge). The
 * delete is scope-guarded: a non-NULL project that doesn't match the item's
 * scope_project returns CBM_STORE_NOT_FOUND. Returns CBM_STORE_OK,
 * CBM_STORE_NOT_FOUND (no such item / already soft-deleted / out of scope), or
 * CBM_STORE_ERR. */
int cbm_store_memory_delete(cbm_store_t *s, const char *id, const char *project, const char *mode,
                            const char *user);
/* Undo a soft delete: clear deleted_at, scope-guarded, writes a restore audit
 * event. Returns CBM_STORE_NOT_FOUND if the item isn't soft-deleted. */
int cbm_store_memory_restore(cbm_store_t *s, const char *id, const char *project, const char *user);
/* Retention sweep: physically purge every item soft-deleted more than grace_ms
 * ago (full purge, source events included — the grace window was the undo
 * chance). Collects ids first then deletes in one batch transaction. *purged
 * receives the count removed. */
int cbm_store_memory_purge_expired(cbm_store_t *s, const char *project, int64_t grace_ms,
                                   int *purged);
int cbm_store_memory_consolidate(cbm_store_t *s, const char *project, int limit, int *processed);
/* Stage 14 controller-only variant. The caller must already own a write transaction on this
 * store and a matching running global_maintenance_run row. It never commits or rolls back. */
int cbm_store_memory_consolidate_in_transaction(cbm_store_t *s, const char *project, int limit,
                                                const char *controller_run_id, int *processed);
int cbm_store_memory_decay(cbm_store_t *s, const char *project, int limit, int *processed);
/* Lazy auto-maintenance: runs consolidate and/or decay only when "due" (by a
 * cheap candidate-count + elapsed-time gate), so a single-user agent never has
 * to call admin endpoints by hand. Safe to call on the memory hot path: it must
 * NOT be invoked inside an open transaction (consolidate opens its own). Honors
 * env CBM_MEMORY_AUTO_MAINTAIN=0 to disable. out may be NULL. Maintenance
 * failures are swallowed (return CBM_STORE_OK) so they never fail the caller. */
int cbm_store_memory_maintain_if_due(cbm_store_t *s, const char *project,
                                     cbm_memory_maintain_report_t *out);
/* Rebuild the FTS index for a project from memory_item using the current
 * segmentation (heals rows indexed before CJK bigram segmentation existed).
 * Returns number of items reindexed in *processed. */
int cbm_store_memory_reindex_fts(cbm_store_t *s, const char *project, int *processed);
int cbm_store_memory_health(cbm_store_t *s, const char *project, cbm_memory_health_t *out);
void cbm_store_memory_item_free(cbm_memory_item_t *item);
void cbm_store_memory_result_free(cbm_memory_result_t *out);

/* ── ADR ───────────────────────────────────────────────────────────── */

/* P1: ADR list — structured query for decision/constraint-class memories.
 * Returns a JSON array of matching items (projection: id, kind, layer, title,
 * summary, entity_key, status, importance, hit_count, decay, version,
 * supersedes, created_at, updated_at). Filters by scope_project, kind,
 * status, and entity_key. Caller must free the returned string. */
int cbm_store_memory_adr_list(cbm_store_t *s, const char *project, const char *kind_filter,
                              const char *status_filter, const char *entity_key_filter, int limit,
                              char **out_json);

/* Same as cbm_store_memory_adr_list but queries the global (cross-project)
 * store where scope_project IS NULL. project label in output is "__global__". */
int cbm_store_memory_adr_list_global(cbm_store_t *s, const char *kind_filter,
                                     const char *status_filter, const char *entity_key_filter,
                                     int limit, char **out_json);

/* Walk the supersedes chain for an ADR. Start from item_id (walk backward to
 * root then forward to newest) or entity_key (find root at version=1). Returns
 * JSON with items in version order (oldest first) plus generation ordinal.
 * Cycle detection caps at max_depth. Caller frees *out_json. */
int cbm_store_memory_adr_chain(cbm_store_t *s, const char *project, const char *entity_key,
                               const char *item_id, int max_depth, char **out_json);

#endif /* CBM_MEMORY_STORE_H */
