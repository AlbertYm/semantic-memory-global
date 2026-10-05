# STAGE14-FEEDBACK-001：自然反馈验收待办

- 状态：`PENDING`
- 类型：非阻塞验收提醒
- 影响：不影响 MCP 加载、recall、正常任务执行或跨目录打包使用
- 待补内容：在一个真实自然任务中使用一条 recall 候选，并形成同一 task 的 `usage attribution -> feedback` 证据链
- 触发提醒：任务已产生 recall 候选和 evidence，但同一 task 没有合法 `memory_usage_attribution` 或 `feedback_event`
- 处理原则：只提醒，不阻塞；不猜测候选，不伪造 usage/feedback，不为填充证据写生产业务数据

完成后，使用者应保留同一 task、retrieval session、candidate、evidence、usage 和 feedback 的独立核对结果，再把本项从 `PENDING` 更新为已验证状态。
