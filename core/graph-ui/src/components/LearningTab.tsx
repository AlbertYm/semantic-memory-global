import { useCallback, useEffect, useState } from "react";
import { Pause, Play, RefreshCw, RotateCcw } from "lucide-react";
import { managerFetch, callTool } from "../api/rpc";

interface LearningState {
  enabled: boolean; ready: boolean; generation: number; last_code: number;
  batch_limit: number; pair_batch_limit: number;
  items: { item_id: string; positive: number; negative: number; utility: number; decay: number; status: string; restorable: boolean }[];
  pending_user_confirmations?: { evidence_id: string; result_hash: string; evidence_ref: string; result_ref: string; memory_summary: string }[];
  associations: { src_id: string; dst_id: string; success_count: number }[];
}

export function LearningTab({ project }: { project: string }) {
  const [data, setData] = useState<LearningState | null>(null);
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const refresh = useCallback(async () => {
    if (!project) return;
    try { setData(await callTool<LearningState>("memory_learning_status", { project })); setError(""); }
    catch (value) { setError(value instanceof Error ? value.message : "读取失败"); }
  }, [project]);
  useEffect(() => { setData(null); void refresh(); }, [refresh]);
  async function control(action: "pause" | "resume" | "restore", item_id?: string) {
    if (!data || busy) return;
    setBusy(true);
    try {
      const result = await callTool<LearningState>("memory_learning_control", {
        project, action, item_id, expected_generation: data.generation,
        idempotency_key: crypto.randomUUID(),
      });
      if (!Array.isArray(result.items)) throw new Error("状态已变化，请刷新后重试");
      setData(result); setError("");
    } catch (value) { setError(value instanceof Error ? value.message : "操作失败"); }
    finally { setBusy(false); }
  }
  async function confirm(evidence_id: string, result_hash: string) {
    setBusy(true);
    try {
      const response = await managerFetch("/api/manager/learning-confirmation", {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ project, evidence_id, result_hash }),
      });
      if (!response.ok) throw new Error("Confirmation conflict");
      await refresh();
    } catch (value) { setError(value instanceof Error ? value.message : "Confirmation failed"); }
    finally { setBusy(false); }
  }
  return <div className="workspace-scroll">
    <section className="toolbar-band"><h2>自动学习</h2>
      <span>{data ? (data.ready ? (data.enabled ? "运行中" : "已暂停") : "等待首次学习批次") : "读取中"}</span>
      <button className="icon-button" aria-label="刷新学习状态" onClick={() => void refresh()}><RefreshCw size={16} /></button>
      {data && <button disabled={busy} onClick={() => void control(data.enabled ? "pause" : "resume")}>
        {data.enabled ? <Pause size={14} /> : <Play size={14} />}{data.enabled ? "暂停学习" : "恢复学习"}
      </button>}
    </section>
    {error && <div role="alert" className="state-line error">{error}</div>}
    <section className="section-block"><h2>学习依据</h2>
      <p>完成任务中实际使用的记忆，关联可核验的执行结果或用户确认后才取得信用。重复召回和模型自述不增加信用。共同有效使用形成关联，保留原始事实与作用域。</p>
      <p>每批最多处理 {data?.batch_limit ?? 64} 条记忆和 {data?.pair_batch_limit ?? 64} 对关联。过期内容逐步降权；自动归档保留内容和向量，可按版本恢复。偏好、约束和重要决策受保护。</p>
      {data?.last_code !== undefined && data.last_code !== 0 && <p className="error">最近批次未提交，可能触及预算或数据校验边界。原状态保留，请查看诊断。</p>}
    </section>
    <section className="section-block"><h2>信用与可恢复归档</h2>
      <div className="data-table learning-table"><div className="table-row table-head"><span>记忆 ID</span><span>有效 / 否定</span><span>信用调整</span><span>衰减</span><span>状态</span><span>恢复</span></div>
        {data?.items.map(item => <div className="table-row" key={item.item_id}>
          <span className="mono truncate" title={item.item_id}>{item.item_id}</span><span>{item.positive} / {item.negative}</span>
          <span>{item.utility.toFixed(4)}</span><span>{item.decay.toFixed(4)}</span><span>{item.status}</span>
          <span>{item.restorable && <button disabled={busy} onClick={() => void control("restore", item.item_id)}><RotateCcw size={14} />恢复</button>}</span>
        </div>)}
      </div>{data && !data.items.length && <p>尚无学习记录。可核验的使用证据产生后会显示在这里。</p>}
    </section>
    {!!data?.pending_user_confirmations?.length && <section className="section-block"><h2>待用户确认的证据</h2>
      <p>仅当你确认对应结果真实有效时点击。模型填写 explicit_user 本身不会增加信用。</p>
      {data.pending_user_confirmations.map(entry => <p key={entry.evidence_id}><span>{entry.memory_summary}</span> <span>{entry.result_ref} / {entry.evidence_ref}</span> <button disabled={busy} onClick={() => void confirm(entry.evidence_id,entry.result_hash)}>我确认此结果</button></p>)}
    </section>}
    <section className="section-block"><h2>共同使用关联</h2>{data?.associations.map(edge => <p className="mono" key={`${edge.src_id}:${edge.dst_id}`}>{edge.src_id} ↔ {edge.dst_id} · {edge.success_count} 个有效任务</p>)}
      {data && !data.associations.length && <p>尚无经过有效任务确认的共同使用关联。</p>}
    </section>
  </div>;
}
