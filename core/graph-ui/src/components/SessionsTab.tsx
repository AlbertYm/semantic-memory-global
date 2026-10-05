import { RefreshCw, X } from "lucide-react";
import { useState } from "react";
import { useTaskSessions } from "../hooks/useTaskSessions";
import type { TaskRun } from "../lib/types";

export function SessionsTab({ project, onSelect }: { project: string; onSelect?: (task: TaskRun) => void }) {
  const { tasks, error, loading, refresh } = useTaskSessions(project);
  const [filter, setFilter] = useState("");
  const [selected, setSelected] = useState<TaskRun | null>(null);
  const visible = tasks.filter((task) => !filter || task.state === filter);
  return (
    <div className="workspace-scroll">
      <section className="toolbar-band">
        <h2>任务与召回</h2>
        <select value={filter} onChange={(event) => setFilter(event.target.value)} aria-label="状态筛选">
          <option value="">全部状态</option><option value="completed">completed</option>
          <option value="recall_completed">recall_completed</option><option value="abandoned">abandoned</option>
        </select>
        <button className="icon-button" title="刷新" onClick={() => void refresh()}><RefreshCw size={16} /></button>
      </section>
      {loading ? <div className="state-line">正在读取任务...</div> : error ? <div className="state-line error">{error}</div> : visible.length === 0 ? (
        <div className="empty-state">暂无符合条件的任务</div>
      ) : (
        <div className="data-table tasks-table">
          <div className="table-row table-head"><span>状态</span><span>Task</span><span>Recall</span><span>Evidence</span><span>Attribution</span><span>时间</span></div>
          {visible.map((task) => (
            <button className="table-row task-row" key={task.task_id} onClick={() => { setSelected(task); onSelect?.(task); }}>
              <span className={`status ${task.state}`}>{task.state}</span>
              <span className="mono truncate">{task.task_id}</span>
              <span className="mono truncate">{task.retrieval_session_id ?? "zero-hit"}</span>
              <span>{task.evidence_count}</span><span>{task.attribution_count}</span><span>{task.updated_at}</span>
            </button>
          ))}
        </div>
      )}
      {selected && <aside className="detail-drawer" aria-label="任务详情">
        <div className="detail-drawer-header"><div><span className="kicker">TASK DETAIL</span><strong className="mono">{selected.task_id}</strong></div>
          <button className="icon-button" title="关闭" onClick={() => setSelected(null)}><X size={16} /></button></div>
        <div className="detail-metrics">
          <div><span>状态</span><strong>{selected.state}</strong></div><div><span>Outcome</span><strong>{selected.outcome ?? "pending"}</strong></div>
          <div><span>耗时</span><strong>{selected.duration_ms} ms</strong></div><div><span>Feedback</span><strong>{selected.feedback_disposition}</strong></div>
          <div><span>Candidates</span><strong>{selected.candidate_count}</strong></div><div><span>Sources</span><strong>{selected.source_count}</strong></div>
          <div><span>Evidence</span><strong>{selected.evidence_count}</strong></div><div><span>Attributions</span><strong>{selected.attribution_count}</strong></div>
        </div>
        <div className="detail-list"><h3>召回候选</h3>{selected.candidates.length === 0 ? <span className="muted">zero-hit</span> : selected.candidates.map((candidate) =>
          <div key={candidate.id}><strong className="mono">#{candidate.rank ?? "-"} {candidate.memory_item_id}</strong><span>{candidate.decision} / {candidate.sources.map((source) => source.type).join(", ") || "no source"}</span></div>)}</div>
      </aside>}
    </div>
  );
}
