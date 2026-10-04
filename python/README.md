# dlb — Python binding for datalog-dafsa (libdatalog.so)

In-process access path from Python to the DAFSA-backed Datalog engine in this
repository. Dependency-light (stdlib `ctypes` only). It lives beside the engine
because it is **1:1 with the C ABI** (`dafsa_abi_version()`, ABI 1): co-locating
them means an ABI change and its Python mirror land in ONE commit instead of two
repositories drifting. Consumers include the agent harness at `~/thing/agent/`
and the setpoint product.

**Read `ENVELOPE.md` first — the measured latency/concurrency envelope is
the load-bearing deliverable.**

Layout (all paths relative to this directory, which is the import root):
- `dlb/_binding.py` — raw ctypes layer: exact argtypes/restype for every
  function used (~50 symbols incl. the corpus-parameterized vector tier),
  error mapping (no silent zero-returns), callback trampolines.
- `dlb/api.py` — thin ergonomic API (`Db`, `Iter`, `Transaction`).
- `tests/test_binding.py` — 42 correctness tests (unittest; stdlib only).
- `tests/bench_envelope.py` — the measured envelope (open cost, per-call
  latency, recursive query cost, locking, thread safety, vector tier).
- `ENVELOPE.md` — the recorded measurements.

Run them:
```sh
python3 python/tests/test_binding.py       # correctness (needs libdatalog.so built)
python3 python/tests/bench_envelope.py     # the envelope; --quick for a short pass
```

Quick use (`<repo>/python` is the path to put on `sys.path`):

```python
import sys; sys.path.insert(0, "/path/to/datalog-dafsa/python")
import dlb
db = dlb.Db.open("/tmp/my-store")        # writer (sole, enforced in-process)
db.declare_relation("edge", 2)
db.add_fact("edge", (1, 2))
db.lookup("edge", (1, 2))                # ~3.5 us
rows = db.query_rules_ro("tc(X,Y) :- edge(X,Y).\ntc(X,Y) :- edge(X,Z), tc(Z,Y).", "tc")
with db.transaction() as tx:             # atomic CAS-guarded commit
    tx.add_fact("edge", (2, 3))
    tx.cas("rev-entity", 0, 1)
db.close()
ro = dlb.Db.open_ro("/tmp/my-store")     # reader — only when no writer is open
```

Canonical library: `zig-out/lib/libdatalog.so` — the README names
`zig build -Drelease` as the canonical build. ABI check: `dlb.abi_version()`
must equal `dlb.SUPPORTED_ABI` (currently 1, `dafsa_abi_version()`), verified at
every library load. Override the library path with `$DLBLIB`.

Threading model: ONE writer process (the CEN) + read-only handles in OTHER
processes — but a reader can only open while NO writer holds the lock (the
LOCK fcntl excludes in both directions; measured). Within one process the
binding's registry enforces one handle per directory (fcntl locks cannot
arbitrate within a process — measured; the C library allows a silent second
writer). API calls are serialized with an RLock; single-threaded use is the
contract.
