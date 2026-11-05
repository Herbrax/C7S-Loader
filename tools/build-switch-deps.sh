#!/usr/bin/env bash
set -euo pipefail

MHD_VER=1.0.10
MHD_SHA256=04bfe8ef75db7d629a33de767599765cecadc56274a39822d5d081030d577685

root="$(cd "$(dirname "$0")/.." && pwd)"
prefix="$root/.deps/switch"
src="$root/.deps/src"
build="$root/.deps/build-switch"

[[ -f "$prefix/lib/libmicrohttpd.a" ]] && exit 0
: "${DEVKITPRO:?set DEVKITPRO, e.g. export DEVKITPRO=/opt/devkitpro}"
[[ -f "$DEVKITPRO/switchvars.sh" ]] || { echo "no $DEVKITPRO/switchvars.sh; install switch-dev" >&2; exit 1; }

mkdir -p "$src" "$build" "$prefix/lib" "$prefix/include"

if [[ ! -f "$src/libmicrohttpd-$MHD_VER.tar.gz" ]]; then
  curl -fsSL -o "$src/libmicrohttpd-$MHD_VER.tar.gz" \
    "https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-$MHD_VER.tar.gz"
fi
if command -v sha256sum >/dev/null 2>&1; then
  echo "$MHD_SHA256  $src/libmicrohttpd-$MHD_VER.tar.gz" | sha256sum -c - >/dev/null
else
  echo "$MHD_SHA256  $src/libmicrohttpd-$MHD_VER.tar.gz" | shasum -a 256 -c - >/dev/null
fi

rm -rf "$build/libmicrohttpd-$MHD_VER" "$build/mhd"
tar -xzf - -C "$build" < "$src/libmicrohttpd-$MHD_VER.tar.gz"
mkdir -p "$build/mhd"

dkp_prefix="$DEVKITPRO"
source "$DEVKITPRO/switchvars.sh"
if [ "$dkp_prefix" != "/opt/devkitpro" ]; then
  for v in CPPFLAGS CFLAGS CXXFLAGS LDFLAGS PORTLIBS_PREFIX PORTLIBS_ROOT; do
    eval "export $v=\"\${$v//\/opt\/devkitpro/$dkp_prefix}\""
  done
  export DEVKITPRO="$dkp_prefix"
  export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
fi
export LDFLAGS="-specs=$DEVKITPRO/libnx/switch.specs -L$prefix/lib $LDFLAGS"
export CPPFLAGS="-I$prefix/include $CPPFLAGS"
MAKE="${MAKE:-make}"

compat="$prefix/lib/libc7s-nx-compat.a"
echo "building the socketpair shim for libmicrohttpd's link tests"
"$CC" $CFLAGS $CPPFLAGS -I"$root/include" -c "$root/source/nx_net.c" -o "$build/nx_net.o"
rm -f "$compat"
"${AR:-aarch64-none-elf-gcc-ar}" rcs "$compat" "$build/nx_net.o"
cp "$root/include/nx_net.h" "$prefix/include/"
export LIBS="-lc7s-nx-compat $LIBS"

jobs="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null) | head -n1 )"
[[ -n "$jobs" ]] || jobs=4

(
  cd "$build/mhd"
  CFLAGS="$CFLAGS -O2 -ffunction-sections -Dsocketpair=chiral_socketpair" \
  CPPFLAGS="${CPPFLAGS:-}" \
  LDFLAGS="$LDFLAGS" \
  LIBS="$LIBS" \
  "$build/libmicrohttpd-$MHD_VER/configure" \
    --host=aarch64-none-elf \
    --prefix="$prefix" \
    --disable-shared --enable-static \
    --disable-dependency-tracking \
    --disable-https --without-gnutls --disable-curl \
    --disable-doc --disable-examples --disable-tools \
    --disable-epoll --disable-sendfile \
    --enable-itc=socketpair
)

"$MAKE" -C "$build/mhd/src/microhttpd" -j"$jobs" libmicrohttpd.la
cp "$build/mhd/src/microhttpd/.libs/libmicrohttpd.a" "$prefix/lib/"
cp "$build/libmicrohttpd-$MHD_VER/src/include/microhttpd.h" "$prefix/include/"
echo "libmicrohttpd $MHD_VER built for the Switch in $prefix"
