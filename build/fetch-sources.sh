#!/bin/sh
# Fetch every file in build/SOURCES into dl/ and check its sha256. A file
# already in dl/ with the right checksum is kept.
# Part of riscos-reeledit. GPL version 2 or later.
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
DL=${DL:-$TOP/dl}
mkdir -p "$DL"
grep -v '^#' "$TOP/build/SOURCES" | while read -r file sha url; do
  [ -n "$file" ] || continue
  if [ -f "$DL/$file" ] && echo "$sha  $DL/$file" | sha256sum -c --status; then
    continue
  fi
  echo "fetching $file"
  curl -sSfL -o "$DL/$file.part" "$url"
  if ! echo "$sha  $DL/$file.part" | sha256sum -c --status; then
    echo "fetch-sources: $file has the wrong checksum" >&2
    rm -f "$DL/$file.part"
    exit 1
  fi
  mv "$DL/$file.part" "$DL/$file"
done
