/**************************************************************************/
/*  git_manager.cpp                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#include "git_manager.h"

#include "launch_controller.h"
#include "project_domain_manager.h"

#include "core/io/dir_access.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/string/print_string.h"

GitManager *GitManager::singleton = nullptr;

GitManager *GitManager::get_singleton() {
	return singleton;
}

void GitManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_repos_base_path", "path"), &GitManager::set_repos_base_path);
	ClassDB::bind_method(D_METHOD("get_repos_base_path"), &GitManager::get_repos_base_path);
	ClassDB::bind_method(D_METHOD("clone_repo", "url", "target_dir"), &GitManager::clone_repo);
	ClassDB::bind_method(D_METHOD("fetch", "repo_path"), &GitManager::fetch);
	ClassDB::bind_method(D_METHOD("checkout_branch", "repo_path", "branch"), &GitManager::checkout_branch);
	ClassDB::bind_method(D_METHOD("get_current_commit_info", "repo_path"), &GitManager::get_current_commit_info);
	ClassDB::bind_method(D_METHOD("list_branches", "repo_path"), &GitManager::list_branches);
	ClassDB::bind_method(D_METHOD("get_current_branch", "repo_path"), &GitManager::get_current_branch);
	ClassDB::bind_method(D_METHOD("switch_and_remount", "repo_path", "branch"), &GitManager::switch_and_remount);
}

void GitManager::set_launch_controller(LaunchController *p_launch_controller) {
	launch_controller = p_launch_controller;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void GitManager::set_repos_base_path(const String &p_path) {
	repos_base_path = p_path;
	print_line("[GitManager] Repos base path set to: " + repos_base_path);
}

String GitManager::get_repos_base_path() const {
	return repos_base_path;
}

// ---------------------------------------------------------------------------
// Internal helper
// ---------------------------------------------------------------------------

Error GitManager::_run_git(const String &p_repo_path, const Vector<String> &p_args, String &r_output) {
#ifdef APPLE_EMBEDDED_ENABLED
	// On iOS (Apple Embedded), fork() and popen() are prohibited by the
	// sandbox. Attempting to call OS::execute() will crash the process.
	// Return an error instead.
	r_output = "Git operations are not available on iOS.";
	ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "[GitManager] Git operations are not supported on iOS (fork() is prohibited).");
#endif

	// Build the argument list. We always pass -C <repo_path> first so that
	// the command runs inside the repository directory, except when the
	// caller passes an empty repo_path (used for clone where the repo does
	// not yet exist).
	List<String> args;
	if (!p_repo_path.is_empty()) {
		args.push_back("-C");
		args.push_back(p_repo_path);
	}
	for (int i = 0; i < p_args.size(); i++) {
		args.push_back(p_args[i]);
	}

	// Build a human-readable command string for logging.
	String cmd_str = "git";
	for (const String &a : args) {
		cmd_str += " " + a;
	}
	print_line("[GitManager] Running: " + cmd_str);

	String pipe_output;
	int exit_code = -1;
	Error err = OS::get_singleton()->execute("git", args, &pipe_output, &exit_code, true);

	r_output = pipe_output.strip_edges();

	if (err != OK) {
		ERR_PRINT("[GitManager] Failed to execute git command: " + cmd_str);
		return err;
	}

	if (exit_code != 0) {
		ERR_PRINT("[GitManager] git command exited with code " + itos(exit_code) + ": " + cmd_str);
		if (!r_output.is_empty()) {
			ERR_PRINT("[GitManager] Output: " + r_output);
		}
		return FAILED;
	}

	return OK;
}

// ---------------------------------------------------------------------------
// Core git operations
// ---------------------------------------------------------------------------

Error GitManager::clone_repo(const String &p_url, const String &p_target_dir) {
	ERR_FAIL_COND_V_MSG(p_url.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Clone URL is empty.");
	ERR_FAIL_COND_V_MSG(p_target_dir.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Target directory is empty.");

	// Check if the target directory already exists.
	Ref<DirAccess> da = DirAccess::open(p_target_dir);
	if (da.is_valid()) {
		ERR_FAIL_V_MSG(ERR_ALREADY_EXISTS, "[GitManager] Target directory already exists: " + p_target_dir);
	}

	print_line("[GitManager] Cloning " + p_url + " into " + p_target_dir + " ...");

	// Clone does not have a repo path yet, so we pass empty string and
	// provide the full clone command.
	Vector<String> args;
	args.push_back("clone");
	args.push_back(p_url);
	args.push_back(p_target_dir);

	String output;
	Error err = _run_git("", args, output);
	if (err != OK) {
		return err;
	}

	print_line("[GitManager] Clone complete: " + p_target_dir);
	return OK;
}

Error GitManager::fetch(const String &p_repo_path) {
	ERR_FAIL_COND_V_MSG(p_repo_path.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Repo path is empty.");

	print_line("[GitManager] Fetching updates for: " + p_repo_path);

	Vector<String> args;
	args.push_back("fetch");
	args.push_back("--all");
	args.push_back("--prune");

	String output;
	Error err = _run_git(p_repo_path, args, output);
	if (err != OK) {
		return err;
	}

	print_line("[GitManager] Fetch complete for: " + p_repo_path);
	return OK;
}

Error GitManager::checkout_branch(const String &p_repo_path, const String &p_branch) {
	ERR_FAIL_COND_V_MSG(p_repo_path.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Repo path is empty.");
	ERR_FAIL_COND_V_MSG(p_branch.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Branch name is empty.");

	print_line("[GitManager] Checking out branch '" + p_branch + "' in " + p_repo_path);

	Vector<String> args;
	args.push_back("checkout");
	args.push_back(p_branch);

	String output;
	Error err = _run_git(p_repo_path, args, output);
	if (err != OK) {
		return err;
	}

	print_line("[GitManager] Checkout complete. Now on branch: " + p_branch);
	return OK;
}

// ---------------------------------------------------------------------------
// Query operations
// ---------------------------------------------------------------------------

Dictionary GitManager::get_current_commit_info(const String &p_repo_path) {
	Dictionary info;
	ERR_FAIL_COND_V_MSG(p_repo_path.is_empty(), info, "[GitManager] Repo path is empty.");

	// Hash.
	{
		Vector<String> args;
		args.push_back("log");
		args.push_back("-1");
		args.push_back("--format=%H");

		String output;
		if (_run_git(p_repo_path, args, output) == OK) {
			info["hash"] = output;
		}
	}

	// Subject (commit message first line).
	{
		Vector<String> args;
		args.push_back("log");
		args.push_back("-1");
		args.push_back("--format=%s");

		String output;
		if (_run_git(p_repo_path, args, output) == OK) {
			info["message"] = output;
		}
	}

	// Author name.
	{
		Vector<String> args;
		args.push_back("log");
		args.push_back("-1");
		args.push_back("--format=%an");

		String output;
		if (_run_git(p_repo_path, args, output) == OK) {
			info["author"] = output;
		}
	}

	// Date (ISO 8601).
	{
		Vector<String> args;
		args.push_back("log");
		args.push_back("-1");
		args.push_back("--format=%aI");

		String output;
		if (_run_git(p_repo_path, args, output) == OK) {
			info["date"] = output;
		}
	}

	return info;
}

PackedStringArray GitManager::list_branches(const String &p_repo_path) {
	PackedStringArray branches;
	ERR_FAIL_COND_V_MSG(p_repo_path.is_empty(), branches, "[GitManager] Repo path is empty.");

	Vector<String> args;
	args.push_back("branch");
	args.push_back("-a");
	args.push_back("--format=%(refname:short)");

	String output;
	if (_run_git(p_repo_path, args, output) != OK) {
		return branches;
	}

	// Parse the output: one branch name per line.
	Vector<String> lines = output.split("\n", false);
	for (int i = 0; i < lines.size(); i++) {
		String branch = lines[i].strip_edges();
		if (!branch.is_empty()) {
			branches.push_back(branch);
		}
	}

	return branches;
}

String GitManager::get_current_branch(const String &p_repo_path) {
	ERR_FAIL_COND_V_MSG(p_repo_path.is_empty(), String(), "[GitManager] Repo path is empty.");

	Vector<String> args;
	args.push_back("rev-parse");
	args.push_back("--abbrev-ref");
	args.push_back("HEAD");

	String output;
	if (_run_git(p_repo_path, args, output) != OK) {
		return String();
	}

	return output;
}

// ---------------------------------------------------------------------------
// Orchestrated branch switch
// ---------------------------------------------------------------------------

Error GitManager::switch_and_remount(const String &p_repo_path, const String &p_branch) {
	ERR_FAIL_COND_V_MSG(p_repo_path.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Repo path is empty.");
	ERR_FAIL_COND_V_MSG(p_branch.is_empty(), ERR_INVALID_PARAMETER, "[GitManager] Branch name is empty.");
	ERR_FAIL_NULL_V_MSG(launch_controller, ERR_UNCONFIGURED, "[GitManager] LaunchController not set. Cannot orchestrate switch_and_remount.");

	String current_branch = get_current_branch(p_repo_path);
	print_line("[GitManager] === BRANCH SWITCH: '" + current_branch + "' -> '" + p_branch + "' ===");

	// Check if the project is currently mounted on this repo path.
	ProjectDomainManager *pdm = ProjectDomainManager::get_singleton();
	bool was_mounted = false;
	String mounted_path;
	String mounted_scene;

	if (pdm && pdm->is_project_mounted()) {
		mounted_path = pdm->get_mounted_project_path();
		// Check whether the mounted project lives inside (or is) this repo.
		if (mounted_path == p_repo_path || mounted_path.begins_with(p_repo_path + "/")) {
			was_mounted = true;
			mounted_scene = pdm->get_target_scene();
			print_line("[GitManager] Step 1/4: Unmounting project at " + mounted_path);
			launch_controller->stop_project();
		}
	}

	if (!was_mounted) {
		print_line("[GitManager] Step 1/4: No project mounted from this repo, skipping unmount.");
	}

	// Step 2: Fetch remote updates.
	print_line("[GitManager] Step 2/4: Fetching remote updates...");
	Error err = fetch(p_repo_path);
	if (err != OK) {
		ERR_PRINT("[GitManager] Fetch failed, aborting branch switch.");
		// Try to remount the original branch if we unmounted.
		if (was_mounted) {
			print_line("[GitManager] Attempting to remount original project...");
			launch_controller->launch_project(mounted_path, mounted_scene);
		}
		return err;
	}

	// Step 3: Checkout the target branch.
	print_line("[GitManager] Step 3/4: Checking out branch '" + p_branch + "'...");
	err = checkout_branch(p_repo_path, p_branch);
	if (err != OK) {
		ERR_PRINT("[GitManager] Checkout failed, aborting branch switch.");
		if (was_mounted) {
			print_line("[GitManager] Attempting to remount original project...");
			launch_controller->launch_project(mounted_path, mounted_scene);
		}
		return err;
	}

	// Step 4: Remount the project if it was mounted before.
	if (was_mounted) {
		print_line("[GitManager] Step 4/4: Remounting project from " + mounted_path);
		err = launch_controller->launch_project(mounted_path, mounted_scene);
		if (err != OK) {
			ERR_PRINT("[GitManager] Remount failed after branch switch.");
			return err;
		}
	} else {
		print_line("[GitManager] Step 4/4: Skipping remount (no project was mounted).");
	}

	print_line("[GitManager] === BRANCH SWITCH COMPLETE: now on '" + p_branch + "' ===");
	return OK;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

GitManager::GitManager() {
	singleton = this;

#ifdef APPLE_EMBEDDED_ENABLED
	// On iOS, the executable directory is inside the read-only .app bundle.
	// Use the user data directory (Documents/) for writable storage instead.
	repos_base_path = OS::get_singleton()->get_user_data_dir().path_join("repos");
#else
	// Default repos base path: next to the engine executable.
	repos_base_path = OS::get_singleton()->get_executable_path().get_base_dir().path_join("repos");
#endif
	print_line("[GitManager] Default repos base path: " + repos_base_path);
}

GitManager::~GitManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
