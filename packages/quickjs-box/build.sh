#!/bin/sh
# Builds the QuickJS box: the TelegramMessenger/quickjs fork (the commit the native packages
# compile), the Swift package's qjs_shim.c and guest/qjsbox_guest.c compiled to one wasm module,
# and that module translated back to C by WABT's wasm2c. The C is what the clients compile
# (src/ includes it); the engine then runs inside a linear memory of its own.
#
#   packages/quickjs-box/build.sh [--docker] [fork checkout]   rebuild wasm/, generated/, wabt/ and manifest.sha256
#   packages/quickjs-box/build.sh --check [--docker]           both stages in a temporary directory, compared with
#                                                              what is committed (exit 1 on a difference)
#   packages/quickjs-box/build.sh --check-generated            stage 2 only: needs WABT, not emscripten
#   packages/quickjs-box/build.sh --out DIR [fork checkout]    write the same tree to DIR instead (an experiment
#                                                              with another engine commit; nothing here changes)
#
# Stage 1, the wasm: the pinned emscripten (EMSDK_VERSION) compiles the sources from a fixed
# layout (src/quickjs, src/shim, src/include, src/guest under a temporary directory, relative
# paths, -ffile-prefix-map), so the same fork commit gives the same bytes on any machine.
# --docker runs this stage in the emsdk image of that version, pinned by digest (needs only
# Docker); without it the emcc on PATH must report that version.
#
# Stage 2, the C: the pinned wasm2c (WABT_COMMIT, the commit of the 1.0.42 tag) translates
# wasm/qjsbox.wasm into generated/ (8 files, --num-outputs=8), and WABT's runtime is copied from
# the same checkout into wabt/. The script fetches that commit into .cache/ and builds wasm2c from
# it (CMake and a host C++ compiler; a few minutes, once). --system-wabt uses the wasm2c on PATH
# instead: its version is checked, its commit cannot be, so use it for a quick look, not a release.
#
# Everything this script writes is committed and listed with its sha256 in manifest.sha256.
# Never edit those files by hand. A new emscripten, a new WABT or a new fork commit is a
# deliberate change: bump the pin, rebuild, commit the outputs and the manifest together.
set -eu
EMSDK_VERSION=6.0.12
EMSDK_IMAGE=emscripten/emsdk:6.0.12@sha256:77638e6c215a65f25a704c0c831d5c2463a8c0468a58c6cb82489dbe621dfa05
WABT_VERSION=1.0.42
WABT_COMMIT=ff0ef7e0009402740c805a9744c09b05be063e48
WABT_REPO=https://github.com/WebAssembly/wabt.git

# The module's own stack and memory. The host reads them from generated/qjsbox_build.h.
GUEST_STACK_BYTES=1048576        # the stack QuickJS measures its recursion on
INITIAL_MEMORY_BYTES=2097152     # static data + that stack + the first heap
MAXIMUM_MEMORY_BYTES=270532608   # 258 MB: the ceiling of any per-instance limit the host sets
NUM_OUTPUTS=8

# What the module may import and must export; a difference stops the build (the surface the host trusts).
# The three emscripten helpers among the exports (stack and setThrew) come with its runtime; the host never calls them.
IMPORTS="env.emscripten_notify_memory_growth host.call host.interrupt host.ops host.take host.tz_offset host.words
wasi_snapshot_preview1.clock_time_get wasi_snapshot_preview1.fd_close wasi_snapshot_preview1.fd_seek wasi_snapshot_preview1.fd_write"
EXPORTS="__indirect_function_table _emscripten_stack_restore _initialize emscripten_stack_get_current setThrew box_alloc box_call box_capture box_compile box_eval box_eval_bytecode box_free box_gc box_init
box_job_pending box_memory_usage box_memory_used box_register box_release box_result_kind box_result_len
box_result_number box_result_ptr box_run_job box_set_memory_limit memory"

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
MODE=build DOCKER=0 SYSTEM_WABT=0 FORK_ARG= OUT_ARG= STAGE1_OUT=
while [ $# -gt 0 ]; do
  case $1 in
    --check) MODE=check ;;
    --check-generated) MODE=check-generated ;;
    --docker) DOCKER=1 ;;
    --system-wabt) SYSTEM_WABT=1 ;;
    --out) OUT_ARG=$2; shift ;;
    --stage1) MODE=stage1; STAGE1_OUT=$2; shift ;;   # internal: what --docker runs in the container
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    -*) echo "build.sh: unknown option $1" >&2; exit 2 ;;
    *) FORK_ARG=$1 ;;
  esac
  shift
done
FORK=$(cd "${FORK_ARG:-$ROOT/packages/ios/Sources/CQuickJS/quickjs}" && pwd)
SHIM="$ROOT/packages/ios/Sources/CQuickJS"

sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
# The sources of stage 1 that live in this repository (the fork is named by its commit), from the repository root.
SOURCES="packages/quickjs-box/guest/qjsbox_guest.c packages/ios/Sources/CQuickJS/shim/qjs_shim.c packages/ios/Sources/CQuickJS/include/qjs_shim.h"
# `<sha256>  <path>` of each: wasm/SOURCES.sha256 says what the wasm was built from, so a unit test can tell a
# changed source from a rebuilt box without emscripten.
sources() { (cd "$ROOT" && for f in $SOURCES; do sha256 "$f"; done); }
# `<sha256>  <path>` for every file under wasm/, generated/ and wabt/ of a tree, sorted by path.
manifest() { (cd "$1" && find wasm generated wabt -type f | LC_ALL=C sort | while read -r f; do sha256 "$f"; done); }

# The fork's version: the first 7 hex digits of its commit. Inside the container git cannot
# follow the submodule's gitdir, so the host passes it in.
if [ -z "${QJS_VERSION:-}" ]; then
  QJS_VERSION=$(git -C "$FORK" rev-parse HEAD 2>/dev/null | cut -c1-7)
  [ -n "$QJS_VERSION" ] || QJS_VERSION=fork
  if [ -n "$(git -C "$FORK" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    echo "build.sh: warning: $FORK has local changes; the output will not match a clean checkout of $QJS_VERSION" >&2
  fi
fi

WORK=$(mktemp -d)
CACHE_TMP=
trap 'rm -rf "$WORK" ${CACHE_TMP:+"$CACHE_TMP"}' EXIT

# ---- stage 1: sources -> qjsbox.wasm --------------------------------------------------------
stage1() { # $1: the wasm to write
  EMCC_VERSION=$(emcc --version 2>/dev/null | sed -n '1s/.*) \([0-9][0-9.]*\).*/\1/p')
  if [ "$EMCC_VERSION" != "$EMSDK_VERSION" ]; then
    echo "build.sh: emcc ${EMCC_VERSION:-not found}; the pinned toolchain is emscripten $EMSDK_VERSION (use --docker, or emsdk install $EMSDK_VERSION)" >&2
    exit 1
  fi
  S="$WORK/stage1"
  mkdir -p "$S/src/quickjs" "$S/src/shim" "$S/src/include" "$S/src/guest"
  cp "$FORK/quickjs.c" "$FORK/dtoa.c" "$FORK/libregexp.c" "$FORK/libunicode.c" "$FORK"/*.h "$S/src/quickjs/"
  cp "$SHIM/shim/qjs_shim.c" "$S/src/shim/"
  cp "$SHIM/include/qjs_shim.h" "$S/src/include/"
  cp "$HERE/guest/qjsbox_guest.c" "$S/src/guest/"
  # The CQuickJS target's defines (Package.swift), and the one libc call the box answers itself.
  # --profiling-funcs keeps the function names: a crash inside the box then names the engine function.
  # --stack-first puts the module's stack below its data: an overflow leaves the memory and traps
  # instead of running into the engine's tables.
  (cd "$S" && emcc -O2 -D_GNU_SOURCE -DQUICKJS_NG_BUILD -DQJS_DISABLE_DUMPS -Dlocaltime_r=qjsbox_localtime_r \
    -Isrc/include -Isrc/quickjs -ffile-prefix-map="$S/=" \
    src/quickjs/quickjs.c src/quickjs/dtoa.c src/quickjs/libregexp.c src/quickjs/libunicode.c src/shim/qjs_shim.c src/guest/qjsbox_guest.c \
    -sSTANDALONE_WASM --no-entry -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=$INITIAL_MEMORY_BYTES -sMAXIMUM_MEMORY=$MAXIMUM_MEMORY_BYTES \
    -sSTACK_SIZE=$GUEST_STACK_BYTES -Wl,--stack-first -sERROR_ON_UNDEFINED_SYMBOLS=1 --profiling-funcs -o qjsbox.wasm)
  mkdir -p "$(dirname "$1")"
  cp "$S/qjsbox.wasm" "$1"
}

if [ "$MODE" = stage1 ]; then stage1 "$STAGE1_OUT"; exit 0; fi

wasm() { # $1: the wasm to write, with the local emcc or in the pinned image
  if [ "$DOCKER" = 1 ]; then
    case $FORK in "$ROOT"/*) ;; *) echo "build.sh: --docker builds a fork inside this repository only ($FORK is outside $ROOT)" >&2; exit 2 ;; esac
    CACHE_TMP="$ROOT/.cache/qjsbox-stage1.$$"
    mkdir -p "$CACHE_TMP"
    # EM_CACHE: the image's cache is not writable by this user, and the standalone libraries are built on first use.
    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e EM_CACHE=/tmp/emcache -e QJS_VERSION="$QJS_VERSION" \
      -v "$ROOT:/repo" -w /repo "$EMSDK_IMAGE" sh packages/quickjs-box/build.sh --stage1 "/repo/${CACHE_TMP#"$ROOT"/}/qjsbox.wasm" "/repo/${FORK#"$ROOT"/}"
    mkdir -p "$(dirname "$1")"
    cp "$CACHE_TMP/qjsbox.wasm" "$1"
  else
    stage1 "$1"
  fi
}

# ---- stage 2: qjsbox.wasm -> generated/ and wabt/ -------------------------------------------
# Sets WASM2C, WASM_OBJDUMP and WABT_RT (a directory with WABT's wasm2c runtime and its LICENSE).
wabt() {
  WABT_RT="$WORK/wabt-rt"; mkdir -p "$WABT_RT"
  # The module uses no wasm exceptions, so the runtime's exception half is left out.
  RT_FILES="wasm-rt.h wasm-rt-impl.h wasm-rt-impl.c wasm-rt-impl-tableops.inc wasm-rt-mem-impl.c wasm-rt-mem-impl-helper.inc"
  if [ "$SYSTEM_WABT" = 1 ]; then
    WASM2C=$(command -v wasm2c) || { echo "build.sh: --system-wabt: no wasm2c on PATH" >&2; exit 1; }
    WASM_OBJDUMP=$(command -v wasm-objdump)
    BIN=$(cd "$(dirname "$WASM2C")" && pwd -P); PREFIX=$(cd "$BIN/.." && pwd -P)
    for f in $RT_FILES; do
      if [ -f "$PREFIX/share/wabt/wasm2c/$f" ]; then cp "$PREFIX/share/wabt/wasm2c/$f" "$WABT_RT/"; else cp "$PREFIX/include/$f" "$WABT_RT/"; fi
    done
    echo "build.sh: warning: --system-wabt: $WASM2C reports $("$WASM2C" --version), its commit is not checked against $WABT_COMMIT" >&2
  else
    W="$ROOT/.cache/wabt-$WABT_COMMIT"
    if [ ! -x "$W/build/wasm2c" ] || [ ! -x "$W/build/wasm-objdump" ]; then
      echo "build.sh: building wasm2c from WABT $WABT_COMMIT in $W (once)…" >&2
      rm -rf "$W"; mkdir -p "$W"
      git -C "$W" init -q
      git -C "$W" fetch -q --depth 1 "$WABT_REPO" "$WABT_COMMIT"
      git -C "$W" checkout -q FETCH_HEAD
      git -C "$W" submodule -q update --init --depth 1 third_party/picosha2
      GEN=; command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
      { cmake -S "$W" -B "$W/build" $GEN -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_LIBWASM=OFF -DUSE_INTERNAL_SHA256=ON -DWITH_WASI=OFF &&
        cmake --build "$W/build" --target wasm2c wasm-objdump; } >"$W/build.log" 2>&1 || { cat "$W/build.log" >&2; exit 1; }
    fi
    [ "$(git -C "$W" rev-parse HEAD)" = "$WABT_COMMIT" ] || { echo "build.sh: $W is not at $WABT_COMMIT" >&2; exit 1; }
    [ -z "$(git -C "$W" status --porcelain --untracked-files=no)" ] || { echo "build.sh: $W has local changes" >&2; exit 1; }
    WASM2C="$W/build/wasm2c"; WASM_OBJDUMP="$W/build/wasm-objdump"
    for f in $RT_FILES; do cp "$W/wasm2c/$f" "$WABT_RT/"; done
    cp "$W/LICENSE" "$WABT_RT/LICENSE"
  fi
  [ -f "$WABT_RT/LICENSE" ] || cp "$HERE/wabt/LICENSE" "$WABT_RT/LICENSE"
  V=$("$WASM2C" --version)
  [ "$V" = "$WABT_VERSION" ] || { echo "build.sh: wasm2c $V; the pinned WABT is $WABT_VERSION" >&2; exit 1; }
}

stage2() { # $1: the tree (with wasm/qjsbox.wasm) to write generated/ and wabt/ into
  T=$1
  # The surface first: the module imports nothing but the list above.
  got=$("$WASM_OBJDUMP" -x -j Import "$T/wasm/qjsbox.wasm" | sed -n 's/.* <- \(.*\)$/\1/p' | LC_ALL=C sort | tr '\n' ' ')
  want=$(echo $IMPORTS | tr ' ' '\n' | LC_ALL=C sort | tr '\n' ' ')
  [ "$got" = "$want" ] || { echo "build.sh: the module's imports changed:" >&2; echo "  got:  $got" >&2; echo "  want: $want" >&2; exit 1; }
  got=$("$WASM_OBJDUMP" -x -j Export "$T/wasm/qjsbox.wasm" | sed -n 's/.* -> "\(.*\)"$/\1/p' | LC_ALL=C sort | tr '\n' ' ')
  want=$(echo $EXPORTS | tr ' ' '\n' | LC_ALL=C sort | tr '\n' ' ')
  [ "$got" = "$want" ] || { echo "build.sh: the module's exports changed:" >&2; echo "  got:  $got" >&2; echo "  want: $want" >&2; exit 1; }
  rm -rf "$T/generated" "$T/wabt"; mkdir -p "$T/generated" "$T/wabt"
  # Run next to the input, so no path of this machine can reach the output.
  (cd "$T/wasm" && "$WASM2C" qjsbox.wasm --module-name=qjsbox --num-outputs=$NUM_OUTPUTS -o ../generated/qjsbox.c)
  cat > "$T/generated/qjsbox_build.h" <<EOF
/* Written by packages/quickjs-box/build.sh with the module. Do not edit. */
#define QJSBOX_QUICKJS_COMMIT "$(cat "$T/wasm/QUICKJS_VERSION")"
#define QJSBOX_EMSCRIPTEN_VERSION "$EMSDK_VERSION"
#define QJSBOX_WABT_VERSION "$WABT_VERSION"
#define QJSBOX_WABT_COMMIT "$WABT_COMMIT"
#define QJSBOX_WASM_SHA256 "$(sha256 "$T/wasm/qjsbox.wasm" | cut -d' ' -f1)"
#define QJSBOX_GUEST_STACK_BYTES $GUEST_STACK_BYTES
#define QJSBOX_INITIAL_MEMORY_BYTES $INITIAL_MEMORY_BYTES
#define QJSBOX_MAXIMUM_MEMORY_BYTES $MAXIMUM_MEMORY_BYTES
#define QJSBOX_NUM_OUTPUTS $NUM_OUTPUTS
EOF
  cp "$WABT_RT"/* "$T/wabt/"
}

# ---- modes ----------------------------------------------------------------------------------
if [ "$MODE" = build ]; then
  OUT=${OUT_ARG:-$HERE}
  mkdir -p "$OUT/wasm"
  wasm "$OUT/wasm/qjsbox.wasm"
  echo "$QJS_VERSION" > "$OUT/wasm/QUICKJS_VERSION"
  sources > "$OUT/wasm/SOURCES.sha256"
  wabt
  stage2 "$OUT"
  manifest "$OUT" > "$OUT/manifest.sha256"
  echo "built from $FORK ($QJS_VERSION) with emscripten $EMSDK_VERSION and wasm2c $WABT_VERSION ($([ "$SYSTEM_WABT" = 1 ] && echo "system, commit not checked" || echo "$WABT_COMMIT")); manifest.sha256 updated in $OUT"
  exit 0
fi

FAIL=0
# What is committed is exactly what the manifest lists.
if ! manifest "$HERE" | cmp -s - "$HERE/manifest.sha256"; then
  echo "build.sh --check: the tree differs from manifest.sha256:" >&2
  manifest "$HERE" | diff "$HERE/manifest.sha256" - >&2 || true
  FAIL=1
fi
T="$WORK/tree"; mkdir -p "$T/wasm"
cp "$HERE/wasm/QUICKJS_VERSION" "$T/wasm/"
if [ "$MODE" = check ]; then
  # Stage 1: the wasm rebuilt from the sources is the committed one, and it was built from this fork commit.
  wasm "$T/wasm/qjsbox.wasm"
  if ! cmp -s "$T/wasm/qjsbox.wasm" "$HERE/wasm/qjsbox.wasm"; then
    echo "build.sh --check: wasm/qjsbox.wasm rebuilt as $(sha256 "$T/wasm/qjsbox.wasm" | cut -d' ' -f1), committed is $(sha256 "$HERE/wasm/qjsbox.wasm" | cut -d' ' -f1)" >&2
    FAIL=1
  fi
  if [ "$(cat "$HERE/wasm/QUICKJS_VERSION")" != "$QJS_VERSION" ]; then
    echo "build.sh --check: wasm/QUICKJS_VERSION is $(cat "$HERE/wasm/QUICKJS_VERSION"), the fork is at $QJS_VERSION" >&2
    FAIL=1
  fi
  if ! sources | cmp -s - "$HERE/wasm/SOURCES.sha256"; then
    echo "build.sh --check: wasm/SOURCES.sha256 does not list the sources as they are now" >&2
    FAIL=1
  fi
fi
# Stage 2: the C translated from the COMMITTED wasm, and WABT's runtime, are the committed ones.
cp "$HERE/wasm/qjsbox.wasm" "$T/wasm/qjsbox.wasm"
wabt
stage2 "$T"
for d in generated wabt; do
  for f in $(cd "$T/$d" && ls) $(cd "$HERE/$d" && ls); do
    if ! cmp -s "$T/$d/$f" "$HERE/$d/$f" 2>/dev/null; then echo "build.sh --check: $d/$f differs from what the pinned tools write" >&2; FAIL=1; fi
  done
done
if [ "$FAIL" = 0 ]; then
  if [ "$MODE" = check ]; then
    echo "build.sh --check: wasm/ is reproducible from $FORK ($QJS_VERSION) with emscripten $EMSDK_VERSION$([ "$DOCKER" = 1 ] && echo " (the pinned image)"), generated/ and wabt/ from it with wasm2c $WABT_VERSION ($([ "$SYSTEM_WABT" = 1 ] && echo "system, commit not checked" || echo "$WABT_COMMIT"))"
  else
    echo "build.sh --check-generated: generated/ and wabt/ are what wasm2c $WABT_VERSION ($([ "$SYSTEM_WABT" = 1 ] && echo "system, commit not checked" || echo "$WABT_COMMIT")) writes from the committed wasm"
  fi
fi
exit "$FAIL"
