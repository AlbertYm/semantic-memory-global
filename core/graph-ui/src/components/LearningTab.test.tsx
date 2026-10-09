import "@testing-library/jest-dom";
import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import { LearningTab } from "./LearningTab";
import { callTool } from "../api/rpc";
vi.mock("../api/rpc", () => ({ callTool: vi.fn(), managerFetch: vi.fn() }));
const state = { schema: "verified-learning/v1", enabled: true, ready: true, generation: 3, last_code: 0,
  batch_limit: 64, pair_batch_limit: 64, items: [], associations: [] };
describe("Learning controls", () => {
  beforeEach(() => vi.clearAllMocks());
  it("shows a scope denial without treating an error body as learning data", async () => {
    vi.mocked(callTool).mockResolvedValue({ status: "error", code: "SECURITY_SCOPE_VIOLATION" });
    render(<LearningTab project="unknown" />);
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent("SECURITY_SCOPE_VIOLATION"));
    expect(screen.queryByRole("button", { name: "暂停学习" })).not.toBeInTheDocument();
  });
  it("uses the inspected generation and preserves the visible state when a control is rejected", async () => {
    vi.mocked(callTool).mockResolvedValueOnce(state).mockResolvedValueOnce({ code: "LEARNING_CONTROL_CONFLICT" });
    render(<LearningTab project="registered-project" />);
    await waitFor(() => expect(screen.getByText("运行中")).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "暂停学习" }));
    await waitFor(() => expect(screen.getByRole("alert")).toHaveTextContent("状态已变化"));
    expect(callTool).toHaveBeenLastCalledWith("memory_learning_control", expect.objectContaining({
      project: "registered-project", action: "pause", expected_generation: 3, idempotency_key: expect.any(String),
    }));
    expect(screen.getByText("运行中")).toBeInTheDocument();
  });
});
