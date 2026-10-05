import { useEffect, useState } from "react";
import type { LucideIcon } from "lucide-react";
import { Activity, Archive, BrainCircuit, Database, GitBranch, ListChecks, Network, Settings, Share2, Sparkles, Wrench } from "lucide-react";
import { AllProjectsTab } from "./components/AllProjectsTab";
import { GlobalMemoryTab } from "./components/GlobalMemoryTab";
import { CrossProjectTopologyTab } from "./components/CrossProjectTopologyTab";
import { EvolutionTimelineTab } from "./components/EvolutionTimelineTab";
import { DriftRepairTab } from "./components/DriftRepairTab";
import { DashboardTab } from "./components/DashboardTab";
import { SessionsTab } from "./components/SessionsTab";
import { RecallPathPanel } from "./components/RecallPathPanel";
import { MemoryInspectorTab } from "./components/MemoryInspectorTab";
import { ReviewTab } from "./components/ReviewTab";
import { OperationsTab } from "./components/OperationsTab";
import { DiagnosticsTab } from "./components/DiagnosticsTab";
import type { TabId } from "./lib/types";

const TAB_IDS: TabId[] = ["overview", "all-projects", "global-memory", "topology", "evolution", "drift", "tasks", "recall", "memory", "review", "operations", "diagnostics"];
const DEFAULT_PROJECT = "H-Codex_H-neuroplastic-main";

const labels = {
  zh: { overview: "概览", "all-projects": "所有项目", "global-memory": "全局记忆", topology: "跨项目拓扑", evolution: "演化记录", drift: "漂移与维护", tasks: "任务", recall: "召回路径", memory: "Memory", review: "审核", operations: "备份恢复", diagnostics: "诊断" },
  en: { overview: "Overview", "all-projects": "All Projects", "global-memory": "Global Memory", topology: "Topology", evolution: "Evolution", drift: "Drift & Maintenance", tasks: "Tasks", recall: "Recall", memory: "Memory", review: "Review", operations: "Backup", diagnostics: "Diagnostics" },
};

const icons: Record<TabId, LucideIcon> = {
  overview: Activity, tasks: ListChecks, recall: Share2, memory: Database,
  review: BrainCircuit, operations: Archive, diagnostics: Settings,
  "all-projects": GitBranch, "global-memory": Sparkles, topology: Network, evolution: Activity, drift: Wrench,
};

function initialTab(): TabId {
  const value = new URLSearchParams(window.location.search).get("tab") as TabId | null;
  return value && TAB_IDS.includes(value) ? value : "overview";
}

export function App() {
  const params = new URLSearchParams(window.location.search);
  const [activeTab, setActiveTab] = useState<TabId>(initialTab);
  const [project, setProject] = useState(params.get("project") || DEFAULT_PROJECT);
  const [language, setLanguage] = useState<"zh" | "en">("zh");

  useEffect(() => {
    const next = new URLSearchParams();
    next.set("tab", activeTab);
    next.set("project", project);
    window.history.replaceState(null, "", `${window.location.pathname}?${next}`);
  }, [activeTab, project]);

  return (
    <div className="manager-shell">
      <header className="manager-header">
        <div className="brand"><BrainCircuit size={19} /><strong>Semantic Memory</strong><span>Beta</span></div>
        <label className="project-field"><span>Project</span><input value={project} onChange={(event) => setProject(event.target.value)} /></label>
        <div className="segmented" aria-label="Language">
          <button aria-pressed={language === "zh"} onClick={() => setLanguage("zh")}>中</button>
          <button aria-pressed={language === "en"} onClick={() => setLanguage("en")}>EN</button>
        </div>
      </header>
      <div className="manager-body">
        <nav className="manager-nav" aria-label="Manager views">
          {TAB_IDS.map((tab) => {
            const Icon = icons[tab];
            return <button key={tab} className={activeTab === tab ? "active" : ""} onClick={() => setActiveTab(tab)} title={labels[language][tab]}>
              <Icon size={17} /><span>{labels[language][tab]}</span>
            </button>;
          })}
        </nav>
        <main className="manager-content">
          {activeTab === "overview" && <DashboardTab project={project} />}
          {activeTab === "all-projects" && <AllProjectsTab />}
          {activeTab === "global-memory" && <GlobalMemoryTab />}
          {activeTab === "topology" && <CrossProjectTopologyTab />}
          {activeTab === "evolution" && <EvolutionTimelineTab />}
          {activeTab === "drift" && <DriftRepairTab />}
          {activeTab === "tasks" && <SessionsTab project={project} />}
          {activeTab === "recall" && <RecallPathPanel project={project} />}
          {activeTab === "memory" && <MemoryInspectorTab project={project} />}
          {activeTab === "review" && <ReviewTab />}
          {activeTab === "operations" && <OperationsTab project={project} />}
          {activeTab === "diagnostics" && <DiagnosticsTab project={project} />}
        </main>
      </div>
    </div>
  );
}
