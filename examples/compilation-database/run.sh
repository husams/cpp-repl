#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=${CPP_REPL:-"$example_dir/../../build/cpp-repl"}
# Resolve a supplied relative path before changing the working directory.
binary_dir=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd)
binary="$binary_dir/$(basename -- "$binary")"
cd "$example_dir"
exec "$binary" "$@"
