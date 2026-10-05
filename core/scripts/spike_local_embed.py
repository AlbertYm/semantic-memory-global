#!/usr/bin/env python3
"""Spike: can a REAL local sentence-embedding model separate the semantic
positives from the negatives that static-lookup (nomic code vectors + sparse
random) provably cannot?

Go/no-go gate for a local-model integration. Runs entirely offline against a
locally-cached model — no API. Pulls the SAME seed corpus + semantic/negative
cases the C recall eval uses, so the comparison is apples-to-apples.

Decision rule: a model "passes" if there is a cosine floor T such that every
semantic-positive (query vs its target memory) scores >= T AND every negative
(query vs its best memory) scores < T. We report the positive/negative score
bands and whether they're cleanly separable.
"""
import sys, os
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
from memory_recall_cases import SEED_MEMORIES, CASES  # noqa: E402

# Models to try, smallest/most-multilingual first. First locally-available wins.
CANDIDATES = [
    "BAAI/bge-small-zh-v1.5",
    "BAAI/bge-m3",
    "paraphrase-multilingual-MiniLM-L12-v2",
    "all-MiniLM-L6-v2",
]


def load_model():
    from sentence_transformers import SentenceTransformer
    last_err = None
    for name in CANDIDATES:
        try:
            # local_files_only first (offline); fall back to download if allowed.
            try:
                m = SentenceTransformer(name, local_files_only=True)
                print(f"[model] {name} (cached)")
                return m, name
            except Exception:
                m = SentenceTransformer(name)
                print(f"[model] {name} (downloaded)")
                return m, name
        except Exception as e:
            last_err = e
            print(f"[skip] {name}: {type(e).__name__}")
    raise SystemExit(f"no usable model (last error: {last_err})")


def main():
    model, name = load_model()
    mem_texts = [m["content"] for m in SEED_MEMORIES]
    mem_vecs = model.encode(mem_texts, normalize_embeddings=True)

    semantic_pos = []  # (query, expect, sim_to_target, best_other_sim)
    negatives = []     # (query, best_sim)

    for c in CASES:
        q, expect = c["query"], c["expect"]
        qv = model.encode([q], normalize_embeddings=True)[0]
        sims = mem_vecs @ qv
        if expect is None:
            negatives.append((q, float(sims.max())))
            continue
        # target = the memory whose content contains `expect`
        ti = next((i for i, t in enumerate(mem_texts) if expect in t), None)
        if ti is None:
            continue
        # only score the cases that are genuinely "semantic" (query shares few
        # tokens) — but simplest: report all positives; the semantic ones are
        # the low band. Mark whether target is the argmax.
        target_sim = float(sims[ti])
        other = float(max(s for i, s in enumerate(sims) if i != ti))
        is_top = sims.argmax() == ti
        semantic_pos.append((q, expect, target_sim, other, is_top))

    print("\n=== POSITIVES (query vs its target memory) ===")
    for q, expect, ts, other, is_top in semantic_pos:
        flag = "TOP " if is_top else "    "
        print(f"  {flag} sim={ts:.3f} (best_other={other:.3f})  {q[:42]}")
    print("\n=== NEGATIVES (query vs best memory) ===")
    for q, bs in negatives:
        print(f"   max_sim={bs:.3f}  {q[:42]}")

    pos_sims = [ts for _, _, ts, _, _ in semantic_pos]
    neg_sims = [bs for _, bs in negatives]
    top1 = sum(1 for *_, t in semantic_pos if t) / max(len(semantic_pos), 1)

    print("\n=== SEPARABILITY ===")
    print(f"  model              {name}")
    print(f"  positives  min={min(pos_sims):.3f}  mean={np.mean(pos_sims):.3f}  max={max(pos_sims):.3f}")
    print(f"  negatives  min={min(neg_sims):.3f}  mean={np.mean(neg_sims):.3f}  max={max(neg_sims):.3f}")
    print(f"  top1 (target is argmax over corpus): {top1:.3f}")
    gap = min(pos_sims) - max(neg_sims)
    if gap > 0:
        T = (min(pos_sims) + max(neg_sims)) / 2
        print(f"  CLEANLY SEPARABLE: floor T={T:.3f} (gap={gap:.3f})  -> GO")
    else:
        print(f"  NOT cleanly separable: worst_pos={min(pos_sims):.3f} <= best_neg={max(neg_sims):.3f} (gap={gap:.3f})")
        print(f"  but check top1 — if high, vector-as-recall still works via ranking, not a floor")


if __name__ == "__main__":
    main()
