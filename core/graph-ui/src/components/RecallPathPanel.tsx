import { Line, OrbitControls } from "@react-three/drei";
import { Canvas } from "@react-three/fiber";
import { useState } from "react";
import { useTaskSessions } from "../hooks/useTaskSessions";
import type { RecallCandidate, RecallSource, TaskRun } from "../lib/types";

type Position = [number, number, number];
type Selection = { type: "seed" | "candidate" | "source" | "edge"; title: string; detail: string };

function candidatePosition(index: number, total: number): Position {
  const angle = (index / Math.max(total, 1)) * Math.PI * 2;
  const radius = 3.1 + (index % 2) * 0.55;
  return [Math.cos(angle) * radius, (index % 3) * 0.65 - 0.65, Math.sin(angle) * radius];
}

function sourcePosition(candidate: Position, index: number, total: number): Position {
  const angle = (index / Math.max(total, 1)) * Math.PI * 2;
  return [candidate[0] + Math.cos(angle) * 0.8, candidate[1] + 0.55, candidate[2] + Math.sin(angle) * 0.8];
}

function SourceNode({ source, position, parent, onSelect }: { source: RecallSource; position: Position; parent: Position; onSelect: (value: Selection) => void }) {
  return <>
    <Line points={[parent, position]} color="#47605e" lineWidth={1} />
    <mesh position={position} onClick={(event) => { event.stopPropagation(); onSelect({ type: "source", title: source.type, detail: `rank ${source.rank ?? "-"} / score ${source.score?.toFixed(3) ?? "-"}` }); }}>
      <octahedronGeometry args={[0.13, 0]} /><meshStandardMaterial color="#f4b942" />
    </mesh>
  </>;
}

function CandidateNode({ candidate, position, seed, onSelect }: { candidate: RecallCandidate; position: Position; seed: Position; onSelect: (value: Selection) => void }) {
  return <>
    <Line points={[seed, position]} color="#345b58" lineWidth={1.5} />
    <mesh position={position} onClick={(event) => { event.stopPropagation(); onSelect({ type: "candidate", title: candidate.memory_item_id, detail: `${candidate.decision} / rank ${candidate.rank ?? "-"} / score ${candidate.score?.toFixed(3) ?? "-"}` }); }}>
      <sphereGeometry args={[0.24, 20, 20]} /><meshStandardMaterial color={candidate.decision === "selected" ? "#42c7b9" : "#e06767"} />
    </mesh>
    {candidate.sources.map((source, index) => <SourceNode key={`${candidate.id}:${source.type}:${source.rank ?? index}`} source={source}
      position={sourcePosition(position, index, candidate.sources.length)} parent={position} onSelect={onSelect} />)}
  </>;
}

function RecallScene({ task, onSelect }: { task: TaskRun; onSelect: (value: Selection) => void }) {
  const seed: Position = [0, 0, 0];
  const positions = new Map(task.candidates.map((candidate, index) => [candidate.id, candidatePosition(index, task.candidates.length)]));
  return <>
    <color attach="background" args={["#111315"]} />
    <ambientLight intensity={0.8} /><pointLight position={[4, 7, 5]} intensity={30} />
    <gridHelper args={[14, 14, "#3c4448", "#252a2d"]} />
    <mesh position={seed} onClick={(event) => { event.stopPropagation(); onSelect({ type: "seed", title: task.task_id, detail: task.retrieval_session_id ?? "zero-hit" }); }}>
      <icosahedronGeometry args={[0.38, 1]} /><meshStandardMaterial color="#f4b942" />
    </mesh>
    {task.candidates.map((candidate, index) => <CandidateNode key={candidate.id} candidate={candidate}
      position={candidatePosition(index, task.candidates.length)} seed={seed} onSelect={onSelect} />)}
    {task.edges.map((edge) => {
      const from = edge.from ? positions.get(edge.from) : seed;
      const to = positions.get(edge.to);
      if (!from || !to) return null;
      return <Line key={edge.id} points={[from, to]} color="#8d6eaa" lineWidth={2}
        onClick={() => onSelect({ type: "edge", title: edge.relation, detail: `${edge.visit_status} / hop ${edge.hop_depth}` })} />;
    })}
    <OrbitControls enableDamping minDistance={3} maxDistance={14} />
  </>;
}

export function RecallPathPanel({ project }: { project: string }) {
  const { tasks, loading, error } = useTaskSessions(project);
  const [selected, setSelected] = useState<Selection | null>(null);
  const task = tasks.find((candidate) => candidate.candidates.length > 0) ?? tasks[0] ?? null;
  const nodeCount = task ? 1 + task.candidates.length + task.candidates.reduce((sum, candidate) => sum + candidate.sources.length, 0) : 0;
  return (
    <div className="recall-workspace">
      <div className="canvas-status"><strong>召回路径</strong><span>{tasks.length} tasks / {nodeCount} nodes / {task?.edges.length ?? 0} edges</span></div>
      {loading ? <div className="canvas-overlay">正在装载...</div> : error ? <div className="canvas-overlay error">{error}</div> : task && task.candidates.length > 0 ? (
        <Canvas camera={{ position: [0, 4.5, 8], fov: 48 }}><RecallScene task={task} onSelect={setSelected} /></Canvas>
      ) : <div className="canvas-empty"><div className="empty-grid" /><div className="canvas-overlay">当前没有召回路径</div></div>}
      {selected && <div className="canvas-detail"><span className="kicker">{selected.type}</span><strong className="mono">{selected.title}</strong><span>{selected.detail}</span></div>}
    </div>
  );
}
