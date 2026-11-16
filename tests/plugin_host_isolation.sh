#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
core="${BAREDAW_CORE_SOURCE_DIR:-$root/external/BareDAW}"

if rg -n '#include[[:space:]]*[<"]engine/private' "$root/src"; then
  echo "PluginHost must use only public BareDAW Core APIs." >&2
  exit 1
fi

if rg -n '#include[[:space:]]*[<"]bridge/plugin-host' \
    "$core/src/core" "$core/src/engine" "$core/src/io"; then
  echo "BareDAW Core must not depend on PluginHost." >&2
  exit 1
fi

echo "plugin_host_isolation: OK"
