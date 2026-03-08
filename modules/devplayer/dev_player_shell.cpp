/**************************************************************************/
/*  dev_player_shell.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "dev_player_shell.h"

#include "autoload_session_manager.h"
#include "devplayer_debug.h"
#include "launch_controller.h"
#include "project_domain_manager.h"
#include "resource_domain_manager.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "scene/gui/box_container.h"
#include "scene/main/scene_tree.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/separator.h"

void DevPlayerShell::_bind_methods() {
}

void DevPlayerShell::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_READY: {
			_build_ui();
		} break;
		case NOTIFICATION_PROCESS: {
			_update_debug_display();
			if (automated_test_mode) {
				_run_test_step();
			}
		} break;
	}
}

void DevPlayerShell::_capture_engine_root() {
	if (engine_root_captured) {
		return;
	}
	// Capture the engine's resource path before any mount changes it.
	// ProjectSettings::get_resource_path() returns the directory containing
	// the engine's own project.godot — which is the engine root.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps) {
		engine_root_path = ps->get_resource_path();
		engine_root_captured = true;
		print_line("[DevPlayerShell] Captured engine root path: " + engine_root_path);
	}
}

String DevPlayerShell::_test_project_path(const String &p_project_name) const {
	return engine_root_path.path_join("test_projects").path_join(p_project_name);
}

void DevPlayerShell::_test_check(bool p_condition, const String &p_pass_msg, const String &p_fail_msg) {
	if (p_condition) {
		print_line("[TEST PASS] " + p_pass_msg);
		test_pass_count++;
	} else {
		print_line("[TEST FAIL] " + p_fail_msg);
		test_fail_count++;
	}
}

void DevPlayerShell::_build_ui() {
	// Capture engine root path before anything can change it.
	_capture_engine_root();

	// Grab singleton references if not already set.
	if (!launch_controller) {
		launch_controller = LaunchController::get_singleton();
	}
	if (!project_domain_manager) {
		project_domain_manager = ProjectDomainManager::get_singleton();
	}
	if (!autoload_session_manager) {
		autoload_session_manager = AutoloadSessionManager::get_singleton();
	}
	if (!resource_domain_manager) {
		resource_domain_manager = ResourceDomainManager::get_singleton();
	}
	if (!debug) {
		debug = DevPlayerDebug::get_singleton();
	}

	set_anchors_preset(PRESET_FULL_RECT);

	shell_ui_root = memnew(VBoxContainer);
	shell_ui_root->set_anchors_preset(PRESET_FULL_RECT);
	shell_ui_root->set_offset(SIDE_LEFT, 20);
	shell_ui_root->set_offset(SIDE_TOP, 20);
	shell_ui_root->set_offset(SIDE_RIGHT, -20);
	shell_ui_root->set_offset(SIDE_BOTTOM, -20);
	add_child(shell_ui_root);

	// Title.
	title_label = memnew(Label);
	title_label->set_text("GodotBeam DevPlayer Shell");
	title_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	shell_ui_root->add_child(title_label);

	shell_ui_root->add_child(memnew(HSeparator));

	// Info container.
	info_container = memnew(VBoxContainer);
	shell_ui_root->add_child(info_container);

	status_label = memnew(Label);
	status_label->set_text("Status: No project mounted");
	info_container->add_child(status_label);

	project_path_label = memnew(Label);
	project_path_label->set_text("Project: <none>");
	info_container->add_child(project_path_label);

	resource_count_label = memnew(Label);
	resource_count_label->set_text("Tracked resources: 0");
	info_container->add_child(resource_count_label);

	autoload_count_label = memnew(Label);
	autoload_count_label->set_text("Active autoloads: 0");
	info_container->add_child(autoload_count_label);

	mount_duration_label = memnew(Label);
	mount_duration_label->set_text("Last mount duration: N/A");
	info_container->add_child(mount_duration_label);

	shell_ui_root->add_child(memnew(HSeparator));

	// Project path input.
	Label *path_label = memnew(Label);
	path_label->set_text("Project Path:");
	shell_ui_root->add_child(path_label);

	path_line_edit = memnew(LineEdit);
#ifdef APPLE_EMBEDDED_ENABLED
	// On iOS, the engine root is inside the read-only .app bundle.
	// Default to a Documents-relative project path instead.
	path_line_edit->set_text(OS::get_singleton()->get_user_data_dir().path_join("projects").path_join("minimal_2d"));
#else
	// Default to the minimal_2d test project path, derived from engine root.
	path_line_edit->set_text(_test_project_path("minimal_2d"));
#endif
	path_line_edit->set_placeholder("Enter absolute path to project directory...");
	shell_ui_root->add_child(path_line_edit);

	shell_ui_root->add_child(memnew(HSeparator));

	// Buttons.
	HBoxContainer *button_container = memnew(HBoxContainer);
	shell_ui_root->add_child(button_container);

	mount_button = memnew(Button);
	mount_button->set_text("Mount Project");
	mount_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_mount_pressed));
	button_container->add_child(mount_button);

	unmount_button = memnew(Button);
	unmount_button->set_text("Unmount Project");
	unmount_button->connect("pressed", callable_mp(this, &DevPlayerShell::_on_unmount_pressed));
	button_container->add_child(unmount_button);

	set_process(true);
}

void DevPlayerShell::_update_debug_display() {
	if (!debug) {
		return;
	}

	bool mounted = project_domain_manager && project_domain_manager->is_project_mounted();

	if (status_label) {
		status_label->set_text(mounted ? "Status: Project MOUNTED" : "Status: No project mounted");
	}

	if (project_path_label) {
		String path = debug->get_mounted_project_path();
		project_path_label->set_text("Project: " + (path.is_empty() ? String("<none>") : path));
	}

	if (resource_count_label) {
		resource_count_label->set_text("Tracked resources: " + itos(debug->get_tracked_resource_count()));
	}

	if (autoload_count_label) {
		autoload_count_label->set_text("Active autoloads: " + itos(debug->get_active_autoload_count()));
	}

	if (mount_duration_label) {
		double dur = debug->get_last_mount_duration();
		mount_duration_label->set_text("Last mount duration: " + (dur > 0.0 ? rtos(dur) + "s" : String("N/A")));
	}

	if (mount_button) {
		mount_button->set_disabled(mounted);
	}

	if (unmount_button) {
		unmount_button->set_disabled(!mounted);
	}
}

void DevPlayerShell::_on_mount_pressed() {
	if (!launch_controller) {
		ERR_PRINT("[DevPlayerShell] LaunchController not set.");
		return;
	}

	// Read the project path from the LineEdit input field.
	String project_path;
	if (path_line_edit) {
		project_path = path_line_edit->get_text().strip_edges();
	}

	if (project_path.is_empty()) {
		ERR_PRINT("[DevPlayerShell] No project path specified.");
		return;
	}

	print_line("[DevPlayerShell] Mount button pressed. Attempting to mount: " + project_path);

	// The target scene is left empty -- LaunchController will read it from ProjectSettings
	// after loading the project's project.godot file.
	Error err = launch_controller->launch_project(project_path, String());
	if (err != OK) {
		ERR_PRINT("[DevPlayerShell] Failed to launch project: " + project_path);
		return;
	}

	// Hide the shell UI to show the project scene.
	if (shell_ui_root) {
		shell_ui_root->set_visible(false);
	}
}

void DevPlayerShell::_on_unmount_pressed() {
	if (!launch_controller) {
		ERR_PRINT("[DevPlayerShell] LaunchController not set.");
		return;
	}

	print_line("[DevPlayerShell] Unmount button pressed.");
	launch_controller->stop_project();

	// Show the shell UI again after the project scene is removed.
	if (shell_ui_root) {
		shell_ui_root->set_visible(true);
	}
}

void DevPlayerShell::set_managers(
		LaunchController *p_launch_controller,
		ProjectDomainManager *p_project_domain,
		AutoloadSessionManager *p_autoload_session,
		ResourceDomainManager *p_resource_domain,
		DevPlayerDebug *p_debug) {
	launch_controller = p_launch_controller;
	project_domain_manager = p_project_domain;
	autoload_session_manager = p_autoload_session;
	resource_domain_manager = p_resource_domain;
	debug = p_debug;
}

void DevPlayerShell::set_automated_test_mode(bool p_enabled) {
	automated_test_mode = p_enabled;
	if (p_enabled) {
		print_line("[DEVPLAYER TEST] Automated test mode enabled.");
		test_step = 0;
		test_timer = 0.0;
		test_pass_count = 0;
		test_fail_count = 0;
		stress_cycle_index = 0;
	}
}

void DevPlayerShell::_run_test_step() {
	// Grab singletons lazily (they may not be set via set_managers yet).
	if (!launch_controller) {
		launch_controller = LaunchController::get_singleton();
	}
	if (!project_domain_manager) {
		project_domain_manager = ProjectDomainManager::get_singleton();
	}
	if (!debug) {
		debug = DevPlayerDebug::get_singleton();
	}

	if (!launch_controller || !project_domain_manager) {
		return; // Not ready yet, wait for next frame.
	}

	// Capture engine root on first test frame (before any mount).
	_capture_engine_root();

	double delta = get_process_delta_time();

	// Test project paths derived from engine root.
	const String path_minimal_2d = _test_project_path("minimal_2d");
	const String path_collision_a = _test_project_path("class_name_collision_test_a");
	const String path_collision_b = _test_project_path("class_name_collision_test_b");
	const String path_autoload = _test_project_path("autoload_reset_test");

	// =========================================================================
	// Test state machine.
	//
	// Each "cycle" follows the pattern:
	//   MOUNT step  -> launch_project(), verify Error code
	//   VERIFY step -> wait 0.3s, check is_project_mounted(), then stop_project()
	//   CLEANUP step -> wait 1 frame, check unmounted + leaked refs == 0
	//
	// Cycle 1  (steps 0-2):   Mount minimal_2d
	// Cycle 2  (steps 3-5):   Mount class_name_collision_test_a
	// Cycle 3  (steps 6-8):   Remount minimal_2d (A->B->A pattern)
	// Cycle 4  (steps 9-11):  Mount class_name_collision_test_b
	// Cycle 5  (steps 12-14): Mount autoload_reset_test
	// Cycle 6  (steps 15-17): Remount autoload_reset_test (verify re-mount)
	// Stress   (steps 18-19): 5 rapid A/B cycles (minimal_2d <-> collision_a)
	// Final    (step 20):     Summary + quit
	// =========================================================================

	switch (test_step) {
		// =================================================================
		// Cycle 1: Mount minimal_2d
		// =================================================================
		case 0: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 1: Mount minimal_2d");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_minimal_2d, String());
			_test_check(err == OK,
					"Mount call succeeded for " + path_minimal_2d,
					"Failed to mount " + path_minimal_2d + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 1;
		} break;

		case 1: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project is mounted after wait (cycle 1).",
						"Project should be mounted but is not (cycle 1).");
				print_line("[DEVPLAYER TEST] Stopping project (cycle 1).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 2;
			}
		} break;

		case 2: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted successfully (cycle 1).",
						"Project still mounted after stop_project (cycle 1).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked references after unmount (cycle 1).",
							"Leaked references after unmount (cycle 1): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 3;
			}
		} break;

		// =================================================================
		// Cycle 2: Mount class_name_collision_test_a
		// =================================================================
		case 3: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 2: Mount class_name_collision_test_a");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_collision_a, String());
			_test_check(err == OK,
					"Mount call succeeded for " + path_collision_a,
					"Failed to mount " + path_collision_a + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 4;
		} break;

		case 4: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project is mounted after wait (cycle 2).",
						"Project should be mounted but is not (cycle 2).");
				print_line("[DEVPLAYER TEST] Stopping project (cycle 2).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 5;
			}
		} break;

		case 5: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted successfully (cycle 2).",
						"Project still mounted after stop_project (cycle 2).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked references after unmount (cycle 2).",
							"Leaked references after unmount (cycle 2): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 6;
			}
		} break;

		// =================================================================
		// Cycle 3: Remount minimal_2d (A->B->A pattern)
		// =================================================================
		case 6: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 3: Remount minimal_2d (A->B->A)");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_minimal_2d, String());
			_test_check(err == OK,
					"Remount call succeeded for " + path_minimal_2d,
					"Failed to remount " + path_minimal_2d + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 7;
		} break;

		case 7: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project is mounted after remount (cycle 3).",
						"Project should be mounted but is not (cycle 3).");
				print_line("[DEVPLAYER TEST] Stopping project (cycle 3).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 8;
			}
		} break;

		case 8: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted successfully (cycle 3).",
						"Project still mounted after stop_project (cycle 3).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked references after unmount (cycle 3).",
							"Leaked references after unmount (cycle 3): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 9;
			}
		} break;

		// =================================================================
		// Cycle 4: Mount class_name_collision_test_b
		// =================================================================
		case 9: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 4: Mount class_name_collision_test_b");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_collision_b, String());
			_test_check(err == OK,
					"Mount call succeeded for " + path_collision_b,
					"Failed to mount " + path_collision_b + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 10;
		} break;

		case 10: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project is mounted after wait (cycle 4).",
						"Project should be mounted but is not (cycle 4).");
				print_line("[DEVPLAYER TEST] Stopping project (cycle 4).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 11;
			}
		} break;

		case 11: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted successfully (cycle 4).",
						"Project still mounted after stop_project (cycle 4).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked references after unmount (cycle 4).",
							"Leaked references after unmount (cycle 4): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 12;
			}
		} break;

		// =================================================================
		// Cycle 5: Mount autoload_reset_test
		// =================================================================
		case 12: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 5: Mount autoload_reset_test");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_autoload, String());
			_test_check(err == OK,
					"Mount call succeeded for " + path_autoload,
					"Failed to mount " + path_autoload + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 13;
		} break;

		case 13: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project is mounted after wait (cycle 5).",
						"Project should be mounted but is not (cycle 5).");

				// Verify autoloads were created for this project.
				if (debug) {
					int autoloads = debug->get_active_autoload_count();
					_test_check(autoloads > 0,
							"Autoloads created for autoload_reset_test: " + itos(autoloads),
							"No autoloads created for autoload_reset_test (expected > 0).");
				}

				print_line("[DEVPLAYER TEST] Stopping project (cycle 5).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 14;
			}
		} break;

		case 14: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted successfully (cycle 5).",
						"Project still mounted after stop_project (cycle 5).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked references after unmount (cycle 5).",
							"Leaked references after unmount (cycle 5): " + itos(leaked));
				}
				test_timer = 0.0;
				test_step = 15;
			}
		} break;

		// =================================================================
		// Cycle 6: Remount autoload_reset_test (verify re-create)
		// =================================================================
		case 15: {
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Cycle 6: Remount autoload_reset_test");
			print_line("========================================");
			Error err = launch_controller->launch_project(path_autoload, String());
			_test_check(err == OK,
					"Remount call succeeded for " + path_autoload,
					"Failed to remount " + path_autoload + " (error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 16;
		} break;

		case 16: {
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project is mounted after remount (cycle 6).",
						"Project should be mounted but is not (cycle 6).");

				// Verify autoloads were re-created on remount.
				if (debug) {
					int autoloads = debug->get_active_autoload_count();
					_test_check(autoloads > 0,
							"Autoloads re-created on remount: " + itos(autoloads),
							"No autoloads on remount (expected > 0, count was reset).");
				}

				print_line("[DEVPLAYER TEST] Stopping project (cycle 6).");
				launch_controller->stop_project();
				test_timer = 0.0;
				test_step = 17;
			}
		} break;

		case 17: {
			test_timer += delta;
			if (test_timer >= delta) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted successfully (cycle 6).",
						"Project still mounted after stop_project (cycle 6).");
				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked references after unmount (cycle 6).",
							"Leaked references after unmount (cycle 6): " + itos(leaked));
				}
				// Initialize stress test.
				stress_cycle_index = 0;
				test_timer = 0.0;
				test_step = 18;
			}
		} break;

		// =================================================================
		// Stress test: 5 rapid A/B cycles (minimal_2d <-> collision_a)
		// =================================================================
		case 18: {
			// Mount phase of stress cycle.
			if (stress_cycle_index >= STRESS_CYCLE_COUNT) {
				// All stress cycles done, move to final summary.
				test_step = 20;
				break;
			}

			bool use_a = (stress_cycle_index % 2 == 0);
			String stress_path = use_a ? path_minimal_2d : path_collision_a;
			String stress_name = use_a ? "minimal_2d" : "class_name_collision_test_a";

			print_line("[DEVPLAYER TEST] Stress cycle " + itos(stress_cycle_index + 1) +
					"/" + itos(STRESS_CYCLE_COUNT) + ": Mounting " + stress_name);

			Error err = launch_controller->launch_project(stress_path, String());
			_test_check(err == OK,
					"Stress mount succeeded: " + stress_name + " (cycle " + itos(stress_cycle_index + 1) + ")",
					"Stress mount failed: " + stress_name + " (cycle " + itos(stress_cycle_index + 1) + ", error " + itos(err) + ")");
			test_timer = 0.0;
			test_step = 19;
		} break;

		case 19: {
			// Wait, verify, stop, verify unmount, then loop back to step 18.
			test_timer += delta;
			if (test_timer >= 0.3) {
				bool mounted = project_domain_manager->is_project_mounted();
				_test_check(mounted,
						"Project mounted in stress cycle " + itos(stress_cycle_index + 1) + ".",
						"Project NOT mounted in stress cycle " + itos(stress_cycle_index + 1) + ".");

				launch_controller->stop_project();

				// Wait one more frame worth of time by checking on next entry.
				// Since stop_project is synchronous for unmount state, we can
				// check immediately.
				mounted = project_domain_manager->is_project_mounted();
				_test_check(!mounted,
						"Project unmounted in stress cycle " + itos(stress_cycle_index + 1) + ".",
						"Project still mounted after stop in stress cycle " + itos(stress_cycle_index + 1) + ".");

				if (debug) {
					int leaked = debug->get_leaked_references_after_unmount();
					_test_check(leaked == 0,
							"No leaked refs in stress cycle " + itos(stress_cycle_index + 1) + ".",
							"Leaked refs in stress cycle " + itos(stress_cycle_index + 1) + ": " + itos(leaked));
				}

				stress_cycle_index++;
				test_timer = 0.0;
				test_step = 18; // Loop back to mount the next stress cycle.
			}
		} break;

		// =================================================================
		// Final: Report results and quit
		// =================================================================
		case 20: {
			print_line("");
			print_line("========================================");
			print_line("[DEVPLAYER TEST] All test cycles complete.");
			print_line("========================================");
			print_line("[DEVPLAYER TEST] Test projects exercised:");
			print_line("  - minimal_2d");
			print_line("  - class_name_collision_test_a");
			print_line("  - class_name_collision_test_b");
			print_line("  - autoload_reset_test");
			print_line("[DEVPLAYER TEST] Cycles: 6 named + " + itos(STRESS_CYCLE_COUNT) + " stress = " + itos(6 + STRESS_CYCLE_COUNT) + " total");
			print_line("----------------------------------------");
			print_line("[DEVPLAYER TEST] Passed: " + itos(test_pass_count));
			print_line("[DEVPLAYER TEST] Failed: " + itos(test_fail_count));
			print_line("[DEVPLAYER TEST] Total:  " + itos(test_pass_count + test_fail_count));

			if (test_fail_count == 0) {
				print_line("[TEST PASS] ALL TESTS PASSED");
			} else {
				print_line("[TEST FAIL] SOME TESTS FAILED");
			}
			print_line("========================================");

			// Quit the engine with appropriate exit code.
			SceneTree *tree = SceneTree::get_singleton();
			if (tree) {
				tree->quit(test_fail_count > 0 ? 1 : 0);
			}

			// Prevent re-entry.
			automated_test_mode = false;
			test_step = 21;
		} break;

		default:
			break;
	}
}

DevPlayerShell::DevPlayerShell() {
}

DevPlayerShell::~DevPlayerShell() {
}
