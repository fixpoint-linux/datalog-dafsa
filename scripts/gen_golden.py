#!/usr/bin/env python3
"""
gen_golden.py — one-time generator for tests/data/bge_golden.txt.

Embeds a few fixed strings with a REFERENCE bge-small-en-v1.5 model via
fastembed (ONNX) and writes them as a golden fixture.  Its original in-tree
consumer, the C++ `dl-embed self-test` gate, has been removed together with the
whole embed sidecar, so this is now a REFERENCE FILE: use it to check any
external encoder's numbers (cosine >= 0.9999 against these vectors).  The
vector/embedding tier itself was REMOVED from the engine on 2026-08-11 (agents
query with the words they wrote, so retrieval is lexical), so nothing in-tree
consumes this fixture any more — it is kept purely as reference data.  Run ONCE
on a host with network + fastembed installed (the project venv: build-tmp/venv):

    pip install fastembed
    python3 scripts/gen_golden.py

The fixture is then committed and no Python is needed afterwards.
Format per line:  <text>\\n<384 comma-separated float32 values>\\n
"""
import os
import sys

import numpy as np

TEXTS = [
    "hello world",
    "datalog dafsa vector search",
    "entity_42",
]

def main():
    try:
        from fastembed import TextEmbedding
    except ImportError:
        print("fastembed not installed: pip install fastembed", file=sys.stderr)
        return 1

    model = TextEmbedding(model_name="BAAI/bge-small-en-v1.5")
    out_path = os.path.join(os.path.dirname(__file__), "..",
                            "tests", "data", "bge_golden.txt")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        for t in TEXTS:
            # fastembed 0.8 `embed()` returns a generator of one embedding.
            v = next(model.embed(t))
            assert v.shape == (384,) and v.dtype == np.float32
            norm = np.linalg.norm(v)
            if norm < 1e-12:
                norm = 1e-12
            v = v / norm
            f.write(t + "\n")
            f.write(",".join("%.9g" % x for x in v) + "\n")
    print("wrote", os.path.abspath(out_path))
    return 0

if __name__ == "__main__":
    sys.exit(main())
