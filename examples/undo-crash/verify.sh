#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=${1:-"$example_dir/../../build/cpp-repl"}
binary_dir=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd)
binary="$binary_dir/$(basename -- "$binary")"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cd "$scratch"
ulimit -c 0

for mode in add anonymous using; do
  case "$mode" in
    add) input="%add \"$example_dir/anonymous.cpp\"" ;;
    anonymous) input='namespace { int undo_local = 1; } extern int undo_missing; int undo_result = undo_missing;' ;;
    using) input='namespace UndoNames { int undo_local = 1; } using namespace UndoNames; extern int undo_missing; int undo_result = undo_missing;' ;;
  esac
  # Both names must be reusable: merely skipping rollback does not pass.
  if printf '%s\n' "$input" 'int undo_result = 7; int undo_local = 2;' \
      'undo_result + undo_local' '%quit' | "$binary" > "$mode.log" 2>&1; then
    cat "$mode.log"
  else
    status=$?
    cat "$mode.log"
    echo "FAIL $mode: process exited $status" >&2
    exit "$status"
  fi
  grep -q 'error: undefined symbol: undo_missing' "$mode.log"
  grep -q '(int) 9' "$mode.log"
  if grep -q 'redefinition\|ambiguous' "$mode.log"; then
    echo "FAIL $mode: declarations survived rollback" >&2
    exit 1
  fi
  echo "PASS $mode: missing symbol reported, rollback complete, session usable"
done
