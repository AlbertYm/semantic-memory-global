from __future__ import annotations

import json
import locale
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import uuid
import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "packaging" / "package_release.ps1"
ACCEPTANCE = ROOT / "packaging" / "windows" / "Invoke-PackageAcceptance.ps1"
TIMEOUT_SECONDS = 180


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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


def run_script(
    script: Path,
    *arguments: object,
    expected_exit: int | None = 0,
) -> subprocess.CompletedProcess[str]:
    raw = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(script),
            *(str(value) for value in arguments),
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
    if expected_exit is not None:
        assert result.returncode == expected_exit, result.stdout + result.stderr
    return result


def line_number_containing(script: Path, needle: str, occurrence: int = 1) -> int:
    matches = [
        index
        for index, line in enumerate(
            script.read_text(encoding="utf-8").splitlines(), start=1
        )
        if needle in line
    ]
    assert occurrence > 0 and len(matches) >= occurrence, (needle, occurrence, matches)
    return matches[occurrence - 1]


def run_script_with_debugger_barriers(
    root: Path,
    script: Path,
    arguments: list[object],
    phases: list[tuple[int, object]],
    *,
    observe_line: int | None = None,
) -> tuple[subprocess.CompletedProcess[str], list[object], str | None]:
    barrier_root = root / ("debug-barriers-" + uuid.uuid4().hex)
    barrier_root.mkdir()
    wrapper = barrier_root / "wrapper.ps1"
    observed = barrier_root / "observed.txt"
    breakpoint_blocks: list[str] = []
    for line, _action in phases:
        observe = ""
        if observe_line == line:
            observe = (
                "[IO.File]::WriteAllText($env:STAGE14_RACE_OBSERVED, "
                "[string]$ProductionSummary.status, "
                "[Text.UTF8Encoding]::new($false))"
            )
        breakpoint_blocks.append(
            f"""
Set-PSBreakpoint -Script $target -Line {line} -Action {{
    {observe}
    $ready = Join-Path $env:STAGE14_RACE_BARRIER_ROOT '{line}.ready'
    $go = Join-Path $env:STAGE14_RACE_BARRIER_ROOT '{line}.go'
    [IO.File]::WriteAllText($ready, 'ready', [Text.UTF8Encoding]::new($false))
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (-not (Test-Path -LiteralPath $go)) {{
        if ([DateTime]::UtcNow -gt $deadline) {{ throw 'TEST_BARRIER_TIMEOUT' }}
        Start-Sleep -Milliseconds 10
    }}
}} | Out-Null
"""
        )
    wrapper.write_text(
        "$ErrorActionPreference = 'Stop'\n"
        "$target = $env:STAGE14_RACE_TARGET_SCRIPT\n"
        + "".join(breakpoint_blocks)
        + "try {\n"
        + "  $rawArgs = @((ConvertFrom-Json -InputObject $env:STAGE14_RACE_ARGUMENTS) | ForEach-Object { [string]$_ })\n"
        + "  $targetArgs = @{}\n"
        + "  for ($i = 0; $i -lt $rawArgs.Count; $i += 2) {\n"
        + "    if ($i + 1 -ge $rawArgs.Count -or -not $rawArgs[$i].StartsWith('-')) { throw 'TEST_ARGUMENT_PAIR_INVALID' }\n"
        + "    $targetArgs[$rawArgs[$i].TrimStart('-')] = $rawArgs[$i + 1]\n"
        + "  }\n"
        + "  & $target @targetArgs\n"
        + "  if ($null -eq $LASTEXITCODE) { exit 0 }\n"
        + "  exit $LASTEXITCODE\n"
        + "} catch { Write-Error $_; exit 1 }\n",
        encoding="utf-8",
    )
    process = subprocess.Popen(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(wrapper),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={
            **os.environ,
            "STAGE14_RACE_TARGET_SCRIPT": str(script),
            "STAGE14_RACE_ARGUMENTS": json.dumps([str(value) for value in arguments]),
            "STAGE14_RACE_BARRIER_ROOT": str(barrier_root),
            "STAGE14_RACE_OBSERVED": str(observed),
        },
    )
    effects: list[object] = []
    try:
        for line, action in phases:
            ready = barrier_root / f"{line}.ready"
            deadline = time.monotonic() + 90
            while not ready.exists():
                if process.poll() is not None:
                    stdout, stderr = process.communicate()
                    raise AssertionError(decode(stdout) + decode(stderr))
                if time.monotonic() > deadline:
                    process.kill()
                    raise AssertionError(f"debug barrier timeout at line {line}")
                time.sleep(0.01)
            effects.append(action())
            (barrier_root / f"{line}.go").write_text("go", encoding="ascii")
        stdout, stderr = process.communicate(timeout=TIMEOUT_SECONDS)
    except BaseException:
        if process.poll() is None:
            process.kill()
            process.communicate()
        raise
    return (
        subprocess.CompletedProcess(
            process.args,
            process.returncode,
            decode(stdout),
            decode(stderr),
        ),
        effects,
        observed.read_text(encoding="utf-8") if observed.exists() else None,
    )


class PackageReleaseProvenanceContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.gettempdir()) / (
            "stage14-package-provenance-contract-" + uuid.uuid4().hex
        )
        self.root.mkdir()
        self.candidate = self.root / "candidate.exe"
        self.candidate.write_bytes(b"stage14-provenance-candidate\n")
        self.source_manifest = self.root / "source.manifest.json"
        self.source_manifest.write_text('{"schema":"fixture"}\n', encoding="utf-8")
        self.summary = self.root / "production-build-summary.json"
        self.summary.write_text(
            json.dumps(
                {
                    "schema": "stage14-production-build/v2",
                    "status": "PASS_REV8_UI_PRODUCTION_BUILD",
                    "candidate": {
                        "path": str(self.candidate.resolve()),
                        "bytes": self.candidate.stat().st_size,
                        "sha256": sha256_file(self.candidate),
                    },
                    "source_manifest": {
                        "path": str(self.source_manifest.resolve()),
                        "bytes": self.source_manifest.stat().st_size,
                        "sha256": sha256_file(self.source_manifest),
                    },
                    "checks": {"all_inputs_bound": True},
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        if self.root.exists():
            shutil.rmtree(self.root)

    def replace_candidate_with_different_file(self) -> bool:
        moved = self.root / ("candidate-original-" + uuid.uuid4().hex + ".exe")
        try:
            self.candidate.rename(moved)
        except OSError:
            return False
        self.candidate.write_bytes(b"stage14-replaced-candidate\n")
        return True

    def test_package_release_requires_explicit_provenance_before_writes(self) -> None:
        release = self.root / "missing-provenance-release"
        result = run_script(
            PACKAGER,
            "-SourceRoot",
            ROOT,
            "-CandidateExe",
            self.candidate,
            "-ReleaseRoot",
            release,
            expected_exit=None,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ExpectedCandidateSha256", result.stdout + result.stderr)
        self.assertFalse(release.exists())

    def test_package_release_rejects_candidate_hash_mismatch_before_writes(self) -> None:
        release = self.root / "candidate-mismatch-release"
        result = run_script(
            PACKAGER,
            "-SourceRoot",
            ROOT,
            "-CandidateExe",
            self.candidate,
            "-ExpectedCandidateSha256",
            "0" * 64,
            "-ProductionBuildSummaryPath",
            self.summary,
            "-ExpectedProductionBuildSummarySha256",
            sha256_file(self.summary),
            "-SourceManifestPath",
            self.source_manifest,
            "-ExpectedSourceManifestSha256",
            sha256_file(self.source_manifest),
            "-ReleaseRoot",
            release,
            expected_exit=1,
        )
        self.assertIn("CANDIDATE_SHA256_MISMATCH", result.stdout + result.stderr)
        self.assertFalse(release.exists())

    def test_package_release_parses_the_same_summary_snapshot_it_hashed(self) -> None:
        original = self.summary.read_text(encoding="utf-8")
        replacement_value = json.loads(original)
        replacement_value["status"] = "PASS_STAGE14_FINAL_UI_PRODUCTION_BUILD"
        replacement = json.dumps(replacement_value, indent=2) + "\n"
        release = self.root / "summary-snapshot-release"
        parse_line = line_number_containing(PACKAGER, "$ProductionSummary =")
        observe_line = line_number_containing(
            PACKAGER,
            "& python (Join-Path $SourceRoot 'packaging\\release_manifest.py')",
            occurrence=3,
        )
        result, _effects, observed = run_script_with_debugger_barriers(
            self.root,
            PACKAGER,
            [
                "-SourceRoot",
                ROOT,
                "-CandidateExe",
                self.candidate,
                "-ExpectedCandidateSha256",
                sha256_file(self.candidate),
                "-ProductionBuildSummaryPath",
                self.summary,
                "-ExpectedProductionBuildSummarySha256",
                sha256_file(self.summary),
                "-SourceManifestPath",
                self.source_manifest,
                "-ExpectedSourceManifestSha256",
                sha256_file(self.source_manifest),
                "-ReleaseRoot",
                release,
                "-BuildId",
                "stable-summary-snapshot",
            ],
            [
                (parse_line, lambda: self.summary.write_text(replacement, encoding="utf-8")),
                (observe_line, lambda: self.summary.write_text(original, encoding="utf-8")),
            ],
            observe_line=observe_line,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(observed, "PASS_REV8_UI_PRODUCTION_BUILD")

    def test_package_release_copies_candidate_from_the_verified_locked_stream(
        self,
    ) -> None:
        expected_candidate_sha256 = sha256_file(self.candidate)
        release = self.root / "locked-candidate-release"
        copy_line = line_number_containing(
            PACKAGER, "Join-Path $ReleaseRoot 'semantic-memory-mcp.exe'"
        )
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            PACKAGER,
            [
                "-SourceRoot",
                ROOT,
                "-CandidateExe",
                self.candidate,
                "-ExpectedCandidateSha256",
                expected_candidate_sha256,
                "-ProductionBuildSummaryPath",
                self.summary,
                "-ExpectedProductionBuildSummarySha256",
                sha256_file(self.summary),
                "-SourceManifestPath",
                self.source_manifest,
                "-ExpectedSourceManifestSha256",
                sha256_file(self.source_manifest),
                "-ReleaseRoot",
                release,
                "-BuildId",
                "locked-candidate-stream",
            ],
            [(copy_line, self.replace_candidate_with_different_file)],
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(effects, [False])
        self.assertEqual(
            sha256_file(release / "semantic-memory-mcp.exe"),
            expected_candidate_sha256,
        )


class PackageAcceptanceManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.root = Path(tempfile.gettempdir()) / (
            "stage14-package-acceptance-manifest-" + uuid.uuid4().hex
        )
        created = subprocess.run(
            [
                "powershell.exe",
                "-NoLogo",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "[IO.Directory]::CreateDirectory("
                "$env:STAGE14_PACKAGE_ACCEPTANCE_TEST_ROOT) | Out-Null",
            ],
            capture_output=True,
            env={
                **os.environ,
                "STAGE14_PACKAGE_ACCEPTANCE_TEST_ROOT": str(cls.root),
            },
            timeout=30,
        )
        assert created.returncode == 0, decode(created.stdout) + decode(created.stderr)
        cls.candidate = cls.root / "candidate.exe"
        cls.candidate.write_bytes(b"stage14-package-acceptance-candidate\n")
        cls.source_manifest = cls.root / "source.manifest.json"
        cls.source_manifest.write_text('{"schema":"fixture"}\n', encoding="utf-8")
        cls.production_summary = cls.root / "production-build-summary.json"
        cls.production_summary.write_text(
            json.dumps(
                {
                    "schema": "stage14-production-build/v2",
                    "status": "PASS_REV8_UI_PRODUCTION_BUILD",
                    "candidate": {
                        "path": str(cls.candidate.resolve()),
                        "bytes": cls.candidate.stat().st_size,
                        "sha256": sha256_file(cls.candidate),
                    },
                    "source_manifest": {
                        "path": str(cls.source_manifest.resolve()),
                        "bytes": cls.source_manifest.stat().st_size,
                        "sha256": sha256_file(cls.source_manifest),
                    },
                    "checks": {"all_inputs_bound": True},
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        cls.release = cls.root / "release"
        cls.package_run = cls.root / "package-run"
        result = run_script(
            PACKAGER,
            "-SourceRoot",
            ROOT,
            "-CandidateExe",
            cls.candidate,
            *cls.provenance_arguments(),
            "-ReleaseRoot",
            cls.release,
            "-BuildId",
            "manifest-fixture",
            "-RunDir",
            cls.package_run,
        )
        cls.package_result = json.loads(result.stdout)
        assert cls.package_result["status"] == "PASS_STAGE14_PACKAGE_RELEASE"
        package_report = cls.package_run / "package-release-result.json"
        assert package_report.is_file()
        assert json.loads(package_report.read_text(encoding="utf-8"))["status"] == (
            "PASS_STAGE14_PACKAGE_RELEASE"
        )

    @classmethod
    def tearDownClass(cls) -> None:
        if cls.root.exists():
            shutil.rmtree(cls.root)

    @classmethod
    def provenance_arguments(cls) -> list[object]:
        return [
            "-ExpectedCandidateSha256",
            sha256_file(cls.candidate),
            "-ProductionBuildSummaryPath",
            cls.production_summary,
            "-ExpectedProductionBuildSummarySha256",
            sha256_file(cls.production_summary),
            "-SourceManifestPath",
            cls.source_manifest,
            "-ExpectedSourceManifestSha256",
            sha256_file(cls.source_manifest),
        ]

    def copy_release(self, name: str) -> Path:
        target = self.root / name
        shutil.copytree(self.release, target)
        return target

    def create_junction(self, junction: Path, target: Path) -> None:
        created = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(junction), str(target)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)

    def run_acceptance(
        self,
        release: Path,
        name: str,
        *,
        expected_exit: int = 0,
        output: Path | None = None,
    ) -> tuple[subprocess.CompletedProcess[str], dict[str, object]]:
        work = self.root / f"{name}-work"
        arguments: list[object] = [
            "-ReleaseRoot",
            release,
            "-WorkRoot",
            work,
        ]
        if output is not None:
            arguments.extend(("-Output", output))
        result = run_script(
            ACCEPTANCE,
            *arguments,
            expected_exit=expected_exit,
        )
        assert result.stdout.strip(), result.stderr
        return result, json.loads(result.stdout)

    def test_baseline_release_passes_full_acceptance(self) -> None:
        output = self.root / "baseline-acceptance-result.json"
        _, accepted = self.run_acceptance(self.release, "baseline", output=output)
        self.assertEqual(
            accepted["status"],
            "PASS_STAGE14_PACKAGE_ACCEPTANCE",
        )
        self.assertEqual(
            json.loads(output.read_text(encoding="utf-8"))["status"],
            "PASS_STAGE14_PACKAGE_ACCEPTANCE",
        )
        for name in (
            "release_manifest_top_level_exact",
            "release_manifest_version_contract",
            "release_manifest_source_bindings",
            "release_manifest_production_provenance",
            "release_manifest_toolchain",
            "release_manifest_binary_records",
            "release_manifest_archive_records",
            "release_manifest_installer_contract",
            "release_manifest_plugin_transaction",
            "release_manifest_plugin_file_set",
            "release_manifest_file_set_exact",
            "release_manifest_portable_archive_source_exact",
            "release_manifest_installer_archive_source_exact",
        ):
            self.assertTrue(accepted["checks"][name], name)

    def test_manifest_field_file_set_and_archive_tampering_fail_gate(self) -> None:
        mutations = {
            "schema": lambda value, _: value.__setitem__("schema", "tampered/v1"),
            "version": lambda value, _: value.__setitem__("version", "v9.9.9"),
            "source-manifest": lambda value, _: value.__setitem__(
                "source_manifest_sha256", "INVALID"
            ),
            "production-provenance": lambda value, _: value[
                "production_provenance"
            ].__setitem__("production_build_summary_sha256", "INVALID"),
            "toolchain": lambda value, _: value["toolchain"].__setitem__(
                "package_script_sha256", "0" * 64
            ),
            "portable-archive": lambda value, _: value["portable_archive"].__setitem__(
                "sha256", "0" * 64
            ),
            "plugin-transaction-script": lambda value, _: value["personal_plugin"][
                "transaction"
            ]["script"].__setitem__("sha256", "0" * 64),
            "manifest-file-record-missing": lambda value, _: value["files"].pop(),
            "unexpected-release-file": lambda _value, release: (
                release / "unexpected.bin"
            ).write_bytes(b"unexpected\n"),
            "referenced-release-file-missing": lambda _value, release: (
                release / "NOTICES.txt"
            ).unlink(),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                release = self.copy_release(f"tampered-{name}")
                manifest_path = release / "release-manifest.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                mutate(manifest, release)
                manifest_path.write_text(
                    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
                _, rejected = self.run_acceptance(
                    release,
                    f"tampered-{name}",
                    expected_exit=1,
                )
                self.assertEqual(
                    rejected["status"],
                    "FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE",
                )
                self.assertTrue(
                    any(value is False for value in rejected["checks"].values())
                )

    def test_provenance_witness_path_bytes_and_hash_tampering_fail_gate(self) -> None:
        baseline = json.loads(
            (self.release / "release-manifest.json").read_text(encoding="utf-8")
        )["production_provenance"]
        self.assertEqual(baseline["schema"], "stage14-production-provenance/v2")
        self.assertEqual(
            set(baseline),
            {
                "schema",
                "candidate_exe_sha256",
                "source_manifest_sha256",
                "production_build_summary_sha256",
                "source_manifest",
                "production_build_summary",
            },
        )
        mutations = {
            "source-path": lambda value: value["source_manifest"].__setitem__(
                "path", "relative/source.manifest.json"
            ),
            "source-bytes": lambda value: value["source_manifest"].__setitem__(
                "bytes", str(value["source_manifest"]["bytes"])
            ),
            "source-hash": lambda value: value["source_manifest"].__setitem__(
                "sha256", "0" * 64
            ),
            "summary-path": lambda value: value["production_build_summary"].__setitem__(
                "path", "relative/production-build-summary.json"
            ),
            "summary-bytes": lambda value: value[
                "production_build_summary"
            ].__setitem__("bytes", str(value["production_build_summary"]["bytes"])),
            "summary-hash": lambda value: value[
                "production_build_summary"
            ].__setitem__("sha256", "0" * 64),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                release = self.copy_release(f"tampered-witness-{name}")
                manifest_path = release / "release-manifest.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                mutate(manifest["production_provenance"])
                manifest_path.write_text(
                    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
                _, rejected = self.run_acceptance(
                    release,
                    f"tampered-witness-{name}",
                    expected_exit=1,
                )
                self.assertEqual(
                    rejected["status"],
                    "FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE",
                )
                self.assertFalse(
                    rejected["checks"]["release_manifest_production_provenance"]
                )

    def test_all_manifest_bytes_require_nonnegative_json_int32_or_int64(self) -> None:
        mutations = {
            "record-string": lambda value: value["release_exe"].__setitem__(
                "bytes", str(value["release_exe"]["bytes"])
            ),
            "record-double": lambda value: value["manager_exe"].__setitem__(
                "bytes", float(value["manager_exe"]["bytes"])
            ),
            "candidate-bool": lambda value: value["candidate_exe"].__setitem__(
                "bytes", True
            ),
            "candidate-negative": lambda value: value["candidate_exe"].__setitem__(
                "bytes", -1
            ),
            "candidate-overflow-double": lambda value: value[
                "candidate_exe"
            ].__setitem__("bytes", 1e100),
            "witness-string": lambda value: value["production_provenance"][
                "source_manifest"
            ].__setitem__(
                "bytes",
                str(value["production_provenance"]["source_manifest"]["bytes"]),
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                release = self.copy_release(f"tampered-json-bytes-{name}")
                manifest_path = release / "release-manifest.json"
                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                mutate(manifest)
                manifest_path.write_text(
                    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
                result = run_script(
                    ACCEPTANCE,
                    "-ReleaseRoot",
                    release,
                    "-WorkRoot",
                    self.root / f"tampered-json-bytes-{name}-work",
                    expected_exit=None,
                )
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertTrue(
                    result.stdout.strip(),
                    "acceptance must return a structured manifest-gate rejection: "
                    + result.stderr,
                )
                rejected = json.loads(result.stdout)
                self.assertEqual(
                    rejected["status"],
                    "FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE",
                )

    def test_work_root_inside_release_is_rejected_before_creation(self) -> None:
        nested = self.release / "forbidden-work"
        result = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            self.release,
            "-WorkRoot",
            nested,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_ACCEPTANCE_WORK_ROOT_INSIDE_RELEASE_ROOT",
            result.stdout + result.stderr,
        )
        self.assertFalse(nested.exists())

    def test_release_work_and_release_tree_reparse_are_rejected_before_work_write(
        self,
    ) -> None:
        release_junction = self.root / "release-junction"
        self.create_junction(release_junction, self.release)
        release_work = self.root / "release-junction-work"
        release_result = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            release_junction,
            "-WorkRoot",
            release_work,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN",
            release_result.stdout + release_result.stderr,
        )
        self.assertFalse(release_work.exists())
        release_junction.rmdir()

        work_target = self.root / "work-target"
        work_target.mkdir()
        work_junction = self.root / "work-junction"
        self.create_junction(work_junction, work_target)
        work_result = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            self.release,
            "-WorkRoot",
            work_junction,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN",
            work_result.stdout + work_result.stderr,
        )
        self.assertEqual(list(work_target.iterdir()), [])
        work_junction.rmdir()

        tree_release = self.copy_release("release-tree-reparse")
        plugin_root = tree_release / "offline-installer" / "plugin" / "semantic-memory"
        shutil.rmtree(plugin_root)
        self.create_junction(
            plugin_root,
            self.release / "offline-installer" / "plugin" / "semantic-memory",
        )
        tree_work = self.root / "release-tree-reparse-work"
        tree_result = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            tree_release,
            "-WorkRoot",
            tree_work,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_ACCEPTANCE_REPARSE_PATH_FORBIDDEN",
            tree_result.stdout + tree_result.stderr,
        )
        self.assertFalse(tree_work.exists())
        plugin_root.rmdir()

    def test_output_path_conflicts_and_existing_report_fail_before_work_write(self) -> None:
        release_manifest = self.release / "release-manifest.json"
        release_manifest_before = release_manifest.read_bytes()
        conflict_work = self.root / "output-conflict-work"
        conflict = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            self.release,
            "-WorkRoot",
            conflict_work,
            "-Output",
            release_manifest,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_ACCEPTANCE_OUTPUT_PATH_CONFLICT",
            conflict.stdout + conflict.stderr,
        )
        self.assertFalse(conflict_work.exists())
        self.assertEqual(release_manifest.read_bytes(), release_manifest_before)

        existing_output = self.root / "existing-acceptance-result.json"
        sentinel = b"existing-output-must-not-change\n"
        existing_output.write_bytes(sentinel)
        existing_work = self.root / "existing-output-work"
        existing = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            self.release,
            "-WorkRoot",
            existing_work,
            "-Output",
            existing_output,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_ACCEPTANCE_OUTPUT_ALREADY_EXISTS",
            existing.stdout + existing.stderr,
        )
        self.assertFalse(existing_work.exists())
        self.assertEqual(existing_output.read_bytes(), sentinel)

    def test_package_run_dir_conflicts_and_existing_report_are_zero_write(self) -> None:
        conflict_release = self.root / "package-run-conflict-release"
        conflict = run_script(
            PACKAGER,
            "-SourceRoot",
            ROOT,
            "-CandidateExe",
            self.candidate,
            *self.provenance_arguments(),
            "-ReleaseRoot",
            conflict_release,
            "-BuildId",
            "run-dir-conflict",
            "-RunDir",
            conflict_release / "run",
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_RELEASE_RUN_DIR_PATH_CONFLICT",
            conflict.stdout + conflict.stderr,
        )
        self.assertFalse(conflict_release.exists())

        existing_run = self.root / "existing-package-run"
        existing_run.mkdir()
        existing_report = existing_run / "package-release-result.json"
        sentinel = b"existing-package-report-must-not-change\n"
        existing_report.write_bytes(sentinel)
        existing_release = self.root / "existing-report-release"
        existing = run_script(
            PACKAGER,
            "-SourceRoot",
            ROOT,
            "-CandidateExe",
            self.candidate,
            *self.provenance_arguments(),
            "-ReleaseRoot",
            existing_release,
            "-BuildId",
            "existing-report",
            "-RunDir",
            existing_run,
            expected_exit=1,
        )
        self.assertIn(
            "PACKAGE_RELEASE_RESULT_ALREADY_EXISTS",
            existing.stdout + existing.stderr,
        )
        self.assertFalse(existing_release.exists())
        self.assertEqual(existing_report.read_bytes(), sentinel)

    def test_package_release_rejects_reparse_source_before_release_write(self) -> None:
        junction = self.root / "source-junction"
        created = subprocess.run(
            [
                "cmd.exe",
                "/d",
                "/c",
                "mklink",
                "/J",
                str(junction),
                str(ROOT),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )
        self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
        rejected_release = self.root / "reparse-release-must-not-exist"
        result = run_script(
            PACKAGER,
            "-SourceRoot",
            junction,
            "-CandidateExe",
            self.candidate,
            *self.provenance_arguments(),
            "-ReleaseRoot",
            rejected_release,
            "-BuildId",
            "reparse-rejected",
            expected_exit=1,
        )
        self.assertIn("SOURCE_REPARSE_PATH_FORBIDDEN", result.stdout + result.stderr)
        self.assertFalse(rejected_release.exists())
        junction.rmdir()

    def test_package_release_rejects_reparse_release_ancestor_before_any_write(
        self,
    ) -> None:
        target = self.root / "release-ancestor-target"
        target.mkdir()
        junction = self.root / "release-ancestor-junction"
        self.create_junction(junction, target)
        release = junction / "nested" / "release-must-not-exist"
        result = run_script(
            PACKAGER,
            "-SourceRoot",
            ROOT,
            "-CandidateExe",
            self.candidate,
            *self.provenance_arguments(),
            "-ReleaseRoot",
            release,
            "-BuildId",
            "reparse-release-ancestor-rejected",
            expected_exit=None,
        )
        try:
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "PACKAGE_RELEASE_REPARSE_PATH_FORBIDDEN",
                result.stdout + result.stderr,
            )
            self.assertFalse((target / "nested").exists())
        finally:
            junction.rmdir()

    def replace_directory_with_junction(self, path: Path, target: Path) -> bool:
        try:
            path.rmdir()
        except OSError:
            return False
        created = subprocess.run(
            ["cmd.exe", "/d", "/c", "mklink", "/J", str(path), str(target)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        return created.returncode == 0

    def replace_directory_with_equivalent_directory(self, path: Path) -> bool:
        moved = path.with_name(path.name + "-original-" + uuid.uuid4().hex)
        try:
            path.rename(moved)
        except OSError:
            return False
        try:
            shutil.copytree(moved, path)
        except BaseException:
            if not path.exists() and moved.exists():
                moved.rename(path)
            raise
        return True

    def replace_directory_with_external_equivalent_directory(
        self, path: Path
    ) -> bool:
        moved = self.root / (path.name + "-outside-" + uuid.uuid4().hex)
        try:
            path.rename(moved)
        except OSError:
            return False
        try:
            shutil.copytree(moved, path)
        except BaseException:
            if not path.exists() and moved.exists():
                moved.rename(path)
            raise
        return True

    def add_unleased_release_directory(self, release: Path) -> bool:
        inserted = release / ("late-unleased-" + uuid.uuid4().hex)
        inserted.mkdir()
        (inserted / "unexpected.txt").write_text("unexpected\n", encoding="ascii")
        return True

    def test_package_release_preflight_holds_release_parent_identity(self) -> None:
        package_barrier = line_number_containing(
            PACKAGER, "if ((Get-FileSha256 -Path $CandidateExe)"
        )
        release_parent = self.root / "preflight-release-parent"
        release_parent.mkdir()
        release = release_parent / "release"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            PACKAGER,
            [
                "-SourceRoot",
                ROOT,
                "-CandidateExe",
                self.candidate,
                *self.provenance_arguments(),
                "-ReleaseRoot",
                release,
                "-BuildId",
                "preflight-release-parent-race",
            ],
            [
                (
                    package_barrier,
                    lambda: self.replace_directory_with_equivalent_directory(
                        release_parent
                    ),
                )
            ],
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(effects, [False])

    def test_package_release_preflight_holds_existing_run_dir_identity(self) -> None:
        package_barrier = line_number_containing(
            PACKAGER, "if ((Get-FileSha256 -Path $CandidateExe)"
        )
        run_dir = self.root / "preflight-run-dir"
        run_dir.mkdir()
        run_release = self.root / "preflight-run-release"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            PACKAGER,
            [
                "-SourceRoot",
                ROOT,
                "-CandidateExe",
                self.candidate,
                *self.provenance_arguments(),
                "-ReleaseRoot",
                run_release,
                "-RunDir",
                run_dir,
                "-BuildId",
                "preflight-run-dir-race",
            ],
            [
                (
                    package_barrier,
                    lambda: self.replace_directory_with_equivalent_directory(run_dir),
                )
            ],
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(effects, [False])

    def test_package_acceptance_preflight_holds_release_tree_identity(self) -> None:
        acceptance_barrier = line_number_containing(
            ACCEPTANCE, "if (Test-Path -LiteralPath $WorkRoot)"
        )
        acceptance_release = self.copy_release("preflight-acceptance-release")
        release_child = acceptance_release / "manager-portable"
        release_work = self.root / "preflight-acceptance-release-work"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            ["-ReleaseRoot", acceptance_release, "-WorkRoot", release_work],
            [
                (
                    acceptance_barrier,
                    lambda: self.replace_directory_with_equivalent_directory(
                        release_child
                    ),
                )
            ],
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(effects, [False])

    def test_package_acceptance_rejects_release_child_identity_drift_before_lease(
        self,
    ) -> None:
        acceptance_barrier = line_number_containing(
            ACCEPTANCE,
            "$LeaseSink.Add((Enter-SmStableDirectoryLease",
            occurrence=2,
        )
        acceptance_release = self.copy_release("prelease-identity-drift-release")
        release_child = acceptance_release / "manager-portable"
        release_work = self.root / "prelease-identity-drift-work"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            ["-ReleaseRoot", acceptance_release, "-WorkRoot", release_work],
            [
                (
                    acceptance_barrier,
                    lambda: self.replace_directory_with_external_equivalent_directory(
                        release_child
                    ),
                )
            ],
        )
        self.assertEqual(effects, [True])
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "PACKAGE_ACCEPTANCE_RELEASE_IDENTITY_DRIFT",
            result.stdout + result.stderr,
        )
        self.assertFalse(release_work.exists())

    def test_package_acceptance_rejects_directory_inserted_after_release_bfs(
        self,
    ) -> None:
        acceptance_barrier = line_number_containing(
            ACCEPTANCE, "$workRootAnchorLease = Enter-SmStableDirectoryAnchorLease"
        )
        acceptance_release = self.copy_release("post-bfs-directory-drift-release")
        release_work = self.root / "post-bfs-directory-drift-work"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            ["-ReleaseRoot", acceptance_release, "-WorkRoot", release_work],
            [
                (
                    acceptance_barrier,
                    lambda: self.add_unleased_release_directory(acceptance_release),
                )
            ],
        )
        self.assertEqual(effects, [True])
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "PACKAGE_ACCEPTANCE_RELEASE_TREE_DRIFT",
            result.stdout + result.stderr,
        )
        self.assertFalse(release_work.exists())

    def test_package_acceptance_rejects_directory_inserted_before_work_creation_with_output(
        self,
    ) -> None:
        acceptance_barrier = line_number_containing(
            ACCEPTANCE, "Assert-NoReparsePath -Path $WorkRoot"
        )
        acceptance_release = self.copy_release("post-output-directory-drift-release")
        output_parent = self.root / "post-output-directory-drift-output-parent"
        output_parent.mkdir()
        output = output_parent / "result.json"
        release_work = self.root / "post-output-directory-drift-work"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            [
                "-ReleaseRoot",
                acceptance_release,
                "-WorkRoot",
                release_work,
                "-Output",
                output,
            ],
            [
                (
                    acceptance_barrier,
                    lambda: self.add_unleased_release_directory(acceptance_release),
                )
            ],
        )
        self.assertEqual(effects, [True])
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(
            "PACKAGE_ACCEPTANCE_RELEASE_TREE_DRIFT",
            result.stdout + result.stderr,
        )
        self.assertFalse(release_work.exists())

    def test_package_acceptance_preflight_holds_work_parent_identity(self) -> None:
        acceptance_barrier = line_number_containing(
            ACCEPTANCE, "if (Test-Path -LiteralPath $WorkRoot)"
        )
        work_parent = self.root / "preflight-acceptance-work-parent"
        work_parent.mkdir()
        work = work_parent / "work"
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            ["-ReleaseRoot", self.release, "-WorkRoot", work],
            [
                (
                    acceptance_barrier,
                    lambda: self.replace_directory_with_equivalent_directory(
                        work_parent
                    ),
                )
            ],
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(effects, [False])

    def test_manifest_gate_output_parent_is_leased_before_rejection(self) -> None:
        missing_parent_release = self.copy_release(
            "manifest-gate-missing-output-parent"
        )
        missing_manifest = missing_parent_release / "release-manifest.json"
        missing_value = json.loads(missing_manifest.read_text(encoding="utf-8"))
        missing_value["schema"] = "tampered/v1"
        missing_manifest.write_text(
            json.dumps(missing_value, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        missing_output = self.root / "missing-output-parent" / "result.json"
        missing_result = run_script(
            ACCEPTANCE,
            "-ReleaseRoot",
            missing_parent_release,
            "-WorkRoot",
            self.root / "missing-output-parent-work",
            "-Output",
            missing_output,
            expected_exit=None,
        )
        self.assertEqual(
            missing_result.returncode,
            1,
            missing_result.stdout + missing_result.stderr,
        )
        self.assertTrue(missing_result.stdout.strip(), missing_result.stderr)
        self.assertEqual(
            json.loads(missing_result.stdout)["status"],
            "FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE",
        )
        self.assertEqual(
            json.loads(missing_output.read_text(encoding="utf-8"))["status"],
            "FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE",
        )

        race_release = self.copy_release("manifest-gate-output-race")
        race_manifest = race_release / "release-manifest.json"
        race_value = json.loads(race_manifest.read_text(encoding="utf-8"))
        race_value["schema"] = "tampered/v1"
        race_manifest.write_text(
            json.dumps(race_value, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        output_parent = self.root / "manifest-gate-output-parent"
        output_parent.mkdir()
        output_target = self.root / "manifest-gate-output-target"
        output_target.mkdir()
        output = output_parent / "result.json"
        output_line = line_number_containing(
            ACCEPTANCE, "$stream = [System.IO.File]::Open("
        )
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            [
                "-ReleaseRoot",
                race_release,
                "-WorkRoot",
                self.root / "manifest-gate-output-race-work",
                "-Output",
                output,
            ],
            [
                (
                    output_line,
                    lambda: self.replace_directory_with_junction(
                        output_parent, output_target
                    ),
                )
            ],
        )
        try:
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertEqual(effects, [False])
            self.assertEqual(
                json.loads(output.read_text(encoding="utf-8"))["status"],
                "FAIL_STAGE14_PACKAGE_ACCEPTANCE_MANIFEST_GATE",
            )
            self.assertFalse((output_target / "result.json").exists())
        finally:
            if output_parent.exists() and output_parent.is_junction():
                output_parent.rmdir()

    def test_package_release_holds_release_and_run_directory_leases(self) -> None:
        release = self.root / "lease-race-release"
        release_target = self.root / "lease-race-release-target"
        release_target.mkdir()
        release_line = line_number_containing(
            PACKAGER,
            "$directoryLeases.Add((Enter-SmStableDirectoryLease",
            occurrence=2,
        )
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            PACKAGER,
            [
                "-SourceRoot",
                ROOT,
                "-CandidateExe",
                self.candidate,
                *self.provenance_arguments(),
                "-ReleaseRoot",
                release,
                "-BuildId",
                "release-lease-race",
            ],
            [
                (
                    release_line,
                    lambda: self.replace_directory_with_junction(
                        release, release_target
                    ),
                )
            ],
        )
        try:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(effects, [False])
            self.assertEqual(list(release_target.iterdir()), [])
        finally:
            if release.exists() and release.is_junction():
                release.rmdir()

        run_dir = self.root / "lease-race-run"
        run_dir.mkdir()
        run_target = self.root / "lease-race-run-target"
        run_target.mkdir()
        run_release = self.root / "lease-race-run-release"
        result_line = line_number_containing(
            PACKAGER, "$stream = [System.IO.File]::Open("
        )
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            PACKAGER,
            [
                "-SourceRoot",
                ROOT,
                "-CandidateExe",
                self.candidate,
                *self.provenance_arguments(),
                "-ReleaseRoot",
                run_release,
                "-RunDir",
                run_dir,
                "-BuildId",
                "run-lease-race",
            ],
            [
                (
                    result_line,
                    lambda: self.replace_directory_with_junction(run_dir, run_target),
                )
            ],
        )
        try:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(effects, [False])
            self.assertFalse((run_target / "package-release-result.json").exists())
        finally:
            if run_dir.exists() and run_dir.is_junction():
                run_dir.rmdir()

    def test_package_acceptance_holds_work_and_output_parent_leases(self) -> None:
        work = self.root / "acceptance-work-lease-race"
        work_target = self.root / "acceptance-work-lease-target"
        work_target.mkdir()
        work_line = line_number_containing(
            ACCEPTANCE, "$installerRoot = Join-Path $ReleaseRoot 'offline-installer'"
        )
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            ["-ReleaseRoot", self.release, "-WorkRoot", work],
            [
                (
                    work_line,
                    lambda: self.replace_directory_with_junction(work, work_target),
                )
            ],
        )
        try:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(effects, [False])
            self.assertEqual(list(work_target.iterdir()), [])
        finally:
            if work.exists() and work.is_junction():
                work.rmdir()

        output_parent = self.root / "acceptance-output-lease-race"
        output_parent.mkdir()
        output_target = self.root / "acceptance-output-lease-target"
        output_target.mkdir()
        output = output_parent / "result.json"
        output_work = self.root / "acceptance-output-lease-work"
        output_line = line_number_containing(
            ACCEPTANCE, "$stream = [System.IO.File]::Open("
        )
        result, effects, _ = run_script_with_debugger_barriers(
            self.root,
            ACCEPTANCE,
            [
                "-ReleaseRoot",
                self.release,
                "-WorkRoot",
                output_work,
                "-Output",
                output,
            ],
            [
                (
                    output_line,
                    lambda: self.replace_directory_with_junction(
                        output_parent, output_target
                    ),
                )
            ],
        )
        try:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(effects, [False])
            self.assertFalse((output_target / "result.json").exists())
        finally:
            if output_parent.exists() and output_parent.is_junction():
                output_parent.rmdir()


if __name__ == "__main__":
    unittest.main(verbosity=2)
