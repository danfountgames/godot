#!/usr/bin/env bash
set -euo pipefail

# Comprehensive GodotBeam DevPlayer test suite
# Tests: boot, mount, unmount, two-project switch, autoload reset,
#        class_name isolation, script output verification, stress cycling

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$ROOT_DIR/bin/godot.linuxbsd.editor.x86_64"
PASS_COUNT=0
FAIL_COUNT=0
TEST_NUM=0

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "  FAIL: $1"
}

check() {
    # Use grep without -q to avoid SIGPIPE issues with pipefail.
    # Redirect output to /dev/null instead.
    if echo "$OUTPUT" | grep "$2" > /dev/null 2>&1; then
        pass "$1"
    else
        fail "$1"
        echo "    Expected to find: $2"
    fi
}

check_not() {
    if echo "$OUTPUT" | grep "$2" > /dev/null 2>&1; then
        fail "$1"
        echo "    Should NOT contain: $2"
    else
        pass "$1"
    fi
}

if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "Run: scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j\$(nproc)"
    exit 1
fi

echo "=== GodotBeam DevPlayer Comprehensive Test Suite ==="
echo ""

# ========================
# Test 1: Basic shell boot
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Basic DevPlayer shell boot ---"
OUTPUT=$("$BINARY" --devplayer --headless --quit-after 2000 2>&1 || true)
check "Module initialized" "\[DevPlayer\] Module initialized"
check "Shell UI created" "DevPlayer: Shell UI created"
check "Clean shutdown" "\[DevPlayer\] Module shut down"
echo ""

# ========================
# Test 2: Auto-mount minimal_2d
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Auto-mount minimal_2d project ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/minimal_2d" --quit-after 5000 2>&1 || true)
check "Settings loaded" "Project settings loaded successfully"
check "Main scene resolved" "Main scene setting: res://main.tscn"
check "Project mounted" "\[ProjectDomainManager\] Project mounted"
check "Scene instantiated" "Scene instantiated and added to SceneTree"
check "Project launched" "PROJECT LAUNCHED"
check "GDScript executed" "Minimal 2D project loaded"
echo ""

# ========================
# Test 3: Mount class_name test A (verify Enemy class + output)
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Mount class_name_collision_test_a ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/class_name_collision_test_a" --quit-after 5000 2>&1 || true)
check "Settings loaded" "Project settings loaded successfully"
check "Scene launched" "PROJECT LAUNCHED"
check "Enemy class registered" "Registered global class: Enemy"
check "Enemy behavior A" "Project A Enemy - Aggressive"
echo ""

# ========================
# Test 4: Mount class_name test B (verify DIFFERENT Enemy class)
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Mount class_name_collision_test_b ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/class_name_collision_test_b" --quit-after 5000 2>&1 || true)
check "Settings loaded" "Project settings loaded successfully"
check "Scene launched" "PROJECT LAUNCHED"
check "Enemy class registered" "Registered global class: Enemy"
check "Enemy behavior B" "Project B Enemy - Passive"
check_not "No stale A data" "Project A Enemy - Aggressive"
echo ""

# ========================
# Test 5: Mount autoload_reset_test (verify autoload lifecycle)
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Mount autoload_reset_test ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/autoload_reset_test" --quit-after 5000 2>&1 || true)
check "Settings loaded" "Project settings loaded successfully"
check "Scene launched" "PROJECT LAUNCHED"
check "Autoloads built" "Built.*autoload"
echo ""

# ========================
# Test 6: Automated cycling test + autoload contamination regression
# Run the full cycling test once (6 named + 50 stress cycles) and
# check both pass/fail results AND autoload contamination regression.
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Automated cycling test (6 named + 50-cycle stress) ---"
OUTPUT=$(timeout 300 "$BINARY" --devplayer-test --headless 2>&1 || true)
check "All cycling tests passed" "\[TEST PASS\] ALL TESTS PASSED"
check_not "No test failures" "\[TEST FAIL\]"

# Count individual passes from cycling test
CYCLE_PASSES=$(echo "$OUTPUT" | grep -c "\[TEST PASS\]" || true)
echo "  (Cycling test reported $CYCLE_PASSES individual [TEST PASS] checks)"
echo ""

# ========================
# Test 7: Autoload contamination regression (same output as test 6)
# After the autoload project (cycle 5+6), the stress cycles mount
# minimal_2d and class_name_collision_test_a, neither of which has
# TestManager. If autoloads leak, the mount will try to load
# res://test_manager.gd and produce ERROR lines.
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Autoload contamination regression ---"
check_not "No phantom autoload load" "Attempt to open script.*test_manager"
check_not "No named_globals error" "named_globals.has"
check "Project-added properties cleaned on unmount" "Removed 1 project-added properties"
# Zero ERROR lines in the entire 56-cycle run
ERROR_COUNT=$(echo "$OUTPUT" | grep -c "^ERROR:" || true)
if [ "$ERROR_COUNT" -eq 0 ]; then
    pass "Zero ERROR lines in cycling test"
else
    fail "Found $ERROR_COUNT ERROR lines in cycling test"
fi
# Verify real leak measurements are all zero (not a stub)
LEAK_LINES=$(echo "$OUTPUT" | grep -c "Post-unmount leak check" || true)
NONZERO_LEAKS=$(echo "$OUTPUT" | grep "Post-unmount leak check" | grep -cv "0 total leaked" || true)
if [ "$LEAK_LINES" -gt 0 ] && [ "$NONZERO_LEAKS" -eq 0 ]; then
    pass "Real leak measurements: $LEAK_LINES cycles, all zero"
else
    fail "Leak measurement: $LEAK_LINES cycles measured, $NONZERO_LEAKS had non-zero leaks"
fi
echo ""

# ========================
# Test 8: Import session validation (import_test project)
# Verifies that ImportSessionManager scans the filesystem and detects
# importable assets that are missing .import metadata.
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Import session validation (import_test project) ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/import_test" --quit-after 5000 2>&1 || true)
check "Import cache directory detected" "Import cache directory found"
check "Importable files scanned" "importable files found"
check "Missing import metadata detected" "missing .import metadata"
check "Missing file listed" "Missing:.*icon.png"
check "Project launched despite missing imports" "PROJECT LAUNCHED"
check "GDScript executed" "Import test project loaded"
echo ""

# ========================
# Test 9: Import validation on minimal_2d (scripts-only project)
# Verifies that a project with no importable assets (only .gd and .tscn)
# reports clean import status.
# ========================
TEST_NUM=$((TEST_NUM + 1))
echo "--- Test $TEST_NUM: Import validation on scripts-only project ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/minimal_2d" --quit-after 5000 2>&1 || true)
check "Scripts-only project scan" "Scan complete"
check "SVG detected as importable" "importable files found"
echo ""

# ========================
# Summary
# ========================
echo "==========================================="
echo "  RESULTS: $PASS_COUNT passed, $FAIL_COUNT failed out of $((PASS_COUNT + FAIL_COUNT)) checks"
echo "==========================================="

if [ $FAIL_COUNT -gt 0 ]; then
    exit 1
fi
