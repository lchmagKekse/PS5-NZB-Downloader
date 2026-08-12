#!/usr/bin/env bash
# Builds libexpat (streaming/SAX XML parser, used for NZB parsing) for the
# x86_64-sie-ps5 target and installs it into the SDK sysroot. Modeled on
# websrv/libmicrohttpd.sh -- same recipe, different upstream package.

LIB_URL="https://github.com/libexpat/libexpat/releases/download/R_2_8_2"
LIB_VER="2.8.2"

if [[ -z "$PS5_PAYLOAD_SDK" ]]; then
    echo "error: PS5_PAYLOAD_SDK is not set"
    exit 1
fi

source ${PS5_PAYLOAD_SDK}/toolchain/prospero.sh || exit 1

TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$TEMPDIR"' EXIT

wget -O $TEMPDIR/lib.tar.gz $LIB_URL/expat-$LIB_VER.tar.gz || exit 1
tar xf $TEMPDIR/lib.tar.gz -C $TEMPDIR || exit 1

cd $TEMPDIR/expat-$LIB_VER
./configure --prefix="${PS5_HBROOT}" --host=x86_64 \
	    --disable-shared --enable-static \
	    --without-docbook --without-tests --without-examples \
	    --without-xmlwf
${MAKE} install DESTDIR="${PS5_PAYLOAD_SDK}/target"
