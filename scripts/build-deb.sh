#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 <Nitrux Latinoamericana S.C. <hello@nxos.org>>

set -e

PACKAGE_VERSION="${PACKAGE_VERSION:-0.0.1}"
TARGET_ARCH="${TARGET_ARCH:-$(dpkg --print-architecture)}"
PACKAGE_ROOT="build/package"
PACKAGE_FILE="build/afhs_${PACKAGE_VERSION}_${TARGET_ARCH}.deb"

rm -rf build
mkdir -p "${PACKAGE_ROOT}/DEBIAN" "${PACKAGE_ROOT}/usr/bin"

make clean
make
install -Dm755 afhs "${PACKAGE_ROOT}/usr/bin/afhs"

cat > "${PACKAGE_ROOT}/DEBIAN/control" <<EOF
Package: afhs
Version: ${PACKAGE_VERSION}
Section: utils
Priority: optional
Architecture: ${TARGET_ARCH}
Maintainer: Uri Herrera <uri_herrera@nxos.org>
Description: Aesthetic FHS directory presentation controller
 Userspace controller for Nitrux's Aesthetic FHS directory filtering.
EOF

dpkg-deb --build --root-owner-group "${PACKAGE_ROOT}" "${PACKAGE_FILE}"
