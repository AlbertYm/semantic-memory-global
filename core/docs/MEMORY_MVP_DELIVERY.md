# Long-Term Memory MVP Delivery Notes

This note captures the current delivery posture for the graph-style long-term memory MVP.

## Windows Validation

Run the focused semantic memory validation on Windows with:

```cmd
scripts\test-memory-windows.cmd
```

The `.cmd` wrapper runs the PowerShell script with process-local `ExecutionPolicy Bypass`, so it does not require changing machine policy.

Latest local result:

```text
22/22 passed
```

## Implemented MVP Surface

- Single SQLite-backed memory schema: `memory_event`, `memory_item`, `memory_edge`, `memory_vec`, and `memory_fts`.
- Hot path `events` writes a raw event and a `candidate` item only.
- `admin_consolidate` performs deterministic candidate processing, dedup/update/coexist handling, evidence edges, and vector upsert.
- `memories_retrieve` combines structured filters, FTS, vector candidates, evidence expansion, read-time conflict resolution, and hit counter writeback.
- `memory_update_status`, `memory_feedback`, `admin_decay`, and `memory_health` cover lifecycle, feedback, explainable decay, and audit counters.

## Intentional MVP Difference From The Target Framework

The target framework mentions `sqlite-vec` / `vec0`. This codebase keeps the MVP
zero-dependency by storing memory vectors in a regular SQLite table (`memory_vec`)
as an int8 BLOB, with a custom `cbm_cosine_i8` SQL function for the vector path.

The vectors are **signed feature-hashing bag-of-features** embeddings
(`memory_feature_vec`, 256 dims): ASCII text is lowercased and tokenized on word
boundaries, and CJK / multibyte text contributes per-codepoint unigrams plus
adjacent bigrams (Chinese has no word spaces). Shared words / n-grams hash into
overlapping buckets, so cosine carries real lexical-semantic signal — distinct
paraphrases stay close while divergent statements separate. This replaced an
earlier whole-string hash where any two distinct strings were orthogonal noise
(which left the dedup / contradiction detector effectively inert).

This is deliberately lighter than a learned sentence embedding. The repo does
ship a pretrained `nomic-embed-code` table, but it is distilled for *code*
identifiers and pulls in the full semantic/worker-pool stack; wiring it into the
memory path was rejected to keep the MVP store minimal (framework §0 principle 1).
If learned natural-language embeddings become a hard requirement, add them as a
separate task rather than mixing into this stabilization pass.

## Remaining Release Checks

- Confirm local antivirus tooling has not quarantined `scripts/setup.sh` again before release.
- Run the focused Windows memory validation from a clean shell using `scripts\test-memory-windows.cmd`.
- Run broader project tests/builds as time allows after the MVP memory path is green.
