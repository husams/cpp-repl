#!/usr/bin/env bash
# Run CTest with deliberately hostile ambient settings; pass an absolute executable.
# Usage: bash tests/run_with_hostile_environment.sh /absolute/path/to/ctest --test-dir build
set -euo pipefail
poison=$(mktemp -d /tmp/cpp-repl-ambient.XXXXXX)
trap 'rm -rf "$poison"' EXIT
mkdir -p "$poison/include" "$poison/home/.cpp-repl" "$poison/bin" "$poison/python" "$poison/loader" "$poison/config" "$poison/keep"
printf '%s\n' '#error CPP_REPL_AMBIENT_INCLUDE_POISON' > "$poison/include/vector"
printf '%s\n' '#error CPP_REPL_AMBIENT_STARTUP_POISON' > "$poison/home/.cpp-repl/10-ambient.repl"
printf '%s\n' 'not a cache or temporary directory' > "$poison/cache-file"
printf '%s\n' 'must survive the test environment cleanup' > "$poison/keep/sentinel"
printf '%s\n' '#!/bin/sh' 'printf "%s\n" /cpp-repl-invalid-sdk' > "$poison/bin/xcrun"
chmod +x "$poison/bin/xcrun"
printf '%s\n' 'raise RuntimeError("CPP_REPL_AMBIENT_PYTHON_POISON")' > "$poison/python/tempfile.py"
set +e
env HOME="$poison/home" XDG_CACHE_HOME="$poison/cache-file" \
  TMPDIR="$poison/cache-file" TEMP="$poison/cache-file" TMP="$poison/cache-file" \
  CPATH="$poison/include" CPLUS_INCLUDE_PATH="$poison/include" \
  C_INCLUDE_PATH="$poison/include" OBJC_INCLUDE_PATH="$poison/include" \
  CLANG_CONFIG_FILE_USER_DIR="$poison/config" \
  CLANG_CONFIG_FILE_SYSTEM_DIR="$poison/config" \
  PATH="$poison/bin:/usr/bin:/bin" SDKROOT=/cpp-repl-invalid-sdk \
  DEVELOPER_DIR=/cpp-repl-invalid-developer TOOLCHAINS=cpp-repl-invalid-toolchain \
  LC_ALL=cpp_REPL_INVALID_LOCALE LANG=cpp_REPL_INVALID_LOCALE LANGUAGE=cpp_REPL_INVALID_LOCALE \
  NO_COLOR=1 FORCE_COLOR=1 CLICOLOR_FORCE=1 TERM=dumb \
  GTEST_FILTER=NoSuchTest GTEST_REPEAT=0 GTEST_SHUFFLE=1 \
  GTEST_OUTPUT="xml:$poison/unexpected-gtest.xml" \
  TESTBRIDGE_TEST_ONLY=NoSuchTest TEST_PREMATURE_EXIT_FILE="$poison/keep/sentinel" \
  XML_OUTPUT_FILE="$poison/unexpected-gtest-fallback.xml" \
  PYTHONPATH="$poison/python" PYTHONHOME="$poison/home" \
  LD_LIBRARY_PATH="$poison/loader" DYLD_LIBRARY_PATH="$poison/loader" \
  DYLD_FALLBACK_LIBRARY_PATH="$poison/loader" \
  CPP_REPL_TEST_ENVIRONMENT_CLEANUP=1 CPP_REPL_TEST_ENVIRONMENT_ROOT="$poison/keep" \
  "$@"
test_rc=$?
set -e
test -f "$poison/keep/sentinel"
test ! -e "$poison/unexpected-gtest.xml"
test ! -e "$poison/unexpected-gtest-fallback.xml"
printf 'Ambient sentinel preserved; inherited GTest output suppressed; exit=%s\n' "$test_rc"
exit "$test_rc"
