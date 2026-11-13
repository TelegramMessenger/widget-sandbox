#!/bin/sh
# Builds tools/qjsbox-run from the sources a client compiles (src/), in both memory modes, and with
# `test` runs its selftest in each (guard pages only where this machine has them: 64-bit macOS).
#   packages/quickjs-box/tools/build.sh [test] [--tree DIR]   DIR: a tree written by ../build.sh --out (another engine commit)
# The binaries go to .cache/qjsbox-tools/ (or DIR/tools/).
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TEST=0 TREE=$HERE
while [ $# -gt 0 ]; do
  case $1 in
    test) TEST=1 ;;
    --tree) TREE=$(cd "$2" && pwd); shift ;;
    *) echo "tools/build.sh: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done
if [ "$TREE" = "$HERE" ]; then OUT="$ROOT/.cache/qjsbox-tools"; else OUT="$TREE/tools"; fi
CC=${CC:-cc}
MODES="bounds-checks"
case "$(uname -s)-$(uname -m)" in Darwin-arm64|Darwin-x86_64) MODES="guard-pages bounds-checks" ;; esac
for mode in $MODES; do
  o="$OUT/$mode"; rm -rf "$o"; mkdir -p "$o"
  [ "$mode" = bounds-checks ] && DEF="-DQJSBOX_BOUNDS_CHECKS=1" || DEF="-DQJSBOX_BOUNDS_CHECKS=0"
  # -DNDEBUG: the release behaviour (a call from inside a callback is refused, not asserted).
  FLAGS="-O2 -DNDEBUG $DEF -I$HERE/include -I$HERE/src -I$TREE/generated -I$TREE/wabt"
  # The wrappers include ../generated and ../wabt relative to src/: for another tree, compile copies of them next to it.
  SRC="$HERE/src"
  if [ "$TREE" != "$HERE" ]; then rm -rf "$TREE/src"; cp -R "$HERE/src" "$TREE/src"; SRC="$TREE/src"; fi
  pids=
  for f in "$SRC"/*.c; do $CC $FLAGS -c "$f" -o "$o/$(basename "$f" .c).o" & pids="$pids $!"; done
  $CC $FLAGS -Wall -Wextra -c "$HERE/tools/qjsbox-run.c" -o "$o/qjsbox-run.o" & pids="$pids $!"
  for p in $pids; do wait "$p"; done
  $CC "$o"/*.o -lm -lpthread -o "$OUT/qjsbox-run-$mode"
  echo "built $OUT/qjsbox-run-$mode ($("$OUT/qjsbox-run-$mode" info))"
done
[ "$TEST" = 1 ] || exit 0
J="$ROOT/packages/ios/Sources/WidgetSandbox/WidgetJS"
FAIL=0
for mode in $MODES; do
  echo "== selftest, $mode"
  # A zone with daylight saving east of Greenwich, so both the sign and the season are checked.
  TZ=Europe/Berlin QJSBOX_EXPECT_TZ="-60,-120,12" "$OUT/qjsbox-run-$mode" selftest "$J/sandbox-bundle.js" "$J/sandbox-bundle.qbc" || FAIL=1
done
exit $FAIL
