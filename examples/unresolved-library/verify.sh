#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
case "$(uname -s)" in
  Darwin) platform=macos; extension=dylib
          nm_tool=/opt/homebrew/opt/llvm/bin/llvm-nm ;;
  Linux) platform=linux-dev; extension=so; nm_tool=nm ;;
  *) exit 2 ;;
esac
consumer="build/libunresolved_consumer.$extension"
set -- build/*."$extension"
if [ "$#" -ne 1 ] || [ "$1" != "$consumer" ]; then
  printf 'Expected exactly one supplied shared library: %s\n' "$consumer" >&2
  exit 1
fi
mkdir -p evidence
transcript="evidence/$platform.txt"
test_home=$(mktemp -d "${TMPDIR:-/tmp}/unresolved-library-home.XXXXXX")
trap 'rmdir "$test_home"' EXIT HUP INT TERM
before=$(cmake -E sha256sum "$consumer")
{
  uname -sm
  date -u '+Verified: %Y-%m-%dT%H:%M:%SZ'
  printf 'SUPPLIED_LIBRARY_COUNT_PASS: 1\n'
  printf 'UNDEFINED IN FINAL CONSUMER LIBRARY:\n'
  "$nm_tool" --format=posix --undefined-only --demangle "$consumer" |
    sed 's/[[:blank:]]*$//' |
    grep 'unresolved_demo::State::'
  printf '\nLOADING WITHOUT DEFINITIONS:\n'
  ./build/loader_probe "$PWD/$consumer"
  printf '\nLOADING WITH REPL DEFINITIONS:\n'
  env HOME="$test_home" NO_COLOR=1 TERM=dumb ./run.sh < verify.repl
} > "$transcript" 2>&1
for marker in EXPECTED_LOAD_FAILURE NATIVE_LIBRARY_PASS SHARED_STORAGE_PASS NATIVE_READS_REPL_WRITE_PASS; do
  if ! grep -q "^$marker" "$transcript"; then cat "$transcript"; exit 1; fi
done
if ! grep -Eq '^EXPECTED_LOAD_FAILURE: .*_ZN15unresolved_demo5State' "$transcript"; then
  cat "$transcript"
  exit 1
fi
if grep -Eq 'error:|_FAIL$|UNEXPECTED_LOAD_SUCCESS' "$transcript"; then
  cat "$transcript"
  exit 1
fi
for member in total_ title samples weights; do
  if ! grep -Eq "^unresolved_demo::State::$member(\\[abi:[^]]*\\])? U( |$)" "$transcript"; then
    cat "$transcript"
    exit 1
  fi
done
after=$(cmake -E sha256sum "$consumer")
test "$before" = "$after"
printf '\nCONSUMER_UNCHANGED_PASS: %s\n' "$after" >> "$transcript"
printf 'PASS: one supplied library retains undefined members; native calls resolve REPL-defined storage.\n'
printf 'Transcript: %s/%s\n' "$PWD" "$transcript"
