import { FolderKanban, RefreshCw } from "lucide-react";
import { useGlobalDetail } from "../hooks/useGlobalMemory";
import type { GlobalOverview } from "../lib/types";

export function AllProjectsTab() {
  const { data, error, loading, refresh } = useGlobalDetail<GlobalOverview>("manager_global_overview");
  if (loading) return <div className="state-line">正在读取全局概览...</div>;
  if (error || !data) return <div className="state-line error">{error || "GLOBAL_OVERVIEW_UNAVAILABLE"}</div>;
  const metrics = [["项目", data.project_count], ["记忆", data.memory_count], ["跨项目关系", data.cross_project_edge_count], ["任务", data.task_count]];
  return <div className="workspace-scroll">
    <section className="toolbar-band"><h2>所有项目</h2><span>全局目录与 provenance</span><button className="icon-button" title="刷新" onClick={() => void refresh()}><RefreshCw size={16} /></button></section>
    <section className="global-metrics">{metrics.map(([label, value]) => <div key={String(label)}><span>{label}</span><strong>{value}</strong></div>)}</section>
    <section className="section-block"><h2>工程目录</h2><div className="data-table global-project-table"><div className="table-row table-head"><span>项目</span><span>Project UUID</span><span>工作区</span><span>索引</span><span>记忆</span><span>任务</span></div>
      {data.projects.map((project) => <div className="table-row" key={project.project_uuid}><span className="role"><FolderKanban size={14} />{project.display_name}</span><span className="mono truncate">{project.project_uuid}</span><span>{project.workspace_state}</span><span className="ok">{project.index_state}</span><span>{project.memory_count}</span><span>{project.task_count}</span></div>)}
    </div></section>
  </div>;
}
