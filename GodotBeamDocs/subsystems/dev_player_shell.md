# DevPlayerShell

## Purpose

DevPlayerShell is the UI shell and automated test harness for the DevPlayer. It provides a basic Control-based interface for manually mounting/unmounting projects, displays live debug metrics (mount status, resource counts, autoload counts, mount duration), and implements a comprehensive automated test mode triggered by the `--devplayer-test` command-line flag.

**Source files:** `modules/devplayer/dev_player_shell.h`, `modules/devplayer/dev_player_shell.cpp`

## Key APIs

| Method | Description |
|--------|-------------|
| `set_managers(...)` | Injects references to LaunchController, ProjectDomainManager, AutoloadSessionManager, ResourceDomainManager, and DevPlayerDebug. Alternatively, the shell discovers these singletons lazily. |
| `set_automated_test_mode(enabled)` | Enables or disables the automated test state machine. When enabled, resets all test counters and begins execution on the next `NOTIFICATION_PROCESS`. |

## Architecture

### UI Layout

The shell builds its UI in `_build_ui()` during `NOTIFICATION_READY`:

- **Title bar** -- "GodotBeam DevPlayer Shell" centered label.
- **Info panel** -- Five labels showing: mount status, project path, tracked resource count, active autoload count, and last mount duration.
- **Project path input** -- A `LineEdit` pre-populated with the `minimal_2d` test project path (derived from the engine root).
- **Mount/Unmount buttons** -- Two buttons in an `HBoxContainer`. The Mount button is disabled when a project is mounted; the Unmount button is disabled when no project is mounted.

The entire UI is contained in a `VBoxContainer` (`shell_ui_root`) that can be hidden/shown. When a project is mounted via the Mount button, the shell UI is hidden to reveal the project's scene. When the project is unmounted, the shell UI is shown again.

### Debug Display

`_update_debug_display()` runs every frame during `NOTIFICATION_PROCESS` and updates all status labels from `DevPlayerDebug` and `ProjectDomainManager`. It also enables/disables the Mount and Unmount buttons based on whether a project is currently mounted.

### Engine Root Capture

The `_capture_engine_root()` method records `ProjectSettings::get_resource_path()` on first call, before any mount operation can change it. This captured path is used to construct absolute paths to test projects via `_test_project_path()`, which returns `<engine_root>/test_projects/<project_name>`.

### Automated Test Mode (`--devplayer-test`)

When `set_automated_test_mode(true)` is called, the shell runs a state machine driven by `_run_test_step()` every frame. The state machine exercises the full mount/unmount lifecycle across multiple test projects.

#### Test State Machine

The test runs through 21 steps organized into test cycles:

| Steps | Cycle | Description |
|-------|-------|-------------|
| 0-2 | Cycle 1 | Mount `minimal_2d`, verify mounted, stop, verify unmounted + no leaked refs |
| 3-5 | Cycle 2 | Mount `class_name_collision_test_a`, verify, stop, verify clean |
| 6-8 | Cycle 3 | Remount `minimal_2d` (A->B->A pattern test), verify, stop, verify clean |
| 9-11 | Cycle 4 | Mount `class_name_collision_test_b`, verify, stop, verify clean |
| 12-14 | Cycle 5 | Mount `autoload_reset_test`, verify autoloads created (count > 0), stop, verify clean |
| 15-17 | Cycle 6 | Remount `autoload_reset_test`, verify autoloads re-created on remount, stop, verify clean |
| 18-19 | Stress | 50 rapid A/B cycles alternating `minimal_2d` and `class_name_collision_test_a`. Each cycle: mount, wait 0.3s, verify mounted, stop, verify unmounted + no leaked refs. |
| 20 | Final | Print summary (pass/fail counts), exit with code 0 (all pass) or 1 (any fail). |

#### Test Cycle Pattern

Each named cycle (1-6) follows a three-step pattern:

1. **MOUNT step** -- Call `launch_project()`, check the returned `Error` code.
2. **VERIFY step** -- Wait 0.3 seconds, check `is_project_mounted()`, then call `stop_project()`.
3. **CLEANUP step** -- Wait one frame, verify `is_project_mounted()` returns false, check `DevPlayerDebug::get_leaked_references_after_unmount()` is zero.

#### Stress Test

The stress test (steps 18-19) runs `STRESS_CYCLE_COUNT` (50) rapid mount/unmount cycles, alternating between two projects. This tests for resource leaks, class_name collisions, and cache corruption under repeated switching.

### Test Helper Methods

| Method | Description |
|--------|-------------|
| `_test_check(condition, pass_msg, fail_msg)` | Logs `[TEST PASS]` or `[TEST FAIL]` and increments the appropriate counter. |
| `_test_project_path(project_name) -> String` | Returns `<engine_root>/test_projects/<project_name>`. |

## Integration

- **LaunchController** is called for `launch_project()` and `stop_project()` during both manual UI interaction and automated tests.
- **ProjectDomainManager** is queried for `is_project_mounted()` to update UI state and validate test assertions.
- **DevPlayerDebug** provides all metrics displayed in the info panel and the leaked-reference count used in test assertions.
- **AutoloadSessionManager** and **ResourceDomainManager** are referenced for potential direct access but metrics are primarily read through DevPlayerDebug.

## Critical Notes

- The test state machine is NOT re-entrant. Calling `set_automated_test_mode(true)` while tests are already running will reset all counters and restart from step 0.
- Test projects must exist at `<engine_root>/test_projects/`. The four required test projects are: `minimal_2d`, `class_name_collision_test_a`, `class_name_collision_test_b`, and `autoload_reset_test`. Missing test projects will cause mount failures that are reported as test failures.
- The shell captures the engine root path exactly once, on the first call to `_capture_engine_root()`. If this method is called after a mount has changed the resource path, the captured path will be wrong, and all test project paths will be incorrect.
- The 0.3-second wait in verify steps is a hardcoded delay to allow the SceneTree to process the mounted scene. It is not adaptive and may need adjustment on slower devices.
- After all tests complete, the engine exits via `SceneTree::quit()` with the appropriate exit code. The `automated_test_mode` flag is set to false and `test_step` is set to 21 to prevent re-entry.
- The stress test constant `STRESS_CYCLE_COUNT` is set to 50. This can be adjusted in the header for more or less thorough stress testing.
