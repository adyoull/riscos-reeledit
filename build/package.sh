#!/bin/bash
# Make the zip of !MLT, the engine test: melt, MLT's data files, the
# Obey files and icons from app/!MLT. Out: out/MLT-$VERSION.zip
# Usage: VERSION=7.42.0-riscos1-test1 build/package.sh   (after build/build-riscos.sh)
# Part of riscos-reeledit. GPL version 2 or later.
set -euo pipefail
. "$(dirname "$0")/env.sh"
VERSION=${VERSION:-$MLT_VERSION-riscos1}
P=$OUT/package
A="$P/!MLT"
rm -rf "$P" && mkdir -p "$A"
cp "$TOP"/app/'!MLT'/* "$A/"
cp "$OUT/riscos/melt,ff8" "$A/melt,ff8"
cp "$OUT/riscos/bench,ff8" "$A/bench,ff8"
python3 "$RCD_DIR/package/png2sprite.py" "$A/!Sprites,ff9" '!mlt='"$TOP/app/icons/mlt.png" \
  'sm!mlt='"$TOP/app/icons/sm-mlt.png" > /dev/null
mkdir -p "$A/share"
cp -r "$OUT/riscos/stage/share/mlt-7" "$A/share/"
# MLT's metadata (.yml) is only read by melt -query: kept, it's small
python3 "$RCD_DIR/package/rozip.py" "$OUT/MLT-$VERSION.zip" -C "$P" '!MLT'
for b in melt bench; do
  if strings -a "$A/$b,ff8" | grep -iqE 'cl[a]ude|anthr[o]pic'; then
    echo "package: a name that mustn't be there is in $b" >&2; exit 1
  fi
done
(cd "$OUT" && sha256sum "MLT-$VERSION.zip")
