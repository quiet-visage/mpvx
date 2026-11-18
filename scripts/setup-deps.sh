#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DEPS_DIR="${PROJECT_ROOT}/.deps/libmpv"

echo "=== mpvx Dependency Setup ==="

mkdir -p "${DEPS_DIR}"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "${TMP_DIR}"' EXIT

cd "${TMP_DIR}"
echo "Downloading required RPMs..."
PACKAGES=(
    mpv-devel.x86_64
    mpv-libs.x86_64
    libinput.x86_64
    libwacom.x86_64
    libevdev.x86_64
    mtdev.x86_64
)

dnf download -q -y "${PACKAGES[@]}" 2>/dev/null || \
dnf download -y "${PACKAGES[@]}"

for rpm in *.rpm; do
    echo "Extracting ${rpm}..."
    rpm2cpio "${rpm}" | (cd "${DEPS_DIR}" && cpio -idmu --quiet)
done

# Standardize layout into include/ and lib/
if [[ -d "${DEPS_DIR}/usr" ]]; then
    mkdir -p "${DEPS_DIR}/include" "${DEPS_DIR}/lib" "${DEPS_DIR}/lib/pkgconfig"
    cp -r "${DEPS_DIR}/usr/include/"* "${DEPS_DIR}/include/" 2>/dev/null || true
    
    if [[ -d "${DEPS_DIR}/usr/lib64" ]]; then
        cp -r "${DEPS_DIR}/usr/lib64/"* "${DEPS_DIR}/lib/" 2>/dev/null || true
    elif [[ -d "${DEPS_DIR}/usr/lib" ]]; then
        cp -r "${DEPS_DIR}/usr/lib/"* "${DEPS_DIR}/lib/" 2>/dev/null || true
    fi

    # Fix paths inside .pc file if present
    if [[ -f "${DEPS_DIR}/lib/pkgconfig/mpv.pc" ]]; then
        sed -i "s|^prefix=.*|prefix=${DEPS_DIR}|g" "${DEPS_DIR}/lib/pkgconfig/mpv.pc"
        sed -i "s|/usr/lib64|${DEPS_DIR}/lib|g" "${DEPS_DIR}/lib/pkgconfig/mpv.pc"
        sed -i "s|/usr/include|${DEPS_DIR}/include|g" "${DEPS_DIR}/lib/pkgconfig/mpv.pc"
    fi
fi

# Create unversioned .so symlinks if missing
cd "${DEPS_DIR}/lib"
for lib in *.so.*; do
    [ -f "${lib}" ] || continue
    base="${lib%%.so*}.so"
    if [ ! -e "${base}" ]; then
        ln -sf "${lib}" "${base}"
    fi
done

echo "Successfully set up dependencies at ${DEPS_DIR}"
