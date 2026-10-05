import { ShieldCheck } from "lucide-react";

export function ReviewTab() {
  return <div className="workspace-scroll"><section className="toolbar-band"><h2>Concept / edge 审核</h2><span>eligible only</span></section>
    <div className="empty-state"><ShieldCheck size={24} />当前没有真实 eligible concept 或可恢复 edge</div></div>;
}
