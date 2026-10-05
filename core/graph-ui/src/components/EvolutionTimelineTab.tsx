import { ChevronLeft, ChevronRight, RefreshCw, X } from "lucide-react";
import { useState } from "react";
import { useGlobalDetail, useGlobalList } from "../hooks/useGlobalMemory";
import type { EvolutionEvent, TaskCausalChain } from "../lib/types";

export function EvolutionTimelineTab() {
  const { data, error, loading, cursor, hasPrevious, nextPage, previousPage, refresh } = useGlobalList<EvolutionEvent>("manager_evolution", { limit: 100 });
  const [selected, setSelected] = useState<EvolutionEvent | null>(null);
  const taskId = selected?.task_id ?? "";
  const chain = useGlobalDetail<TaskCausalChain>("manager_task_chain", { task_id: taskId }, Boolean(taskId));
  if (loading) return <div className="state-line">正在读取演化记录...</div>;
  if (error || !data) return <div className="state-line error">{error || "EVOLUTION_UNAVAILABLE"}</div>;
  return <div className="workspace-scroll"><section className="toolbar-band"><h2>演化记录</h2><span>{data.total} 个可审计事件 · offset {cursor}</span><button className="icon-button" title="上一页" aria-label="上一页" disabled={!hasPrevious} onClick={previousPage}><ChevronLeft size={16} /></button><button className="icon-button" title="下一页" aria-label="下一页" disabled={data.next_cursor === null} onClick={nextPage}><ChevronRight size={16} /></button><button className="icon-button" title="刷新" onClick={() => void refresh()}><RefreshCw size={16} /></button></section>
    <section className="section-block"><div className="data-table evolution-table"><div className="table-row table-head"><span>序号</span><span>操作</span><span>Evidence</span><span>对象</span><span>项目</span><span>时间</span></div>{data.items.map((event) => <button className="table-row task-row" key={event.event_id} onClick={() => setSelected(event)}><span>{event.sequence_no}</span><span>{event.operation}</span><span className={`grade grade-${event.evidence_grade.toLowerCase()}`}>{event.evidence_grade}</span><span className="mono truncate">{event.object_kind}:{event.object_id}</span><span className="mono truncate">{event.project_uuid}</span><span>{event.created_at}</span></button>)}</div></section>
    {selected && <aside className="detail-drawer" aria-label="任务因果链"><div className="detail-drawer-header"><div><span className="kicker">TASK CAUSAL CHAIN</span><strong className="mono">{selected.task_id ?? "no task"}</strong></div><button className="icon-button" title="关闭" onClick={() => setSelected(null)}><X size={16} /></button></div>
      <div className="detail-metrics"><div><span>Evidence grade</span><strong>{chain.data?.evidence_grade ?? selected.evidence_grade}</strong></div><div><span>Evolution</span><strong>{selected.operation}</strong></div><div><span>Project</span><strong className="mono">{selected.project_uuid}</strong></div><div><span>Before / after</span><strong className="mono">{selected.before_sha256} / {selected.after_sha256}</strong></div></div>
      {chain.loading ? <div className="state-line">正在读取因果链...</div> : chain.error ? <div className="state-line error">{chain.error}</div> : <div className="causal-chain">{chain.data?.links.map((link) => <div key={`${link.kind}:${link.id}`}><span>{link.kind}</span><strong className="mono">{link.id}</strong><small>{link.label}{link.provenance ? ` · ${link.provenance}` : ""}</small></div>)}</div>}
    </aside>}
  </div>;
}
