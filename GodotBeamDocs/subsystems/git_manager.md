# GitManager

## Purpose

GitManager provides git operations for branch switching within the DevPlayer. It wraps the system `git` binary to perform clone, fetch, checkout, and query operations on repositories. Its primary high-level feature is `switch_and_remount()`, which orchestrates a complete unmount-fetch-checkout-remount cycle so that a user can switch branches and have the DevPlayer automatically reload the project from the new branch.

**Source files:** `modules/devplayer/git_manager.h`, `modules/devplayer/git_manager.cpp`

## Key APIs

### Configuration

| Method | Description |
|--------|-------------|
| `set_repos_base_path(path)` | Sets the base directory where cloned repositories are stored. |
| `get_repos_base_path() -> String` | Returns the current repos base path. Defaults to `<engine_executable_dir>/repos/`. |
| `set_launch_controller(lc)` | Injects the LaunchController reference needed for `switch_and_remount()`. |

### Core Git Operations

| Method | Description |
|--------|-------------|
| `clone_repo(url, target_dir) -> Error` | Runs `git clone <url> <target_dir>`. Fails if the target directory already exists. |
| `fetch(repo_path) -> Error` | Runs `git -C <repo_path> fetch --all --prune` to update all remote branches and remove stale tracking refs. |
| `checkout_branch(repo_path, branch) -> Error` | Runs `git -C <repo_path> checkout <branch>` to switch to the specified branch. |

### Query Operations

| Method | Description |
|--------|-------------|
| `get_current_commit_info(repo_path) -> Dictionary` | Returns a Dictionary with keys `hash`, `message`, `author`, and `date` (ISO 8601) for the HEAD commit. Each field is fetched via a separate `git log -1 --format=...` call. |
| `list_branches(repo_path) -> PackedStringArray` | Runs `git branch -a --format=%(refname:short)` and returns all local and remote branch names. |
| `get_current_branch(repo_path) -> String` | Runs `git rev-parse --abbrev-ref HEAD` and returns the current branch name. |

### Orchestrated Operations

| Method | Description |
|--------|-------------|
| `switch_and_remount(repo_path, branch) -> Error` | Performs a 4-step orchestrated branch switch: (1) unmount if mounted, (2) fetch, (3) checkout, (4) remount if previously mounted. Includes rollback on failure. |

## Architecture

### Internal Git Execution

All git operations go through the private `_run_git()` helper, which:

1. Builds an argument list, prepending `-C <repo_path>` to run the command inside the repository directory. For `clone_repo()`, the repo path is empty (the repo does not exist yet).
2. Logs the full command string for debugging.
3. Calls `OS::get_singleton()->execute("git", args, &output, &exit_code, true)` to run git synchronously and capture stdout.
4. Returns `OK` if git exits with code 0, `FAILED` otherwise.

### The `switch_and_remount()` Workflow

This is a 4-step orchestrated process:

**Step 1: Unmount (conditional)** -- Checks `ProjectDomainManager` to see if a project is currently mounted from this repository (by comparing the mounted project path against the repo path). If so, saves the mounted path and target scene, then calls `LaunchController::stop_project()`.

**Step 2: Fetch** -- Calls `fetch()` to pull the latest remote changes. If fetch fails and a project was unmounted in step 1, attempts to remount the original project as a rollback.

**Step 3: Checkout** -- Calls `checkout_branch()` to switch to the target branch. On failure, attempts the same rollback as step 2.

**Step 4: Remount (conditional)** -- If a project was unmounted in step 1, calls `LaunchController::launch_project()` with the original path and scene to remount from the new branch's files.

### Default Repository Path

The constructor sets `repos_base_path` to `<engine_executable_dir>/repos/` using `OS::get_singleton()->get_executable_path().get_base_dir().path_join("repos")`.

## Integration

- **LaunchController** is called by `switch_and_remount()` for the unmount/remount cycle.
- **ProjectDomainManager** is queried to determine whether the currently mounted project lives inside the target repository.
- All methods are bound to GDScript via `ClassDB::bind_method`, making them callable from the DevPlayer shell UI or external scripts.

## Critical Notes

- All git operations are **synchronous and blocking**. Long-running operations like `clone` or `fetch` on large repositories will freeze the engine's main thread. Consider calling these from a background thread for production use.
- The `switch_and_remount()` rollback strategy attempts to remount the original branch's project if fetch or checkout fails. However, if the original files on disk have been corrupted or are in an inconsistent state, the rollback mount may also fail.
- `clone_repo()` will return `ERR_ALREADY_EXISTS` if the target directory exists, even if it is not a valid git repository. It does not verify the directory is a git repo.
- The `git` binary must be available on the system `PATH`. If git is not installed, all operations will fail with `OS::execute()` errors.
- The `get_current_commit_info()` method makes four separate `git log` invocations to retrieve hash, message, author, and date individually. This could be optimized into a single call with a compound format string.
