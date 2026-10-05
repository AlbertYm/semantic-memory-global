from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

from release_manifest import read_version, sha256_file


def component_for(path: Path, release_root: Path) -> dict[str, Any]:
    rel = path.resolve().relative_to(release_root.resolve()).as_posix()
    return {
        "type": "file",
        "name": rel,
        "hashes": [{"alg": "SHA-256", "content": sha256_file(path)}],
    }


def build_sbom(source_root: Path, release_root: Path) -> dict[str, Any]:
    version = read_version(source_root)
    components = [
        component_for(path, release_root)
        for path in sorted(p for p in release_root.rglob("*") if p.is_file())
        if path.name != "sbom.cdx.json"
    ]
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "name": "semantic-memory-mcp",
                "version": version,
            }
        },
        "components": components,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--release-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    payload = build_sbom(Path(args.source_root), Path(args.release_root))
    Path(args.output).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
