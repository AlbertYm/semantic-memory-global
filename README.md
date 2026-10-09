# Semantic Memory Global

Auditable local memory for Codex Desktop, with global recall across workspaces.

面向 Windows x64 的本地记忆 MCP：保存可复用偏好、经验和项目决策，按任务召回，并保留来源、使用归因与生命周期证据。全局记忆跨工作区共享，项目记忆保留项目范围。

**当前发布：R7 / `1.1.0-rc.1+codex.20261009152000`。** 原生核心、Hook 和 Manager 已从源码重建，新增基于结果的自动学习。版本、哈希与兼容组件以 [RELEASE.json](RELEASE.json) 为准。

**R7 根据可验证的使用结果调整选择与关联。** 已完成任务中的真实候选归因结合命令退出结果或 Manager 用户确认，产生正负信用；信用影响相关候选排序与共同使用关联，受控晋升、重复整合、衰减与归档保留恢复路径。R6 的持久注册与项目身份修复继续保留。

- [下载 Windows 完整发布包](https://github.com/AlbertYm/semantic-memory-global/releases/latest)
- [安装、升级、验证和卸载](docs/INSTALL.zh-CN.md)
- [详细功能和日常使用](docs/FUNCTIONS.zh-CN.md)
- [实现、源码和构建边界](docs/ARCHITECTURE.zh-CN.md)
- [发布修复与验证范围](docs/RELEASE-NOTES.zh-CN.md)

## 快速开始

1. 从 Releases 下载 `SemanticMemory-1.1.0-rc.1-codex.20261009152000-windows-x64-r7.zip` 和对应 SHA256 文件，解压整个 ZIP。
2. 保存工作，完全退出 Codex Desktop 和 Memory Manager。已有 R6 持久修复时，先在原包运行 `Rollback Persistent Codex Memory.cmd`，保留数据再升级。
3. 确认已安装 Python 3.11+ 且 `python.exe` 可用，双击 `Install Semantic Memory.cmd`。使用当前普通用户，无需管理员权限。
4. 成功后重新打开 Codex；如果出现插件信任提示，核对插件名称和版本后确认。
5. 新建聊天，说明一项值得长期保存的偏好，例如“记住：解释步骤时默认用中文，这条偏好跨项目生效”。后续在另一个工作区查询这项偏好，验证实际记录和召回。

没有相关历史时，召回为空是正常情况。记录成功、候选可召回、知识晋升、使用反馈是不同状态，不能把它们混为一谈。

## 包含什么

| 组件 | 用途 |
| --- | --- |
| 原生 MCP | 事件记录、记忆召回、任务生命周期、证据、反馈和自动学习，以及代码图查询；完整分页共 55 项，含 4 项旧控制器兼容工具 |
| 项目身份适配 | 新增 `memory_resolve_project`，解析已登记工作区/任务的 UUID；唯一匹配的旧目录名可兼容，原生内容与作用域检查保留 |
| Codex Personal Plugin | `UserPromptSubmit` 召回、`PostToolUse` hash-only 证据、受控 `Stop` 收尾、严格结果收据、记忆 Skill |
| Memory Manager | 原生本地管理入口，新增“自动学习”：信用、关联、暂停/继续、归档恢复、用户确认；目标电脑 GUI/DPI 需自行验收 |
| Windows 安装器 | 用户级安装、已有受管理版本升级、配置修复、文件校验与保留数据卸载 |
| 源码与维护工具 | 固定上游提交的 C 源码、作用域修复、Windows 云端构建、公开可读安装脚本与打包脚本 |

## 默认行为

- 全局数据位于 `%LOCALAPPDATA%\SemanticMemory\data`，与解压目录、原开发盘符无关。
- 当前工作区路径或 UUID 是来源上下文，不把全局偏好绑定到某个开发项目。
- 使用本地 static embedding；基本记忆功能不需要外部 AI、API key 或付费模型调用。
- 新自动学习默认启用，由检索、反馈和任务完成触发有界批次；旧物理清理与独立授权演化仍关闭，不自动物理删除或恢复数据库。
- 不安装服务、开机启动项或修改全局 PATH。公开包不包含发布者的记忆库、Codex 配置、凭据或聊天历史。

## 开源与可复现范围

本仓库采用 MIT，保留原作者及第三方声明。核心源码来自 [AlbertYm/neuroplastic-memory-mcp](https://github.com/AlbertYm/neuroplastic-memory-mcp)，固定提交见 RELEASE.json，并同步作用域修复。

主运行时来自本仓库 Windows GitHub Actions 完整源码构建。上游固定源码缺少预编译版本中的 4 项 `neuroplastic_*` 控制器实现，因此另附经 SHA256 校验的 R6 兼容二进制，仅在调用这 4 项时启动，保留其原有独立授权、lease 和 fencing 限制。这个兼容组件尚未完成源码闭合；整个包没有逐字节可复现构建证明。详见架构文档。

## 支持范围

Windows 10/11 x64、Windows PowerShell 5.1、已安装的 Codex Desktop。R7 安装和 MCP 适配运行要求 Python 3.11+（标准库）；不需要 Node.js、编译器或外部 AI。原生 EXE 未增加代码签名，下载后可核对 SHA256。

当前核心的 InstallRoot 必须为 ASCII 路径；中文用户名请按安装文档选择可写的 ASCII 根。中文工作区可用。安装器会在写入前拒绝不支持的根目录。

自定义安装根还需按安装文档设置当前用户的 `SEMANTIC_MEMORY_HOME`，让插件 Hook 引用同一运行时；设置后重新登录 Windows 再打开 Codex。默认安装根不需要该设置。

R7 已验证隔离环境中的原生召回、实际命令结果、任务完成和下一次排序变化，并执行 Windows 源码回归及完整 ZIP 安装/升级/恢复检查。它证明机制工作；尚不证明长期用户任务的质量提升。当前已运行的 Codex 不会自动重载，重启后的真实任务链、插件信任和 Manager 登录态/DPI 仍需验收。

问题反馈请附版本、错误代码和脱敏后的验证结果。不要上传数据库、API key、token、Cookie、完整配置或完整聊天记录。
