#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
case "$(uname -s)" in
  Darwin) platform=macos; extension=dylib; nm_tool=/opt/homebrew/opt/llvm/bin/llvm-nm ;;
  Linux) platform=linux-dev; extension=so; nm_tool=nm ;;
  *) exit 2 ;;
esac
consumer="build/libcallable_consumer.$extension"
set -- build/*."$extension"
test "$#" -eq 1 && test "$1" = "$consumer"
mkdir -p evidence
transcript="evidence/$platform.txt"
test_home=$(mktemp -d "${TMPDIR:-/tmp}/callable-library-home.XXXXXX")
trap 'rmdir "$test_home"' EXIT HUP INT TERM
before=$(cmake -E sha256sum "$consumer")
{
  uname -sm
  date -u '+Verified: %Y-%m-%dT%H:%M:%SZ'
  printf 'SUPPLIED_LIBRARY_COUNT_PASS: 1\n'
  printf 'UNDEFINED IN FINAL CONSUMER LIBRARY:\n'
  "$nm_tool" --format=posix --undefined-only --demangle "$consumer" |
    sed 's/[[:blank:]]*$//' | grep 'callable_demo::'
  printf '\nLOADING WITHOUT DEFINITIONS:\n'
  ./build/loader_probe "$PWD/$consumer"
  printf '\nLOADING WITH REPL DEFINITIONS:\n'
  env HOME="$test_home" NO_COLOR=1 TERM=dumb ./run.sh < verify.repl
} > "$transcript" 2>&1
for marker in EXPECTED_LOAD_FAILURE NATIVE_CALLABLES_PASS JIT_OVERLOADS_PASS PRIVATE_METHODS_PASS; do
  if ! grep -q "^$marker" "$transcript"; then cat "$transcript"; exit 1; fi
done
for symbol in 'adjust(int)' 'adjust(double)' 'transform(' 'decorate(' 'Worker::hidden(' 'Worker::scale('; do
  if ! grep -F "$symbol" "$transcript" | grep -q ' U'; then cat "$transcript"; exit 1; fi
done
if grep -Eq 'error:|_FAIL$|UNEXPECTED_LOAD_SUCCESS' "$transcript"; then cat "$transcript"; exit 1; fi
after=$(cmake -E sha256sum "$consumer")
test "$before" = "$after"
printf '\nCONSUMER_UNCHANGED_PASS: %s\n' "$after" >> "$transcript"
printf 'PASS: native calls in one unchanged supplied library resolve REPL callable definitions.\nTranscript: %s/%s\n' "$PWD" "$transcript"
