#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PLATFORM="${ER301_THIRD_PARTY_PLATFORM:-}"
DEPLOYMENT="${ER301_MACOS_DEPLOYMENT_TARGET:-}"

case "$PLATFORM" in
  macos-arm64) : "${DEPLOYMENT:=11.0}" ;;
  macos-x86_64) : "${DEPLOYMENT:=10.9}" ;;
  *)
    echo "error: ER301_THIRD_PARTY_PLATFORM must be macos-arm64 or macos-x86_64" >&2
    exit 1
    ;;
esac

PREFIX="$ROOT/third-party/install/$PLATFORM"
libs=(
  "$PREFIX/lib/libSDL2.a"
  "$PREFIX/lib/libSDL2main.a"
  "$PREFIX/lib/libSDL2_ttf.a"
  "$PREFIX/lib/libfreetype.a"
  "$PREFIX/lib/libfftw3f.a"
  "$PREFIX/lib/libfftw3f_threads.a"
)

for lib in "${libs[@]}"; do
  if [[ ! -f "$lib" ]]; then
    echo "error: missing pinned macOS dependency: $lib" >&2
    echo "Run: make third-party-macos-rebuild" >&2
    exit 1
  fi

done

# Modern Apple clang writes LC_BUILD_VERSION/minos into every object. Inspect
# every archive member through otool and reject any object whose minimum macOS
# exceeds the Rack target floor. This catches the exact 14.0 -> 11.0 mismatch
# that previously produced hundreds of linker warnings.
for lib in "${libs[@]}"; do
  if ! /usr/bin/otool -l "$lib" | awk -v max="$DEPLOYMENT" '
    function vnum(v, p,n,a,i,r) {
      n=split(v,a,"."); r=0;
      for(i=1;i<=3;i++) r = r*1000 + (i<=n ? a[i]+0 : 0);
      return r;
    }
    $1=="minos" && vnum($2) > vnum(max) { bad=1; badv[$2]=1 }
    END {
      if (bad) {
        printf("objects require newer macOS than %s:", max) > "/dev/stderr";
        for (v in badv) printf(" %s", v) > "/dev/stderr";
        printf("\n") > "/dev/stderr";
        exit 1;
      }
    }
  '; then
    echo "error: deployment-target mismatch in $lib" >&2
    echo "Run: make third-party-macos-rebuild" >&2
    exit 1
  fi
done

echo "macOS static dependency audit: PASS (<= macOS $DEPLOYMENT)"
