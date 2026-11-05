#!/usr/bin/env bash
set -euo pipefail

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$example_dir/../.." && pwd)
repl_binary=${CPP_REPL_BINARY:-$project_dir/build/cpp-repl}

if [[ ! -x $repl_binary ]]; then
  printf 'Build this worktree first: cmake --preset default && cmake --build build\n' >&2
  exit 1
fi

session_dir=$(mktemp -d "${TMPDIR:-/tmp}/cpp-repl-defines-manual.XXXXXX")
cp "$example_dir/api.hpp" "$example_dir/consumer.cpp" "$session_dir/"

cat <<EOF
Manual example: $session_dir
Files are unloaded; this uses the binary built in the feature worktree.

Try these commands:
  %generate-defines consumer.cpp --output definitions.repl
  %prepare-defines consumer.cpp
  %define definition_demo::adjust(int value) cpp\`return value + 7;\`
  %define definition_demo::bias cpp\`35\`
  %commit-defines
  answer()
  definition_demo::initialization_count

Expected values: 42 and 1. Generated files stay in the directory above.
EOF

cd -- "$session_dir"
exec "$repl_binary" "$@"
