#!/usr/bin/env bash
# Builds zlib (DEFLATE, needed for ZIP archive support in libarchive) for
# the x86_64-sie-ps5 target and installs it into the SDK sysroot.

LIB_URL="https://zlib.net"
LIB_VER="1.3.2"

if [[ -z "$PS5_PAYLOAD_SDK" ]]; then
    echo "error: PS5_PAYLOAD_SDK is not set"
    exit 1
fi

source ${PS5_PAYLOAD_SDK}/toolchain/prospero.sh || exit 1

TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$TEMPDIR"' EXIT

wget -O $TEMPDIR/lib.tar.gz $LIB_URL/zlib-$LIB_VER.tar.gz || exit 1
tar xf $TEMPDIR/lib.tar.gz -C $TEMPDIR || exit 1

cd $TEMPDIR/zlib-$LIB_VER
./configure --prefix="${PS5_HBROOT}" --static || exit 1
${MAKE} install DESTDIR="${PS5_PAYLOAD_SDK}/target"
