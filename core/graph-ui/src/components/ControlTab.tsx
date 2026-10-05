import { DiagnosticsTab } from "./DiagnosticsTab";

/* Compatibility export. Stage12 removes process termination from the UI. */
export function ControlTab({ project = "H-Codex_H-neuroplastic-main" }: { project?: string }) {
  return <DiagnosticsTab project={project} />;
}
