#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Leonardo Boulay
# openssl.sh DIR - the static OpenSSL that cb-mpc needs (signer/vendor/README.md), built into DIR (DIR/include, DIR/lib/libcrypto.a)
# for the compiler $CC: for Windows when it is a MinGW-w64 one, else for the machine. The release tarball is checked by its hash; the
# one change cb-mpc asks for makes the functions of crypto/ec/curve25519.c visible (their `static` removed), as its own scripts do.
set -eu
DIR=$1
VER=3.6.4
SUM=9bffaa1ad1e07b354c21bd3324ec02fa15579f45a7d0494b3e74bc449b7333ef
TAR=$DIR.src/openssl-$VER.tar.gz
CC=${CC:-cc}

mkdir -p "$DIR.src"
[ -f "$TAR" ] || curl -fsSL -o "$TAR.part" "https://github.com/openssl/openssl/releases/download/openssl-$VER/openssl-$VER.tar.gz"
[ -f "$TAR" ] || mv "$TAR.part" "$TAR"
got=$( (sha256sum "$TAR" 2>/dev/null || shasum -a 256 "$TAR") | cut -d' ' -f1)
[ "$got" = "$SUM" ] || { echo "openssl.sh: $TAR has SHA-256 $got, not $SUM" >&2; rm -f "$TAR"; exit 1; }

cd "$DIR.src"
rm -rf "openssl-$VER"
tar -xzf "openssl-$VER.tar.gz"
cd "openssl-$VER"
sed -e 's/^static//' crypto/ec/curve25519.c > curve25519.c.new && mv curve25519.c.new crypto/ec/curve25519.c

# the options of cb-mpc's own scripts (scripts/openssl/ in its repository)
OPTS="no-shared no-afalgeng no-apps no-aria no-autoload-config no-bf no-camellia no-cast no-chacha no-cmac no-cms no-crypto-mdebug
  no-comp no-cmp no-ct no-des no-dh no-dgram no-dsa no-dso no-dtls no-dynamic-engine no-ec2m no-egd no-engine no-external-tests
  no-gost no-http no-idea no-mdc2 no-md2 no-md4 no-module no-nextprotoneg no-ocb no-ocsp no-psk no-padlockeng no-poly1305
  no-quic no-rc2 no-rc4 no-rc5 no-rfc3779 no-scrypt no-sctp no-seed no-siphash no-sm2 no-sm3 no-sm4 no-sock no-srtp no-srp
  no-ssl-trace no-ssl3 no-stdio no-tests no-tls no-ts no-unit-test no-uplink no-whirlpool no-zlib"
case $($CC -dumpmachine) in
*mingw*) T=$($CC -dumpmachine)
         ./Configure mingw64 CC="$CC" AR="$T-ar" RANLIB="$T-ranlib" RC="$T-windres" $OPTS --prefix="$DIR" --libdir=lib ;;
*)       CC=$CC ./config $OPTS --prefix="$DIR" --libdir=lib ;;
esac
MAKEFLAGS= make -j4 build_generated >/dev/null   # its own jobs, not those of the make that runs this
MAKEFLAGS= make -j4 libcrypto.a >/dev/null
mkdir -p "$DIR/lib"
cp libcrypto.a "$DIR/lib/"
rm -rf "$DIR/include" && cp -R include "$DIR/include"
echo "openssl.sh: OpenSSL $VER for $($CC -dumpmachine) in $DIR"
