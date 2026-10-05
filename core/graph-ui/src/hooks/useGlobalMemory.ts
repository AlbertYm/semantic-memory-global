import { useCallback, useEffect, useState } from "react";
import { callTool } from "../api/rpc";

interface ToolDetail<T> { schema: string; status: string; item: T; }
interface ToolList<T> { schema: string; status: string; items: T[]; next_cursor: string | null; total: number; }

export function useGlobalDetail<T>(tool: string, args: Record<string, unknown> = {}, enabled = true) {
  const [data, setData] = useState<T | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const refresh = useCallback(async () => {
    if (!enabled) {
      setLoading(false);
      return;
    }
    setLoading(true);
    try {
      const response = await callTool<ToolDetail<T>>(tool, args);
      if (response.status !== "ok") throw new Error(response.status || "MANAGER_UNAVAILABLE");
      setData(response.item);
      setError("");
    } catch (value) {
      setError(value instanceof Error ? value.message : "MANAGER_UNAVAILABLE");
    } finally {
      setLoading(false);
    }
  }, [tool, JSON.stringify(args), enabled]);
  useEffect(() => { void refresh(); }, [refresh]);
  return { data, error, loading, refresh };
}

export function useGlobalList<T>(tool: string, args: Record<string, unknown> = {}) {
  const [data, setData] = useState<ToolList<T> | null>(null);
  const [error, setError] = useState("");
  const [loading, setLoading] = useState(true);
  const [cursor, setCursor] = useState(0);
  const argsKey = JSON.stringify(args);
  const pageSize = typeof args.limit === "number" ? args.limit : 50;
  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const response = await callTool<ToolList<T>>(tool, { ...args, cursor });
      if (response.status !== "ok") throw new Error(response.status || "MANAGER_UNAVAILABLE");
      setData(response);
      setError("");
    } catch (value) {
      setError(value instanceof Error ? value.message : "MANAGER_UNAVAILABLE");
    } finally {
      setLoading(false);
    }
  }, [tool, argsKey, cursor]);
  useEffect(() => { void refresh(); }, [refresh]);
  const nextPage = useCallback(() => {
    if (data?.next_cursor !== null && data?.next_cursor !== undefined) setCursor(Number(data.next_cursor));
  }, [data]);
  const previousPage = useCallback(() => setCursor((value) => Math.max(0, value - pageSize)), [pageSize]);
  return { data, error, loading, cursor, hasPrevious: cursor > 0, refresh, nextPage, previousPage };
}
