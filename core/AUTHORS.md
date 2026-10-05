# Authors and Attribution

This project is a derivative work. It exists in three layers, each built on the
one before it. All upstream copyright notices are retained in full as required
by the MIT License; see [`LICENSE`](LICENSE) and
[`THIRD_PARTY.md`](THIRD_PARTY.md).

## Layer 1 — Upstream engine

**[DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp)**

The original code intelligence engine: the pure-C indexing pipeline,
tree-sitter AST extraction across 158 languages, Hybrid LSP semantic type
resolution, the knowledge-graph store, the Cypher query layer, and the MCP tool
surface. Described in *Codebase-Memory: Tree-Sitter-Based Knowledge Graphs for
LLM Code Exploration via MCP* ([arXiv:2603.27277](https://arxiv.org/abs/2603.27277)).

Governance and review routing for the upstream project are recorded in
[`MAINTAINERS.md`](MAINTAINERS.md).

## Layer 2 — ADR long-term memory fork

**[ZR113146/semantic-memory-mcp](https://github.com/ZR113146/semantic-memory-mcp)**
— MIT copyright holder, `Copyright (c) 2025 ZR113146`

Added the ADR (Architecture Decision Record) long-term memory system and
Chinese-optimized CJK full-text-search segmentation, confined to `src/memory/`
and `src/mcp/mcp_memory_handlers.c` so that upstream merges remain trivial
(zero `store.c` diff).

## Layer 3 — Neuroplastic memory mechanisms

**[AlbertYm](https://github.com/AlbertYm)** — this repository

Designed and implemented a biologically-inspired neuroplasticity layer over the
ADR memory system, developed across staged phases 0 through 14. Each stage was
gated on real verification evidence rather than on code completion alone. The
substantive additions are:

Bounded graph activation spreading, modelled on maze flood-fill, which finds
indirectly relevant memories beyond the initial candidate set while holding
noise and cost under explicit budgets. Edge-level success-path reinforcement,
modelled on ant-colony pheromone trails, which makes connections that real
evidence proved useful more likely to be traversed in future tasks.
Slime-mould conductance with decay and soft pruning, which lets durable
connections consolidate into trunk paths while low-value connections drop out
of default propagation and stay recoverable. Concept generation with memory
growth, merging, abstraction, and branching, modelled on neuronal
consolidation, which forms higher-level knowledge without destroying the
original memories.

Supporting work: an observe-only recall session and path-tracing layer,
task-result and external-evidence feedback, security/privacy/concurrency and
recovery hardening, Codex integration with a Windows offline installer, a
controlled-experiment harness for release acceptance, and the Stage 14
lifecycle migration that moved 19,983 rows of lifecycle closure data into the
global store with live verification.

The neuroplasticity work is documented in `Neuroplastic_Memory_Codex_Project_Spec.md`,
`PROJECT_TASK_PLAN.md`, and `PROJECT_PROGRESS.md`.

## Vendored third-party code

159 pre-generated tree-sitter grammars, the tree-sitter C runtime, mimalloc,
tre, xxhash, yyjson, and other vendored components remain the work of their
respective upstream authors. Each vendored directory carries its upstream
license file. The canonical provenance record for grammars — upstream
repository, pinned commit, and cross-registry verification status — is
[`internal/cbm/vendored/grammars/MANIFEST.md`](internal/cbm/vendored/grammars/MANIFEST.md).
Full details are in [`THIRD_PARTY.md`](THIRD_PARTY.md).
