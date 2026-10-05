import { useCallback, useEffect, useState } from "react";
import { managerFetch } from "../api/rpc";
import type { ManagerHealth, ManagerLatency } from "../lib/types";

const latencySamples: number[] = [];

function latencyStats(last: number): ManagerLatency {
  latencySamples.push(last);
  if (latencySamples.length > 64) latencySamples.shift();
  const ordered = [...latencySamples].sort((left, right) => left - right);
  const percentile = (fraction: number) => ordered[Math.min(ordered.length - 1, Math.ceil(ordered.length * fraction) - 1)] ?? last;
  return { last, p50: percentile(0.5), p95: percentile(0.95), max: ordered[ordered.length - 1] ?? last, samples: ordered.length };
}

export function useManagerHealth(project: string) {
  const [data, setData] = useState<ManagerHealth | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [latency, setLatency] = useState<ManagerLatency>({ last: 0, p50: 0, p95: 0, max: 0, samples: 0 });
  const refresh = useCallback(async () => {
    setLoading(true);
    const startedAt = Date.now();
    try {
      const response = await managerFetch(`/api/manager/health?project=${encodeURIComponent(project)}`);
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      setData(await response.json() as ManagerHealth);
      setError("");
    } catch (value) {
      setError(value instanceof Error ? value.message : "MANAGER_UNAVAILABLE");
    } finally {
      setLatency(latencyStats(Date.now() - startedAt));
      setLoading(false);
    }
  }, [project]);
  useEffect(() => { void refresh(); }, [refresh]);
  return { data, error, loading, latency, refresh };
}
