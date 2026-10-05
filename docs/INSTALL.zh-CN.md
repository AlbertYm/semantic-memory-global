# 安装、升级与恢复

## 前提与依赖

- Windows 10/11 x64，当前普通用户，可写自己的 LocalAppData 和 `.codex`。
- 已安装并启动过 Codex Desktop；安装前保存工作并完全退出 Codex 与 Memory Manager。
- 使用系统 Windows PowerShell 5.1。完整包包含原生运行时，安装不要求 Python、Node.js、编译器、外部 AI 或 API key。
- 原生 r2 的安装根路径须使用 ASCII 字符，可有空格。中文工作区可正常记录和召回；中文 Windows 用户名使默认 LocalAppData 路径非 ASCII 时，预检会停止且不写文件，请选择自己可写的 ASCII InstallRoot。
- 下载需要网络；安装不下载第三方依赖。MCP 的基本记忆与 static embedding 在本地执行。Codex 自己的模型服务网络要求由 Codex 决定。

## 全新安装

1. 从 Releases 下载 ZIP 与 `.sha256`。可在 PowerShell 用 `Get-FileHash -Algorithm SHA256 <ZIP路径>` 核对哈希。
2. 完整解压；不要从压缩包预览中直接运行，也不要只复制某个 EXE。
3. 完全退出 Codex 和 Manager，双击 `Install Semantic Memory.cmd`。
4. 安装器验证 payload、安装用户级原生程序、合并个人插件及 MCP 注册，并执行自动校验。
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

包内 CMD 入口使用默认 `%LOCALAPPDATA%\SemanticMemory`。采用自定义根时，在解压包目录的 PowerShell 中使用以下对应入口，并把 `D:\SemanticMemory` 换成实际路径：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Verify-Bundle.ps1 -InstallRoot D:\SemanticMemory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Repair-Bundle.ps1 -InstallRoot D:\SemanticMemory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File D:\SemanticMemory\bin\semantic-memory-launcher.ps1 -Mode manager -InstallRoot D:\SemanticMemory
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Uninstall-Bundle.ps1 -InstallRoot D:\SemanticMemory
```

这些命令分别用于验证、配置修复、打开 Manager 和卸载；不要一次全部执行。安装、修复和卸载前须正常退出相关应用。已安装的 MCP 注册会引用所选根目录，日常在 Codex 使用时不需要每次运行这些命令。

先备份自己的数据，再正常退出相关应用，运行新包的安装入口。安装器根据包版本与实际 payload 判断安装、升级或配置修复；升级保留记忆，不导入发布者数据库。旧版本与配置/插件事务备份用于恢复。

已经安装相同包时重新运行入口进入配置 Repair。`Repair Semantic Memory MCP.cmd` 修复受管理注册；它不能替代缺失的新运行时升级。

## 验证

双击 `Verify Semantic Memory.cmd`。自动验证覆盖版本链、完整文件 SHA256、插件一致性、受管理配置及原生 MCP initialize/tools/list。自动 PASS 不证明当前 Codex 聊天已经重新加载。

重启后的真实验收：在新聊天确认专用工具可用，写一条明确的非敏感全局偏好，再从另一工作区召回。查看 Hook 的召回、hash-only evidence 和正常收尾；没有相关候选时不应伪造引用。最终回答可见性、插件信任、长内容、缩放和 DPI 需要目标电脑实际检查。

## Manager

双击 `Open Memory Manager.cmd`。如果未安装程序或完整性不符，先运行 Verify。不要把 Manager 展示的状态当成当前聊天工具绑定的证明。

## 卸载与恢复

正常退出相关应用，运行 `Uninstall Semantic Memory.cmd`。按本次事务恢复插件和配置，并保留记忆数据。安装后若配置有其他修改，保护性停止时根据事务备份合并；不要覆盖最新配置。

升级失败会尝试恢复受管理运行时、插件与配置，并保留错误；验证失败不要继续投入真实数据。历史 payload、安装事务和数据目录是恢复依据。本仓库的原始精确基线补丁脚本是离线生成工具，不直接改安装目录或数据库。

## 常见错误

| 提示 | 处理 |
| --- | --- |
| `CODEX_DESKTOP_RUNNING` / `RUNTIME_BUSY` | 保存工作并正常退出 Codex、Manager，再运行；不强制杀进程 |
| `RED_PAYLOAD_INTEGRITY_FAILURE` | 完整解压官方本仓库 Release，核对 ZIP 与文件哈希；不要混用不同包的 manifest |
| `CONFIG_UNKNOWN_MANAGED_KEY` | 保留配置备份，核对已有 Semantic Memory 字段；不要删其他 MCP 或供应商配置 |
| `SECURITY_SCOPE_VIOLATION` | 核对 scope 与实际项目来源；global 用于跨项目偏好，受控操作仍有范围限制 |
| `INVALID_ARGUMENT_OR_ATTRIBUTION` | 核对真实 session/candidate/usage/evidence 关联和信任来源；model_self_report 对应 model |
| 只有普通 memory 工具 | 确认 Semantic Memory 注册、插件启用及重启后的专用工具表 |

反馈状态 `pending_confirmation` 与 `reward=0` 对模型自报告可以是正常协议结果，不能改成虚构的用户确认或外部验证。
