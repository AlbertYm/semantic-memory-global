# 代码审计报告 · semantic-memory-mcp

> 审计基准：《图谱式长期记忆框架 · 工程落地方案》§14 验收标准
> 审计时间：2026-06-25
> 覆盖提交：`7a7fadc`（最新）

---

## 总体结论

**核心路径整体合规，有 4 个需要修复或确认的问题，1 个设计层面的已知偏差需明确记录。**

---

## §14.1 范围合规

| 检查项 | 结论 | 说明 |
|---|---|---|
| 只实现 MVP 必做项，未提前做禁止项 | ✅ 通过 | 未见 L0–L5 物理分层、社区发现用于记忆边 |
| 存储是单一 SQLite | ✅ 通过 | `memory_event / memory_item / memory_edge / memory_vec / memory_fts` 均在同一 SQLite 文件 |
| 未在 LLM 噪声边上跑社区发现 | ✅ 通过 | Leiden/Louvain 算法只接入代码图（`CALLS` 边，`origin=rule`），不触及 `memory_edge` |

---

## §14.2 架构正确性

| 检查项 | 结论 | 说明 |
|---|---|---|
| 写入热路径无 embedding / LLM 调用 | ⚠️ **轻微违规** | `cbm_store_memory_append_candidate()` 在写入 `memory_item` 后立即调用 `memory_fts_upsert()`，在热路径内同步写入 FTS5 索引。框架要求"embedding/去重/抽边"移出热路径，FTS 属于索引构建，严格来说属于同类操作。该操作开销较低（纯 SQLite 写入，无网络），但与框架第 §6.1 步骤 5 的约束不符 |
| 去重 / 抽边 / 打分全在异步 Pass | ✅ 通过 | 向量构建（`memory_vec_upsert`）、四重匹配去重、抽边均在 `cbm_store_memory_consolidate()` 中执行，调用入口是手动触发的 `admin_consolidate` 工具 |
| 每条 item 有 hit_count / last_hit_at，召回时确实回写 | ✅ 通过 | `handle_memories_retrieve()` 在召回后调用 `cbm_store_memory_mark_hits()`，批量执行 `hit_count+1, last_hit_at=now` |

**⚠️ 问题 1 — 热路径含 FTS 同步写入**

```c
// src/store/store.c: cbm_store_memory_append_candidate()
int rc = sqlite3_step(stmt);       // 写 memory_item
...
(void)memory_fts_upsert(s, item, id);  // ← 热路径内同步写 FTS5
```

框架 §6.1 明确："不做 embedding"。FTS 索引构建虽不调 LLM，仍是"整理类"操作。建议将 FTS 写入移至 `consolidate` Pass 的 P1 阶段，或在 P2（embedding）同步执行，以彻底遵守热路径约束。若评估后认为开销可接受、可接受作为例外，需在 `MEMORY_MVP_DELIVERY.md` 中显式说明。

---

## §14.3 关键机制

| 检查项 | 结论 | 说明 |
|---|---|---|
| 去重是四重匹配，合并阈值偏保守 | ⚠️ **部分合规** | 详见问题 2 |
| 读时冲突裁决 | ✅ 通过 | `memory_compare_for_conflict()` 按 scope精确度 → confidence → recency → hit_count 加权，`memory_resolve_conflicts()` 在 retrieve 末尾执行 |
| 墓地层降权冷存，任何路径不硬删 | ✅ 通过 | 全库检索 `DELETE FROM memory_item` 零结果；被合并的 item 置 `status='archived'`，被证伪置 `status='retracted'`，均保留行 |
| 衰减可解释（三因子可见） | ✅ 通过 | `admin_decay` 用 `last_hit_at / confidence / reusability` 三个可见字段计算 `next_decay`，公式与框架 §11.2 一致 |

**⚠️ 问题 2 — 四重匹配缺少语义相似度维度；冲突判定阈值语义有歧义**

框架要求四维同时判断：实体 + 谓词 + 作用域 + **语义相似度**。当前实现的去重查询：

```c
// cbm_store_memory_consolidate(): dup_sql
"SELECT id,content FROM memory_item WHERE status='active' AND entity_key=?1 AND predicate=?2
 AND scope_project=?3 AND scope_user=?4 AND scope_task=?5 LIMIT 20;"
```

前三维（实体/谓词/作用域）通过结构化 SQL 过滤，第四维（语义）通过 `memory_content_similarity()` 计算。这是正确的，但存在两个子问题：

**子问题 2a — 语义相似度函数质量不足**

`memory_content_similarity()` 用 XXH3 哈希的字节级汉明距离模拟语义相似，不是真正的语义向量相似度。两段意思相同但用词不同的内容（"用户偏好暗色模式" vs "user prefers dark theme"）哈希距离极高，无法被合并。这在实践中会导致大量本应合并的记忆并存，与框架的"减少信噪"目标背离。

**子问题 2b — contradicts 阈值有歧义**

```c
if (sim < 0.55 && other_id) {
    memory_edge_insert(s, id, other_id, "contradicts", "rule", confidence);
}
```

相似度 < 0.55 就建 `contradicts` 边，但对于哈希距离来说，两段内容哈希差距大既可能是真实冲突，也可能只是表述不同。这会产生大量误报 `contradicts` 边，污染读时裁决。

建议：用 `memory_vec` 中已有的 int8 向量做 cosine 相似度作为第四维判断，替换掉哈希距离方案。阈值可参考框架"合并阈值调高"原则，建议 cosine ≥ 0.90 合并，cosine ≤ 0.30 才标 contradicts，中间区间并存。

---

## §14.4 证据与可追溯

| 检查项 | 结论 | 说明 |
|---|---|---|
| memory_event.payload 保留完整原文 | ✅ 通过 | Schema 中 `payload TEXT NOT NULL`，无截断逻辑 |
| memory_item 有 source_event_ids 证据链 | ✅ 通过 | 热路径写入时即填充，consolidate 阶段插入 `derived_from` 边 |
| memory_edge 有 origin 字段，可区分 rule/llm/post | ✅ 通过 | schema 含 `origin TEXT NOT NULL`，代码中使用 `"rule"/"post"/"merge"` 填充；llm 来源预留但当前 MVP 未使用（符合范围） |

---

## 额外发现（超出 §14 验收范围）

### 问题 3 — rejected 写入无审计日志落库

框架架构图（§3）中明确：`必不写 → 写审计日志`。当前实现对 `rejected` 决策只返回 JSON 响应，不入库：

```c
if (strcmp(policy_decision, "rejected") == 0) {
    // 返回 JSON 响应后直接 return，没有写 memory_event 或审计表
    return result;
}
```

审计日志缺失意味着无法回溯"哪些内容被拒绝了，为什么"，也无法用数据驱动调整写入策略（框架 §0 原则8）。建议对 rejected 写入一条 `type='audit.rejected'` 的 `memory_event` 记录，或单独建 `memory_audit` 表。

### 问题 4 — supersedes 字段语义反向

框架 §7 定义：`supersedes` 指向"本条 updates 的旧 item"，即新条目的 `supersedes = 旧条目 id`。当前合并逻辑：

```c
// 被合并的 candidate (id) 设置:
"UPDATE memory_item SET status='archived', supersedes=?1 WHERE id=?3;"
// 其中 ?1=merge_id (已有 active 条目), ?3=id (当前 candidate)
```

即被归档的 candidate 的 `supersedes` 字段指向 **存活的 active item**，而不是"被本条取代的旧条"。语义是反的：应该是 active item 的 `supersedes` 指向被它取代的旧 item，而不是被淘汰的 candidate 指向存活者。这不影响功能（active item 仍然保留），但会在追溯证据链时产生混淆，且与框架文档约定不一致。

### 已知偏差（已记录，无需修复）

**向量存储非 sqlite-vec**：框架 §5.4 指定 `sqlite-vec / vec0` 扩展，当前实现用普通表存储 int8 BLOBs + 自定义 `cbm_cosine_i8` 函数。`MEMORY_MVP_DELIVERY.md` 已作为显式交付决策记录。审计确认该文件存在且描述准确。

---

## 汇总

| 编号 | 问题 | 严重程度 | 建议动作 |
|---|---|---|---|
| 1 | FTS5 写入在热路径内同步执行 | 低（性能风险，无正确性问题） | 移至 consolidate P1，或在 DELIVERY.md 中显式豁免 |
| 2a | 语义相似度用哈希距离替代向量相似 | 中（去重漏报，信噪比下降） | 改用 int8 cosine 向量做第四维判断 |
| 2b | contradicts 边由哈希距离触发，误报率高 | 中（冲突裁决受污染） | 提高阈值或改用向量余弦 |
| 3 | rejected 写入无审计日志 | 低（可观测性缺失，不影响功能） | 写 `type='audit.rejected'` 事件记录 |
| 4 | supersedes 字段语义反向 | 低（不影响运行，影响追溯） | 修正赋值方向 |
| — | sqlite-vec 替换为 BLOB+cosine | 已知偏差，已记录 | 无需修复，已有 DELIVERY.md |

所有 MVP 禁止项均未出现；核心路径（写入、去重、冲突裁决、衰减、命中回写）逻辑完整且与框架一致。上述问题中无阻塞性缺陷，可选择在下一轮迭代中按优先级处理。

---

## 复审：修复结果（覆盖提交 `47ecc9f` 之后的修复轮）

> 复审时间：2026-06-25 · 测试：22/22 通过（新增 3 项语义行为测试）

| 编号 | 问题 | 状态 | 修复说明 |
|---|---|---|---|
| 1 | FTS5 写入在热路径内 | ✅ 已闭环（前一轮） | FTS 写入已移至 consolidate P1，热路径仅追加 event + candidate |
| 2a | 语义相似度无真实信号 | ✅ **本轮修复** | 根因不是阈值而是向量：`memory_hash_vec64`（整段做 64 个无关哈希，任意两串正交）替换为 `memory_feature_vec`（256 维带符号特征哈希词袋）。ASCII 按词、CJK 按字 unigram + 相邻 bigram，共享词/字落入重叠桶，cosine 携带真实词法语义信号 |
| 2b | contradicts 由噪声触发 | ✅ **本轮修复** | 同上。阈值 0.90/0.30 不变，但现在作用在有信号的向量上。在已被 SQL 锁定的同 (entity+predicate+scope) 桶内，低 cosine 是合理的"同主题异取值"信号 |
| 3 | rejected 无审计日志 | ✅ 已闭环（前一轮） | 写 `type='audit.rejected'` 事件 |
| 4 | supersedes 方向 | ⚠️ 保留为有意决策 | 团队已用测试 + 注释将"survivor.supersedes → 被归档 candidate"锁定。该路径是**合并**（同事实重复表达）而非框架 §7 的**版本替换**，合并场景下 candidate 是更新输入，指向关系自洽。证据链可追溯，不影响功能。若未来实现真正的版本替换（用户"以此为准"覆盖），需另走 `updates` 边并使新条 `supersedes` 指向旧条 |
| B | `memory_vec.dim` 元数据=768 实存 64 字节 | ✅ **本轮修复** | schema 默认与 upsert 均改为 `dim=MEMORY_VEC_DIM`(256)，与实际 blob 长度一致 |
| C | 命中不回落 decay | ✅ **本轮修复** | `mark_hits` 增加 `decay=MAX(0.0,decay-0.10)`，符合框架 §11.2"任何一次成功命中 → decay 衰减回落" |
| — | sqlite-vec 替换为 BLOB+cosine | 已知偏差，已记录 | `MEMORY_MVP_DELIVERY.md` 已更新描述新向量方案与不接 Nomic 的理由 |

**新增测试（证明语义路径真实生效，不再靠手注边）：**
- `memory_consolidate_merges_paraphrase` —— 近义改写在同桶内 cosine ≥ 0.90 → 真实合并（archived + similar_to 边）
- `memory_consolidate_detects_contradiction` —— 同桶异取值 cosine ≤ 0.30 → consolidate 自身产出 `contradicts`(`origin=rule`) 边，无任何手注
- `memory_mark_hits_relaxes_decay` —— 召回命中后 decay 实际回落

**复审结论：上一轮中等严重的 2a/2b 已真正闭环（替换根因向量，非表层换算法），B/C 一并修复，4 作为有意设计保留并记录。MVP 记忆路径从"结构对、语义空"转为语义可用。**
