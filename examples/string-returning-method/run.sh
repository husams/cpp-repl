#!/usr/bin/env bash
set -euo pipefail

example_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repl="${CPP_REPL_BIN:-$example_dir/../../build/cpp-repl}"
if [[ ! -x "$repl" ]]; then
  printf '%s\n' 'Build cpp-repl in this worktree first, or set CPP_REPL_BIN to its executable.' >&2
  exit 1
fi

cd "$example_dir"
exec "$repl" -I "$example_dir/include" -fvisibility=hidden "$@"
