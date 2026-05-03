#!/usr/bin/env bash
# Verify libcow.so emulates overlayfs copy-up semantics on a
# hardlink-cloned rootfs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$HERE/libcow.so"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

LOWER="$TMP/lower"
UPPER="$TMP/merged"

# ---- prepare lower (image) ----
mkdir -p "$LOWER"
echo "image-original" > "$LOWER/hello.txt"
echo "static-content" > "$LOWER/static.txt"
mkdir -p "$LOWER/etc"
echo "127.0.0.1 image-host" > "$LOWER/etc/hosts"

# ---- clone via hardlinks (overlay upper = merged) ----
cp -al "$LOWER" "$UPPER"

# Sanity: inodes should match before any write
INO_LOWER=$(stat -c %i "$LOWER/hello.txt")
INO_UPPER=$(stat -c %i "$UPPER/hello.txt")
[ "$INO_LOWER" = "$INO_UPPER" ] || { echo "FAIL: clone not hardlinked"; exit 1; }
echo "ok: hardlink clone (inode=$INO_LOWER)"

# ---- modify inside merged with libcow loaded ----
LD_PRELOAD="$LIB" COW_DEBUG=1 bash -c "
  echo 'container-modified' > '$UPPER/hello.txt'
  echo '10.0.0.1 container-host' >> '$UPPER/etc/hosts'
  # read-only access should NOT break link
  cat '$UPPER/static.txt' > /dev/null
" 2>&1 | sed 's/^/  /'

# ---- verify: lower unchanged ----
if ! grep -q "^image-original$" "$LOWER/hello.txt"; then
  echo "FAIL: lower/hello.txt leaked container write"; exit 1
fi
if grep -q container "$LOWER/etc/hosts"; then
  echo "FAIL: lower/etc/hosts leaked container write"; exit 1
fi
echo "ok: lower preserved"

# ---- verify: upper has new content ----
if ! grep -q "^container-modified$" "$UPPER/hello.txt"; then
  echo "FAIL: upper/hello.txt missing write"; exit 1
fi
echo "ok: upper shows container write"

# ---- verify: inodes now differ for written files ----
INO_LOWER2=$(stat -c %i "$LOWER/hello.txt")
INO_UPPER2=$(stat -c %i "$UPPER/hello.txt")
[ "$INO_LOWER2" != "$INO_UPPER2" ] || { echo "FAIL: hardlink not broken"; exit 1; }
echo "ok: hardlink broken on write ($INO_LOWER2 vs $INO_UPPER2)"

# ---- verify: read-only static.txt still shares inode ----
INO_STATIC_L=$(stat -c %i "$LOWER/static.txt")
INO_STATIC_U=$(stat -c %i "$UPPER/static.txt")
[ "$INO_STATIC_L" = "$INO_STATIC_U" ] || { echo "FAIL: read-only file was copied unnecessarily"; exit 1; }
echo "ok: read-only file still shares inode (no unnecessary copy)"

# ---- metadata hooks (chmod / chown / utimes) must also break-on-write ----
# Prepare fresh hardlinked files for each operation
echo "meta-chmod" > "$LOWER/chmod.txt"
echo "meta-utime" > "$LOWER/utime.txt"
chmod 644 "$LOWER/chmod.txt"
ln "$LOWER/chmod.txt" "$UPPER/chmod.txt"
ln "$LOWER/utime.txt" "$UPPER/utime.txt"
LOWER_MODE_BEFORE=$(stat -c %a "$LOWER/chmod.txt")
LOWER_MTIME_BEFORE=$(stat -c %Y "$LOWER/utime.txt")

# chmod inside container — must NOT change lower's mode (libcow copies first)
LD_PRELOAD="$LIB" bash -c "chmod 700 '$UPPER/chmod.txt'" 2>/dev/null
LOWER_MODE_AFTER=$(stat -c %a "$LOWER/chmod.txt")
UPPER_MODE_AFTER=$(stat -c %a "$UPPER/chmod.txt")
[ "$LOWER_MODE_AFTER" = "$LOWER_MODE_BEFORE" ] \
    || { echo "FAIL: chmod leaked to lower ($LOWER_MODE_BEFORE → $LOWER_MODE_AFTER)"; exit 1; }
[ "$UPPER_MODE_AFTER" = "700" ] \
    || { echo "FAIL: chmod didn't apply to upper (got $UPPER_MODE_AFTER)"; exit 1; }
echo "ok: chmod isolated (lower=$LOWER_MODE_AFTER, upper=$UPPER_MODE_AFTER)"

# utimes via `touch -d` — must NOT change lower's mtime
LD_PRELOAD="$LIB" bash -c "touch -d '2000-01-01 00:00:00' '$UPPER/utime.txt'" 2>/dev/null
LOWER_MTIME_AFTER=$(stat -c %Y "$LOWER/utime.txt")
UPPER_MTIME_AFTER=$(stat -c %Y "$UPPER/utime.txt")
[ "$LOWER_MTIME_AFTER" = "$LOWER_MTIME_BEFORE" ] \
    || { echo "FAIL: utimes leaked to lower"; exit 1; }
[ "$UPPER_MTIME_AFTER" != "$LOWER_MTIME_BEFORE" ] \
    || { echo "FAIL: utimes didn't change upper mtime"; exit 1; }
echo "ok: utimes isolated (lower mtime preserved)"

# ---- xattr: setxattr must break link AND copy existing xattrs on break ----
if command -v setfattr >/dev/null 2>&1 && command -v getfattr >/dev/null 2>&1; then
    echo "pre-xattr" > "$LOWER/xattr.txt"
    # seed lower with a user xattr (must be set BEFORE hardlink so upper inherits)
    setfattr -n user.seed -v "lowerval" "$LOWER/xattr.txt" 2>/dev/null \
        && HAS_XATTR=1 || HAS_XATTR=0
    if [ "$HAS_XATTR" = "1" ]; then
        ln "$LOWER/xattr.txt" "$UPPER/xattr.txt"
        LOWER_SEED_BEFORE=$(getfattr -n user.seed --only-values "$LOWER/xattr.txt" 2>/dev/null || true)
        # container adds a NEW xattr — break_hardlink must occur AND preserve seed
        LD_PRELOAD="$LIB" bash -c "setfattr -n user.new -v upperval '$UPPER/xattr.txt'" 2>/dev/null
        LOWER_SEED_AFTER=$(getfattr -n user.seed --only-values "$LOWER/xattr.txt" 2>/dev/null || true)
        LOWER_HAS_NEW=$(getfattr -n user.new --only-values "$LOWER/xattr.txt" 2>/dev/null || true)
        UPPER_HAS_SEED=$(getfattr -n user.seed --only-values "$UPPER/xattr.txt" 2>/dev/null || true)
        UPPER_HAS_NEW=$(getfattr -n user.new --only-values "$UPPER/xattr.txt" 2>/dev/null || true)
        [ "$LOWER_SEED_AFTER" = "$LOWER_SEED_BEFORE" ] \
            || { echo "FAIL: lower seed xattr changed"; exit 1; }
        [ -z "$LOWER_HAS_NEW" ] \
            || { echo "FAIL: new xattr leaked to lower"; exit 1; }
        [ "$UPPER_HAS_SEED" = "lowerval" ] \
            || { echo "FAIL: seed xattr lost on copy-up (got '$UPPER_HAS_SEED')"; exit 1; }
        [ "$UPPER_HAS_NEW" = "upperval" ] \
            || { echo "FAIL: upper missing new xattr"; exit 1; }
        echo "ok: xattr isolated + preserved across copy-up"
    else
        echo "skip: xattr test (filesystem doesn't support user.* xattrs)"
    fi
else
    echo "skip: xattr test (setfattr/getfattr not installed)"
fi

# ---- fd-based chmod emulation via /proc/self/fd ----
# Open file RDONLY then fchmod the fd — must not leak to lower.
# Use a tiny Python one-liner to exercise the fchmod syscall path.
echo "fd-mode" > "$LOWER/fchmod.txt"
chmod 644 "$LOWER/fchmod.txt"
ln "$LOWER/fchmod.txt" "$UPPER/fchmod.txt"
LOWER_FMODE_BEFORE=$(stat -c %a "$LOWER/fchmod.txt")
LD_PRELOAD="$LIB" python3 -c "
import os
fd = os.open('$UPPER/fchmod.txt', os.O_RDONLY)
os.fchmod(fd, 0o600)
os.close(fd)
" 2>/dev/null
LOWER_FMODE_AFTER=$(stat -c %a "$LOWER/fchmod.txt")
UPPER_FMODE_AFTER=$(stat -c %a "$UPPER/fchmod.txt")
[ "$LOWER_FMODE_AFTER" = "$LOWER_FMODE_BEFORE" ] \
    || { echo "FAIL: fchmod(fd) leaked to lower ($LOWER_FMODE_BEFORE → $LOWER_FMODE_AFTER)"; exit 1; }
[ "$UPPER_FMODE_AFTER" = "600" ] \
    || { echo "FAIL: fchmod(fd) didn't apply to upper (got $UPPER_FMODE_AFTER)"; exit 1; }
echo "ok: fchmod(fd) emulated via path (lower preserved)"

echo
echo "ALL TESTS PASSED"
