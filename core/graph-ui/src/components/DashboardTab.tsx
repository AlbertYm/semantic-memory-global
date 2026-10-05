import { Activity, Database, LockKeyhole, RefreshCw } from "lucide-react";
import { useManagerHealth } from "../hooks/useManagerHealth";

export function DashboardTab({ project }: { project: string }) {
  const { data, error, loading, refresh } = useManagerHealth(project);
  if (loading) return <div className="state-line">正在读取本地状态...</div>;
  if (error || !data) return <div className="state-line error">{error || "MANAGER_UNAVAILABLE"}</div>;
  return (
    <div className="workspace-scroll">
      <section className="summary-band">
        <div><span className="kicker">MANAGER</span><strong>{data.version}</strong></div>
        <div><Activity size={16} /><span>本地服务</span><strong className="ok">正常</strong></div>
        <div><LockKeyhole size={16} /><span>安全策略</span><strong className="ok">{data.modes.security}</strong></div>
        <button className="icon-button" title="刷新" onClick={() => void refresh()}><RefreshCw size={16} /></button>
      </section>
      <section className="section-block">
        <h2>数据库</h2>
        <div className="data-table">
          <div className="table-row table-head"><span>角色</span><span>文件</span><span>大小</span><span>Quick</span><span>FK</span></div>
          {data.stores.map((store) => (
            <div className="table-row" key={store.role}>
              <span className="role"><Database size={14} />{store.role}</span>
              <span className="mono truncate">{store.filename}</span>
              <span>{store.size_bytes < 0 ? "-" : `${(store.size_bytes / 1048576).toFixed(1)} MB`}</span>
              <span className={store.quick_check === "ok" ? "ok" : "warn"}>{store.quick_check}</span>
              <span>{store.foreign_key_violations}</span>
            </div>
          ))}
        </div>
      </section>
      <section className="section-block mode-grid">
        <div><span>Plasticity</span><strong>{data.modes.plasticity}</strong></div>
        <div><span>Active</span><strong>{data.modes.active ? "on" : "off"}</strong></div>
        <div><span>Auto maintenance</span><strong>{data.modes.automatic_maintenance ? "on" : "off"}</strong></div>
        <div><span>Global union</span><strong>{data.modes.global_union ? "on" : "off"}</strong></div>
      </section>
    </div>
  );
}
