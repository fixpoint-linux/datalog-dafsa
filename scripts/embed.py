#!/usr/bin/env python3
"""
embed.py — Vector tier S3: embed pipeline

Fits ITQ basis, embeds entity names, ITQ-encodes to 256-bit signatures,
packs into per-band postings (__sig0__..__sig15__), int8 re-rank vectors
(__vec_q__), and ITQ basis (__itq_basis__).  Emits CSVs, bulk-loads via dl,
and publishes a single atomic snapshot.

Constants MUST match src/vector.h exactly:
  VEC_D = 384   (bge-small embedding dim)
  VEC_C = 256   (ITQ bit-code length)
  VEC_M = 16    (MIH bands)
  VEC_W = 16    (bits/band = ceil(VEC_C/VEC_M))
  VEC_SIG_WORDS = 8    (u32 words for the signature)
  VEC_IVEC_WORDS = 96  (u32 words for int8 vector, 4 int8 packed per u32)

Band layout (C1 — MUST match vector.c:band_slice exactly):
  band j = (sig[j//2] >> ((1-(j%2))*16)) & 0xFFFF
  MSB-first: band 0 = HIGH 16 of sig[0], band 1 = LOW 16 of sig[0], etc.

Usage:
  python3 scripts/embed.py --db <dbdir> [--self-test]
"""

import argparse
import csv
import os
import subprocess
import struct
import sys
import tempfile
import numpy as np

# ─── Constants (MUST match src/vector.h) ───────────────────────────────────────

VEC_D = 384          # embedding dim (bge-small)
VEC_C = 256         # ITQ bit-code length (c bits)
VEC_M = 16          # MIH bands (__sig0__..__sig15__)
VEC_W = 16          # bits/band = ceil(VEC_C/VEC_M)
VEC_SIG_WORDS = VEC_C // 32   # 8 u32, MSB-first
VEC_IVEC_WORDS = VEC_D // 4   # 96 u32, 4 int8 packed little-endian

# Relation names
SIG_REL_PREFIX = "__sig"
VEC_Q_REL = "__vec_q__"
ITQ_BASIS_REL = "__itq_basis__"
ENTITY_REL = "entity"


def band_slice(sig, j):
    """MSB-first band slicing.  MUST match vector.c:band_slice exactly.
    
    sig: list/tuple of VEC_SIG_WORDS uint32 values (MSB-first: sig[0] = bits 255..224)
    j: band index 0..VEC_M-1
    returns: 16-bit band value as a Python int (0..65535)
    """
    word_idx = j // 2
    shift = (1 - (j % 2)) * 16
    return (sig[word_idx] >> shift) & 0xFFFF


def band_set(sig, j, val16):
    """Inverse of band_slice: set band j to val16 (for round-trip test).
    
    sig: mutable list of VEC_SIG_WORDS uint32 values
    j: band index 0..VEC_M-1
    val16: 16-bit value to set
    """
    word_idx = j // 2
    shift = (1 - (j % 2)) * 16
    mask = 0xFFFF << shift
    # Use uint32 arithmetic: mask out the band, then OR in the new value
    word = sig[word_idx]
    # Clear the band bits (using positive mask)
    word = (word & (0xFFFFFFFF ^ mask)) | ((val16 & 0xFFFF) << shift)
    # Ensure it stays as a positive Python int (uint32)
    sig[word_idx] = word & 0xFFFFFFFF


def pack4_le(int8s):
    """Pack 4 int8 values into one uint32, little-endian byte order.
    
    int8s: list/tuple/array of 4 integers in range [-128, 127]
    returns: uint32 as Python int
    """
    assert len(int8s) == 4
    # Convert to list of Python ints, handling numpy int8
    b0 = int(int8s[0]) & 0xFF
    b1 = int(int8s[1]) & 0xFF
    b2 = int(int8s[2]) & 0xFF
    b3 = int(int8s[3]) & 0xFF
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)


def unpack4_le(packed):
    """Unpack uint32 into 4 int8 values (little-endian).
    
    packed: uint32 as Python int
    returns: list of 4 int8 values (signed)
    """
    b0 = (packed >> 0) & 0xFF
    b1 = (packed >> 8) & 0xFF
    b2 = (packed >> 16) & 0xFF
    b3 = (packed >> 24) & 0xFF
    # Sign-extend from 8 bits
    def sext(b):
        return b if b < 128 else b - 256
    return [sext(b0), sext(b1), sext(b2), sext(b3)]


def float32_to_bits(f):
    """Convert a float32 to its bit-pattern as a uint32."""
    return struct.unpack('>I', struct.pack('>f', f))[0]


def bits_to_float32(bits):
    """Convert a uint32 bit-pattern back to float32."""
    return struct.unpack('>f', struct.pack('>I', bits & 0xFFFFFFFF))[0]


# ─── ITQ encoding ─────────────────────────────────────────────────────────────

def fit_itq_basis(embeddings):
    """Fit ITQ basis B (D x c) via PCA(D->c) + iterative orthogonal rotation.
    
    embeddings: numpy array of shape (n_samples, VEC_D) with float32 values
    returns: numpy array of shape (VEC_D, VEC_C) — the encode matrix B
    
    Standard ITQ algorithm:
      1. PCA via SVD of the (unit-norm) embeddings, keep top c components -> W_pca (D x c)
      2. Initialize R = random orthogonal (c x c) via QR
      3. Iterate: B = sign(embeddings @ W_pca @ R); R := V U^T from SVD(B^T @ (embeddings @ W_pca))
      4. Return B_encode = W_pca @ R (D x c)

    NOTE: no centering.  Embeddings are L2-normalized before this call and
    itq_encode projects the same raw (uncentered) vectors, so the rotation R
    is fitted and applied on the SAME distribution (centering would fit R on a
    centered distribution but apply it to uncentered vectors, degrading code
    quality).  (Reviewer F1 fix.)
    """
    n, d = embeddings.shape
    assert d == VEC_D
    
    # PCA via SVD: embeddings = U S Vt  (embeddings are already unit-norm)
    U, S, Vt = np.linalg.svd(embeddings, full_matrices=False)
    
    # PCA projection matrix: Vt[:VEC_C] is (c x D), so W_pca = Vt[:VEC_C].T = (D, c)
    W_pca = Vt[:VEC_C].T  # (D, c)
    
    # Project embeddings to c-dimensional space
    X_pca = embeddings @ W_pca  # (n, c)
    
    # Initialize R as random orthogonal (c x c)
    np.random.seed(42)
    R = np.random.randn(VEC_C, VEC_C)
    Q, _ = np.linalg.qr(R)
    R = Q
    
    # Iterate to find optimal rotation
    max_iters = 50
    for _ in range(max_iters):
        # Project and binarize
        B_proj = X_pca @ R  # (n, c)
        B_sign = np.sign(B_proj)  # (n, c) with values +1, -1, 0
        # Ensure no zeros (use +1 for zero)
        B_sign = np.where(B_sign == 0, 1, B_sign)
        
        # SVD of B_sign^T @ X_pca
        U_b, S_b, Vt_b = np.linalg.svd(B_sign.T @ X_pca, full_matrices=False)
        
        # Update R: R = Vt_b.T @ U_b.T (orthogonal)
        R = Vt_b.T @ U_b.T
    
    # Combined encode matrix: B_encode = W_pca @ R (D x c)
    B_encode = W_pca @ R
    
    return B_encode.astype(np.float32)


def itq_encode(v, B):
    """ITQ encode a normalized vector v (D,) using basis B (D x c).
    
    v: numpy array of shape (VEC_D,) — normalized (unit length)
    B: numpy array of shape (VEC_D, VEC_C) — encode matrix
    returns: list of VEC_SIG_WORDS uint32 values (MSB-first bit packing)
    
    Steps:
      1. Project: y = v @ B  (c-dimensional projection)
      2. Binarize via sign: b = (y >= 0).astype(np.uint32) * 1
      3. Pack bits into uint32 words, MSB-first
    """
    assert v.shape == (VEC_D,)
    assert B.shape == (VEC_D, VEC_C)
    
    # Project
    y = np.dot(v, B)  # (c,)
    
    # Binarize
    bits = (y >= 0).astype(np.uint32)  # (c,) of 0/1
    
    # Pack into uint32 words, MSB-first
    # bits[0] is the MSB of the first word
    sig = []
    for word_idx in range(VEC_SIG_WORDS):
        word_bits = bits[word_idx * 32 : (word_idx + 1) * 32]
        # MSB-first within the word: bits[word_idx*32] is bit 31 of the word
        word = 0
        for bit_idx, b in enumerate(word_bits):
            word = (word << 1) | b
        sig.append(word)
    
    return sig


def quantize_int8(v):
    """Quantize a normalized float32 vector to int8.
    
    v: numpy array of shape (VEC_D,) — normalized
    returns: numpy array of shape (VEC_D,) with int8 values
    """
    # Scale to [-127, 127] preserving sign
    # v / (max(|v|, 1e-12)) * 127
    max_abs = np.max(np.abs(v))
    if max_abs < 1e-12:
        max_abs = 1e-12
    scaled = v / max_abs * 127.0
    return np.clip(scaled, -127, 127).astype(np.int8)


# ─── DL CLI helpers ───────────────────────────────────────────────────────────

def _dl_path():
    """Resolve the dl binary path relative to this script."""
    script_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(script_dir, "dl")


def dl_prefix(db_dir, rel, leading=None):
    """Run dl prefix <rel> [leading...] and return output lines."""
    cmd = [_dl_path(), "-d", db_dir, "prefix", rel]
    if leading is not None:
        cmd.extend(str(x) for x in leading)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"dl prefix failed: {result.stderr}")
    return result.stdout.strip().split('\n')


def load_symbols_array(db_dir):
    """Load symbols.array and return a dict mapping name -> sym_id.
    
    symbols.array format: line N (1-based) = interned string with sym_id N
    Empty line = unallocated slot.
    """
    symbols_path = os.path.join(db_dir, "symbols.array")
    name_to_sym_id = {}
    sym_id = 0
    if os.path.exists(symbols_path):
        with open(symbols_path, 'r') as f:
            for line in f:
                sym_id += 1
                line = line.rstrip('\n')
                if line:  # non-empty line
                    name_to_sym_id[line] = sym_id
    return name_to_sym_id


def dl_load(db_dir, csv_path, rel_name):
    """Run dl load <csv> --rel <name> and return output."""
    cmd = [_dl_path(), "-d", db_dir, "load", csv_path, "--rel", rel_name]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"dl load failed: {result.stderr}")
    return result.stdout.strip()


def dl_publish(db_dir):
    """Run dl publish."""
    cmd = [_dl_path(), "-d", db_dir, "publish"]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"dl publish failed: {result.stderr}")
    return result.stdout.strip()


# ─── CSV emission ─────────────────────────────────────────────────────────────

def write_csv(path, rows):
    """Write a CSV file with raw u32 values (no header).
    
    rows: list of tuples of int values (will be written as bare integers)
    """
    with open(path, 'w', newline='') as f:
        for row in rows:
            f.write(','.join(str(x) for x in row) + '\n')


# ─── Self-test ───────────────────────────────────────────────────────────────

def test_band_slice_roundtrip():
    """Test that band_slice + band_set is a round-trip."""
    print("Testing band_slice round-trip...")
    
    # Create a random 256-bit signature
    np.random.seed(42)
    sig = list(np.random.randint(0, 2**32, VEC_SIG_WORDS))
    
    # Slice all bands
    bands = [band_slice(sig, j) for j in range(VEC_M)]
    
    # Reconstruct
    sig_recon = list(sig)
    for j in range(VEC_M):
        band_set(sig_recon, j, bands[j])
    
    assert sig == sig_recon, f"Round-trip failed: {sig} != {sig_recon}"
    print("  ✓ band_slice round-trip passed")


def test_pack4_roundtrip():
    """Test pack4/unpack4 round-trip."""
    print("Testing pack4/unpack4 round-trip...")
    
    np.random.seed(42)
    for _ in range(100):
        int8s = list(np.random.randint(-128, 127, 4))
        packed = pack4_le(int8s)
        unpacked = unpack4_le(packed)
        assert int8s == unpacked, f"Round-trip failed: {int8s} != {unpacked}"
    
    print("  ✓ pack4/unpack4 round-trip passed")


def test_itq_determinism():
    """Test that ITQ fit+encode is deterministic with fixed seed."""
    print("Testing ITQ determinism...")
    
    # Generate >= 300 samples for full-rank PCA (c=256)
    np.random.seed(42)
    embeddings = np.random.randn(300, VEC_D).astype(np.float32)
    
    # Fit basis
    np.random.seed(42)
    B1 = fit_itq_basis(embeddings)
    
    np.random.seed(42)
    B2 = fit_itq_basis(embeddings)
    
    assert np.allclose(B1, B2), "Basis not deterministic"
    
    # Encode a vector
    v = np.random.randn(VEC_D).astype(np.float32)
    v = v / np.linalg.norm(v)
    
    sig1 = itq_encode(v, B1)
    sig2 = itq_encode(v, B2)
    
    assert sig1 == sig2, f"Encoding not deterministic: {sig1} != {sig2}"
    print("  ✓ ITQ determinism passed")


def test_end_to_end_synthetic():
    """End-to-end test on a synthetic DB with >= 300 entities."""
    print("Testing end-to-end synthetic...")
    
    # Create a temporary DB
    tmp_db = tempfile.mkdtemp(prefix="embed_test_")
    
    try:
        # Create entity relation with 300 test entities for full-rank PCA
        entity_csv = os.path.join(tmp_db, "entity.csv")
        num_entities = 300
        with open(entity_csv, 'w') as f:
            for i in range(num_entities):
                f.write(f"entity_{i},type_{i % 10}\n")
        
        # Load entities
        dl_load(tmp_db, entity_csv, "entity")
        
        # Publish to create symbols.array
        dl_publish(tmp_db)
        
        # Load symbols.array to get name -> sym_id mapping
        name_to_sym_id = load_symbols_array(tmp_db)
        
        # Get entity names and sym_ids
        lines = dl_prefix(tmp_db, "entity")
        entities = []
        for line in lines:
            if not line.strip():
                continue
            parts = line.split()
            if len(parts) >= 1:
                name = parts[0]
                if name in name_to_sym_id:
                    entities.append((name, name_to_sym_id[name]))
        
        print(f"  Found {len(entities)} entities")
        
        # Create synthetic embeddings (deterministic)
        np.random.seed(42)
        embeddings = {}
        for name, sym_id in entities:
            v = np.random.randn(VEC_D).astype(np.float32)
            v = v / np.linalg.norm(v)
            embeddings[name] = v
        
        # Fit ITQ basis
        np.random.seed(42)
        X = np.array([embeddings[name] for name, _ in entities])
        B = fit_itq_basis(X)
        
        # Encode each entity
        sig_facts = {j: [] for j in range(VEC_M)}  # band_value -> entity_sym_id
        vec_q_facts = []  # (entity_sym_id, chunk_idx, packed_u32)
        
        for name, sym_id in entities:
            v = embeddings[name]
            
            # ITQ encode
            sig = itq_encode(v, B)
            
            # Emit sig_j facts
            for j in range(VEC_M):
                band_val = band_slice(sig, j)
                sig_facts[j].append((band_val, sym_id))
            
            # Quantize to int8
            vi8 = quantize_int8(v)
            
            # Emit vec_q facts
            for chunk_idx in range(VEC_IVEC_WORDS):
                chunk = vi8[chunk_idx * 4 : (chunk_idx + 1) * 4]
                packed = pack4_le(chunk)
                vec_q_facts.append((sym_id, chunk_idx, packed))
        
        # Write sig_j CSVs
        for j in range(VEC_M):
            rel_name = f"{SIG_REL_PREFIX}{j}__"
            csv_path = os.path.join(tmp_db, f"sig_{j}.csv")
            write_csv(csv_path, sig_facts[j])
            dl_load(tmp_db, csv_path, rel_name)
            print(f"  Loaded {len(sig_facts[j])} facts into {rel_name}")
        
        # Write vec_q CSV
        vec_q_csv = os.path.join(tmp_db, "vec_q.csv")
        write_csv(vec_q_csv, vec_q_facts)
        dl_load(tmp_db, vec_q_csv, VEC_Q_REL)
        print(f"  Loaded {len(vec_q_facts)} facts into {VEC_Q_REL}")
        
        # Write itq_basis CSV
        basis_facts = []
        for i in range(VEC_D):
            for j in range(VEC_C):
                bits = float32_to_bits(B[i, j])
                basis_facts.append((i, j, bits))
        
        basis_csv = os.path.join(tmp_db, "itq_basis.csv")
        write_csv(basis_csv, basis_facts)
        dl_load(tmp_db, basis_csv, ITQ_BASIS_REL)
        print(f"  Loaded {len(basis_facts)} facts into {ITQ_BASIS_REL}")
        
        # Publish
        dl_publish(tmp_db)
        print("  ✓ Published snapshot")
        
        # Verify: check that we can prefix query sig_0
        sig0_lines = dl_prefix(tmp_db, f"{SIG_REL_PREFIX}0__")
        print(f"  ✓ sig_0 has {len(sig0_lines)} rows")
        
        # Verify band_slice reconstruction
        for name, sym_id in entities:
            v = embeddings[name]
            sig = itq_encode(v, B)
            bands = [band_slice(sig, j) for j in range(VEC_M)]
            sig_recon = list(sig)
            for j in range(VEC_M):
                band_set(sig_recon, j, bands[j])
            assert sig == sig_recon, f"Band reconstruction failed for {name}"
        
        print("  ✓ End-to-end synthetic test passed")
        
    finally:
        # Clean up
        import shutil
        shutil.rmtree(tmp_db, ignore_errors=True)


def run_self_test():
    """Run all self-tests."""
    print("\n=== Running self-tests ===")
    test_band_slice_roundtrip()
    test_pack4_roundtrip()
    test_itq_determinism()
    test_end_to_end_synthetic()
    print("=== All self-tests passed ===\n")


# ─── Main embed pipeline ───────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Vector tier embed pipeline (S3)")
    parser.add_argument("--db", help="Database directory (required unless --self-test)")
    parser.add_argument("--self-test", action="store_true", help="Run self-tests only")
    parser.add_argument("--model", default="BAAI/bge-small-en-v1.5",
                        help="FastEmbed model name (default: BAAI/bge-small-en-v1.5)")
    args = parser.parse_args()
    
    if args.self_test:
        run_self_test()
        return
    
    if args.db is None:
        parser.error("--db is required (unless using --self-test)")
    
    db_dir = args.db
    
    # Check that db_dir exists
    if not os.path.isdir(db_dir):
        print(f"Error: {db_dir} is not a directory", file=sys.stderr)
        sys.exit(1)
    
    # Get all entity names from the DB
    print("Walking entity names...")
    lines = dl_prefix(db_dir, ENTITY_REL)
    
    # Load symbols.array to get name -> sym_id mapping
    name_to_sym_id = load_symbols_array(db_dir)
    
    entities = []  # (name, sym_id)
    for line in lines:
        if not line.strip():
            continue
        parts = line.split()
        if len(parts) >= 1:
            name = parts[0]
            if name in name_to_sym_id:
                entities.append((name, name_to_sym_id[name]))
            else:
                print(f"Warning: could not find sym_id for '{name}' in symbols.array", file=sys.stderr)
    
    print(f"Found {len(entities)} entities")
    
    if len(entities) == 0:
        print("No entities found. Nothing to embed.")
        return
    
    # Try to import fastembed
    try:
        import fastembed
    except ImportError:
        print("Error: fastembed not installed. Install with:", file=sys.stderr)
        print("  pip install fastembed", file=sys.stderr)
        sys.exit(1)
    
    # Embed all entity names
    print("Embedding entity names...")
    model = fastembed.TextEmbedding(model_name=args.model)
    
    embeddings = {}
    for name, sym_id in entities:
        # Embed the name string
        v = model.embed(name)
        # v is a numpy array of shape (D,) with float32 values
        assert v.shape == (VEC_D,), f"Unexpected embedding shape: {v.shape}"
        assert v.dtype == np.float32, f"Unexpected embedding dtype: {v.dtype}"
        
        # Normalize
        norm = np.linalg.norm(v)
        if norm < 1e-12:
            norm = 1e-12
        v = v / norm
        
        embeddings[name] = v
    
    print(f"Embedded {len(embeddings)} entities")
    
    # Fit ITQ basis
    print("Fitting ITQ basis...")
    X = np.array([embeddings[name] for name, _ in entities])
    assert X.shape == (len(entities), VEC_D)
    
    B = fit_itq_basis(X)
    print(f"Basis shape: {B.shape}")
    
    # Persist the basis for S4
    basis_path = os.path.join(db_dir, "itq_basis.npy")
    np.save(basis_path, B)
    print(f"Saved ITQ basis to {basis_path}")
    
    # Also save metadata
    metadata_path = os.path.join(db_dir, "vector_metadata.txt")
    with open(metadata_path, 'w') as f:
        f.write(f"D={VEC_D}\n")
        f.write(f"c={VEC_C}\n")
        f.write(f"m={VEC_M}\n")
    print(f"Saved metadata to {metadata_path}")
    
    # Encode each entity
    print("Encoding entities...")
    sig_facts = {j: [] for j in range(VEC_M)}  # band_value -> entity_sym_id
    vec_q_facts = []  # (entity_sym_id, chunk_idx, packed_u32)
    
    for name, sym_id in entities:
        v = embeddings[name]
        
        # ITQ encode
        sig = itq_encode(v, B)
        
        # Verify band_slice reconstruction (C1 gate)
        bands = [band_slice(sig, j) for j in range(VEC_M)]
        sig_recon = list(sig)
        for j in range(VEC_M):
            band_set(sig_recon, j, bands[j])
        assert sig == sig_recon, f"Band reconstruction failed for {name}"
        
        # Emit sig_j facts
        for j in range(VEC_M):
            band_val = band_slice(sig, j)
            sig_facts[j].append((band_val, sym_id))
        
        # Quantize to int8
        vi8 = quantize_int8(v)
        
        # Emit vec_q facts
        for chunk_idx in range(VEC_IVEC_WORDS):
            chunk = vi8[chunk_idx * 4 : (chunk_idx + 1) * 4]
            packed = pack4_le(chunk)
            vec_q_facts.append((sym_id, chunk_idx, packed))
    
    print(f"Encoded {len(entities)} entities")
    
    # Write sig_j CSVs
    print("Writing sig_j CSVs...")
    for j in range(VEC_M):
        rel_name = f"{SIG_REL_PREFIX}{j}__"
        csv_path = os.path.join(db_dir, f"sig_{j}.csv")
        write_csv(csv_path, sig_facts[j])
        print(f"  {rel_name}: {len(sig_facts[j])} facts")
    
    # Write vec_q CSV
    print("Writing vec_q CSV...")
    vec_q_csv = os.path.join(db_dir, "vec_q.csv")
    write_csv(vec_q_csv, vec_q_facts)
    print(f"  {VEC_Q_REL}: {len(vec_q_facts)} facts")
    
    # Write itq_basis CSV
    print("Writing itq_basis CSV...")
    basis_facts = []
    for i in range(VEC_D):
        for j in range(VEC_C):
            bits = float32_to_bits(B[i, j])
            basis_facts.append((i, j, bits))
    
    basis_csv = os.path.join(db_dir, "itq_basis.csv")
    write_csv(basis_csv, basis_facts)
    print(f"  {ITQ_BASIS_REL}: {len(basis_facts)} facts")
    
    # Bulk-load all CSVs
    print("Bulk-loading CSVs...")
    for j in range(VEC_M):
        rel_name = f"{SIG_REL_PREFIX}{j}__"
        csv_path = os.path.join(db_dir, f"sig_{j}.csv")
        result = dl_load(db_dir, csv_path, rel_name)
        print(f"  Loaded {rel_name}: {result}")
    
    result = dl_load(db_dir, vec_q_csv, VEC_Q_REL)
    print(f"  Loaded {VEC_Q_REL}: {result}")
    
    result = dl_load(db_dir, basis_csv, ITQ_BASIS_REL)
    print(f"  Loaded {ITQ_BASIS_REL}: {result}")
    
    # Publish snapshot
    print("Publishing snapshot...")
    result = dl_publish(db_dir)
    print(f"  {result}")
    
    # Clean up temporary CSV files
    print("Cleaning up temporary CSV files...")
    for j in range(VEC_M):
        os.remove(os.path.join(db_dir, f"sig_{j}.csv"))
    os.remove(vec_q_csv)
    os.remove(basis_csv)
    
    print("Done!")


if __name__ == "__main__":
    main()
