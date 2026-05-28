#!/bin/bash
# SessionStart hook — provision the build/test toolchain for Claude Code on
# the web so `idf.py` (firmware build) and the host unit tests work.
#
# Mirrors .github/workflows/ci.yml: ESP-IDF v6.0.1 / esp32s3 for the two
# firmware apps, and CMake + Unity for the native host tests.
set -euo pipefail

# Only needed in the remote (web) container. Local developers manage their
# own ESP-IDF install.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

IDF_VERSION="v6.0.1"
IDF_TARGET="esp32s3"
IDF_DIR="$HOME/esp/esp-idf"

# ---- ESP-IDF (firmware build for apps/main + apps/recovery) ---------------
if [ ! -f "$IDF_DIR/export.sh" ]; then
  echo "[session-start] Installing ESP-IDF ${IDF_VERSION} (one-time; cached afterwards)..."
  mkdir -p "$HOME/esp"
  git clone --depth 1 -b "$IDF_VERSION" --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

# openocd (pulled by install.sh) fails to load without libusb, and that
# verification error aborts install.sh. openocd is JTAG flash/debug only —
# not needed to build — so install libusb best-effort and don't let the
# openocd check sink the whole setup.
if ! ldconfig -p 2>/dev/null | grep -q 'libusb-1.0.so.0'; then
  if command -v apt-get >/dev/null 2>&1; then
    apt-get install -y -qq libusb-1.0-0 >/dev/null 2>&1 || \
      echo "[session-start] note: could not install libusb-1.0-0; openocd will be skipped (build unaffected)"
  fi
fi

# This environment's network policy 403s dl.espressif.com, so the optional
# Python dependency-constraints file can't be fetched. Skip it — packages
# still install from PyPI, just unpinned. Without this, venv creation aborts.
export IDF_PYTHON_CHECK_CONSTRAINTS=no

# install.sh is idempotent — it skips toolchains that are already present.
# Tolerate a non-zero exit (e.g. the openocd/libusb verification above); we
# confirm idf.py itself is usable after sourcing the environment.
"$IDF_DIR/install.sh" "$IDF_TARGET" || \
  echo "[session-start] note: install.sh reported a non-build tool issue; continuing"

# Pull ESP-IDF's environment (idf.py, xtensa toolchain, Python venv) into this
# shell. export.sh references unset vars, so relax `set -u` while sourcing.
set +u
# shellcheck disable=SC1091
source "$IDF_DIR/export.sh" >/dev/null 2>&1
set -u

# Fail loudly if the firmware build tool didn't actually land on PATH.
if ! command -v idf.py >/dev/null 2>&1; then
  echo "[session-start] ERROR: idf.py not on PATH after setup — firmware builds will not work"
  exit 1
fi

# ---- Vendor the registry-hosted components from GitHub --------------------
# apps/main pulls `joltwallet/littlefs` and `espressif/cjson` from the ESP
# Component Registry, but this environment's network policy 403s
# components.espressif.com. GitHub is reachable, so fetch them from source and
# drop them into apps/main/components/ under their registry-namespaced names,
# where the build finds them with the component manager turned off. These dirs
# are .gitignore'd — they are session-local, not committed.
MAIN_COMPONENTS="$CLAUDE_PROJECT_DIR/apps/main/components"

if [ ! -f "$MAIN_COMPONENTS/joltwallet__littlefs/CMakeLists.txt" ]; then
  echo "[session-start] Vendoring joltwallet/littlefs from GitHub..."
  rm -rf "$MAIN_COMPONENTS/joltwallet__littlefs"
  tmp="$(mktemp -d)"
  git clone --depth 1 -b v1.21.1 --recurse-submodules --shallow-submodules \
    https://github.com/joltwallet/esp_littlefs.git "$tmp/lfs"
  find "$tmp/lfs" -name .git -exec rm -rf {} + 2>/dev/null || true
  mv "$tmp/lfs" "$MAIN_COMPONENTS/joltwallet__littlefs"
  rm -rf "$tmp"
fi

if [ ! -f "$MAIN_COMPONENTS/espressif__cjson/CMakeLists.txt" ]; then
  echo "[session-start] Vendoring espressif/cjson from GitHub..."
  rm -rf "$MAIN_COMPONENTS/espressif__cjson"
  tmp="$(mktemp -d)"
  git clone --depth 1 https://github.com/espressif/idf-extra-components.git "$tmp/iec"
  ( cd "$tmp/iec" && git submodule update --init --depth 1 cjson/cJSON )
  find "$tmp/iec/cjson" -name .git -exec rm -rf {} + 2>/dev/null || true
  mv "$tmp/iec/cjson" "$MAIN_COMPONENTS/espressif__cjson"
  rm -rf "$tmp"
fi

# Persist the toolchain environment for every turn in the session.
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  {
    echo "export IDF_PATH=\"$IDF_PATH\""
    [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && echo "export IDF_PYTHON_ENV_PATH=\"$IDF_PYTHON_ENV_PATH\""
    [ -n "${ESP_IDF_VERSION:-}" ] && echo "export ESP_IDF_VERSION=\"$ESP_IDF_VERSION\""
    # idf.py re-checks the constraints file on every run; keep it disabled in
    # the session too (dl.espressif.com is blocked here) so each call is clean.
    echo "export IDF_PYTHON_CHECK_CONSTRAINTS=no"
    # The component registry is blocked; deps are vendored into
    # apps/main/components above, so run with the manager off to stop idf.py
    # from trying to reach the registry on every build.
    echo "export IDF_COMPONENT_MANAGER=0"
    echo "export PATH=\"$PATH\""
  } >> "$CLAUDE_ENV_FILE"
fi

# ---- Host unit tests (pure-logic, native gcc — no ESP-IDF) ----------------
# Configure the build dir so `ctest` is ready to run. FetchContent pulls Unity
# from GitHub here so the first `cmake --build` needs no network.
cmake -S "$CLAUDE_PROJECT_DIR/tests/host" -B "$CLAUDE_PROJECT_DIR/tests/host/build" >/dev/null

echo "[session-start] Ready: idf.py on PATH (ESP-IDF ${IDF_VERSION}); host tests configured."
echo "[session-start]   firmware:   (cd apps/main && idf.py build)   /   (cd apps/recovery && idf.py build)"
echo "[session-start]   host tests: cmake --build tests/host/build && ctest --test-dir tests/host/build"
