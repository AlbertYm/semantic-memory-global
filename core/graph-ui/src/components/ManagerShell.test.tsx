import "@testing-library/jest-dom";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { App } from "../App";

const health = {
  schema: "stage12-manager-health/v1", status: "ok", version: "v0.12.0-beta.1",
  project: "H-Codex_H-neuroplastic-main",
  modes: { security: "on", plasticity: "off", active: false, automatic_maintenance: false, global_union: false },
  stores: [
    { role: "graph", filename: "project.db", size_bytes: 1, quick_check: "ok", foreign_key_violations: 0, wal_capable: true },
    { role: "memory", filename: "project-memory.db", size_bytes: 2, quick_check: "ok", foreign_key_violations: 0, wal_capable: true },
    { role: "config", filename: "_config.db", size_bytes: 3, quick_check: "ok", foreign_key_violations: 0, wal_capable: true },
  ],
};

const taskResponse = {
  schema: "stage12-manager-tasks/v1", status: "ok", schema_ready: true,
  tasks: [{
    task_id: "task-stage12-ui", session_id: "session-ui", turn_id: "turn-ui", state: "completed", outcome: "completed",
    retrieval_session_id: "retrieval-ui", updated_at: "2026-07-21T00:00:00Z", duration_ms: 42,
    evidence_count: 1, attribution_count: 1, candidate_count: 1, source_count: 1, feedback_disposition: "recorded",
    candidates: [{ id: "candidate-ui", memory_item_id: "memory-ui", store_kind: "project", score: 0.91, rank: 1, decision: "selected", sources: [{ type: "fts", rank: 1, score: 0.91 }] }],
    edges: [], memories: [{ id: "memory-ui", summary: "Stage 12 verified summary", kind: "decision", scope: "H-Codex_H-neuroplastic-main", version: 3, status: "active", entity_key: "stage12-ui", attribution_state: "used", evidence_id: "evidence-ui", feedback_event_id: "feedback-ui", created_at: "2026-07-21T00:00:00Z" }],
  }],
};

describe("Stage12 manager shell", () => {
  beforeEach(() => {
    window.history.replaceState(null, "", "/#token=" + "a".repeat(64));
    vi.stubGlobal("fetch", vi.fn(async (input: RequestInfo | URL) => new Response(JSON.stringify(String(input).includes("/tasks") ? taskResponse : health), { status: 200 })));
  });

  it("renders twelve operational views without process termination", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("v0.12.0-beta.1")).toBeInTheDocument());
    const viewNames = ["概览", "所有项目", "全局记忆", "跨项目拓扑", "演化记录", "漂移与维护", "任务", "召回路径", "Memory", "审核", "备份恢复", "诊断"];
    expect(screen.getByRole("navigation").querySelectorAll("button")).toHaveLength(12);
    for (const name of viewNames) expect(screen.getByRole("button", { name })).toBeInTheDocument();
    expect(screen.queryByText(/kill|结束进程/i)).not.toBeInTheDocument();
  });

  it("clears the token fragment and sends it only as a header", async () => {
    render(<App />);
    await waitFor(() => expect(fetch).toHaveBeenCalled());
    expect(window.location.hash).toBe("");
    const init = vi.mocked(fetch).mock.calls[0][1] as RequestInit;
    expect(new Headers(init.headers).get("X-Manager-Token")).toBe("a".repeat(64));
  });

  it("shows bounded task detail and read-only memory metadata", async () => {
    render(<App />);
    await waitFor(() => expect(screen.getByText("v0.12.0-beta.1")).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "任务" }));
    await waitFor(() => expect(screen.getByText("task-stage12-ui")).toBeInTheDocument());
    fireEvent.click(screen.getByText("task-stage12-ui").closest("button")!);
    expect(screen.getByLabelText("任务详情")).toHaveTextContent("42 ms");
    expect(screen.getByLabelText("任务详情")).toHaveTextContent("recorded");
    fireEvent.click(screen.getByRole("button", { name: "Memory" }));
    await waitFor(() => expect(screen.getByText("Stage 12 verified summary")).toBeInTheDocument());
    expect(screen.getByText("decision / v3")).toBeInTheDocument();
  });
});
