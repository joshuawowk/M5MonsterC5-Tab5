#!/usr/bin/env bash
set -euo pipefail

IMAGE="${IDF_DOCKER_IMAGE:-espressif/idf:v5.4.1}"
USE_DOCKER="${USE_DOCKER:-1}"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# go two levels up: .github/scripts -> repo root
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
APP_DIR="${REPO_ROOT}/"
CACHE_DIR="${IDF_CACHE_DIR:-${REPO_ROOT}/.espressif}"

usage() {
  cat <<'EOF'
Usage: container_build.sh [--no-docker]

Builds ESP32-P4 firmware using esp-idf v5.4.1.
Defaults to running inside Docker (espressif/idf:v5.4.1).

Options:
  --no-docker   Run idf.py directly (for GitHub Actions container job or host with esp-idf installed)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-docker)
      USE_DOCKER=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

# The component manager's "new versions available" check solves the dependencies
# again against the live registry, after the lock file has already been accepted.
# The manager in espressif/idf:v5.4.1 (2.1.2) cannot read newer registry manifests
# that use $CONFIG{...} conditions (esp_hosted's ESP_HOSTED_HOST_USB_CHERRYUSB_MANIFEST)
# and aborts the build with 'Environment variable "CONFIG" is not set'. The build
# uses what dependencies.lock pins either way, so the check is off unless asked for.
export IDF_COMPONENT_CHECK_NEW_VERSION="${IDF_COMPONENT_CHECK_NEW_VERSION:-0}"

# In a container the checkout belongs to another user, so git refuses it ("dubious
# ownership") and ESP-IDF falls back to PROJECT_VER "1". Trust the checkout for this
# process only (no global git config is written), so the build carries git describe.
trust_checkout() {
  export GIT_CONFIG_COUNT=1
  export GIT_CONFIG_KEY_0=safe.directory
  export GIT_CONFIG_VALUE_0="$1"
}

build_native() {
  cd "$APP_DIR"
  trust_checkout "$(pwd)"
  export IDF_TARGET=esp32p4
  if [ -f /opt/esp/idf/export.sh ]; then
    # shellcheck disable=SC1091
    . /opt/esp/idf/export.sh >/dev/null 2>&1
  fi
  local extra_flags="${IDF_PY_FLAGS:-}"
  if [[ "${IDF_TARGET}" == "esp32p4" && "${extra_flags}" != *"--preview"* ]]; then
    extra_flags="--preview ${extra_flags}"
  fi
  idf.py ${extra_flags} build
}

if [[ "$USE_DOCKER" == "0" || "$USE_DOCKER" == "false" ]]; then
  build_native
  exit 0
fi

mkdir -p "$CACHE_DIR"

docker run --rm \
  --workdir /project \
  --env IDF_TARGET=esp32p4 \
  --env IDF_PY_FLAGS="${IDF_PY_FLAGS:-}" \
  --env IDF_COMPONENT_CHECK_NEW_VERSION \
  --env GIT_CONFIG_COUNT=1 \
  --env GIT_CONFIG_KEY_0=safe.directory \
  --env GIT_CONFIG_VALUE_0=/project \
  --volume "${APP_DIR}:/project" \
  --volume "${CACHE_DIR}:/root/.espressif" \
  "$IMAGE" \
  bash -c '. /opt/esp/idf/export.sh >/dev/null 2>&1; \
           extra="${IDF_PY_FLAGS:-}"; \
           [ "$IDF_TARGET" = "esp32p4" ] && case "$extra" in (*--preview*) ;; (*) extra="--preview $extra";; esac; \
           idf.py $extra build'
