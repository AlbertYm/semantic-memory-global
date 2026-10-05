# Semantic Memory Windows 本地离线安装

Stage 14 RC 离线包默认安装到 `%LOCALAPPDATA%\SemanticMemory`。安装过程仅写用户目录，不写注册表、不修改 `PATH`、不安装系统服务，也不设置开机自启动。

## 安装内容

- `app\versions\<version_id>`：按版本并存、经 manifest 验证的 payload。
- `bin\semantic-memory-mcp.exe`：稳定 MCP 入口。
- `bin\semantic-memory-hook.exe`：稳定 lifecycle hook 入口。
- `bin\semantic-memory-manager.exe`：稳定 Memory Manager 入口。
- `data`、`backups`、`logs`、`state`：用户数据、备份、日志和安装状态。
- `plugin\semantic-memory`：离线包内的 Personal Plugin 候选，仅包含三条 hooks、Memory Manager 入口和 memory skill。

同一 candidate 的三个 native 角色优先使用 NTFS hardlink，文件系统不支持时自动回退为 copy。升级采用 side-by-side 版本目录；切换失败时保留已验证的旧稳定入口。

## 使用入口

双击 `Install Semantic Memory.cmd` 会调用 `Install-SemanticMemoryV2.ps1` 执行 `Install`，并验证 `payload\payload-manifest.json`。高级操作可使用脚本的 `Install`、`Upgrade`、`Repair`、`Verify` 和 `Uninstall` 动作。

Personal Plugin 随离线包交付，但不注册 MCP，避免与全局入口形成重复 namespace。唯一的 `semantic_memory` MCP 入口由用户级 `config.toml` 受管 block 指向稳定 native launcher。本地 payload 安装不等于 Codex App 已加载 Plugin；真实用户级 Plugin/config 事务、hook 信任和 Reload 必须按 Stage 14 canary 闸门单独验收。

## Personal Plugin 原子事务

`Install-SemanticMemoryPlugin.ps1` 提供 `Preview`、`Apply`、`Verify`、`Rollback` 和 `Recover`。它同时管理三个用户级状态：`~/plugins/semantic-memory`、Personal marketplace 中唯一的 `semantic-memory` 记录，以及 `~/.codex/plugins/cache/personal/semantic-memory`。

`Apply` 必须显式传入 source、cache、marketplace 三个写前 CAS 值；不存在时使用精确哨兵 `ABSENT`，并且必须带 `-ConfirmUserMutation`。事务会先校验离线包 manifest 和全部插件文件哈希，拒绝 `.mcp.json`、`mcpServers`、reparse/junction 和越界路径，再使用同卷 staging 与目录 rename 切换。原状态和 marketplace 原字节会保存在事务目录，阶段事件只追加到 JSONL journal；失败自动恢复，进程中断后由 `Recover` 收敛。已经完全一致的 `Apply` 返回 `REPLAYED_ZERO_WRITE`，不会新建事务或再次调用 CLI。

需要走 Codex CLI 合同时，显式传入 `-InvokeCodexCli -CodexCliPath <路径>`。固定命令仅为 `plugin marketplace add <personal marketplace 路径>` 和 `plugin add semantic-memory@personal`；脚本不记录 CLI stdout/stderr，也不把 token 或凭据写入 journal。包内验收只使用隔离用户根和受控 mock CLI，不调用真实 Codex CLI。

## 临时 production canary 配置

`Repair-SemanticMemory.ps1` 的 `Preview`、`Apply`、`Verify` 支持显式的 `-EnableProductionCanary -CanaryAuthManifestPath <绝对路径> -CanaryAuthSha256 <小写 64 位 SHA256>`。启用时只允许以下四项，并把它们纳入语义指纹、CAS、白名单差异、回滚和 Recover：

- `CBM_STAGE14_PRODUCTION_GATE=1`
- `CBM_STAGE14_EVOLUTION_MODE=bounded_canary`
- `CBM_STAGE14_CANARY_AUTH_MANIFEST=<绝对路径>`
- `CBM_STAGE14_CANARY_AUTH_SHA256=<小写 64 位 SHA256>`

未提供 canary 参数时，事务会精确移除这四项。任何其他 `CBM_STAGE14_*` 都 fail closed。永久安全值保持 `CBM_MEMORY_AUTO_MAINTAIN=0`、`CBM_MEMORY_EMBED_BACKEND=static`；全局 fallback 不写 `CBM_MEMORY_NO_GLOBAL_UNION`。

## 数据保留

`Uninstall` 默认保留 `data`，并把程序、稳定入口和状态移入安装根下的 `uninstalled` 审计目录。卸载完成不等于用户数据已删除；本安装器不提供真实数据物理清理。

`Verify` 会校验 current pointer、verification receipt、payload manifest 和三个稳定 native 入口。检测到 pointer、payload 或入口被篡改时会 fail closed；`Repair` 只从当前已验证 payload 重建稳定入口。
