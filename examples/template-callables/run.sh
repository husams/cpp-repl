#!/usr/bin/env bash
set -euo pipefail

example_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repl="${CPP_REPL_BIN:-$example_dir/../../build/cpp-repl}"
scenario="${1:-class-template-instance}"
if [[ ! -x "$repl" ]]; then
  printf '%s\n' 'Build cpp-repl in this worktree first, or set CPP_REPL_BIN.' >&2
  exit 1
fi

case "$scenario" in
  all)
    for item in class-template-instance specialized-method function-template-instance; do
      printf '\n=== %s ===\n' "$item"
      printf '%s\n' '%quit' | "$example_dir/run.sh" "$item"
    done
    exit 0
    ;;
  class-template-instance|specialized-method|function-template-instance) ;;
  *) printf 'Unknown example: %s\n' "$scenario" >&2; exit 1 ;;
esac

case_dir="$example_dir/$scenario"
if [[ "${2:-}" == "--probe-define" ]]; then
  scratch="$(mktemp -d "${TMPDIR:-/tmp}/cpp-repl-template-probe.XXXXXX")"
  trap 'rm -rf "$scratch"' EXIT
  cd "$scratch"
  "$repl" -fvisibility=hidden -I "$case_dir/include" < "$case_dir/unsupported.repl"
else
  cd "$case_dir"
  exec "$repl" -fvisibility=hidden -I "$case_dir/include"
fi
