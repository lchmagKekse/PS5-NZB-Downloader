#!/usr/bin/env bash
# Builds libarchive (RAR/RAR5 read support is built into libarchive
# itself, no external unrar; ZIP DEFLATE via zlib) for the
# x86_64-sie-ps5 target and installs it into the SDK sysroot.
#
# bz2/zstd/lzo2/xml2 support is intentionally left out to keep the
# dependency chain small -- RAR/ZIP/7z cover the overwhelming majority
# of Usenet release packaging. lzma IS enabled (liblzma ported
# separately, see xz.sh) for 7z support.
#
# --with-openssl: without ANY crypto backend (the original build used
# --without-openssl/nettle/cng/mbedtls), libarchive's format readers have
# no way to perform AES operations at all -- meaning RAR5, WinZip-style
# AES-encrypted ZIP, and AES-256 7z all fail to decrypt regardless of
# whether a passphrase is registered via archive_read_add_passphrase().
# OpenSSL (libcrypto) is already built in the SDK sysroot for NNTPS, so
# this reuses it rather than adding a second crypto dependency.

LIB_URL="https://github.com/libarchive/libarchive/releases/download"
LIB_VER="3.8.9"

if [[ -z "$PS5_PAYLOAD_SDK" ]]; then
    echo "error: PS5_PAYLOAD_SDK is not set"
    exit 1
fi

source ${PS5_PAYLOAD_SDK}/toolchain/prospero.sh || exit 1

TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$TEMPDIR"' EXIT

wget -L -O $TEMPDIR/lib.tar.gz $LIB_URL/v$LIB_VER/libarchive-$LIB_VER.tar.gz || exit 1
tar xf $TEMPDIR/lib.tar.gz -C $TEMPDIR || exit 1

cd $TEMPDIR/libarchive-$LIB_VER
PKG_CONFIG_PATH="${PS5_PAYLOAD_SDK}/target/user/homebrew/lib/pkgconfig" \
./configure --prefix="${PS5_HBROOT}" --host=x86_64 \
	    --disable-shared --enable-static \
	    --without-bz2lib --without-libb2 --without-iconv \
	    --without-lz4 --with-lzma --without-lzo2 \
	    --without-zstd --without-xml2 --without-expat \
	    --with-openssl --without-nettle \
	    --without-cng --without-mbedtls \
	    --disable-bsdtar --disable-bsdcat --disable-bsdcpio \
	    --disable-acl --disable-xattr || exit 1
${MAKE} install DESTDIR="${PS5_PAYLOAD_SDK}/target"
