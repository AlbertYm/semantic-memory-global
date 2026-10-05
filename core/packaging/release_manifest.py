from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SERVER_SCHEMA = "https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json"
PRODUCT_NAME = "semantic-memory-mcp"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verified_provenance_witness(
    path: Path | None,
    expected_sha256: str,
    label: str,
) -> dict[str, Any]:
    if path is None:
        raise ValueError(f"{label} path is required")
    if not path.is_absolute():
        raise ValueError(f"{label} path must be absolute")
    try:
        path_before = path.stat(follow_symlinks=False)
        reparse_attribute = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
        if (
            not stat.S_ISREG(path_before.st_mode)
            or getattr(path_before, "st_file_attributes", 0) & reparse_attribute
        ):
            raise ValueError(f"{label} path must be an existing leaf")
        with path.open("rb") as stream:
            handle_before = os.fstat(stream.fileno())
            body = stream.read()
            handle_after = os.fstat(stream.fileno())
        path_after = path.stat(follow_symlinks=False)
    except (OSError, ValueError) as exc:
        if isinstance(exc, ValueError):
            raise
        raise ValueError(f"{label} path must be an existing leaf") from exc
    identity_before = (handle_before.st_dev, handle_before.st_ino)
    identity_after = (handle_after.st_dev, handle_after.st_ino)
    path_identity_after = (path_after.st_dev, path_after.st_ino)
    if (
        identity_before != identity_after
        or identity_after != path_identity_after
        or handle_before.st_size != handle_after.st_size
        or handle_before.st_mtime_ns != handle_after.st_mtime_ns
        or handle_after.st_size != len(body)
    ):
        raise ValueError(f"{label} changed during verified read")
    actual_sha256 = hashlib.sha256(body).hexdigest()
    if actual_sha256 != expected_sha256:
        raise ValueError(f"{label} SHA256 mismatch")
    return {
        "path": str(path),
        "bytes": len(body),
        "sha256": actual_sha256,
    }


def read_version(source_root: Path) -> str:
    data = (source_root / "VERSION").read_bytes()
    if not data.endswith(b"\n") or data.count(b"\n") != 1:
        raise ValueError("VERSION must contain exactly one LF-terminated line")
    version = data.decode("ascii").strip()
    if not version.startswith("v"):
        raise ValueError("VERSION must start with v")
    return version


def public_version(version: str) -> str:
    return version[1:] if version.startswith("v") else version


def now_utc_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")


def expected_server_json(version: str) -> dict[str, Any]:
    v = public_version(version)
    return {
        "$schema": SERVER_SCHEMA,
        "name": "io.github.DeusData/codebase-memory-mcp",
        "title": "Codebase Memory",
        "description": "Local semantic memory MCP server for AI agents.",
        "repository": {
            "url": "https://github.com/DeusData/codebase-memory-mcp",
            "source": "github",
        },
        "websiteUrl": "https://deusdata.github.io/codebase-memory-mcp/",
        "version": v,
        "packages": [
            {
                "registryType": "npm",
                "identifier": "codebase-memory-mcp",
                "version": v,
                "runtimeHint": "npx",
                "transport": {"type": "stdio"},
            },
            {
                "registryType": "pypi",
                "identifier": "codebase-memory-mcp",
                "version": v,
                "runtimeHint": "uvx",
                "transport": {"type": "stdio"},
            },
        ],
    }


def dump_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def rel(path: Path, root: Path) -> str:
    return path.resolve().relative_to(root.resolve()).as_posix()


def file_record(path: Path, root: Path) -> dict[str, Any]:
    return {
        "path": rel(path, root),
        "bytes": path.stat().st_size,
        "sha256": sha256_file(path),
    }


def validate_file_records(
    root: Path,
    records: list[dict[str, Any]],
    *,
    excluded_relative_paths: set[str] | None = None,
) -> dict[str, dict[str, Any]]:
    root = root.resolve()
    excluded = excluded_relative_paths or set()
    by_path: dict[str, dict[str, Any]] = {}
    for record in records:
        relative = record.get("path")
        if not isinstance(relative, str) or not relative or "\\" in relative:
            raise ValueError("manifest file record has an invalid relative path")
        candidate = (root / relative).resolve()
        try:
            candidate.relative_to(root)
        except ValueError as exc:
            raise ValueError(f"manifest file record escapes root: {relative}") from exc
        if relative in by_path:
            raise ValueError(f"duplicate manifest file record: {relative}")
        if not candidate.is_file():
            raise ValueError(f"manifest file record is missing: {relative}")
        if record.get("bytes") != candidate.stat().st_size:
            raise ValueError(f"manifest byte count mismatch: {relative}")
        if record.get("sha256") != sha256_file(candidate):
            raise ValueError(f"manifest SHA256 mismatch: {relative}")
        by_path[relative] = record

    actual = {
        rel(path, root)
        for path in root.rglob("*")
        if path.is_file() and rel(path, root) not in excluded
    }
    if set(by_path) != actual:
        missing = sorted(actual - set(by_path))
        extra = sorted(set(by_path) - actual)
        raise ValueError(f"manifest file set mismatch: missing={missing}, extra={extra}")
    return by_path


def assert_no_absolute_paths(
    payload: Any,
    *,
    location: tuple[str, ...] = (),
    allowed_absolute_fields: frozenset[tuple[str, ...]] = frozenset(),
) -> None:
    if isinstance(payload, dict):
        for key, value in payload.items():
            assert_no_absolute_paths(
                value,
                location=(*location, str(key)),
                allowed_absolute_fields=allowed_absolute_fields,
            )
        return
    if isinstance(payload, list):
        for index, value in enumerate(payload):
            assert_no_absolute_paths(
                value,
                location=(*location, str(index)),
                allowed_absolute_fields=allowed_absolute_fields,
            )
        return
    if isinstance(payload, str):
        if location in allowed_absolute_fields:
            return
        if os.path.isabs(payload) or ":/" in payload or ":\\" in payload:
            raise ValueError(f"absolute path leaked into manifest: {payload}")


def validate_source_server_json(source_root: Path) -> dict[str, Any]:
    version = read_version(source_root)
    actual = json.loads((source_root / "server.json").read_text(encoding="utf-8"))
    expected = expected_server_json(version)
    checks = {
        "schema": actual.get("$schema") == expected["$schema"],
        "version": actual.get("version") == expected["version"],
        "package_versions": [
            pkg.get("version") == expected["version"] for pkg in actual.get("packages", [])
        ],
        "package_count": len(actual.get("packages", [])) == 2,
    }
    ok = all(v is True for v in checks.values() if isinstance(v, bool)) and all(checks["package_versions"])
    return {
        "schema": "stage13-source-server-json-check/v1",
        "status": "PASS" if ok else "FAIL",
        "version": version,
        "public_version": expected["version"],
        "checks": checks,
        "server_json_sha256": sha256_file(source_root / "server.json"),
    }


def portable_manifest(release_root: Path, version: str) -> dict[str, Any]:
    portable_root = release_root / "manager-portable"
    files = [
        file_record(portable_root / "semantic-memory-manager.exe", portable_root),
        file_record(portable_root / "Open Memory Manager.cmd", portable_root),
        file_record(portable_root / "portable-manifest.json", portable_root)
        if (portable_root / "portable-manifest.json").exists()
        else None,
    ]
    return {
        "schema": "stage13-manager-portable-manifest/v1",
        "version": version,
        "release_revision": "rc.1" if version.endswith("-rc.1") else "local",
        "entrypoint": "Open Memory Manager.cmd",
        "command": "semantic-memory-manager.exe manager",
        "runtime_dependencies": [],
        "network_binding": "127.0.0.1:ephemeral",
        "external_assets": False,
        "files": [f for f in files if f],
    }


def release_manifest(
    source_root: Path,
    release_root: Path,
    candidate_exe: Path,
    build_id: str,
    source_manifest_sha256: str | None,
    production_build_summary_sha256: str | None,
    source_manifest_path: Path | None,
    production_build_summary_path: Path | None,
) -> dict[str, Any]:
    if source_manifest_sha256 is None:
        raise ValueError("source manifest SHA256 is required")
    if not re.fullmatch(r"[0-9a-f]{64}", source_manifest_sha256):
        raise ValueError("source manifest SHA256 must be lowercase 64-hex")
    if production_build_summary_sha256 is None:
        raise ValueError("production build summary SHA256 is required")
    if not re.fullmatch(r"[0-9a-f]{64}", production_build_summary_sha256):
        raise ValueError("production build summary SHA256 must be lowercase 64-hex")
    source_manifest_witness = verified_provenance_witness(
        source_manifest_path,
        source_manifest_sha256,
        "source manifest",
    )
    production_build_summary_witness = verified_provenance_witness(
        production_build_summary_path,
        production_build_summary_sha256,
        "production build summary",
    )
    version = read_version(source_root)
    exe_sha = sha256_file(candidate_exe)
    offline_root = release_root / "offline-installer"
    installer_manifest_path = offline_root / "installer-manifest.json"
    payload_manifest_path = offline_root / "payload" / "payload-manifest.json"
    plugin_root = offline_root / "plugin" / "semantic-memory"
    plugin_manifest_path = plugin_root / ".codex-plugin" / "plugin.json"
    plugin_hooks_path = plugin_root / "hooks" / "hooks.json"
    installer_manifest = json.loads(installer_manifest_path.read_text(encoding="utf-8-sig"))
    payload_manifest = json.loads(payload_manifest_path.read_text(encoding="utf-8"))
    plugin_manifest = json.loads(plugin_manifest_path.read_text(encoding="utf-8"))
    if installer_manifest.get("schema") != "stage14-offline-installer-manifest/v1":
        raise ValueError("invalid Stage 14 installer manifest schema")
    if payload_manifest.get("schema") != "stage14-payload-manifest/v1":
        raise ValueError("invalid Stage 14 payload manifest schema")
    if payload_manifest.get("version") != version or installer_manifest.get("version") != version:
        raise ValueError("Stage 14 installer version mismatch")
    if installer_manifest.get("version_id") != payload_manifest.get("version_id"):
        raise ValueError("Stage 14 installer version_id mismatch")
    expected_toolchain = {
        "schema": "stage14-packaging-toolchain/v1",
        "target_os": "windows",
        "target_arch": "x86_64",
        "packaging_runtime": "PowerShell 5.1",
        "manifest_runtime": "Python 3",
        "archive_format": "zip",
        "package_script_sha256": sha256_file(
            source_root / "packaging" / "package_release.ps1"
        ),
        "manifest_generator_sha256": sha256_file(
            source_root / "packaging" / "release_manifest.py"
        ),
    }
    if installer_manifest.get("toolchain") != expected_toolchain:
        raise ValueError("Stage 14 installer toolchain binding mismatch")
    expected_entrypoints = {
        "mcp": "semantic-memory-mcp.exe",
        "hook": "semantic-memory-hook.exe",
        "manager": "semantic-memory-manager.exe",
    }
    if payload_manifest.get("entrypoints") != expected_entrypoints:
        raise ValueError("Stage 14 payload entrypoints mismatch")
    payload_records = validate_file_records(
        payload_manifest_path.parent,
        payload_manifest.get("files", []),
        excluded_relative_paths={"payload-manifest.json"},
    )
    for relative in expected_entrypoints.values():
        record = payload_records.get(relative)
        if not record or record.get("sha256") != exe_sha:
            raise ValueError(f"Stage 14 payload role does not match candidate: {relative}")
    installer_payload = installer_manifest.get("payload", {})
    if installer_payload.get("manifest") != "payload/payload-manifest.json":
        raise ValueError("Stage 14 installer payload manifest path mismatch")
    if installer_payload.get("manifest_sha256") != sha256_file(payload_manifest_path):
        raise ValueError("Stage 14 installer payload manifest SHA256 mismatch")
    if installer_payload.get("entrypoints") != expected_entrypoints:
        raise ValueError("Stage 14 installer payload entrypoints mismatch")
    if installer_payload.get("files") != payload_manifest.get("files"):
        raise ValueError("Stage 14 installer payload file records mismatch")

    required_scripts = (
        "Install-SemanticMemoryV2.ps1",
        "Install-SemanticMemoryPlugin.ps1",
        "SemanticMemory.Common.psm1",
        "semantic-memory-launcher.ps1",
        "Invoke-Stage14Migration.ps1",
        "Repair-SemanticMemory.ps1",
        "Invoke-PackageAcceptance.ps1",
        "README.zh-CN.md",
        "Install Semantic Memory.cmd",
    )
    script_records = validate_file_records(
        offline_root,
        installer_manifest.get("scripts", []),
        excluded_relative_paths={
            "installer-manifest.json",
            "payload/payload-manifest.json",
            *{f"payload/{path}" for path in payload_records},
            *{
                rel(path, offline_root)
                for path in plugin_root.rglob("*")
                if path.is_file()
            },
        },
    )
    if set(script_records) != set(required_scripts):
        raise ValueError("Stage 14 installer script set mismatch")
    if "Install-SemanticMemory.ps1" in script_records:
        raise ValueError("legacy Stage 13 installer must not be packaged")

    plugin_section = installer_manifest.get("personal_plugin", {})
    if plugin_section.get("root") != "plugin/semantic-memory":
        raise ValueError("Stage 14 personal plugin root mismatch")
    if plugin_section.get("manifest") != "plugin/semantic-memory/.codex-plugin/plugin.json":
        raise ValueError("Stage 14 personal plugin manifest path mismatch")
    plugin_transaction = plugin_section.get("transaction", {})
    if (
        plugin_transaction.get("script") != "Install-SemanticMemoryPlugin.ps1"
        or plugin_transaction.get("schema")
        != "stage14-personal-plugin-transaction/v1"
        or plugin_transaction.get("actions")
        != ["Preview", "Apply", "Verify", "Rollback", "Recover"]
        or plugin_transaction.get("cas_preconditions")
        != [
            "ExpectedSourceTreeSha256",
            "ExpectedCacheTreeSha256",
            "ExpectedMarketplaceSha256",
        ]
        or plugin_transaction.get("absent_sentinel") != "ABSENT"
        or plugin_transaction.get("cli_commands")
        != [
            ["plugin", "marketplace", "add", "<personal-marketplace-path>"],
            ["plugin", "add", "semantic-memory@personal"],
        ]
        or plugin_transaction.get("duplicate_mcp_registration_forbidden") is not True
    ):
        raise ValueError("Stage 14 personal plugin transaction contract mismatch")
    plugin_records = validate_file_records(
        offline_root,
        plugin_section.get("files", []),
        excluded_relative_paths={
            "installer-manifest.json",
            "payload/payload-manifest.json",
            *{f"payload/{path}" for path in payload_records},
            *required_scripts,
        },
    )
    expected_plugin_paths = {
        rel(path, offline_root) for path in plugin_root.rglob("*") if path.is_file()
    }
    if set(plugin_records) != expected_plugin_paths:
        raise ValueError("Stage 14 personal plugin file set mismatch")
    if (
        plugin_manifest.get("name") != "semantic-memory"
        or "mcpServers" in plugin_manifest
        or (plugin_root / ".mcp.json").exists()
        or not plugin_hooks_path.is_file()
    ):
        raise ValueError("incomplete Semantic Memory personal plugin")
    portable_zip = release_root / f"semantic-memory-manager-portable-{version}.zip"
    installer_zip = release_root / f"semantic-memory-mcp-offline-installer-{version}.zip"
    files = []
    for path in sorted(p for p in release_root.rglob("*") if p.is_file()):
        if path.name == "release-manifest.json":
            continue
        files.append(file_record(path, release_root))
    payload = {
        "schema": "stage14-release-manifest/v1",
        "product": PRODUCT_NAME,
        "version": version,
        "public_version": public_version(version),
        "release_channel": "rc" if version.endswith("-rc.1") else "final",
        "created_at": now_utc_iso(),
        "build_id": build_id,
        "version_file": {
            "path": "VERSION",
            "sha256": sha256_file(source_root / "VERSION"),
        },
        "source_server_json": {
            "path": "server.json",
            "sha256": sha256_file(source_root / "server.json"),
        },
        "source_manifest_sha256": source_manifest_sha256,
        "production_provenance": {
            "schema": "stage14-production-provenance/v2",
            "candidate_exe_sha256": exe_sha,
            "source_manifest_sha256": source_manifest_sha256,
            "production_build_summary_sha256": production_build_summary_sha256,
            "source_manifest": source_manifest_witness,
            "production_build_summary": production_build_summary_witness,
        },
        "toolchain": expected_toolchain,
        "candidate_exe": {
            "bytes": candidate_exe.stat().st_size,
            "sha256": exe_sha,
        },
        "release_exe": file_record(release_root / "semantic-memory-mcp.exe", release_root),
        "manager_exe": file_record(
            release_root / "manager-portable" / "semantic-memory-manager.exe", release_root
        ),
        "portable_archive": file_record(portable_zip, release_root) if portable_zip.exists() else None,
        "offline_installer": file_record(installer_zip, release_root) if installer_zip.exists() else None,
        "stage14_installer": {
            "manifest": file_record(installer_manifest_path, release_root),
            "payload_manifest": file_record(payload_manifest_path, release_root),
            "payload_version_id": payload_manifest["version_id"],
            "entrypoints": expected_entrypoints,
            "scripts": [
                file_record(offline_root / name, release_root)
                for name in (
                    "Install-SemanticMemoryV2.ps1",
                    "Install-SemanticMemoryPlugin.ps1",
                    "SemanticMemory.Common.psm1",
                    "semantic-memory-launcher.ps1",
                    "Invoke-Stage14Migration.ps1",
                    "Repair-SemanticMemory.ps1",
                    "Invoke-PackageAcceptance.ps1",
                    "README.zh-CN.md",
                    "Install Semantic Memory.cmd",
                )
            ],
        },
        "personal_plugin": {
            "root": "offline-installer/plugin/semantic-memory",
            "plugin_manifest": file_record(plugin_manifest_path, release_root),
            "mcp_registration": "user_managed_config_only",
            "plugin_mcp_config_present": False,
            "transaction": {
                "script": file_record(
                    offline_root / "Install-SemanticMemoryPlugin.ps1", release_root
                ),
                "schema": "stage14-personal-plugin-transaction/v1",
                "actions": ["Preview", "Apply", "Verify", "Rollback", "Recover"],
                "cas_preconditions": [
                    "ExpectedSourceTreeSha256",
                    "ExpectedCacheTreeSha256",
                    "ExpectedMarketplaceSha256",
                ],
                "absent_sentinel": "ABSENT",
                "duplicate_mcp_registration_forbidden": True,
                "cli_commands": [
                    ["plugin", "marketplace", "add", "<personal-marketplace-path>"],
                    ["plugin", "add", "semantic-memory@personal"],
                ],
            },
            "hooks": file_record(plugin_hooks_path, release_root),
            "files": [
                file_record(path, release_root)
                for path in sorted(p for p in plugin_root.rglob("*") if p.is_file())
            ],
        },
        "authenticode_status": "not_signed",
        "public_release_ready": False,
        "real_second_machine_accepted": False,
        "files": files,
    }
    assert_no_absolute_paths(
        payload,
        allowed_absolute_fields=frozenset(
            {
                ("production_provenance", "source_manifest", "path"),
                ("production_provenance", "production_build_summary", "path"),
            }
        ),
    )
    return payload


def stage14_payload_manifest(payload_root: Path, version: str, version_id: str) -> dict[str, Any]:
    if not version_id or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-" for c in version_id):
        raise ValueError("invalid Stage 14 version_id")
    entrypoints = {
        "mcp": "semantic-memory-mcp.exe",
        "hook": "semantic-memory-hook.exe",
        "manager": "semantic-memory-manager.exe",
    }
    for relative in entrypoints.values():
        if not (payload_root / relative).is_file():
            raise ValueError(f"missing Stage 14 payload entrypoint: {relative}")
    files = [
        file_record(path, payload_root)
        for path in sorted(p for p in payload_root.rglob("*") if p.is_file() and p.name != "payload-manifest.json")
    ]
    payload = {
        "schema": "stage14-payload-manifest/v1",
        "version": version,
        "version_id": version_id,
        "entrypoints": entrypoints,
        "files": files,
    }
    assert_no_absolute_paths(payload)
    return payload


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", default=".")
    parser.add_argument("--check-source", action="store_true")
    parser.add_argument("--render-server-json")
    parser.add_argument("--release-root")
    parser.add_argument("--candidate-exe")
    parser.add_argument("--output")
    parser.add_argument("--build-id", default="manual")
    parser.add_argument("--source-manifest-path")
    parser.add_argument("--source-manifest-sha256")
    parser.add_argument("--production-build-summary-path")
    parser.add_argument("--production-build-summary-sha256")
    parser.add_argument("--stage14-payload-root")
    parser.add_argument("--stage14-version-id")
    parser.add_argument("--stage14-payload-output")
    args = parser.parse_args()

    source_root = Path(args.source_root).resolve()
    if args.check_source:
        result = validate_source_server_json(source_root)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if result["status"] == "PASS" else 1
    if args.render_server_json:
        dump_json(Path(args.render_server_json), expected_server_json(read_version(source_root)))
        return 0
    if args.release_root and args.candidate_exe and args.output:
        payload = release_manifest(
            source_root=source_root,
            release_root=Path(args.release_root).resolve(),
            candidate_exe=Path(args.candidate_exe).resolve(),
            build_id=args.build_id,
            source_manifest_sha256=args.source_manifest_sha256,
            production_build_summary_sha256=args.production_build_summary_sha256,
            source_manifest_path=(
                Path(args.source_manifest_path) if args.source_manifest_path else None
            ),
            production_build_summary_path=(
                Path(args.production_build_summary_path)
                if args.production_build_summary_path
                else None
            ),
        )
        dump_json(Path(args.output), payload)
        return 0
    if args.stage14_payload_root and args.stage14_version_id and args.stage14_payload_output:
        payload = stage14_payload_manifest(
            payload_root=Path(args.stage14_payload_root).resolve(),
            version=read_version(source_root),
            version_id=args.stage14_version_id,
        )
        dump_json(Path(args.stage14_payload_output), payload)
        return 0
    parser.error("no action selected")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
