# Semantic Memory Global

Auditable local memory for Codex Desktop, with global recall across workspaces.

面向 Windows x64 的本地记忆 MCP：保存可复用偏好、经验和项目决策，按任务召回，并保留来源、使用归因与生命周期证据。全局记忆跨工作区共享，项目记忆保留项目范围。

**当前发布：R5 / `1.1.0-rc.1+codex.20261005112500`。** 原生核心版本 `v1.1.0-rc.1`，兼容修订 `global-scope-local-r2`。完整版本和哈希以 [RELEASE.json](RELEASE.json) 为准。

**主分支另包含 2026-10-08/09 的未发布维护修复。** 已安装 R5 的用户可从仓库的 `packaging/windows` 运行 `Repair Persistent Codex Memory.cmd`：同步 Codex/MMCAPI 保存的 MCP 注册、清理仅供 Codex 使用的旧 `type` 字段，并启用只读项目身份适配。此可选入口要求 Python 3.11+；已发布的 R5 ZIP 和原生安装依赖未改变。使用与回滚见安装文档的“持久注册与项目身份修复”。

- [下载 Windows 完整发布包](https://github.com/AlbertYm/semantic-memory-global/releases/latest)
- [安装、升级、验证和卸载](docs/INSTALL.zh-CN.md)
- [详细功能和日常使用](docs/FUNCTIONS.zh-CN.md)
- [实现、源码和构建边界](docs/ARCHITECTURE.zh-CN.md)
- [发布修复与验证范围](docs/RELEASE-NOTES.zh-CN.md)

## 快速开始

1. 从 Releases 下载 `SemanticMemory-1.1.0-rc.1-codex.20261005112500-windows-x64-r5.zip` 和对应 SHA256 文件，解压整个 ZIP。
2. 保存工作，完全退出 Codex Desktop；若 Memory Manager 开着，也退出。
3. 双击 `Install Semantic Memory.cmd`。使用当前普通用户，无需管理员权限。
4. 成功后重新打开 Codex；如果出现插件信任提示，核对插件名称和版本后确认。
5. 新建聊天，说明一项值得长期保存的偏好，例如“记住：解释步骤时默认用中文，这条偏好跨项目生效”。后续在另一个工作区查询这项偏好，验证实际记录和召回。

没有相关历史时，召回为空是正常情况。记录成功、候选可召回、知识晋升、使用反馈是不同状态，不能把它们混为一谈。

## 包含什么

| 组件 | 用途 |
| --- | --- |
| 原生 MCP | 事件记录、记忆召回、任务生命周期、证据和反馈，以及代码图查询；工具表分页，19 项是首个分页的历史数量 |
| 可选项目身份适配 | 新增 `memory_resolve_project`，解析已登记工作区/任务的 UUID；唯一匹配的旧目录名可兼容，原生内容与作用域检查保留 |
| Codex Personal Plugin | `UserPromptSubmit` 召回、`PostToolUse` hash-only 证据、受控 `Stop` 收尾、记忆 Skill |
| Memory Manager | 原生本地管理入口；完整 GUI/DPI 行为需在目标电脑自行验收 |
| Windows 安装器 | 用户级安装、已有受管理版本升级、配置修复、文件校验与保留数据卸载 |
| 源码与维护工具 | 固定上游提交的 C 源码、作用域修复、精确基线补丁、公开可读安装脚本与打包脚本 |

## 默认行为

- 全局数据位于 `%LOCALAPPDATA%\SemanticMemory\data`，与解压目录、原开发盘符无关。
- 当前工作区路径或 UUID 是来源上下文，不把全局偏好绑定到某个开发项目。
- 使用本地 static embedding；基本记忆功能不需要外部 AI、API key 或付费模型调用。
- 自动维护默认关闭；检索与普通事件不自动执行晋升、演化、物理删除或数据库恢复。
- 不安装服务、开机启动项或修改全局 PATH。公开包不包含发布者的记忆库、Codex 配置、凭据或聊天历史。

## 开源与可复现范围

本仓库采用 MIT，保留原作者及第三方声明。核心源码来自 [AlbertYm/neuroplastic-memory-mcp](https://github.com/AlbertYm/neuroplastic-memory-mcp)，固定提交见 RELEASE.json，并同步作用域修复。

**本次可运行内核采用已验收的精确基线二进制兼容修订，没有宣称从全部源码重新编译得到逐字节相同的 EXE。** C 源码修复、二进制改动区域及校验过程公开；缺少原构建工具链的逐字节可复现验证。使用者可审阅源码，维护者可按上游构建流程重建。详见架构文档。

## 支持范围

Windows 10/11 x64、Windows PowerShell 5.1、已安装的 Codex Desktop。日常安装和运行不需要 Python、Node.js 或编译器；源码打包工具使用 Python 3.11+。原生 EXE 未增加代码签名，下载后可核对 SHA256。

当前核心的 InstallRoot 必须为 ASCII 路径；中文用户名请按安装文档选择可写的 ASCII 根。中文工作区可用。安装器会在写入前拒绝不支持的根目录。

自定义安装根还需按安装文档设置当前用户的 `SEMANTIC_MEMORY_HOME`，让插件 Hook 引用同一运行时；设置后重新登录 Windows 再打开 Codex。默认安装根不需要该设置。

本机已验证当前聊天的全局写入、两个实际工作区召回、反馈重放和数据库完整性。发布包的自动隔离验收与目标电脑 GUI/信任/重启验收分别记录，不把本机结果当成另一台电脑的验收。

问题反馈请附版本、错误代码和脱敏后的验证结果。不要上传数据库、API key、token、Cookie、完整配置或完整聊天记录。
