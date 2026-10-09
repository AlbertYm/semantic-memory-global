# 安装、升级与恢复

## 前提与依赖

- Windows 10/11 x64，当前普通用户，可写自己的 LocalAppData 和 `.codex`。
- 已安装并启动过 Codex Desktop；安装前保存工作并完全退出 Codex 与 Memory Manager。
- 使用系统 Windows PowerShell 5.1。完整包包含原生运行时。R7 安装及项目身份适配要求 Python 3.11+，`python.exe` 须可用；自定义解释器可向 `Install-Bundle.ps1` 传入 `-PythonExe`。不需要 Node.js、编译器、外部 AI 或 API key。
- 原生 r2 的安装根路径须使用 ASCII 字符，可有空格。中文工作区可正常记录和召回；中文 Windows 用户名使默认 LocalAppData 路径非 ASCII 时，预检会停止且不写文件，请选择自己可写的 ASCII InstallRoot。
- 下载需要网络；安装不下载第三方依赖。MCP 的基本记忆与 static embedding 在本地执行。Codex 自己的模型服务网络要求由 Codex 决定。

## 全新安装

1. 从 Releases 下载 ZIP 与 `.sha256`。可在 PowerShell 用 `Get-FileHash -Algorithm SHA256 <ZIP路径>` 核对哈希。
2. 完整解压；不要从压缩包预览中直接运行，也不要只复制某个 EXE。
3. 完全退出 Codex 和 Manager，双击 `Install Semantic Memory.cmd`。
4. 安装器先检查 Python 和路径，再验证 payload、安装用户级原生程序、合并个人插件、启用项目身份适配并同步持久 MCP 注册，最后执行自动校验。
5. 看到安装成功后重新打开 Codex，在新聊天验证工具、记录与召回。

| 位置 | 内容 |
| --- | --- |
| `%LOCALAPPDATA%\SemanticMemory\bin` | 日常稳定启动入口 |
| `%LOCALAPPDATA%\SemanticMemory\app\versions` | 带 manifest 的版本 payload |
| `%LOCALAPPDATA%\SemanticMemory\data` | 当前用户的共享记忆、图和审计数据 |
| `%USERPROFILE%\plugins\semantic-memory` | 个人插件源 |
| `%USERPROFILE%\.codex\plugins\cache\personal\semantic-memory` | 插件版本缓存 |
| `%USERPROFILE%\.codex\config.toml` | 仅合并 Semantic Memory 的受管理 MCP 字段 |

插件安装会更新个人 marketplace 中对应条目，保留其他插件。未知非空安装目录、文件漂移、作用域拒绝和并发占用会停止；不要以删除目录来绕过保护。

## 升级已有版本

中文用户名或自定义安装位置可在退出应用后运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Install-Bundle.ps1 -InstallRoot D:\SemanticMemory
```

按自己的磁盘选择一个可写的 ASCII 路径。此时数据保存在所选根目录的 data，插件和配置仍属于当前用户；不要以管理员身份安装到其他人的用户目录。

自定义根目录还需要让插件 Hook 找到同一个运行时。先记录用户环境变量 `SEMANTIC_MEMORY_HOME` 的原值，再将它设为实际安装根；默认根目录安装不需要设置：

```powershell
[Environment]::GetEnvironmentVariable('SEMANTIC_MEMORY_HOME','User')
[Environment]::SetEnvironmentVariable('SEMANTIC_MEMORY_HOME','D:\SemanticMemory','User')
$env:SEMANTIC_MEMORY_HOME = 'D:\SemanticMemory'
```

这是当前用户的持久设置，不是整机共享设置。修改后注销并重新登录 Windows，再打开 Codex，保证桌面进程继承新值；当前 PowerShell 中的 `$env:` 赋值只立即影响该终端的子进程。安装器不会自动写入或恢复这个用户环境变量。彻底卸载自定义安装后恢复先前值；原值为空时可用 `[Environment]::SetEnvironmentVariable('SEMANTIC_MEMORY_HOME',$null,'User')` 清除。

包内 CMD 入口使用默认 `%LOCALAPPDATA%\SemanticMemory`。采用自定义根时，在解压包目录的 PowerShell 中使用以下对应入口，并把 `D:\SemanticMemory` 换成实际路径：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Verify-Bundle.ps1 -InstallRoot D:\SemanticMemory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Repair-Bundle.ps1 -InstallRoot D:\SemanticMemory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\SemanticMemory\bin\semantic-memory-launcher.ps1 -Mode manager -InstallRoot D:\SemanticMemory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Uninstall-Bundle.ps1 -InstallRoot D:\SemanticMemory
```

这些命令分别用于验证、配置修复、打开 Manager 和卸载；不要一次全部执行。安装、修复和卸载前须正常退出相关应用。已安装的 MCP 注册会引用所选根目录，日常在 Codex 使用时不需要每次运行这些命令。

先备份自己的数据，再正常退出相关应用。若存在已启用的正式持久修复（存在 codex-memory-repair-state.json，包括 R6 或 R7），先使用原修复包的 `Rollback Persistent Codex Memory.cmd` 回滚；成功后运行新包的安装入口。未启用适配的 R5/R6 可直接运行 R7 安装入口。安装器根据包版本与实际 payload 判断安装、升级或配置修复；升级保留记忆，不导入发布者数据库。旧版本与配置/插件事务备份用于恢复。

R7.1 补充早期 `semantic_memory_project_adapter_20261009.py` 的直接迁移。没有正式持久事务时，不需要先运行 R6 回滚；直接运行 R7.1 安装入口。安装器核对旧适配器 SHA256、精确参数/数据根、原私有事务中的注册和已验收状态。没有证明、文件漂移或未知注册仍拒绝。安装前 Check 对配置及 MMCAPI 做只读预检，避免先升级运行时再发现未知注册。新事务从当前配置建立备份，不回填早期完整配置，后来的模型与其他设置保留。

R7.1 的回滚入口在没有正式事务时返回 `NOTHING_TO_ROLLBACK / NO_PERSISTENT_REPAIR_TRANSACTION`，不改注册、不创建事务；这不表示早期适配器被卸载。

已经安装相同包时重新运行入口进入配置 Repair。`Repair Semantic Memory MCP.cmd` 修复受管理注册；它不能替代缺失的新运行时升级。

## 持久注册与项目身份修复（R7）

R7 完整 ZIP 已包含修复源码、入口和原生 payload；安装成功后无需再单独执行修复。下列独立入口用于已有受管理运行时的维护，或重新启用已回滚的适配。

1. 安装 Python 3.11 或更高版本，确保 `python.exe` 可用。R7 安装和维护入口使用 Python 标准库，不下载依赖。缺少 Python 会在安装写入前停止。
2. 保存工作，正常退出 Codex 和 Memory Manager。在 R7 解压目录双击 `Repair Persistent Codex Memory.cmd`。
3. 修复先核对已安装核心的哈希，创建仅当前用户和 SYSTEM 可访问的事务备份，然后更新当前 Codex 配置。如果当前用户存在 `.mmcapi/mmcapi.db`，同步 Codex 专用登记、公共配置、全部 Codex 供应商模板及已有代理恢复快照；模型、服务地址、认证字段及其他应用的登记保留。共享给其他应用的旧传输字段会保护性拒绝，要求人工核对。
4. 双击 `Verify Persistent Codex Memory.cmd` 查看持久来源与适配文件验证。重新打开 Codex，在新聊天调用 `memory_resolve_project`，传入实际绝对工作目录或 Hook 提供的当前 `task_id`，将返回的 `project_uuid` 用于记忆调用。`scope="global"` 仍要求有效项目审计锚点。

已索引代码项目与已登记记忆工作区不同；`list_projects` 为空不表示没有工作区。适配只将唯一匹配的已登记目录名或兼容别名转换为 UUID；未知名称、歧义、路径穿越和非法 scope 不会获得授权。原生核心继续执行秘密和提示注入检查。R7 主核心与 Hook 已重建，新自动学习默认启用，旧独立授权控制器保持原限制。

自定义根、独立 Codex 配置或显式配置管理器数据库可使用：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Repair-Codex-Memory.ps1 -Mode Apply -InstallRoot D:\SemanticMemory -CodexHome D:\CodexProfile -MmcapiDatabase D:\ConfigManager\mmcapi.db -PythonExe D:\Python\python.exe
```

没有指定 `-MmcapiDatabase` 且使用独立 `CodexHome` 时，不会自动操作真实用户的 MMCAPI 数据库；仅修复所选 Codex 配置。日常模型切换和登录刷新不影响验证，真正的适配/登记漂移会返回错误。重复运行已验证的修复返回 `REPLAYED_ZERO_WRITE`。

回滚用 `Rollback Persistent Codex Memory.cmd`，或在同一组目录参数下运行 `Repair-Codex-Memory.ps1 -Mode Rollback`。需要选择历史事务时传 `-TransactionPath <备份事务目录>`。回滚逐项核对当前配置和来源行，发现后续变更会停止；它不会用整库备份覆盖新记忆。备份可能含使用者的配置和凭据，只保存在该电脑，不上传仓库。

启用后，仓库中的 `Verify-Bundle.ps1` 会验证适配注册和实际协议入口，`Repair-Bundle.ps1` 保留适配。升级到其他原生包或卸载前须先回滚持久修复，防止旧安装器覆盖新入口。Python 的注册路径必须继续可用；升级或移除 Python 后应核对适配注册。已经运行的 MCP 进程不会自动重载，正常退出并重启 Codex 后验收工具表。

源代码隔离检查覆盖配置同步、未知登记拒绝、并发保护、失败回滚、其他应用保护、目录解析、真实原生写入、工具分页和管道 EOF。它不代表另一台电脑的 Codex 信任、重启绑定或 GUI 验收。

## R7 包验证

双击 `Verify Semantic Memory.cmd`。自动验证覆盖版本链、完整文件 SHA256、插件一致性、受管理配置及原生 MCP initialize/tools/list。自动 PASS 不证明当前 Codex 聊天已经重新加载。

重启后的真实验收：在新聊天确认专用工具可用，写一条明确的非敏感全局偏好，再从另一工作区召回。查看 Hook 的召回、hash-only evidence 和正常收尾；没有相关候选时不应伪造引用。最终回答可见性、插件信任、长内容、缩放和 DPI 需要目标电脑实际检查。

## 升级后验证自动学习

新聊天中确认 `memory_learning_status` 与 `memory_learning_control` 可用，使用 resolver 返回的项目 UUID 读取状态。新控制器应 enabled；全新空库在首次检索或任务完成前可能 ready=false，首次有界处理后变为 ready=true；旧维护和旧演化关闭是独立状态。打开 Manager 的“自动学习”，检查信用、关联与暂停/继续入口。

选择一条实际召回并使用的记忆，执行与任务有关的真实检查，按插件协议关联候选、使用、证据并完成任务，再检查信用及下一次排序。没有可信命令结果收据时，在 Manager 审核并确认待确认结果；不能自行把模型自报告改成用户确认。两项独立成功任务才满足候选晋升条件。无候选或无已确认结果时信用不变是正常情况。

通过“自动学习”暂停只停用新控制器，记忆仍可记录/召回；恢复归档需版本校验。回退整个 R7 时按下文回滚持久注册、卸载升级事务并恢复上一运行时；新增学习表保留在数据中，旧组件不使用它们。先备份数据，不用旧数据库覆盖已有新记忆。

## Manager

双击 `Open Memory Manager.cmd`。如果未安装程序或完整性不符，先运行 Verify。不要把 Manager 展示的状态当成当前聊天工具绑定的证明。

## 卸载与恢复

正常退出相关应用，先运行 `Rollback Persistent Codex Memory.cmd`，成功后运行 `Uninstall Semantic Memory.cmd`。按本次事务恢复插件和配置，并保留记忆数据。安装后若配置有其他修改，保护性停止时根据事务备份合并；不要覆盖最新配置。

升级失败会尝试恢复受管理运行时、插件与配置，并保留错误；验证失败不要继续投入真实数据。历史 payload、安装事务和数据目录是恢复依据。本仓库的原始精确基线补丁脚本是离线生成工具，不直接改安装目录或数据库。

## 常见错误

| 提示 | 处理 |
| --- | --- |
| `PYTHON_311_REQUIRED` | 安装 Python 3.11+ 并确保 `python.exe` 可用，或指定 `-PythonExe`；预检未写入安装配置 |
| `PERSISTENT_REPAIR_ACTIVE` | 保存工作并退出相关应用，先回滚持久修复，再升级或卸载；不要删除状态文件 |
| `CODEX_DESKTOP_RUNNING` / `RUNTIME_BUSY` | 保存工作并正常退出 Codex、Manager，再运行；不强制杀进程 |
| `RED_PAYLOAD_INTEGRITY_FAILURE` | 完整解压官方本仓库 Release，核对 ZIP 与文件哈希；不要混用不同包的 manifest |
| `CONFIG_UNKNOWN_MANAGED_KEY` | 保留配置备份，核对已有 Semantic Memory 字段；不要删其他 MCP 或供应商配置 |
| `SECURITY_SCOPE_VIOLATION` | 核对 scope 与实际项目来源；global 用于跨项目偏好，受控操作仍有范围限制 |
| `INVALID_ARGUMENT_OR_ATTRIBUTION` | 核对真实 session/candidate/usage/evidence 关联和信任来源；model_self_report 对应 model |
| 只有普通 memory 工具 | 确认 Semantic Memory 注册、插件启用及重启后的专用工具表 |

反馈状态 `pending_confirmation` 与 `reward=0` 对模型自报告可以是正常协议结果，不能改成虚构的用户确认或外部验证。
