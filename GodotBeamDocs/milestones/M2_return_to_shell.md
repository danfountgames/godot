# Milestone 2 — Return to Shell

## Status: COMPLETE
**Started**: 2026-03-08
**Completed**: 2026-03-08

## Objective
Project can stop and return to shell. No app restart required.

## Pass Criteria
- [x] Launch and exit works 10 times in a row

## Tasks
- [x] Implement LaunchController.stop_project()
- [x] Implement scene tree teardown without process exit
- [x] Implement shell UI re-display after project stop
- [x] Test stop_project() basic functionality
- [x] Test 10x launch/exit cycle

## Critical Discovery: memdelete() vs queue_free()

**Problem**: Initial implementation used `queue_free()` for scene teardown during unmount. This caused stale script references because `queue_free()` uses deferred deletion -- the node is not actually freed until the end of the current frame. This means script references remain alive past the point where the script cache is cleared, causing dangling script objects that corrupt the next mount.

**Solution**: Use `memdelete()` for immediate, synchronous cleanup during unmount. This ensures all node references are fully released before script cache clearing proceeds.

### Why This Matters
- `queue_free()` defers deletion to the end of the frame via `MessageQueue`
- During unmount, the script cache is cleared in the same frame
- If nodes still hold script references when the cache is cleared, the scripts become orphaned
- On the next mount, these orphaned scripts interfere with new script loading
- `memdelete()` is immediate -- no deferral, no stale references

## Unmount Sequence (stop_project)
1. Stop any running scene (remove from tree)
2. `memdelete()` all project scene nodes (immediate, not deferred)
3. Re-display DevPlayerShell
4. Shell is ready for next mount command

## Dependencies
- M1 must be complete (SATISFIED)
