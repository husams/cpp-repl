#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
exec "${CPP_REPL:-cpp-repl}" "$@"
