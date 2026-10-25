#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
case "$(uname -s)" in
  Darwin) platform=macos ;;
  Linux) platform=linux-dev ;;
  *) platform=other ;;
esac
mkdir -p evidence
transcript="evidence/$platform.txt"
test_home=$(mktemp -d "${TMPDIR:-/tmp}/static-members-home.XXXXXX")
trap 'rm -rf "$test_home"' EXIT HUP INT TERM
# Isolate user startup files; project startup scripts remain active.
{
  printf 'Platform: '
  uname -sm
  date -u '+Verified: %Y-%m-%dT%H:%M:%SZ'
  printf 'Executable: %s\n' "${CPP_REPL:-$(command -v cpp-repl)}"
  env HOME="$test_home" NO_COLOR=1 TERM=dumb ./run.sh < verify.repl
} > "$transcript" 2>&1
if ! grep -q '^STATIC_MEMBERS_PASS' "$transcript" ||
   grep -Eq 'error:|STATIC_MEMBERS_FAIL' "$transcript"; then
  cat "$transcript"
  exit 1
fi
missing_names=$(awk '/^storage_demo::/ { sub(/\[abi:[^]]*\]/, ""); print }' "$transcript")
expected_names='storage_demo::State::counter_
storage_demo::State::samples
storage_demo::State::title
storage_demo::State::weights'
if [ "$missing_names" != "$expected_names" ]; then
  cat "$transcript"
  exit 1
fi
# The delimited query must contain no unresolved names after the definitions.
if ! awk '
  /^UNDEFINED_BEGIN$/ { inside = 1; begin++; next }
  /^UNDEFINED_END$/ { inside = 0; end++; next }
  inside && /storage_demo::/ { missing = 1 }
  END { exit !(begin == 1 && end == 1 && !missing) }
' "$transcript"; then
  cat "$transcript"
  exit 1
fi
printf 'PASS: static members, native storage, compile flags, and no unresolved members.\n'
printf 'Transcript: %s/%s\n' "$PWD" "$transcript"
