#!/usr/bin/env bash
set -euo pipefail

# GitManager functional test
# Creates a temporary git repo with two branches, then exercises
# GitManager operations through the DevPlayer engine.

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$ROOT_DIR/bin/godot.linuxbsd.editor.x86_64"
PASS_COUNT=0
FAIL_COUNT=0

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL_COUNT=$((FAIL_COUNT + 1))
    echo "  FAIL: $1"
}

check() {
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
    exit 1
fi

# ========================
# Setup: Create a temporary git repo with two branches
# ========================
TMPDIR=$(mktemp -d)
REPO_PATH="$TMPDIR/test_repo"
echo "=== GitManager Functional Test ==="
echo "Temp repo: $REPO_PATH"
echo ""

cleanup() {
    echo ""
    echo "Cleaning up $TMPDIR ..."
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

# Initialize the repo with a "main" branch containing minimal_2d-like project.
mkdir -p "$REPO_PATH"
cd "$REPO_PATH"
git init -b main > /dev/null 2>&1
git config user.email "test@test.com"
git config user.name "Test"

# Create main branch content: a minimal project.
cat > project.godot << 'PROJEOF'
config_version=5

[application]

config/name="Git Test Main"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.4")
PROJEOF

cat > main.gd << 'GDEOF'
extends Node2D

func _ready():
	print("Git Test Main branch loaded")
	print("BRANCH_ID: main")
GDEOF

cat > main.tscn << 'TSCNEOF'
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://main.gd" id="1"]

[node name="Main" type="Node2D"]
script = ExtResource("1")
TSCNEOF

git add -A > /dev/null 2>&1
git commit -m "Initial commit on main" > /dev/null 2>&1

# Create "feature" branch with different content.
git checkout -b feature > /dev/null 2>&1

cat > main.gd << 'GDEOF'
extends Node2D

func _ready():
	print("Git Test Feature branch loaded")
	print("BRANCH_ID: feature")
GDEOF

cat > project.godot << 'PROJEOF'
config_version=5

[application]

config/name="Git Test Feature"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.4")
PROJEOF

git add -A > /dev/null 2>&1
git commit -m "Feature branch changes" > /dev/null 2>&1

# Go back to main.
git checkout main > /dev/null 2>&1

cd "$ROOT_DIR"
echo "Setup complete: repo has 'main' and 'feature' branches."
echo ""

# ========================
# Test 1: Mount main branch, verify GitManager query operations
# ========================
echo "--- Test 1: GitManager query operations on main branch ---"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$REPO_PATH" --quit-after 5000 2>&1 || true)
check "Project mounted from repo" "PROJECT LAUNCHED"
check "Main branch GDScript executed" "BRANCH_ID: main"
check "Git Test Main project name" "Git Test Main"
echo ""

# ========================
# Test 2: Direct git checkout to feature branch, then mount
# ========================
echo "--- Test 2: Checkout feature branch, then mount ---"
cd "$REPO_PATH" && git checkout feature > /dev/null 2>&1 && cd "$ROOT_DIR"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$REPO_PATH" --quit-after 5000 2>&1 || true)
check "Project mounted from feature" "PROJECT LAUNCHED"
check "Feature branch GDScript executed" "BRANCH_ID: feature"
check "Git Test Feature project name" "Git Test Feature"
check_not "No main branch contamination" "BRANCH_ID: main"
echo ""

# ========================
# Test 3: Switch back to main, verify isolation
# ========================
echo "--- Test 3: Switch back to main, verify isolation ---"
cd "$REPO_PATH" && git checkout main > /dev/null 2>&1 && cd "$ROOT_DIR"
OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$REPO_PATH" --quit-after 5000 2>&1 || true)
check "Project mounted back on main" "PROJECT LAUNCHED"
check "Main branch back" "BRANCH_ID: main"
check_not "No feature branch contamination" "BRANCH_ID: feature"
echo ""

# ========================
# Test 4: GitManager._run_git() through engine query test
# Uses the branch_switch_test project which prints its branch marker.
# We create a custom project that exercises GitManager API.
# ========================
echo "--- Test 4: GitManager API via GDScript ---"

# Create a GDScript test project that calls GitManager.
GIT_TEST_DIR="$TMPDIR/git_api_test"
mkdir -p "$GIT_TEST_DIR"

cat > "$GIT_TEST_DIR/project.godot" << 'PROJEOF'
config_version=5

[application]

config/name="Git API Test"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.4")
PROJEOF

# Write the GDScript with the repo path embedded.
cat > "$GIT_TEST_DIR/main.gd" << GDEOF
extends Node2D

func _ready():
	print("Git API test started")
	var gm = Engine.get_singleton("GitManager")
	if gm == null:
		print("GITAPI_ERROR: GitManager singleton not found")
		return

	var repo_path = "$REPO_PATH"
	print("GITAPI: Testing repo at " + repo_path)

	# Test get_current_branch
	var branch = gm.get_current_branch(repo_path)
	print("GITAPI_BRANCH: " + str(branch))

	# Test list_branches
	var branches = gm.list_branches(repo_path)
	print("GITAPI_BRANCHES: " + str(branches))

	# Test get_current_commit_info
	var info = gm.get_current_commit_info(repo_path)
	print("GITAPI_HASH: " + str(info.get("hash", "")))
	print("GITAPI_MESSAGE: " + str(info.get("message", "")))
	print("GITAPI_AUTHOR: " + str(info.get("author", "")))

	# Test checkout_branch
	var err = gm.checkout_branch(repo_path, "feature")
	print("GITAPI_CHECKOUT: " + str(err))

	# Verify we're now on feature
	var after_branch = gm.get_current_branch(repo_path)
	print("GITAPI_AFTER_CHECKOUT: " + str(after_branch))

	# Switch back to main
	gm.checkout_branch(repo_path, "main")

	print("Git API test complete")
GDEOF

cat > "$GIT_TEST_DIR/main.tscn" << 'TSCNEOF'
[gd_scene load_steps=2 format=3]

[ext_resource type="Script" path="res://main.gd" id="1"]

[node name="Main" type="Node2D"]
script = ExtResource("1")
TSCNEOF

OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$GIT_TEST_DIR" --quit-after 5000 2>&1 || true)
check "GitManager singleton found" "GITAPI: Testing repo"
check "Current branch is main" "GITAPI_BRANCH: main"
check "Branches list contains main" "GITAPI_BRANCHES.*main"
check "Branches list contains feature" "GITAPI_BRANCHES.*feature"
check "Commit hash returned" "GITAPI_HASH: [0-9a-f]"
check "Commit message returned" "GITAPI_MESSAGE: Initial commit"
check "Commit author returned" "GITAPI_AUTHOR: Test"
check "Checkout succeeded (error code 0)" "GITAPI_CHECKOUT: 0"
check "After checkout on feature" "GITAPI_AFTER_CHECKOUT: feature"
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
