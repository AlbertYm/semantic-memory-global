#!/usr/bin/env python3
"""mock_embed_sidecar.py — deterministic stand-in for embed_sidecar.py in tests.

Speaks the same newline-delimited JSON protocol but needs NO model: it derives a
reproducible 1024-d unit vector from a hash of the text, so the C side can
exercise the sidecar transport (spawn, request, parse, quantize, fallback)
without pulling in torch/bge-m3. The vector is deterministic per text, so two
identical texts get cosine 1.0 and different texts get a stable lower score.
"""
import hashlib
import json
import math
import sys

DIM = 1024


def vec_for(text):
    # Expand a digest into DIM floats deterministically, then L2-normalize.
    out = []
    counter = 0
    while len(out) < DIM:
        h = hashlib.sha256((text + "#" + str(counter)).encode("utf-8")).digest()
        for b in h:
            out.append((b / 255.0) * 2.0 - 1.0)  # [-1, 1]
            if len(out) >= DIM:
                break
        counter += 1
    mag = math.sqrt(sum(x * x for x in out)) or 1.0
    return [x / mag for x in out]


def main():
    sys.stderr.write(json.dumps({"ready": True, "dim": DIM, "model": "mock"}) + "\n")
    sys.stderr.flush()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            out = {"id": req.get("id"), "vec": vec_for(req.get("text", ""))}
        except Exception as e:  # noqa: BLE001
            out = {"id": None, "error": str(e)}
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
