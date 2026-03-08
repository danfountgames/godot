/**************************************************************************/
/*  git_manager.h                                                         */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#pragma once

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/variant/dictionary.h"
#include "core/templates/vector.h"

class LaunchController;

class GitManager : public Object {
	GDCLASS(GitManager, Object);

	static GitManager *singleton;

	LaunchController *launch_controller = nullptr;

	// Base directory where cloned repositories are stored.
	// Defaults to "<engine_executable_dir>/repos/".
	String repos_base_path;

	// Internal helper: runs a git command in the given repo directory and
	// captures stdout into r_output. Returns OK on success (exit code 0).
	Error _run_git(const String &p_repo_path, const Vector<String> &p_args, String &r_output);

protected:
	static void _bind_methods();

public:
	static GitManager *get_singleton();

	void set_launch_controller(LaunchController *p_launch_controller);

	// Configuration.
	void set_repos_base_path(const String &p_path);
	String get_repos_base_path() const;

	// Core git operations.
	Error clone_repo(const String &p_url, const String &p_target_dir);
	Error fetch(const String &p_repo_path);
	Error checkout_branch(const String &p_repo_path, const String &p_branch);

	// Query operations.
	Dictionary get_current_commit_info(const String &p_repo_path);
	PackedStringArray list_branches(const String &p_repo_path);
	String get_current_branch(const String &p_repo_path);

	// Orchestrated branch switch: unmount -> fetch -> checkout -> remount.
	Error switch_and_remount(const String &p_repo_path, const String &p_branch);

	GitManager();
	~GitManager();
};
