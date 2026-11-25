#!/usr/bin/env bash
# Local firmware build: builds every target listed in build.yaml, the same way
# the GitHub Actions workflow does, without needing GitHub.
#
#   ./build-local.sh              build all targets from build.yaml
#   ./build-local.sh left         build only targets whose name contains "left"
#   ./build-local.sh -p [filter]  pristine build (wipe the build dir first)
#
# Output: firmware/<shield>-<board>-zmk.uf2
# Settings: .env (copy env.tmpl). Run setup-local.sh once before the first build.
set -euo pipefail

CONFIG_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Settings come from .env next to this script (see env.tmpl); variables already
# set in the environment take precedence over it.
if [ -f "$CONFIG_REPO/.env" ]; then
    for v in ZMK_WORKSPACE ZMK_REVISION ZEPHYR_SDK_VER PYTHON; do
        [ -z "${!v:-}" ] || declare "_keep_$v=${!v}"
    done
    set -a; . "$CONFIG_REPO/.env"; set +a
    for v in ZMK_WORKSPACE ZMK_REVISION ZEPHYR_SDK_VER PYTHON; do
        k="_keep_$v"; [ -z "${!k:-}" ] || export "$v=${!k}"
    done
fi
WS="${ZMK_WORKSPACE:?ZMK_WORKSPACE is not set: copy env.tmpl to .env and fill it in}"
SDK="$(ls -d "$WS"/zephyr-sdk-* 2>/dev/null | head -1 || true)"

[ -d "$WS/zmk/app" ] || { echo "ZMK workspace not found in $WS: run setup-local.sh first" >&2; exit 1; }
[ -n "$SDK" ] || { echo "Zephyr SDK not found in $WS: run setup-local.sh first" >&2; exit 1; }

PRISTINE=auto
if [ "${1:-}" = "-p" ]; then PRISTINE=always; shift; fi
FILTER="${1:-}"

# shellcheck disable=SC1091
. "$WS/.venv/bin/activate"
export ZEPHYR_SDK_INSTALL_DIR="$SDK" ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export TMPDIR="$WS/tmp" XDG_CACHE_HOME="$WS/cache"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME" "$CONFIG_REPO/firmware" "$WS/build"

# One line per target: board <TAB> shield <TAB> snippet <TAB> cmake-args
TARGETS="$(python3 - "$CONFIG_REPO/build.yaml" <<'EOF'
import sys, yaml
for t in (yaml.safe_load(open(sys.argv[1])) or {}).get("include", []):
    print("\t".join(t.get(k) or "-" for k in ("board", "shield", "snippet", "cmake-args")))
EOF
)"

cd "$WS/zmk"
built=0
while IFS=$'\t' read -r board shield snippet cmake_args; do
    [ -n "$board" ] || continue
    name="${board}-zmk"
    [ "$shield" = "-" ] || name="${shield// /-}-${name}"
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then continue; fi

    args=(-s app -d "$WS/build/$name" -b "$board" -p "$PRISTINE")
    [ "$snippet" = "-" ] || args+=(-S "$snippet")
    args+=(-- "-DZMK_CONFIG=$CONFIG_REPO/config" "-DZMK_EXTRA_MODULES=$CONFIG_REPO")
    [ "$shield" = "-" ] || args+=("-DSHIELD=$shield")
    # shellcheck disable=SC2206
    [ "$cmake_args" = "-" ] || args+=($cmake_args)

    echo "=== $name"
    if ! west build "${args[@]}" > "$WS/build/$name.log" 2>&1 < /dev/null; then
        tail -30 "$WS/build/$name.log" >&2
        echo "FAILED: $name (full log: $WS/build/$name.log)" >&2
        exit 1
    fi
    cp "$WS/build/$name/zephyr/zmk.uf2" "$CONFIG_REPO/firmware/$name.uf2"
    echo "    -> $CONFIG_REPO/firmware/$name.uf2"
    built=$((built + 1))
done <<< "$TARGETS"

[ "$built" -gt 0 ] || { echo "No targets matched '$FILTER'" >&2; exit 1; }
echo "Built $built target(s)."
