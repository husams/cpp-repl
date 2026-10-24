#!/usr/bin/env bash
# Build and install cpp-repl on RHEL (or any Linux) with a custom LLVM/Clang toolchain.
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] [-- extra cmake args]

Options:
  --prefix DIR       Install prefix                          (default: /usr/local)
  --toolchain DIR    LLVM/Clang root with bin/clang++ and
                     lib{,64}/cmake/{llvm,clang}             (default: /usr)
  --build-dir DIR    Build directory                         (default: <src>/build-rhel)
  --build-type TYPE  CMake build type                        (default: Release)
  --jobs N           Parallel build jobs                     (default: nproc)
  --no-tests         Skip building and running unit tests
  --no-install       Build only, do not install
  -h, --help         Show this help
EOF
}

src_dir=$(cd -- "$(dirname -- "$0")" && pwd)
prefix=/usr/local
toolchain=/usr
build_dir=$src_dir/build-rhel
build_type=Release
jobs=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
tests=ON
install=1
extra=()

die() { echo "error: $*" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
  case $1 in
    --prefix)       prefix=${2:?--prefix needs a value}; shift 2 ;;
    --prefix=*)     prefix=${1#*=}; shift ;;
    --toolchain)    toolchain=${2:?--toolchain needs a value}; shift 2 ;;
    --toolchain=*)  toolchain=${1#*=}; shift ;;
    --build-dir)    build_dir=${2:?--build-dir needs a value}; shift 2 ;;
    --build-dir=*)  build_dir=${1#*=}; shift ;;
    --build-type)   build_type=${2:?--build-type needs a value}; shift 2 ;;
    --build-type=*) build_type=${1#*=}; shift ;;
    --jobs|-j)      jobs=${2:?--jobs needs a value}; shift 2 ;;
    --jobs=*)       jobs=${1#*=}; shift ;;
    --no-tests)     tests=OFF; shift ;;
    --no-install)   install=0; shift ;;
    -h|--help)      usage; exit 0 ;;
    --)             shift; extra=("$@"); break ;;
    *)              usage >&2; die "unknown option: $1" ;;
  esac
done

toolchain=$(cd -- "$toolchain" 2>/dev/null && pwd) || die "toolchain dir not found: $toolchain"

cc=$toolchain/bin/clang
cxx=$toolchain/bin/clang++
[[ -x $cxx ]] || die "clang++ not found at $cxx"
[[ -x $cc ]] || die "clang not found at $cc"

llvm_dir=
clang_dir=
for lib in lib64 lib; do
  if [[ -z $llvm_dir && -f $toolchain/$lib/cmake/llvm/LLVMConfig.cmake ]]; then
    llvm_dir=$toolchain/$lib/cmake/llvm
  fi
  if [[ -z $clang_dir && -f $toolchain/$lib/cmake/clang/ClangConfig.cmake ]]; then
    clang_dir=$toolchain/$lib/cmake/clang
  fi
done
[[ -n $llvm_dir ]] || die "LLVMConfig.cmake not found under $toolchain/lib{64,}/cmake/llvm (install llvm-devel?)"
[[ -n $clang_dir ]] || die "ClangConfig.cmake not found under $toolchain/lib{64,}/cmake/clang (install clang-devel?)"

command -v cmake >/dev/null || die "cmake not found in PATH"
generator="Unix Makefiles"
command -v ninja >/dev/null && generator=Ninja

echo "==> toolchain: $toolchain"
echo "==> LLVM_DIR:  $llvm_dir"
echo "==> prefix:    $prefix"
echo "==> build dir: $build_dir ($generator, $build_type)"

cmake -S "$src_dir" -B "$build_dir" -G "$generator" \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_C_COMPILER="$cc" \
  -DCMAKE_CXX_COMPILER="$cxx" \
  -DLLVM_DIR="$llvm_dir" \
  -DClang_DIR="$clang_dir" \
  -DREPLXX_EXAMPLE_BUILD_TESTS="$tests" \
  ${extra[@]+"${extra[@]}"}

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
