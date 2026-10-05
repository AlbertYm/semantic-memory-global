import { useManagerHealth } from "../hooks/useManagerHealth";

export function DiagnosticsTab({ project }: { project: string }) {
  const { data, error, latency } = useManagerHealth(project);
  return <div className="workspace-scroll"><section className="toolbar-band"><h2>设置与诊断</h2><span>redacted</span></section>
    {error ? <div className="state-line error">{error}</div> : <div className="diagnostic-list">
      <div><span>Embedding backend</span><strong>static / local</strong></div><div><span>External calls</span><strong>0</strong></div>
      <div><span>Model / service</span><strong>local deterministic / ready</strong></div><div><span>Timeout</span><strong>3000 ms</strong></div>
      <div><span>Health latency</span><strong>{latency.p50} / {latency.p95} / {latency.max} ms (P50 / P95 / max)</strong></div><div><span>Samples</span><strong>{latency.samples}</strong></div>
      <div><span>Fallback</span><strong>local deterministic</strong></div><div><span>Security</span><strong>{data?.modes.security ?? "-"}</strong></div>
      <div><span>Plasticity</span><strong>{data?.modes.plasticity ?? "-"}</strong></div><div><span>Log redaction</span><strong>on</strong></div>
    </div>}
  </div>;
}
