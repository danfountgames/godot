Below is a full implementation spec for an AI coding agent to build the system we’ve now converged on.

This is intentionally written as an execution document, not just a brainstorm. It assumes the agent may need to make code changes, run experiments, produce logs, and update the spec as truths are discovered.

⸻

GODOT DEV PLAYER FOR IOS

AI AGENT IMPLEMENTATION SPEC

Version 1.0

Status: authoritative working spec

Audience: autonomous coding agent / technical implementer

Target: significant Godot fork + iOS shell app + sync tooling

⸻

1. PROJECT SUMMARY

Build a custom fork of the Godot engine for iOS that:
	•	runs with editor/runtime tooling enabled
	•	strips or bypasses the normal editor UI
	•	replaces that UI with a custom shell focused on:
	•	Git repo browsing
	•	branch selection
	•	live sync host discovery
	•	launch controls
	•	logs / reload prompts / status
	•	mounts a Godot project dynamically from device storage
	•	imports assets on iOS
	•	merges mounted project settings at runtime
	•	tears down and rebuilds:
	•	autoloads
	•	script domain
	•	caches
	•	project runtime state
	•	launches a chosen target scene
	•	returns to shell without stale state
	•	can then mount a different project or branch

This is not a thin embedding exercise.
This is a substantial Godot fork with a new concept of project domain mount / unmount.

⸻

2. CORE PRODUCT TRUTH

The product is:

A custom iOS Godot editor-core runtime with the editor UI replaced by a developer shell, capable of mounting a project from storage, importing assets on-device, launching it, then unmounting it cleanly and mounting another project without stale state.

This project explicitly assumes:
	•	the exact supported engine/editor version matters
	•	iOS imports may differ from desktop imports
	•	on-device import is required for correctness
	•	GDScript unload/reload correctness is a core requirement
	•	autoloads and project settings must be rebuilt per mounted project
	•	project switching is a first-class runtime concept that upstream Godot does not currently model robustly

⸻

3. NON-GOALS

Do not optimize for these in v1:
	•	generic support for arbitrary Godot versions
	•	generic support for any third-party editor plugins
	•	App Store-ready productization
	•	fluid runtime switching across many bundled engines
	•	perfect hot reload of all runtime changes without any relaunch or session rebuild
	•	on-device compilation of GDExtension
	•	web-based cloud relay routing
	•	multiplayer tooling
	•	Android support
	•	polished production UX

This is an internal/developer-focused tool first.

⸻

4. SUCCESS CRITERIA

The project is successful when all of the following are true:
	1.	The forked engine compiles for iOS with required editor/runtime code present.
	2.	A shell app launches on iOS and shows a custom UI instead of the standard Godot editor UI.
	3.	The shell can mount a project folder containing project.godot.
	4.	The mounted project’s settings are loaded and applied.
	5.	The mounted project’s assets import successfully on iOS.
	6.	The mounted project’s autoloads are instantiated correctly.
	7.	The mounted project’s GDScript domain works correctly.
	8.	A chosen target scene launches and runs.
	9.	The user can exit back to the shell.
	10.	The project domain can be unmounted.
	11.	A second project or branch can then be mounted and run without stale script/class/autoload/cache leakage from the first.
	12.	Live file sync can update the mounted project and trigger the correct reload tier.
	13.	Git repo branch switching can replace the mounted project state and relaunch it.

⸻

5. PRIMARY TECHNICAL RISKS

The AI agent must explicitly investigate and document these first:

Risk A — iOS tools=yes viability

Can the required editor/runtime code build and run on iOS at all?

Risk B — GDScript domain unload

Can script caches, class_name, preloads, globals, and script-bound resources be fully torn down and rebuilt per project domain?

Risk C — autoload teardown/rebuild

Can project autoloads be destroyed and recreated cleanly across project switches?

Risk D — ProjectSettings rebinding

Can project settings be reloaded and layered safely when the mounted project changes?

Risk E — resource cache invalidation

Can resources from one mounted project be released so another can mount without stale state?

These must be treated as first-order engineering tasks, not implementation details.

⸻

6. HIGH-LEVEL ARCHITECTURE

The whole system has three major parts:

A. Godot Fork

A modified Godot engine with:
	•	required editor/runtime/tooling code
	•	editor UI removed or bypassed
	•	project-domain mount/unmount machinery added
	•	script/autoload/settings/cache reset support added
	•	runtime shell entrypoint added

B. iOS Dev Player Shell

An iOS app that:
	•	hosts the forked engine
	•	provides Git/live-sync/shell UI
	•	manages mounted project folders
	•	calls engine mount/unmount/launch APIs
	•	shows launch metadata and logs

C. Development Sync Tooling

Desktop-side utilities for:
	•	live sync
	•	repo indexing helpers
	•	optional future editor integration
	•	test fixtures and replay scripts

⸻

7. RECOMMENDED REPOSITORY STRUCTURE

Use a monorepo.

godot-devplayer/
├── engine/
│   ├── upstream-godot-fork/
│   ├── patches/
│   ├── devplayer_docs/
│   └── test_harness/
│
├── mobile-app/
│   ├── ios-shell/
│   ├── bridge/
│   └── resources/
│
├── dev-sync-agent/
│   ├── src/
│   ├── protocol/
│   └── tests/
│
├── test-projects/
│   ├── minimal_2d/
│   ├── autoload_reset_test/
│   ├── class_name_collision_test/
│   ├── resource_cache_test/
│   ├── import_ios_variants_test/
│   ├── branch_switch_test/
│   └── live_reload_test/
│
├── scripts/
│   ├── build_engine_ios.sh
│   ├── package_ios_runtime.sh
│   ├── run_domain_tests.sh
│   └── collect_logs.sh
│
└── docs/
    ├── master_spec.md
    ├── project_domain_reset.md
    ├── milestones.md
    └── known_failures.md


⸻

8. CORE ENGINE CONCEPT: PROJECT DOMAIN

The AI agent must add a first-class concept called Project Domain.

A Project Domain includes at minimum:
	•	project root path
	•	loaded project.godot
	•	active merged project settings
	•	active autoload list
	•	active script domain
	•	active resource cache entries for this project
	•	active editor filesystem/import context
	•	active main scene / target scene
	•	active launch metadata

Upstream Godot does not robustly model this as a swappable unit.
The fork must.

⸻

9. REQUIRED NEW ENGINE SUBSYSTEMS

The agent should introduce or emulate the following systems.

9.1 ProjectDomainManager

Responsibilities:
	•	mount project root
	•	validate project.godot
	•	trigger settings load / merge
	•	trigger import scan
	•	trigger script domain creation
	•	trigger autoload build
	•	launch target scene
	•	coordinate unmount sequence

Suggested API:

class ProjectDomainManager {
public:
    Error mount_project(const String &project_path, const String &target_scene = "");
    Error relaunch_target_scene();
    Error unmount_project();
    bool is_project_mounted() const;
    String get_mounted_project_path() const;
};

9.2 ScriptDomainManager

Responsibilities:
	•	own GDScript-related caches and registrations for mounted project
	•	clear class_name registrations associated with mounted project
	•	invalidate compiled scripts / cache entries
	•	tear down project-bound script references
	•	rebuild script domain on mount

Suggested API:

class ScriptDomainManager {
public:
    Error initialize_for_project(const String &project_path);
    Error shutdown_project_scripts();
    Error clear_script_caches();
    Error clear_global_class_registrations();
};

9.3 AutoloadSessionManager

Responsibilities:
	•	read autoload definitions from mounted project settings
	•	instantiate autoload nodes
	•	attach them to runtime tree as needed
	•	destroy them during unmount
	•	verify no leaked references remain

Suggested API:

class AutoloadSessionManager {
public:
    Error build_autoloads_from_project();
    Error destroy_autoloads();
    PackedStringArray get_active_autoload_names() const;
};

9.4 ProjectSettingsLayerManager

Responsibilities:
	•	define layered settings model
	•	separate base runtime settings from mounted project settings
	•	apply session overrides
	•	rebuild merged effective settings on mount
	•	revert to shell-safe settings on unmount

Layer model:
	1.	Runtime Base Settings
	2.	Platform / Shell Settings
	3.	Mounted Project Settings
	4.	Session Override Settings

Suggested API:

class ProjectSettingsLayerManager {
public:
    Error load_base_settings();
    Error load_project_settings(const String &project_path);
    Error apply_session_overrides(const Dictionary &overrides);
    Error commit_effective_settings();
    Error clear_project_settings();
};

9.5 ResourceDomainManager

Responsibilities:
	•	track resources loaded from mounted project
	•	identify cache entries tied to mounted project root
	•	force release / invalidate on unmount
	•	help verify that stale resources do not survive project switch

Suggested API:

class ResourceDomainManager {
public:
    void begin_tracking(const String &project_root);
    void register_loaded_resource(const Ref<Resource> &resource);
    Error purge_project_resources();
    int get_tracked_resource_count() const;
};

9.6 ImportSessionManager

Responsibilities:
	•	bind editor filesystem/importer state to mounted project
	•	scan project root
	•	perform import/reimport on iOS
	•	clear previous project’s import scan state

Suggested API:

class ImportSessionManager {
public:
    Error bind_project_root(const String &project_path);
    Error scan_filesystem();
    Error import_pending_assets();
    Error clear_import_state();
};

9.7 LaunchController

Responsibilities:
	•	orchestrate project mount sequence
	•	optionally accept target scene override
	•	show metadata overlay info to shell
	•	manage exit back to shell

Suggested API:

class LaunchController {
public:
    Error launch_project(const String &project_path, const String &target_scene = "");
    Error stop_project();
    Error relaunch();
};


⸻

10. ENGINE ENTRYPOINT STRATEGY

The agent must not rely on whole-engine process restart semantics.

Instead:
	•	the engine process/app lifetime persists
	•	shell runtime persists
	•	project domains mount/unmount inside it

The shell is the “stable world.”
Projects are mounted sessions.

The runtime sequence becomes:
	1.	shell boots
	2.	no project mounted
	3.	user selects repo/branch/live project
	4.	project domain mounts
	5.	imports run on iOS
	6.	target scene launches
	7.	user exits project
	8.	project domain unmounts
	9.	shell remains alive
	10.	next project mounts

⸻

11. SOURCE AREAS TO MODIFY

The AI agent must inspect and likely modify at least these engine areas.

Main startup / runtime orchestration

main/

Project settings loading

core/config/project_settings.*

resource loading / cache

core/io/resource_loader.*
core/io/resource.*

scene launch and main loop

scene/main/

GDScript runtime

modules/gdscript/

editor import pipeline

editor/import/
editor/editor_file_system.*

autoload setup paths

Search for autoload-related project setup and main scene boot code.

global class / script registration paths

Search for class_name, global script class cache, script server, and associated maps.

The agent must create a source-map document as it discovers exact files and symbols.

⸻

12. REQUIRED INVESTIGATION OUTPUTS

Before broad implementation, the AI agent must produce these docs:

12.1 Source Map

A list of:
	•	exact files
	•	exact classes
	•	exact methods
	•	what each contributes to project load, script caching, autoloads, settings, imports

12.2 Domain Reset Failure Matrix

A table of:
	•	subsystem
	•	expected reset behavior
	•	actual observed behavior
	•	current failure mode
	•	fix strategy

12.3 Project Mount Sequence

A step-by-step order of operations for mount.

12.4 Project Unmount Sequence

A step-by-step order of operations for unmount.

These docs are mandatory.

⸻

13. REQUIRED TEST PROJECTS

The AI agent must build and use dedicated Godot test projects.

13.1 minimal_2d

Purpose:
	•	basic mount and target scene launch
	•	validates project settings load and shell return

Contains:
	•	one simple scene
	•	one script
	•	one texture
	•	touch input
	•	simple label showing loaded project identity

13.2 autoload_reset_test

Purpose:
	•	prove autoloads are destroyed and rebuilt

Contains:
	•	autoload singleton with static counters
	•	runtime UI that shows current autoload instance id
	•	writes logs when created/destroyed

Expected:
	•	instance id changes between project mounts
	•	no stale state carries over

13.3 class_name_collision_test

Purpose:
	•	prove script/global class cache reset

Contains:
	•	Project A defines class_name Enemy with one behavior
	•	Project B defines class_name Enemy with different behavior

Expected:
	•	switching from A to B must use B’s class entirely
	•	no collisions, stale registrations, or wrong script resolution

13.4 preload_reference_test

Purpose:
	•	prove preload/script references from old project do not leak

Contains:
	•	preloaded resources and scripts referenced from singleton and normal nodes

Expected:
	•	after unmount, old preload refs do not survive

13.5 resource_cache_test

Purpose:
	•	validate project resource cache invalidation

Contains:
	•	same resource path names but different content in two projects
	•	verifies correct resource content after switch

13.6 import_ios_variants_test

Purpose:
	•	validate that iOS imports are performed correctly on-device

Contains:
	•	textures/audio/models imported with settings likely to vary by platform
	•	runtime scene that displays what imported result was used

13.7 branch_switch_test

Purpose:
	•	validate Git branch switching and project remount

Contains:
	•	same repo, two branches, visibly different scene and autoload behavior

Expected:
	•	branch switch tears down old domain and launches new one cleanly

13.8 live_reload_test

Purpose:
	•	validate live sync file updates and reload tiers

Contains:
	•	scripts, textures, scenes
	•	version text displayed in scene

Expected:
	•	agent can change files from desktop and observe correct reload behavior on device

⸻

14. RELOAD TIERS

The AI agent must implement explicit reload tiers.

Tier 1 — lightweight content reload

Examples:
	•	text/config/data only
	•	maybe textures if safe

Expected action:
	•	reimport or refresh assets
	•	optional scene reload
	•	no full project unmount

Tier 2 — project session relaunch

Examples:
	•	scene changes
	•	GDScript changes
	•	autoload-relevant settings changes

Expected action:
	•	unmount project domain
	•	remount same project
	•	relaunch target scene

Tier 3 — runtime-unsafe change

Examples:
	•	GDExtension binary changes
	•	engine-sensitive ABI changes
	•	known unrecoverable script domain corruption

Expected action:
	•	return to shell with explicit “full app relaunch required” state
	•	optionally prompt user

The AI agent must classify file changes into these tiers.

⸻

15. EXAMPLE LIVE SYNC RELOAD RULES

Use these as default policy.

*.json         -> Tier 1
*.cfg          -> Tier 1
*.translation  -> Tier 1
*.png          -> Tier 1 or Tier 2 depending on importer behavior
*.jpg          -> Tier 1 or Tier 2
*.wav          -> Tier 1 or Tier 2
*.ogg          -> Tier 1 or Tier 2
*.tscn         -> Tier 2
*.tres         -> Tier 2
*.gd           -> Tier 2
project.godot  -> Tier 2
override.cfg   -> Tier 2
*.dylib        -> Tier 3

The agent may revise these after experiments, but must document why.

⸻

16. REQUIRED MOUNT SEQUENCE

The AI agent must implement a deterministic mount sequence like this:
	1.	Validate project root contains project.godot
	2.	Ensure no project currently mounted, or unmount existing project first
	3.	Bind project root path
	4.	Load project settings into layered settings model
	5.	Apply session overrides if any
	6.	Commit effective settings
	7.	Bind editor filesystem/import session to mounted project
	8.	Scan filesystem
	9.	Import/reimport pending assets on iOS
	10.	Initialize script domain for mounted project
	11.	Instantiate autoloads
	12.	Determine target scene
	13.	Launch target scene
	14.	Notify shell that runtime is active
	15.	Display launch metadata overlay

The exact implementation may differ, but this order is the default reference.

⸻

17. REQUIRED UNMOUNT SEQUENCE

The AI agent must implement a deterministic unmount sequence like this:
	1.	Stop target scene / gameplay session
	2.	Detach user-facing runtime tree from shell
	3.	Destroy autoload nodes in controlled order
	4.	Clear project-held singleton references
	5.	Shut down project script domain
	6.	Clear global script class registrations tied to project
	7.	Purge script caches / compiled data tied to project
	8.	Release project-bound resources
	9.	Purge resource cache entries tied to project root
	10.	Clear import/editor filesystem state for mounted project
	11.	Clear mounted project settings layer
	12.	Recommit shell/base settings
	13.	Mark no project mounted
	14.	Return control to shell UI

The agent must instrument this sequence with logs and counters.

⸻

18. REQUIRED LOGGING AND DEBUG INSTRUMENTATION

The agent must add a debug mode exposing at least:
	•	current mounted project path
	•	current target scene
	•	active autoload count
	•	tracked resource count
	•	tracked script cache count
	•	active script class registration count
	•	last import duration
	•	last mount duration
	•	last unmount duration
	•	reload tier triggered by latest live sync
	•	known leaked references after unmount

These metrics should be viewable in logs and preferably in a shell debug panel.

⸻

19. EXAMPLE ENGINE-SIDE PSEUDOCODE

Project mount orchestration

Error LaunchController::launch_project(const String &project_path, const String &target_scene) {
    ERR_FAIL_COND_V(project_path.is_empty(), ERR_INVALID_PARAMETER);

    if (project_domain_manager->is_project_mounted()) {
        Error err = project_domain_manager->unmount_project();
        ERR_FAIL_COND_V(err != OK, err);
    }

    Error err = settings_layer_manager->load_project_settings(project_path);
    ERR_FAIL_COND_V(err != OK, err);

    err = settings_layer_manager->commit_effective_settings();
    ERR_FAIL_COND_V(err != OK, err);

    err = import_session_manager->bind_project_root(project_path);
    ERR_FAIL_COND_V(err != OK, err);

    err = import_session_manager->scan_filesystem();
    ERR_FAIL_COND_V(err != OK, err);

    err = import_session_manager->import_pending_assets();
    ERR_FAIL_COND_V(err != OK, err);

    err = script_domain_manager->initialize_for_project(project_path);
    ERR_FAIL_COND_V(err != OK, err);

    err = autoload_session_manager->build_autoloads_from_project();
    ERR_FAIL_COND_V(err != OK, err);

    err = project_domain_manager->mount_project(project_path, target_scene);
    ERR_FAIL_COND_V(err != OK, err);

    return OK;
}

Project unmount orchestration

Error ProjectDomainManager::unmount_project() {
    if (!is_project_mounted()) {
        return OK;
    }

    Error err = autoload_session_manager->destroy_autoloads();
    ERR_FAIL_COND_V(err != OK, err);

    err = script_domain_manager->shutdown_project_scripts();
    ERR_FAIL_COND_V(err != OK, err);

    err = script_domain_manager->clear_global_class_registrations();
    ERR_FAIL_COND_V(err != OK, err);

    err = script_domain_manager->clear_script_caches();
    ERR_FAIL_COND_V(err != OK, err);

    err = resource_domain_manager->purge_project_resources();
    ERR_FAIL_COND_V(err != OK, err);

    err = import_session_manager->clear_import_state();
    ERR_FAIL_COND_V(err != OK, err);

    err = settings_layer_manager->clear_project_settings();
    ERR_FAIL_COND_V(err != OK, err);

    mounted = false;
    mounted_project_path = "";

    return OK;
}

These are not guaranteed to compile as-is. They are structural targets.

⸻

20. IOS SHELL APP REQUIREMENTS

The iOS shell must provide these screens.

20.1 Projects Screen

Shows:
	•	local repos / mounted-ready project folders
	•	current branch
	•	engine identity
	•	last synced commit
	•	launch button
	•	branch selection button
	•	update status

20.2 Live Screen

Shows:
	•	discovered live hosts
	•	available live projects
	•	connection status
	•	reload prompts
	•	current mounted live project

20.3 Runtime Overlay

Displayed for ~5 seconds on launch:
	•	project name
	•	repo name
	•	branch
	•	commit hash
	•	commit message
	•	target scene
	•	engine identity

20.4 Debug Screen

Shows:
	•	import timings
	•	cache stats
	•	active autoloads
	•	mounted project path
	•	latest reload tier
	•	latest errors

⸻

21. GIT MODE REQUIREMENTS

The AI agent must implement Git mode with at least:
	•	clone repo
	•	fetch
	•	checkout branch
	•	report latest commit info
	•	support private repos
	•	support Git LFS at least at a basic full-pull level

Storage location:

Documents/repos/<repo_name>/

Branch switch flow:
	1.	ensure mounted project unmounted
	2.	fetch remote
	3.	checkout branch
	4.	update working tree
	5.	remount project domain
	6.	relaunch target scene

The agent must test:
	•	switching branch A → B in same repo
	•	switching back B → A
	•	repeated branch toggling 20+ times

⸻

22. LIVE SYNC REQUIREMENTS

The agent must implement desktop-to-device live sync with:
	•	host discovery
	•	manual IP fallback
	•	websocket transport
	•	file write messages
	•	file delete messages
	•	reload request messages
	•	file hash handshake on reconnect

Recommended default:
	•	Rust desktop agent
	•	websocket protocol
	•	sha256 hashing
	•	zstd for large payloads

Example protocol messages

hello

{
  "type": "hello",
  "host_name": "MacBook-Pro",
  "project_name": "Raincaster",
  "project_root_hash": "abc123"
}

manifest

{
  "type": "manifest",
  "files": [
    {"path": "scripts/player.gd", "sha256": "....", "size": 1040},
    {"path": "scenes/main.tscn", "sha256": "....", "size": 2211}
  ]
}

write_small_file

{
  "type": "write_small_file",
  "path": "scripts/player.gd",
  "sha256": "....",
  "content_base64": "...."
}

write_large_file_begin

{
  "type": "write_large_file_begin",
  "path": "art/huge_texture.png",
  "sha256": "....",
  "size": 3456789,
  "compression": "zstd"
}

reload_hint

{
  "type": "reload_hint",
  "reload_tier": 2,
  "reason": "gdscript_changed",
  "changed_paths": ["scripts/player.gd"]
}

sync_complete

{
  "type": "sync_complete"
}

The mobile client must reconcile file hashes after reconnect.

⸻

23. REQUIRED TEST AUTOMATION

The AI agent must build repeatable test harnesses.

23.1 Domain switch stress test

Automated sequence:
	•	mount Project A
	•	launch
	•	exit
	•	unmount
	•	mount Project B
	•	launch
	•	exit
	•	unmount
	•	repeat 50 times

Collect:
	•	memory
	•	cache counts
	•	active autoload count
	•	leaked resource count
	•	crash rate

Pass condition:
	•	zero stale class/autoload/resource leakage across cycles

23.2 same-repo branch toggle stress test

Automated sequence:
	•	checkout branch A
	•	mount
	•	launch
	•	unmount
	•	checkout branch B
	•	mount
	•	launch
	•	unmount
	•	repeat 50 times

Pass condition:
	•	no stale class_name or autoload behavior

23.3 live script reload stress test

Automated sequence:
	•	mounted live project
	•	edit .gd file 20 times
	•	trigger Tier 2 relaunch each time

Pass condition:
	•	latest script behavior visible each time
	•	no cumulative stale script state

23.4 import test

Automated sequence:
	•	clear imported state
	•	mount project
	•	import assets on iOS
	•	verify imported outputs are generated
	•	run target scene

Pass condition:
	•	assets display correctly
	•	import output stable across remounts

23.5 autoload teardown test

Automated sequence:
	•	mount autoload project
	•	record autoload instance ids / state
	•	unmount
	•	remount
	•	verify recreated instances and cleared state

⸻

24. VALIDATION METRICS

The AI agent must report these metrics after each milestone.

Functional
	•	mount success rate
	•	unmount success rate
	•	relaunch success rate
	•	branch switch success rate
	•	live reload success rate

Correctness
	•	stale autoload count after unmount
	•	stale resource count after unmount
	•	stale script class registration count after unmount
	•	stale preload reference count after unmount

Performance
	•	mount duration
	•	unmount duration
	•	import duration
	•	target scene launch duration
	•	live sync file apply latency

Suggested initial targets:
	•	mount under 10s for medium test project
	•	unmount under 3s
	•	script Tier 2 relaunch under 5s
	•	branch switch relaunch under 8s

These are dev-tool targets, not shipping-game targets.

⸻

25. REQUIRED MILESTONES

The AI agent should work in milestones and must be able to stop after each with a demonstrable result.

Milestone 0 — build viability

Deliverables:
	•	fork compiles for iOS with required editor/runtime code included
	•	shell app boots
	•	standard editor UI does not appear
	•	shell placeholder UI visible

Pass:
	•	app launches on device or simulator if possible

Milestone 1 — minimal project mount

Deliverables:
	•	shell can select one local project
	•	project settings load
	•	imports run on iOS
	•	target scene launches

Pass:
	•	minimal_2d runs

Milestone 2 — return to shell

Deliverables:
	•	project can stop and return to shell
	•	no app restart required

Pass:
	•	launch and exit works 10 times in a row

Milestone 3 — project domain unmount

Deliverables:
	•	autoload teardown
	•	script cache clear attempt
	•	resource cache purge attempt
	•	mounted project settings removed

Pass:
	•	debug counters show reset after exit

Milestone 4 — two-project switch

Deliverables:
	•	mount Project A then Project B
	•	no stale scene/script/autoload behavior

Pass:
	•	class_name_collision_test passes

Milestone 5 — branch switching

Deliverables:
	•	Git fetch + checkout
	•	remount correct branch content

Pass:
	•	branch_switch_test passes 20 toggles

Milestone 6 — live sync

Deliverables:
	•	live host discovery or manual IP
	•	file updates arrive on device
	•	reload tiers triggered

Pass:
	•	live_reload_test passes for .gd and .tscn

Milestone 7 — stress stability

Deliverables:
	•	50-cycle mount/unmount stress pass
	•	logs and metrics report stable state

Pass:
	•	no crash, no growing stale counters

⸻

26. BUILD SCRIPT EXAMPLES

build_engine_ios.sh

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE_DIR="$ROOT_DIR/engine/upstream-godot-fork"
OUTPUT_DIR="$ROOT_DIR/engine/build-output/ios"

mkdir -p "$OUTPUT_DIR"

cd "$ENGINE_DIR"

scons -c || true

scons \
    platform=ios \
    tools=yes \
    target=debug \
    arch=arm64 \
    module_mono_enabled=no \
    dev_build=yes

cp "bin/libgodot.ios.debug.arm64.a" "$OUTPUT_DIR/libgodot_devplayer_ios.a"

echo "Built $OUTPUT_DIR/libgodot_devplayer_ios.a"

package_ios_runtime.sh

#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/engine/build-output/ios"
FRAMEWORK_DIR="$ROOT_DIR/mobile-app/ios-shell/Frameworks/EngineRuntime.framework"

rm -rf "$FRAMEWORK_DIR"
mkdir -p "$FRAMEWORK_DIR"

cp "$BUILD_DIR/libgodot_devplayer_ios.a" "$FRAMEWORK_DIR/EngineRuntime"

cat > "$FRAMEWORK_DIR/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
"http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>EngineRuntime</string>
    <key>CFBundleIdentifier</key>
    <string>com.fountaininteractive.devplayer.engine</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
</dict>
</plist>
EOF

echo "Packaged $FRAMEWORK_DIR"

These scripts are only starting points. The agent may need to revise them based on actual Godot iOS build outputs.

⸻

27. EXAMPLE IOS BRIDGE API

Swift-facing API target:

final class EngineBridge {
    func mountProject(path: String, targetScene: String?) throws
    func unmountProject() throws
    func relaunchMountedProject() throws
    func isProjectMounted() -> Bool
    func currentMountedProjectPath() -> String?
}

C bridge target:

extern "C" {
    int devplayer_mount_project(const char *project_path, const char *target_scene);
    int devplayer_unmount_project();
    int devplayer_relaunch_project();
    int devplayer_is_project_mounted();
    const char *devplayer_get_mounted_project_path();
}

The AI agent should implement this bridge once engine-side managers exist.

⸻

28. REQUIRED FAILURE HANDLING

The system must fail transparently.

If any of these happen:
	•	import failure
	•	script domain teardown failure
	•	autoload destroy failure
	•	branch checkout failure
	•	live sync apply failure

then the shell must:
	•	remain alive
	•	show the error
	•	keep logs
	•	not silently continue with partially stale project state

If the mounted project becomes unsafe:
	•	unmount it
	•	return to shell
	•	mark session invalid
	•	optionally require full app relaunch

⸻

29. DOCUMENTATION REQUIREMENTS FOR THE AGENT

At the end of each milestone the agent must update:

docs/milestones.md
	•	milestone status
	•	completed tasks
	•	blockers
	•	observed truths

docs/known_failures.md
	•	failing tests
	•	current failure modes
	•	hypotheses

docs/project_domain_reset.md
	•	actual mount order
	•	actual unmount order
	•	cache/script/autoload behavior
	•	revised reset semantics

The agent must never leave discoveries only in code comments or ephemeral logs.

⸻

30. EXECUTION PRIORITY

The AI agent must do work in this order:
	1.	prove iOS build viability
	2.	prove shell boots instead of editor UI
	3.	prove minimal project can mount/import/launch
	4.	prove return to shell
	5.	prove unmount can clear enough state for second project
	6.	prove class/autoload cache reset across two-project switch
	7.	add Git branch switching
	8.	add live sync
	9.	add stress harnesses
	10.	improve UX only after correctness

Correctness first, polish later.

⸻

31. FINAL INSTRUCTION TO THE AGENT

You are not building a simple app wrapper.
You are building a project-domain-capable fork of Godot for iOS.

The true product is the correctness of:
	•	project domain mount
	•	project domain unmount
	•	script domain reset
	•	autoload reset
	•	project settings rebinding
	•	iOS import pipeline rebinding

If those are solved, the UX is straightforward.

If those are not solved, the UX is fake.

Always prioritize:
	1.	truth-finding experiments
	2.	instrumentation
	3.	deterministic mount/unmount ordering
	4.	repeatable stress tests
	5.	written findings

⸻

I can turn this into a second companion document next: a JSON job list split into many small agent tasks for an AI coder pipeline.
