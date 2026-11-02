#!/usr/bin/env bash
set -euo pipefail
script=$1
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
mkdir -p "$scratch/bin" "$scratch/vendor toolchain/bin" "$scratch/vendor toolchain/lib.llvm/cmake/llvm" \
  "$scratch/vendor toolchain/lib.llvm/cmake/clang" "$scratch/library path"
touch "$scratch/vendor toolchain/lib.llvm/cmake/llvm/LLVMConfig.cmake" \
  "$scratch/vendor toolchain/lib.llvm/cmake/clang/ClangConfig.cmake"
export SCRIPT_TEST_LOG=$scratch/invocations
export SCRIPT_TEST_ROOT=$scratch
for name in clang clang++ cmake ctest; do
  cat > "$scratch/bin/$name" <<'STUB'
#!/usr/bin/env bash
printf '%s\n' "COMMAND:${0##*/}" "$@" >> "$SCRIPT_TEST_LOG"
STUB
  chmod +x "$scratch/bin/$name"
done
cp "$scratch/bin/clang" "$scratch/vendor toolchain/bin/clang"
cp "$scratch/bin/clang++" "$scratch/vendor toolchain/bin/clang++"
cat > "$scratch/vendor toolchain/bin/llvm-config" <<'STUB'
#!/usr/bin/env bash
printf '%s/vendor toolchain/lib.llvm/cmake/llvm\n' "$SCRIPT_TEST_ROOT"
STUB
chmod +x "$scratch/vendor toolchain/bin/llvm-config"
export PATH=$scratch/bin:$PATH

bash "$script" --toolchain "$scratch/vendor toolchain" --no-install --jobs 2 \
  --sysroot "$scratch/sysroot path" --stdlib libstdc++ \
  --gcc-install-dir "$scratch/gcc/8.5.0" --system-include-dir "$scratch/header path" \
  --include-dir "$scratch/user path" --library-dir "$scratch/library path" \
  --resource-dir "$scratch/resource path" --clang-config "$scratch/config path.cfg" \
  --default-arg=-DREPL_DEFAULT=42 -- -DCUSTOM_OVERRIDE=ON > "$scratch/output"
require() { grep -F -x -- "$1" "$SCRIPT_TEST_LOG" >/dev/null || { cat "$SCRIPT_TEST_LOG"; exit 1; }; }
require "-DLLVM_DIR=$scratch/vendor toolchain/lib.llvm/cmake/llvm"
require "-DClang_DIR=$scratch/vendor toolchain/lib.llvm/cmake/clang"
require "-DCMAKE_CXX_COMPILER=$scratch/vendor toolchain/bin/clang++"
require "-DCPP_REPL_CLANG_CONFIG_FILE=$scratch/config path.cfg"
require "-DCPP_REPL_CLANG_RESOURCE_DIR=$scratch/resource path"
require "-DCMAKE_BUILD_RPATH=$scratch/library path"
require "-DCMAKE_CXX_FLAGS=\"--sysroot=$scratch/sysroot path\" \"--gcc-install-dir=$scratch/gcc/8.5.0\" \"-isystem\" \"$scratch/header path\" \"-I$scratch/user path\" \"--config=$scratch/config path.cfg\" \"-stdlib=libstdc++\" "
require "-DCPP_REPL_DEFAULT_COMPILE_OPTIONS=--sysroot=$scratch/sysroot path;-stdlib=libstdc++;--gcc-install-dir=$scratch/gcc/8.5.0;-isystem;$scratch/header path;-I$scratch/user path;-DREPL_DEFAULT=42"
require 'COMMAND:ctest'
require --output-on-failure
require -DCUSTOM_OVERRIDE=ON
if grep -F -x -- --install "$SCRIPT_TEST_LOG"; then exit 1; fi

# Default selection uses PATH, without assuming /usr/bin.
cp "$scratch/vendor toolchain/bin/llvm-config" "$scratch/bin/llvm-config"
: > "$SCRIPT_TEST_LOG"
bash "$script" --no-tests --no-install > "$scratch/output"
require "-DCMAKE_CXX_COMPILER=$scratch/bin/clang++"
if grep -F -x 'COMMAND:ctest' "$SCRIPT_TEST_LOG"; then exit 1; fi

for option in '--jobs=0' '--stdlib=unknown' '--sysroot=' '--unknown'; do
  : > "$SCRIPT_TEST_LOG"
  if bash "$script" "$option" > "$scratch/output" 2>&1; then exit 1; fi
  [[ ! -s $SCRIPT_TEST_LOG ]]
done
printf '%s\n' 'Build script configuration checks passed'
