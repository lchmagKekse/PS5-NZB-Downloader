#!/usr/bin/env bash
# Builds liblzma (LZMA/XZ compression, needed for 7z archive support in
# libarchive) for the x86_64-sie-ps5 target and installs it into the SDK
# sysroot. Only the library is needed, not the xz/xzdec CLI tools.

LIB_URL="https://github.com/tukaani-project/xz/releases/download"
LIB_VER="5.8.3"

if [[ -z "$PS5_PAYLOAD_SDK" ]]; then
    echo "error: PS5_PAYLOAD_SDK is not set"
    exit 1
fi

source ${PS5_PAYLOAD_SDK}/toolchain/prospero.sh || exit 1

TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$TEMPDIR"' EXIT

wget -L -O $TEMPDIR/lib.tar.gz $LIB_URL/v$LIB_VER/xz-$LIB_VER.tar.gz || exit 1
tar xf $TEMPDIR/lib.tar.gz -C $TEMPDIR || exit 1

cd $TEMPDIR/xz-$LIB_VER
./configure --prefix="${PS5_HBROOT}" --host=x86_64 \
	    --disable-shared --enable-static \
	    --disable-xz --disable-xzdec --disable-lzmadec \
	    --disable-lzmainfo --disable-scripts --disable-doc \
	    --disable-nls || exit 1
${MAKE} install DESTDIR="${PS5_PAYLOAD_SDK}/target"
