import { Pause, Play, RefreshCw, ShieldAlert } from "lucide-react";
import { useState } from "react";
import { callTool } from "../api/rpc";
import { useGlobalDetail } from "../hooks/useGlobalMemory";
import type { DriftPreview, MaintenanceControl, MaintenancePreview } from "../lib/types";

export function DriftRepairTab() {
  const drift = useGlobalDetail<DriftPreview>("manager_drift_preview");
  const maintenance = useGlobalDetail<MaintenancePreview>("manager_maintenance_preview");
  const loading = drift.loading || maintenance.loading;
  const error = drift.error || maintenance.error;
  const [controlStatus, setControlStatus] = useState("");
  if (loading) return <div className="state-line">正在读取漂移与维护状态...</div>;
  if (error || !drift.data || !maintenance.data) return <div className="state-line error">{error || "DRIFT_PREVIEW_UNAVAILABLE"}</div>;
  const isolated = maintenance.data.mode === "isolated_mock";
  const productionReady = maintenance.data.production_gate_loaded && maintenance.data.production_actions_enabled;
  async function control(action: "pause" | "resume" | "dry_run") {
    if (!isolated) return;
    try {
      const result = await callTool<{ schema: string; status: string; item: MaintenanceControl }>("manager_maintenance_control", { action, scope: "isolated_mock" });
      setControlStatus(result.item.status);
      await maintenance.refresh();
    } catch (value) {
      setControlStatus(value instanceof Error ? value.message : "MAINTENANCE_CONTROL_UNAVAILABLE");
    }
  }
  return <div className="workspace-scroll"><section className="toolbar-band"><h2>漂移与维护</h2><span>preview only</span><button className="icon-button" title="刷新" onClick={() => { void drift.refresh(); void maintenance.refresh(); }}><RefreshCw size={16} /></button></section>
    <div className="operations-grid"><section className="section-block"><h2><ShieldAlert size={16} /> 配置漂移</h2><div className="preview-list"><div><span>分类</span><strong>{drift.data.classification}</strong></div><div><span>阻断</span><strong className={drift.data.blocking ? "error" : "ok"}>{String(drift.data.blocking)}</strong></div><div><span>Repair preview</span><strong>{drift.data.repair_preview.action}</strong></div><div><span>需要写入</span><strong>{drift.data.repair_preview.writes_required}</strong></div></div><p className="read-only-note">Repair 只显示冻结的预览，不能从 Manager 直接写入配置。</p></section>
      <section className="section-block"><h2>维护</h2><div className="preview-list"><div><span>状态</span><strong>{maintenance.data.state}</strong></div><div><span>Lease</span><strong className="mono">{maintenance.data.lease_owner ?? "none"}</strong></div><div><span>Checkpoint</span><strong>{maintenance.data.checkpoint}</strong></div><div><span>Production gate</span><strong className={productionReady ? "ok" : "error"}>{maintenance.data.production_gate_status}</strong></div><div><span>授权清单</span><strong className="mono truncate" title={maintenance.data.authorization_manifest_path}>{maintenance.data.authorization_manifest_sha256 || "not loaded"}</strong></div><div><span>Task manifest</span><strong className="mono truncate" title={maintenance.data.task_evolution_manifest_path}>{maintenance.data.task_evolution_manifest_sha256 || "not verified"}</strong></div><div><span>Edge manifest</span><strong className="mono truncate" title={maintenance.data.edge_manifest_path}>{maintenance.data.edge_manifest_sha256 || "not verified"}</strong></div><div><span>Concept manifest</span><strong className="mono truncate" title={maintenance.data.concept_manifest_path}>{maintenance.data.concept_manifest_sha256 || "not verified"}</strong></div><div><span>Pause API</span><strong className="error">{maintenance.data.production_pause_status}</strong></div></div><div className="maintenance-actions"><button className="command-button" disabled={!isolated} title={isolated ? "隔离 mock dry-run" : "生产操作需显式清单与审计 MCP 请求"} onClick={() => void control("dry_run")}><RefreshCw size={15} />生成维护预览</button><button className="command-button" disabled={!isolated} title={isolated ? "隔离 mock 模拟" : "生产 Pause 核心 API 尚不可用"} onClick={() => void control("pause")}><Pause size={15} />暂停维护</button><button className="command-button" disabled={!isolated} title={isolated ? "隔离 mock 模拟" : "生产 Resume 需显式清单与审计 MCP 请求"} onClick={() => void control("resume")}><Play size={15} />恢复维护</button></div><p className="read-only-note">生产按钮默认锁定；只有携带冻结授权、task/edge/concept 清单与哈希的显式 MCP 请求才能进入控制器。</p>{controlStatus && <p className="read-only-note">{controlStatus}</p>}</section>
    </div>
    <section className="section-block"><h2>维护历史</h2><div className="data-table maintenance-history"><div className="table-row table-head"><span>Run</span><span>模式</span><span>状态</span><span>时间</span></div>{(maintenance.data.history ?? []).map((entry) => <div className="table-row" key={entry.run_id}><span className="mono truncate">{entry.run_id}</span><span>{entry.mode}</span><span>{entry.status}</span><span>{entry.created_at}</span></div>)}</div></section>
  </div>;
}
