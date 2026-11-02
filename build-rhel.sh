#!/usr/bin/env bash
# Build, test and optionally install with a system or custom Clang/LLVM toolchain.
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- extra cmake args]

Options:
  --prefix DIR            Install prefix (default: /usr/local)
  --toolchain DIR         Toolchain root (default: system clang/clang++ in PATH)
  --cc FILE               C compiler override
  --cxx FILE              C++ compiler override
  --llvm-dir DIR          Directory containing LLVMConfig.cmake
  --clang-dir DIR         Directory containing ClangConfig.cmake
  --sysroot DIR           Build and default JIT sysroot
  --stdlib NAME           libc++ or libstdc++ for build and JIT
  --gcc-toolchain DIR     GCC root for build and JIT
  --gcc-install-dir DIR   Exact GCC installation for build and JIT
  --clang-config FILE     Clang driver config for build and JIT
  --resource-dir DIR      Builtin header directory for the linked Clang
  --include-dir DIR       Default include directory (repeatable)
  --system-include-dir DIR Default system include directory (repeatable)
  --library-dir DIR       Link/runtime library directory (repeatable)
  --default-arg ARG       Additional default JIT flag (repeatable)
  --build-dir DIR         Build directory (default: <src>/build-rhel)
  --build-type TYPE       CMake build type (default: Release)
  --jobs N, -j N          Parallel build/test jobs (default: nproc)
  --no-tests              Skip building and running tests
  --no-install            Build and test only
  -h, --help              Show this help

Extra CMake arguments are passed last. CMake toolchain files and standard
CMAKE_* overrides remain available through --. LLVM 21/22 is required;
GCC 8 libstdc++ headers/runtime are supported with a matching Clang build.
EOF
}

src_dir=$(cd -- "$(dirname -- "$0")" && pwd)
prefix=/usr/local
toolchain=
cc=
cxx=
llvm_dir=
clang_dir=
resource_dir=
clang_config=AUTO
build_dir=$src_dir/build-rhel
build_type=Release
jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
tests=ON
install=1
common_flags=()
cxx_flags=()
default_args=()
library_dirs=()
extra=()

die() { echo "error: $*" >&2; exit 1; }
# Quote one argument for CMake's compiler command-line parser.
quote_flag() {
  local value=$1
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  printf '"%s" ' "$value"
}

while [[ $# -gt 0 ]]; do
  option=${1%%=*}
  case $option in
    --prefix|--toolchain|--cc|--cxx|--llvm-dir|--clang-dir|--sysroot|--stdlib|\
    --gcc-toolchain|--gcc-install-dir|--clang-config|--resource-dir|--include-dir|\
    --system-include-dir|--library-dir|--default-arg|--build-dir|--build-type|--jobs|-j)
      if [[ $1 == *=* ]]; then
        value=${1#*=}; shift
      else
        [[ $# -ge 2 && -n $2 ]] || die "$option needs a value"
        value=$2; shift 2
      fi
      [[ -n $value ]] || die "$option needs a value"
      case $option in
        --prefix|--toolchain|--llvm-dir|--clang-dir|--sysroot|--gcc-toolchain|\
        --gcc-install-dir|--clang-config|--resource-dir|--include-dir|\
        --system-include-dir|--library-dir|--build-dir)
          [[ $value == /* ]] || value=$PWD/$value ;;
      esac
      case $option in
        --prefix) prefix=$value ;;
        --toolchain) toolchain=$value ;;
        --cc) cc=$value ;;
        --cxx) cxx=$value ;;
        --llvm-dir) llvm_dir=$value ;;
        --clang-dir) clang_dir=$value ;;
        --resource-dir) resource_dir=$value ;;
        --build-dir) build_dir=$value ;;
        --build-type) build_type=$value ;;
        --jobs|-j) jobs=$value ;;
        --sysroot|--gcc-toolchain|--gcc-install-dir)
          common_flags+=("$option=$value"); default_args+=("$option=$value") ;;
        --clang-config)
          clang_config=$value; common_flags+=("--config=$value") ;;
        --stdlib)
          [[ $value == libc++ || $value == libstdc++ ]] || die "--stdlib must be libc++ or libstdc++"
          cxx_flags+=("-stdlib=$value"); default_args+=("-stdlib=$value") ;;
        --include-dir)
          common_flags+=("-I$value"); default_args+=("-I$value") ;;
        --system-include-dir)
          common_flags+=(-isystem "$value"); default_args+=(-isystem "$value") ;;
        --library-dir) library_dirs+=("$value") ;;
        --default-arg) default_args+=("$value") ;;
      esac ;;
    --no-tests) tests=OFF; shift ;;
    --no-install) install=0; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; extra=("$@"); break ;;
    *) usage >&2; die "unknown option: $1" ;;
  esac
done

[[ $jobs =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"
if [[ -n $toolchain ]]; then
  toolchain=$(cd -- "$toolchain" 2>/dev/null && pwd) || die "toolchain dir not found: $toolchain"
  cc=${cc:-$toolchain/bin/clang}
  cxx=${cxx:-$toolchain/bin/clang++}
else
  cc=${cc:-clang}
  cxx=${cxx:-clang++}
fi
cc=$(command -v -- "$cc") || die "C compiler not found"
cxx=$(command -v -- "$cxx") || die "C++ compiler not found"
[[ -x $cc && -x $cxx ]] || die "compilers must be executable"
compiler_bin=$(cd -- "$(dirname -- "$cxx")" && pwd)

# Ask the selected installation before trying conventional layouts.
if [[ -z $llvm_dir && -x $compiler_bin/llvm-config ]]; then
  candidate=$("$compiler_bin/llvm-config" --cmakedir 2>/dev/null || true)
  [[ ! -f $candidate/LLVMConfig.cmake ]] || llvm_dir=$candidate
fi
if [[ -z $clang_dir && -n $llvm_dir ]]; then
  candidate=$(dirname -- "$llvm_dir")/clang
  [[ ! -f $candidate/ClangConfig.cmake ]] || clang_dir=$candidate
fi
search_root=${toolchain:-$(dirname -- "$compiler_bin")}
for lib in lib64 lib lib.llvm; do
  [[ -n $llvm_dir || ! -f $search_root/$lib/cmake/llvm/LLVMConfig.cmake ]] || llvm_dir=$search_root/$lib/cmake/llvm
  [[ -n $clang_dir || ! -f $search_root/$lib/cmake/clang/ClangConfig.cmake ]] || clang_dir=$search_root/$lib/cmake/clang
done
[[ -f $llvm_dir/LLVMConfig.cmake ]] || die "LLVMConfig.cmake not found; use --llvm-dir"
[[ -f $clang_dir/ClangConfig.cmake ]] || die "ClangConfig.cmake not found; use --clang-dir"

cmake_args=()
if [[ ${#common_flags[@]} -gt 0 ]]; then
  c_flags=; for flag in "${common_flags[@]}"; do c_flags+=$(quote_flag "$flag"); done
  cmake_args+=("-DCMAKE_C_FLAGS=$c_flags")
  cxx_flags=("${common_flags[@]}" ${cxx_flags[@]+"${cxx_flags[@]}"})
fi
if [[ ${#cxx_flags[@]} -gt 0 ]]; then
  cpp_flags=; for flag in "${cxx_flags[@]}"; do cpp_flags+=$(quote_flag "$flag"); done
  cmake_args+=("-DCMAKE_CXX_FLAGS=$cpp_flags")
fi
if [[ ${#library_dirs[@]} -gt 0 ]]; then
  link_flags=
  runtime_paths=
  for directory in "${library_dirs[@]}"; do
    directory=$(cd -- "$directory" 2>/dev/null && pwd) || die "library directory not found: $directory"
    link_flags+=$(quote_flag "-L$directory")
    runtime_paths+=${runtime_paths:+;}$directory
  done
  cmake_args+=("-DCMAKE_EXE_LINKER_FLAGS=$link_flags" "-DCMAKE_SHARED_LINKER_FLAGS=$link_flags"
    "-DCMAKE_BUILD_RPATH=$runtime_paths" "-DCMAKE_INSTALL_RPATH=$runtime_paths")
fi
if [[ ${#default_args[@]} -gt 0 ]]; then
  defaults=
  for flag in "${default_args[@]}"; do
    defaults+=${defaults:+;}${flag//;/\\;}
  done
  cmake_args+=("-DCPP_REPL_DEFAULT_COMPILE_OPTIONS=$defaults")
fi
[[ -z $resource_dir ]] || cmake_args+=("-DCPP_REPL_CLANG_RESOURCE_DIR=$resource_dir")
cmake_args+=("-DCPP_REPL_CLANG_CONFIG_FILE=$clang_config")

command -v cmake >/dev/null || die "cmake not found in PATH"
generator="Unix Makefiles"
command -v ninja >/dev/null && generator=Ninja

echo "==> C++ compiler: $cxx"
echo "==> LLVM_DIR:     $llvm_dir"
echo "==> build dir:    $build_dir ($generator, $build_type)"

cmake -S "$src_dir" -B "$build_dir" -G "$generator" \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_CXX_COMPILER="$cxx" \
  -DLLVM_DIR="$llvm_dir" \
  -DClang_DIR="$clang_dir" \
  -DREPLXX_EXAMPLE_BUILD_TESTS="$tests" \
  "${cmake_args[@]}" ${extra[@]+"${extra[@]}"}

cmake --build "$build_dir" --parallel "$jobs"

if [[ $tests == ON ]]; then
  ctest --test-dir "$build_dir" --output-on-failure --parallel "$jobs"
fi

if [[ $install == 1 ]]; then
  if [[ -w $prefix || ( ! -e $prefix && -w $(dirname -- "$prefix") ) ]]; then
    cmake --install "$build_dir" --component cpp-repl
  else
    sudo cmake --install "$build_dir" --component cpp-repl
  fi
  echo "==> installed $prefix/bin/cpp-repl"
fi
