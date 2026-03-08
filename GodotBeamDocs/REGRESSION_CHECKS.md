# Regression Demo — 38 Checks Explained

**Script:** `scripts/regression_demo.sh`
**Command:** `bash scripts/regression_demo.sh`
**Raw output:** `GodotBeamDocs/test_results/regression_demo_38checks.log`

Each check is a `grep` assertion against captured engine stdout. The check passes if the grep pattern is found (or not found, for `check_not`). Failing checks print what was expected.

## Step 1: Launch DevPlayer shell (4 checks)

Engine command: `--devplayer --headless --quit-after 2000` (no project mounted)

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 1 | Shell UI created | `Shell UI created` | The `_build_ui()` method ran and completed |
| 2 | Shell reports ready | `DevPlayer shell ready` | The shell logged its ready message after UI construction |
| 3 | Project discovery ran | `Found.*projects` | `_scan_projects()` found test projects in `test_projects/` |
| 4 | No project auto-launched | NOT `PROJECT LAUNCHED` | Without `--devplayer-mount`, no project was mounted |

## Step 2: Mount project A — minimal_2d (3 checks)

Engine command: `--devplayer --headless --devplayer-mount .../test_projects/minimal_2d --quit-after 3000`

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 5 | Project A launched | `PROJECT LAUNCHED` | `LaunchController::launch_project()` completed the full 10-step mount sequence |
| 6 | Project A GDScript executed | `Minimal 2D project loaded` | The mounted project's `main.gd` `_ready()` ran and printed its marker |
| 7 | Shell UI still created alongside project | `Shell UI created` | The shell UI was constructed even when a project is auto-mounted |

## Step 3: Verify project A runtime (2 checks)

Uses same output as Step 2.

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 8 | Project path contains minimal_2d | `minimal_2d` | The mounted project path is correct |
| 9 | ResourceDomainManager tracking active | `resources for project` | `ResourceDomainManager::begin_tracking()` was called during mount |

## Step 4: Mount then unmount project A (3 checks)

Engine command: `--devplayer-test --headless --quit-after 30000` (runs automated test state machine)

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 10 | Mount call succeeded | `Mount call succeeded` | `launch_project()` returned `OK` for minimal_2d |
| 11 | Unmount succeeded | `Project unmounted successfully (cycle 1)` | `stop_project()` completed; `is_project_mounted()` returns false |
| 12 | Zero leaked references | `No leaked references after unmount (cycle 1)` | `DevPlayerDebug::get_leaked_references_after_unmount()` returned 0 — measured post-unmount counters for res:// resources, autoloads, and ProjectSettings autoload entries all reached zero |

## Step 5: Mount project B — autoload_reset_test (3 checks)

Engine command: `--devplayer --headless --devplayer-mount .../test_projects/autoload_reset_test --quit-after 3000`

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 13 | Project B launched | `PROJECT LAUNCHED` | Full mount sequence completed for autoload_reset_test |
| 14 | Autoload session built | `Built.*autoloads` | `AutoloadSessionManager::build_autoloads_from_project()` created autoload nodes |
| 15 | Project B GDScript executed | `Autoload test scene ready` | The project's main scene script ran and accessed its autoload |

## Step 6: Verify project B autoload behavior (2 checks)

Uses same output as Step 5.

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 16 | TestManager autoload referenced | `TestManager` | The TestManager autoload was loaded, instantiated, and accessible from GDScript |
| 17 | Project path is autoload_reset_test | `autoload_reset_test` | Correct project path was mounted |

## Step 7: A→B→A cycle isolation (5 checks)

Engine command: `--devplayer-test --headless --quit-after 30000` (automated test cycles 2-3)

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 18 | Cycle 2 unmount clean | `Project unmounted successfully (cycle 2)` | Second project unmounted cleanly |
| 19 | Cycle 2 zero leaks | `No leaked references after unmount (cycle 2)` | Measured counters at zero after second unmount |
| 20 | A→B→A remount succeeded | `Remount call succeeded` | `launch_project()` returned OK when remounting a previously-used project |
| 21 | A remounted cleanly | `Project is mounted after remount (cycle 3)` | `is_project_mounted()` returns true for the remounted project |
| 22 | Cycle 3 zero leaks | `No leaked references after unmount (cycle 3)` | Measured counters at zero after third unmount — the A→B→A pattern left no residual state |

## Step 8: Git branch switch (4 checks)

Creates a temporary git repo with `main` and `feature` branches. Each branch has different GDScript output. Mounts main, verifies output. Switches to feature, remounts, verifies different output.

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 23 | Main branch GDScript executed | `BRANCH_ID: main` | The main branch's script printed its marker |
| 24 | Project name correct | `Git Branch Test` | Project name from project.godot was loaded |
| 25 | Feature branch GDScript executed after switch | `BRANCH_ID: feature` | After `git checkout feature`, remounting the same path loads the new branch's script |
| 26 | No main branch contamination | NOT `BRANCH_ID: main` | The feature branch run does not contain main branch output — scripts were properly flushed and reloaded |

## Step 9: Switch back to main branch (2 checks)

Switches back to `main` branch, remounts same path.

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 27 | Main branch restored | `BRANCH_ID: main` | After switching back, the main branch script runs correctly |
| 28 | No feature branch contamination | NOT `BRANCH_ID: feature` | No residual feature branch output — round-trip branch switching is clean |

## Step 10: SyncServer live sync (7 checks)

Creates a temporary project that starts SyncServer on port 16851. A Python websockets client connects and exercises the protocol.

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 29 | SyncServer started | `SYNC_READY` | `SyncServer::start_server()` succeeded and the GDScript confirmed the server is listening |
| 30 | WebSocket hello handshake | `HELLO: ok` | Python client connected, sent `{"type":"hello"}`, received `{"type":"hello_ack","engine":"GodotBeam"}` |
| 31 | File written via sync | `WRITE: ok` | Python client sent `write_small_file` with base64 content, received `{"type":"write_ack","status":"ok"}` |
| 32 | Correct byte count (25) | `BYTES: 25` | The write_ack reported exactly 25 bytes written (length of "Live sync update content!") |
| 33 | Reload tier 1 acknowledged | `RELOAD_TIER: 1` | Python client sent `reload_hint` with tier 1, received `{"type":"reload_ack","tier":1}` |
| 34 | Sync demo completed | `SYNC_DEMO_DONE: yes` | The full protocol exercise completed without errors |
| 35 | Synced file content verified on disk | (file read assertion) | The file `synced_file.txt` exists in the project directory and contains exactly "Live sync update content!" |

## Step 11: Clean shutdown (3 checks)

Engine command: `--devplayer --headless --devplayer-mount .../test_projects/minimal_2d --quit-after 3000`

| # | Check | Grep pattern | What it asserts |
|---|-------|-------------|-----------------|
| 36 | Final project launched | `PROJECT LAUNCHED` | Mount still works after all previous steps |
| 37 | No ERROR lines in final run | NOT `ERROR` | No error messages appeared in engine output |
| 38 | No LEAK messages in final run | NOT `LEAK` | No leak-related messages appeared in engine output |
