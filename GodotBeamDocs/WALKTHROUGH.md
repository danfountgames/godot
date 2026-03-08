# DevPlayer Developer Walkthrough

A step-by-step guide for another developer to build, run, and exercise the Linux DevPlayer prototype.

## Prerequisites

- Linux x86_64 (tested on Linux Mint 22.1 / Ubuntu 24.04)
- GCC 13+ and standard Godot build dependencies
- Python 3 with `websockets` library (`pip3 install websockets`) for sync tests
- Git

## 1. Build

```bash
cd /path/to/GodotBeam
scons platform=linuxbsd target=editor module_devplayer_enabled=yes -j$(nproc)
```

Output binary: `bin/godot.linuxbsd.editor.x86_64` (~152 MB).

Clean build takes ~10 minutes. Incremental builds take ~13 seconds.

## 2. Launch the DevPlayer shell

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer
```

This opens a window with the interactive DevPlayer shell. You will see:

- **Status section** at top: "No project mounted"
- **Projects section**: list of discovered test projects (9 projects from `test_projects/`)
- **Git section**: repo path and branch controls
- **Sync section**: SyncServer start/stop controls
- **Log section**: scrollable log at the bottom

For headless mode (no window), add `--headless`:

```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless --quit-after 5000
```

## 3. Mount minimal_2d

**Interactive:** Select "minimal_2d" in the project list and click "Mount Selected", or double-click it.

**Headless:**
```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/minimal_2d --quit-after 5000
```

What happens:
1. `ProjectSettingsLayerManager` captures current settings, loads `test_projects/minimal_2d/project.godot`
2. `res://` is remapped to `test_projects/minimal_2d/`
3. `ScriptDomainManager` scans for `class_name` declarations and registers global classes
4. `ResourceDomainManager` begins tracking resources
5. `ImportSessionManager` scans for importable assets and validates `.import` metadata
6. `AutoloadSessionManager` builds autoloads from project settings (none for minimal_2d)
7. Main scene (`res://main.tscn`) is loaded fresh from disk and added to the SceneTree
8. Project GDScript executes: prints "Minimal 2D project loaded"

The shell hides itself and shows a "Back to Shell (F12)" overlay button.

## 4. Return to shell

**Interactive:** Press F12, or click the "Back to Shell (F12)" overlay button.

The shell becomes visible again. The project is still mounted (status shows "MOUNTED"). You can:
- Click "Unmount" to tear down the project and return to a clean state
- Click "Relaunch" to restart the project's main scene
- Press F12 again to hide the shell and see the project

**Headless unmount:** Use `--devplayer-test` which exercises the automated mount/unmount cycle:
```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer-test --headless --quit-after 30000
```

## 5. Mount autoload_reset_test

**Interactive:** Click "Unmount" first (if minimal_2d is still mounted), then select "autoload_reset_test" and click "Mount Selected".

**Headless:**
```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/autoload_reset_test --quit-after 5000
```

This project has a `TestManager` autoload defined in its `project.godot`. You will see:
- `AutoloadSessionManager` loads `res://test_manager.gd`, instantiates it, adds to SceneTree root
- The main scene accesses `TestManager` and prints its instance info

## 6. Switch to a git repo project

First, create a test git repo (or use any Godot project in a git repo):

```bash
# Create a temp repo
REPO=$(mktemp -d)/test_repo
mkdir -p "$REPO"
cd "$REPO"
git init -b main
git config user.email "test@test.com"
git config user.name "Test"

# Main branch
cat > project.godot << 'EOF'
config_version=5
[application]
config/name="Git Test"
run/main_scene="res://main.tscn"
config/features=PackedStringArray("4.4")
EOF

cat > main.gd << 'EOF'
extends Node2D
func _ready():
    print("Running on: main branch")
EOF

cat > main.tscn << 'EOF'
[gd_scene load_steps=2 format=3]
[ext_resource type="Script" path="res://main.gd" id="1"]
[node name="Main" type="Node2D"]
script = ExtResource("1")
EOF

git add -A && git commit -m "Main branch"

# Feature branch with different content
git checkout -b feature
echo 'extends Node2D
func _ready():
    print("Running on: feature branch")' > main.gd
git add -A && git commit -m "Feature branch"
git checkout main
```

**Interactive:**
1. In the DevPlayer shell, paste the repo path into the "Path:" field and click "Mount"
2. You'll see "Running on: main branch" in output
3. Press F12 to return to shell
4. Click "Unmount"
5. In the Git section, enter the repo path in "Repo:" and click "List Branches" — you'll see `main, feature`
6. Enter `feature` in "Switch to:" and click "Switch & Remount"
7. You'll see "Running on: feature branch" — the content changed because the branch changed

**Headless:**
```bash
# Mount main branch
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount "$REPO" --quit-after 3000

# Switch to feature branch externally, then remount
cd "$REPO" && git checkout feature && cd -
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount "$REPO" --quit-after 3000
```

## 7. Run SyncServer and push a file

**Interactive:**
1. Mount any project
2. In the Sync section, leave port as 6850 (or change it) and click "Start"
3. The status will show "Server: Running on :6850 Clients: 0"

Then from another terminal, push a file via WebSocket:

```python
# save as sync_push.py
import asyncio, json, base64, websockets

async def push_file():
    async with websockets.connect("ws://127.0.0.1:6850") as ws:
        # Handshake
        await ws.send(json.dumps({"type": "hello"}))
        resp = json.loads(await ws.recv())
        print(f"Server: {resp['engine']}, project mounted: {resp['project_mounted']}")

        # Push a file
        content = b"Hello from sync!"
        await ws.send(json.dumps({
            "type": "write_small_file",
            "path": "synced.txt",
            "content": base64.b64encode(content).decode()
        }))
        resp = json.loads(await ws.recv())
        print(f"Write: {resp['status']}, {resp['bytes']} bytes")

        # Send reload hint
        await ws.send(json.dumps({
            "type": "reload_hint",
            "tier": 1,
            "changed_paths": ["synced.txt"]
        }))
        resp = json.loads(await ws.recv())
        print(f"Reload: tier {resp['tier']}")

asyncio.run(push_file())
```

```bash
python3 sync_push.py
```

The file `synced.txt` will appear in the mounted project's directory.

**Headless:** The regression demo (Step 10) exercises this automatically.

## 8. Observe reload

When the SyncServer receives a `reload_hint` with a tier, it logs the event. The three reload tiers are:

| Tier | Meaning | Engine response |
|------|---------|----------------|
| 1 | Content changed (textures, audio) | Log + ack. Full resource reload not yet implemented. |
| 2 | Script changed (.gd files) | Log + ack. Full script hot-reload not yet implemented. |
| 3 | Scene structure changed (.tscn) | Log + ack. Full scene reload not yet implemented. |

Currently, reload hints are acknowledged but the actual reload behavior (re-importing resources, re-parsing scripts, re-instantiating scenes) is not yet implemented. The protocol and tier model are proven; the runtime reload actions are future work.

## 9. Clean shutdown

**Interactive:** Click "Unmount" (if a project is mounted), then close the window.

**Headless:** Use `--quit-after <ms>`:
```bash
./bin/godot.linuxbsd.editor.x86_64 --devplayer --headless \
    --devplayer-mount test_projects/minimal_2d --quit-after 3000
```

The engine will:
1. Run the mounted project for the specified duration
2. Exit cleanly
3. The DevPlayer module shuts down: all 10 singletons are destroyed, Engine singletons are unregistered

## Running the full test suite

```bash
# Core domain tests (36 checks)
bash scripts/run_domain_tests.sh

# GitManager functional tests (19 checks)
bash scripts/test_git_manager.sh

# SyncServer end-to-end tests (23 checks)
bash scripts/test_sync_server.sh

# 11-step regression demo (38 checks)
bash scripts/regression_demo.sh
```

All scripts exit 0 on success, non-zero on failure.
