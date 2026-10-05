#!/usr/bin/env python3
"""Memory recall evaluation harness (zhiwei-kb kb_quality.py pattern).

Spawns the MCP server over stdio, seeds a fixed set of memories into an
isolated throwaway project, consolidates, then runs each query case and scores
retrieval quality. Inspired by zhiwei-kb's kb_quality.py: fixed cases, a small
metric set, a saved baseline, and `--check` that exits 1 on regression.

Metrics (per the case set):
    match_rate        fraction of cases whose expected substring is found
                      anywhere in the returned memories
    top1_rate         fraction of cases where the expected substring is the
                      #1 ranked result (ranking quality)
    mrr               mean reciprocal rank of the expected hit
    zero_result_rate  fraction of NON-negative cases that returned nothing
                      (lower is better; negative cases are excluded)
    false_positive    fraction of negative cases (expect=None) that wrongly
                      returned an FTS-sourced hit

Usage:
    python scripts/memory_recall_eval.py [--binary PATH]      # run + print report
    python scripts/memory_recall_eval.py --baseline           # save baseline
    python scripts/memory_recall_eval.py --check               # compare, exit 1 on regression

Exit codes:
    0 - PASS (or report printed)
    1 - regression vs baseline (only with --check)
    2 - harness error (binary missing, server crash, etc.)
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

from memory_recall_cases import CASES, SEED_MEMORIES

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
DEFAULT_BINARY = os.path.join(REPO_ROOT, "build", "c", "semantic-memory-mcp")
BASELINE_PATH = os.path.join(SCRIPT_DIR, "memory_recall_baseline.json")

# Regression tolerance: metrics may not drop by more than this vs baseline.
TOLERANCE = {
    "match_rate": 0.02,
    "top1_rate": 0.05,
    "mrr": 0.03,
    "zero_result_rate": 0.02,   # this one is "higher is worse"
    "false_positive": 0.02,     # higher is worse
}
# Metrics where a HIGHER value is a regression.
HIGHER_IS_WORSE = {"zero_result_rate", "false_positive"}

# A negative-case hit at or above this retrieval_score counts as a false
# positive. Set from observed data: genuine oblique-match positives land
# ~0.24 while unrelated nearest-neighbours reach ~0.30–0.32, so the bands
# overlap — this floor is a reporting line, not a clean separator. Lowering
# it makes the metric stricter (more negatives flagged).
FP_SCORE_FLOOR = 0.30


class MCPClient:
    """Minimal stdio JSON-RPC client for the MCP server binary."""

    def __init__(self, binary):
        # Deterministic isolation: this eval measures PROJECT recall against its
        # own seeded throwaway project. The global (__global__) store is shared
        # mutable state whose contents vary run-to-run, so unioning it would make
        # scores non-deterministic and let unrelated global memories pollute the
        # result set. Force project-only retrieval. (Production unions global; that
        # path is validated separately, not by this regression gate.)
        env = {**os.environ, "CBM_MEMORY_NO_GLOBAL_UNION": "1"}
        self.proc = subprocess.Popen(
            [binary, "serve"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=env,
        )
        self._id = 0

    def _rpc(self, obj):
        self.proc.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                return None
            s = line.decode("utf-8", errors="replace").strip()
            if not s:
                continue
            try:
                msg = json.loads(s)
            except json.JSONDecodeError:
                continue
            if msg.get("id") == obj.get("id"):
                return msg

    def initialize(self):
        self._id += 1
        self._rpc({
            "jsonrpc": "2.0", "id": self._id, "method": "initialize",
            "params": {"protocolVersion": "2025-11-25", "capabilities": {},
                       "clientInfo": {"name": "recall-eval", "version": "1"}},
        })
        self.proc.stdin.write(
            (json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n").encode())
        self.proc.stdin.flush()

    def call(self, name, arguments):
        self._id += 1
        resp = self._rpc({
            "jsonrpc": "2.0", "id": self._id, "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        })
        if not resp or "result" not in resp:
            return {"_error": resp}
        try:
            return json.loads(resp["result"]["content"][0]["text"])
        except (KeyError, IndexError, json.JSONDecodeError):
            return {"_raw": resp}

    def close(self):
        try:
            self.proc.stdin.close()
            self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def setup_project(client):
    """Index a tiny throwaway dir to register a project the memory tools accept."""
    tmpdir = tempfile.mkdtemp(prefix="recall_eval_")
    with open(os.path.join(tmpdir, "seed.py"), "w", encoding="utf-8") as f:
        f.write("def placeholder():\n    return 1\n")
    res = client.call("index_repository", {"repo_path": tmpdir, "mode": "fast"})
    project = res.get("project")
    if not project:
        raise RuntimeError(f"index_repository failed: {res}")
    return project, tmpdir


def seed_memories(client, project):
    # Dataset-integrity guard: every seed MUST actually be written. The write
    # policy gate rejects e.g. a decision/constraint/lesson with no distinct
    # summary (missing_summary / summary_equals_content). A rejected seed silently
    # vanishes from the store, so the eval would score recall against a dataset
    # that is missing those targets — exactly the silent breakage that masked a
    # real regression once. Fail loudly instead: the test data must mirror the
    # production format the gate guarantees.
    rejected = []
    for mem in SEED_MEMORIES:
        args = {"project": project, "payload": mem.get("payload", {})}
        args.update({k: v for k, v in mem.items() if k != "payload"})
        args["project"] = project
        res = client.call("events", args)
        decision = res.get("policy_decision") if isinstance(res, dict) else None
        if res.get("status") == "rejected" or decision == "rejected":
            rejected.append((mem.get("kind"), res.get("policy_reason"),
                             (mem.get("content") or "")[:40]))
    if rejected:
        msg = "; ".join(f"{k}({r}): {c}" for k, r, c in rejected)
        raise RuntimeError(
            f"{len(rejected)} seed(s) REJECTED by write policy — the eval dataset "
            f"is incomplete and its scores would be meaningless. Fix the seed format "
            f"to match the production write gate (e.g. add a distinct summary). "
            f"Rejected: {msg}")
    client.call("admin_consolidate", {"project": project})


def evaluate(client, project):
    cases = CASES
    n_pos = sum(1 for c in cases if c["expect"] is not None)
    n_neg = sum(1 for c in cases if c["expect"] is None)
    hits = top1 = 0
    rr_sum = 0.0
    zero = 0
    false_pos = 0
    details = []

    for c in cases:
        res = client.call("memories_retrieve",
                          {"project": project, "query": c["query"], "limit": 5})
        mems = res.get("memories", []) if isinstance(res, dict) else []
        expect = c["expect"]

        if expect is None:
            # Negative case: a false positive is any hit — regardless of source —
            # scored at or above FP_SCORE_FLOOR. The original "fts-only" rule went
            # blind once vector became a primary path: an unrelated query still
            # pulls back its 5 nearest memories, and (because static mean-pooled
            # embeddings compress all cosines into a narrow ~0.2–0.32 band) those
            # can outscore a genuinely-relevant-but-obliquely-worded hit. So this
            # metric deliberately reports the over-recall instead of hiding it.
            fp_hits = [m for m in mems
                       if (m.get("retrieval_score") or 0.0) >= FP_SCORE_FLOOR]
            fp = len(fp_hits) > 0
            if fp:
                false_pos += 1
            details.append({"query": c["query"], "expect": None,
                            "false_positive": fp, "n": len(mems),
                            "fp_top": round(max((m.get("retrieval_score") or 0.0)
                                                 for m in mems), 3) if mems else 0.0})
            continue

        rank = None
        for i, m in enumerate(mems):
            if expect in (m.get("content") or ""):
                rank = i + 1
                break
        if rank:
            hits += 1
            rr_sum += 1.0 / rank
            if rank == 1:
                top1 += 1
        else:
            if not mems:
                zero += 1
        details.append({"query": c["query"], "expect": expect,
                        "rank": rank, "n": len(mems),
                        "top_src": mems[0].get("retrieval_source") if mems else None})

    return {
        "n_cases": len(cases),
        "n_positive": n_pos,
        "n_negative": n_neg,
        "match_rate": round(hits / max(n_pos, 1), 4),
        "top1_rate": round(top1 / max(n_pos, 1), 4),
        "mrr": round(rr_sum / max(n_pos, 1), 4),
        "zero_result_rate": round(zero / max(n_pos, 1), 4),
        "false_positive": round(false_pos / max(n_neg, 1), 4),
        "_details": details,
    }


def print_report(metrics):
    print("=" * 56)
    print("  Memory Recall Evaluation")
    print("=" * 56)
    print(f"  cases: {metrics['n_cases']} "
          f"(positive={metrics['n_positive']}, negative={metrics['n_negative']})")
    print(f"  match_rate       {metrics['match_rate']:.3f}   (expected hit found)")
    print(f"  top1_rate        {metrics['top1_rate']:.3f}   (expected hit ranked #1)")
    print(f"  mrr              {metrics['mrr']:.3f}   (mean reciprocal rank)")
    print(f"  zero_result_rate {metrics['zero_result_rate']:.3f}   (lower better)")
    print(f"  false_positive   {metrics['false_positive']:.3f}   (lower better)")
    print("-" * 56)
    for d in metrics["_details"]:
        if d.get("expect") is None:
            flag = "FP!" if d.get("false_positive") else "ok "
            print(f"  [neg] {flag} top={d.get('fp_top', 0.0):.3f} n={d['n']:<2} {d['query']}")
        else:
            r = d.get("rank")
            mark = "MISS" if r is None else (f"#{r}")
            print(f"  [{mark:>4}] src={str(d.get('top_src')):<7} {d['query']}")
    print("=" * 56)


def compare_baseline(metrics):
    if not os.path.exists(BASELINE_PATH):
        print(f"FAIL: no baseline at {BASELINE_PATH}; run --baseline first.")
        return False
    with open(BASELINE_PATH, encoding="utf-8") as f:
        base = json.load(f)
    ok = True
    print("--- regression check vs baseline ---")
    for key, tol in TOLERANCE.items():
        cur = metrics[key]
        ref = base.get(key, 0.0)
        if key in HIGHER_IS_WORSE:
            regressed = cur > ref + tol
            arrow = "↑" if cur > ref else ("↓" if cur < ref else "=")
        else:
            regressed = cur < ref - tol
            arrow = "↓" if cur < ref else ("↑" if cur > ref else "=")
        status = "REGRESSION" if regressed else "ok"
        print(f"  {key:<18} base={ref:.3f} cur={cur:.3f} {arrow}  {status}")
        if regressed:
            ok = False
    print("PASS" if ok else "FAIL: recall regressed beyond tolerance")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=DEFAULT_BINARY)
    ap.add_argument("--baseline", action="store_true", help="save current metrics as baseline")
    ap.add_argument("--check", action="store_true", help="compare vs baseline, exit 1 on regression")
    ap.add_argument("--json", action="store_true", help="print metrics as JSON")
    args = ap.parse_args()

    if not os.path.isfile(args.binary):
        # Windows appends .exe to the build artifact.
        if os.path.isfile(args.binary + ".exe"):
            args.binary = args.binary + ".exe"
        else:
            print(f"FAIL: binary not found at {args.binary}")
            return 2

    client = MCPClient(args.binary)
    tmpdir = None
    project = None
    try:
        client.initialize()
        project, tmpdir = setup_project(client)
        seed_memories(client, project)
        metrics = evaluate(client, project)
    except Exception as e:  # noqa: BLE001
        print(f"FAIL: harness error: {e}")
        client.close()
        return 2
    finally:
        # Drop the throwaway index registration before closing the client,
        # then remove the temp dir on disk — otherwise every run leaks a
        # project in the cache plus a recall_eval_* dir.
        if project:
            try:
                client.call("delete_project", {"project": project})
            except Exception:  # noqa: BLE001
                pass
        client.close()
        if tmpdir and os.path.isdir(tmpdir):
            shutil.rmtree(tmpdir, ignore_errors=True)

    if args.json:
        print(json.dumps({k: v for k, v in metrics.items() if k != "_details"},
                         ensure_ascii=False, indent=2))
    else:
        print_report(metrics)

    if args.baseline:
        with open(BASELINE_PATH, "w", encoding="utf-8") as f:
            json.dump({k: v for k, v in metrics.items() if not k.startswith("_")},
                      f, ensure_ascii=False, indent=2)
        print(f"baseline saved -> {BASELINE_PATH}")
        return 0

    if args.check:
        return 0 if compare_baseline(metrics) else 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
