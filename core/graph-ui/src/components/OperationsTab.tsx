import { Archive, RotateCcw } from "lucide-react";
import { useState } from "react";
import { managerFetch } from "../api/rpc";

export function OperationsTab({ project }: { project: string }) {
  const [backupRoot, setBackupRoot] = useState("");
  const [source, setSource] = useState("");
  const [target, setTarget] = useState("");
  const [result, setResult] = useState("");
  async function post(path: string, body: Record<string, string>) {
    setResult("running");
    const response = await managerFetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    const json = await response.json();
    setResult(`${json.status}: ${json.code ?? "OK"}`);
  }
  return <div className="workspace-scroll operations-grid">
    <section className="section-block"><h2>Fresh backup</h2><label>目标根目录<input value={backupRoot} onChange={(event) => setBackupRoot(event.target.value)} /></label>
      <button className="command-button" disabled={!backupRoot} onClick={() => void post("/api/manager/backup", { project, destination_root: backupRoot })}><Archive size={16} />创建并验证</button></section>
    <section className="section-block"><h2>新目录恢复验证</h2><label>备份目录<input value={source} onChange={(event) => setSource(event.target.value)} /></label>
      <label>全新目标目录<input value={target} onChange={(event) => setTarget(event.target.value)} /></label>
      <button className="command-button" disabled={!source || !target} onClick={() => void post("/api/manager/verify-restore", { project, source_directory: source, target_directory: target })}><RotateCcw size={16} />验证恢复</button></section>
    {result && <div className="operation-result mono">{result}</div>}
  </div>;
}
