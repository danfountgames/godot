# Testing GodotBeam DevPlayer

## Running the Full Test Suite

```bash
bash scripts/run_domain_tests.sh
```

This script runs 6 test categories (tests 1-6) against the compiled binary at `bin/godot.linuxbsd.editor.x86_64`. If the binary does not exist, the script exits with an error and prints the build command.

The test suite requires no external dependencies beyond the built binary and the `test_projects/` directory.

## Test Projects

All test projects live under `test_projects/`. Each is a minimal Godot project with a `project.godot` and a main scene:

| # | Project | Purpose |
|---|---------|---------|
| 1 | `minimal_2d` | Baseline mount test -- a simple 2D scene with a script that prints "Minimal 2D project loaded" |
| 2 | `class_name_collision_test_a` | Defines a global class named `Enemy` with behavior "Aggressive" -- tests class_name registration |
| 3 | `class_name_collision_test_b` | Defines a **different** global class also named `Enemy` with behavior "Passive" -- tests that class_name registrations from project A do not leak into project B |
| 4 | `autoload_reset_test` | Defines one or more autoloads in its `project.godot` -- tests autoload creation and cleanup |
| 5 | `resource_cache_test_a` | Tests resource cache isolation when switching between projects with identically-named resources |
| 6 | `resource_cache_test_b` | Companion to `resource_cache_test_a` for A/B resource cache testing |
| 7 | `branch_switch_test` | Tests the `GitManager::switch_and_remount()` workflow (unmount, checkout branch, remount) |
| 8 | `live_reload_test` | Tests the `SyncServer` live reload workflow (file sync, tiered reload hints) |
| 9 | `import_test` | Contains a PNG texture to test ImportSessionManager's import cache validation |

## Test Scripts

| Script | Checks | Description |
|--------|--------|-------------|
| `scripts/run_domain_tests.sh` | 36 | Core test suite: mount/unmount lifecycle, class_name isolation, autoload reset, stress cycling, import validation |
| `scripts/test_git_manager.sh` | 19 | GitManager GDScript API: branch listing, commit info, checkout, branch switch |
| `scripts/test_sync_server.sh` | 23 | SyncServer WebSocket protocol: all 5 message types, path traversal rejection, disk write verification |
| `scripts/regression_demo.sh` | 38 | 11-step end-to-end regression: shell launch, mount A/B, A→B→A, git branch switch, SyncServer live sync, clean shutdown |

## Test Categories

### Test 1: Basic DevPlayer Shell Boot

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless --quit-after 2000
```

Validates:
- Module initialization (`[DevPlayer] Module initialized`)
- Shell UI creation (`DevPlayer: Shell UI created`)
- Clean shutdown (`[DevPlayer] Module shut down`)

**3 checks.**

### Test 2: Auto-Mount minimal_2d

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/minimal_2d --quit-after 5000
```

Validates:
- Project settings loaded successfully
- Main scene resolved from `project.godot`
- Project domain mounted
- Scene instantiated and added to SceneTree
- Project launch completed
- GDScript output ("Minimal 2D project loaded") confirms scripts executed

**6 checks.**

### Test 3: Mount class_name_collision_test_a

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/class_name_collision_test_a --quit-after 5000
```

Validates:
- Settings loaded and scene launched
- Global class `Enemy` was registered during mount
- The Enemy class outputs "Project A Enemy - Aggressive" (correct behavior for project A)

**4 checks.**

### Test 4: Mount class_name_collision_test_b

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/class_name_collision_test_b --quit-after 5000
```

Validates:
- Settings loaded and scene launched
- Global class `Enemy` was registered (this time from project B)
- The Enemy class outputs "Project B Enemy - Passive" (correct behavior for project B)
- Output does **not** contain "Project A Enemy - Aggressive" (no stale data from project A)

**5 checks.**

This is the most critical isolation test: two projects define the same `class_name` with different behavior. If domain cleanup fails, project B would inherit project A's Enemy class.

### Test 5: Mount autoload_reset_test

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/autoload_reset_test --quit-after 5000
```

Validates:
- Settings loaded and scene launched
- Autoloads were built from the project's autoload settings

**3 checks.**

### Test 6: Automated Cycling Test (11 cycles + 50-cycle stress)

```bash
timeout 300 ./bin/godot.linuxbsd.editor.x86_64 --devplayer-test --headless
```

This is the most comprehensive test. It runs the built-in test state machine in `DevPlayerShell`, which executes mount/unmount cycles entirely within the engine process.

**6 named cycles:**

| Cycle | Project | What It Tests |
|-------|---------|---------------|
| 1 | `minimal_2d` | Basic mount/unmount lifecycle |
| 2 | `class_name_collision_test_a` | Mount a different project after cycle 1 |
| 3 | `minimal_2d` (remount) | A-B-A pattern: remount a previously-used project |
| 4 | `class_name_collision_test_b` | Mount project B with same class_name as project A |
| 5 | `autoload_reset_test` | Autoload creation + verification (autoload count > 0) |
| 6 | `autoload_reset_test` (remount) | Verify autoloads are re-created correctly on remount |

Each named cycle performs 3-4 checks:
1. `launch_project()` returns `OK`
2. After 300ms wait, `is_project_mounted()` returns `true`
3. After `stop_project()`, `is_project_mounted()` returns `false`
4. `leaked_references_after_unmount` equals 0

Cycles 5 and 6 add an additional check: `active_autoload_count > 0`.

**50-cycle stress test:**

After the 6 named cycles, the state machine runs 50 rapid mount/unmount cycles alternating between `minimal_2d` and `class_name_collision_test_a`. Each stress cycle performs 3 checks:
1. Mount succeeded
2. Project is mounted
3. Project unmounted cleanly with zero leaked references

The stress test validates that:
- No memory leaks accumulate over many mount/unmount cycles
- ResourceCache is properly evicted each time
- GDScript caches are properly flushed each time
- class_name registrations do not collide across cycles
- The engine remains stable after 50+ domain switches in a single session

The test script checks for `[TEST PASS] ALL TESTS PASSED` in the output and verifies no `[TEST FAIL]` lines are present.

**2 checks** from the shell script, plus the individual pass count is reported.

## Expected Output for Passing Tests

### Shell script summary (tests 1-5)

```
=== GodotBeam DevPlayer Comprehensive Test Suite ===

--- Test 1: Basic DevPlayer shell boot ---
  PASS: Module initialized
  PASS: Shell UI created
  PASS: Clean shutdown

--- Test 2: Auto-mount minimal_2d project ---
  PASS: Settings loaded
  PASS: Main scene resolved
  PASS: Project mounted
  PASS: Scene instantiated
  PASS: Project launched
  PASS: GDScript executed

--- Test 3: Mount class_name_collision_test_a ---
  PASS: Settings loaded
  PASS: Scene launched
  PASS: Enemy class registered
  PASS: Enemy behavior A

--- Test 4: Mount class_name_collision_test_b ---
  PASS: Settings loaded
  PASS: Scene launched
  PASS: Enemy class registered
  PASS: Enemy behavior B
  PASS: No stale A data

--- Test 5: Mount autoload_reset_test ---
  PASS: Settings loaded
  PASS: Scene launched
  PASS: Autoloads built

--- Test 6: Automated cycling test (11 cycles + 50-cycle stress) ---
  PASS: All cycling tests passed
  PASS: No test failures
  (Cycling test reported 227 individual [TEST PASS] checks)

===========================================
  RESULTS: 23 passed, 0 failed out of 23 checks
===========================================
```

### Cycling test internal output (test 6)

The 227 individual `[TEST PASS]` checks break down as:
- 6 named cycles x ~4 checks each = ~25 checks (23 base + 2 autoload checks)
- 50 stress cycles x ~3 checks each = ~150 checks
- Plus mount-call success checks for each cycle

Total: **23 shell-level checks + 227 cycling checks** for a fully passing run.

## Running Individual Tests Manually

You can mount any single test project using the `--devplayer-mount` flag:

```bash
# Mount a specific test project
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount /absolute/path/to/test_projects/minimal_2d \
    --quit-after 5000

# Mount with a specific scene override
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount /absolute/path/to/test_projects/minimal_2d \
    --devplayer-scene res://other_scene.tscn \
    --quit-after 5000

# Run the cycling test alone (no shell script)
./bin/godot.linuxbsd.editor.x86_64 --devplayer-test --headless
```

The `--quit-after <ms>` flag tells the engine to quit after the specified number of milliseconds. For mount tests, 5000ms (5 seconds) is typically sufficient to see script output and verify the mount completed.

For interactive testing (with a visible window), omit `--headless`:

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer
```

This opens the interactive DevPlayerShell UI with:
- **Project list**: Discovers projects in `test_projects/`, mount by selection or double-click
- **Custom path**: Enter any absolute path to mount a project
- **Git controls**: Enter a repo path, list branches, switch branch and remount
- **Sync controls**: Start/stop the SyncServer on a configurable port
- **Log panel**: Scrollable log of all operations
- **F12**: Toggle between shell and running project
- **Unmount/Relaunch**: Return to shell or restart the mounted project

## Adding New Test Projects

To add a new test project:

1. Create a new directory under `test_projects/`:
   ```
   test_projects/my_new_test/
   ```

2. Add a minimal `project.godot`:
   ```ini
   [gd_resource type="ProjectSettings" format=3]

   config_version=5

   [application]
   config/name="My New Test"
   run/main_scene="res://main.tscn"
   config/features=PackedStringArray("4.4")
   ```

3. Create a main scene (`main.tscn`) with a root node and an attached script that prints identifiable output:
   ```gdscript
   extends Node2D

   func _ready():
       print("My new test project loaded")
   ```

4. To include the project in the **shell script tests** (`scripts/run_domain_tests.sh`), add a new test section:
   ```bash
   TEST_NUM=$((TEST_NUM + 1))
   echo "--- Test $TEST_NUM: Mount my_new_test ---"
   OUTPUT=$("$BINARY" --devplayer --headless --devplayer-mount "$ROOT_DIR/test_projects/my_new_test" --quit-after 5000 2>&1 || true)
   check "Settings loaded" "Project settings loaded successfully"
   check "Scene launched" "PROJECT LAUNCHED"
   check "Test output" "My new test project loaded"
   echo ""
   ```

5. To include the project in the **automated cycling test** (`dev_player_shell.cpp`), add its path to the test state machine and create new test steps following the existing cycle pattern (mount, wait 300ms, verify, stop, verify unmount, check leaks).

6. If the project uses `class_name`, autoloads, or other features being tested for isolation, make sure the names overlap intentionally with existing test projects to verify domain cleanup.
