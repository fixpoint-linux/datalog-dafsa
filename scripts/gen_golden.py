#!/usr/bin/env python3
"""
gen_golden.py — one-time generator for tests/data/bge_golden.txt.

Embeds a few fixed strings with the REFERENCE bge-small-en-v1.5 model via
fastembed (ONNX) and writes them as the golden fixture consumed by
`dl-embed self-test` (cosine >= 0.9999 gate).  Run ONCE on a host with
network + fastembed installed (the project venv: build-tmp/venv):

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
            v = model.embed(t)
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
