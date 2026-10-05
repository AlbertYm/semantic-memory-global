from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "packaging"))

from build_sbom import build_sbom


class SbomTests(unittest.TestCase):
    def test_sbom_is_cyclonedx_and_uses_version(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            base = Path(td)
            source = base / "source"
            release = base / "release"
            source.mkdir()
            release.mkdir()
            (source / "VERSION").write_bytes(b"v1.0.0-rc.1\n")
            (release / "semantic-memory-mcp.exe").write_bytes(b"fake-exe")
            sbom = build_sbom(source, release)
            self.assertEqual(sbom["bomFormat"], "CycloneDX")
            self.assertEqual(sbom["metadata"]["component"]["version"], "v1.0.0-rc.1")
            self.assertEqual(sbom["components"][0]["name"], "semantic-memory-mcp.exe")


if __name__ == "__main__":
    unittest.main()
