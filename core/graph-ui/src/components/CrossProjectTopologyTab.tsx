import { ChevronLeft, ChevronRight, Network, RefreshCw } from "lucide-react";
import { useState, type CSSProperties } from "react";
import { useGlobalList } from "../hooks/useGlobalMemory";
import type { GlobalTopologyEdge } from "../lib/types";

const MAX_VISIBLE_NODES = 32;
const GRID_COLUMNS = 4;
const GRID_ROWS = 8;
const CANVAS_WIDTH = 1000;
const CANVAS_HEIGHT = 620;

interface TopologyNode {
  id: string;
  x: number;
  y: number;
}

function compareProjectIds(left: string, right: string) {
  return left < right ? -1 : left > right ? 1 : 0;
}

function layoutTopology(projectIds: string[]): TopologyNode[] {
  return projectIds.map((id, index) => ({
    id,
    x: ((index % GRID_COLUMNS) + 0.5) * (CANVAS_WIDTH / GRID_COLUMNS),
    y: (Math.floor(index / GRID_COLUMNS) + 0.5) * (CANVAS_HEIGHT / GRID_ROWS),
  }));
}

function selectVisibleProjectIds(projectIds: string[], selected: GlobalTopologyEdge | null) {
  const visible = projectIds.slice(0, MAX_VISIBLE_NODES);
  if (!selected || visible.length < MAX_VISIBLE_NODES) return visible;

  const selectedIds = [selected.source_project_uuid, selected.target_project_uuid];
  let replacementIndex = visible.length - 1;
  for (const projectId of selectedIds) {
    if (!visible.includes(projectId)) {
      visible[replacementIndex] = projectId;
      replacementIndex -= 1;
    }
  }
  return visible;
}

export function CrossProjectTopologyTab() {
  const { data, error, loading, cursor, hasPrevious, nextPage, previousPage, refresh } = useGlobalList<GlobalTopologyEdge>(
    "manager_global_topology",
    { limit: 100 },
  );
  const [selectedEdgeId, setSelectedEdgeId] = useState<string | null>(null);

  if (loading) return <div className="state-line">正在读取跨项目拓扑 / Loading cross-project topology...</div>;
  if (error || !data) return <div className="state-line error">{error || "TOPOLOGY_UNAVAILABLE"}</div>;

  const edges = data.items;
  const selected = edges.find((edge) => edge.edge_id === selectedEdgeId) ?? null;
  const projectIds = [...new Set(edges.flatMap((edge) => [edge.source_project_uuid, edge.target_project_uuid]))].sort(compareProjectIds);
  const visibleProjectIds = selectVisibleProjectIds(projectIds, selected);
  const nodes = layoutTopology(visibleProjectIds);
  const nodeById = new Map(nodes.map((node) => [node.id, node]));
  const renderedEdges = edges.filter(
    (edge) => nodeById.has(edge.source_project_uuid) && nodeById.has(edge.target_project_uuid),
  );
  const selectedProjects = new Set(selected ? [selected.source_project_uuid, selected.target_project_uuid] : []);
  const focusNode = (projectId: string) => {
    const adjacent = edges
      .filter((edge) => edge.source_project_uuid === projectId || edge.target_project_uuid === projectId)
      .sort((left, right) => compareProjectIds(left.edge_id, right.edge_id))[0];
    if (adjacent) setSelectedEdgeId(adjacent.edge_id);
  };

  return (
    <div className="workspace-scroll">
      <section className="toolbar-band topology-toolbar">
        <h2>跨项目拓扑 / Cross-project topology</h2>
        <span>{projectIds.length} 项目 / Projects · {data.total} 关系 / Relations · offset {cursor}</span>
        <div className="topology-toolbar-actions">
          <button className="icon-button" title="上一页 / Previous page" aria-label="上一页 / Previous page" disabled={!hasPrevious} onClick={previousPage}>
            <ChevronLeft size={16} />
          </button>
          <button className="icon-button" title="下一页 / Next page" aria-label="下一页 / Next page" disabled={data.next_cursor === null} onClick={nextPage}>
            <ChevronRight size={16} />
          </button>
          <button className="icon-button" title="刷新 / Refresh" aria-label="刷新 / Refresh" onClick={() => void refresh()}>
            <RefreshCw size={16} />
          </button>
        </div>
      </section>

      <section className="section-block topology-surface" aria-label="跨项目拓扑图 / Cross-project topology graph">
        {nodes.length > 0 && (
          <svg className="topology-edge-canvas" viewBox={`0 0 ${CANVAS_WIDTH} ${CANVAS_HEIGHT}`} preserveAspectRatio="none" aria-hidden="true">
            {renderedEdges.map((edge) => {
              const source = nodeById.get(edge.source_project_uuid)!;
              const target = nodeById.get(edge.target_project_uuid)!;
              return (
                <line
                  key={edge.edge_id}
                  x1={source.x}
                  y1={source.y}
                  x2={target.x}
                  y2={target.y}
                  className={selectedEdgeId === edge.edge_id ? "topology-edge selected" : "topology-edge"}
                />
              );
            })}
          </svg>
        )}
        {nodes.map((node) => {
          const style = {
            "--topology-x": `${(node.x / CANVAS_WIDTH) * 100}%`,
            "--topology-y": `${(node.y / CANVAS_HEIGHT) * 100}%`,
          } as CSSProperties;
          return (
            <button
              className={`topology-node ${selectedProjects.has(node.id) ? "selected" : ""}`}
              key={node.id}
              style={style}
              onClick={() => focusNode(node.id)}
              title={node.id}
            >
              <Network size={15} />
              <strong className="mono">{node.id}</strong>
              <span>{selectedProjects.has(node.id) ? "已选关系 / Selected relation" : "溯源节点 / Provenance node"}</span>
            </button>
          );
        })}
        {projectIds.length === 0 && <div className="empty-state">暂无跨项目节点 / No cross-project nodes</div>}
      </section>

      <section className="section-block">
        <h2>关系 / Relations</h2>
        <div className="data-table topology-table">
          <div className="table-row table-head">
            <span>来源 / Source</span>
            <span>目标 / Target</span>
            <span>类型 / Type</span>
            <span>权重 / Weight</span>
            <span>置信度 / Confidence</span>
            <span>溯源 / Provenance</span>
          </div>
          {edges.map((edge) => (
            <button
              className={`table-row task-row ${selectedEdgeId === edge.edge_id ? "selected" : ""}`}
              key={edge.edge_id}
              onClick={() => setSelectedEdgeId(edge.edge_id)}
            >
              <span className="mono truncate">{edge.source_project_uuid}</span>
              <span className="mono truncate">{edge.target_project_uuid}</span>
              <span>{edge.relation_type}</span>
              <span>{edge.weight_ppm}</span>
              <span>{edge.confidence_ppm}</span>
              <span className="truncate">{edge.provenance.source_kind} / {edge.provenance.evidence_event_id || "无证据 / No evidence"}</span>
            </button>
          ))}
        </div>
      </section>
    </div>
  );
}
