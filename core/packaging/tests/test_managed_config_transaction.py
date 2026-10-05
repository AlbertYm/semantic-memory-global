from __future__ import annotations

import hashlib
import json
import locale
import os
import shutil
import subprocess
import time
import traceback
from pathlib import Path
from typing import Callable


ROOT = Path(__file__).resolve().parents[2]
WINDOWS = ROOT / "packaging" / "windows"
PLUGIN = ROOT / "packaging" / "semantic-memory"
EVIDENCE_ROOT = Path(
    os.environ.get(
        "STAGE14_CONFIG_TX_TEST_ROOT",
        ROOT.parents[1]
        / "test-results"
        / "stage14"
        / "stage14-managed-config-transaction-not-configured",
    )
).resolve()
REPAIR = WINDOWS / "Repair-SemanticMemory.ps1"
INSTALLER = WINDOWS / "Install-SemanticMemoryV2.ps1"
TIMEOUT_SECONDS = 90


def parametrize(
    names: tuple[str, ...],
    cases: list[tuple[object, ...]],
) -> Callable[[Callable[..., None]], Callable[..., None]]:
    def decorate(function: Callable[..., None]) -> Callable[..., None]:
        setattr(function, "_parameter_names", names)
        setattr(function, "_parameter_cases", cases)
        return function

    return decorate


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def decode_process_stream(value: bytes | None) -> str:
    if value is None:
        return ""
    if value.startswith((b"\xff\xfe", b"\xfe\xff")):
        return value.decode("utf-16")
    if value.startswith(b"\xef\xbb\xbf"):
        return value.decode("utf-8-sig")
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        return value.decode(locale.getpreferredencoding(False), errors="replace")


def run_powershell(
    script: Path,
    *arguments: object,
    expected_exit: int | None = 0,
    environment: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(script),
        *(str(value) for value in arguments),
    ]
    raw = subprocess.run(
        command,
        capture_output=True,
        env=environment,
        timeout=TIMEOUT_SECONDS,
    )
    result = subprocess.CompletedProcess(
        args=raw.args,
        returncode=raw.returncode,
        stdout=decode_process_stream(raw.stdout),
        stderr=decode_process_stream(raw.stderr),
    )
    if expected_exit is not None:
        assert result.returncode == expected_exit, result.stdout + result.stderr
    return result


def parse_json_result(result: subprocess.CompletedProcess[str]) -> dict[str, object]:
    assert result.stdout.strip(), result.stderr
    return json.loads(result.stdout)


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


def make_payload(root: Path) -> Path:
    root.mkdir(parents=True)
    records: list[dict[str, object]] = []
    for name in (
        "semantic-memory-mcp.exe",
        "semantic-memory-hook.exe",
        "semantic-memory-manager.exe",
    ):
        path = root / name
        path.write_bytes((f"{name}:managed-config-transaction\n").encode("ascii"))
        records.append(
            {
                "path": name,
                "bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )
    manifest = {
        "schema": "stage14-payload-manifest/v1",
        "version_id": "v1.1.0-rc.1+configtxfixture",
        "version": "v1.1.0-rc.1",
        "entrypoints": {
            "mcp": "semantic-memory-mcp.exe",
            "hook": "semantic-memory-hook.exe",
            "manager": "semantic-memory-manager.exe",
        },
        "files": records,
    }
    manifest_path = root / "payload-manifest.json"
    write_json(manifest_path, manifest)
    return manifest_path


def installed_template() -> Path:
    EVIDENCE_ROOT.mkdir(parents=True, exist_ok=True)
    fixture_root = EVIDENCE_ROOT / "installed-template"
    assert not fixture_root.exists()
    fixture_root.mkdir()
    payload_manifest = make_payload(fixture_root / "payload")
    install_root = fixture_root / "install"
    result = run_powershell(
        INSTALLER,
        "-Action",
        "Install",
        "-InstallRoot",
        install_root,
        "-PayloadManifestPath",
        payload_manifest,
    )
    assert parse_json_result(result)["status"] == "PASS"
    verify = run_powershell(
        INSTALLER,
        "-Action",
        "Verify",
        "-InstallRoot",
        install_root,
    )
    assert parse_json_result(verify)["status"] == "PASS"
    return install_root


def copy_install(installed_template: Path, root: Path) -> Path:
    install = root / "安装 根" / "SemanticMemory"
    install.parent.mkdir(parents=True)
    shutil.copytree(installed_template, install)
    return install


def original_config_bytes(secret: str = "third-party-secret-must-not-leak") -> bytes:
    text = (
        'model = "unmanaged-model"\r\n'
        f'third_party_api_key = "{secret}"\r\n'
        'third_party_note = "保留中文与空格"\r\n'
        "\r\n"
        "[mcp_servers.semantic_memory]\r\n"
        "# managed-root-comment-must-survive\r\n"
        "command = 'C:\\Wrong\\semantic-memory-mcp.exe' # command comment\r\n"
        "args = ['--wrong']\r\n"
        "enabled = false\r\n"
        "\r\n"
        "[mcp_servers.semantic_memory.env]\r\n"
        "# managed-env-comment-must-survive\r\n"
        "CBM_DATA_ROOT = 'C:\\Wrong\\data'\r\n"
        'CBM_MEMORY_AUTO_MAINTAIN = "1"\r\n'
        'CBM_MEMORY_EMBED_BACKEND = "network"\r\n'
        'CBM_MEMORY_NO_GLOBAL_UNION = "1"\r\n'
        "\r\n"
        "[mcp_servers.node_repl]\r\n"
        'command = "node"\r\n'
        f'unmanaged_tail = "{secret}-tail"\r\n'
    )
    return b"\xef\xbb\xbf" + text.encode("utf-8")


def security_state(path: Path) -> tuple[str, int]:
    environment = os.environ.copy()
    environment["SM_CONFIG_TX_SECURITY_PATH"] = str(path)
    raw = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "$p=$env:SM_CONFIG_TX_SECURITY_PATH;"
            "$i=Get-Item -Force -LiteralPath $p;"
            "$a=(Get-Acl -LiteralPath $p).Sddl;"
            "[ordered]@{acl=$a;attributes=[int]$i.Attributes}|ConvertTo-Json -Compress",
        ],
        capture_output=True,
        env=environment,
        timeout=TIMEOUT_SECONDS,
    )
    assert raw.returncode == 0, decode_process_stream(raw.stderr)
    parsed = json.loads(decode_process_stream(raw.stdout))
    return str(parsed["acl"]), int(parsed["attributes"])


def set_hidden_attribute(path: Path) -> None:
    environment = os.environ.copy()
    environment["SM_CONFIG_TX_SECURITY_PATH"] = str(path)
    raw = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "$p=$env:SM_CONFIG_TX_SECURITY_PATH;"
            "$a=[IO.File]::GetAttributes($p);"
            "[IO.File]::SetAttributes($p,$a -bor [IO.FileAttributes]::Hidden)",
        ],
        capture_output=True,
        env=environment,
        timeout=TIMEOUT_SECONDS,
    )
    assert raw.returncode == 0, decode_process_stream(raw.stderr)
    assert security_state(path)[1] & 2


def set_readonly_attribute(path: Path) -> None:
    environment = os.environ.copy()
    environment["SM_CONFIG_TX_SECURITY_PATH"] = str(path)
    raw = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "$p=$env:SM_CONFIG_TX_SECURITY_PATH;"
            "$a=[IO.File]::GetAttributes($p);"
            "[IO.File]::SetAttributes($p,$a -bor [IO.FileAttributes]::ReadOnly)",
        ],
        capture_output=True,
        env=environment,
        timeout=TIMEOUT_SECONDS,
    )
    assert raw.returncode == 0, decode_process_stream(raw.stderr)
    assert security_state(path)[1] & 1


def file_identity(path: Path) -> tuple[bytes, int, int]:
    stat = path.stat()
    return path.read_bytes(), stat.st_ino, stat.st_mtime_ns


def tree_state(root: Path) -> dict[str, tuple[bytes, int, int]]:
    if not root.exists():
        return {}
    return {
        path.relative_to(root).as_posix(): (
            path.read_bytes(),
            path.stat().st_ino,
            path.stat().st_mtime_ns,
        )
        for path in sorted(item for item in root.rglob("*") if item.is_file())
    }


def repair(
    config: Path,
    install: Path,
    mode: str,
    *extra: object,
    expected_exit: int | None = 0,
) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
    result = run_powershell(
        REPAIR,
        "-ConfigPath",
        config,
        "-InstallRoot",
        install,
        "-Mode",
        mode,
        *extra,
        expected_exit=expected_exit,
    )
    return result, parse_json_result(result)


def assert_no_secret_in_json_or_process(
    root: Path,
    secret: str,
    *results: subprocess.CompletedProcess[str],
) -> None:
    for result in results:
        assert secret not in result.stdout
        assert secret not in result.stderr
    for path in root.rglob("*.json"):
        assert secret not in path.read_text(encoding="utf-8")


def test_preview_apply_verify_and_replay_are_byte_safe(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "preview-apply-verify-replay"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "用户 配置" / "config.toml"
    config.parent.mkdir()
    secret = "third-party-secret-must-not-leak"
    before = original_config_bytes(secret)
    config.write_bytes(before)
    before_security = security_state(config)
    before_identity = file_identity(config)
    backup_root = install / "backups" / "codex-config-test"

    preview_result, preview = repair(config, install, "Preview")
    assert preview["classification"] == "RED_MANAGED_CONFIG_DRIFT"
    assert preview["apply_performed"] is False
    assert preview["unmanaged_bytes_preserved"] is True
    assert file_identity(config) == before_identity
    assert not backup_root.exists()

    apply_result, applied = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
    )
    assert applied["status"] == "APPLIED_VERIFIED"
    assert applied["write_performed"] is True
    assert applied["unmanaged_bytes_preserved"] is True
    after = config.read_bytes()
    assert after.startswith(b"\xef\xbb\xbf")
    assert b"\n" not in after.replace(b"\r\n", b"")
    for sentinel in (
        'model = "unmanaged-model"\r\n',
        f'third_party_api_key = "{secret}"\r\n',
        'third_party_note = "保留中文与空格"\r\n',
        "# managed-root-comment-must-survive\r\n",
        "# command comment\r\n",
        "# managed-env-comment-must-survive\r\n",
        "[mcp_servers.node_repl]\r\n",
        f'unmanaged_tail = "{secret}-tail"\r\n',
    ):
        assert sentinel.encode("utf-8") in after
    assert after.count(b"[mcp_servers.semantic_memory]\r\n") == 1
    assert after.count(b"[mcp_servers.semantic_memory.env]\r\n") == 1
    assert b"CBM_MEMORY_NO_GLOBAL_UNION" not in after
    assert security_state(config) == before_security
    assert Path(str(applied["backup_path"])).read_bytes() == before
    assert applied["swap_backup_removed"] is True
    assert not Path(str(applied["swap_backup_path"])).exists()

    verify_result, verified = repair(config, install, "Verify")
    assert verified["status"] == "PASS"
    assert verified["managed_fields_match"] is True

    replay_before = file_identity(config)
    backup_before = tree_state(backup_root)
    replay_result, replay = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_file(config),
        "-BackupRoot",
        backup_root,
    )
    assert replay["status"] == "REPLAYED_ZERO_WRITE"
    assert replay["write_performed"] is False
    assert file_identity(config) == replay_before
    assert tree_state(backup_root) == backup_before
    assert_no_secret_in_json_or_process(
        backup_root,
        secret,
        preview_result,
        apply_result,
        verify_result,
        replay_result,
    )
    assert not (PLUGIN / ".mcp.json").exists()
    plugin_manifest = json.loads(
        (PLUGIN / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8")
    )
    assert "mcpServers" not in plugin_manifest


@parametrize(
    ("fault", "expected_status", "expected_write"),
    [
        ("after_backup", "FAILED_BEFORE_SWAP", False),
        ("after_temp", "FAILED_BEFORE_SWAP", False),
        ("after_replace", "ROLLED_BACK_VERIFIED", True),
        ("before_verify", "ROLLED_BACK_VERIFIED", True),
    ],
)
def test_fault_injection_rolls_back_exactly_and_removes_sensitive_temps(
    installed_template: Path,
    fault: str,
    expected_status: str,
    expected_write: bool,
) -> None:
    root = EVIDENCE_ROOT / f"fault-{fault}"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    secret = f"fault-secret-{fault}-must-not-leak"
    before = original_config_bytes(secret)
    config.write_bytes(before)
    before_security = security_state(config)
    backup_root = install / "backups" / "fault-config"

    result, parsed = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
        "-FaultInjection",
        fault,
        expected_exit=1,
    )
    assert parsed["status"] == expected_status
    assert parsed["write_performed"] is expected_write
    assert config.read_bytes() == before
    assert security_state(config) == before_security
    assert Path(str(parsed["backup_path"])).read_bytes() == before
    assert not list(config.parent.glob("*.candidate.tmp"))
    assert not list(config.parent.glob("*.failed-candidate*"))
    assert not list(config.parent.glob("*.rollback-source"))
    assert not list(config.parent.glob("*.compensation-*"))
    assert_no_secret_in_json_or_process(backup_root, secret, result)


def test_readonly_candidate_temp_is_handle_deleted_on_pre_swap_failure(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "readonly-candidate-cleanup"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    secret = "readonly-candidate-secret-must-not-leak"
    before = original_config_bytes(secret)
    config.write_bytes(before)
    set_readonly_attribute(config)
    before_identity = file_identity(config)
    before_security = security_state(config)
    backup_root = install / "backups" / "readonly-candidate-cleanup"

    result, parsed = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
        "-FaultInjection",
        "after_temp",
        expected_exit=1,
    )

    assert parsed["status"] == "FAILED_BEFORE_SWAP"
    assert parsed["write_performed"] is False
    assert file_identity(config) == before_identity
    assert security_state(config) == before_security
    assert not list(config.parent.glob("*.candidate.tmp"))
    assert not list(config.parent.glob("*.failed-candidate*"))
    assert not list(config.parent.glob("*.rollback-source"))
    assert not list(config.parent.glob("*.compensation-*"))
    assert_no_secret_in_json_or_process(backup_root, secret, result)


def test_precondition_conflict_is_zero_write_and_nonzero_exit(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "precondition-conflict"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("cas-precondition-secret")
    config.write_bytes(before)
    identity = file_identity(config)
    backup_root = install / "backups" / "precondition-conflict"
    result, parsed = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        "0" * 64,
        "-BackupRoot",
        backup_root,
        expected_exit=1,
    )
    assert parsed["status"] == "CAS_CONFLICT_PRECONDITION"
    assert parsed["write_performed"] is False
    assert file_identity(config) == identity
    assert not backup_root.exists()
    assert "cas-precondition-secret" not in result.stdout
    assert "cas-precondition-secret" not in result.stderr


def test_apply_requires_exact_lowercase_or_absent_precondition_before_any_write(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "strict-expected-config-sha256"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("strict-precondition-secret")
    config.write_bytes(before)
    before_identity = file_identity(config)
    backup_root = install / "backups" / "strict-precondition"

    cases: tuple[tuple[tuple[object, ...], str], ...] = (
        ((), "CONFIG_EXPECTED_SHA256_REQUIRED"),
        (
            ("-ExpectedConfigSha256", sha256_bytes(before).upper()),
            "CONFIG_EXPECTED_SHA256_INVALID",
        ),
        (
            ("-ExpectedConfigSha256", "not-a-sha256"),
            "CONFIG_EXPECTED_SHA256_INVALID",
        ),
    )
    for arguments, expected_error in cases:
        result, parsed = repair(
            config,
            install,
            "Apply",
            *arguments,
            "-BackupRoot",
            backup_root,
            expected_exit=1,
        )
        assert parsed["status"] == "FAILED_CLOSED"
        assert parsed["error_code"] == expected_error
        assert parsed["write_performed"] is False
        assert file_identity(config) == before_identity
        assert not backup_root.exists()
        assert "strict-precondition-secret" not in result.stdout
        assert "strict-precondition-secret" not in result.stderr


@parametrize(
    ("case_name", "content", "expected_error"),
    [
        (
            "duplicate-managed-table",
            "[mcp_servers.semantic_memory]\ncommand='one'\n"
            "[mcp_servers.semantic_memory]\ncommand='two'\n",
            "CONFIG_DUPLICATE_MANAGED_TABLE",
        ),
        (
            "duplicate-managed-key",
            "[mcp_servers.semantic_memory]\ncommand='one'\ncommand='two'\n",
            "CONFIG_DUPLICATE_MANAGED_KEY",
        ),
        (
            "dotted-managed-key",
            "mcp_servers.semantic_memory.command = 'one'\n",
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
        (
            "inline-managed-table",
            "mcp_servers = { semantic_memory = { command = 'one' } }\n",
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
        (
            "array-managed-table",
            "[[mcp_servers.semantic_memory]]\ncommand='one'\n",
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
        (
            "unknown-managed-key",
            "[mcp_servers.semantic_memory]\nunknown='one'\n",
            "CONFIG_UNKNOWN_MANAGED_KEY",
        ),
        (
            "unknown-managed-subtable",
            "[mcp_servers.semantic_memory.extra]\nvalue='one'\n",
            "CONFIG_UNKNOWN_MANAGED_SUBTABLE",
        ),
        (
            "malformed-managed-header",
            "[mcp_servers.semantic_memory] trailing\ncommand='one'\n",
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
        (
            "escaped-malformed-managed-header",
            '[mcp_servers."semantic\\u005fmemory"] trailing\ncommand=\'one\'\n',
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
        (
            "escaped-unclosed-managed-header",
            '["mcp_servers"."semantic\\u005fmemory"\ncommand=\'one\'\n',
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
        (
            "invalid-managed-literal",
            "[mcp_servers.semantic_memory]\ncommand = 'evil' junk '\n",
            "CONFIG_UNSUPPORTED_MANAGED_VALUE",
        ),
        (
            "duplicate-parent-table",
            "[mcp_servers]\nnode='one'\n[mcp_servers]\nother='two'\n",
            "CONFIG_AMBIGUOUS_MANAGED_SHAPE",
        ),
    ],
)
def test_ambiguous_or_invalid_managed_toml_fails_closed_without_writes(
    installed_template: Path,
    case_name: str,
    content: str,
    expected_error: str,
) -> None:
    root = EVIDENCE_ROOT / f"invalid-{case_name}"
    root.mkdir()
    config = root / "config.toml"
    secret = f"shape-secret-{case_name}"
    before = (f'third_party_key="{secret}"\n' + content).encode("utf-8")
    config.write_bytes(before)
    identity = file_identity(config)
    result, parsed = repair(
        config,
        installed_template,
        "Preview",
        expected_exit=1,
    )
    assert parsed["status"] == "FAILED_CLOSED"
    assert parsed["error_code"] == expected_error
    assert parsed["write_performed"] is False
    assert file_identity(config) == identity
    assert secret not in result.stdout
    assert secret not in result.stderr


@parametrize(
    ("case_name", "content", "expected_error"),
    [
        ("invalid-utf8", b'model="safe"\n\xff', "CONFIG_ENCODING_NOT_STRICT_UTF8"),
        ("utf16-le", b"\xff\xfe[\x00x\x00]\x00", "CONFIG_ENCODING_NOT_UTF8"),
        ("nul", b'model="safe"\x00\n', "CONFIG_CONTAINS_NUL"),
    ],
)
def test_invalid_encoding_fails_closed_without_writes(
    installed_template: Path,
    case_name: str,
    content: bytes,
    expected_error: str,
) -> None:
    root = EVIDENCE_ROOT / f"encoding-{case_name}"
    root.mkdir()
    config = root / "config.toml"
    config.write_bytes(content)
    identity = file_identity(config)
    _, parsed = repair(
        config,
        installed_template,
        "Preview",
        expected_exit=1,
    )
    assert parsed["status"] == "FAILED_CLOSED"
    assert parsed["error_code"] == expected_error
    assert file_identity(config) == identity


def test_reparse_config_path_fails_closed_without_writes(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "reparse-config"
    root.mkdir()
    target = root / "target"
    target.mkdir()
    config = target / "config.toml"
    before = original_config_bytes("reparse-secret")
    config.write_bytes(before)
    link = root / "junction"
    environment = os.environ.copy()
    environment["SM_CONFIG_TX_JUNCTION"] = str(link)
    environment["SM_CONFIG_TX_TARGET"] = str(target)
    created = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "New-Item -ItemType Junction -Path $env:SM_CONFIG_TX_JUNCTION "
            "-Target $env:SM_CONFIG_TX_TARGET | Out-Null",
        ],
        capture_output=True,
        env=environment,
        timeout=TIMEOUT_SECONDS,
    )
    assert created.returncode == 0, decode_process_stream(created.stderr)
    result, parsed = repair(
        link / "config.toml",
        installed_template,
        "Preview",
        expected_exit=1,
    )
    assert parsed["status"] == "FAILED_CLOSED"
    assert parsed["error_code"] == "CONFIG_PATH_REPARSE_POINT"
    assert config.read_bytes() == before
    assert "reparse-secret" not in result.stdout
    assert "reparse-secret" not in result.stderr


def test_missing_config_apply_fault_rollback_and_recover_are_idempotent(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "missing-config"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "new-config" / "config.toml"
    config.parent.mkdir()
    backup_root = install / "backups" / "missing-config"

    apply_result, applied = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        "ABSENT",
        "-BackupRoot",
        backup_root,
    )
    assert applied["status"] == "APPLIED_VERIFIED"
    assert applied["backup_path"] is None
    assert config.is_file()
    verify_result, verified = repair(config, install, "Verify")
    assert verified["status"] == "PASS"
    transaction_path = Path(str(applied["transaction_path"]))

    recover_result, recovered = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
    )
    assert recovered["status"] == "RECOVERED_COMMIT_VERIFIED"
    config_before_replay = file_identity(config)
    transaction_before_replay = tree_state(transaction_path.parent)
    replay_result, replay = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
    )
    assert replay["status"] == "RECOVERED_COMMIT_VERIFIED"
    assert replay["config_write_performed"] is False
    assert file_identity(config) == config_before_replay
    assert tree_state(transaction_path.parent) == transaction_before_replay

    missing_again = root / "new-config" / "faulted.toml"
    fault_result, faulted = repair(
        missing_again,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        "ABSENT",
        "-BackupRoot",
        backup_root,
        "-FaultInjection",
        "after_replace",
        expected_exit=1,
    )
    assert faulted["status"] == "ROLLED_BACK_VERIFIED"
    assert not missing_again.exists()
    assert not list(missing_again.parent.glob("*.failed-candidate*"))
    rollback_transaction = Path(str(faulted["transaction_path"]))
    _, rollback_recovered = repair(
        missing_again,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        rollback_transaction,
    )
    assert rollback_recovered["status"] == "RECOVERED_ROLLBACK_VERIFIED"
    assert not missing_again.exists()
    assert_no_secret_in_json_or_process(
        backup_root,
        "third-party-secret-must-not-leak",
        apply_result,
        verify_result,
        recover_result,
        replay_result,
        fault_result,
    )


def test_post_cas_external_write_is_restored_instead_of_lost(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "post-cas-external-write"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("race-secret-must-not-leak")
    external = before.replace(
        b'model = "unmanaged-model"',
        b'model = "codex-app-concurrent-refresh"',
    )
    assert external != before
    config.write_bytes(before)
    backup_root = install / "backups" / "post-cas-race"
    command = [
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(REPAIR),
        "-ConfigPath",
        str(config),
        "-InstallRoot",
        str(install),
        "-Mode",
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        str(backup_root),
        "-FaultInjection",
        "after_cas_delay",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 15
    observed_cas = False
    while time.monotonic() < deadline and process.poll() is None:
        journals = list(backup_root.rglob("transaction.json")) if backup_root.exists() else []
        for journal in journals:
            try:
                record = json.loads(journal.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                continue
            if record.get("phase") == "CAS_VERIFIED":
                observed_cas = True
                break
        if observed_cas:
            break
        time.sleep(0.02)
    assert observed_cas, "did not observe the deterministic post-CAS race window"
    config.write_bytes(external)
    stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
    result = subprocess.CompletedProcess(
        args=command,
        returncode=process.returncode,
        stdout=decode_process_stream(stdout),
        stderr=decode_process_stream(stderr),
    )
    assert result.returncode == 1, result.stdout + result.stderr
    parsed = parse_json_result(result)
    assert parsed["status"] == "CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE"
    assert parsed["rollback_performed"] is True
    assert config.read_bytes() == external
    assert not list(config.parent.glob("*.candidate.tmp"))
    assert not list(config.parent.glob("*.failed-candidate*"))
    assert "race-secret-must-not-leak" not in result.stdout
    assert "race-secret-must-not-leak" not in result.stderr
    transaction_path = Path(str(parsed["transaction_path"]))
    config_terminal = file_identity(config)
    journal_terminal = tree_state(transaction_path.parent)
    recover_result, recovered = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
        expected_exit=1,
    )
    assert recovered["status"] == "CAS_CONFLICT_RESTORED_EXTERNAL_CHANGE"
    assert recovered["config_write_performed"] is False
    assert recovered["journal_write_performed"] is False
    assert file_identity(config) == config_terminal
    assert tree_state(transaction_path.parent) == journal_terminal
    assert "race-secret-must-not-leak" not in recover_result.stdout
    assert "race-secret-must-not-leak" not in recover_result.stderr


def test_post_swap_external_write_is_never_adopted_as_candidate(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "post-swap-external-write"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("post-swap-secret-must-not-leak")
    external = before.replace(
        b'model = "unmanaged-model"',
        b'model = "codex-app-post-swap-refresh"',
    )
    config.write_bytes(before)
    backup_root = install / "backups" / "post-swap-race"
    command = [
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(REPAIR),
        "-ConfigPath",
        str(config),
        "-InstallRoot",
        str(install),
        "-Mode",
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        str(backup_root),
        "-FaultInjection",
        "after_swap_delay",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 15
    candidate_seen = False
    while time.monotonic() < deadline and process.poll() is None:
        journals = list(backup_root.rglob("transaction.json")) if backup_root.exists() else []
        if journals and config.exists():
            try:
                record = json.loads(journals[0].read_text(encoding="utf-8"))
                candidate_seen = (
                    record.get("phase") == "CAS_VERIFIED"
                    and config.read_bytes() != before
                )
            except (OSError, json.JSONDecodeError):
                candidate_seen = False
            if candidate_seen:
                break
        time.sleep(0.02)
    assert candidate_seen, "did not observe deterministic post-swap window"
    config.write_bytes(external)
    external_security = security_state(config)

    stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
    result = subprocess.CompletedProcess(
        args=command,
        returncode=process.returncode,
        stdout=decode_process_stream(stdout),
        stderr=decode_process_stream(stderr),
    )
    assert result.returncode == 1, result.stdout + result.stderr
    parsed = parse_json_result(result)
    assert parsed["status"] == "ROLLBACK_CONFLICT_EXTERNAL_CHANGE"
    assert parsed["rollback_performed"] is False
    assert config.read_bytes() == external
    assert security_state(config) == external_security
    assert not list(config.parent.glob("*.compensation-*"))
    assert not list(config.parent.glob("*.failed-candidate*"))
    assert "post-swap-secret-must-not-leak" not in result.stdout
    assert "post-swap-secret-must-not-leak" not in result.stderr

    transaction_path = Path(str(parsed["transaction_path"]))
    config_terminal = file_identity(config)
    security_terminal = security_state(config)
    recover_result, recovered = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
        expected_exit=1,
    )
    assert recovered["status"] == "RECOVERY_CONFLICT_EXTERNAL_CHANGE"
    assert recovered["config_write_performed"] is False
    assert file_identity(config) == config_terminal
    assert security_state(config) == security_terminal
    assert "post-swap-secret-must-not-leak" not in recover_result.stdout
    assert "post-swap-secret-must-not-leak" not in recover_result.stderr


def test_post_restore_metadata_only_write_is_preserved(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "post-restore-metadata-write"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("metadata-race-secret-must-not-leak")
    external = before.replace(
        b'model = "unmanaged-model"',
        b'model = "codex-app-first-concurrent-refresh"',
    )
    config.write_bytes(before)
    backup_root = install / "backups" / "post-restore-metadata-race"
    command = [
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(REPAIR),
        "-ConfigPath",
        str(config),
        "-InstallRoot",
        str(install),
        "-Mode",
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        str(backup_root),
        "-FaultInjection",
        "after_cas_and_restore_delay",
    ]
    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    deadline = time.monotonic() + 15
    journal: Path | None = None
    while time.monotonic() < deadline and process.poll() is None:
        journals = list(backup_root.rglob("transaction.json")) if backup_root.exists() else []
        if journals:
            journal = journals[0]
            try:
                record = json.loads(journal.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                time.sleep(0.02)
                continue
            if record.get("phase") == "CAS_VERIFIED":
                break
        time.sleep(0.02)
    assert journal is not None, "did not observe transaction journal"
    assert json.loads(journal.read_text(encoding="utf-8"))["phase"] == "CAS_VERIFIED"
    config.write_bytes(external)

    deadline = time.monotonic() + 15
    restored_external = b""
    while time.monotonic() < deadline and process.poll() is None:
        try:
            record = json.loads(journal.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            time.sleep(0.02)
            continue
        if record.get("phase") == "DISPLACED_EXTERNAL_DETECTED" and config.exists():
            restored_external = config.read_bytes()
            if restored_external == external:
                break
        time.sleep(0.02)
    assert restored_external == external, "did not observe post-source-move window"
    set_hidden_attribute(config)

    stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
    result = subprocess.CompletedProcess(
        args=command,
        returncode=process.returncode,
        stdout=decode_process_stream(stdout),
        stderr=decode_process_stream(stderr),
    )
    assert result.returncode == 1, result.stdout + result.stderr
    parsed = parse_json_result(result)
    assert parsed["status"] == "ROLLBACK_CONFLICT_EXTERNAL_CHANGE"
    assert config.read_bytes() == external
    assert security_state(config)[1] & 2
    assert not list(config.parent.glob("*.failed-candidate*"))
    assert len(list(config.parent.glob("*.compensation-*"))) == 1
    assert "metadata-race-secret-must-not-leak" not in result.stdout
    assert "metadata-race-secret-must-not-leak" not in result.stderr

    transaction_path = Path(str(parsed["transaction_path"]))
    config_terminal = file_identity(config)
    terminal_security = security_state(config)
    journal_before_recover = tree_state(transaction_path.parent)
    recover_result, recovered = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
        expected_exit=1,
    )
    assert recovered["status"] == "RECOVERY_CONFLICT_EXTERNAL_CHANGE"
    assert recovered["config_write_performed"] is False
    assert recovered["journal_write_performed"] is True
    assert file_identity(config) == config_terminal
    assert security_state(config) == terminal_security
    assert tree_state(transaction_path.parent) != journal_before_recover
    assert not list(config.parent.glob("*.compensation-*"))
    assert "metadata-race-secret-must-not-leak" not in recover_result.stdout
    assert "metadata-race-secret-must-not-leak" not in recover_result.stderr
    recovered_config_terminal = file_identity(config)
    recovered_journal_terminal = tree_state(transaction_path.parent)
    replay_result, replay = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
        expected_exit=1,
    )
    assert replay["status"] == "RECOVERY_CONFLICT_EXTERNAL_CHANGE"
    assert replay["config_write_performed"] is False
    assert replay["journal_write_performed"] is False
    assert file_identity(config) == recovered_config_terminal
    assert tree_state(transaction_path.parent) == recovered_journal_terminal
    assert "metadata-race-secret-must-not-leak" not in replay_result.stdout
    assert "metadata-race-secret-must-not-leak" not in replay_result.stderr


def test_recovery_requires_candidate_metadata_and_swap_proof(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "recovery-metadata-and-swap-proof"
    root.mkdir()
    install = copy_install(installed_template, root)

    metadata_config = root / "metadata" / "config.toml"
    metadata_config.parent.mkdir()
    metadata_before = original_config_bytes("metadata-proof-secret-must-not-leak")
    metadata_config.write_bytes(metadata_before)
    metadata_backup_root = install / "backups" / "metadata-proof"
    _, metadata_applied = repair(
        metadata_config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(metadata_before),
        "-BackupRoot",
        metadata_backup_root,
    )
    metadata_transaction = Path(str(metadata_applied["transaction_path"]))
    set_hidden_attribute(metadata_config)
    metadata_terminal = file_identity(metadata_config)
    metadata_security = security_state(metadata_config)
    metadata_result, metadata_recovered = repair(
        metadata_config,
        install,
        "Recover",
        "-BackupRoot",
        metadata_backup_root,
        "-TransactionPath",
        metadata_transaction,
        expected_exit=1,
    )
    assert metadata_recovered["status"] == "RECOVERY_CONFLICT_EXTERNAL_CHANGE"
    assert metadata_recovered["config_write_performed"] is False
    assert file_identity(metadata_config) == metadata_terminal
    assert security_state(metadata_config) == metadata_security

    swap_config = root / "swap" / "config.toml"
    swap_config.parent.mkdir()
    swap_before = original_config_bytes("swap-proof-secret-must-not-leak")
    swap_config.write_bytes(swap_before)
    swap_backup_root = install / "backups" / "swap-proof"
    _, swap_applied = repair(
        swap_config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(swap_before),
        "-BackupRoot",
        swap_backup_root,
    )
    swap_transaction = Path(str(swap_applied["transaction_path"]))
    record = json.loads(swap_transaction.read_text(encoding="utf-8"))
    record["phase"] = "CAS_VERIFIED"
    record["status"] = "IN_PROGRESS"
    write_json(swap_transaction, record)
    assert not Path(str(record["swap_backup_path"])).exists()
    swap_terminal = file_identity(swap_config)
    swap_result, swap_recovered = repair(
        swap_config,
        install,
        "Recover",
        "-BackupRoot",
        swap_backup_root,
        "-TransactionPath",
        swap_transaction,
        expected_exit=1,
    )
    assert swap_recovered["status"] == "RECOVERY_CONFLICT_EXTERNAL_CHANGE"
    assert swap_recovered["config_write_performed"] is False
    assert file_identity(swap_config) == swap_terminal
    swap_record = json.loads(swap_transaction.read_text(encoding="utf-8"))
    assert swap_record["error_code"] == "CONFIG_RECOVERY_SWAP_BACKUP_MISSING"

    for secret in (
        "metadata-proof-secret-must-not-leak",
        "swap-proof-secret-must-not-leak",
    ):
        assert secret not in metadata_result.stdout
        assert secret not in metadata_result.stderr
        assert secret not in swap_result.stdout
        assert secret not in swap_result.stderr


def test_transaction_journals_all_deterministic_sensitive_artifact_intents(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "artifact-intent-journal"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("artifact-intent-secret-must-not-leak")
    config.write_bytes(before)
    backup_root = install / "backups" / "artifact-intent"
    result, failed = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
        "-FaultInjection",
        "after_backup",
        expected_exit=1,
    )
    assert failed["status"] == "FAILED_BEFORE_SWAP"
    transaction_path = Path(str(failed["transaction_path"]))
    record = json.loads(transaction_path.read_text(encoding="utf-8"))
    transaction_id = str(record["transaction_id"])
    prefix = f".{config.name}.semantic-memory.{transaction_id}"
    expected = [
        config.parent / f"{prefix}.rollback-source",
        config.parent / f"{prefix}.failed-candidate",
        *(
            config.parent / f"{prefix}.compensation-{depth}"
            for depth in range(8)
        ),
    ]
    assert [Path(value) for value in record["artifact_intent_paths"]] == expected
    assert len(set(record["artifact_intent_paths"])) == 10
    assert config not in expected
    assert not any(path.exists() for path in expected)
    assert "artifact-intent-secret-must-not-leak" not in result.stdout
    assert "artifact-intent-secret-must-not-leak" not in result.stderr


def test_recover_consumes_safe_artifact_intents_and_preserves_unknown_artifacts(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "artifact-intent-recovery"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    secret = "artifact-recovery-secret-must-not-leak"
    before = original_config_bytes(secret)
    config.write_bytes(before)
    backup_root = install / "backups" / "artifact-recovery"
    _, applied = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
    )
    transaction_path = Path(str(applied["transaction_path"]))
    record = json.loads(transaction_path.read_text(encoding="utf-8"))
    intents = [Path(value) for value in record["artifact_intent_paths"]]

    safe_candidate_artifact = intents[2]
    shutil.copy2(config, safe_candidate_artifact)
    assert security_state(safe_candidate_artifact) == security_state(config)
    first_result, first = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
    )
    assert first["status"] == "RECOVERED_COMMIT_VERIFIED"
    assert first["config_write_performed"] is False
    assert first["journal_write_performed"] is True
    assert not safe_candidate_artifact.exists()

    unknown_artifact = intents[3]
    unknown_artifact.write_bytes(
        b'model="external-unknown-artifact"\nthird_party_key="synthetic"\n'
    )
    unknown_terminal = file_identity(unknown_artifact)
    config_terminal = file_identity(config)
    conflict_result, conflict = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
        expected_exit=1,
    )
    assert conflict["status"] == "RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT"
    assert Path(conflict["recovery_artifact_paths_present"][0]) == unknown_artifact
    assert conflict["config_write_performed"] is False
    assert conflict["journal_write_performed"] is True
    assert file_identity(config) == config_terminal
    assert file_identity(unknown_artifact) == unknown_terminal
    journal_terminal = tree_state(transaction_path.parent)
    replay_result, replay = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
        expected_exit=1,
    )
    assert replay["status"] == "RECOVERY_CONFLICT_SENSITIVE_ARTIFACTS_PRESENT"
    assert replay["config_write_performed"] is False
    assert replay["journal_write_performed"] is False
    assert file_identity(config) == config_terminal
    assert file_identity(unknown_artifact) == unknown_terminal
    assert tree_state(transaction_path.parent) == journal_terminal
    for process_result in (first_result, conflict_result, replay_result):
        assert secret not in process_result.stdout
        assert secret not in process_result.stderr


def test_recover_rejects_journal_cleanup_paths_that_alias_real_config(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "recover-path-binding"
    root.mkdir()
    install = copy_install(installed_template, root)
    backup_root = install / "backups" / "recover-path-binding"

    candidate_config = root / "candidate" / "config.toml"
    candidate_config.parent.mkdir()
    candidate_config.write_bytes(original_config_bytes("candidate-path-secret"))
    _, applied = repair(
        candidate_config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_file(candidate_config),
        "-BackupRoot",
        backup_root,
    )
    candidate_transaction = Path(str(applied["transaction_path"]))
    candidate_journal = json.loads(candidate_transaction.read_text(encoding="utf-8"))
    candidate_journal["temporary_path"] = str(candidate_config)
    write_json(candidate_transaction, candidate_journal)
    candidate_identity = file_identity(candidate_config)
    _, rejected_candidate = repair(
        candidate_config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        candidate_transaction,
        expected_exit=1,
    )
    assert rejected_candidate["status"] == "FAILED_CLOSED"
    assert rejected_candidate["error_code"] == "CONFIG_RECOVERY_TEMP_PATH_INVALID"
    assert file_identity(candidate_config) == candidate_identity

    before_config = root / "before" / "config.toml"
    before_config.parent.mkdir()
    before_bytes = original_config_bytes("before-path-secret")
    before_config.write_bytes(before_bytes)
    _, faulted = repair(
        before_config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before_bytes),
        "-BackupRoot",
        backup_root,
        "-FaultInjection",
        "after_backup",
        expected_exit=1,
    )
    before_transaction = Path(str(faulted["transaction_path"]))
    before_journal = json.loads(before_transaction.read_text(encoding="utf-8"))
    before_journal["swap_backup_path"] = str(before_config)
    write_json(before_transaction, before_journal)
    before_identity = file_identity(before_config)
    _, rejected_before = repair(
        before_config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        before_transaction,
        expected_exit=1,
    )
    assert rejected_before["status"] == "FAILED_CLOSED"
    assert rejected_before["error_code"] == "CONFIG_RECOVERY_BACKUP_PATH_INVALID"
    assert file_identity(before_config) == before_identity


def test_production_canary_apply_verify_recover_replay_and_exact_removal(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "production-canary-lifecycle"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config" / "config.toml"
    config.parent.mkdir()
    before = original_config_bytes("canary-third-party-secret")
    config.write_bytes(before)
    auth = root / "authorization" / "production-canary.json"
    write_json(
        auth,
        {
            "schema": "stage14-production-canary-authorization/v1",
            "authorization_id": "isolated-canary-fixture",
        },
    )
    auth_hash = sha256_file(auth)
    backup_root = install / "backups" / "production-canary"
    canary_args = (
        "-EnableProductionCanary",
        "-CanaryAuthManifestPath",
        auth,
        "-CanaryAuthSha256",
        auth_hash,
    )

    _, preview = repair(config, install, "Preview", *canary_args)
    assert preview["production_canary_enabled"] is True
    assert set(preview["changed_managed_fields"]).issuperset(
        {
            "mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE",
            "mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE",
            "mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST",
            "mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256",
        }
    )
    _, applied = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
        *canary_args,
    )
    assert applied["status"] == "APPLIED_VERIFIED"
    assert applied["production_canary_enabled"] is True
    text = config.read_text(encoding="utf-8-sig")
    assert 'CBM_STAGE14_PRODUCTION_GATE = "1"' in text
    assert 'CBM_STAGE14_EVOLUTION_MODE = "bounded_canary"' in text
    assert f'CBM_STAGE14_CANARY_AUTH_SHA256 = "{auth_hash}"' in text
    assert str(auth).replace("\\", "\\\\") in text
    assert "CBM_MEMORY_NO_GLOBAL_UNION" not in text

    _, verified = repair(config, install, "Verify", *canary_args)
    assert verified["status"] == "PASS"
    assert verified["production_canary_enabled"] is True
    terminal = file_identity(config)
    transaction_path = Path(str(applied["transaction_path"]))
    _, recovered = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
    )
    assert recovered["status"] == "RECOVERED_COMMIT_VERIFIED"
    assert recovered["config_write_performed"] is False
    assert file_identity(config) == terminal
    recovered_tree = tree_state(transaction_path.parent)
    _, recovered_replay = repair(
        config,
        install,
        "Recover",
        "-BackupRoot",
        backup_root,
        "-TransactionPath",
        transaction_path,
    )
    assert recovered_replay["status"] == "RECOVERED_COMMIT_VERIFIED"
    assert recovered_replay["journal_write_performed"] is False
    assert tree_state(transaction_path.parent) == recovered_tree

    with_canary = config.read_bytes()
    _, removal_preview = repair(config, install, "Preview")
    assert removal_preview["production_canary_enabled"] is False
    assert set(removal_preview["changed_managed_fields"]) == {
        "mcp_servers.semantic_memory.env.CBM_STAGE14_PRODUCTION_GATE",
        "mcp_servers.semantic_memory.env.CBM_STAGE14_EVOLUTION_MODE",
        "mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_MANIFEST",
        "mcp_servers.semantic_memory.env.CBM_STAGE14_CANARY_AUTH_SHA256",
    }
    _, removed = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(with_canary),
        "-BackupRoot",
        backup_root,
    )
    assert removed["status"] == "APPLIED_VERIFIED"
    assert removed["production_canary_enabled"] is False
    final_text = config.read_text(encoding="utf-8-sig")
    assert "CBM_STAGE14_" not in final_text
    assert 'CBM_MEMORY_AUTO_MAINTAIN = "0"' in final_text
    assert 'CBM_MEMORY_EMBED_BACKEND = "static"' in final_text
    assert "CBM_MEMORY_NO_GLOBAL_UNION" not in final_text
    assert "canary-third-party-secret" in final_text
    _, final_verify = repair(config, install, "Verify")
    assert final_verify["status"] == "PASS"
    final_identity = file_identity(config)
    transaction_count = len(list(backup_root.glob("config-repair__*")))
    _, removal_replay = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_file(config),
        "-BackupRoot",
        backup_root,
    )
    assert removal_replay["status"] == "REPLAYED_ZERO_WRITE"
    assert file_identity(config) == final_identity
    assert len(list(backup_root.glob("config-repair__*"))) == transaction_count


def test_production_canary_binding_and_namespace_fail_closed(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "production-canary-fail-closed"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config.toml"
    before = original_config_bytes("canary-fail-closed-secret")
    config.write_bytes(before)
    identity = file_identity(config)
    auth = root / "authorization.json"
    write_json(auth, {"schema": "stage14-production-canary-authorization/v1"})
    auth_hash = sha256_file(auth)

    for arguments, expected_error in (
        (
            (
                "-EnableProductionCanary",
                "-CanaryAuthManifestPath",
                auth,
            ),
            "CONFIG_CANARY_ARGUMENTS_INCOMPLETE",
        ),
        (
            (
                "-EnableProductionCanary",
                "-CanaryAuthManifestPath",
                auth,
                "-CanaryAuthSha256",
                "0" * 64,
            ),
            "CONFIG_CANARY_AUTH_BINDING_MISMATCH",
        ),
        (
            (
                "-CanaryAuthManifestPath",
                auth,
                "-CanaryAuthSha256",
                auth_hash,
            ),
            "CONFIG_CANARY_ENABLE_SWITCH_REQUIRED",
        ),
    ):
        result, parsed = repair(
            config,
            install,
            "Preview",
            *arguments,
            expected_exit=1,
        )
        assert parsed["status"] == "FAILED_CLOSED"
        assert parsed["error_code"] == expected_error
        assert file_identity(config) == identity
        assert "canary-fail-closed-secret" not in result.stdout
        assert "canary-fail-closed-secret" not in result.stderr

    arbitrary = (
        '[mcp_servers.semantic_memory.env]\n'
        'CBM_STAGE14_ARBITRARY_ESCAPE = "1"\n'
    ).encode("utf-8")
    config.write_bytes(arbitrary)
    arbitrary_identity = file_identity(config)
    _, rejected = repair(
        config,
        install,
        "Preview",
        expected_exit=1,
    )
    assert rejected["status"] == "FAILED_CLOSED"
    assert rejected["error_code"] == "CONFIG_UNKNOWN_MANAGED_KEY"
    assert file_identity(config) == arbitrary_identity


def test_production_canary_fault_injection_restores_original_bytes(
    installed_template: Path,
) -> None:
    root = EVIDENCE_ROOT / "production-canary-fault-rollback"
    root.mkdir()
    install = copy_install(installed_template, root)
    config = root / "config.toml"
    before = original_config_bytes("canary-fault-secret")
    config.write_bytes(before)
    auth = root / "authorization.json"
    write_json(auth, {"schema": "stage14-production-canary-authorization/v1"})
    backup_root = install / "backups" / "canary-fault"
    result, parsed = repair(
        config,
        install,
        "Apply",
        "-ExpectedConfigSha256",
        sha256_bytes(before),
        "-BackupRoot",
        backup_root,
        "-EnableProductionCanary",
        "-CanaryAuthManifestPath",
        auth,
        "-CanaryAuthSha256",
        sha256_file(auth),
        "-FaultInjection",
        "after_replace",
        expected_exit=1,
    )
    assert parsed["status"] == "ROLLED_BACK_VERIFIED"
    assert config.read_bytes() == before
    assert "canary-fault-secret" not in result.stdout
    assert "canary-fault-secret" not in result.stderr


EXPECTED_TEST_NAMES = frozenset(
    {
        "test_ambiguous_or_invalid_managed_toml_fails_closed_without_writes",
        "test_apply_requires_exact_lowercase_or_absent_precondition_before_any_write",
        "test_fault_injection_rolls_back_exactly_and_removes_sensitive_temps",
        "test_invalid_encoding_fails_closed_without_writes",
        "test_missing_config_apply_fault_rollback_and_recover_are_idempotent",
        "test_post_cas_external_write_is_restored_instead_of_lost",
        "test_post_restore_metadata_only_write_is_preserved",
        "test_post_swap_external_write_is_never_adopted_as_candidate",
        "test_precondition_conflict_is_zero_write_and_nonzero_exit",
        "test_preview_apply_verify_and_replay_are_byte_safe",
        "test_production_canary_apply_verify_recover_replay_and_exact_removal",
        "test_production_canary_binding_and_namespace_fail_closed",
        "test_production_canary_fault_injection_restores_original_bytes",
        "test_readonly_candidate_temp_is_handle_deleted_on_pre_swap_failure",
        "test_recover_consumes_safe_artifact_intents_and_preserves_unknown_artifacts",
        "test_recover_rejects_journal_cleanup_paths_that_alias_real_config",
        "test_recovery_requires_candidate_metadata_and_swap_proof",
        "test_reparse_config_path_fails_closed_without_writes",
        "test_transaction_journals_all_deterministic_sensitive_artifact_intents",
    }
)


def validate_discovered_test_names(
    discovered: set[str], expected: set[str] | frozenset[str]
) -> None:
    if discovered != set(expected):
        raise RuntimeError(
            "TEST_DISCOVERY_CONTRACT_MISMATCH: "
            f"expected={sorted(expected)!r} discovered={sorted(discovered)!r}"
        )


def main() -> int:
    fixture = installed_template()
    results: list[dict[str, object]] = []
    discovered = {
        name: test
        for name, test in globals().items()
        if name.startswith("test_") and callable(test)
    }
    validate_discovered_test_names(set(discovered), EXPECTED_TEST_NAMES)
    for name, test in sorted(discovered.items()):
        parameter_names = tuple(getattr(test, "_parameter_names", ()))
        parameter_cases = list(getattr(test, "_parameter_cases", [()]))
        for parameter_case in parameter_cases:
            case_values = dict(zip(parameter_names, parameter_case, strict=True))
            case_label = ",".join(str(case_values[item]) for item in parameter_names)
            result_name = f"{name}[{case_label}]" if case_label else name
            try:
                test(installed_template=fixture, **case_values)
                results.append({"name": result_name, "status": "PASS"})
                print(f"PASS {result_name}")
            except Exception as exc:
                results.append(
                    {
                        "name": result_name,
                        "status": "FAIL",
                        "error": str(exc),
                        "traceback": traceback.format_exc(),
                    }
                )
                print(f"FAIL {result_name}: {exc}")
    payload = {
        "schema": "stage14-managed-config-transaction-tests/v1",
        "status": (
            "PASS_STAGE14_MANAGED_CONFIG_TRANSACTION"
            if all(item["status"] == "PASS" for item in results)
            else "FAIL_STAGE14_MANAGED_CONFIG_TRANSACTION"
        ),
        "pass_count": sum(item["status"] == "PASS" for item in results),
        "fail_count": sum(item["status"] == "FAIL" for item in results),
        "tests": results,
    }
    write_json(EVIDENCE_ROOT / "summary.json", payload)
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if payload["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
