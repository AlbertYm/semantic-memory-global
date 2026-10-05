/* Graph data types matching the C layout3d.c JSON output */

export interface GraphNode {
  id: number;
  x: number;
  y: number;
  z: number;
  label: string;
  name: string;
  file_path?: string;
  qualified_name?: string;
  start_line?: number;
  end_line?: number;
  size: number;
  color: string;
  /* Dead-code classification from the backend layout (layout3d.c). */
  status?: NodeStatus;
  in_calls?: number;
}

export type NodeStatus =
  | "dead"
  | "single"
  | "entry"
  | "test"
  | "exported"
  | "normal"
  | "structural";

/* Git remote metadata for building GitHub deep-links (/api/repo-info). */
export interface RepoInfo {
  root_path: string;
  branch: string;
  remote_url: string;
  web_base: string; /* e.g. github.com/<org>/<repo> */
  blob_base: string; /* e.g. github.com/<org>/<repo>/blob/<branch> */
}

export interface GraphEdge {
  source: number;
  target: number;
  type: string;
}

export interface LinkedProject {
  project: string;
  nodes: GraphNode[];
  edges: GraphEdge[];
  offset: { x: number; y: number; z: number };
  cross_edges: GraphEdge[];
}

export interface GraphData {
  nodes: GraphNode[];
  edges: GraphEdge[];
  total_nodes: number;
  linked_projects?: LinkedProject[];
}

export interface Project {
  name: string;
  root_path: string;
  indexed_at: string;
}

export interface SchemaInfo {
  node_labels: { label: string; count: number }[];
  edge_types: { type: string; count: number }[];
  total_nodes: number;
  total_edges: number;
}

export type TabId =
  | "overview"
  | "all-projects"
  | "global-memory"
  | "topology"
  | "evolution"
  | "drift"
  | "tasks"
  | "recall"
  | "memory"
  | "review"
  | "operations"
  | "diagnostics";

export interface ManagerStoreHealth {
  role: "graph" | "memory" | "config";
  filename: string;
  size_bytes: number;
  quick_check: string;
  foreign_key_violations: number;
  wal_capable: boolean;
}

export interface ManagerHealth {
  schema: string;
  status: string;
  version: string;
  project: string;
  modes: {
    security: string;
    plasticity: string;
    active: boolean;
    automatic_maintenance: boolean;
    global_union: boolean;
  };
  stores: ManagerStoreHealth[];
}

export interface TaskRun {
  task_id: string;
  session_id: string;
  turn_id: string;
  state: string;
  outcome: string | null;
  retrieval_session_id: string | null;
  updated_at: string;
  evidence_count: number;
  attribution_count: number;
  candidate_count: number;
  source_count: number;
  duration_ms: number;
  feedback_disposition: "none" | "pending" | "recorded";
  candidates: RecallCandidate[];
  edges: RecallEdge[];
  memories: TaskMemory[];
}

export interface TaskRunsResponse {
  schema: string;
  status: string;
  schema_ready: boolean;
  tasks: TaskRun[];
}

export interface RecallSource {
  type: string;
  rank: number | null;
  score: number | null;
}

export interface RecallCandidate {
  id: string;
  memory_item_id: string;
  store_kind: string;
  score: number | null;
  rank: number | null;
  decision: string;
  sources: RecallSource[];
}

export interface RecallEdge {
  id: string;
  from: string | null;
  to: string;
  relation: string;
  hop_depth: number;
  visit_status: string;
}

export interface TaskMemory {
  id: string;
  summary: string;
  kind: string;
  scope: string | null;
  version: number;
  status: string;
  entity_key: string;
  attribution_state: string;
  evidence_id: string | null;
  feedback_event_id: string | null;
  created_at: string;
}

export interface ManagerLatency {
  last: number;
  p50: number;
  p95: number;
  max: number;
  samples: number;
}

export interface ProcessInfo {
  pid: number;
  cpu: number;
  rss_mb: number;
  elapsed: string;
  command: string;
  is_self: boolean;
}

export interface CursorPage<T> {
  schema: string;
  status: string;
  items: T[];
  next_cursor: string | null;
  total: number;
}

export interface GlobalOverview {
  project_count: number;
  memory_count: number;
  cross_project_edge_count: number;
  task_count: number;
  maintenance_state: string;
  projects: GlobalProject[];
}

export interface GlobalProject {
  project_uuid: string;
  display_name: string;
  workspace_state: string;
  index_state: string;
  last_seen_at: string;
  memory_count: number;
  task_count: number;
}

export interface GlobalMemoryItem {
  memory_item_id: string;
  title: string;
  summary: string;
  kind: string;
  project_uuid: string;
  project_name: string;
  evidence_grade: string;
  status: string;
  created_at: string;
  provenance: GlobalProvenance;
}

export interface GlobalProvenance {
  project_uuid?: string;
  project_name?: string;
  source_kind: string;
  legacy_project_id?: string;
  evidence_event_id?: string;
  payload_sha256?: string;
}

export interface GlobalTopologyEdge {
  edge_id: string;
  source_project_uuid: string;
  target_project_uuid: string;
  relation_type: string;
  weight_ppm: number;
  confidence_ppm: number;
  status: string;
  version: number;
  provenance: GlobalProvenance;
}

export interface EvolutionEvent {
  event_id: string;
  sequence_no: number;
  task_id: string | null;
  project_uuid: string;
  operation: string;
  evidence_grade: string;
  object_kind: string;
  object_id: string;
  before_sha256: string;
  after_sha256: string;
  created_at: string;
}

export interface TaskCausalChain {
  task_id: string;
  project_uuid: string;
  evidence_grade: string;
  evidence_count: number;
  attribution_total: number;
  links: { kind: string; id: string; label: string; project_uuid?: string; provenance?: string }[];
}

export interface DriftPreview {
  classification: string;
  blocking: boolean;
  repair_preview: { action: string; writes_required: number; requires_confirmation: boolean };
}

export interface MaintenancePreview {
  state: string;
  lease_owner: string | null;
  last_run_at: string | null;
  checkpoint: string;
  mode: "isolated_mock" | "production";
  production_gate_loaded: boolean;
  production_actions_enabled: boolean;
  production_gate_status: string;
  authorization_manifest_path: string;
  authorization_manifest_sha256: string;
  authorization_expires_at: string;
  task_evolution_manifest_path: string;
  task_evolution_manifest_sha256: string;
  edge_manifest_path: string;
  edge_manifest_sha256: string;
  concept_manifest_path: string;
  concept_manifest_sha256: string;
  production_pause_supported: boolean;
  production_pause_status: string;
  history: { run_id: string; mode: string; status: string; created_at: string }[];
}

export interface MaintenanceControl {
  action: "preview" | "run" | "pause" | "resume" | "dry_run";
  scope: "isolated_mock" | "production";
  status: "planned" | "simulated" | "rejected" | "applied" | "replayed" | "checkpointed" | "conflict";
  code: string;
  production_state_written: boolean;
  isolated_mock: boolean;
  authorization_ready: boolean;
  authorization_status: string;
}
