import { useTaskSessions } from "../hooks/useTaskSessions";

export function MemoryInspectorTab({ project }: { project: string }) {
  const { tasks, loading, error } = useTaskSessions(project);
  const attributed = tasks.flatMap((task) => task.memories.map((memory) => ({ task, memory })));
  return (
    <div className="workspace-scroll">
      <section className="toolbar-band"><h2>Memory 检查器</h2><span>只读</span></section>
      {loading ? <div className="state-line">正在读取...</div> : error ? <div className="state-line error">{error}</div> : attributed.length === 0 ? (
        <div className="empty-state">没有可审计的 memory attribution</div>
      ) : attributed.map(({ task, memory }) => (
        <section className="memory-row" key={`${task.task_id}:${memory.id}:${memory.created_at}:${memory.attribution_state}`}>
          <div><span className="kicker">MEMORY</span><strong>{memory.summary || memory.entity_key || memory.id}</strong><span className="mono">{memory.id}</span></div>
          <div><span>Kind / version</span><strong>{memory.kind} / v{memory.version}</strong></div>
          <div><span>Scope</span><strong>{memory.scope || project}</strong></div><div><span>Status</span><strong>{memory.status}</strong></div>
          <div><span>Provenance</span><strong>{memory.attribution_state} / {task.retrieval_session_id ?? "direct"}</strong></div>
        </section>
      ))}
    </div>
  );
}
