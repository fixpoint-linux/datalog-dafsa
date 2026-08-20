#!/usr/bin/env python3
"""
oracle.py — S4 int8-vs-float re-rank precision gate

Implements the precision_gate from handoff-no-sidecar-artifact-1:
On REAL bge-small embeddings, feed REAL MIH candidates (from dl_vector_search
via `dl vsearch --cand-only`), compute top-10 by float-exact cosine vs by
int8 cosine over n sampled queries, assert agreement >= 99% (seed-fixed).

Design (what makes it a REAL gate, not a proxy):
  * float_exact side uses the actual bge-small embeddings of the entity names
    (re-embedded here; S3 does not persist raw float vectors).
  * int8 side uses the vectors actually stored in __vec_q__ (read back from
    the store, same bytes the C re-rank reads).
  * candidate sets come from the REAL MIH implementation (dl_vector_search in
    C, surfaced via `dl vsearch --sig ... --ivec ... --cand-only`), NOT a
    top-k proxy.
  * the query is embedded and ITQ-encoded with the same basis/scale the corpus
    was emitted with (qscale from vector_metadata.txt), so int8 scores are
    directly comparable across the candidate set.

If the gate fails, the S3 quantization must use a GLOBAL corpus-wide int8 scale
(F2).  embed.py already does this (quantize_int8_global + qscale metadata); a
corpus emitted before that fix will fail here and that is the intended signal.

Usage (real data — requires fastembed + the bge-small model):
    python3 scripts/oracle.py --db <dbdir> [--n-queries 10000] [--k 10] [--seed 0]

Usage (synthetic self-check, no fastembed/network):
    python3 scripts/oracle.py --synthetic [--n-queries 5000] [--k 10] [--seed 0]
Builds a synthetic DB under build-tmp, runs the identical gate on it, and
prints the agreement.  This exercises the full oracle machinery (store read,
real MIH candidates, float-exact vs int8) without the model download.
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys

import numpy as np

# Constants (MUST match src/vector.h / embed.py)
VEC_D = 384
VEC_C = 256
VEC_M = 16
VEC_W = 16
VEC_SIG_WORDS = VEC_C // 32   # 8
VEC_IVEC_WORDS = VEC_D // 4   # 96

SIG_REL_PREFIX = "__sig"
VEC_Q_REL = "__vec_q__"
ITQ_BASIS_REL = "__itq_basis__"
ENTITY_REL = "entity"
OBS_REL = "observation"


def dl_path():
    script_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(script_dir, "dl")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{cmd[0]} failed (rc={r.returncode}): {r.stderr}")
    return r.stdout


def dl_prefix(db_dir, rel, leading=None, raw=False):
    cmd = [dl_path(), "-d", db_dir, "prefix", rel]
    if raw:
        cmd.append("--raw")
    if leading is not None:
        cmd.extend(str(x) for x in leading)
    return run(cmd).strip().split("\n")


def dl_load(db_dir, csv_path, rel_name):
    run([dl_path(), "-d", db_dir, "load", csv_path, "--rel", rel_name])


def dl_publish(db_dir):
    run([dl_path(), "-d", db_dir, "publish"])


def load_symbols_array(db_dir):
    """symbols.array: line N (1-based) = interned string with sym_id N."""
    name_to_sym_id = {}
    sym_id = 0
    path = os.path.join(db_dir, "symbols.array")
    if os.path.exists(path):
        with open(path, "r") as f:
            for line in f:
                sym_id += 1
                line = line.rstrip("\n")
                if line:
                    name_to_sym_id[line] = sym_id
    return name_to_sym_id


def load_metadata(db_dir):
    meta = {}
    path = os.path.join(db_dir, "vector_metadata.txt")
    if os.path.exists(path):
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if line and "=" in line:
                    k, v = line.split("=", 1)
                    meta[k.strip()] = v.strip()
    return meta


def unpack4_le(packed):
    b0 = (packed >> 0) & 0xFF
    b1 = (packed >> 8) & 0xFF
    b2 = (packed >> 16) & 0xFF
    b3 = (packed >> 24) & 0xFF
    return [b if b < 128 else b - 256 for b in (b0, b1, b2, b3)]


def load_int8_vectors(db_dir):
    """entity_sym_id -> int8 ndarray (VEC_D,) from __vec_q__ (store bytes).

    Uses `dl prefix --raw` so the leading entity sym-id column is the raw u32
    (not the reverse-mapped name).
    """
    vecs = {}
    lines = dl_prefix(db_dir, VEC_Q_REL, raw=True)
    for line in lines:
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        try:
            sym = int(parts[0])
        except ValueError:
            continue
        chunk = int(parts[1])
        packed = int(parts[2])
        if chunk >= VEC_IVEC_WORDS:
            continue
        v = vecs.setdefault(sym, np.zeros(VEC_D, dtype=np.int8))
        i8 = unpack4_le(packed)
        v[chunk * 4: chunk * 4 + 4] = i8
    return vecs


def pack4_le(int8s):
    return (int(int8s[0]) & 0xFF) | ((int(int8s[1]) & 0xFF) << 8) | \
           ((int(int8s[2]) & 0xFF) << 16) | ((int(int8s[3]) & 0xFF) << 24)


def quantize_int8_global(v, gscale):
    if gscale < 1e-12:
        gscale = 1e-12
    return np.clip(v / gscale * 127.0, -127, 127).astype(np.int8)


def float32_to_bits(f):
    return struct.unpack(">I", struct.pack(">f", f))[0]


def bits_to_float32(bits):
    return struct.unpack(">f", struct.pack(">I", bits & 0xFFFFFFFF))[0]


def load_itq_basis(db_dir):
    path = os.path.join(db_dir, "itq_basis.npy")
    if os.path.exists(path):
        return np.load(path).astype(np.float32)
    # fall back: read from __itq_basis__ relation (dim_i, dim_j, bits)
    lines = dl_prefix(db_dir, ITQ_BASIS_REL, raw=True)
    basis = np.zeros((VEC_D, VEC_C), dtype=np.float32)
    n = 0
    for line in lines:
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) < 3:
            continue
        i = int(parts[0]); j = int(parts[1]); bits = int(parts[2])
        if i < VEC_D and j < VEC_C:
            basis[i, j] = bits_to_float32(bits)
            n += 1
    if n == 0:
        raise RuntimeError("no ITQ basis available")
    return basis


def itq_encode(v, B):
    y = np.dot(v, B)
    bits = (y >= 0).astype(np.uint32)
    sig = []
    for wi in range(VEC_SIG_WORDS):
        w = 0
        for b in bits[wi * 32: (wi + 1) * 32]:
            w = (w << 1) | int(b)
        sig.append(w)
    return sig


def band_slice(sig, j):
    return (sig[j // 2] >> ((1 - (j % 2)) * 16)) & 0xFFFF


def sig_hex(sig):
    return "".join(f"{w:08x}" for w in sig)


def ivec_hex(v_int8):
    return "".join(
        f"{pack4_le(v_int8[i*4:(i+1)*4]):08x}" for i in range(VEC_IVEC_WORDS)
    )


def get_mih_candidates(db_dir, q_sig_hex, q_int8_hex, k_cap, radius):
    """REAL MIH candidate sym-ids from dl_vector_search (C), via vsearch
    --cand-only (before re-rank)."""
    out = run([dl_path(), "-d", db_dir, "vsearch", "q",
               "--sig", q_sig_hex, "--ivec", q_int8_hex,
               "--k", str(k_cap), "--radius", str(radius), "--cand-only"])
    syms = []
    for line in out.strip().split("\n"):
        line = line.strip()
        if line and line != "(no results)":
            try:
                syms.append(int(line))
            except ValueError:
                pass
    return syms


def float_topk(q_float, cand_syms, entity_float, k):
    """Top-k candidate sym-ids by float-exact cosine with q_float."""
    scored = []
    for s in cand_syms:
        v = entity_float.get(s)
        if v is None:
            continue
        nq = np.linalg.norm(q_float)
        nv = np.linalg.norm(v)
        if nq < 1e-12 or nv < 1e-12:
            c = 0.0
        else:
            c = float(np.dot(q_float, v) / (nq * nv))
        scored.append((c, s))
    scored.sort(key=lambda t: (-t[0], t[1]))
    return [s for _, s in scored[:k]]


def int8_topk(q_int8, cand_syms, entity_int8, k):
    """Top-k candidate sym-ids by int8 cosine (identical semantics to C
    re-rank: dot_a*|b| vs dot_b*|a| with integer norms)."""
    q = q_int8.astype(np.float64)
    scored = []
    for s in cand_syms:
        v = entity_int8.get(s)
        if v is None:
            continue
        vf = v.astype(np.float64)
        dot = float(np.dot(q, vf))
        nv = float(np.sqrt(np.dot(vf, vf)))
        nq = float(np.sqrt(np.dot(q, q)))
        if nq < 1e-12 or nv < 1e-12:
            c = 0.0
        else:
            c = dot / (nq * nv)
        scored.append((c, s))
    scored.sort(key=lambda t: (-t[0], t[1]))
    return [s for _, s in scored[:k]]


def topk_set(topk_list):
    """Top-k AGREEMENT uses the SET of returned docs (recall@k), the standard
    ANN metric: does the int8 re-rank surface the same top-k documents as
    float-exact, regardless of intra-top-k order.  (The C re-rank emits exact
    cosine order; near-ties below int8 resolution can swap within the top-k
    without changing the returned document set — which is what a search
    pipeline cares about.)"""
    return set(topk_list)


def precision_gate(db_dir, n_queries=10000, k=10, seed=0, radius=2,
                   k_cap=200):
    """Run the gate.  Returns (agreement_ratio, n_queries)."""
    rng = np.random.RandomState(seed)

    name_to_sym = load_symbols_array(db_dir)
    sym_to_name = {v: name for name, v in name_to_sym.items()}
    meta = load_metadata(db_dir)
    qscale = None
    try:
        qscale = float(meta.get("qscale", ""))
    except ValueError:
        qscale = None

    # Import fastembed (real data path).
    try:
        import fastembed
    except ImportError:
        raise RuntimeError(
            "fastembed is required for the real oracle. Install it, or use "
            "--synthetic for the headless self-check.")

    model = fastembed.TextEmbedding(model_name="BAAI/bge-small-en-v1.5")
    B = load_itq_basis(db_dir)

    # Entity names -> (sym_id, float embedding).
    entity_float = {}
    for name, sym in name_to_sym.items():
        v = model.embed(name)
        if v.shape != (VEC_D,):
            continue
        norm = np.linalg.norm(v)
        if norm < 1e-12:
            norm = 1e-12
        entity_float[sym] = v / norm

    entity_int8 = load_int8_vectors(db_dir)

    # Entities present in both sides (a gate candidate).
    common = sorted(set(entity_float) & set(entity_int8))
    if len(common) < k:
        print("  (too few entities with both float+int8 sides to run the gate)")
        return 0.0, 0

    # If no qscale (pre-F2 corpus), the int8 side is per-vector-scaled and the
    # gate will (correctly) fail — report that loudly.
    if qscale is None:
        print("  WARNING: no qscale in vector_metadata.txt (pre-F2 corpus).",
              "The int8 side is per-vector-scaled; gate will likely fail.")

    agreed = 0
    n_run = 0
    for i in range(n_queries):
        # Query = a random entity name (re-embedded), so the true float NNs
        # are well-defined over the corpus.
        sym = common[rng.randint(len(common))]
        q_float = entity_float[sym]

        # Encode the query exactly as the CLI/encode path would.
        sig = itq_encode(q_float, B)
        q_int8 = quantize_int8_global(q_float, qscale) if qscale \
            else (q_float / (np.max(np.abs(q_float)) + 1e-12) * 127.0)\
            .clip(-127, 127).astype(np.int8)

        cands = get_mih_candidates(db_dir, sig_hex(sig), ivec_hex(q_int8),
                                   k_cap, radius)
        if len(cands) < k:
            continue

        ft = float_topk(q_float, cands, entity_float, k)
        it = int8_topk(q_int8, cands, entity_int8, k)
        n_run += 1
        if topk_set(ft) == topk_set(it):
            agreed += 1

    if n_run == 0:
        print("  (no queries produced >= k candidates)")
        return 0.0, 0

    return agreed / n_run, n_run


# ─── Synthetic self-check (headless, no fastembed/network) ──────────────────

def build_synthetic_db(base, n_ent=300, seed=42, pert=0.02):
    """Build a synthetic vector DB under build-tmp and return its path.

    The embeddings are generated in tight CLUSTERS (each entity is a small
    perturbation of a shared centroid), so MIH signatures recall many
    candidates per query — mirroring how real bge-small embeddings cluster.
    This makes the synthetic gate exercise the real machinery (store read,
    dl_vector_search candidate retrieval, float-exact vs int8 agreement).
    """
    db_dir = os.path.join(base, "oracle-synth")
    shutil.rmtree(db_dir, ignore_errors=True)
    os.makedirs(db_dir, exist_ok=True)

    rng = np.random.RandomState(seed)

    entity_csv = os.path.join(db_dir, "entity.csv")
    with open(entity_csv, "w") as f:
        for i in range(n_ent):
            f.write(f"ent{i},doc\n")
    dl_load(db_dir, entity_csv, ENTITY_REL)
    dl_publish(db_dir)

    name_to_sym = load_symbols_array(db_dir)
    # Only the entity names are our gate corpus (sym_ids of ent0..entN-1).
    entities = [name_to_sym[f"ent{i}"] for i in range(n_ent)]

    # Clustered float embeddings: n_clusters centroids, each entity a small
    # perturbation of one centroid (unit-norm).  bge-small-like locality.
    n_clusters = max(1, n_ent // 10)
    centers = rng.randn(n_clusters, VEC_D).astype(np.float32)
    centers /= np.linalg.norm(centers, axis=1, keepdims=True)
    X = np.empty((n_ent, VEC_D), dtype=np.float32)
    for i in range(n_ent):
        c = centers[i % n_clusters]
        X[i] = c + pert * rng.randn(VEC_D).astype(np.float32)
        X[i] /= np.linalg.norm(X[i])
    entity_float = {sym: X[i] for i, sym in enumerate(entities)}

    gscale = float(np.max(np.abs(X)))
    # Fit an ITQ basis on the corpus (exactly as embed.py does) so signatures
    # are locality-preserving and MIH recalls cluster members at a small
    # radius — mirroring the real-data gate.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import embed
    B = embed.fit_itq_basis(X)
    B = B.astype(np.float32)

    # Emit sig_j + vec_q + itq_basis CSVs.
    sig_rows = {j: [] for j in range(VEC_M)}
    vec_rows = []
    basis_rows = []
    for i, sym in enumerate(entities):
        v = X[i]
        sig = itq_encode(v, B)
        for j in range(VEC_M):
            sig_rows[j].append((band_slice(sig, j), sym))
        vi8 = quantize_int8_global(v, gscale)
        for c in range(VEC_IVEC_WORDS):
            vec_rows.append((sym, c, pack4_le(vi8[c * 4: c * 4 + 4])))
    for i in range(VEC_D):
        for j in range(VEC_C):
            basis_rows.append((i, j, float32_to_bits(float(B[i, j]))))

    for j in range(VEC_M):
        p = os.path.join(db_dir, f"sig{j}.csv")
        with open(p, "w") as f:
            for r in sig_rows[j]:
                f.write(f"{r[0]},{r[1]}\n")
        dl_load(db_dir, p, f"{SIG_REL_PREFIX}{j}__")
    p = os.path.join(db_dir, "vecq.csv")
    with open(p, "w") as f:
        for r in vec_rows:
            f.write(f"{r[0]},{r[1]},{r[2]}\n")
    dl_load(db_dir, p, VEC_Q_REL)
    p = os.path.join(db_dir, "basis.csv")
    with open(p, "w") as f:
        for r in basis_rows:
            f.write(f"{r[0]},{r[1]},{r[2]}\n")
    dl_load(db_dir, p, ITQ_BASIS_REL)
    dl_publish(db_dir)

    with open(os.path.join(db_dir, "vector_metadata.txt"), "w") as f:
        f.write(f"D={VEC_D}\nc={VEC_C}\nm={VEC_M}\nqscale={gscale}\n")

    np.save(os.path.join(db_dir, "itq_basis.npy"), B)
    np.save(os.path.join(db_dir, "oracle_float.npy"),
            np.stack([entity_float[s] for s in entities]))
    with open(os.path.join(db_dir, "oracle_syms.txt"), "w") as f:
        for s in entities:
            f.write(f"{s}\n")
    return db_dir


def run_synthetic(base, n_queries, k, seed, radius, k_cap):
    db_dir = build_synthetic_db(base)
    # Reuse the saved float matrix so the "float-exact" side is independent of
    # the store (mirrors re-embedding in the real path).
    syms = []
    with open(os.path.join(db_dir, "oracle_syms.txt")) as f:
        syms = [int(x) for x in f.read().split()]
    X = np.load(os.path.join(db_dir, "oracle_float.npy"))
    entity_float = {s: X[i] for i, s in enumerate(syms)}
    entity_int8 = load_int8_vectors(db_dir)
    meta = load_metadata(db_dir)
    qscale = float(meta["qscale"])
    B = np.load(os.path.join(db_dir, "itq_basis.npy"))

    rng = np.random.RandomState(seed)
    agreed = n_run = 0
    for _ in range(n_queries):
        # Held-out query: a small perturbation of a random corpus vector, so
        # the query is NOT an exact store member (mirrors a novel user query
        # string whose embedding sits near, not on, a corpus point).
        base = X[rng.randint(len(syms))]
        q_float = base + 0.05 * rng.randn(VEC_D).astype(np.float32)
        norm = np.linalg.norm(q_float)
        if norm < 1e-12:
            norm = 1e-12
        q_float = q_float / norm

        sig = itq_encode(q_float, B)
        q_int8 = quantize_int8_global(q_float, qscale)
        cands = get_mih_candidates(db_dir, sig_hex(sig), ivec_hex(q_int8),
                                   k_cap, radius)
        if len(cands) < k:
            continue
        ft = float_topk(q_float, cands, entity_float, k)
        it = int8_topk(q_int8, cands, entity_int8, k)
        n_run += 1
        if topk_set(ft) == topk_set(it):
            agreed += 1
    if n_run == 0:
        return 0.0, 0
    return agreed / n_run, n_run


def main():
    ap = argparse.ArgumentParser(
        description="int8 vs float re-rank precision gate (S4)")
    ap.add_argument("--db", help="Database directory (real-data gate)")
    ap.add_argument("--synthetic", action="store_true",
                    help="Headless synthetic self-check (no fastembed)")
    ap.add_argument("--n-queries", type=int, default=10000)
    ap.add_argument("--k", type=int, default=10)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--radius", type=int, default=2)
    ap.add_argument("--k-cap", type=int, default=200,
                    help="candidate cap passed to dl_vector_search")
    args = ap.parse_args()

    if args.synthetic:
        base = os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "build-tmp")
        print("=== Oracle synthetic self-check (no fastembed) ===")
        agreement, n = run_synthetic(
            base, args.n_queries, args.k, args.seed, args.radius, args.k_cap)
    else:
        if not args.db or not os.path.isdir(args.db):
            print(f"Error: --db {args.db} is not a directory", file=sys.stderr)
            print("(or use --synthetic for the headless self-check)",
                  file=sys.stderr)
            sys.exit(1)
        print("=== Oracle real-data gate ===")
        agreement, n = precision_gate(
            args.db, args.n_queries, args.k, args.seed, args.radius,
            args.k_cap)

    print(f"\nPrecision gate results:")
    print(f"  Queries run: {n}")
    print(f"  Agreement ratio: {agreement:.4f}")
    print(f"  Required: >= 0.99")
    if agreement >= 0.99:
        print("  ✓ PASS")
        sys.exit(0)
    else:
        print("  ✗ FAIL")
        print("\n  The int8 quantization is not precise enough.")
        print("  Ensure the corpus was emitted with the GLOBAL int8 scale")
        print("  (embed.py quantize_int8_global + qscale in vector_metadata.txt).")
        sys.exit(1)


if __name__ == "__main__":
    main()
