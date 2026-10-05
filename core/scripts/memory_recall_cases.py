#!/usr/bin/env python3
"""Fixed memory + query cases for memory_recall_eval.py.

SEED_MEMORIES: written once into a throwaway project, then consolidated.
Each entry is the argument set for the `events` tool (project is injected by
the harness). Mix of Chinese and English to exercise the CJK bigram
segmentation and the OR-joined FTS retrieval path.

CASES: each is {query, expect}.
    expect = substring that must appear in a returned memory's content
             (None means a NEGATIVE case: nothing relevant should match —
             used to catch false positives from over-eager OR matching).

Keep cases deterministic and content substrings unique enough to score
unambiguously. When you change retrieval logic, re-run with --baseline to
re-anchor, but only after manually confirming the new numbers are correct.
"""

# ── Seed corpus ────────────────────────────────────────────────────
SEED_MEMORIES = [
    # Chinese — preferences / decisions / constraints
    {"kind": "preference", "content": "用户偏好用 pnpm 而非 npm，所有新项目默认使用 pnpm 作为包管理器",
     "summary": "用户用哪个包管理器", "importance": 0.9, "reusability": 0.9, "payload": {"tool": "pnpm"}},
    {"kind": "decision", "content": "项目数据统一存储在 SQLite，知识图谱和长期记忆都走同一个库",
     "summary": "项目数据存在什么数据库", "importance": 0.8, "payload": {"storage": "sqlite"}},
    {"kind": "constraint", "content": "构建产物必须是单一静态二进制，不允许引入运行时外部依赖",
     "summary": "构建产物的形态约束", "importance": 0.8, "payload": {"rule": "static-binary"}},
    {"kind": "decision", "content": "认证服务采用 JWT 令牌，使用 RS256 算法对所有接口签名",
     "summary": "认证用什么令牌和签名算法", "importance": 0.8, "payload": {"auth": "jwt-rs256"}},
    {"kind": "lesson", "content": "中文检索失效的根因是分词器把整段中文当成一个词，需要切成字符二元组",
     "summary": "中文检索为什么失效", "importance": 0.7, "payload": {"topic": "tokenizer"}},
    # English — preferences / decisions
    {"kind": "preference", "content": "Prefer ripgrep over grep for code search across the repository",
     "summary": "preferred code search tool", "importance": 0.7, "payload": {"tool": "ripgrep"}},
    {"kind": "decision", "content": "The HTTP server binds to localhost only and requires no authentication in dev mode",
     "summary": "how the dev HTTP server is exposed", "importance": 0.6, "payload": {"net": "localhost"}},
    {"kind": "constraint", "content": "All database writes must go through the WAL journal mode for durability",
     "summary": "database write durability rule", "importance": 0.7, "payload": {"db": "wal"}},
    # A second pnpm-adjacent memory to test ranking (less specific)
    {"kind": "event", "content": "团队讨论了包管理器选型，最终倾向 pnpm 的硬链接节省磁盘",
     "summary": "团队包管理器选型讨论", "importance": 0.5, "payload": {"topic": "pkgmgr"}},
]

# ── Query cases ────────────────────────────────────────────────────
CASES = [
    # --- Chinese recall (the core fix) ---
    {"query": "包管理器", "expect": "pnpm 作为包管理器"},
    {"query": "pnpm", "expect": "pnpm 作为包管理器"},
    {"query": "数据存储", "expect": "统一存储在 SQLite"},
    {"query": "静态二进制", "expect": "单一静态二进制"},
    {"query": "认证令牌", "expect": "JWT 令牌"},
    {"query": "签名算法", "expect": "RS256"},
    {"query": "分词器", "expect": "分词器把整段中文"},
    {"query": "字符二元组", "expect": "切成字符二元组"},
    # --- English recall ---
    {"query": "ripgrep", "expect": "Prefer ripgrep over grep"},
    {"query": "code search", "expect": "ripgrep over grep for code search"},
    {"query": "WAL journal", "expect": "WAL journal mode"},
    {"query": "authentication localhost", "expect": "binds to localhost"},
    # --- Mixed / cross-lingual token ---
    {"query": "JWT RS256", "expect": "RS256"},
    {"query": "SQLite 知识图谱", "expect": "知识图谱和长期记忆"},
    # --- Semantic recall (query shares few/no words with target; only the
    #     768-d embedding can bridge meaning -> spelling. These MISS under a
    #     pure-FTS / bag-of-words path and are the real measure of the
    #     256->768 nomic upgrade. Keep `expect` a substring of the target
    #     content but choose query wording that avoids those exact tokens.) ---
    {"query": "怎么给接口做鉴权", "expect": "JWT 令牌"},
    {"query": "登录态用什么方式校验", "expect": "RS256"},
    {"query": "程序最终如何打包交付", "expect": "单一静态二进制"},
    {"query": "节省磁盘空间的依赖安装方式", "expect": "pnpm 的硬链接节省磁盘"},
    {"query": "为什么中文搜索结果总是空的", "expect": "分词器把整段中文"},
    {"query": "a fast tool to search text inside source files", "expect": "ripgrep over grep"},
    {"query": "can the dev service be reached from another machine", "expect": "binds to localhost"},
    {"query": "how are writes kept safe across a crash", "expect": "WAL journal mode"},
    {"query": "where does all the project data live", "expect": "统一存储在 SQLite"},
    # --- Negative cases (must NOT produce a relevant hit) ---
    {"query": "区块链智能合约", "expect": None},
    {"query": "kubernetes deployment yaml", "expect": None},
    {"query": "用户的生日和家庭住址", "expect": None},
]
