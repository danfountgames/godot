/**************************************************************************/
/*  devplayer_debug.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           GODOT BEAM ENGINE                            */
/**************************************************************************/

#include "devplayer_debug.h"

#include "core/object/class_db.h"
#include "core/string/print_string.h"

DevPlayerDebug *DevPlayerDebug::singleton = nullptr;

DevPlayerDebug *DevPlayerDebug::get_singleton() {
	return singleton;
}

void DevPlayerDebug::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_mounted_project_path", "path"), &DevPlayerDebug::set_mounted_project_path);
	ClassDB::bind_method(D_METHOD("get_mounted_project_path"), &DevPlayerDebug::get_mounted_project_path);

	ClassDB::bind_method(D_METHOD("set_target_scene", "scene"), &DevPlayerDebug::set_target_scene);
	ClassDB::bind_method(D_METHOD("get_target_scene"), &DevPlayerDebug::get_target_scene);

	ClassDB::bind_method(D_METHOD("set_active_autoload_count", "count"), &DevPlayerDebug::set_active_autoload_count);
	ClassDB::bind_method(D_METHOD("get_active_autoload_count"), &DevPlayerDebug::get_active_autoload_count);

	ClassDB::bind_method(D_METHOD("set_tracked_resource_count", "count"), &DevPlayerDebug::set_tracked_resource_count);
	ClassDB::bind_method(D_METHOD("get_tracked_resource_count"), &DevPlayerDebug::get_tracked_resource_count);

	ClassDB::bind_method(D_METHOD("set_tracked_script_cache_count", "count"), &DevPlayerDebug::set_tracked_script_cache_count);
	ClassDB::bind_method(D_METHOD("get_tracked_script_cache_count"), &DevPlayerDebug::get_tracked_script_cache_count);

	ClassDB::bind_method(D_METHOD("set_active_script_class_count", "count"), &DevPlayerDebug::set_active_script_class_count);
	ClassDB::bind_method(D_METHOD("get_active_script_class_count"), &DevPlayerDebug::get_active_script_class_count);

	ClassDB::bind_method(D_METHOD("set_last_import_duration", "duration"), &DevPlayerDebug::set_last_import_duration);
	ClassDB::bind_method(D_METHOD("get_last_import_duration"), &DevPlayerDebug::get_last_import_duration);

	ClassDB::bind_method(D_METHOD("set_last_mount_duration", "duration"), &DevPlayerDebug::set_last_mount_duration);
	ClassDB::bind_method(D_METHOD("get_last_mount_duration"), &DevPlayerDebug::get_last_mount_duration);

	ClassDB::bind_method(D_METHOD("set_last_unmount_duration", "duration"), &DevPlayerDebug::set_last_unmount_duration);
	ClassDB::bind_method(D_METHOD("get_last_unmount_duration"), &DevPlayerDebug::get_last_unmount_duration);

	ClassDB::bind_method(D_METHOD("set_reload_tier", "tier"), &DevPlayerDebug::set_reload_tier);
	ClassDB::bind_method(D_METHOD("get_reload_tier"), &DevPlayerDebug::get_reload_tier);

	ClassDB::bind_method(D_METHOD("set_leaked_references_after_unmount", "count"), &DevPlayerDebug::set_leaked_references_after_unmount);
	ClassDB::bind_method(D_METHOD("get_leaked_references_after_unmount"), &DevPlayerDebug::get_leaked_references_after_unmount);

	ClassDB::bind_method(D_METHOD("reset_all"), &DevPlayerDebug::reset_all);
	ClassDB::bind_method(D_METHOD("print_status"), &DevPlayerDebug::print_status);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "mounted_project_path"), "set_mounted_project_path", "get_mounted_project_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "target_scene"), "set_target_scene", "get_target_scene");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "active_autoload_count"), "set_active_autoload_count", "get_active_autoload_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tracked_resource_count"), "set_tracked_resource_count", "get_tracked_resource_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tracked_script_cache_count"), "set_tracked_script_cache_count", "get_tracked_script_cache_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "active_script_class_count"), "set_active_script_class_count", "get_active_script_class_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "last_import_duration"), "set_last_import_duration", "get_last_import_duration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "last_mount_duration"), "set_last_mount_duration", "get_last_mount_duration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "last_unmount_duration"), "set_last_unmount_duration", "get_last_unmount_duration");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "reload_tier"), "set_reload_tier", "get_reload_tier");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "leaked_references_after_unmount"), "set_leaked_references_after_unmount", "get_leaked_references_after_unmount");
}

void DevPlayerDebug::set_mounted_project_path(const String &p_path) {
	mounted_project_path = p_path;
}

String DevPlayerDebug::get_mounted_project_path() const {
	return mounted_project_path;
}

void DevPlayerDebug::set_target_scene(const String &p_scene) {
	target_scene = p_scene;
}

String DevPlayerDebug::get_target_scene() const {
	return target_scene;
}

void DevPlayerDebug::set_active_autoload_count(int p_count) {
	active_autoload_count = p_count;
}

int DevPlayerDebug::get_active_autoload_count() const {
	return active_autoload_count;
}

void DevPlayerDebug::set_tracked_resource_count(int p_count) {
	tracked_resource_count = p_count;
}

int DevPlayerDebug::get_tracked_resource_count() const {
	return tracked_resource_count;
}

void DevPlayerDebug::set_tracked_script_cache_count(int p_count) {
	tracked_script_cache_count = p_count;
}

int DevPlayerDebug::get_tracked_script_cache_count() const {
	return tracked_script_cache_count;
}

void DevPlayerDebug::set_active_script_class_count(int p_count) {
	active_script_class_count = p_count;
}

int DevPlayerDebug::get_active_script_class_count() const {
	return active_script_class_count;
}

void DevPlayerDebug::set_last_import_duration(double p_duration) {
	last_import_duration = p_duration;
}

double DevPlayerDebug::get_last_import_duration() const {
	return last_import_duration;
}

void DevPlayerDebug::set_last_mount_duration(double p_duration) {
	last_mount_duration = p_duration;
}

double DevPlayerDebug::get_last_mount_duration() const {
	return last_mount_duration;
}

void DevPlayerDebug::set_last_unmount_duration(double p_duration) {
	last_unmount_duration = p_duration;
}

double DevPlayerDebug::get_last_unmount_duration() const {
	return last_unmount_duration;
}

void DevPlayerDebug::set_reload_tier(int p_tier) {
	reload_tier = p_tier;
}

int DevPlayerDebug::get_reload_tier() const {
	return reload_tier;
}

void DevPlayerDebug::set_leaked_references_after_unmount(int p_count) {
	leaked_references_after_unmount = p_count;
}

int DevPlayerDebug::get_leaked_references_after_unmount() const {
	return leaked_references_after_unmount;
}

void DevPlayerDebug::reset_all() {
	mounted_project_path = String();
	target_scene = String();
	active_autoload_count = 0;
	tracked_resource_count = 0;
	tracked_script_cache_count = 0;
	active_script_class_count = 0;
	last_import_duration = 0.0;
	last_mount_duration = 0.0;
	last_unmount_duration = 0.0;
	reload_tier = 0;
	leaked_references_after_unmount = 0;
	print_line("[DevPlayerDebug] All metrics reset.");
}

void DevPlayerDebug::print_status() {
	print_line("=== DevPlayer Debug Status ===");
	print_line("  Mounted project: " + (mounted_project_path.is_empty() ? String("<none>") : mounted_project_path));
	print_line("  Target scene: " + (target_scene.is_empty() ? String("<none>") : target_scene));
	print_line("  Active autoloads: " + itos(active_autoload_count));
	print_line("  Tracked resources: " + itos(tracked_resource_count));
	print_line("  Script cache entries: " + itos(tracked_script_cache_count));
	print_line("  Active script classes: " + itos(active_script_class_count));
	print_line("  Last import duration: " + rtos(last_import_duration) + "s");
	print_line("  Last mount duration: " + rtos(last_mount_duration) + "s");
	print_line("  Last unmount duration: " + rtos(last_unmount_duration) + "s");
	print_line("  Reload tier: " + itos(reload_tier));
	print_line("  Leaked refs after unmount: " + itos(leaked_references_after_unmount));
	print_line("==============================");
}

DevPlayerDebug::DevPlayerDebug() {
	singleton = this;
}

DevPlayerDebug::~DevPlayerDebug() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
