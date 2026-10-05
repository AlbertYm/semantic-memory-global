from __future__ import annotations

import json
import inspect
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "packaging"))

import release_manifest as release_manifest_module

from release_manifest import (
    dump_json,
    expected_server_json,
    file_record,
    public_version,
    read_version,
    release_manifest,
    sha256_file,
    stage14_payload_manifest,
    validate_source_server_json,
)


class ReleaseManifestTests(unittest.TestCase):
    def make_stage14_release_fixture(self, root: Path) -> tuple[Path, Path]:
        candidate = root / "candidate.exe"
        candidate.write_bytes(b"MZ-stage14-fixture")
        release_root = root / "release"
        offline_root = release_root / "offline-installer"
        payload_root = offline_root / "payload"
        plugin_root = offline_root / "plugin" / "semantic-memory"
        manager_root = release_root / "manager-portable"
        payload_root.mkdir(parents=True)
        (plugin_root / ".codex-plugin").mkdir(parents=True)
        (plugin_root / "hooks").mkdir(parents=True)
        manager_root.mkdir(parents=True)

        (release_root / "semantic-memory-mcp.exe").write_bytes(candidate.read_bytes())
        (manager_root / "semantic-memory-manager.exe").write_bytes(candidate.read_bytes())
        for name in (
            "semantic-memory-mcp.exe",
            "semantic-memory-hook.exe",
            "semantic-memory-manager.exe",
        ):
            (payload_root / name).write_bytes(candidate.read_bytes())
        (payload_root / "server.json").write_text("{}\n", encoding="utf-8")
        payload = stage14_payload_manifest(payload_root, read_version(ROOT), "v1.1.0-rc.1+fixture")
        dump_json(payload_root / "payload-manifest.json", payload)

        (plugin_root / ".codex-plugin" / "plugin.json").write_text(
            json.dumps({"name": "semantic-memory", "version": "1.1.0-rc.1"}) + "\n",
            encoding="utf-8",
        )
        (plugin_root / "hooks" / "hooks.json").write_text("{}\n", encoding="utf-8")

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
        for name in required_scripts:
            (offline_root / name).write_text(f"fixture:{name}\n", encoding="utf-8")

        installer = {
            "schema": "stage14-offline-installer-manifest/v1",
            "version": read_version(ROOT),
            "version_id": payload["version_id"],
            "toolchain": {
                "schema": "stage14-packaging-toolchain/v1",
                "target_os": "windows",
                "target_arch": "x86_64",
                "packaging_runtime": "PowerShell 5.1",
                "manifest_runtime": "Python 3",
                "archive_format": "zip",
                "package_script_sha256": sha256_file(
                    ROOT / "packaging" / "package_release.ps1"
                ),
                "manifest_generator_sha256": sha256_file(
                    ROOT / "packaging" / "release_manifest.py"
                ),
            },
            "payload": {
                "manifest": "payload/payload-manifest.json",
                "manifest_sha256": sha256_file(payload_root / "payload-manifest.json"),
                "entrypoints": payload["entrypoints"],
                "files": payload["files"],
            },
            "scripts": [file_record(offline_root / name, offline_root) for name in required_scripts],
            "personal_plugin": {
                "root": "plugin/semantic-memory",
                "manifest": "plugin/semantic-memory/.codex-plugin/plugin.json",
                "files": [
                    file_record(path, offline_root)
                    for path in sorted(item for item in plugin_root.rglob("*") if item.is_file())
                ],
                "transaction": {
                    "script": "Install-SemanticMemoryPlugin.ps1",
                    "schema": "stage14-personal-plugin-transaction/v1",
                    "actions": ["Preview", "Apply", "Verify", "Rollback", "Recover"],
                    "cas_preconditions": [
                        "ExpectedSourceTreeSha256",
                        "ExpectedCacheTreeSha256",
                        "ExpectedMarketplaceSha256",
                    ],
                    "absent_sentinel": "ABSENT",
                    "cli_commands": [
                        [
                            "plugin",
                            "marketplace",
                            "add",
                            "<personal-marketplace-path>",
                        ],
                        ["plugin", "add", "semantic-memory@personal"],
                    ],
                    "duplicate_mcp_registration_forbidden": True,
                },
            },
        }
        dump_json(offline_root / "installer-manifest.json", installer)
        (release_root / "semantic-memory-manager-portable-v1.1.0-rc.1.zip").write_bytes(b"zip")
        (release_root / "semantic-memory-mcp-offline-installer-v1.1.0-rc.1.zip").write_bytes(b"zip")
        return release_root, candidate

    def make_provenance_witnesses(self, root: Path) -> tuple[Path, Path]:
        source_manifest = root / "source.manifest.json"
        source_manifest.write_text('{"schema":"fixture"}\n', encoding="utf-8")
        production_summary = root / "production-build-summary.json"
        production_summary.write_text('{"status":"PASS"}\n', encoding="utf-8")
        return source_manifest.resolve(), production_summary.resolve()

    def test_version_reader_uses_exact_version_file(self) -> None:
        self.assertEqual(read_version(ROOT), "v1.1.0-rc.1")
        self.assertEqual(public_version("v1.1.0-rc.1"), "1.1.0-rc.1")

    def test_expected_server_json_derives_public_versions(self) -> None:
        server = expected_server_json("v1.1.0-rc.1")
        self.assertEqual(server["version"], "1.1.0-rc.1")
        self.assertEqual({pkg["version"] for pkg in server["packages"]}, {"1.1.0-rc.1"})

    def test_source_server_json_matches_version(self) -> None:
        result = validate_source_server_json(ROOT)
        self.assertEqual(result["status"], "PASS")
        self.assertTrue(result["checks"]["schema"])
        self.assertTrue(all(result["checks"]["package_versions"]))

    def test_stage14_payload_manifest_binds_three_native_roles(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            payload_root = Path(tmp)
            for name in (
                "semantic-memory-mcp.exe",
                "semantic-memory-hook.exe",
                "semantic-memory-manager.exe",
            ):
                (payload_root / name).write_bytes(b"same-candidate")
            payload = stage14_payload_manifest(
                payload_root, "v1.1.0-rc.1", "v1.1.0-rc.1+fixture"
            )
            self.assertEqual(payload["schema"], "stage14-payload-manifest/v1")
            self.assertEqual(
                payload["entrypoints"],
                {
                    "mcp": "semantic-memory-mcp.exe",
                    "hook": "semantic-memory-hook.exe",
                    "manager": "semantic-memory-manager.exe",
                },
            )
            self.assertEqual({record["sha256"] for record in payload["files"]}, {sha256_file(payload_root / "semantic-memory-mcp.exe")})

    def test_stage14_release_manifest_binds_installer_payload_and_plugin(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release_root, candidate = self.make_stage14_release_fixture(root)
            source_manifest, production_summary = self.make_provenance_witnesses(root)
            source_sha256 = sha256_file(source_manifest)
            summary_sha256 = sha256_file(production_summary)
            result = release_manifest(
                ROOT,
                release_root,
                candidate,
                "fixture",
                source_sha256,
                summary_sha256,
                source_manifest,
                production_summary,
            )
            self.assertEqual(result["schema"], "stage14-release-manifest/v1")
            self.assertEqual(result["release_channel"], "rc")
            self.assertEqual(result["candidate_exe"]["sha256"], sha256_file(candidate))
            self.assertEqual(result["stage14_installer"]["payload_version_id"], "v1.1.0-rc.1+fixture")
            self.assertEqual(len(result["stage14_installer"]["scripts"]), 9)
            self.assertEqual(result["personal_plugin"]["root"], "offline-installer/plugin/semantic-memory")
            self.assertEqual(
                result["personal_plugin"]["mcp_registration"], "user_managed_config_only"
            )
            self.assertFalse(result["personal_plugin"]["plugin_mcp_config_present"])
            self.assertEqual(
                result["personal_plugin"]["transaction"]["actions"],
                ["Preview", "Apply", "Verify", "Rollback", "Recover"],
            )
            self.assertFalse(result["public_release_ready"])
            self.assertFalse(result["real_second_machine_accepted"])
            self.assertEqual(
                result["production_provenance"],
                {
                    "schema": "stage14-production-provenance/v2",
                    "candidate_exe_sha256": sha256_file(candidate),
                    "source_manifest_sha256": source_sha256,
                    "production_build_summary_sha256": summary_sha256,
                    "source_manifest": {
                        "path": str(source_manifest),
                        "bytes": source_manifest.stat().st_size,
                        "sha256": source_sha256,
                    },
                    "production_build_summary": {
                        "path": str(production_summary),
                        "bytes": production_summary.stat().st_size,
                        "sha256": summary_sha256,
                    },
                },
            )

    def test_stage14_release_manifest_requires_nonnull_provenance(self) -> None:
        self.assertIn(
            "production_build_summary_sha256",
            inspect.signature(release_manifest).parameters,
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release_root, candidate = self.make_stage14_release_fixture(root)
            source_manifest, production_summary = self.make_provenance_witnesses(root)
            with self.assertRaisesRegex(ValueError, "source manifest SHA256 is required"):
                release_manifest(
                    ROOT,
                    release_root,
                    candidate,
                    "fixture",
                    None,
                    production_build_summary_sha256=sha256_file(production_summary),
                    source_manifest_path=source_manifest,
                    production_build_summary_path=production_summary,
                )
            with self.assertRaisesRegex(
                ValueError, "production build summary SHA256 is required"
            ):
                release_manifest(
                    ROOT,
                    release_root,
                    candidate,
                    "fixture",
                    sha256_file(source_manifest),
                    production_build_summary_sha256=None,
                    source_manifest_path=source_manifest,
                    production_build_summary_path=production_summary,
                )

    def test_stage14_release_manifest_persists_verified_provenance_witnesses(
        self,
    ) -> None:
        parameters = inspect.signature(release_manifest).parameters
        self.assertIn("source_manifest_path", parameters)
        self.assertIn("production_build_summary_path", parameters)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release_root, candidate = self.make_stage14_release_fixture(root)
            source_manifest = root / "source.manifest.json"
            source_manifest.write_text('{"schema":"fixture"}\n', encoding="utf-8")
            production_summary = root / "production-build-summary.json"
            production_summary.write_text('{"status":"PASS"}\n', encoding="utf-8")
            source_sha256 = sha256_file(source_manifest)
            summary_sha256 = sha256_file(production_summary)

            result = release_manifest(
                ROOT,
                release_root,
                candidate,
                "fixture",
                source_sha256,
                summary_sha256,
                source_manifest_path=source_manifest.resolve(),
                production_build_summary_path=production_summary.resolve(),
            )
            self.assertEqual(
                result["production_provenance"],
                {
                    "schema": "stage14-production-provenance/v2",
                    "candidate_exe_sha256": sha256_file(candidate),
                    "source_manifest_sha256": source_sha256,
                    "production_build_summary_sha256": summary_sha256,
                    "source_manifest": {
                        "path": str(source_manifest.resolve()),
                        "bytes": source_manifest.stat().st_size,
                        "sha256": source_sha256,
                    },
                    "production_build_summary": {
                        "path": str(production_summary.resolve()),
                        "bytes": production_summary.stat().st_size,
                        "sha256": summary_sha256,
                    },
                },
            )

            for missing_name, source_path, summary_path in (
                ("source manifest", None, production_summary.resolve()),
                ("production build summary", source_manifest.resolve(), None),
            ):
                with self.subTest(missing=missing_name), self.assertRaisesRegex(
                    ValueError, f"{missing_name} path is required"
                ):
                    release_manifest(
                        ROOT,
                        release_root,
                        candidate,
                        "fixture",
                        source_sha256,
                        summary_sha256,
                        source_manifest_path=source_path,
                        production_build_summary_path=summary_path,
                    )

            for relative_name, source_path, summary_path in (
                ("source manifest", Path("source.manifest.json"), production_summary.resolve()),
                (
                    "production build summary",
                    source_manifest.resolve(),
                    Path("production-build-summary.json"),
                ),
            ):
                with self.subTest(relative=relative_name), self.assertRaisesRegex(
                    ValueError, f"{relative_name} path must be absolute"
                ):
                    release_manifest(
                        ROOT,
                        release_root,
                        candidate,
                        "fixture",
                        source_sha256,
                        summary_sha256,
                        source_manifest_path=source_path,
                        production_build_summary_path=summary_path,
                    )

            source_manifest.write_text('{"schema":"drifted"}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "source manifest SHA256 mismatch"):
                release_manifest(
                    ROOT,
                    release_root,
                    candidate,
                    "fixture",
                    source_sha256,
                    summary_sha256,
                    source_manifest_path=source_manifest.resolve(),
                    production_build_summary_path=production_summary.resolve(),
                )
            source_manifest.write_text('{"schema":"fixture"}\n', encoding="utf-8")
            production_summary.write_text('{"status":"DRIFTED"}\n', encoding="utf-8")
            with self.assertRaisesRegex(
                ValueError, "production build summary SHA256 mismatch"
            ):
                release_manifest(
                    ROOT,
                    release_root,
                    candidate,
                    "fixture",
                    source_sha256,
                    summary_sha256,
                    source_manifest_path=source_manifest.resolve(),
                    production_build_summary_path=production_summary.resolve(),
                )

    def test_verified_provenance_witness_uses_one_stable_body_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            witness = Path(tmp) / "witness.json"
            original_bytes = b'{"status":"A"}\n'
            replacement_bytes = b'{"status":"BBBB"}\n'
            witness.write_bytes(original_bytes)
            expected_sha256 = sha256_file(witness)
            original_fstat = release_manifest_module.os.fstat
            observed_fds: list[int] = []

            def replace_after_first_fstat(fd: int):
                result = original_fstat(fd)
                observed_fds.append(fd)
                if len(observed_fds) == 1:
                    witness.write_bytes(replacement_bytes)
                return result

            with mock.patch.object(
                release_manifest_module.os,
                "fstat",
                side_effect=replace_after_first_fstat,
            ):
                with self.assertRaisesRegex(
                    ValueError, "fixture witness changed during verified read"
                ):
                    release_manifest_module.verified_provenance_witness(
                        witness.resolve(),
                        expected_sha256,
                        "fixture witness",
                    )
            self.assertEqual(len(observed_fds), 2)
            self.assertEqual(observed_fds[0], observed_fds[1])
            self.assertEqual(witness.read_bytes(), replacement_bytes)

    def test_stage14_release_manifest_rejects_tampered_payload_role(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            release_root, candidate = self.make_stage14_release_fixture(root)
            source_manifest, production_summary = self.make_provenance_witnesses(root)
            (release_root / "offline-installer" / "payload" / "semantic-memory-hook.exe").write_bytes(
                b"tampered"
            )
            with self.assertRaisesRegex(ValueError, "manifest (byte count|SHA256) mismatch"):
                release_manifest(
                    ROOT,
                    release_root,
                    candidate,
                    "fixture",
                    sha256_file(source_manifest),
                    production_build_summary_sha256=sha256_file(production_summary),
                    source_manifest_path=source_manifest,
                    production_build_summary_path=production_summary,
                )


if __name__ == "__main__":
    unittest.main()
