#!/usr/bin/env bash
# One-time setup of the local ZMK build environment used by build-local.sh.
# Safe to re-run: finished steps are skipped, so it also repairs a partial install.
#
#   ./setup-local.sh
#
# Settings: .env (copy env.tmpl and fill it in), or the same variables in the
# environment:
#   ZMK_WORKSPACE   where everything is installed, ~3.5 GB (required)
#   ZMK_REVISION    ZMK tag/branch (default: the zmk revision from config/west.yml)
#   ZEPHYR_SDK_VER  Zephyr SDK version (default 0.16.8)
#   PYTHON          python interpreter used to create the venv (default python3)
#
# Host requirements: git, curl, python3 with the venv and lzma modules. x86_64 Linux.
# Nothing is installed system-wide; removing $ZMK_WORKSPACE removes the environment.
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
SDK_VER="${ZEPHYR_SDK_VER:-0.16.8}"
PYTHON="${PYTHON:-python3}"
REV="${ZMK_REVISION:-$(awk '$1 == "revision:" { print $2; exit }' "$CONFIG_REPO/config/west.yml")}"

[ -n "$REV" ] || { echo "Cannot find the zmk revision in config/west.yml (set ZMK_REVISION)" >&2; exit 1; }
[ "$(uname -sm)" = "Linux x86_64" ] || { echo "Only x86_64 Linux is supported (SDK download is arch-specific)" >&2; exit 1; }
for tool in git curl "$PYTHON"; do
    command -v "$tool" >/dev/null || { echo "Missing required tool: $tool" >&2; exit 1; }
done
"$PYTHON" -c 'import venv, lzma, tarfile' || { echo "$PYTHON needs the venv and lzma modules" >&2; exit 1; }

step() { echo; echo "=== $*"; }

mkdir -p "$WS/tmp" "$WS/pip-cache" "$WS/cache"
export TMPDIR="$WS/tmp" PIP_CACHE_DIR="$WS/pip-cache" XDG_CACHE_HOME="$WS/cache"
export PIP_DISABLE_PIP_VERSION_CHECK=1
cd "$WS"

step "Python environment ($WS/.venv)"
[ -x .venv/bin/python ] || "$PYTHON" -m venv .venv
# shellcheck disable=SC1091
. .venv/bin/activate
# cmake 4 dropped policies Zephyr 3.5 relies on; setuptools 81+ dropped
# pkg_resources, which the nanopb generator imports.
pip -q install west 'cmake>=3.28,<4' ninja 'setuptools<81' protobuf grpcio-tools

step "ZMK $REV ($WS/zmk)"
if [ ! -d zmk/.git ]; then
    git clone -q --depth 1 --branch "$REV" https://github.com/zmkfirmware/zmk.git zmk
else
    git -C zmk fetch -q --depth 1 origin "$REV"
    git -C zmk checkout -q --detach FETCH_HEAD
fi
[ -d zmk/.west ] || west init -l zmk/app

step "Zephyr and modules (west update)"
(cd zmk && west update --narrow -o=--depth=1)

step "Zephyr Python requirements"
pip -q install -r zmk/zephyr/scripts/requirements-base.txt

step "Zephyr SDK $SDK_VER (ARM toolchain)"
SDK_DIR="$WS/zephyr-sdk-$SDK_VER"
if [ ! -x "$SDK_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" ]; then
    base="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$SDK_VER"
    curl -fL --retry 3 -o "$TMPDIR/sdk-min.tar.xz" "$base/zephyr-sdk-${SDK_VER}_linux-x86_64_minimal.tar.xz"
    curl -fL --retry 3 -o "$TMPDIR/sdk-arm.tar.xz" "$base/toolchain_linux-x86_64_arm-zephyr-eabi.tar.xz"
    # Extracted with Python so the host does not need xz installed.
    python - "$TMPDIR/sdk-min.tar.xz" "$WS" "$TMPDIR/sdk-arm.tar.xz" "$SDK_DIR" <<'EOF'
import sys, tarfile
for archive, dest in zip(sys.argv[1::2], sys.argv[2::2]):
    with tarfile.open(archive, "r:xz") as tar:
        try:
            tar.extractall(dest, filter="fully_trusted")
        except TypeError:  # Python < 3.12
            tar.extractall(dest)
EOF
    rm -f "$TMPDIR/sdk-min.tar.xz" "$TMPDIR/sdk-arm.tar.xz"
fi
"$SDK_DIR/arm-zephyr-eabi/bin/arm-zephyr-eabi-gcc" --version | head -1

echo
echo "Done. Build with: ./build-local.sh"
