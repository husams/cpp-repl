#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
case "$(uname -s)" in
  Darwin) default_repl=../../build/cpp-repl ;;
  *) default_repl=../../build-linux/cpp-repl ;;
esac
repl_path=${CPP_REPL:-$default_repl}
exec "$repl_path" -I. "$@"
