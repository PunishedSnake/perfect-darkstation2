#!/bin/sh
# Builds pd_spu2bank, the SPU2 sound bank converter (docs/ps2-spu2-audio.md).
#
#   tools/ps2/build_spu2bank.sh                 host build with cc and system zlib
#   tools/ps2/build_spu2bank.sh <zig-target>... cross builds with zig cc and a
#                                               zlib checkout in $ZLIB_DIR
#
# Example cross build (as the release workflow does):
#   pip install ziglang
#   git clone --depth 1 https://github.com/madler/zlib.git /tmp/zlib
#   ZLIB_DIR=/tmp/zlib tools/ps2/build_spu2bank.sh \
#       x86_64-linux-musl x86_64-windows-gnu x86_64-macos aarch64-macos
#
# Outputs go to build-tools/.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${OUT:-$ROOT/build-tools}
mkdir -p "$OUT"
SRC="$ROOT/tools/ps2/pd_spu2bank.c $ROOT/port/ps2/spu2_adpcm.c"
INC="-I$ROOT/port/ps2 -I$ROOT/port/include"

if [ $# -eq 0 ]; then
    ${CC:-cc} -O2 $INC $SRC -lz -lm -o "$OUT/pd_spu2bank"
    echo "built $OUT/pd_spu2bank"
    exit 0
fi

: "${ZLIB_DIR:?set ZLIB_DIR to a zlib source checkout}"
ZSRC=""
for f in adler32 crc32 inflate inftrees inffast zutil; do
    ZSRC="$ZSRC $ZLIB_DIR/$f.c"
done
ZIG=${ZIG:-python3 -m ziglang}
for target in "$@"; do
    ext=""
    case "$target" in *windows*) ext=".exe" ;; esac
    out="$OUT/pd_spu2bank-$target$ext"
    $ZIG cc -target "$target" -O2 -s $INC -I"$ZLIB_DIR" $SRC $ZSRC -lm -o "$out"
    rm -f "$OUT/pd_spu2bank-$target.pdb"
    echo "built $out"
done
