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


ROOT = Path(__file__).resolve().parents[2]
PLUGIN_SOURCE = ROOT / "packaging" / "semantic-memory"
TRANSACTION = ROOT / "packaging" / "windows" / "Install-SemanticMemoryPlugin.ps1"
EVIDENCE_ROOT = Path(
    os.environ.get(
        "STAGE14_PLUGIN_TX_TEST_ROOT",
        ROOT.parents[1]
        / "test-results"
        / "stage14"
        / "stage14-personal-plugin-transaction-not-configured",
    )
).resolve()
TIMEOUT_SECONDS = 90


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def decode(value: bytes | None) -> str:
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


def transaction_command(
    package: Path,
    user_home: Path,
    action: str,
    *extra: object,
) -> list[str]:
    return [
        "powershell.exe",
        "-NoLogo",
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(TRANSACTION),
        "-Action",
        action,
        "-PackageRoot",
        str(package),
        "-UserHome",
        str(user_home),
        "-CodexHome",
        str(user_home / ".codex"),
        "-StateRoot",
        str(
            user_home
            / "AppData"
            / "Local"
            / "SemanticMemory"
            / "backups"
            / "codex-plugin"
        ),
        *(str(value) for value in extra),
    ]


def run_transaction(
    package: Path,
    user_home: Path,
    action: str,
    *extra: object,
    expected_exit: int = 0,
) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
    raw = subprocess.run(
        transaction_command(package, user_home, action, *extra),
        capture_output=True,
        timeout=TIMEOUT_SECONDS,
    )
    result = subprocess.CompletedProcess(
        raw.args,
        raw.returncode,
        decode(raw.stdout),
        decode(raw.stderr),
    )
    assert result.returncode == expected_exit, result.stdout + result.stderr
    assert result.stdout.strip(), result.stderr
    return result, json.loads(result.stdout)


def file_records(root: Path, prefix: str = "") -> list[dict[str, object]]:
    return [
        {
            "path": prefix + path.relative_to(root).as_posix(),
            "bytes": path.stat().st_size,
            "sha256": sha256_file(path),
        }
        for path in sorted(item for item in root.rglob("*") if item.is_file())
    ]


def tree_hash(root: Path) -> str:
    records = file_records(root)
    value = "\n".join(
        f"{record['path']}`0{record['bytes']}`0{record['sha256']}" for record in records
    )
    return sha256_bytes(value.encode("utf-8"))


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


def make_package(root: Path) -> Path:
    package = root / "offline-installer"
    plugin = package / "plugin" / "semantic-memory"
    shutil.copytree(PLUGIN_SOURCE, plugin)
    records = file_records(plugin, "plugin/semantic-memory/")
    manifest = {
        "schema": "stage14-offline-installer-manifest/v1",
        "version": "v1.1.0-rc.1",
        "personal_plugin": {
            "root": "plugin/semantic-memory",
            "manifest": "plugin/semantic-memory/.codex-plugin/plugin.json",
            "files": records,
            "transaction": {
                "script": "Install-SemanticMemoryPlugin.ps1",
                "schema": "stage14-personal-plugin-transaction/v1",
            },
        },
    }
    (package / "installer-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    return package


def make_mock_cli(root: Path) -> tuple[Path, Path]:
    mock = root / "mock-codex.cmd"
    log = root / "mock-codex-commands.log"
    mock.write_text(
        '@echo off\r\n'
        'if "%SM_PLUGIN_MOCK_LOG%"=="" exit /b 23\r\n'
        'echo %*>>"%SM_PLUGIN_MOCK_LOG%"\r\n'
        "exit /b 0\r\n",
        encoding="ascii",
    )
    return mock, log


def prepare_user(
    root: Path,
    *,
    existing: bool = True,
) -> tuple[Path, Path, Path, Path, Path]:
    user_home = root / "isolated user 中文"
    user_home.mkdir(parents=True)
    source = user_home / "plugins" / "semantic-memory"
    cache = user_home / ".codex" / "plugins" / "cache" / "personal" / "semantic-memory"
    marketplace = user_home / ".agents" / "plugins" / "marketplace.json"
    marketplace.parent.mkdir(parents=True, exist_ok=True)
    if existing:
        source.mkdir(parents=True)
        (source / "old.txt").write_bytes(b"old-source-exact\n")
        (cache / "0.9.0").mkdir(parents=True)
        (cache / "0.9.0" / "old.txt").write_bytes(b"old-cache-exact\n")
        marketplace.write_text(
            json.dumps(
                {
                    "name": "personal",
                    "interface": {
                        "displayName": "Personal",
                        "thirdPartyUi": {"color": "blue"},
                    },
                    "thirdPartyTop": {"preserve": True},
                    "plugins": [
                        {
                            "name": "other-plugin",
                            "source": {"source": "local", "path": "./plugins/other"},
                            "custom": {"tokenNameOnly": "not-a-token-value"},
                        },
                        {
                            "name": "semantic-memory",
                            "source": {
                                "source": "local",
                                "path": "./plugins/old-semantic-memory",
                                "thirdPartySourceField": "preserve",
                            },
                            "policy": {
                                "installation": "BLOCKED",
                                "authentication": "NONE",
                                "thirdPartyPolicyField": 17,
                            },
                            "category": "Old",
                            "thirdPartyPluginField": {"preserve": "yes"},
                        },
                    ],
                },
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    return user_home, source, cache, marketplace, (
        user_home / "AppData" / "Local" / "SemanticMemory" / "backups" / "codex-plugin"
    )


def expected_args(source: Path, cache: Path, marketplace: Path) -> tuple[str, ...]:
    return (
        "-ExpectedSourceTreeSha256",
        tree_hash(source) if source.exists() else "ABSENT",
        "-ExpectedCacheTreeSha256",
        tree_hash(cache) if cache.exists() else "ABSENT",
        "-ExpectedMarketplaceSha256",
        sha256_file(marketplace) if marketplace.exists() else "ABSENT",
    )


def assert_no_runtime_secret(
    result: subprocess.CompletedProcess[str],
    state_root: Path,
) -> None:
    sentinel = "not-a-token-value"
    assert sentinel not in result.stdout
    assert sentinel not in result.stderr
    for path in state_root.rglob("*.json"):
        if path.name == "marketplace.before.preserved":
            continue
        assert sentinel not in path.read_text(encoding="utf-8")
    for path in state_root.rglob("*.jsonl"):
        assert sentinel not in path.read_text(encoding="utf-8")


def test_preview_apply_verify_cli_replay_and_explicit_rollback() -> None:
    root = EVIDENCE_ROOT / "apply-verify-replay-rollback"
    root.mkdir(parents=True)
    package = make_package(root)
    user, source, cache, marketplace, state_root = prepare_user(root)
    mock, mock_log = make_mock_cli(root)
    before_source = tree_state(source)
    before_cache = tree_state(cache)
    before_marketplace = marketplace.read_bytes()

    _, preview = run_transaction(package, user, "Preview")
    assert preview["status"] == "RED_PLUGIN_DRIFT"
    assert preview["write_performed"] is False
    assert preview["cli_commands"][0][:3] == ["plugin", "marketplace", "add"]
    assert preview["cli_commands"][1] == ["plugin", "add", "semantic-memory@personal"]
    assert not state_root.exists()

    environment_log = str(mock_log)
    prior = os.environ.get("SM_PLUGIN_MOCK_LOG")
    os.environ["SM_PLUGIN_MOCK_LOG"] = environment_log
    try:
        result, applied = run_transaction(
            package,
            user,
            "Apply",
            *expected_args(source, cache, marketplace),
            "-ConfirmUserMutation",
            "-InvokeCodexCli",
            "-CodexCliPath",
            mock,
        )
    finally:
        if prior is None:
            os.environ.pop("SM_PLUGIN_MOCK_LOG", None)
        else:
            os.environ["SM_PLUGIN_MOCK_LOG"] = prior
    assert applied["status"] == "APPLIED_VERIFIED"
    assert applied["third_party_projection_preserved"] is True
    assert applied["cli_invoked"] is True
    assert tree_hash(source) == preview["states_desired"]["source"]["tree_sha256"]
    assert tree_hash(cache) == preview["states_desired"]["cache"]["tree_sha256"]
    marketplace_json = json.loads(marketplace.read_text(encoding="utf-8"))
    assert marketplace_json["thirdPartyTop"] == {"preserve": True}
    assert marketplace_json["interface"]["thirdPartyUi"] == {"color": "blue"}
    semantic = [
        item for item in marketplace_json["plugins"] if item["name"] == "semantic-memory"
    ]
    assert len(semantic) == 1
    assert semantic[0]["thirdPartyPluginField"] == {"preserve": "yes"}
    assert semantic[0]["source"]["thirdPartySourceField"] == "preserve"
    assert semantic[0]["policy"]["thirdPartyPolicyField"] == 17
    assert semantic[0]["source"]["path"] == "./plugins/semantic-memory"
    assert semantic[0]["policy"]["installation"] == "AVAILABLE"
    assert not (source / ".mcp.json").exists()
    assert not list(cache.rglob(".mcp.json"))
    mock_commands = decode(mock_log.read_bytes()).splitlines()
    assert len(mock_commands) == 2
    assert mock_commands[0].startswith("plugin marketplace add ")
    assert mock_commands[0][len("plugin marketplace add ") :].strip('"') == str(marketplace)
    assert mock_commands[1] == "plugin add semantic-memory@personal"
    backup = Path(str(applied["marketplace_original_backup_path"]))
    assert backup.read_bytes() == before_marketplace
    transaction = Path(str(applied["transaction_path"]))
    journal = Path(str(applied["journal_path"]))
    events = [
        json.loads(line)["event"]
        for line in journal.read_text(encoding="utf-8").splitlines()
    ]
    assert events == [
        "PREPARED",
        "SOURCE_DISPLACED",
        "SOURCE_SWAPPED",
        "CACHE_DISPLACED",
        "CACHE_SWAPPED",
        "MARKETPLACE_SWAPPED",
        "CLI_COMMAND_SUCCEEDED",
        "CLI_COMMAND_SUCCEEDED",
        "COMMIT_VERIFIED",
    ]
    assert_no_runtime_secret(result, state_root)

    _, verified = run_transaction(package, user, "Verify")
    assert verified["status"] == "PASS"
    assert verified["source_hash_match"] is True
    assert verified["cache_hash_match"] is True
    assert verified["marketplace_unique"] is True
    terminal_source = tree_state(source)
    terminal_cache = tree_state(cache)
    terminal_marketplace = marketplace.read_bytes()
    terminal_transactions = sorted(path.name for path in state_root.iterdir())
    terminal_journal = journal.read_bytes()
    terminal_mock = mock_log.read_bytes()
    _, replay = run_transaction(
        package,
        user,
        "Apply",
        *expected_args(source, cache, marketplace),
        "-ConfirmUserMutation",
        "-InvokeCodexCli",
        "-CodexCliPath",
        mock,
    )
    assert replay["status"] == "REPLAYED_ZERO_WRITE"
    assert replay["transaction_path"] is None
    assert replay["cli_invoked"] is False
    assert tree_state(source) == terminal_source
    assert tree_state(cache) == terminal_cache
    assert marketplace.read_bytes() == terminal_marketplace
    assert sorted(path.name for path in state_root.iterdir()) == terminal_transactions
    assert journal.read_bytes() == terminal_journal
    assert mock_log.read_bytes() == terminal_mock

    _, rolled_back = run_transaction(
        package,
        user,
        "Rollback",
        "-TransactionPath",
        transaction,
        "-ConfirmUserMutation",
    )
    assert rolled_back["status"] == "ROLLED_BACK_VERIFIED"
    assert tree_state(source) == before_source
    assert tree_state(cache) == before_cache
    assert marketplace.read_bytes() == before_marketplace


def test_fault_injection_automatically_restores_all_three_states() -> None:
    for fault in (
        "after_source_swap",
        "after_cache_swap",
        "after_marketplace_swap",
        "after_cli",
    ):
        root = EVIDENCE_ROOT / f"fault-{fault}"
        root.mkdir(parents=True)
        package = make_package(root)
        user, source, cache, marketplace, state_root = prepare_user(root)
        mock, mock_log = make_mock_cli(root)
        before = (tree_state(source), tree_state(cache), marketplace.read_bytes())
        prior = os.environ.get("SM_PLUGIN_MOCK_LOG")
        os.environ["SM_PLUGIN_MOCK_LOG"] = str(mock_log)
        try:
            _, failed = run_transaction(
                package,
                user,
                "Apply",
                *expected_args(source, cache, marketplace),
                "-ConfirmUserMutation",
                "-InvokeCodexCli",
                "-CodexCliPath",
                mock,
                "-FaultInjection",
                fault,
                expected_exit=1,
            )
        finally:
            if prior is None:
                os.environ.pop("SM_PLUGIN_MOCK_LOG", None)
            else:
                os.environ["SM_PLUGIN_MOCK_LOG"] = prior
        assert failed["status"] == "ROLLED_BACK_VERIFIED"
        assert failed["rollback_performed"] is True
        assert (tree_state(source), tree_state(cache), marketplace.read_bytes()) == before
        journal = Path(str(failed["journal_path"]))
        assert json.loads(journal.read_text(encoding="utf-8").splitlines()[-1])[
            "event"
        ] == "ROLLBACK_VERIFIED"
        assert state_root.exists()


def test_crash_recovery_restores_partial_transaction_and_replays_zero_write() -> None:
    for fault in (
        "crash_after_source_displace",
        "crash_after_cache_displace",
        "crash_after_cache_swap",
    ):
        root = EVIDENCE_ROOT / f"crash-recovery-{fault}"
        root.mkdir(parents=True)
        package = make_package(root)
        user, source, cache, marketplace, state_root = prepare_user(root)
        before = (tree_state(source), tree_state(cache), marketplace.read_bytes())
        _, crashed = run_transaction(
            package,
            user,
            "Apply",
            *expected_args(source, cache, marketplace),
            "-ConfirmUserMutation",
            "-FaultInjection",
            fault,
            expected_exit=1,
        )
        assert crashed["status"] == "CRASH_SIMULATED_RECOVERY_REQUIRED"
        transaction = Path(str(crashed["transaction_path"]))
        journal = Path(str(crashed["journal_path"]))
        _, recovered = run_transaction(
            package,
            user,
            "Recover",
            "-TransactionPath",
            transaction,
            "-ConfirmUserMutation",
        )
        assert recovered["status"] == "RECOVERED_ROLLBACK_VERIFIED"
        assert (tree_state(source), tree_state(cache), marketplace.read_bytes()) == before
        terminal_user_tree = tree_state(user)
        terminal_journal = journal.read_bytes()
        _, replay = run_transaction(
            package,
            user,
            "Recover",
            "-TransactionPath",
            transaction,
            "-ConfirmUserMutation",
        )
        assert replay["status"] == "RECOVERED_ROLLBACK_VERIFIED"
        assert replay["write_performed"] is False
        assert replay["journal_write_performed"] is False
        assert tree_state(user) == terminal_user_tree
        assert journal.read_bytes() == terminal_journal
        assert not list(user.rglob(f".semantic-memory.*.stage"))
        assert not list(user.rglob(".marketplace.*.candidate.tmp"))


def test_marketplace_replace_crash_uses_tracked_backup_and_replays_zero_write() -> None:
    root = EVIDENCE_ROOT / "crash-recovery-marketplace-replace"
    root.mkdir(parents=True)
    package = make_package(root)
    user, source, cache, marketplace, _ = prepare_user(root)
    marketplace_before = marketplace.read_bytes()
    marketplace_before_sha256 = sha256_file(marketplace)
    _, crashed = run_transaction(
        package,
        user,
        "Apply",
        *expected_args(source, cache, marketplace),
        "-ConfirmUserMutation",
        "-FaultInjection",
        "crash_after_marketplace_replace",
        expected_exit=1,
    )
    assert crashed["status"] == "CRASH_SIMULATED_RECOVERY_REQUIRED"
    transaction = Path(str(crashed["transaction_path"]))
    journal = Path(str(crashed["journal_path"]))
    descriptor = json.loads(transaction.read_text(encoding="utf-8"))
    tracked_backup = Path(str(descriptor["backups"]["marketplace_swap_backup"]))
    assert tracked_backup.parent == transaction.parent
    assert tracked_backup.read_bytes() == marketplace_before
    assert sha256_file(tracked_backup) == marketplace_before_sha256
    assert not list(marketplace.parent.glob(".marketplace.*.swap-backup"))

    _, recovered = run_transaction(
        package,
        user,
        "Recover",
        "-TransactionPath",
        transaction,
        "-ConfirmUserMutation",
    )
    assert recovered["status"] == "RECOVERED_COMMIT_VERIFIED"
    assert recovered["write_performed"] is False
    assert tracked_backup.read_bytes() == marketplace_before
    terminal_user_tree = tree_state(user)
    terminal_journal = journal.read_bytes()
    _, replay = run_transaction(
        package,
        user,
        "Recover",
        "-TransactionPath",
        transaction,
        "-ConfirmUserMutation",
    )
    assert replay["status"] == "RECOVERED_COMMIT_VERIFIED"
    assert replay["write_performed"] is False
    assert replay["journal_write_performed"] is False
    assert tree_state(user) == terminal_user_tree
    assert journal.read_bytes() == terminal_journal
    assert not list(marketplace.parent.glob(".marketplace.*.swap-backup"))


def test_three_state_cas_detects_drift_at_every_swap_and_rolls_back_owned_changes() -> None:
    gates = (
        ("delay_before_source_swap", "PREPARED"),
        ("delay_before_cache_swap", "SOURCE_SWAPPED"),
        ("delay_before_marketplace_swap", "CACHE_SWAPPED"),
    )
    for fault, gate_event in gates:
        root = EVIDENCE_ROOT / f"three-state-cas-{fault}"
        root.mkdir(parents=True)
        package = make_package(root)
        user, source, cache, marketplace, state_root = prepare_user(root)
        before_source = tree_state(source)
        before_cache = tree_state(cache)
        external_marketplace = (
            '{"name":"external-drift","plugins":[],"preserve":"external"}\n'
        ).encode("utf-8")
        process = subprocess.Popen(
            transaction_command(
                package,
                user,
                "Apply",
                *expected_args(source, cache, marketplace),
                "-ConfirmUserMutation",
                "-FaultInjection",
                fault,
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 10
        observed = False
        while time.monotonic() < deadline:
            journals = list(state_root.rglob("journal.jsonl")) if state_root.exists() else []
            if journals and gate_event in journals[0].read_text(encoding="utf-8"):
                observed = True
                break
            if process.poll() is not None:
                break
            time.sleep(0.02)
        assert observed, f"did not observe {gate_event} for {fault}"
        marketplace.write_bytes(external_marketplace)
        stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
        result = subprocess.CompletedProcess(
            process.args,
            process.returncode,
            decode(stdout),
            decode(stderr),
        )
        assert result.returncode == 1, result.stdout + result.stderr
        parsed = json.loads(result.stdout)
        assert parsed["status"] == "ROLLED_BACK_OWNED_CHANGES_EXTERNAL_DRIFT"
        assert parsed["rollback_performed"] is True
        assert parsed["states_after"]["external_drift_detected"] is True
        assert "marketplace" in parsed["states_after"]["external_drift_components"]
        assert tree_state(source) == before_source
        assert tree_state(cache) == before_cache
        assert marketplace.read_bytes() == external_marketplace
        journal = next(state_root.rglob("journal.jsonl"))
        assert json.loads(journal.read_text(encoding="utf-8").splitlines()[-1])[
            "event"
        ] == "ROLLBACK_OWNED_CHANGES_VERIFIED_EXTERNAL_DRIFT"
        assert not list(user.rglob(".semantic-memory.*.stage"))
        assert not list(user.rglob(".marketplace.*.candidate.tmp"))


def test_recover_rejects_descriptor_and_journal_tampering_with_zero_write() -> None:
    descriptor_mutations = {
        "package-installer-manifest": lambda value: value.__setitem__(
            "package_installer_manifest_sha256", "0" * 64
        ),
        "package-plugin-manifest": lambda value: value.__setitem__(
            "package_plugin_manifest_sha256", "0" * 64
        ),
        "desired-state": lambda value: value["states_desired"]["source"].__setitem__(
            "tree_sha256", "0" * 64
        ),
        "before-state": lambda value: value["states_before"]["source"].__setitem__(
            "tree_sha256", "0" * 64
        ),
        "staging-path": lambda value: value["staging"].__setitem__(
            "cache", value["staging"]["cache"] + ".tampered"
        ),
        "backup-path": lambda value: value["backups"].__setitem__(
            "source_before", value["backups"]["source_before"] + ".tampered"
        ),
    }
    for case_name, mutate in descriptor_mutations.items():
        root = EVIDENCE_ROOT / f"recover-tamper-{case_name}"
        root.mkdir(parents=True)
        package = make_package(root)
        user, source, cache, marketplace, _ = prepare_user(root)
        _, crashed = run_transaction(
            package,
            user,
            "Apply",
            *expected_args(source, cache, marketplace),
            "-ConfirmUserMutation",
            "-FaultInjection",
            "crash_after_source_displace",
            expected_exit=1,
        )
        transaction = Path(str(crashed["transaction_path"]))
        descriptor = json.loads(transaction.read_text(encoding="utf-8"))
        mutate(descriptor)
        transaction.write_text(
            json.dumps(descriptor, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        tampered_tree = tree_state(user)
        _, rejected = run_transaction(
            package,
            user,
            "Recover",
            "-TransactionPath",
            transaction,
            "-ConfirmUserMutation",
            expected_exit=1,
        )
        assert rejected["status"] == "FAILED_CLOSED"
        assert str(rejected["error_code"]).startswith("PLUGIN_")
        assert rejected["write_performed"] is False
        assert tree_state(user) == tampered_tree

    root = EVIDENCE_ROOT / "recover-tamper-journal-chain"
    root.mkdir(parents=True)
    package = make_package(root)
    user, source, cache, marketplace, _ = prepare_user(root)
    _, crashed = run_transaction(
        package,
        user,
        "Apply",
        *expected_args(source, cache, marketplace),
        "-ConfirmUserMutation",
        "-FaultInjection",
        "crash_after_source_displace",
        expected_exit=1,
    )
    transaction = Path(str(crashed["transaction_path"]))
    journal = Path(str(crashed["journal_path"]))
    lines = journal.read_text(encoding="utf-8").splitlines()
    record = json.loads(lines[1])
    record["sequence"] = 99
    lines[1] = json.dumps(record, ensure_ascii=False, separators=(",", ":"))
    journal.write_text("\n".join(lines) + "\n", encoding="utf-8")
    tampered_tree = tree_state(user)
    _, rejected = run_transaction(
        package,
        user,
        "Recover",
        "-TransactionPath",
        transaction,
        "-ConfirmUserMutation",
        expected_exit=1,
    )
    assert rejected["error_code"] in {
        "PLUGIN_JOURNAL_INVALID",
        "PLUGIN_JOURNAL_ORDER_INVALID",
    }
    assert rejected["write_performed"] is False
    assert tree_state(user) == tampered_tree

    copied_root = transaction.parent.parent / "copied-transaction"
    copied_root.mkdir()
    copied_descriptor = copied_root / "transaction.json"
    shutil.copy2(transaction, copied_descriptor)
    shutil.copy2(journal, copied_root / "journal.jsonl")
    copied_tree = tree_state(user)
    _, rejected_path = run_transaction(
        package,
        user,
        "Recover",
        "-TransactionPath",
        copied_descriptor,
        "-ConfirmUserMutation",
        expected_exit=1,
    )
    assert rejected_path["error_code"] == "PLUGIN_TRANSACTION_DIRECTORY_MISMATCH"
    assert rejected_path["write_performed"] is False
    assert tree_state(user) == copied_tree

    root_a = EVIDENCE_ROOT / "recover-valid-but-inconsistent-journal-a"
    root_b = EVIDENCE_ROOT / "recover-valid-but-inconsistent-journal-b"
    root_a.mkdir(parents=True)
    root_b.mkdir(parents=True)
    package_a = make_package(root_a)
    package_b = make_package(root_b)
    user_a, source_a, cache_a, marketplace_a, _ = prepare_user(root_a)
    user_b, source_b, cache_b, marketplace_b, _ = prepare_user(root_b)
    _, crashed_a = run_transaction(
        package_a,
        user_a,
        "Apply",
        *expected_args(source_a, cache_a, marketplace_a),
        "-ConfirmUserMutation",
        "-FaultInjection",
        "crash_after_source_displace",
        expected_exit=1,
    )
    _, crashed_b = run_transaction(
        package_b,
        user_b,
        "Apply",
        *expected_args(source_b, cache_b, marketplace_b),
        "-ConfirmUserMutation",
        "-FaultInjection",
        "crash_after_source_displace",
        expected_exit=1,
    )
    transaction_a = Path(str(crashed_a["transaction_path"]))
    journal_a = Path(str(crashed_a["journal_path"]))
    journal_b = Path(str(crashed_b["journal_path"]))
    journal_a.write_bytes(journal_b.read_bytes())
    inconsistent_tree = tree_state(user_a)
    _, rejected_inconsistent = run_transaction(
        package_a,
        user_a,
        "Recover",
        "-TransactionPath",
        transaction_a,
        "-ConfirmUserMutation",
        expected_exit=1,
    )
    assert (
        rejected_inconsistent["error_code"]
        == "PLUGIN_TRANSACTION_DESCRIPTOR_JOURNAL_MISMATCH"
    )
    assert rejected_inconsistent["write_performed"] is False
    assert tree_state(user_a) == inconsistent_tree


def test_three_way_cas_and_package_mcp_tamper_fail_closed() -> None:
    root = EVIDENCE_ROOT / "cas-and-package-tamper"
    root.mkdir(parents=True)
    package = make_package(root)
    user, source, cache, marketplace, state_root = prepare_user(root)
    before = (tree_state(source), tree_state(cache), marketplace.read_bytes())
    _, rejected = run_transaction(
        package,
        user,
        "Apply",
        "-ExpectedSourceTreeSha256",
        "0" * 64,
        "-ExpectedCacheTreeSha256",
        tree_hash(cache),
        "-ExpectedMarketplaceSha256",
        sha256_file(marketplace),
        "-ConfirmUserMutation",
        expected_exit=1,
    )
    assert rejected["error_code"] == "PLUGIN_CAS_PRECONDITION_CONFLICT"
    assert rejected["write_performed"] is False
    assert (tree_state(source), tree_state(cache), marketplace.read_bytes()) == before
    assert not state_root.exists()

    plugin = package / "plugin" / "semantic-memory"
    (plugin / ".mcp.json").write_text('{"mcpServers":{}}\n', encoding="utf-8")
    manifest = json.loads((package / "installer-manifest.json").read_text(encoding="utf-8"))
    manifest["personal_plugin"]["files"] = file_records(plugin, "plugin/semantic-memory/")
    (package / "installer-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )
    _, tampered = run_transaction(package, user, "Preview", expected_exit=1)
    assert tampered["error_code"] == "PLUGIN_DUPLICATE_MCP_REGISTRATION_FORBIDDEN"
    assert (tree_state(source), tree_state(cache), marketplace.read_bytes()) == before


def test_marketplace_without_semantic_record_is_extended_without_third_party_drift() -> None:
    root = EVIDENCE_ROOT / "marketplace-without-semantic-record"
    root.mkdir(parents=True)
    package = make_package(root)
    user, source, cache, marketplace, state_root = prepare_user(root, existing=False)
    marketplace.parent.mkdir(parents=True, exist_ok=True)
    sentinel = "unrelated-marketplace-record-preserved"
    marketplace.write_text(
        json.dumps(
            {
                "name": "personal",
                "interface": {
                    "displayName": "Personal",
                    "thirdPartyUi": {"theme": "unchanged"},
                },
                "plugins": [
                    {
                        "name": "other-plugin",
                        "source": {"source": "local", "path": "./plugins/other"},
                        "custom": sentinel,
                    }
                ],
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    before_marketplace = marketplace.read_bytes()
    args = expected_args(source, cache, marketplace)
    _, preview = run_transaction(package, user, "Preview")
    assert preview["status"] == "RED_PLUGIN_DRIFT"
    assert preview["third_party_projection_sha256_before"] == (
        preview["third_party_projection_sha256_after"]
    )
    _, applied = run_transaction(
        package,
        user,
        "Apply",
        *args,
        "-ConfirmUserMutation",
    )
    assert applied["status"] == "APPLIED_VERIFIED"
    assert applied["third_party_projection_preserved"] is True
    after = json.loads(marketplace.read_text(encoding="utf-8"))
    assert sum(item.get("name") == "semantic-memory" for item in after["plugins"]) == 1
    assert next(item for item in after["plugins"] if item["name"] == "other-plugin")[
        "custom"
    ] == sentinel
    backup = Path(str(applied["marketplace_original_backup_path"]))
    assert backup.read_bytes() == before_marketplace
    _, verified = run_transaction(package, user, "Verify")
    assert verified["status"] == "PASS"
    assert state_root.exists()


def test_package_reparse_tree_is_rejected_without_user_writes() -> None:
    root = EVIDENCE_ROOT / "reparse-rejection"
    root.mkdir(parents=True)
    package = make_package(root)
    user, source, cache, marketplace, state_root = prepare_user(root)
    plugin = package / "plugin" / "semantic-memory"
    external = root / "external"
    external.mkdir()
    (external / "outside.txt").write_text("outside\n", encoding="utf-8")
    junction = plugin / "junction"
    result = subprocess.run(
        ["cmd.exe", "/d", "/c", "mklink", "/J", str(junction), str(external)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    before = (tree_state(source), tree_state(cache), marketplace.read_bytes())
    _, rejected = run_transaction(package, user, "Preview", expected_exit=1)
    assert rejected["error_code"] == "PLUGIN_REPARSE_TREE_FORBIDDEN"
    assert (tree_state(source), tree_state(cache), marketplace.read_bytes()) == before
    assert not state_root.exists()


def test_custom_plugin_paths_must_be_pairwise_disjoint() -> None:
    cases = ("codex-inside-source", "state-equals-source")
    for case_name in cases:
        root = EVIDENCE_ROOT / f"path-overlap-{case_name}"
        root.mkdir(parents=True)
        package = make_package(root)
        user, source, cache, marketplace, state_root = prepare_user(root)
        before = (tree_state(source), tree_state(cache), marketplace.read_bytes())
        codex_home = source if case_name == "codex-inside-source" else user / ".codex"
        custom_state = source if case_name == "state-equals-source" else state_root
        raw = subprocess.run(
            [
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(TRANSACTION),
                "-Action",
                "Preview",
                "-PackageRoot",
                str(package),
                "-UserHome",
                str(user),
                "-CodexHome",
                str(codex_home),
                "-StateRoot",
                str(custom_state),
            ],
            capture_output=True,
            timeout=TIMEOUT_SECONDS,
        )
        result = subprocess.CompletedProcess(
            raw.args,
            raw.returncode,
            decode(raw.stdout),
            decode(raw.stderr),
        )
        assert result.returncode == 1, result.stdout + result.stderr
        rejected = json.loads(result.stdout)
        assert rejected["status"] == "FAILED_CLOSED"
        assert rejected["error_code"] == "PLUGIN_USER_PATHS_OVERLAP"
        assert rejected["write_performed"] is False
        assert (tree_state(source), tree_state(cache), marketplace.read_bytes()) == before
        if case_name == "codex-inside-source":
            assert not state_root.exists()


EXPECTED_TEST_NAMES = frozenset(
    {
        "test_crash_recovery_restores_partial_transaction_and_replays_zero_write",
        "test_custom_plugin_paths_must_be_pairwise_disjoint",
        "test_fault_injection_automatically_restores_all_three_states",
        "test_marketplace_replace_crash_uses_tracked_backup_and_replays_zero_write",
        "test_marketplace_without_semantic_record_is_extended_without_third_party_drift",
        "test_package_reparse_tree_is_rejected_without_user_writes",
        "test_preview_apply_verify_cli_replay_and_explicit_rollback",
        "test_recover_rejects_descriptor_and_journal_tampering_with_zero_write",
        "test_three_state_cas_detects_drift_at_every_swap_and_rolls_back_owned_changes",
        "test_three_way_cas_and_package_mcp_tamper_fail_closed",
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
    assert not EVIDENCE_ROOT.exists(), EVIDENCE_ROOT
    EVIDENCE_ROOT.mkdir(parents=True)
    results: list[dict[str, object]] = []
    discovered = {
        name: function
        for name, function in globals().items()
        if name.startswith("test_") and callable(function)
    }
    validate_discovered_test_names(set(discovered), EXPECTED_TEST_NAMES)
    for name, function in sorted(discovered.items()):
        try:
            function()
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
        "schema": "stage14-personal-plugin-transaction-tests/v1",
        "status": (
            "PASS_STAGE14_PERSONAL_PLUGIN_TRANSACTION"
            if all(item["status"] == "PASS" for item in results)
            else "FAIL_STAGE14_PERSONAL_PLUGIN_TRANSACTION"
        ),
        "pass_count": sum(item["status"] == "PASS" for item in results),
        "fail_count": sum(item["status"] == "FAIL" for item in results),
        "tests": results,
    }
    (EVIDENCE_ROOT / "summary.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(payload, ensure_ascii=False))
    return 0 if payload["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
