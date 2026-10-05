import { useCallback, useEffect, useState } from "react";
import { managerFetch } from "../api/rpc";
import type { TaskRun, TaskRunsResponse } from "../lib/types";

export function useTaskSessions(project: string) {
  const [tasks, setTasks] = useState<TaskRun[]>([]);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const response = await managerFetch(`/api/manager/tasks?project=${encodeURIComponent(project)}`);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const body = await response.json() as TaskRunsResponse;
      setTasks((body.tasks ?? []).map((task) => ({
        ...task,
        candidate_count: task.candidate_count ?? 0,
        source_count: task.source_count ?? 0,
        duration_ms: task.duration_ms ?? 0,
        feedback_disposition: task.feedback_disposition ?? "none",
        candidates: task.candidates ?? [],
        edges: task.edges ?? [],
        memories: task.memories ?? [],
      })));
      setError("");
    } catch (value) {
      setError(value instanceof Error ? value.message : "TASKS_UNAVAILABLE");
    } finally {
      setLoading(false);
    }
  }, [project]);
  useEffect(() => { void refresh(); }, [refresh]);
  return { tasks, error, loading, refresh };
}
