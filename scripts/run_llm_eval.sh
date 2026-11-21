#!/bin/sh
# Copper LLM eval (track G): run eval/prompts.jsonl through copper-core's agent loop and write
# eval_out/<YYYY-MM-DD>[-N]/results.jsonl and summary.txt (plus per-prompt logs).
#
#   scripts/run_llm_eval.sh        mock backend (deterministic, no model needed; the only backend)
#
# COPPER_CORE overrides the copper-core binary. All the work is in C (copper-core agent-eval); this only picks the
# output folder, which is never reused.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CORE=${COPPER_CORE:-}
if [ -z "$CORE" ]; then
  for c in build/copper-core build-local/copper-core build-v2/copper-core build/Release/copper-core; do
    if [ -x "$ROOT/$c" ]; then CORE="$ROOT/$c"; break; fi
  done
fi
[ -n "$CORE" ] || { echo "copper-core not found: build it or set COPPER_CORE" >&2; exit 64; }
BACKEND=${BACKEND:-mock}
DAY=$(date +%Y-%m-%d)
OUT="$ROOT/eval_out/$DAY"
N=2
while [ -e "$OUT" ]; do OUT="$ROOT/eval_out/$DAY-$N"; N=$((N + 1)); done
mkdir -p "$ROOT/eval_out"
set -- agent-eval "$ROOT/eval/prompts.jsonl" "$OUT" --backend "$BACKEND" --root "$ROOT"
rc=0
"$CORE" "$@" || rc=$?
cat "$OUT/summary.txt"
exit $rc
