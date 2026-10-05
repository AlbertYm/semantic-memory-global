# 功能与用法

## 记忆的范围

`global` 保存当前用户的跨项目偏好、约束与经验；不同工作区可召回。`project` 保存某个工程的理由、约束与决策。即使是 global，工具仍需要实际工作区路径或已知项目 UUID 作为来源上下文。项目来源不是独立数据仓库绑定，也不是旧开发盘符白名单。

只记录简短、稳定、有证据且未来有用的内容。不要存密码、token、Cookie、原始私人素材或完整对话。

## 日常示例

- “记住：教程默认中文，按钮名和文件名保留原文。这是全局偏好。”
- “这个项目使用 static embedding，以减少外部服务依赖；把原因记为项目决策。”
- “回忆我们之前对这个模块的决定，并给出来源。”
- “在另一个工作区确认能否召回我的全局偏好。”

是否实际记录以 events 收据为准；是否能召回以 retrieve 结果为准。Hook 自动产生任务证据不等于已经写入知识。候选用于可审计召回，知识晋升需要独立控制器和权限，不在普通记录的热路径中执行。

## 19 个当前专用工具

| 工具 | 功能与边界 |
| --- | --- |
| `events` | 记录一条事件/知识；支持偏好、经验、约束、事实、决策；代码决策分 Decision/Context/Rejected alternatives/Anchors 四段 |
| `memories_retrieve` | 按查询、entity_key、类型等召回，返回候选、来源、证据与检索会话标识 |
| `memory_task_begin` | 开始任务生命周期；Hook 已提供 task_id 时沿用，不重复建立 |
| `memory_task_status` | 读取任务最新状态及有界 hash-only evidence |
| `memory_task_complete` | 记录 completed/failed/cancelled，一次收尾并与 Hook 去重 |
| `memory_observe_injection` | 按真实检索会话记录注入审计，保存 hash 和元数据 |
| `memory_observe_usage` | 记录实际使用、忽略、拒绝、矛盾或不确定归因，不伪造使用 |
| `memory_feedback` | 追加带证据的反馈，精确重放和冲突保护；observe-only、shadow reward |
| `neuroplastic_capture` | 有界、项目范围的捕获提案；证据等级和确定性矩阵限制晋升 |
| `neuroplastic_evolution` | 对精确项目对象执行有界演化；模型自报告不触发持久演化 |
| `neuroplastic_maintenance` | 具备 lease、fencing、checkpoint 和策略复检的受控维护 |
| `neuroplastic_runtime_control` | 按独立授权与 CAS 控制 pause/resume |
| `memory_edge_lifecycle_migrate` | 受固定 fixture 或生产 canary 条件限制的边生命周期迁移 |
| `memory_edge_maintenance` | 导通率与 active/cold/archived 评估；off/shadow/dry-run 不写状态 |
| `memory_edge_restore` | 受相同授权边界限制的冷边预览/恢复 |
| `memory_reinforcement_replay` | 固定重放合同及受控有界任务预览/应用 |
| `index_repository` | 索引代码库并构建代码知识图；代码索引是按工程的功能 |
| `search_graph` | 查询函数、类、路由和变量等代码实体 |
| `query_graph` | 执行代码知识图中的结构化、多跳查询 |

准确参数以当前 `tools/list` schema 为准。完整参数参考见 [TOOL-SCHEMAS.json](TOOL-SCHEMAS.json)。高级控制器的存在不等于已经启用或已在用户电脑完成运行验收。

## 自动 Hook

`UserPromptSubmit` 建立/沿用任务并召回；`PostToolUse` 产生 hash-only 审计证据；`Stop` 保持有界收尾及 fail-open，避免把记忆步骤变成重复答复。代理遵循 Skill：仅归因实际使用的候选，错误或缺失工具时保留真实边界，不用普通 memory 工具替代专用协议。

## 默认没有做什么

不自动执行无限维护、物理删除或生产数据库恢复。没有外部模型调用就不宣称 AI 模型驱动结果。没有真实使用链就不补造正向 feedback。Scope 拒绝后不直接写数据库绕过控制器。
