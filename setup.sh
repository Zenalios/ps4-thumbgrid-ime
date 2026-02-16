#!/bin/bash
# setup.sh - Verify build environment and show status
# Run from the custom-ime directory

set -e

echo "=== Custom IME Build Environment Check ==="
echo ""

# Check OO_PS4_TOOLCHAIN
if [ -z "$OO_PS4_TOOLCHAIN" ]; then
    echo "[FAIL] OO_PS4_TOOLCHAIN not set"
    echo "  Fix: export OO_PS4_TOOLCHAIN=~/ps4/OpenOrbis-PS4-Toolchain-release/OpenOrbis/PS4Toolchain"
    exit 1
fi
echo "[OK] OO_PS4_TOOLCHAIN = $OO_PS4_TOOLCHAIN"

# Check GOLDHEN_SDK
if [ -z "$GOLDHEN_SDK" ]; then
    echo "[FAIL] GOLDHEN_SDK not set"
    echo "  Fix: export GOLDHEN_SDK=~/ps4/GoldHEN_Plugins_SDK"
    exit 1
fi
echo "[OK] GOLDHEN_SDK = $GOLDHEN_SDK"

# Check clang
if ! command -v clang &> /dev/null; then
    echo "[FAIL] clang not found"
    echo "  Fix: sudo apt install clang"
    exit 1
fi
echo "[OK] clang: $(clang --version | head -1)"

# Check ld.lld
if ! command -v ld.lld &> /dev/null; then
    echo "[FAIL] ld.lld not found"
    echo "  Fix: sudo apt install lld"
    exit 1
fi
echo "[OK] ld.lld: $(ld.lld --version | head -1)"

# Check GoldHEN library
if [ ! -f "$GOLDHEN_SDK/libGoldHEN_Hook.a" ]; then
    echo "[FAIL] libGoldHEN_Hook.a not found"
    echo "  Fix: cd $GOLDHEN_SDK && make"
    exit 1
fi
echo "[OK] libGoldHEN_Hook.a exists"

# Check CRT object
if [ ! -f "$GOLDHEN_SDK/build/crtprx.o" ]; then
    echo "[FAIL] crtprx.o not found"
    echo "  Fix: cd $GOLDHEN_SDK && make"
    exit 1
fi
echo "[OK] crtprx.o exists"

# Check create-fself
FSELF="$OO_PS4_TOOLCHAIN/bin/linux/create-fself"
if [ ! -f "$FSELF" ]; then
    echo "[FAIL] create-fself not found at $FSELF"
    exit 1
fi
if [ ! -x "$FSELF" ]; then
    echo "[WARN] create-fself not executable, fixing..."
    chmod +x "$FSELF"
fi
echo "[OK] create-fself exists"

# Check link.x
if [ ! -f "$OO_PS4_TOOLCHAIN/link.x" ]; then
    echo "[FAIL] link.x not found at $OO_PS4_TOOLCHAIN/link.x"
    exit 1
fi
echo "[OK] link.x exists"

echo ""
echo "=== All checks passed! ==="
echo ""
echo "To build:  make"
echo "To clean:  make clean"
echo ""
