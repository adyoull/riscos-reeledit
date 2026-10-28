# Source this: . build/env.sh
# Settings shared by the build scripts. Each can be set in the environment
# first to override it.
#
#   RCD_DIR    riscos-crossdev (a release checked out, with its toolchain:
#              `make prebuilt` there). In its container image it is
#              /opt/riscos-crossdev.
#   JOBS       parallel jobs (default: the number of CPUs)
# Part of riscos-reeledit. GPL version 2 or later.
TOP=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)
if [ -z "${RCD_DIR:-}" ]; then
  for d in /opt/riscos-crossdev "$TOP/../riscos-crossdev"; do
    [ -f "$d/env.sh" ] && RCD_DIR=$(cd "$d" && pwd) && break
  done
fi
if [ ! -f "${RCD_DIR:-}/env.sh" ]; then
  echo "build/env.sh: set RCD_DIR to a riscos-crossdev checkout (see README.md)" >&2
  return 1 2>/dev/null || exit 1
fi
. "$RCD_DIR/env.sh"
DL=${DL:-$TOP/dl}
OUT=${OUT:-$TOP/out}
dep_version() { awk -v n="$1" '$1 == n {sub(/^v/, "", $2); print $2}' "$TOP/.riscos-deps"; }
MLT_VERSION=$(sed -n 's/^mlt-\([0-9.]*\)\.tar\.gz.*/\1/p' "$TOP/build/SOURCES")
FFMPEG_DEVKIT_VERSION=$(dep_version riscos-ffmpeg)
MESA_DEVKIT_VERSION=$(dep_version riscos-mesa)
export TOP DL OUT MLT_VERSION FFMPEG_DEVKIT_VERSION MESA_DEVKIT_VERSION
