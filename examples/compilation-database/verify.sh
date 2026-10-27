#!/bin/sh
set -eu

example_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=${1:-${CPP_REPL:-"$example_dir/../../build/cpp-repl"}}
binary_dir=$(CDPATH= cd -- "$(dirname -- "$binary")" && pwd)
binary="$binary_dir/$(basename -- "$binary")"
scratch_dir=$(mktemp -d)
printf 'Verification logs: %s\n' "$scratch_dir"
# Run a copy so REPL history and logs never alter the example or project history.
cp -R "$example_dir" "$scratch_dir/project"
cd "$scratch_dir/project"

if ! NO_COLOR=1 TERM=dumb "$binary" < demo.repl > "$scratch_dir/demo.log" 2>&1; then
  cat "$scratch_dir/demo.log"
  exit 1
fi
cat "$scratch_dir/demo.log"
if grep -Eq 'error:|FAIL' "$scratch_dir/demo.log"; then
  echo 'FAIL: unexpected diagnostic in the three-case demo' >&2
  exit 1
fi
for expected in \
  'Compile options: ' \
  'PASS 1: each added file uses its own database entry' \
  'PASS 2: unlisted files borrow same-directory entries' \
  'PASS: beta defaults apply to added files' \
  'PASS 3: either database entry can supply session defaults'; do
  if ! grep -Fq "$expected" "$scratch_dir/demo.log"; then
    echo "FAIL: missing expected output: $expected" >&2
    exit 1
  fi
done

# Missing from the database is different from missing on disk.
test ! -e src/missing.cpp
if ! NO_COLOR=1 TERM=dumb "$binary" > "$scratch_dir/missing.log" 2>&1 <<'REPL'
%compile-opts src/missing.cpp
%add src/missing.cpp
extern "C" int puts(const char*);
puts("PASS: session survives a missing source file");
%quit
REPL
then
  cat "$scratch_dir/missing.log"
  exit 1
fi
cat "$scratch_dir/missing.log"
grep -Fq 'alpha.cpp' "$scratch_dir/missing.log"
grep -Fq 'error: no such file: src/missing.cpp' "$scratch_dir/missing.log"
grep -Fq 'PASS: session survives a missing source file' "$scratch_dir/missing.log"
if [ "$(grep -c 'error:' "$scratch_dir/missing.log")" -ne 1 ]; then
  echo 'FAIL: missing-file test produced an unexpected diagnostic' >&2
  exit 1
fi
echo 'PASS: all three compile database cases and missing-file rejection'
