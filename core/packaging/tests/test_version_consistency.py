from __future__ import annotations

import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERSION_PATH = ROOT / "VERSION"


def read_version() -> str:
    data = VERSION_PATH.read_bytes()
    if data != b"v1.1.0-rc.1\n":
        raise AssertionError("RC VERSION must be exactly b'v1.1.0-rc.1\\n'")
    return data.decode("ascii").strip()


class VersionConsistencyTests(unittest.TestCase):
    def test_version_file_is_final_single_source_of_truth(self) -> None:
        self.assertEqual(read_version(), "v1.1.0-rc.1")

    def test_makefile_reads_version_file_without_beta_default(self) -> None:
        makefile = (ROOT / "Makefile.cbm").read_text(encoding="utf-8")
        self.assertIn("$(file < $(CBM_VERSION_FILE))", makefile)
        self.assertNotRegex(makefile, r"CBM_PRODUCT_VERSION\s*\?=\s*v0\.12\.0-beta\.1")
        self.assertNotIn("-DCBM_VERSION=\\\"v0.12.0-beta.1\\\"", makefile)
        self.assertIn("-Wl,--no-insert-timestamp", makefile)
        self.assertIn("-Wl,--build-id=none", makefile)
        self.assertIn("-D__DATE__=\\\"Jan\\ 01\\ 1970\\\"", makefile)
        self.assertIn("-D__TIME__=\\\"00:00:00\\\"", makefile)

    def test_source_server_json_matches_version_without_v_prefix(self) -> None:
        version = read_version()
        server = json.loads((ROOT / "server.json").read_text(encoding="utf-8"))
        public_version = version.removeprefix("v")
        self.assertEqual(server["version"], public_version)
        self.assertEqual(server["packages"][0]["version"], public_version)
        self.assertEqual(server["packages"][1]["version"], public_version)

    def test_packaging_entrypoints_exist_and_are_offline(self) -> None:
        required = [
            ROOT / "packaging" / "release_manifest.py",
            ROOT / "packaging" / "build_sbom.py",
            ROOT / "packaging" / "package_release.ps1",
            ROOT / "packaging" / "windows" / "Install-SemanticMemoryV2.ps1",
            ROOT / "packaging" / "windows" / "SemanticMemory.Common.psm1",
            ROOT / "packaging" / "windows" / "Invoke-Stage14Migration.ps1",
            ROOT / "packaging" / "windows" / "Repair-SemanticMemory.ps1",
            ROOT / "packaging" / "windows" / "Invoke-PackageAcceptance.ps1",
            ROOT / "packaging" / "semantic-memory" / ".codex-plugin" / "plugin.json",
        ]
        for path in required:
            self.assertTrue(path.exists(), f"missing {path.relative_to(ROOT)}")

        package_script = (ROOT / "packaging" / "package_release.ps1").read_text(encoding="utf-8")
        banned = re.compile(r"\b(git|curl|Invoke-WebRequest|iwr|wget|npm\s+install|pip\s+install)\b", re.I)
        self.assertIsNone(banned.search(package_script))
        self.assertIn("--stage14-payload-root", package_script)
        self.assertIn("semantic-memory-hook.exe", package_script)
        self.assertIn("semantic-memory-manager.exe", package_script)

    def test_plugin_version_matches_rc_without_v_prefix(self) -> None:
        plugin = json.loads(
            (ROOT / "packaging" / "semantic-memory" / ".codex-plugin" / "plugin.json").read_text(
                encoding="utf-8"
            )
        )
        base_version = read_version().removeprefix("v")
        self.assertRegex(plugin["version"], rf"^{re.escape(base_version)}\+codex\.\d+$")
        self.assertEqual(plugin["version"].count("+codex."), 1)


if __name__ == "__main__":
    unittest.main()
