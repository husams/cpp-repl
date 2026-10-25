#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
if [ -n "${CPP_REPL:-}" ]; then
  exec "$CPP_REPL" "$@"
elif [ -x ../../build/cpp-repl ]; then
  exec ../../build/cpp-repl "$@"
elif [ -x ../../build-linux/cpp-repl ]; then
  exec ../../build-linux/cpp-repl "$@"
fi
printf 'Build this checkout first, or set CPP_REPL to the new executable.\n' >&2
exit 1
