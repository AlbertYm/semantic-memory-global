from __future__ import annotations

import argparse
from pathlib import Path


def build_notices(source_root: Path, output: Path) -> None:
    license_text = (source_root / "LICENSE").read_text(encoding="utf-8", errors="replace")
    third_party = (source_root / "THIRD_PARTY.md").read_text(encoding="utf-8", errors="replace")
    text = (
        "semantic-memory-mcp local release notices\n"
        "==========================================\n\n"
        "Primary license\n"
        "---------------\n"
        f"{license_text.strip()}\n\n"
        "Third-party notices\n"
        "-------------------\n"
        f"{third_party.strip()}\n"
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    build_notices(Path(args.source_root), Path(args.output))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
