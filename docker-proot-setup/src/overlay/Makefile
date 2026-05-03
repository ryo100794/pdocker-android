# libcow — copy-on-write LD_PRELOAD shim
#
# Build targets:
#   make            — build libcow.so for host toolchain
#   make android    — cross-compile for Android (bionic libc) via NDK
#                     requires NDK=/path/to/android-ndk; outputs
#                     libcow-android-{arm64,x86_64}.so
#
# Android note: bionic exposes a narrower symbol set than glibc. The
# shim only hooks POSIX calls that exist in bionic, so it builds
# cleanly against the NDK.

# Resolve the project root from this Makefile's own location, so the build
# works regardless of where the repo is cloned.
PREFIX ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LIBDIR ?= $(PREFIX)/lib

# -U_FORTIFY_SOURCE drops calls to glibc's __memcpy_chk / __fprintf_chk
# so the same .so loads under musl (alpine) where those symbols don't
# exist. Without this, LD_PRELOAD on musl fails with "symbol not found".
CFLAGS  ?= -O2 -fPIC -Wall -Wextra -Wno-unused-parameter -U_FORTIFY_SOURCE
LDFLAGS ?= -shared -ldl

TARGET  = libcow.so

all: $(TARGET)

$(TARGET): libcow.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: $(TARGET)
	install -d $(LIBDIR)
	install -m 0755 $(TARGET) $(LIBDIR)/$(TARGET)

clean:
	rm -f $(TARGET) libcow-android-*.so libcow-musl.so

# ---- musl cross-build (built inside an alpine container) ----
# Requires a working pdockerd + alpine:3.18 image. Builds inside the
# container with `apk add gcc musl-dev`, then copies the result back
# via the bind mount. The resulting libcow-musl.so links against
# /lib/ld-musl-aarch64.so.1, so it loads cleanly inside alpine rootfs
# where the glibc variant fails (ld-linux-* missing).
HERE := $(shell pwd)
musl:
	@command -v docker >/dev/null 2>&1 || (echo "docker CLI required (pdockerd must be running)"; exit 1)
	rm -rf /tmp/libcow-musl-build
	mkdir -p /tmp/libcow-musl-build
	cp libcow.c /tmp/libcow-musl-build/
	docker run --rm -v /tmp/libcow-musl-build:/work alpine:3.18 /bin/sh -c '\
	    apk add --no-cache gcc musl-dev linux-headers >/dev/null && \
	    cd /work && \
	    gcc $(CFLAGS) $(LDFLAGS) -o libcow-musl.so libcow.c'
	cp /tmp/libcow-musl-build/libcow-musl.so $(HERE)/libcow-musl.so
	rm -rf /tmp/libcow-musl-build

install-musl: musl
	install -d $(LIBDIR)
	install -m 0755 libcow-musl.so $(LIBDIR)/libcow-musl.so

# ---- Android cross-compile ----
NDK       ?=
ANDROID_API ?= 24
ANDROID_TOOLCHAIN = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin

android-arm64:
	@test -n "$(NDK)"   || (echo "set NDK=/path/to/android-ndk"; exit 1)
	$(ANDROID_TOOLCHAIN)/aarch64-linux-android$(ANDROID_API)-clang \
	    $(CFLAGS) $(LDFLAGS) -o libcow-android-arm64.so libcow.c

android-x86_64:
	@test -n "$(NDK)"   || (echo "set NDK=/path/to/android-ndk"; exit 1)
	$(ANDROID_TOOLCHAIN)/x86_64-linux-android$(ANDROID_API)-clang \
	    $(CFLAGS) $(LDFLAGS) -o libcow-android-x86_64.so libcow.c

android: android-arm64 android-x86_64

.PHONY: all install clean android android-arm64 android-x86_64 musl install-musl
