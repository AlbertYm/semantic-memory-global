from __future__ import annotations

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class PackageLayoutTests(unittest.TestCase):
    def test_packaging_allowlist_files_exist(self) -> None:
        required = [
            "packaging/windows/Install-SemanticMemoryV2.ps1",
            "packaging/windows/Install-SemanticMemoryPlugin.ps1",
            "packaging/windows/SemanticMemory.Common.psm1",
            "packaging/windows/Invoke-Stage14Migration.ps1",
            "packaging/windows/Repair-SemanticMemory.ps1",
            "packaging/windows/Invoke-PackageAcceptance.ps1",
            "packaging/windows/Invoke-CodeSign.ps1",
            "packaging/windows/Install Semantic Memory.cmd",
            "packaging/windows/Open Memory Manager.cmd",
            "packaging/windows/README.zh-CN.md",
            "packaging/templates/server.json.in",
            "packaging/release_manifest.py",
            "packaging/build_sbom.py",
            "packaging/package_release.ps1",
            "packaging/clean-machine/Invoke-CleanMachineAcceptance.ps1",
            "packaging/clean-machine/CLEAN-MACHINE-CHECKLIST.zh-CN.md",
            "packaging/clean-machine/README.zh-CN.md",
            "packaging/signing/signing-policy-v1.json",
            "packaging/signing/Invoke-VerifySignature.ps1",
            "packaging/licenses/build_notices.py",
            "packaging/semantic-memory/.codex-plugin/plugin.json",
            "packaging/semantic-memory/hooks/hooks.json",
            "packaging/semantic-memory/skills/neuroplastic-memory/SKILL.md",
        ]
        for relative in required:
            self.assertTrue((ROOT / relative).exists(), relative)

    def test_packager_is_offline_and_user_level(self) -> None:
        text = (ROOT / "packaging" / "package_release.ps1").read_text(encoding="utf-8")
        banned = re.compile(r"\b(git|curl|Invoke-WebRequest|iwr|wget|npm\s+install|pip\s+install)\b", re.I)
        self.assertIsNone(banned.search(text))
        self.assertNotRegex(text, r"New-Service|sc\.exe|Set-ItemProperty|New-ItemProperty|setx", re.I)

    def test_installer_retain_data_and_purge_guard(self) -> None:
        text = (ROOT / "packaging" / "windows" / "Install-SemanticMemoryV2.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("data_retained_by_default", text)
        self.assertIn("stage14-install-marker/v2", text)
        self.assertIn("semantic-memory-hook.exe", text)
        self.assertIn("semantic-memory-manager.exe", text)
        self.assertNotRegex(text, r"New-Service|sc\.exe|Set-ItemProperty|New-ItemProperty|setx", re.I)

    def test_rc_packager_uses_v2_and_excludes_legacy_installer(self) -> None:
        text = (ROOT / "packaging" / "package_release.ps1").read_text(encoding="utf-8")
        self.assertIn("Install-SemanticMemoryV2.ps1", text)
        self.assertIn("stage14-offline-installer-manifest/v1", text)
        self.assertIn("packaging\\semantic-memory", text)
        self.assertIn("Install-SemanticMemoryPlugin.ps1", text)
        self.assertNotIn("'Install-SemanticMemory.ps1'", text)

    def test_personal_plugin_uses_stable_hooks_without_duplicate_mcp(self) -> None:
        plugin_root = ROOT / "packaging" / "semantic-memory"
        manifest = (plugin_root / ".codex-plugin" / "plugin.json").read_text(encoding="utf-8")
        hooks = (ROOT / "packaging" / "semantic-memory" / "hooks" / "hooks.json").read_text(
            encoding="utf-8"
        )
        self.assertNotIn('"mcpServers"', manifest)
        self.assertFalse((plugin_root / ".mcp.json").exists())
        self.assertIn(r"${CLAUDE_PLUGIN_ROOT}\\scripts\\semantic-memory-hook.ps1", hooks)
        self.assertNotIn(r"%LOCALAPPDATA%", hooks)
        self.assertEqual(hooks.count('"timeout": 2'), 3)

    def test_personal_plugin_transaction_is_cas_guarded_and_mockable(self) -> None:
        text = (
            ROOT / "packaging" / "windows" / "Install-SemanticMemoryPlugin.ps1"
        ).read_text(encoding="utf-8")
        for action in ("Preview", "Apply", "Verify", "Rollback", "Recover"):
            self.assertIn(action, text)
        for parameter in (
            "ExpectedSourceTreeSha256",
            "ExpectedCacheTreeSha256",
            "ExpectedMarketplaceSha256",
        ):
            self.assertIn(parameter, text)
        self.assertIn("semantic-memory@personal", text)
        self.assertIn("'plugin','marketplace','add'", text)
        self.assertIn("REPLAYED_ZERO_WRITE", text)
        self.assertIn("PLUGIN_REPARSE_TREE_FORBIDDEN", text)
        self.assertNotIn(r"C:\Users\ASUS", text)
        self.assertNotIn(r"H:\Codex_H", text)

    def test_clean_machine_contract_keeps_second_machine_false(self) -> None:
        text = (ROOT / "packaging" / "clean-machine" / "Invoke-CleanMachineAcceptance.ps1").read_text(
            encoding="utf-8"
        )
        self.assertIn("real_second_machine_accepted = $false", text)


if __name__ == "__main__":
    unittest.main()
