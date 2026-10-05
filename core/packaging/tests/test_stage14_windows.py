from __future__ import annotations

import hashlib
import json
import locale
import os
import shutil
import sqlite3
import subprocess
import time
import traceback
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WINDOWS = ROOT / "packaging" / "windows"
PLUGIN = ROOT / "packaging" / "semantic-memory"
EVIDENCE_ROOT = Path(os.environ.get("STAGE14_AGENT_B_TEST_ROOT", ROOT / "build" / "stage14-agent-b-not-configured")).resolve()
REAL_CANDIDATE = Path(os.environ.get("STAGE14_REAL_CANDIDATE", ROOT / "build" / "stage14-agent-b-candidate-not-configured.exe")).resolve()
SUBPROCESS_TIMEOUT_SECONDS = 120


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def file_tree_bytes(root: Path) -> dict[str, bytes]:
    return {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in sorted(item for item in root.rglob("*") if item.is_file())
    }


def file_tree_state(root: Path) -> dict[str, tuple[bytes, int, int]]:
    return {
        path.relative_to(root).as_posix(): (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns)
        for path in sorted(item for item in root.rglob("*") if item.is_file())
    }


def tree_metadata_state(root: Path) -> dict[str, tuple[int, int, int, int, int]]:
    root_metadata = root.stat(follow_symlinks=False)
    state: dict[str, tuple[int, int, int, int, int]] = {
        ".": (
            root_metadata.st_mode,
            root_metadata.st_size,
            root_metadata.st_mtime_ns,
            root_metadata.st_ino,
            getattr(root_metadata, "st_file_attributes", 0),
        )
    }
    pending = [root]
    while pending:
        current = pending.pop()
        for entry in os.scandir(current):
            path = Path(entry.path)
            metadata = entry.stat(follow_symlinks=False)
            state[path.relative_to(root).as_posix()] = (
                metadata.st_mode,
                metadata.st_size,
                metadata.st_mtime_ns,
                metadata.st_ino,
                getattr(metadata, "st_file_attributes", 0),
            )
            if entry.is_dir(follow_symlinks=False):
                pending.append(path)
    return state


def exact_tree_state(root: Path) -> tuple[
    set[tuple[str, str]],
    dict[str, tuple[bytes, int, int]],
    dict[str, tuple[int, int, int, int, int]],
]:
    return tree_entries(root), file_tree_state(root), tree_metadata_state(root)


def sqlite_transient_sidecars(root: Path) -> list[Path]:
    return [
        path for path in root.rglob("*")
        if path.is_file() and path.name.endswith(("-wal", "-shm", "-journal"))
    ]


def exact_file_states(paths: tuple[Path, ...]) -> dict[str, tuple[bytes, int, int]]:
    return {
        str(path.resolve()): (path.read_bytes(), path.stat().st_ino, path.stat().st_mtime_ns)
        for path in paths
    }


def sqlite_path_sidecars(paths: tuple[Path, ...]) -> list[Path]:
    return [
        Path(f"{path}{suffix}")
        for path in paths
        for suffix in ("-wal", "-shm", "-journal")
        if Path(f"{path}{suffix}").exists()
    ]


def tree_entries(root: Path) -> set[tuple[str, str]]:
    return {
        (path.relative_to(root).as_posix(), "dir" if path.is_dir() else "file")
        for path in root.rglob("*")
    }


def make_junction(path: Path, target: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment["SM_TEST_JUNCTION"] = str(path)
    environment["SM_TEST_JUNCTION_TARGET"] = str(target)
    result = subprocess.run(
        [
            "powershell.exe", "-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
            "$ErrorActionPreference='Stop'; New-Item -ItemType Junction "
            "-Path $env:SM_TEST_JUNCTION -Target $env:SM_TEST_JUNCTION_TARGET | Out-Null",
        ],
        capture_output=True,
        env=environment,
        timeout=SUBPROCESS_TIMEOUT_SECONDS,
    )
    assert result.returncode == 0, decode_process_stream(result.stdout) + decode_process_stream(result.stderr)
    assert path.is_dir()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def decode_process_stream(value: bytes | str | None) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    if value.startswith((b"\xff\xfe", b"\xfe\xff")):
        return value.decode("utf-16")
    if value.startswith(b"\xef\xbb\xbf"):
        return value.decode("utf-8-sig")
    try:
        return value.decode("utf-8")
    except UnicodeDecodeError:
        return value.decode(locale.getpreferredencoding(False), errors="replace")


def run_ps(
    script: Path, *args: str, expect: int = 0, environment: dict[str, str] | None = None,
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
        *map(str, args),
    ]
    raw = subprocess.run(
        command, capture_output=True, env=environment, timeout=SUBPROCESS_TIMEOUT_SECONDS,
    )
    result = subprocess.CompletedProcess(
        args=raw.args,
        returncode=raw.returncode,
        stdout=decode_process_stream(raw.stdout),
        stderr=decode_process_stream(raw.stderr),
    )
    if expect == 0:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0, result.stdout + result.stderr
    return result


def make_payload(root: Path, version_id: str, content: str) -> Path:
    root.mkdir(parents=True)
    files = []
    for name in ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe"):
        path = root / name
        path.write_text(f"{name}:{content}\n", encoding="ascii")
        files.append({"path": name, "bytes": path.stat().st_size, "sha256": sha256(path)})
    manifest = {
        "schema": "stage14-payload-manifest/v1",
        "version_id": version_id,
        "version": version_id.split("+")[0],
        "entrypoints": {
            "mcp": "semantic-memory-mcp.exe",
            "hook": "semantic-memory-hook.exe",
            "manager": "semantic-memory-manager.exe",
        },
        "files": files,
    }
    path = root / "payload-manifest.json"
    write_json(path, manifest)
    return path


def make_native_payload(root: Path, version_id: str) -> Path:
    assert REAL_CANDIDATE.is_file(), f"missing Stage 14 native candidate: {REAL_CANDIDATE}"
    root.mkdir(parents=True)
    files = []
    for name in ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe"):
        path = root / name
        try:
            os.link(REAL_CANDIDATE, path)
        except OSError:
            shutil.copy2(REAL_CANDIDATE, path)
        files.append({"path": name, "bytes": path.stat().st_size, "sha256": sha256(path)})
    manifest = {
        "schema": "stage14-payload-manifest/v1",
        "version_id": version_id,
        "version": version_id.split("+")[0],
        "entrypoints": {
            "mcp": "semantic-memory-mcp.exe",
            "hook": "semantic-memory-hook.exe",
            "manager": "semantic-memory-manager.exe",
        },
        "files": files,
    }
    path = root / "payload-manifest.json"
    write_json(path, manifest)
    return path


def run_native(
    executable: Path, *args: str, expect: int = 0, timeout: float = 2.0,
    environment: dict[str, str] | None = None, cwd: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    raw = subprocess.run(
        [str(executable), *args], capture_output=True, env=environment, cwd=cwd, timeout=timeout,
    )
    result = subprocess.CompletedProcess(
        args=raw.args,
        returncode=raw.returncode,
        stdout=decode_process_stream(raw.stdout),
        stderr=decode_process_stream(raw.stderr),
    )
    if expect == 0:
        assert result.returncode == 0, result.stdout + result.stderr
    else:
        assert result.returncode != 0, result.stdout + result.stderr
    return result


def managed_toml(command: str, extra: str = "", omit_enabled: bool = False) -> str:
    enabled = "" if omit_enabled else "enabled = true\n"
    return f"""
model = "unmanaged-model"
{extra}
[mcp_servers.semantic_memory]
args = []
{enabled}command = '{command}'

[mcp_servers.semantic_memory.env]
CBM_MEMORY_EMBED_BACKEND = "static"
CBM_DATA_ROOT = 'C:\\Users\\Example\\AppData\\Local\\SemanticMemory\\data'
CBM_MEMORY_AUTO_MAINTAIN = "0"
""".lstrip()


def test_plugin_contract_and_short_hooks() -> None:
    manifest = json.loads((PLUGIN / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8"))
    assert PLUGIN.name == manifest["name"] == "semantic-memory"
    assert manifest["version"].startswith("1.1.0-rc.1+codex.")
    assert manifest["version"].count("+codex.") == 1
    assert "hooks" not in manifest
    assert "mcpServers" not in manifest
    assert not (PLUGIN / ".mcp.json").exists()
    assert "Local MCP" not in manifest["interface"]["capabilities"]
    assert set(manifest["interface"]["capabilities"]) == {
        "Lifecycle hooks",
        "Memory skill",
        "Memory Manager",
    }
    assert "apps" not in manifest and not (PLUGIN / ".app.json").exists()
    hooks = json.loads((PLUGIN / "hooks" / "hooks.json").read_text(encoding="utf-8"))["hooks"]
    assert set(hooks) == {"UserPromptSubmit", "PostToolUse", "Stop"}
    commands = [hooks[name][0]["hooks"][0]["command"] for name in hooks]
    assert all(len(command) < 320 for command in commands)
    assert all("H:\\Codex_H" not in command for command in commands)
    assert all("CBM_" not in command and "set " not in command.lower() for command in commands)
    hook_actions = {"UserPromptSubmit": "recall", "PostToolUse": "post-tool", "Stop": "stop"}
    for name, action in hook_actions.items():
        hook = hooks[name][0]["hooks"][0]
        assert hook["command"] == (
            'powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass '
            '-File "${CLAUDE_PLUGIN_ROOT}\\scripts\\semantic-memory-hook.ps1" ' + action
        )
        assert hook["timeout"] == 2
        assert "%LOCALAPPDATA%" not in hook["command"]

    hook_script = (PLUGIN / "scripts" / "semantic-memory-hook.ps1").read_text(encoding="utf-8")
    assert "semantic-memory-hook.exe" in hook_script
    assert "semantic-memory-launcher.ps1" not in hook_script

    manager_script = (PLUGIN / "scripts" / "semantic-memory-manager.ps1").read_text(encoding="utf-8")
    assert "param([switch]$VerifyOnly)" in manager_script
    assert "semantic-memory-launcher.ps1" in manager_script
    assert "-Mode manager -VerifyOnly:$VerifyOnly" in manager_script

def test_release_tool_generates_stage14_payload_manifest() -> None:
    root = EVIDENCE_ROOT / "manifest-stage14-disposable"
    payload_root = root / "payload"
    payload_root.mkdir(parents=True)
    for name in ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe"):
        (payload_root / name).write_text(f"{name}\n", encoding="ascii")
    output = payload_root / "payload-manifest.json"
    result = subprocess.run(
        [
            "python",
            str(ROOT / "packaging" / "release_manifest.py"),
            "--source-root",
            str(ROOT),
            "--stage14-payload-root",
            str(payload_root),
            "--stage14-version-id",
            "v1.1.0-rc.1+fixture",
            "--stage14-payload-output",
            str(output),
        ],
        text=True,
        capture_output=True,
        encoding="utf-8",
        env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"},
        timeout=SUBPROCESS_TIMEOUT_SECONDS,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    manifest = json.loads(output.read_text(encoding="utf-8"))
    assert manifest["schema"] == "stage14-payload-manifest/v1"
    assert manifest["version_id"] == "v1.1.0-rc.1+fixture"
    assert len(manifest["files"]) == 3
    assert all("H:\\" not in json.dumps(record) for record in manifest["files"])


def test_semantic_fingerprint_and_repair_preview() -> None:
    root = EVIDENCE_ROOT / "config-stage14-disposable"
    root.mkdir(parents=True)
    reference = root / "reference.toml"
    unrelated = root / "unrelated.toml"
    managed = root / "managed.toml"
    reference.write_text(managed_toml("C:\\Stable\\semantic-memory-mcp.exe"), encoding="utf-8")
    unrelated.write_text(
        "# formatting and unrelated provider change\n"
        + managed_toml("c:/stable/semantic-memory-mcp.exe", extra="model_provider = 'other-local-provider'"),
        encoding="utf-8",
    )
    managed.write_text(managed_toml("C:\\Changed\\semantic-memory-mcp.exe", omit_enabled=True), encoding="utf-8")
    green = run_ps(
        WINDOWS / "Repair-SemanticMemory.ps1",
        "-ReferenceConfigPath",
        reference,
        "-CandidateConfigPath",
        unrelated,
    )
    green_json = json.loads(green.stdout)
    assert green_json["classification"] == "GREEN_UNRELATED_DRIFT"
    assert green_json["operations"] == []
    assert green_json["whole_config_sha256_before"] != green_json["whole_config_sha256_after"]
    red = run_ps(
        WINDOWS / "Repair-SemanticMemory.ps1",
        "-ReferenceConfigPath",
        reference,
        "-CandidateConfigPath",
        managed,
    )
    red_json = json.loads(red.stdout)
    assert red_json["classification"] == "RED_MANAGED_CONFIG_DRIFT"
    fields = {item["field"] for item in red_json["operations"]}
    assert "mcp_servers.semantic_memory.command" in fields
    assert "mcp_servers.semantic_memory.enabled" in fields
    assert red_json["apply_performed"] is False


def test_adversarial_toml_strings_cannot_forge_managed_table_or_expose_credentials() -> None:
    root = EVIDENCE_ROOT / "config-adversarial-stage14-disposable"
    root.mkdir(parents=True)
    reference = root / "reference.toml"
    multiline = root / "multiline.toml"
    quoted = root / "quoted.toml"
    command = "C:\\Stable\\semantic-memory-mcp.exe"
    multiline_secret = "multiline-secret-must-not-appear"
    quoted_secret = "quoted-secret-must-not-appear"
    reference.write_text(managed_toml(command), encoding="utf-8")
    multiline.write_text(
        'third_party_blob = """\n'
        '[mcp_servers.semantic_memory]\n'
        f'command = "C:\\\\Forged\\\\{multiline_secret}.exe"\n'
        'enabled = false\n'
        '"""\n\n'
        + managed_toml(command, extra="provider = 'changed-but-unmanaged'"),
        encoding="utf-8",
    )
    quoted.write_text(
        f'third_party = "[mcp_servers.semantic_memory] command={quoted_secret}"\n'
        '[third_party.reordered]\n'
        'enabled = true # unrelated comment\n\n'
        + managed_toml(command),
        encoding="utf-8",
    )
    for candidate, secret in ((multiline, multiline_secret), (quoted, quoted_secret)):
        result = run_ps(
            WINDOWS / "Repair-SemanticMemory.ps1",
            "-ReferenceConfigPath", reference,
            "-CandidateConfigPath", candidate,
        )
        parsed = json.loads(result.stdout)
        assert parsed["classification"] == "GREEN_UNRELATED_DRIFT"
        assert parsed["operations"] == []
        assert parsed["raw_credential_values_recorded"] is False
        assert secret not in result.stdout
        assert secret not in result.stderr


def test_side_by_side_upgrade_repair_verify_and_retain_data_uninstall() -> None:
    root = EVIDENCE_ROOT / "installer-stage14-disposable"
    package1 = make_payload(root / "package-v1", "v1.1.0-rc.1+aaa111", "one")
    package2 = make_payload(root / "package-v2", "v1.1.0+bbb222", "two")
    install = root / "install"
    install.mkdir(parents=True)
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"
    first = run_ps(installer, "-Action", "Install", "-InstallRoot", install, "-PayloadManifestPath", package1)
    assert json.loads(first.stdout)["current_pointer_switched"] is True
    for relative in ("data/projects", "data/artifacts", "backups", "logs"):
        assert (install / relative).is_dir()
    stable_names = ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe")
    for name in stable_names:
        assert (install / "bin" / name).read_bytes() == (package1.parent / name).read_bytes()
        assert os.stat(install / "bin" / name).st_ino == os.stat(
            install / "app" / "versions" / "v1.1.0-rc.1+aaa111" / name
        ).st_ino
    stable_launcher = install / "bin" / "semantic-memory-launcher.ps1"
    assert stable_launcher.read_bytes() == (WINDOWS / "semantic-memory-launcher.ps1").read_bytes()
    assert not (install / "bin" / "SemanticMemory.Common.psm1").exists()
    manager_environment = os.environ.copy()
    manager_environment["SEMANTIC_MEMORY_HOME"] = str(install)
    manager_probe = run_ps(
        PLUGIN / "scripts" / "semantic-memory-manager.ps1",
        "-VerifyOnly",
        environment=manager_environment,
    )
    assert manager_probe.stdout.strip() == "PASS_FULL_SHA256"
    sentinel = install / "data" / "user-data.sentinel"
    sentinel.write_text("retain\n", encoding="ascii")
    pointer = install / "state" / "current.json"
    before = pointer.read_bytes()
    failed = run_ps(
        installer,
        "-Action",
        "Upgrade",
        "-InstallRoot",
        install,
        "-PayloadManifestPath",
        package2,
        "-FaultInjection",
        "after_stage",
        expect=1,
    )
    assert "FAULT_INJECTION: after_stage" in failed.stderr
    assert pointer.read_bytes() == before
    failed_launcher_copy = run_ps(
        installer,
        "-Action",
        "Upgrade",
        "-InstallRoot",
        install,
        "-PayloadManifestPath",
        package2,
        "-FaultInjection",
        "launcher_copy",
        expect=1,
    )
    assert "FAULT_INJECTION: launcher_copy" in failed_launcher_copy.stderr
    assert pointer.read_bytes() == before
    for name in stable_names:
        assert (install / "bin" / name).read_bytes() == (package1.parent / name).read_bytes()
    assert stable_launcher.read_bytes() == (WINDOWS / "semantic-memory-launcher.ps1").read_bytes()
    stable_before_switch = {
        name: (os.stat(install / "bin" / name).st_ino, (install / "bin" / name).read_bytes())
        for name in stable_names
    }
    stable_launcher_before_switch = (
        os.stat(stable_launcher).st_ino,
        stable_launcher.read_bytes(),
    )
    failed_before_switch = run_ps(
        installer,
        "-Action", "Upgrade",
        "-InstallRoot", install,
        "-PayloadManifestPath", package2,
        "-FaultInjection", "before_switch",
        expect=1,
    )
    assert "FAULT_INJECTION: before_switch" in failed_before_switch.stderr
    assert pointer.read_bytes() == before
    for name, (file_identity, content) in stable_before_switch.items():
        assert os.stat(install / "bin" / name).st_ino == file_identity
        assert (install / "bin" / name).read_bytes() == content
    assert (os.stat(stable_launcher).st_ino, stable_launcher.read_bytes()) == stable_launcher_before_switch
    upgraded = run_ps(installer, "-Action", "Upgrade", "-InstallRoot", install, "-PayloadManifestPath", package2)
    assert json.loads(upgraded.stdout)["current_pointer_switched"] is True
    assert json.loads(pointer.read_text(encoding="utf-8"))["version_id"] == "v1.1.0+bbb222"
    for name in stable_names:
        assert (install / "bin" / name).read_bytes() == (package2.parent / name).read_bytes()
        assert os.stat(install / "bin" / name).st_ino == os.stat(
            install / "app" / "versions" / "v1.1.0+bbb222" / name
        ).st_ino
    assert stable_launcher.read_bytes() == (WINDOWS / "semantic-memory-launcher.ps1").read_bytes()
    preserved = list((install / "state" / "stable-transactions").glob("*/old/*.exe"))
    assert {path.name for path in preserved} >= set(stable_names)
    assert list((install / "state" / "stable-transactions").glob("*/old/semantic-memory-launcher.ps1"))
    run_ps(installer, "-Action", "Verify", "-InstallRoot", install)
    stable_hook = install / "bin" / "semantic-memory-hook.exe"
    stable_replacement = stable_hook.with_name("semantic-memory-hook.tampered")
    stable_replacement.write_text("tampered\n", encoding="ascii")
    os.replace(stable_replacement, stable_hook)
    entrypoint_failure = run_ps(installer, "-Action", "Verify", "-InstallRoot", install, expect=1)
    assert "RED_PAYLOAD_INTEGRITY_FAILURE" in entrypoint_failure.stderr
    run_ps(installer, "-Action", "Repair", "-InstallRoot", install)
    run_ps(installer, "-Action", "Verify", "-InstallRoot", install)
    assert stable_hook.read_bytes() == (package2.parent / "semantic-memory-hook.exe").read_bytes()
    assert os.stat(stable_hook).st_ino == os.stat(
        install / "app" / "versions" / "v1.1.0+bbb222" / "semantic-memory-hook.exe"
    ).st_ino
    launcher_replacement = stable_launcher.with_name("semantic-memory-launcher.tampered")
    launcher_replacement.write_text("tampered\n", encoding="ascii")
    os.replace(launcher_replacement, stable_launcher)
    launcher_failure = run_ps(installer, "-Action", "Verify", "-InstallRoot", install, expect=1)
    assert "RED_PAYLOAD_INTEGRITY_FAILURE" in launcher_failure.stderr
    run_ps(installer, "-Action", "Repair", "-InstallRoot", install)
    run_ps(installer, "-Action", "Verify", "-InstallRoot", install)
    assert stable_launcher.read_bytes() == (WINDOWS / "semantic-memory-launcher.ps1").read_bytes()
    manager_probe = run_ps(
        PLUGIN / "scripts" / "semantic-memory-manager.ps1",
        "-VerifyOnly",
        environment=manager_environment,
    )
    assert manager_probe.stdout.strip() == "PASS_FULL_SHA256"
    uninstall = run_ps(installer, "-Action", "Uninstall", "-InstallRoot", install)
    result = json.loads(uninstall.stdout)
    assert result["data_dir_exists"] is True and result["data_retained_by_default"] is True
    assert sentinel.read_text(encoding="ascii") == "retain\n"
    assert not (install / "app").exists() and not (install / "bin").exists()
    assert list((install / "uninstalled").iterdir())


def test_install_rejects_unmanaged_nonempty_roots_without_mutation() -> None:
    root = EVIDENCE_ROOT / "installer-unmanaged-root-stage14-disposable"
    package = make_payload(root / "package", "v1.1.0-rc.1+unmanaged", "clean")
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"
    for label, marker_bytes in (("no-marker", None), ("invalid-marker", b'{"schema":"foreign/v1"}\n')):
        install = root / label
        (install / "bin").mkdir(parents=True)
        (install / "state").mkdir()
        (install / "bin" / "semantic-memory-mcp.exe").write_bytes(b"foreign-bin\x00\xff")
        (install / "state" / "current.json").write_bytes(b"foreign-current\r\n")
        if marker_bytes is not None:
            (install / ".semantic-memory-managed.json").write_bytes(marker_bytes)
        before = file_tree_bytes(install)
        rejected = run_ps(
            installer, "-Action", "Install", "-InstallRoot", install,
            "-PayloadManifestPath", package, expect=1,
        )
        assert "INSTALL_ROOT_NOT_EMPTY_UNMANAGED" in rejected.stderr
        assert file_tree_bytes(install) == before


def test_install_root_reparse_components_fail_closed_without_writes() -> None:
    root = EVIDENCE_ROOT / "installer-reparse-root-stage14-disposable"
    package1 = make_payload(root / "package-v1", "v1.1.0-rc.1+reparse-old", "old")
    package2 = make_payload(root / "package-v2", "v1.1.0-rc.1+reparse-new", "new")
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"

    empty_target = root / "empty-target"
    empty_target.mkdir(parents=True)
    parent_junction = root / "junction-parent"
    make_junction(parent_junction, empty_target)
    empty_before = file_tree_state(empty_target)
    for install_root in (parent_junction, parent_junction / "nonexistent-child"):
        rejected = run_ps(
            installer, "-Action", "Install", "-InstallRoot", install_root,
            "-PayloadManifestPath", package1, expect=1,
        )
        assert "INSTALL_ROOT_REPARSE_POINT" in rejected.stderr
        assert file_tree_state(empty_target) == empty_before

    managed_target = root / "managed-target"
    run_ps(installer, "-Action", "Install", "-InstallRoot", managed_target, "-PayloadManifestPath", package1)
    managed_junction = root / "managed-junction"
    make_junction(managed_junction, managed_target)
    managed_before = file_tree_state(managed_target)
    for action, arguments in (
        ("Upgrade", ("-PayloadManifestPath", package2)),
        ("Repair", ()),
    ):
        rejected = run_ps(
            installer, "-Action", action, "-InstallRoot", managed_junction,
            *arguments, expect=1,
        )
        assert "INSTALL_ROOT_REPARSE_POINT" in rejected.stderr
        assert file_tree_state(managed_target) == managed_before


def test_same_payload_upgrade_replay_is_exactly_zero_write() -> None:
    root = EVIDENCE_ROOT / "installer-exact-replay-stage14-disposable"
    package = make_payload(root / "package", "v1.1.0-rc.1+exact-replay", "same")
    install = root / "install"
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"
    run_ps(installer, "-Action", "Install", "-InstallRoot", install, "-PayloadManifestPath", package)
    pointer = install / "state" / "current.json"
    marker = install / ".semantic-memory-managed.json"
    stable_names = ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe")
    pointer_before = (pointer.read_bytes(), pointer.stat().st_ino)
    marker_before = (marker.read_bytes(), marker.stat().st_ino)
    stable_before = {
        name: ((install / "bin" / name).read_bytes(), (install / "bin" / name).stat().st_ino)
        for name in stable_names
    }
    transaction_root = install / "state" / "stable-transactions"
    transactions_before = sorted(path.name for path in transaction_root.iterdir())
    tree_before = file_tree_state(install)

    replay = run_ps(
        installer, "-Action", "Upgrade", "-InstallRoot", install, "-PayloadManifestPath", package,
    )
    replay_json = json.loads(replay.stdout)
    assert replay_json["status"] == "REPLAYED"
    assert replay_json["current_pointer_switched"] is False
    assert (pointer.read_bytes(), pointer.stat().st_ino) == pointer_before
    assert (marker.read_bytes(), marker.stat().st_ino) == marker_before
    for name, state in stable_before.items():
        stable = install / "bin" / name
        assert (stable.read_bytes(), stable.stat().st_ino) == state
    assert sorted(path.name for path in transaction_root.iterdir()) == transactions_before
    assert file_tree_state(install) == tree_before


def test_marker_write_failure_rolls_back_pointer_bin_and_marker() -> None:
    root = EVIDENCE_ROOT / "installer-marker-rollback-stage14-disposable"
    package1 = make_payload(root / "package-v1", "v1.1.0-rc.1+marker-old", "old")
    package2 = make_payload(root / "package-v2", "v1.1.0-rc.1+marker-new", "new")
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"

    fresh = root / "fresh-failure"
    failed_fresh = run_ps(
        installer, "-Action", "Install", "-InstallRoot", fresh,
        "-PayloadManifestPath", package2, "-FaultInjection", "marker_write", expect=1,
    )
    assert "FAULT_INJECTION: marker_write" in failed_fresh.stderr
    assert (fresh / "app" / "versions" / "v1.1.0-rc.1+marker-new").is_dir()
    assert not (fresh / ".semantic-memory-managed.json").exists()
    assert not (fresh / "state" / "current.json").exists()
    assert not any((fresh / "bin").glob("semantic-memory-*.exe"))
    assert not (fresh / "bin" / "semantic-memory-launcher.ps1").exists()
    assert not any((fresh / "state").glob("install-marker.new-*.json"))
    assert not any((fresh / "state").glob("current.new-*.json"))
    assert not any((fresh / "state").glob("history/current-before-*.json"))
    fresh_transactions = fresh / "state" / "stable-transactions"
    assert not fresh_transactions.exists() or not any(fresh_transactions.iterdir())

    install = root / "upgrade"
    run_ps(installer, "-Action", "Install", "-InstallRoot", install, "-PayloadManifestPath", package1)
    pointer = install / "state" / "current.json"
    marker = install / ".semantic-memory-managed.json"
    stable_names = ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe")
    pointer_before = (pointer.read_bytes(), pointer.stat().st_ino)
    marker_before = (marker.read_bytes(), marker.stat().st_ino)
    stable_before = {
        name: ((install / "bin" / name).read_bytes(), os.stat(install / "bin" / name).st_ino)
        for name in stable_names
    }
    stable_launcher = install / "bin" / "semantic-memory-launcher.ps1"
    stable_launcher_before = (stable_launcher.read_bytes(), os.stat(stable_launcher).st_ino)
    transactions = install / "state" / "stable-transactions"
    transactions_before = sorted(path.name for path in transactions.iterdir())
    entries_before = tree_entries(install)
    files_before = file_tree_state(install)
    failed_upgrade = run_ps(
        installer, "-Action", "Upgrade", "-InstallRoot", install,
        "-PayloadManifestPath", package2, "-FaultInjection", "marker_write", expect=1,
    )
    assert "FAULT_INJECTION: marker_write" in failed_upgrade.stderr
    assert (pointer.read_bytes(), pointer.stat().st_ino) == pointer_before
    assert (marker.read_bytes(), marker.stat().st_ino) == marker_before
    for name, (content, file_identity) in stable_before.items():
        assert (install / "bin" / name).read_bytes() == content
        assert os.stat(install / "bin" / name).st_ino == file_identity
    assert (stable_launcher.read_bytes(), os.stat(stable_launcher).st_ino) == stable_launcher_before
    assert (install / "app" / "versions" / "v1.1.0-rc.1+marker-new").is_dir()
    new_prefix = "app/versions/v1.1.0-rc.1+marker-new"
    assert tree_entries(install) - entries_before == {
        (new_prefix, "dir"),
        (f"{new_prefix}/payload-manifest.json", "file"),
        (f"{new_prefix}/semantic-memory-hook.exe", "file"),
        (f"{new_prefix}/semantic-memory-manager.exe", "file"),
        (f"{new_prefix}/semantic-memory-mcp.exe", "file"),
        (f"{new_prefix}/verification-receipt.json", "file"),
    }
    files_after_upgrade = file_tree_state(install)
    assert {
        path: state for path, state in files_after_upgrade.items() if not path.startswith(f"{new_prefix}/")
    } == files_before
    assert sorted(path.name for path in transactions.iterdir()) == transactions_before
    assert not any((install / "state").glob("install-marker.new-*.json"))
    assert not any((install / "state").glob("current.new-*.json"))
    assert not any((install / "state").glob("history/current-before-*.json"))
    run_ps(installer, "-Action", "Verify", "-InstallRoot", install)

    repair_pointer_before = (pointer.read_bytes(), pointer.stat().st_ino)
    repair_marker_before = (marker.read_bytes(), marker.stat().st_ino)
    repair_stable_before = {
        name: ((install / "bin" / name).read_bytes(), os.stat(install / "bin" / name).st_ino)
        for name in stable_names
    }
    repair_launcher_before = (stable_launcher.read_bytes(), os.stat(stable_launcher).st_ino)
    repair_transactions_before = sorted(path.name for path in transactions.iterdir())
    repair_entries_before = tree_entries(install)
    repair_files_before = file_tree_state(install)
    failed_repair = run_ps(
        installer, "-Action", "Repair", "-InstallRoot", install,
        "-FaultInjection", "marker_write", expect=1,
    )
    assert "FAULT_INJECTION: marker_write" in failed_repair.stderr
    assert (pointer.read_bytes(), pointer.stat().st_ino) == repair_pointer_before
    assert (marker.read_bytes(), marker.stat().st_ino) == repair_marker_before
    for name, (content, file_identity) in repair_stable_before.items():
        assert (install / "bin" / name).read_bytes() == content
        assert os.stat(install / "bin" / name).st_ino == file_identity
    assert (stable_launcher.read_bytes(), os.stat(stable_launcher).st_ino) == repair_launcher_before
    assert tree_entries(install) == repair_entries_before
    assert file_tree_state(install) == repair_files_before
    assert sorted(path.name for path in transactions.iterdir()) == repair_transactions_before
    assert not any((install / "state").glob("install-marker.new-*.json"))
    assert not any((install / "state").glob("current.new-*.json"))
    assert not any((install / "state").glob("history/current-before-*.json"))
    run_ps(installer, "-Action", "Verify", "-InstallRoot", install)


def test_payload_tamper_fails_closed_without_current_switch() -> None:
    root = EVIDENCE_ROOT / "tamper-stage14-disposable"
    package = make_payload(root / "package", "v1.1.0-rc.1+tamper", "clean")
    install = root / "install"
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"
    run_ps(installer, "-Action", "Install", "-InstallRoot", install, "-PayloadManifestPath", package)
    pointer = install / "state" / "current.json"
    before = pointer.read_bytes()
    payload = install / "app" / "versions" / "v1.1.0-rc.1+tamper" / "semantic-memory-mcp.exe"
    original_stat = payload.stat()
    tampered = bytearray(payload.read_bytes())
    tampered[0] ^= 1
    payload.write_bytes(tampered)
    os.utime(payload, ns=(original_stat.st_atime_ns, original_stat.st_mtime_ns))
    assert payload.stat().st_size == original_stat.st_size
    assert payload.stat().st_mtime_ns == original_stat.st_mtime_ns
    failed = run_ps(installer, "-Action", "Verify", "-InstallRoot", install, expect=1)
    assert "RED_PAYLOAD_INTEGRITY_FAILURE" in failed.stderr
    assert pointer.read_bytes() == before


def test_native_stable_entrypoints_integrity_actions_timeout_and_performance() -> None:
    root = EVIDENCE_ROOT / "native-entrypoints-stage14-disposable"
    package = make_native_payload(root / "package", "v1.1.0-rc.1+native")
    install = root / "local-app-data" / "SemanticMemory"
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"
    run_ps(installer, "-Action", "Install", "-InstallRoot", install, "-PayloadManifestPath", package)
    stable = install / "bin"
    mcp = stable / "semantic-memory-mcp.exe"
    hook = stable / "semantic-memory-hook.exe"
    manager = stable / "semantic-memory-manager.exe"
    launcher = stable / "semantic-memory-launcher.ps1"
    installed_role_paths = [
        payload_path
        for name in ("semantic-memory-mcp.exe", "semantic-memory-hook.exe", "semantic-memory-manager.exe")
        for payload_path in (install / "app" / "versions" / "v1.1.0-rc.1+native" / name, stable / name)
    ]
    file_identities = {os.stat(path).st_ino for path in installed_role_paths}
    assert 0 not in file_identities
    assert len(file_identities) == 1, file_identities
    for executable, args in (
        (mcp, ("--verify-only",)),
        (hook, ("recall", "--verify-only")),
        (hook, ("post-tool", "--verify-only")),
        (hook, ("stop", "--verify-only")),
        (manager, ("--verify-only",)),
    ):
        verified = run_native(executable, *args)
        assert verified.stdout == "PASS_FULL_SHA256\n"
    assert launcher.read_bytes() == (WINDOWS / "semantic-memory-launcher.ps1").read_bytes()
    manager_environment = os.environ.copy()
    manager_environment["SEMANTIC_MEMORY_HOME"] = str(install)
    manager_probe = run_ps(
        PLUGIN / "scripts" / "semantic-memory-manager.ps1",
        "-VerifyOnly",
        environment=manager_environment,
    )
    assert manager_probe.stdout.strip() == "PASS_FULL_SHA256"

    durations = []
    for _ in range(12):
        started = time.perf_counter()
        verified = run_native(mcp, "--verify-only", timeout=2.0)
        durations.append((time.perf_counter() - started) * 1000)
        assert verified.stdout == "PASS_FULL_SHA256\n"
    ordered = sorted(durations)
    p95 = ordered[((95 * len(ordered) + 99) // 100) - 1]
    performance = {
        "schema": "stage14-native-entrypoint-performance/v1",
        "candidate_path": str(REAL_CANDIDATE),
        "candidate_sha256": sha256(REAL_CANDIDATE),
        "process_durations_ms": durations,
        "process_hard_timeout_ms": 2000,
        "new_process_iterations": len(durations),
        "new_process_p95_nearest_rank_ms": p95,
        "new_process_max_ms": max(durations),
        "validation": "native stable mcp entrypoint full manifest-file SHA256",
    }
    write_json(EVIDENCE_ROOT.parent / "native-entrypoint-performance.json", performance)
    assert p95 <= 500, durations

    pointer = install / "state" / "current.json"
    payload = install / "app" / "versions" / "v1.1.0-rc.1+native"
    manifest = payload / "payload-manifest.json"
    receipt = payload / "verification-receipt.json"

    unicode_argv_value = str(root / "argv probe 中文 100% #.sqlite3")
    assert all(marker in unicode_argv_value for marker in (" ", "中文", "%", "#"))
    unicode_argv = ("global-migrate", "--source-memory", unicode_argv_value)
    stable_probe = run_native(mcp, *unicode_argv, expect=1, timeout=5.0)
    probe_diagnostic = json.dumps(
        {
            "argv": unicode_argv,
            "returncode": stable_probe.returncode,
            "stdout": stable_probe.stdout,
            "stderr": stable_probe.stderr,
        },
        ensure_ascii=False,
    )
    assert "RED_PAYLOAD_INTEGRITY_FAILURE" not in stable_probe.stderr, probe_diagnostic
    assert stable_probe.returncode == 2, probe_diagnostic
    assert (
        "global-migrate: all frozen contract arguments are required" in stable_probe.stderr
    ), probe_diagnostic

    def assert_red(executable: Path = mcp, *args: str) -> None:
        failed = run_native(executable, *(args or ("--verify-only",)), expect=1)
        assert "RED_PAYLOAD_INTEGRITY_FAILURE" in failed.stderr

    pointer_bytes = pointer.read_bytes()
    pointer_value = json.loads(pointer_bytes)
    pointer_value["schema"] = "tampered-current-pointer/v1"
    write_json(pointer, pointer_value)
    assert_red()
    pointer.write_bytes(pointer_bytes)
    run_native(mcp, "--verify-only")

    manifest_bytes = manifest.read_bytes()
    manifest.write_bytes(manifest_bytes + b" ")
    assert_red()
    manifest.write_bytes(manifest_bytes)
    run_native(mcp, "--verify-only")

    receipt_bytes = receipt.read_bytes()
    receipt.write_bytes(receipt_bytes + b" ")
    assert_red()
    receipt.write_bytes(receipt_bytes)
    run_native(mcp, "--verify-only")

    payload_file = payload / "semantic-memory-manager.exe"
    tampered_payload = payload / ".semantic-memory-manager.tampered"
    shutil.copy2(payload_file, tampered_payload)
    with tampered_payload.open("r+b") as stream:
        original = stream.read(1)
        stream.seek(0)
        stream.write(bytes([original[0] ^ 1]))
    os.replace(tampered_payload, payload_file)
    assert_red()
    restored_payload = payload / ".semantic-memory-manager.restored"
    os.link(payload / "semantic-memory-mcp.exe", restored_payload)
    os.replace(restored_payload, payload_file)
    assert os.stat(payload_file).st_ino == os.stat(payload / "semantic-memory-mcp.exe").st_ino
    run_native(mcp, "--verify-only")

    receipt_value = json.loads(receipt_bytes)
    receipt_value["files"] = receipt_value["files"][:-1]
    write_json(receipt, receipt_value)
    pointer_value = json.loads(pointer_bytes)
    pointer_value["receipt_sha256"] = sha256(receipt)
    write_json(pointer, pointer_value)
    assert_red()
    receipt.write_bytes(receipt_bytes)
    pointer.write_bytes(pointer_bytes)
    run_native(mcp, "--verify-only")

    manifest_value = json.loads(manifest_bytes)
    manifest_value["entrypoints"]["mcp"] = "semantic-memory-hook.exe"
    write_json(manifest, manifest_value)
    receipt_value = json.loads(receipt_bytes)
    receipt_value["manifest_sha256"] = sha256(manifest)
    write_json(receipt, receipt_value)
    pointer_value = json.loads(pointer_bytes)
    pointer_value["manifest_sha256"] = sha256(manifest)
    pointer_value["receipt_sha256"] = sha256(receipt)
    write_json(pointer, pointer_value)
    assert_red()
    manifest.write_bytes(manifest_bytes)
    receipt.write_bytes(receipt_bytes)
    pointer.write_bytes(pointer_bytes)
    run_native(mcp, "--verify-only")

    assert_red(hook, "invalid-action", "--verify-only")
    for action in ("recall", "post-tool", "stop"):
        run_native(hook, action, "--verify-only")

    timeout_environment = os.environ.copy()
    timeout_environment["CBM_MEMORY_AUTO_MAINTAIN"] = "1"
    started = time.perf_counter()
    blocked = subprocess.Popen(
        [str(hook), "recall"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=timeout_environment,
    )
    try:
        blocked.wait(timeout=2.5)
    except subprocess.TimeoutExpired:
        blocked.kill()
        blocked.wait(timeout=5)
        raise AssertionError("native hook launcher exceeded its own 2000ms hard timeout")
    elapsed_ms = (time.perf_counter() - started) * 1000
    if blocked.stdin:
        blocked.stdin.close()
        blocked.stdin = None
    stdout, stderr = blocked.communicate(timeout=5)
    assert blocked.returncode != 0
    assert elapsed_ms <= 2500, elapsed_ms
    assert b"STAGE14_HOOK_TIMEOUT" in stderr
    write_json(EVIDENCE_ROOT.parent / "native-hook-hard-timeout.json", {
        "schema": "stage14-native-hook-hard-timeout/v1",
        "configured_timeout_ms": 2000,
        "observed_process_ms": elapsed_ms,
        "external_auto_maintain": "1",
        "returncode": blocked.returncode,
        "fail_closed": True,
    })


def test_native_source_forces_safe_auto_maintain_and_internal_hook_timeout() -> None:
    source = (ROOT / "src" / "main.c").read_text(encoding="utf-8")
    environment_start = source.index("static bool stage14_set_runtime_environment")
    environment_end = source.index("static int stage14_launch_payload", environment_start)
    environment_body = source[environment_start:environment_end]
    assert 'SetEnvironmentVariableW(L"CBM_MEMORY_AUTO_MAINTAIN", L"0")' in environment_body
    assert 'GetEnvironmentVariableW(L"CBM_MEMORY_AUTO_MAINTAIN"' not in environment_body
    launch_start = environment_end
    launch_end = source.index("static int stage14_stable_launcher", launch_start)
    launch_body = source[launch_start:launch_end]
    assert "WAIT_TIMEOUT" in launch_body
    assert "INFINITE" not in launch_body
    stable_end = source.index("#endif", launch_end)
    stable_body = source[launch_end:stable_end]
    assert "role == STAGE14_ROLE_HOOK ? 2000 : INFINITE" in stable_body


def make_db(path: Path, label: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(path)
    try:
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("CREATE TABLE sample(id TEXT PRIMARY KEY, value TEXT NOT NULL)")
        db.execute("INSERT INTO sample VALUES(?,?)", (f"{label}-1", label))
        db.commit()
    finally:
        db.close()


def sqlite_projection(path: Path) -> dict[str, object]:
    uri = path.resolve().as_uri()
    separator = "&" if "?" in uri else "?"
    db = sqlite3.connect(f"{uri}{separator}mode=ro&immutable=1", uri=True)
    try:
        quick_check = db.execute("PRAGMA quick_check").fetchone()[0]
        foreign_key_violations = len(db.execute("PRAGMA foreign_key_check").fetchall())
        schemas = {
            row[0]: row[1]
            for row in db.execute(
                "SELECT name, coalesce(sql, '') FROM sqlite_master "
                "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
            )
        }
        rows = {}
        for table in schemas:
            quoted = '"' + table.replace('"', '""') + '"'
            encoded = [
                json.dumps(row, ensure_ascii=False, separators=(",", ":"), default=str)
                for row in db.execute(f"SELECT * FROM {quoted}").fetchall()
            ]
            rows[table] = sorted(encoded)
        return {
            "quick_check": quick_check,
            "foreign_key_violations": foreign_key_violations,
            "schemas": schemas,
            "rows": rows,
        }
    finally:
        db.close()


def assert_source_projection(source: Path, target: Path) -> None:
    source_state = sqlite_projection(source)
    target_state = sqlite_projection(target)
    assert source_state["quick_check"] == target_state["quick_check"] == "ok"
    assert source_state["foreign_key_violations"] == target_state["foreign_key_violations"] == 0
    for table, schema in source_state["schemas"].items():
        assert target_state["schemas"][table] == schema
        assert target_state["rows"][table] == source_state["rows"][table]


def authorize(
    root: Path, memory: Path, graph: Path, config: Path, project: Path, nonce: str,
) -> tuple[Path, str]:
    manifest = root / f"authorization-{nonce}.json"
    payload = {
        "schema": "stage14-windows-migration-authorization/v2",
        "nonce": nonce,
        "project_path": str(project.resolve()),
        "source_memory_sha256": sha256(memory),
        "source_graph_sha256": sha256(graph),
        "source_config_sha256": sha256(config),
        "migration_executable_sha256": sha256(REAL_CANDIDATE),
        "max_backup_age_minutes": 30,
        "minimum_free_bytes": 64 * 1024 * 1024,
        "expires_at": "2099-01-01T00:00:00Z",
    }
    write_json(manifest, payload)
    digest = sha256(manifest)
    write_json(root / ".stage14-disposable-marker.json", {
        "schema": "stage14-disposable-marker/v1", "nonce": nonce,
        "authorization_manifest_sha256": digest,
    })
    return manifest, digest


def migration_args(root: Path, manifest: Path, digest: str, nonce: str) -> list[str]:
    return ["-DisposableRoot", root, "-AuthorizationManifestPath", manifest,
            "-AuthorizationManifestSha256", digest, "-AuthorizationNonce", nonce]


def authorize_production(
    root: Path, memory: Path, graph: Path, config: Path, project: Path,
    target: Path, report: Path, nonce: str, migration_executable: Path = REAL_CANDIDATE,
) -> tuple[Path, str]:
    manifest = root / f"authorization-{nonce}.json"
    payload = {
        "schema": "stage14-windows-production-migration-authorization/v2",
        "nonce": nonce,
        "disposable_root": str(root.resolve()),
        "project_path": str(project.resolve()),
        "source_memory_path": str(memory.resolve()),
        "source_graph_path": str(graph.resolve()),
        "source_config_path": str(config.resolve()),
        "source_memory_sha256": sha256(memory),
        "source_graph_sha256": sha256(graph),
        "source_config_sha256": sha256(config),
        "target_root": str(target.resolve()),
        "report_path": str(report.resolve()),
        "migration_executable_path": str(migration_executable.resolve()),
        "migration_executable_sha256": sha256(migration_executable),
        "max_backup_age_minutes": 30,
        "minimum_free_bytes": 64 * 1024 * 1024,
        "expires_at": "2099-01-01T00:00:00Z",
    }
    write_json(manifest, payload)
    digest = sha256(manifest)
    write_json(root / ".stage14-disposable-marker.json", {
        "schema": "stage14-disposable-marker/v1", "nonce": nonce,
        "authorization_manifest_sha256": digest,
    })
    return manifest, digest


def test_global_migrate_report_reservation_and_containment_fail_closed() -> None:
    root = EVIDENCE_ROOT / "global migrate report 中文 100% # stage14-disposable"
    root.mkdir(parents=True)
    memory = root / "source-memory.sqlite3"
    graph = root / "source-graph.sqlite3"
    config = root / "source-config.sqlite3"
    make_db(memory, "memory")
    make_db(graph, "graph")
    make_db(config, "config")
    source_paths = (memory, graph, config)
    source_before = exact_file_states(source_paths)
    project = ROOT.parents[1]
    target = root / "ContainmentTarget"
    target.mkdir()

    def migration_args(
        target_arg: str, report_arg: str, key: str, mode: str = "apply",
    ) -> tuple[str, ...]:
        return (
            "global-migrate",
            "--source-memory", str(memory),
            "--source-graph", str(graph),
            "--source-config", str(config),
            "--target-root", target_arg,
            "--project-path", str(project),
            "--idempotency-key", key,
            "--mode", mode,
            "--report", report_arg,
        )

    def assert_rejected_without_mutation(
        label: str, target_arg: str, report_arg: str, report_file: Path,
    ) -> None:
        target_before = exact_tree_state(target)
        result = run_native(
            REAL_CANDIDATE,
            *migration_args(target_arg, report_arg, f"reject-{label}"),
            expect=1,
            timeout=30.0,
        )
        assert result.returncode == 2, result.stdout + result.stderr
        assert "RED_PAYLOAD_INTEGRITY_FAILURE" not in result.stderr
        assert not report_file.exists()
        assert exact_file_states(source_paths) == source_before
        assert not sqlite_path_sidecars(source_paths)
        assert exact_tree_state(target) == target_before

    direct = target / "direct.json"
    assert_rejected_without_mutation("direct", str(target), str(direct), direct)

    mixed = target / "mixed.json"
    assert_rejected_without_mutation(
        "mixed",
        str(target).replace("\\", "/") + "/",
        str(mixed).replace("\\", "/"),
        mixed,
    )

    dotted = target / "dotted.json"
    dotted_argument = str(target / ".." / target.name / dotted.name)
    assert_rejected_without_mutation("dotted", str(target), dotted_argument, dotted)

    case_variant = target / "case-variant.json"
    case_argument = str(target.parent / target.name.lower() / case_variant.name)
    assert_rejected_without_mutation("case", str(target), case_argument, case_variant)

    report_alias = root / "report-target-junction"
    make_junction(report_alias, target)
    junction_report = target / "junction-report.json"
    assert_rejected_without_mutation(
        "report-junction",
        str(target),
        str(report_alias / junction_report.name),
        junction_report,
    )

    actual_target = root / "actual-target"
    actual_target.mkdir()
    target_alias = root / "target-root-junction"
    make_junction(target_alias, actual_target)
    target_alias_report = actual_target / "target-alias-report.json"
    target_before = exact_tree_state(actual_target)
    target_alias_result = run_native(
        REAL_CANDIDATE,
        *migration_args(str(target_alias), str(target_alias_report), "reject-target-junction"),
        expect=1,
        timeout=30.0,
    )
    assert target_alias_result.returncode == 2
    assert not target_alias_report.exists()
    assert exact_tree_state(actual_target) == target_before
    assert exact_file_states(source_paths) == source_before

    missing_parent_report = root / "missing-parent" / "report.json"
    target_before = exact_tree_state(target)
    missing_parent = run_native(
        REAL_CANDIDATE,
        *migration_args(str(target), str(missing_parent_report), "missing-parent"),
        expect=1,
        timeout=30.0,
    )
    assert missing_parent.returncode == 1
    assert not missing_parent_report.exists()
    assert not missing_parent_report.parent.exists()
    assert exact_tree_state(target) == target_before
    assert exact_file_states(source_paths) == source_before

    relative_report_name = "relative-stage14-report.json"
    relative_report = root / relative_report_name
    target_before = exact_tree_state(target)
    relative = run_native(
        REAL_CANDIDATE,
        *migration_args(str(target), relative_report_name, "relative-report"),
        expect=1,
        timeout=30.0,
        cwd=root,
    )
    assert relative.returncode == 2
    assert not relative_report.exists()
    assert exact_tree_state(target) == target_before
    assert exact_file_states(source_paths) == source_before

    sentinel_report = root / "preexisting-report.json"
    sentinel_bytes = b"DO-NOT-OVERWRITE\n"
    sentinel_report.write_bytes(sentinel_bytes)
    sentinel_state = (
        sentinel_report.read_bytes(),
        sentinel_report.stat().st_ino,
        sentinel_report.stat().st_mtime_ns,
    )
    target_before = exact_tree_state(target)
    preexisting = run_native(
        REAL_CANDIDATE,
        *migration_args(str(target), str(sentinel_report), "preexisting-report"),
        expect=1,
        timeout=30.0,
    )
    assert preexisting.returncode == 2
    assert (
        sentinel_report.read_bytes(),
        sentinel_report.stat().st_ino,
        sentinel_report.stat().st_mtime_ns,
    ) == sentinel_state
    assert exact_tree_state(target) == target_before
    assert exact_file_states(source_paths) == source_before

    sibling_report = root / "ContainmentTarget-sibling-report.json"
    missing_target = root / "new-target-does-not-exist"
    planned = run_native(
        REAL_CANDIDATE,
        *migration_args(str(missing_target), str(sibling_report), "outside-sibling", "plan"),
        timeout=30.0,
    )
    assert json.loads(sibling_report.read_text(encoding="utf-8"))["status"] == "planned"
    assert not missing_target.exists()
    assert exact_file_states(source_paths) == source_before
    assert not sqlite_path_sidecars(source_paths)
    assert planned.returncode == 0

    race_report = root / "exclusive-race-report.json"
    race_command = [
        str(REAL_CANDIDATE),
        *migration_args(str(missing_target), str(race_report), "exclusive-race", "plan"),
    ]
    first = subprocess.Popen(race_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    second = subprocess.Popen(race_command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        first_stdout, first_stderr = first.communicate(timeout=30.0)
        second_stdout, second_stderr = second.communicate(timeout=30.0)
    finally:
        for process in (first, second):
            if process.poll() is None:
                process.kill()
                process.communicate()
    diagnostics = {
        "first": {
            "returncode": first.returncode,
            "stdout": decode_process_stream(first_stdout),
            "stderr": decode_process_stream(first_stderr),
        },
        "second": {
            "returncode": second.returncode,
            "stdout": decode_process_stream(second_stdout),
            "stderr": decode_process_stream(second_stderr),
        },
    }
    assert sorted((first.returncode, second.returncode)) == [0, 2], diagnostics
    assert json.loads(race_report.read_text(encoding="utf-8"))["status"] == "planned"
    assert exact_file_states(source_paths) == source_before
    assert not sqlite_path_sidecars(source_paths)
    assert not missing_target.exists()


def test_authorized_cross_drive_production_migration_and_escape_rejection() -> None:
    root = EVIDENCE_ROOT / "production migration 中文 100% # stage14-disposable"
    root.mkdir(parents=True)
    memory = root / "source-memory.sqlite3"
    graph = root / "source-graph.sqlite3"
    config = root / "source-config.sqlite3"
    make_db(memory, "memory")
    make_db(graph, "graph")
    make_db(config, "config")
    source_hashes = (sha256(memory), sha256(graph), sha256(config))
    project = ROOT.parents[1]
    local_app_data = Path(os.environ["TEMP"]) / f"stage14-agent-b-{EVIDENCE_ROOT.parent.name}" / "LocalAppData"
    install_root = local_app_data / "SemanticMemory"
    package = make_native_payload(root / "native-package", "v1.1.0-rc.1+install-migrate")
    installer = WINDOWS / "Install-SemanticMemoryV2.ps1"
    run_ps(installer, "-Action", "Install", "-InstallRoot", install_root, "-PayloadManifestPath", package)
    target = install_root / "data"
    assert tree_entries(target) == {("projects", "dir"), ("artifacts", "dir")}
    pointer = install_root / "state" / "current.json"
    pointer_before = pointer.read_bytes()
    migration_executable = install_root / "bin" / "semantic-memory-mcp.exe"
    report = root / "production-apply-report.json"
    auth, auth_hash = authorize_production(
        root, memory, graph, config, project, target, report, "nonce-production-apply",
        migration_executable,
    )
    environment = os.environ.copy()
    environment["LOCALAPPDATA"] = str(local_app_data)
    no_python_path = root / "no-python-path"
    no_python_path.mkdir()
    environment["PATH"] = os.pathsep.join(
        (str(no_python_path), str(Path(os.environ["SystemRoot"]) / "System32"))
    )
    assert shutil.which("python.exe", path=environment["PATH"]) is None
    assert memory.drive.lower() != target.drive.lower()
    invoke = WINDOWS / "Invoke-Stage14Migration.ps1"
    applied = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", target, "-ProjectPath", project, "-IdempotencyKey", "production-key",
        "-MigrationExecutable", migration_executable, "-ReportPath", report, "-ProductionAuthorized",
        "-AllowExistingManagedTarget",
        *migration_args(root, auth, auth_hash, "nonce-production-apply"), environment=environment,
    )
    applied_json = json.loads(applied.stdout)
    assert applied_json["core_status"] == "applied"
    assert applied_json["backup"]["schema"] == "stage14-authorized-source-backups/v2"
    assert applied_json["backup"]["core_source_checks"]["quick_check"] == "ok"
    assert applied_json["source_read_only"] is True
    assert applied_json["current_pointer_unchanged"] is True
    core_report = json.loads(report.read_text(encoding="utf-8"))
    project_uuid = core_report["project_uuid"]
    assert core_report["source_to_target_mapping"] == {
        "memory": "__global__-memory.db",
        "config": "_config.db",
        "graph": f"projects/{project_uuid}/graph.db",
        "global_graph": "__global__-graph.db",
    }
    for expected in (
        target / "_config.db",
        target / "__global__-memory.db",
        target / "__global__-graph.db",
        target / "projects" / project_uuid / "graph.db",
    ):
        assert expected.is_file()
    source_paths = (memory, graph, config)
    sources_before_replay = exact_file_states(source_paths)
    assert not sqlite_path_sidecars(source_paths)
    target_before_replay = exact_tree_state(target)
    assert not sqlite_transient_sidecars(target)
    assert_source_projection(memory, target / "__global__-memory.db")
    assert_source_projection(config, target / "_config.db")
    assert_source_projection(graph, target / "projects" / project_uuid / "graph.db")
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert exact_tree_state(target) == target_before_replay
    assert not (target / f"{project_uuid}-graph.db").exists()
    assert not (target / "cache").exists()
    assert source_hashes == (sha256(memory), sha256(graph), sha256(config))
    assert pointer.read_bytes() == pointer_before

    replay_report = root / "production-replay-report.json"
    replay_auth, replay_hash = authorize_production(
        root, memory, graph, config, project, target, replay_report, "nonce-production-replay",
        migration_executable,
    )
    replayed = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", target, "-ProjectPath", project, "-IdempotencyKey", "production-key",
        "-MigrationExecutable", migration_executable, "-ReportPath", replay_report,
        "-ProductionAuthorized", "-AllowExistingManagedTarget",
        *migration_args(root, replay_auth, replay_hash, "nonce-production-replay"),
        environment=environment,
    )
    assert json.loads(replayed.stdout)["core_status"] == "replayed"
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert not sqlite_transient_sidecars(target)
    assert exact_tree_state(target) == target_before_replay
    assert_source_projection(memory, target / "__global__-memory.db")
    assert_source_projection(config, target / "_config.db")
    assert_source_projection(graph, target / "projects" / project_uuid / "graph.db")
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert exact_tree_state(target) == target_before_replay
    assert pointer.read_bytes() == pointer_before

    verify_report = root / "production-managed-verify-report.json"
    verify_auth, verify_hash = authorize_production(
        root, memory, graph, config, project, target, verify_report, "nonce-production-verify",
        migration_executable,
    )
    verify_source_before = exact_file_states(source_paths)
    verify_target_before = exact_tree_state(target)
    verified = run_ps(
        invoke, "-Action", "VerifyManaged", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config, "-TargetRoot", target, "-ProjectPath", project,
        "-IdempotencyKey", "production-key", "-MigrationExecutable", migration_executable,
        "-ReportPath", verify_report, "-ProductionAuthorized", "-AllowExistingManagedTarget",
        *migration_args(root, verify_auth, verify_hash, "nonce-production-verify"),
        environment=environment,
    )
    verified_json = json.loads(verified.stdout)
    assert verified_json["action"] == "VerifyManaged"
    assert verified_json["core_status"] == "verified"
    assert verified_json["source_read_only"] is True
    assert verified_json["current_pointer_unchanged"] is True
    verify_core = json.loads(verify_report.read_text(encoding="utf-8"))
    assert verify_core["database_write_performed"] is False
    assert verify_core["current_pointer_switched"] is False
    assert verify_core["comparisons"] == {
        "source_payload_match": True,
        "source_logical_match": True,
        "target_logical_match": True,
    }
    assert exact_file_states(source_paths) == verify_source_before
    assert exact_tree_state(target) == verify_target_before
    assert not sqlite_path_sidecars(source_paths)
    assert not sqlite_transient_sidecars(target)
    assert pointer.read_bytes() == pointer_before

    escaped_target = local_app_data / "SemanticMemory" / "data-prefix-confusion"
    rejected_report = root / "production-rejected-report.json"
    rejected_auth, rejected_hash = authorize_production(
        root, memory, graph, config, project, escaped_target, rejected_report, "nonce-production-reject",
        migration_executable,
    )
    rejected = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", escaped_target, "-ProjectPath", project, "-IdempotencyKey", "reject-key",
        "-MigrationExecutable", migration_executable, "-ReportPath", rejected_report,
        "-ProductionAuthorized", *migration_args(
            root, rejected_auth, rejected_hash, "nonce-production-reject",
        ), expect=1, environment=environment,
    )
    assert "exact authorized Semantic Memory data root" in rejected.stderr
    assert not escaped_target.exists() and not rejected_report.exists()
    assert source_hashes == (sha256(memory), sha256(graph), sha256(config))
    assert pointer.read_bytes() == pointer_before


def test_bootstrap_target_adoption_rev9() -> None:
    """A legal empty bootstrap target is adopted exactly once during Apply."""
    fixture_root_value = os.environ.get("STAGE14_BOOTSTRAP_FIXTURE_ROOT")
    assert fixture_root_value, "STAGE14_BOOTSTRAP_FIXTURE_ROOT must bind the fresh current-target backup fixture"
    fixture_root = Path(fixture_root_value).resolve()
    source_root = EVIDENCE_ROOT / "bootstrap-adoption-red"
    source_root.mkdir(parents=True)
    source_paths = {
        name: fixture_root / f"source-{name}.db"
        for name in ("memory", "graph", "config")
    }
    current_paths = {
        name: fixture_root / f"current-{name}.db"
        for name in ("memory", "graph", "config")
    }
    assert all(path.is_file() for path in (*source_paths.values(), *current_paths.values()))
    memory = source_root / "source-memory.db"
    graph = source_root / "source-graph.db"
    config = source_root / "source-config.db"
    for name, target in (("memory", memory), ("graph", graph), ("config", config)):
        shutil.copy2(source_paths[name], target)
    target = source_root / "bootstrap-target"
    (target / "projects").mkdir(parents=True)
    (target / "artifacts").mkdir()
    shutil.copy2(current_paths["memory"], target / "__global__-memory.db")
    shutil.copy2(current_paths["config"], target / "_config.db")
    shutil.copy2(current_paths["graph"], target / "__global__-graph.db")
    project = ROOT.parents[1]
    auth, auth_hash = authorize(source_root, memory, graph, config, project, "nonce-bootstrap-red")
    invoke = WINDOWS / "Invoke-Stage14Migration.ps1"
    before = exact_tree_state(target)
    plan_report = source_root / "plan-report.json"
    planned = run_ps(
        invoke, "-Action", "Plan", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config, "-TargetRoot", target, "-ProjectPath", project,
        "-IdempotencyKey", "bootstrap-red-key", "-MigrationExecutable", REAL_CANDIDATE,
        "-ReportPath", plan_report, *migration_args(source_root, auth, auth_hash, "nonce-bootstrap-red"),
    )
    assert json.loads(planned.stdout)["core_status"] == "planned"
    assert exact_tree_state(target) == before
    source_before = exact_file_states((memory, graph, config))
    apply_report = source_root / "apply-report.json"
    applied = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config, "-TargetRoot", target, "-ProjectPath", project,
        "-IdempotencyKey", "bootstrap-red-key", "-MigrationExecutable", REAL_CANDIDATE,
        "-ReportPath", apply_report, "-AllowExistingIsolatedTarget",
        *migration_args(source_root, auth, auth_hash, "nonce-bootstrap-red"),
    )
    assert json.loads(applied.stdout)["core_status"] == "applied"
    assert json.loads(apply_report.read_text(encoding="utf-8"))["preserved_data_equivalent"] is True
    assert list((target / "projects").rglob("graph.db"))
    assert exact_file_states((memory, graph, config)) == source_before


def test_production_migration_reparse_paths_fail_closed_without_writes() -> None:
    root = EVIDENCE_ROOT / "production-migration-reparse-stage14-disposable"
    root.mkdir(parents=True)
    memory = root / "source-memory.sqlite3"
    graph = root / "source-graph.sqlite3"
    config = root / "source-config.sqlite3"
    make_db(memory, "memory")
    make_db(graph, "graph")
    make_db(config, "config")
    source_hashes = (sha256(memory), sha256(graph), sha256(config))
    project = ROOT.parents[1]
    invoke = WINDOWS / "Invoke-Stage14Migration.ps1"
    temp_root = Path(os.environ["TEMP"]) / f"stage14-agent-b-{EVIDENCE_ROOT.parent.name}" / "reparse-cases"
    manifest_sha = "a" * 64

    cases = (
        ("semantic-memory-root", False, "install"),
        ("data-root", False, "data"),
        ("install-marker", False, "marker"),
        ("current-pointer", False, "current"),
        ("projects-root", False, "projects"),
        ("artifacts-root", False, "artifacts"),
        ("config-database", True, "config_db"),
        ("memory-database", True, "memory_db"),
        ("global-graph-database", True, "global_graph_db"),
        ("project-shard", True, "project_shard"),
        ("project-graph-database", True, "project_graph_db"),
    )
    for index, (label, replay_target, selected_path) in enumerate(cases):
        local_app_data = temp_root / label / "LocalAppData"
        install_root = local_app_data / "SemanticMemory"
        target = install_root / "data"
        projects = target / "projects"
        artifacts = target / "artifacts"
        current = install_root / "state" / "current.json"
        marker = install_root / ".semantic-memory-managed.json"
        projects.mkdir(parents=True)
        artifacts.mkdir()
        write_json(current, {
            "schema": "stage14-current-pointer/v1",
            "version_id": "v1.1.0-rc.1+reparse-fixture",
            "manifest_sha256": manifest_sha,
        })
        write_json(marker, {
            "schema": "stage14-install-marker/v2",
            "product": "semantic-memory",
            "current_version_id": "v1.1.0-rc.1+reparse-fixture",
            "current_manifest_sha256": manifest_sha,
        })
        project_shard = projects / "2fb874ff-b9b3-5d31-997e-793aed30ce00"
        if replay_target:
            for database in (
                target / "_config.db",
                target / "__global__-memory.db",
                target / "__global__-graph.db",
            ):
                database.write_bytes(b"reparse database fixture\n")
            project_shard.mkdir()
            (project_shard / "graph.db").write_bytes(b"reparse project graph fixture\n")

        report = root / f"reparse-rejected-{index}.json"
        nonce = f"nonce-production-reparse-{index}"
        auth, auth_hash = authorize_production(
            root, memory, graph, config, project, target, report, nonce,
        )
        paths = {
            "install": install_root,
            "data": target,
            "marker": marker,
            "current": current,
            "projects": projects,
            "artifacts": artifacts,
            "config_db": target / "_config.db",
            "memory_db": target / "__global__-memory.db",
            "global_graph_db": target / "__global__-graph.db",
            "project_shard": project_shard,
            "project_graph_db": project_shard / "graph.db",
        }
        attacked = paths[selected_path]
        backing = temp_root / label / "junction-backing"
        if attacked.is_dir():
            shutil.move(str(attacked), str(backing))
        else:
            attacked.unlink()
            backing.mkdir(parents=True)
            (backing / "sentinel.txt").write_text(f"{label}\n", encoding="ascii")
        make_junction(attacked, backing)

        environment = os.environ.copy()
        environment["LOCALAPPDATA"] = str(local_app_data)
        root_before = (tree_entries(root), file_tree_state(root), tree_metadata_state(root))
        target_before = (
            tree_entries(temp_root / label),
            file_tree_state(temp_root / label),
            tree_metadata_state(temp_root / label),
        )
        rejected = run_ps(
            invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
            "-SourceConfig", config, "-TargetRoot", target, "-ProjectPath", project,
            "-IdempotencyKey", "reparse-key", "-MigrationExecutable", REAL_CANDIDATE,
            "-ReportPath", report, "-ProductionAuthorized", "-AllowExistingManagedTarget",
            *migration_args(root, auth, auth_hash, nonce), expect=1, environment=environment,
        )
        assert "PRODUCTION_TARGET_REPARSE_POINT" in rejected.stderr, (label, rejected.stderr)
        assert not report.exists(), label
        assert source_hashes == (sha256(memory), sha256(graph), sha256(config)), label
        assert root_before == (
            tree_entries(root), file_tree_state(root), tree_metadata_state(root)
        ), label
        assert target_before == (
            tree_entries(temp_root / label),
            file_tree_state(temp_root / label),
            tree_metadata_state(temp_root / label),
        ), label


def test_disposable_migration_backup_replay_failure_and_forward_recovery() -> None:
    root = EVIDENCE_ROOT / "migration-stage14-disposable"
    root.mkdir(parents=True)
    memory = root / "source-memory.sqlite3"
    graph = root / "source-graph.sqlite3"
    config = root / "source-config.sqlite3"
    make_db(memory, "memory")
    make_db(graph, "graph")
    make_db(config, "config")
    assert REAL_CANDIDATE.is_file()
    project = ROOT.parents[1]
    auth, auth_hash = authorize(root, memory, graph, config, project, "nonce-plan-apply")
    invoke = WINDOWS / "Invoke-Stage14Migration.ps1"
    plan_target = root / "plan-target"
    plan_report = root / "plan-report.json"
    plan = run_ps(
        invoke,
        "-Action", "Plan", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", plan_target, "-ProjectPath", project,
        "-IdempotencyKey", "plan-key", "-MigrationExecutable", REAL_CANDIDATE,
        "-ReportPath", plan_report, *migration_args(root, auth, auth_hash, "nonce-plan-apply"),
    )
    assert json.loads(plan.stdout)["core_status"] == "planned"
    assert not plan_target.exists()
    apply_target = root / "apply-target"
    apply_report = root / "apply-report.json"
    before = (sha256(memory), sha256(graph), sha256(config))
    applied = run_ps(
        invoke,
        "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", apply_target, "-ProjectPath", project,
        "-IdempotencyKey", "apply-key", "-MigrationExecutable", REAL_CANDIDATE,
        "-ReportPath", apply_report, *migration_args(root, auth, auth_hash, "nonce-plan-apply"),
    )
    applied_json = json.loads(applied.stdout)
    assert applied_json["core_status"] == "applied"
    assert applied_json["backup"]["schema"] == "stage14-authorized-source-backups/v2"
    assert applied_json["backup"]["core_source_checks"]["quick_check"] == "ok"
    core_report = json.loads(apply_report.read_text(encoding="utf-8"))
    assert core_report["target"]["quick_check"] == "ok"
    assert core_report["target"]["foreign_key_violations"] == 0
    assert set(core_report["legacy_alias_counts"]) == {"projects", "memory_items"}
    assert core_report["current_pointer_switched"] is False
    project_uuid = core_report["project_uuid"]
    assert (apply_target / "projects" / project_uuid / "graph.db").is_file()
    assert not (apply_target / f"{project_uuid}-graph.db").exists()
    source_paths = (memory, graph, config)
    sources_before_replay = exact_file_states(source_paths)
    assert not sqlite_path_sidecars(source_paths)
    apply_target_before_replay = exact_tree_state(apply_target)
    assert not sqlite_transient_sidecars(apply_target)
    assert_source_projection(memory, apply_target / "__global__-memory.db")
    assert_source_projection(config, apply_target / "_config.db")
    assert_source_projection(graph, apply_target / "projects" / project_uuid / "graph.db")
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert exact_tree_state(apply_target) == apply_target_before_replay
    assert before == (sha256(memory), sha256(graph), sha256(config))
    replay_report = root / "replay-report.json"
    replay = run_ps(
        invoke,
        "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", apply_target, "-ProjectPath", project,
        "-IdempotencyKey", "apply-key", "-MigrationExecutable", REAL_CANDIDATE,
        "-ReportPath", replay_report, "-AllowExistingIsolatedTarget",
        *migration_args(root, auth, auth_hash, "nonce-plan-apply"),
    )
    assert json.loads(replay.stdout)["core_status"] == "replayed"
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert not sqlite_transient_sidecars(apply_target)
    assert exact_tree_state(apply_target) == apply_target_before_replay
    assert_source_projection(memory, apply_target / "__global__-memory.db")
    assert_source_projection(config, apply_target / "_config.db")
    assert_source_projection(graph, apply_target / "projects" / project_uuid / "graph.db")
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert exact_tree_state(apply_target) == apply_target_before_replay
    failed_target = root / "failed-target"
    failed_report = root / "failed-report.json"
    failed = run_ps(
        invoke,
        "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", failed_target, "-ProjectPath", project,
        "-IdempotencyKey", "failed-key", "-MigrationExecutable", REAL_CANDIDATE,
        "-ReportPath", failed_report, "-FaultInjection", "after_backup",
        *migration_args(root, auth, auth_hash, "nonce-plan-apply"), expect=1,
    )
    assert "FAULT_INJECTION: after_backup" in failed.stderr
    assert not failed_target.exists()
    journal = root / "forward-journal.jsonl"
    journal.write_text('{"task_id":"new-task","payload_sha256":"' + "d" * 64 + '"}\n', encoding="utf-8")
    forward = root / "forward-recovery.json"
    recovered = run_ps(
        invoke,
        "-Action", "ForwardRecover", "-TargetRoot", apply_target,
        "-PriorReportPath", apply_report, "-ForwardJournalPath", journal,
        "-ReportPath", forward, *migration_args(root, auth, auth_hash, "nonce-plan-apply"),
    )
    forward_json = json.loads(recovered.stdout)
    assert forward_json["status"] == "READY_FOR_CORE_FORWARD_REPLAY"
    assert forward_json["production_restore_performed"] is False
    assert forward_json["current_pointer_switched"] is False

    crash_target = root / "crash-target"
    crash_report = root / "crash-report.json"
    crashed = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", crash_target, "-ProjectPath", project, "-IdempotencyKey", "crash-key",
        "-MigrationExecutable", REAL_CANDIDATE, "-ReportPath", crash_report,
        "-FaultInjection", "after_core", *migration_args(root, auth, auth_hash, "nonce-plan-apply"), expect=1,
    )
    assert "FAULT_INJECTION: after_core" in crashed.stderr and crash_target.exists()
    assert not (root / "state" / "current.json").exists()
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    crash_target_before_replay = exact_tree_state(crash_target)
    assert not sqlite_transient_sidecars(crash_target)
    assert_source_projection(memory, crash_target / "__global__-memory.db")
    assert_source_projection(config, crash_target / "_config.db")
    assert_source_projection(graph, crash_target / "projects" / project_uuid / "graph.db")
    assert exact_tree_state(crash_target) == crash_target_before_replay
    recovery_report = root / "crash-replay-report.json"
    recovered = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", crash_target, "-ProjectPath", project, "-IdempotencyKey", "crash-key",
        "-MigrationExecutable", REAL_CANDIDATE, "-ReportPath", recovery_report,
        "-AllowExistingIsolatedTarget", *migration_args(root, auth, auth_hash, "nonce-plan-apply"),
    )
    assert json.loads(recovered.stdout)["core_status"] == "replayed"
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert not sqlite_transient_sidecars(crash_target)
    assert exact_tree_state(crash_target) == crash_target_before_replay
    assert_source_projection(memory, crash_target / "__global__-memory.db")
    assert_source_projection(config, crash_target / "_config.db")
    assert_source_projection(graph, crash_target / "projects" / project_uuid / "graph.db")
    assert exact_file_states(source_paths) == sources_before_replay
    assert not sqlite_path_sidecars(source_paths)
    assert exact_tree_state(crash_target) == crash_target_before_replay

    db = sqlite3.connect(memory)
    try:
        db.execute("INSERT INTO sample VALUES('memory-2','changed')")
        db.commit()
    finally:
        db.close()
    conflict_auth, conflict_hash = authorize(root, memory, graph, config, project, "nonce-conflict")
    conflict_report = root / "conflict-report.json"
    conflict = run_ps(
        invoke, "-Action", "Apply", "-SourceMemory", memory, "-SourceGraph", graph,
        "-SourceConfig", config,
        "-TargetRoot", apply_target, "-ProjectPath", project, "-IdempotencyKey", "apply-key",
        "-MigrationExecutable", REAL_CANDIDATE, "-ReportPath", conflict_report,
        "-AllowExistingIsolatedTarget", *migration_args(root, conflict_auth, conflict_hash, "nonce-conflict"), expect=1,
    )
    assert conflict_report.is_file(), conflict.stdout + conflict.stderr
    assert json.loads(conflict_report.read_text(encoding="utf-8"))["status"] == "IDEMPOTENCY_CONFLICT"


def main() -> int:
    results = []
    for name, test in sorted(globals().items()):
        if not name.startswith("test_") or not callable(test):
            continue
        try:
            test()
            results.append({"name": name, "status": "PASS"})
            print(f"PASS {name}")
        except Exception as exc:
            results.append(
                {
                    "name": name,
                    "status": "FAIL",
                    "error": str(exc),
                    "traceback": traceback.format_exc(),
                }
            )
            print(f"FAIL {name}: {exc}")
    payload = {
        "schema": "stage14-agent-b-focused-tests/v1",
        "pass_count": sum(item["status"] == "PASS" for item in results),
        "fail_count": sum(item["status"] == "FAIL" for item in results),
        "tests": results,
    }
    write_json(EVIDENCE_ROOT.parent / "focused-test-result.json", payload)
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if payload["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
