#!/bin/sh
# smoke.sh — End-to-end CLI smoke test for M0
# Runs dl load/lookup/prefix on a small CSV and checks output.

set -e

DBDIR="${DL_TMPDIR:-build-tmp/smoke}"
rm -rf "$DBDIR"
mkdir -p "$DBDIR"

# Create test CSV
cat > "$DBDIR/test.csv" << 'EOF'
1,2
1,3
2,3
2,4
3,5
EOF

DL=./dl

echo "=== dl load ==="
$DL -d "$DBDIR" load "$DBDIR/test.csv" --rel edge

echo ""
echo "=== dl lookup (should be found) ==="
$DL -d "$DBDIR" lookup edge 1 2

echo ""
echo "=== dl lookup (should NOT be found) ==="
$DL -d "$DBDIR" lookup edge 9 9

echo ""
echo "=== dl prefix k=0 (all 5) ==="
$DL -d "$DBDIR" prefix edge

echo ""
echo "=== dl prefix k=1 (col1=2, 2 results) ==="
$DL -d "$DBDIR" prefix edge 2

echo ""
echo "=== Smoke test PASSED ==="
rm -rf "$DBDIR"
