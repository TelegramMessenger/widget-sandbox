#!/bin/sh
# Builds the QuickJS wasm module the web worker and the Node harness run widget
# code in, from the TelegramMessenger/quickjs fork (quickjs-ng with fixes,
# including quickjs-ng#1400), with quickjs-emscripten 0.32.0's C interface and
# its quickjs-ng-wasmfile-release-sync flags, so @jitl's FFI and
# quickjs-emscripten-core work unchanged.
#
#   packages/quickjs-wasm/build.sh [fork checkout]   (default: the packages/ios CQuickJS submodule)
#
# Needs emcc (brew install emscripten) and a host C compiler (for the fork's
# qjs, which generates the amalgamated source). The output in dist/ is
# committed, so the web build and CI need neither.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
FORK=$(cd "${1:-$HERE/../ios/Sources/CQuickJS/quickjs}" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cmake -S "$FORK" -B "$WORK/host" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/host" --target qjs >/dev/null
# interface.c includes ../vendor/quickjs-ng/quickjs.h, as in quickjs-emscripten's tree.
NG="$WORK/vendor/quickjs-ng"
mkdir -p "$NG" "$WORK/c"
(cd "$FORK" && "$WORK/host/qjs" amalgam.js "$NG/quickjs-amalgam.c")
cp "$FORK/quickjs.h" "$FORK/quickjs-libc.h" "$NG/"
(cd "$NG" && patch -s -p1 --fuzz=3 < "$HERE/emscripten/0001-bellard-module-detection.patch")
cp "$HERE/emscripten/interface.c" "$WORK/c/"
VERSION=$(git -C "$FORK" describe --tags --always 2>/dev/null || echo fork)

CFLAGS="-Oz -flto -DQTS_USE_QUICKJS_NG"
LINK="-s MODULARIZE=1 -s IMPORTED_MEMORY=1 -s EXPORT_NAME=QuickJSRaw -s INVOKE_RUN=0 -s ALLOW_MEMORY_GROWTH=1 -s ALLOW_TABLE_GROWTH=1
  -s STACK_SIZE=5MB -s SUPPORT_ERRNO=0 -s IGNORE_MISSING_MAIN=0 --no-entry -s AUTO_JS_LIBRARIES=0 -lccall.js -s AUTO_NATIVE_LIBRARIES=0
  -s AUTO_ARCHIVE_INDEXES=0 -s DEFAULT_TO_CXX=0 -s ALLOW_UNIMPLEMENTED_SYSCALLS=0 -s MIN_NODE_VERSION=180300 -s NODEJS_CATCH_EXIT=0
  -s EXPORTED_RUNTIME_METHODS=@$HERE/emscripten/exportedRuntimeMethods.json -s EXPORTED_FUNCTIONS=@$HERE/emscripten/symbols.json
  --closure 1 -s FILESYSTEM=0 --pre-js $HERE/emscripten/pre-extension.js --pre-js $HERE/emscripten/pre-wasmMemory.js"

emcc $CFLAGS -D_GNU_SOURCE -DQJS_BUILD_LIBC -DCONFIG_VERSION="\"$VERSION\"" -c -o "$WORK/quickjs-amalgam.o" "$NG/quickjs-amalgam.c"
emcc $CFLAGS -Wno-cast-function-type -c -o "$WORK/interface.o" "$WORK/c/interface.c"
OBJS="$WORK/interface.o $WORK/quickjs-amalgam.o"
mkdir -p "$HERE/dist" "$WORK/browser"
emcc $CFLAGS $LINK -s EXPORT_ES6=1 -s ENVIRONMENT=node -o "$HERE/dist/emscripten-module.mjs" $OBJS
emcc $CFLAGS $LINK -s ENVIRONMENT=node -o "$HERE/dist/emscripten-module.cjs" $OBJS
emcc $CFLAGS $LINK -s EXPORT_ES6=1 -s ENVIRONMENT=web,worker -o "$WORK/browser/emscripten-module.js" $OBJS
cp "$WORK/browser/emscripten-module.js" "$HERE/dist/emscripten-module.browser.mjs"
cp "$WORK/browser/emscripten-module.wasm" "$HERE/dist/emscripten-module.wasm"
echo "$VERSION" > "$HERE/dist/QUICKJS_VERSION"
echo "built from $FORK ($VERSION)"
