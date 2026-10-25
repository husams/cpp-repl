#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=${1:-"$example_dir/../../build/cpp-repl"}
binary_dir=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd)
binary="$binary_dir/$(basename -- "$binary")"
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
mkdir "$scratch/home"
export HOME="$scratch/home"
cd "$scratch"
ulimit -c 0

failed=0
for mode in control link parse add; do
  case "$mode" in
    control) input=';' ;;
    link) input='namespace std { int include_recovery_missing(); } int include_recovery_result = std::include_recovery_missing();' ;;
    parse) input='namespace std { int include_recovery_broken = ; }' ;;
    add) input="%add \"$example_dir/failed.cpp\"" ;;
  esac
  # The add case includes <string> for the first time inside the rejected input.
  header='#include <string>'
  if [ "$mode" = add ]; then header=';'; fi
  if printf '%s\n' "$header" "$input" '#include <string>' \
      'std::string recovered = "ss";' 'int(recovered.size())' '%quit' \
      | "$binary" > "$mode.log" 2>&1; then
    cat "$mode.log"
  else
    status=$?
    cat "$mode.log"
    echo "FAIL $mode: process exited $status" >&2
    exit "$status"
  fi
  expected='(int) 2'
  case "$mode" in
    link|add) expected='error: undefined symbol: std::include_recovery_missing()' ;;
    parse) expected='error: Parsing failed.' ;;
  esac
  if grep -Fq "$expected" "$mode.log" && grep -Fq '(int) 2' "$mode.log" &&
      ! grep -q "undeclared identifier 'std'" "$mode.log"; then
    echo "PASS $mode: std::string remains usable"
  else
    echo "FAIL $mode: expected rejection or std::string recovery missing" >&2
    failed=1
  fi
done
exit "$failed"
