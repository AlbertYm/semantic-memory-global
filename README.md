# Semantic Memory Global

Auditable local memory for Codex Desktop, with global recall across workspaces.

面向 Windows x64 的本地记忆 MCP：保存可复用偏好、经验和项目决策，按任务召回，并保留来源、使用归因与生命周期证据。全局记忆跨工作区共享，项目记忆保留项目范围。

**当前发布：R7.1 / `1.1.0-rc.1+codex.20261009170930`。** 原生核心、Hook 和 Manager 已从源码重建，新增基于结果的自动学习。版本、哈希与兼容组件以 [RELEASE.json](RELEASE.json) 为准。

**R7 根据可验证的使用结果调整选择与关联。** 已完成任务中的真实候选归因结合命令退出结果或 Manager 用户确认，产生正负信用；信用影响相关候选排序与共同使用关联，受控晋升、重复整合、衰减与归档保留恢复路径。R6 的持久注册与项目身份修复继续保留。R7.1 沿用 R7 原生运行时，修复早期项目适配器迁移和安装前的注册预检。

- [下载 Windows 完整发布包](https://github.com/AlbertYm/semantic-memory-global/releases/latest)
- [安装、升级、验证和卸载](docs/INSTALL.zh-CN.md)
- [详细功能和日常使用](docs/FUNCTIONS.zh-CN.md)
- [实现、源码和构建边界](docs/ARCHITECTURE.zh-CN.md)
- [发布修复与验证范围](docs/RELEASE-NOTES.zh-CN.md)

## 这个 MCP 的优势

目标是让有用的经验在后续任务中更容易被选中，同时保留来源、证据和恢复路径。它提供可以直接查询和管理的本地记忆服务，可与文件式记忆配合使用；不替代模型训练，也不宣称优于所有版本的 Codex 内置记忆。

| 能力 | 对实际使用的帮助 | 当前边界 |
| --- | --- | --- |
| 跨工作区召回与项目范围 | 换项目仍可召回自己的全局偏好，工程决策按项目过滤 | 全局仍需真实工作区/任务作为审计锚点；安装不共享不同用户的数据库 |
| 中文与结构化检索 | 结合本地向量、CJK 全文检索和类型/实体条件，查找历史经验与决定 | 默认 static embedding，不需要额外模型 API；召回质量需用自己的内容验证 |
| 基于真实结果的选择与关联 | 有用记忆获得有界信用，两条共同成功使用的记忆增加关联，影响后续相关候选排序 | 需已完成任务、真实召回和使用归因、可信结果收据；重复召回和模型自报告不增加成功信用 |
| 受控整合和可恢复归档 | 两个独立成功任务支持候选晋升；折叠精确重复，衰减低效用普通记忆，归档后可恢复 | 不自动语义合并或生成高层概念；保护偏好、约束、决策和高重要性记忆，不自动物理删除 |
| 可检查、可暂停 | MCP 与本地 Memory Manager 可查看信用、关联和归档，确认结果，暂停/恢复新学习 | 没有可信直接命令收据的客户端路径需要 Manager 人工确认；不是所有记录都自动获奖 |
| 本地数据与公开实现 | 记忆处理默认在本机，保留事件、使用和任务证据，可核对源码与构建记录 | Codex 本身仍使用自己的模型服务；旧兼容组件的源码闭合限制见下文 |

例如：一次修复实际召回并使用了两条经验，任务完成且结果证据被确认后，这两条经验及其共同使用关联可得到信用，下一次相关问题的候选排序会受影响。失败或矛盾证据可降低效用，更正与撤回会补偿信用。没有可靠结果证据时只保留审计记录，不伪造强化。

新自动学习默认启用，但由实际检索、反馈和任务完成触发有界处理，闲置时没有后台训练。已验证隔离任务中的信用与排序变化；长期自然任务是否更准确仍需统计，未证明持续成功率提升。完整条件、衰减和恢复规则见 [功能文档](docs/FUNCTIONS.zh-CN.md#r7-自动学习)。

## 快速开始

**普通使用者请下载 Release 的完整安装 ZIP。** `git clone`、`git pull`、Code → Download ZIP，以及 Release 自动生成的 `Source code (zip/tar.gz)` 都是源码，不包含完整安装 payload，不能代替安装包。源码构建方式见 [架构文档](docs/ARCHITECTURE.zh-CN.md#构建与打包)。

先准备 Windows 10/11 x64、已安装并启动过的 Codex Desktop，以及 **Python 3.11+**。**安装包不内置 Python，也不会自动下载它。** 安装 Python 时启用添加到 PATH，打开新终端运行 `python.exe --version` 确认；只有 `py` 启动器可用不满足默认双击入口要求。系统自带 Windows PowerShell 5.1 即可，无需管理员权限、Node.js、编译器或额外 AI API key。

1. 从 [R7.1 Release 的 Assets](https://github.com/AlbertYm/semantic-memory-global/releases/tag/v1.1.0-rc.1-codex.20261009170930-r7.1) 下载 `SemanticMemory-1.1.0-rc.1-codex.20261009170930-windows-x64-r7.1.zip` 和对应 SHA256 文件，完整解压；不要在压缩包预览内运行，也不要只复制 EXE。
2. 保存工作，完全退出 Codex Desktop 和 Memory Manager。正式 R6/R7 持久修复仍处于启用状态时，先在原包运行 `Rollback Persistent Codex Memory.cmd`，保留数据再升级。
3. 确认已安装 Python 3.11+ 且 `python.exe` 可用，双击 `Install Semantic Memory.cmd`。使用当前普通用户，无需管理员权限。
4. 成功后重新打开 Codex；如果出现插件信任提示，核对插件名称和版本后确认。
5. 新建聊天，说明一项值得长期保存的偏好，例如“记住：解释步骤时默认用中文，这条偏好跨项目生效”。后续在另一个工作区查询这项偏好，验证实际记录和召回。

安装器自动安装运行时、个人插件、MCP 注册和项目身份适配并校验；正常安装无需手动编辑配置。可用 `Verify Semantic Memory.cmd` 检查包与注册，`Open Memory Manager.cmd` 打开管理界面。自动检查通过后仍需在重启后的新聊天确认工具绑定。

**中文 Windows 用户名或自定义安装目录：** 安装根须为 ASCII 字符，默认双击入口不会自动改选目录。请按 [自定义安装步骤](docs/INSTALL.zh-CN.md#升级已有版本) 指定可写的英文目录，设置当前用户 `SEMANTIC_MEMORY_HOME`，注销并重新登录 Windows 后再打开 Codex。中文工作区可以正常使用。

早期 `semantic_memory_project_adapter_20261009.py` 且没有正式持久事务的用户，可经校验直接安装 R7.1；已启用正式 R6/R7 持久修复的用户先用原包回滚。详细判断与恢复路径见 [安装文档](docs/INSTALL.zh-CN.md)。每位使用者建立自己的本地记忆库，发布包不导入发布者记忆。没有相关历史时，召回为空是正常情况。记录成功、候选可召回、知识晋升、使用反馈是不同状态。

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

本仓库是派生与集成工作，采用 MIT，保留原作者及第三方声明。源码继承链从早到晚如下；这是源码来源说明，不代表 GitHub 自动显示的 fork 关系，也不代表所有上游功能在当前包中默认启用。

| 层次 | 仓库 | 贡献与关系 |
| --- | --- | --- |
| 原始代码引擎 | [DeusData/codebase-memory-mcp](https://github.com/DeusData/codebase-memory-mcp) | 原生代码索引、代码知识图、结构查询与 MCP 基础 |
| 语义长期记忆 | [ZR113146/semantic-memory-mcp](https://github.com/ZR113146/semantic-memory-mcp) | 在代码引擎上增加 ADR 长期记忆与中文 CJK 全文检索 |
| 直接上游：Neuroplastic Memory | [AlbertYm/neuroplastic-memory-mcp](https://github.com/AlbertYm/neuroplastic-memory-mcp) | 增加神经可塑性机制、路径审计、结果反馈和受控演化；本仓库 `core/` 的直接来源 |
| 当前 Windows/Codex 分发 | [AlbertYm/semantic-memory-global](https://github.com/AlbertYm/semantic-memory-global) | 工作区/global 适配与持久注册、Windows 安装恢复，以及 R7 基于验证结果的选择、关联和可恢复维护 |

直接上游固定提交为 [`0825fca6d25b63dfe865e5834a57878a11f8efef`](https://github.com/AlbertYm/neuroplastic-memory-mcp/tree/0825fca6d25b63dfe865e5834a57878a11f8efef)，由 [RELEASE.json](RELEASE.json) 记录。前三层的归属依据是该提交的 [AUTHORS.md](https://github.com/AlbertYm/neuroplastic-memory-mcp/blob/0825fca6d25b63dfe865e5834a57878a11f8efef/AUTHORS.md)，本地副本见 [core/AUTHORS.md](core/AUTHORS.md)；许可证与依赖声明见 [LICENSE](LICENSE) 和 [THIRD_PARTY.md](THIRD_PARTY.md)。此处标注实际源码继承，未将其他未核实的设计参考仓库列为源码上游。

主运行时来自本仓库 Windows GitHub Actions 完整源码构建。上游固定源码缺少预编译版本中的 4 项 `neuroplastic_*` 控制器实现，因此另附经 SHA256 校验的 R6 兼容二进制，仅在调用这 4 项时启动，保留其原有独立授权、lease 和 fencing 限制。这个兼容组件尚未完成源码闭合；整个包没有逐字节可复现构建证明。详见架构文档。

## 支持范围

Windows 10/11 x64、Windows PowerShell 5.1、已安装的 Codex Desktop。R7 安装和 MCP 适配运行要求 Python 3.11+（标准库）；不需要 Node.js、编译器或外部 AI。原生 EXE 未增加代码签名，下载后可核对 SHA256。

当前核心的 InstallRoot 必须为 ASCII 路径；中文用户名请按安装文档选择可写的 ASCII 根。中文工作区可用。安装器会在写入前拒绝不支持的根目录。

自定义安装根还需按安装文档设置当前用户的 `SEMANTIC_MEMORY_HOME`，让插件 Hook 引用同一运行时；设置后重新登录 Windows 再打开 Codex。默认安装根不需要该设置。

R7 已验证隔离环境中的原生召回、实际命令结果、任务完成和下一次排序变化，并执行 Windows 源码回归及完整 ZIP 安装/升级/恢复检查。它证明机制工作；尚不证明长期用户任务的质量提升。当前已运行的 Codex 不会自动重载，重启后的真实任务链、插件信任和 Manager 登录态/DPI 仍需验收。

问题反馈请附版本、错误代码和脱敏后的验证结果。不要上传数据库、API key、token、Cookie、完整配置或完整聊天记录。
