# semantic-memory-mcp

[![GitHub Release](https://img.shields.io/github/v/release/AlbertYm/neuroplastic-memory-mcp?style=flat&color=blue)](https://github.com/AlbertYm/neuroplastic-memory-mcp/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![CI](https://img.shields.io/github/actions/workflow/status/AlbertYm/neuroplastic-memory-mcp/dry-run.yml?label=CI)](https://github.com/AlbertYm/neuroplastic-memory-mcp/actions/workflows/dry-run.yml)
[![Tests](https://img.shields.io/badge/tests-5600+_passing-brightgreen)](https://github.com/AlbertYm/neuroplastic-memory-mcp)
[![Languages](https://img.shields.io/badge/languages-158-orange)](https://github.com/AlbertYm/neuroplastic-memory-mcp)
[![Hybrid LSP](https://img.shields.io/badge/Hybrid_LSP-11_languages-blue)](#hybrid-lsp)
[![Agents](https://img.shields.io/badge/agents-11-purple)](https://github.com/AlbertYm/neuroplastic-memory-mcp)
[![Pure C](https://img.shields.io/badge/pure_C-zero_dependencies-blue)](https://github.com/AlbertYm/neuroplastic-memory-mcp)
[![Platform](https://img.shields.io/badge/macOS_%7C_Linux_%7C_Windows-supported-lightgrey)](https://github.com/AlbertYm/neuroplastic-memory-mcp/releases/latest)
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/AlbertYm/neuroplastic-memory-mcp/badge)](https://scorecard.dev/viewer/?uri=github.com/AlbertYm/neuroplastic-memory-mcp)
[![SLSA 3](https://slsa.dev/images/gh-badge-level3.svg)](https://slsa.dev)
[![VirusTotal](https://img.shields.io/badge/VirusTotal-scanned_every_release-brightgreen?logo=virustotal)](https://github.com/AlbertYm/neuroplastic-memory-mcp/releases/latest)
[![arXiv](https://img.shields.io/badge/arXiv-2603.27277-b31b1b?logo=arxiv)](https://arxiv.org/abs/2603.27277)

> **AlbertYm fork** — adds a **neuroplastic long-term memory layer** (bounded graph activation spreading, edge-level reinforcement, slime-mould conductance with decay and soft pruning, and concept generation) on top of [ZR113146/semantic-memory-mcp](https://github.com/ZR113146/semantic-memory-mcp), which is itself an upstream-aligned fork of [DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp) carrying a locally-developed **ADR long-term memory system** and **Chinese-optimized CJK FTS segmentation**. Memory/ADR code lives in `src/memory/` and `src/mcp/mcp_memory_handlers.c` so upstream merges remain trivial (<1 minute, zero store.c conflicts). Full layer-by-layer attribution: [`AUTHORS.md`](AUTHORS.md).

## What This Is

**The fastest code intelligence engine for AI coding agents.** Full-indexes an average repository in milliseconds, the Linux kernel (28M LOC, 75K files) in 3 minutes. Answers structural queries in under 1ms. Ships as a single static binary for macOS, Linux, and Windows — download, run `install`, done.

High-quality parsing through [tree-sitter](https://tree-sitter.github.io/tree-sitter/) AST analysis across all 158 languages, enhanced with [**Hybrid LSP** semantic type resolution](#hybrid-lsp) for 11 languages — producing a persistent knowledge graph of functions, classes, call chains, HTTP routes, and cross-service links, plus a [**self-maintaining long-term memory**](#long-term-memory) that recalls prior decisions automatically. **27 MCP tools** (25 upstream + 2 local ADR). Zero dependencies. Plug and play across 11 coding agents.

> The upstream project is described in [*Codebase-Memory: Tree-Sitter-Based Knowledge Graphs for LLM Code Exploration via MCP*](https://arxiv.org/abs/2603.27277) (arXiv:2603.27277). This fork adds ADR long-term memory with Chinese support.

## Fork Differences

This fork maintains **near-zero diff** with upstream on core engine files (store.c = 0 lines difference, store.h = +3 lines, mcp.c = ~30 lines). Local additions are confined to dedicated files:

| Component | Files | Description |
|---|---|---|
| **ADR Memory System** | `src/memory/memory_store.{c,h}` | Long-term memory: 768-d embeddings, CJK bigram FTS, decay model, conflict resolution, code-anchored scoring |
| **ADR MCP Tools** | `src/mcp/mcp_memory_handlers.c` | `adr_list` (ADR index / MEMORY.md equivalent) and `adr_chain` (supersedes chain walker) |
| **Internal helpers** | `src/store/store_internal.h` | Shared `struct cbm_store` definition |

**Why the fork?** The upstream project focuses on code graph performance and language coverage. This fork adds an ADR (Architecture Decision Record) long-term memory layer on top — durable, self-maintaining, and code-anchored — without touching the core engine files that upstream iterates on rapidly.

## Quick Start

**One-line install** (macOS / Linux):
```bash
curl -fsSL https://raw.githubusercontent.com/AlbertYm/neuroplastic-memory-mcp/main/install.sh | bash
```

With graph visualization UI:
```bash
curl -fsSL https://raw.githubusercontent.com/AlbertYm/neuroplastic-memory-mcp/main/install.sh | bash -s -- --ui
```

**Windows** (PowerShell):
```powershell
Invoke-WebRequest -Uri https://raw.githubusercontent.com/AlbertYm/neuroplastic-memory-mcp/main/install.ps1 -OutFile install.ps1
.\install.ps1
```

Options: `--ui` (graph visualization), `--skip-config` (binary only), `--dir=<path>` (custom location).

Restart your coding agent. Say **"Index this project"** — done.

> Install your coding agent first. `install` configures only the agents it can detect. Start the agent once, then run `install`. Re-run `install` after upgrading the binary so new hooks get registered.

### Keeping Up to Date

```bash
semantic-memory-mcp update
```

### Uninstall

```bash
semantic-memory-mcp uninstall
```

Removes all agent configs, skills, hooks, and instructions. Does not remove the binary or SQLite databases.

## Features

### Graph & Analysis
- **Architecture overview**: `get_architecture` returns languages, packages, entry points, routes, hotspots, boundaries, layers, and clusters
- **Call graph**: `trace_path` BFS traversal with Hybrid LSP semantic resolution across 11 languages
- **Dead code detection**: Functions with zero callers, excluding entry points
- **Cypher-like queries**: `query_graph` with openCypher read subset
- **Git diff impact mapping**: `detect_changes` with risk classification
- **Louvain community detection**: Functional module discovery
- **Cross-service linking**: HTTP route ↔ call-site matching, gRPC, GraphQL, tRPC
- **Cross-repo intelligence**: `CROSS_*` edges linking nodes across multiple repos

### Long-Term Memory (ADR)

Beyond the code graph, a **self-maintaining long-term memory** stores durable decisions, lessons, and preferences that persist across sessions:

- **Write via `events`** — one entrypoint, ADR-style format enforced server-side
- **Retrieve via `memories_retrieve`** — vector (768-d nomic) + FTS (CJK bigram) + structured recall
- **Auto-maintenance** — consolidate, decay, conflict adjudication run lazily on the hot path
- **Code anchoring** — `about_code` edges link memories to graph symbols; confidence/reusability derived from symbol topology
- **ADR tools** — `adr_list` (structured index, MEMORY.md equivalent) and `adr_chain` (supersedes version timeline)
- **Lifecycle** — candidate → active → deprecated → archived; soft/hard/purge delete with audit trail

See [ADR Memory System](#adr-memory-system) for details.

### MCP Tools (27 total)

#### Indexing
| Tool | Description |
|------|-------------|
| `index_repository` | Index a repository into the graph |
| `list_projects` | List all indexed projects |
| `delete_project` | Remove a project and its data |
| `index_status` | Check indexing status |

#### Querying
| Tool | Description |
|------|-------------|
| `search_graph` | Structured search by label, name, file, degree |
| `trace_path` | BFS call-chain traversal |
| `detect_changes` | Git diff → affected symbols + risk |
| `query_graph` | Cypher-like graph queries |
| `get_graph_schema` | Node/edge counts, relationship patterns |
| `get_code_snippet` | Source code for a function |
| `get_architecture` | Languages, packages, routes, hotspots |
| `search_code` | Graph-augmented grep |

#### Long-Term Memory
| Tool | Description |
|------|-------------|
| `events` | Write one memory item (ADR format enforced) |
| `memories_retrieve` | Vector + FTS + structured recall |
| `adr_list` ⬢ | Structured ADR index (MEMORY.md equivalent) |
| `adr_chain` ⬢ | Walk supersedes version chain |
| `memories_inspect` | List items for review |
| `memory_update_status` | Mark item status |
| `memory_feedback` | Record feedback, adjust health |
| `memory_delete` | Soft/hard/purge/restore |
| `memory_health` | Health counters and backlog |
| `admin_consolidate` / `admin_decay` | Manual maintenance (usually automatic) |
| `manage_adr` | Upstream legacy ADR (file-based) |
| `ingest_traces` | Ingest runtime traces |

⬢ = local-fork addition

### Edge Types (selected)
`CALLS`, `IMPORTS`, `DEFINES`, `IMPLEMENTS`, `INHERITS`, `HTTP_CALLS`, `ASYNC_CALLS`, `EMITS`, `LISTENS_ON`, `DATA_FLOWS`, `SIMILAR_TO`, `SEMANTICALLY_RELATED`

## ADR Memory System

### Architecture

```
events(...)            →  raw event + candidate written (hot path, transactional)
   ↓ (automatic, time-gated)
consolidate            →  dedup, 768-d embeddings, evidence edges, conflict adjudication
   ↓
memories_retrieve(...)  →  vector + FTS + structured recall, hit-counted
   ↓ (automatic)
decay / retention      →  stale archived; deleted swept after grace period
```

### Key design decisions

- **Embeddings**: 768-d nomic-embed-code vectors compiled into binary. Optional bge-m3 sidecar for higher recall (top-1: 0.83 → 0.96).
- **CJK FTS**: Character bigram segmentation (`memory_segment_cjk`) for Chinese/Japanese/Korean — restores recall that unicode61 tokenizer loses on unsegmented scripts.
- **Code anchoring**: `about_code` edges link memories to graph symbols. Confidence/reusability derived from symbol topology (fan-in/out, entry point status).
- **P3/P4 scoring**: Graph-signal scoring (confidence/reusability), reference/anchoring dimensions, importance falsification, and ADR red line (never physically purge anchored decisions).
- **Global scope**: `scope_project=NULL` memories in `__global__-memory.db` surface from every project, down-weighted to not crowd out project-specific hits.

### Source layout

```
src/memory/
  memory_store.h        All memory types and declarations (252 lines)
  memory_store.c        Full memory/ADR implementation (3,479 lines)
src/mcp/
  mcp_memory_handlers.c ADR MCP handlers: adr_list, adr_chain (437 lines)
  mcp.c                 Upstream + ~30 ADR injection lines
src/store/
  store.c               100% upstream (0 local diff)
  store.h               Upstream + 3 lines (#include "memory/memory_store.h")
```

### Writing memories

The `events` tool enforces an ADR-style format for code decisions:

```
[Decision] What was decided
[Context] Why, with absolute date
[Rejected alternatives] Options considered and why dropped
[Anchors] Commit hashes and files touched
```

Use `about_code` to anchor memories to code symbols. The server derives confidence/reusability scores from the symbol's graph topology — you get objective signal instead of self-reported defaults.

## Upstream Alignment

This fork is designed to track upstream with **minimal merge cost**:

```bash
git fetch origin         # origin = DeusData/codebase-memory-mcp
git merge origin/main
# Conflicts: only branding files (README, install.ps1, .gitignore) — use ours
# store.c: auto-merges (0 local diff)
# store.h: auto-merges (3-line append)
# Makefile.cbm: merge MCP_SRCS = ... + mcp_memory_handlers.c + MEMORY_SRCS
# Build + test, done.
```

| File | Local diff | Merge behavior |
|---|---|---|
| `src/store/store.c` | 0 lines | Auto-merge, never conflicts |
| `src/store/store.h` | +3 lines | Auto-merge |
| `src/mcp/mcp.c` | ~30 lines | Minor conflict on dispatch table |

## Performance

| Operation | Time | Notes |
|-----------|------|-------|
| Linux kernel full index | 3 min | 28M LOC, 75K files → 4.81M nodes, 7.72M edges |
| Django full index | ~6s | 49K nodes, 196K edges |
| Cypher query | <1ms | Relationship traversal |
| Name search (regex) | <10ms | SQL LIKE pre-filtering |
| Trace call path (depth=5) | <10ms | BFS traversal |

**RAM-first pipeline**: All indexing runs in memory (LZ4 HC compressed read, in-memory SQLite, single dump at end). Memory released after indexing.

**Token efficiency**: Five structural queries consumed ~3,400 tokens vs ~412,000 tokens via file-by-file grep — a **99.2% reduction**.

## Graph Visualization UI

If using the UI variant:

```bash
semantic-memory-mcp --ui=true --port=9749
```

Open `http://localhost:9749`. 3D interactive knowledge graph. The UI runs alongside the MCP server — available whenever your agent is connected.

## Auto-Index

```bash
semantic-memory-mcp config set auto_index true
```

New projects indexed automatically on first connection. Configurable limit: `config set auto_index_limit 50000`. Projects already indexed are registered with the background watcher.

## CLI Mode

```bash
semantic-memory-mcp cli index_repository '{"repo_path": "/path/to/repo"}'
semantic-memory-mcp cli search_graph '{"name_pattern": ".*Handler.*", "label": "Function"}'
semantic-memory-mcp cli trace_path '{"function_name": "Search", "direction": "both"}'
semantic-memory-mcp cli query_graph '{"query": "MATCH (f:Function) RETURN f.name LIMIT 5"}'
```

## Hybrid LSP

Semantic type resolution beyond tree-sitter for 11 languages: Python, TypeScript/JS/JSX/TSX, PHP, C#, Go, C, C++, Java, Kotlin, Rust. A lightweight C implementation of language type-resolution algorithms embedded in the static binary — no language server process, no API key. Refines `CALLS`, `USAGE`, and `RESOLVED_CALLS` edges with type information.

## Supported Languages

**158 languages**, all parsed via vendored tree-sitter grammars compiled into the binary.

Benchmarked (Excellent ≥90%): Lua, Kotlin, C++, Perl, Objective-C, Groovy, C, Bash, Zig, Swift, CSS, YAML, TOML, HTML, SCSS, HCL, Dockerfile

Good (75-89%): Python, TypeScript, TSX, Go, Rust, Java, R, Dart, JavaScript, Erlang, Elixir, Scala, Ruby, PHP, C#, SQL

Also: Ada through Zsh — 120+ additional languages with vendored grammars.

## Configuration

```bash
semantic-memory-mcp config list
semantic-memory-mcp config set auto_index true
semantic-memory-mcp config set auto_index_limit 50000
```

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CBM_CACHE_DIR` | `~/.cache/semantic-memory-mcp` | Database storage directory |
| `CBM_LOG_LEVEL` | `info` | Minimum log level: debug, info, warn, error, none |
| `CBM_WORKERS` | *(detected)* | Parallel indexing worker count (1-256) |
| `CBM_DIAGNOSTICS` | `false` | Enable periodic diagnostics |
| `CBM_DUMP_VERIFY_MIN_RATIO` | `0.5` | Verify persisted vs committed node ratio |

## Build from Source

```bash
git clone https://github.com/AlbertYm/neuroplastic-memory-mcp.git
cd semantic-memory-mcp
scripts/build.sh                    # standard binary
scripts/build.sh --with-ui          # with graph visualization
```

Prerequisites: C compiler (gcc/clang), C++ compiler, zlib, git.

## Team-Shared Graph Artifact

Commit `.semantic-memory/graph.db.zst` to share the pre-built knowledge graph. Teammates skip reindex. Two compression tiers (best for explicit exports, fast for watcher updates). `.gitattributes` auto-set for `merge=ours` on the binary artifact.

## Multi-Agent Support

`install` auto-detects 11 agents: Claude Code, Codex CLI, Gemini CLI, Zed, OpenCode, Antigravity, Aider, KiloCode, VS Code, OpenClaw, Kiro. MCP configs, instruction files, and hooks (memory recall + code discovery) configured automatically.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `/mcp` doesn't show server | Check `.mcp.json` path is absolute. Restart agent. |
| `index_repository` fails | Pass absolute path: `repo_path="/absolute/path"` |
| `trace_path` returns 0 results | Use `search_graph(name_pattern=".*PartialName.*")` first |
| Binary not found after install | Add to PATH: `export PATH="$HOME/.local/bin:$PATH"` |
| UI not loading | Use `ui` variant + `--ui=true`. Check `http://localhost:9749`. |

## Security

- Zero runtime dependencies, zero telemetry, 100% local
- VirusTotal scanned every release (70+ engines)
- SLSA Level 3 provenance + Sigstore keyless signatures
- SHA-256 checksums verified by install scripts
- CodeQL SAST blocks release on open alerts

## License

MIT. Upstream: [DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp).
