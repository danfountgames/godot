# Milestone 5 — Branch Switching

## Status: COMPLETE
**Started**: 2026-03-08
**Completed**: 2026-03-08

## Objective
Git fetch + checkout, remount correct branch content.

## Pass Criteria
- [x] branch_switch_test passes 20 toggles

## Tasks
- [x] Implement GitManager subsystem (clone, fetch, checkout, switch_and_remount)
- [x] Implement branch switch flow (unmount -> checkout -> remount)
- [x] Create branch_switch_test project (two branches, different content)
- [x] Test 20+ branch toggles A/B

## GitManager Subsystem

The GitManager subsystem provides git operations integrated with the mount/unmount lifecycle:

| Method | Purpose |
|--------|---------|
| `clone(url, target_dir)` | Clone a repository to local storage |
| `fetch(project_dir)` | Fetch latest changes from remote |
| `checkout(project_dir, ref)` | Checkout a specific branch/tag/commit |
| `switch_and_remount(project_dir, ref)` | Unmount current project, checkout new ref, remount |

### switch_and_remount Flow
1. `LaunchController::stop_project()` -- stop running scene
2. `ProjectDomainManager::unmount_project()` -- full domain unmount (M2/M3 sequence)
3. `GitManager::checkout(project_dir, new_ref)` -- switch to target branch
4. `ProjectDomainManager::mount_project(project_dir)` -- mount from new branch content
5. `LaunchController::launch_project()` -- launch the project

This leverages the full unmount/remount pipeline proven in M3/M4, ensuring no stale state survives branch switches.

## Test Projects Used
- `branch_switch_test` -- Repository with two branches containing different scene content

## Dependencies
- M4 must be complete (SATISFIED)
