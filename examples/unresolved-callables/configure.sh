#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
compiler=${CXX:-clang++}
if [ "$(uname -s)" = Darwin ] && [ -x /opt/homebrew/opt/llvm/bin/clang++ ]; then
  compiler=${CXX:-/opt/homebrew/opt/llvm/bin/clang++}
fi
cmake -S . -B build -DCMAKE_CXX_COMPILER="$compiler"
cmake --build build
