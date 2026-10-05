import { ChevronLeft, ChevronRight, Database, RefreshCw } from "lucide-react";
import { useGlobalList } from "../hooks/useGlobalMemory";
import type { GlobalMemoryItem } from "../lib/types";

export function GlobalMemoryTab() {
  const { data, error, loading, cursor, hasPrevious, nextPage, previousPage, refresh } = useGlobalList<GlobalMemoryItem>("manager_global_memory", { limit: 100 });
  if (loading) return <div className="state-line">正在读取全局记忆...</div>;
  if (error || !data) return <div className="state-line error">{error || "GLOBAL_MEMORY_UNAVAILABLE"}</div>;
  return <div className="workspace-scroll"><section className="toolbar-band"><h2>全局记忆</h2><span>{data.total} 条 · offset {cursor}</span><button className="icon-button" title="上一页" aria-label="上一页" disabled={!hasPrevious} onClick={previousPage}><ChevronLeft size={16} /></button><button className="icon-button" title="下一页" aria-label="下一页" disabled={data.next_cursor === null} onClick={nextPage}><ChevronRight size={16} /></button><button className="icon-button" title="刷新" onClick={() => void refresh()}><RefreshCw size={16} /></button></section>
    <section className="section-block"><div className="memory-global-list">{data.items.map((memory) => <article key={memory.memory_item_id}><Database size={15} /><div><strong>{memory.title || memory.summary}</strong><span>{memory.kind} · {memory.status} · evidence {memory.evidence_grade}</span></div><div className="provenance"><strong>{memory.provenance.project_name || memory.project_name}</strong><span className="mono">{memory.provenance.project_uuid || memory.project_uuid}</span><span>{memory.provenance.source_kind}</span></div></article>)}</div></section>
  </div>;
}
