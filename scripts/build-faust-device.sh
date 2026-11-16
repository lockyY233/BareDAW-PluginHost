#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || $# -gt 4 ]]; then
  echo "usage: $0 INPUT.dsp TYPE OUTPUT_LIBRARY [CXX]" >&2
  exit 2
fi

input=$1
type=$2
output=$3
cxx=${4:-${CXX:-c++}}
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/baredaw-faust-plugin.XXXXXX")
trap 'rm -rf "$work"' EXIT

if [[ ! $type =~ ^[A-Za-z_][A-Za-z0-9_.-]*$ ]]; then
  echo "invalid device type: $type" >&2
  exit 2
fi
command -v faust >/dev/null || { echo "faust compiler not found" >&2; exit 1; }

class_name="BareDAWPostDSP"
faust -lang cpp -cn "$class_name" -es 1 -single -ftz 0 "$input" -o "$work/device.cpp"
sed \
  -e "s|@TYPE@|$type|g" \
  -e "s|@GENERATED@|$work/device.cpp|g" \
  "$root/src/bridge/faust/PostBuildPluginTemplate.cpp.in" > "$work/plugin.cpp"

mkdir -p "$(dirname "$output")"
core="$root/external/BareDAW"
flags=(
  -std=c++17 -O2 -fPIC -fno-rtti
  -I"$root/src"
  -I"$core/src"
  -I"$core/src/core"
  -I"$core/src/engine/public"
  -I"$core/src/io"
)
if [[ $(uname -s) == Darwin ]]; then
  "$cxx" "${flags[@]}" -dynamiclib "$work/plugin.cpp" -o "$output"
else
  "$cxx" "${flags[@]}" -shared "$work/plugin.cpp" -o "$output"
fi
echo "$output"
