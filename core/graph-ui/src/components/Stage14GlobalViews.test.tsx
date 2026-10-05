import "@testing-library/jest-dom";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { App } from "../App";

const health = {
  schema: "stage14-manager-health/v1", status: "ok", version: "v1.1.0-rc.1",
  project: "global", modes: { security: "on", plasticity: "shadow", active: false, automatic_maintenance: false, global_union: true },
  stores: [],
};

const globalPayload = {
  schema: "stage14-manager-global/v1", status: "ok", schema_ready: true,
  overview: { project_count: 3, memory_count: 24, cross_project_edge_count: 5, task_count: 11, maintenance_state: "paused" },
  projects: [{ project_uuid: "project-a", display_name: "Alpha", workspace_state: "ready", index_state: "indexed", last_seen_at: "2026-07-27T01:00:00Z", memory_count: 12, task_count: 5 }],
  memories: [{ memory_item_id: "memory-a", title: "Reusable migration decision", summary: "Reusable migration decision", kind: "decision", project_uuid: "project-a", project_name: "Alpha", evidence_grade: "A", status: "active", created_at: "2026-07-27T01:00:00Z", provenance: { project_uuid: "project-a", project_name: "Alpha", source_kind: "global_memory", legacy_project_id: "legacy-a" } }],
  nextMemory: { memory_item_id: "memory-b", title: "Second page decision", summary: "Second page decision", kind: "decision", project_uuid: "project-b", project_name: "Beta", evidence_grade: "B", status: "active", created_at: "2026-07-27T01:01:00Z", provenance: { project_uuid: "project-b", project_name: "Beta", source_kind: "global_memory", legacy_project_id: "legacy-b" } },
  topology: { edges: [{ edge_id: "edge-a", source_project_uuid: "project-a", target_project_uuid: "project-b", relation_type: "derived_from", weight_ppm: 900000, confidence_ppm: 800000, status: "active", version: 1, provenance: { source_kind: "global_cross_project_graph", evidence_event_id: "event-a", payload_sha256: "abc" } }] },
  events: [{ event_id: "event-a", sequence_no: 8, task_id: "task-a", project_uuid: "project-a", operation: "reinforce", evidence_grade: "A", object_kind: "memory", object_id: "memory-a", before_sha256: "before-a", after_sha256: "after-a", created_at: "2026-07-27T01:00:00Z" }],
  task_chain: { task_id: "task-a", project_uuid: "project-a", evidence_grade: "A", evidence_count: 1, attribution_total: 1, links: [{ kind: "task", id: "task-a", label: "task", project_uuid: "project-a" }, { kind: "evidence", id: "evidence-a", label: "external_verified", provenance: "fixture" }, { kind: "evolution", id: "event-a", label: "reinforce", project_uuid: "project-a" }] },
  drift: { classification: "managed_drift", blocking: true, repair_preview: { action: "restore_managed_fields", writes_required: 0, requires_confirmation: true } },
  maintenance: { state: "idle", lease_owner: null, last_run_at: null, checkpoint: "none", mode: "production", production_gate_loaded: false, production_actions_enabled: false, production_gate_status: "PRODUCTION_GATE_NOT_LOADED", authorization_manifest_path: "", authorization_manifest_sha256: "", authorization_expires_at: "", task_evolution_manifest_path: "", task_evolution_manifest_sha256: "", edge_manifest_path: "", edge_manifest_sha256: "", concept_manifest_path: "", concept_manifest_sha256: "", production_pause_supported: false, production_pause_status: "CORE_PAUSE_API_UNAVAILABLE", history: [{ run_id: "run-a", mode: "shadow", status: "completed", created_at: "2026-07-27T01:00:00Z" }] },
};

const largeTopologyEdges = Array.from({ length: 17 }, (_, index) => {
  const ordinal = index + 1;
  const sourceOrdinal = index * 2 + 1;
  const targetOrdinal = sourceOrdinal + 1;
  return {
    edge_id: `edge-${String(ordinal).padStart(2, "0")}`,
    source_project_uuid: `project-${String(sourceOrdinal).padStart(2, "0")}`,
    target_project_uuid: `project-${String(targetOrdinal).padStart(2, "0")}`,
    relation_type: `relation-${String(ordinal).padStart(2, "0")}`,
    weight_ppm: 900000 - index,
    confidence_ppm: 800000 - index,
    status: "active",
    version: 1,
    provenance: { source_kind: "global_cross_project_graph", evidence_event_id: `event-${ordinal}`, payload_sha256: `sha-${ordinal}` },
  };
});

let isolatedMock = false;
let topologyEdges = globalPayload.topology.edges;

function responseFor(_input: RequestInfo | URL, init?: RequestInit) {
  const body = JSON.parse(String(init?.body ?? "{}"));
  const tool = body?.params?.name as string | undefined;
  const args = body?.params?.arguments as Record<string, unknown> | undefined;
  if (tool === "manager_global_overview") return { schema: globalPayload.schema, status: "ok", item: { ...globalPayload.overview, projects: globalPayload.projects } };
  if (tool === "manager_global_memory") return Number(args?.cursor ?? 0) === 0 ? { schema: globalPayload.schema, status: "ok", items: globalPayload.memories, total: 2, next_cursor: 1 } : { schema: globalPayload.schema, status: "ok", items: [globalPayload.nextMemory], total: 2, next_cursor: null };
  if (tool === "manager_global_topology") return { schema: globalPayload.schema, status: "ok", items: topologyEdges, total: topologyEdges.length, next_cursor: null };
  if (tool === "manager_evolution") return { schema: globalPayload.schema, status: "ok", items: globalPayload.events, total: 1, next_cursor: null };
  if (tool === "manager_task_chain") return { schema: globalPayload.schema, status: "ok", item: globalPayload.task_chain };
  if (tool === "manager_drift_preview") return { schema: globalPayload.schema, status: "ok", item: globalPayload.drift };
  if (tool === "manager_maintenance_preview") return { schema: globalPayload.schema, status: "ok", item: { ...globalPayload.maintenance, mode: isolatedMock ? "isolated_mock" : "production" } };
  if (tool === "manager_maintenance_control") return { schema: globalPayload.schema, status: "planned", item: { action: args?.action, scope: args?.scope, status: "planned", production_state_written: false, isolated_mock: true } };
  return health;
}

describe("Stage14 global Manager views", () => {
  beforeEach(() => {
    isolatedMock = false;
    topologyEdges = globalPayload.topology.edges;
    window.history.replaceState(null, "", "/#token=" + "b".repeat(64));
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL, init?: RequestInit) => {
      const response = responseFor(input, init);
      const payload = init?.body ? { result: { content: [{ text: JSON.stringify(response) }] } } : response;
      return new Response(JSON.stringify(payload), { status: 200 });
    }));
  });

  it("renders global operations and preserves project provenance", async () => {
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "所有项目" }));
    await waitFor(() => expect(screen.getByText("Alpha")).toBeInTheDocument());
    expect(screen.getByText("3")).toBeInTheDocument();
    expect(screen.getByText("indexed")).toBeInTheDocument();
    expect(vi.mocked(fetch).mock.calls.some(([, init]) => String(init?.body).includes("manager_global_overview"))).toBe(true);
  });

  it("paginates real memory payloads and renders nested topology provenance safely", async () => {
    const { container } = render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "全局记忆" }));
    await waitFor(() => expect(screen.getByText("Reusable migration decision")).toBeInTheDocument());
    expect(screen.getByText("Alpha")).toBeInTheDocument();
    expect(screen.getByText("global_memory")).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: "下一页" }));
    await waitFor(() => expect(screen.getByText("Second page decision")).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "跨项目拓扑" }));
    await waitFor(() => expect(screen.getByLabelText("跨项目拓扑图 / Cross-project topology graph")).toBeInTheDocument());
    fireEvent.click(screen.getByText("derived_from").closest("button")!);
    expect(container.querySelectorAll("line.topology-edge")).toHaveLength(1);
    expect(container.querySelector("line.topology-edge.selected")).not.toBeNull();
    fireEvent.click(screen.getByTitle("project-a"));
    expect(container.querySelector("line.topology-edge.selected")).not.toBeNull();
    expect(screen.getAllByText("已选关系 / Selected relation")).toHaveLength(2);
    expect(screen.getByText(/global_cross_project_graph/)).toBeInTheDocument();
    expect(vi.mocked(fetch).mock.calls.some(([, init]) => String(init?.body).includes("manager_global_topology"))).toBe(true);
  });

  it("keeps a selected edge visible through the stable 32-node layout and exposes Refresh", async () => {
    topologyEdges = largeTopologyEdges;
    const { container } = render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "跨项目拓扑" }));
    await waitFor(() => expect(screen.getByText("relation-17")).toBeInTheDocument());

    expect(container.querySelectorAll(".topology-node")).toHaveLength(32);
    const firstNode = screen.getByTitle("project-01");
    expect(firstNode.style.getPropertyValue("--topology-x")).toBe("12.5%");
    expect(firstNode.style.getPropertyValue("--topology-y")).toBe("6.25%");

    fireEvent.click(screen.getByText("relation-17").closest("button")!);
    expect(container.querySelectorAll(".topology-node")).toHaveLength(32);
    expect(screen.getByTitle("project-33")).toBeInTheDocument();
    const lastNode = screen.getByTitle("project-34");
    expect(lastNode.style.getPropertyValue("--topology-x")).toBe("62.5%");
    expect(lastNode.style.getPropertyValue("--topology-y")).toBe("93.75%");
    expect(container.querySelector("line.topology-edge.selected")).not.toBeNull();
    expect(screen.getAllByText("已选关系 / Selected relation")).toHaveLength(2);

    const topologyCallsBeforeRefresh = vi.mocked(fetch).mock.calls.filter(([, init]) => String(init?.body).includes("manager_global_topology")).length;
    fireEvent.click(screen.getByRole("button", { name: "刷新 / Refresh" }));
    await waitFor(() => {
      const topologyCallsAfterRefresh = vi.mocked(fetch).mock.calls.filter(([, init]) => String(init?.body).includes("manager_global_topology")).length;
      expect(topologyCallsAfterRefresh).toBeGreaterThan(topologyCallsBeforeRefresh);
    });
  });

  it("shows the causal task chain and evidence grade without a write action", async () => {
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "演化记录" }));
    await waitFor(() => expect(screen.getByText("reinforce")).toBeInTheDocument());
    fireEvent.click(screen.getByText("reinforce").closest("button")!);
    await waitFor(() => {
      expect(screen.getByLabelText("任务因果链")).toHaveTextContent("evidence-a");
      expect(screen.getByLabelText("任务因果链")).toHaveTextContent("A");
    });
    expect(screen.queryByRole("button", { name: /执行修复|立即维护/i })).not.toBeInTheDocument();
  });

  it("keeps repair and maintenance explicitly preview or confirmation gated", async () => {
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "漂移与维护" }));
    await waitFor(() => expect(screen.getByText("managed_drift")).toBeInTheDocument());
    expect(screen.getByText("restore_managed_fields")).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "生成维护预览" })).toBeDisabled();
    expect(screen.getByRole("button", { name: "恢复维护" })).toBeDisabled();
    expect(screen.getByText("PRODUCTION_GATE_NOT_LOADED")).toBeInTheDocument();
    expect(screen.getByText("CORE_PAUSE_API_UNAVAILABLE")).toBeInTheDocument();
    expect(screen.getByText("not loaded")).toBeInTheDocument();
    expect(screen.getByText("run-a")).toBeInTheDocument();
  });

  it("sends scoped maintenance control only in an isolated mock", async () => {
    isolatedMock = true;
    render(<App />);
    fireEvent.click(screen.getByRole("button", { name: "漂移与维护" }));
    await waitFor(() => expect(screen.getByRole("button", { name: "生成维护预览" })).toBeEnabled());
    fireEvent.click(screen.getByRole("button", { name: "生成维护预览" }));
    await waitFor(() => expect(screen.getByText("planned")).toBeInTheDocument());
    expect(vi.mocked(fetch).mock.calls.some(([, init]) => String(init?.body).includes('"scope":"isolated_mock"'))).toBe(true);
  });
});
