/**************************************************************************/
/*  import_session_manager.cpp                                            */
/**************************************************************************/
/*                         This file is part of:                          */
/*                           Godot LiveMount Module                            */
/**************************************************************************/

#include "import_session_manager.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource_importer.h"
#include "core/object/class_db.h"
#include "core/string/print_string.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_file_system.h"
#include "editor/import/resource_importer_bmfont.h"
#include "editor/import/resource_importer_csv_translation.h"
#include "editor/import/resource_importer_dynamic_font.h"
#include "editor/import/resource_importer_image.h"
#include "editor/import/resource_importer_imagefont.h"
#include "editor/import/resource_importer_layered_texture.h"
#include "editor/import/3d/resource_importer_obj.h"
#include "editor/import/3d/resource_importer_scene.h"
#include "editor/import/resource_importer_shader_file.h"
#include "editor/import/resource_importer_svg.h"
#include "editor/import/resource_importer_texture.h"
#include "editor/import/resource_importer_texture_atlas.h"
#include "editor/import/resource_importer_wav.h"
#include "editor/import/resource_importer_bitmask.h" // ResourceImporterBitMap
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#endif

ImportSessionManager *ImportSessionManager::singleton = nullptr;

// Common importable file extensions that Godot's import pipeline handles.
// These are files that require .import metadata to load correctly at runtime.
// Note: EditorFileSystem supports many more via plugins/GDExtensions, but
// these cover the vast majority of real-world project assets.
const char *ImportSessionManager::IMPORTABLE_EXTENSIONS[] = {
	// Images / textures
	"png", "jpg", "jpeg", "bmp", "svg", "webp", "tga", "hdr", "exr",
	// Audio
	"wav", "ogg", "mp3",
	// 3D models
	"gltf", "glb", "fbx", "obj", "dae", "blend",
	// Fonts
	"ttf", "otf", "woff", "woff2",
	// Translations
	"csv", "po",
	nullptr // Sentinel
};

ImportSessionManager *ImportSessionManager::get_singleton() {
	return singleton;
}

void ImportSessionManager::_bind_methods() {
	ClassDB::bind_method(D_METHOD("bind_project_root", "path"), &ImportSessionManager::bind_project_root);
	ClassDB::bind_method(D_METHOD("scan_filesystem"), &ImportSessionManager::scan_filesystem);
	ClassDB::bind_method(D_METHOD("import_pending_assets"), &ImportSessionManager::import_pending_assets);
	ClassDB::bind_method(D_METHOD("clear_import_state"), &ImportSessionManager::clear_import_state);
	ClassDB::bind_method(D_METHOD("get_total_importable_files"), &ImportSessionManager::get_total_importable_files);
	ClassDB::bind_method(D_METHOD("get_files_with_import_metadata"), &ImportSessionManager::get_files_with_import_metadata);
	ClassDB::bind_method(D_METHOD("get_files_missing_import_metadata"), &ImportSessionManager::get_files_missing_import_metadata);
	ClassDB::bind_method(D_METHOD("has_import_cache"), &ImportSessionManager::has_import_cache);
}

void ImportSessionManager::bind_project_root(const String &p_path) {
	ERR_FAIL_COND_MSG(p_path.is_empty(), "[ImportSessionManager] Project root path is empty.");

	project_root = p_path;
	bound = true;

	print_line("[ImportSessionManager] Bound to project root: " + project_root);
}

bool ImportSessionManager::_is_importable(const String &p_file, const HashSet<String> &p_extensions) const {
	String ext = p_file.get_extension().to_lower();
	return p_extensions.has(ext);
}

void ImportSessionManager::_scan_dir_recursive(const String &p_dir, const HashSet<String> &p_extensions) {
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}

	da->list_dir_begin();
	String fname = da->get_next();
	while (!fname.is_empty()) {
		if (fname == "." || fname == ".." || fname == ".godot") {
			fname = da->get_next();
			continue;
		}

		String full_path = p_dir.path_join(fname);

		if (da->current_is_dir()) {
			// Skip hidden directories.
			if (!fname.begins_with(".")) {
				_scan_dir_recursive(full_path, p_extensions);
			}
		} else {
			// Check if this is an importable file (not an .import file itself).
			if (!fname.ends_with(".import") && _is_importable(fname, p_extensions)) {
				total_importable_files++;

				// Check for corresponding .import metadata file.
				String import_file = full_path + ".import";
				if (FileAccess::exists(import_file)) {
					files_with_import_metadata++;
				} else {
					files_missing_import_metadata++;
					if (missing_imports.size() < 50) { // Cap to avoid huge lists.
						// Make path relative for readability.
						String rel_path = full_path.replace(project_root + "/", "");
						missing_imports.push_back(rel_path);
					}
				}
			}
		}

		fname = da->get_next();
	}
	da->list_dir_end();
}

void ImportSessionManager::scan_filesystem() {
	ERR_FAIL_COND_MSG(!bound, "[ImportSessionManager] Not bound to a project root. Call bind_project_root() first.");

	print_line("[ImportSessionManager] Scanning project for import status...");

	// Reset counters.
	total_importable_files = 0;
	files_with_import_metadata = 0;
	files_missing_import_metadata = 0;
	import_cache_exists = false;
	missing_imports.clear();

	// Check for .godot/imported/ directory (Godot 4.x import cache).
	String import_cache_dir = project_root.path_join(".godot").path_join("imported");
	import_cache_exists = DirAccess::exists(import_cache_dir);

	if (!import_cache_exists) {
		print_line("[ImportSessionManager] WARNING: Import cache directory not found: " + import_cache_dir);
		print_line("[ImportSessionManager] This project may not have been opened in the Godot editor yet.");
		print_line("[ImportSessionManager] Assets requiring import (textures, audio, etc.) may fail to load.");
	} else {
		print_line("[ImportSessionManager] Import cache directory found: " + import_cache_dir);
	}

	// Build extension set.
	HashSet<String> extensions;
	for (int i = 0; IMPORTABLE_EXTENSIONS[i] != nullptr; i++) {
		extensions.insert(String(IMPORTABLE_EXTENSIONS[i]));
	}

	// Scan the project directory recursively.
	_scan_dir_recursive(project_root, extensions);

	// Report results.
	print_line("[ImportSessionManager] Scan complete: " +
			itos(total_importable_files) + " importable files found, " +
			itos(files_with_import_metadata) + " with .import metadata, " +
			itos(files_missing_import_metadata) + " missing .import metadata.");

	if (files_missing_import_metadata > 0) {
		print_line("[ImportSessionManager] WARNING: " + itos(files_missing_import_metadata) +
				" files are missing .import metadata and may not load correctly.");
		print_line("[ImportSessionManager] These assets need to be imported by opening the project in the Godot editor.");
		for (int i = 0; i < missing_imports.size(); i++) {
			print_line("[ImportSessionManager]   Missing: " + missing_imports[i]);
		}
	}

	if (total_importable_files > 0 && files_missing_import_metadata == 0) {
		print_line("[ImportSessionManager] All importable assets have .import metadata. Project is ready for LiveMount.");
	}

	if (total_importable_files == 0) {
		print_line("[ImportSessionManager] No importable assets found (project uses only scripts/scenes).");
	}
}

void ImportSessionManager::register_importers() {
#ifdef TOOLS_ENABLED
	if (importers_registered) {
		return;
	}
	importers_registered = true;

	print_line("[ImportSessionManager] Registering resource importers...");

	Ref<ResourceImporterTexture> import_texture = memnew(ResourceImporterTexture(true));
	ResourceFormatImporter::get_singleton()->add_importer(import_texture);

	Ref<ResourceImporterLayeredTexture> import_cubemap;
	import_cubemap.instantiate();
	import_cubemap->set_mode(ResourceImporterLayeredTexture::MODE_CUBEMAP);
	ResourceFormatImporter::get_singleton()->add_importer(import_cubemap);

	Ref<ResourceImporterLayeredTexture> import_array;
	import_array.instantiate();
	import_array->set_mode(ResourceImporterLayeredTexture::MODE_2D_ARRAY);
	ResourceFormatImporter::get_singleton()->add_importer(import_array);

	Ref<ResourceImporterLayeredTexture> import_cubemap_array;
	import_cubemap_array.instantiate();
	import_cubemap_array->set_mode(ResourceImporterLayeredTexture::MODE_CUBEMAP_ARRAY);
	ResourceFormatImporter::get_singleton()->add_importer(import_cubemap_array);

	Ref<ResourceImporterLayeredTexture> import_3d = memnew(ResourceImporterLayeredTexture(true));
	import_3d->set_mode(ResourceImporterLayeredTexture::MODE_3D);
	ResourceFormatImporter::get_singleton()->add_importer(import_3d);

	Ref<ResourceImporterImage> import_image;
	import_image.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_image);

	Ref<ResourceImporterSVG> import_svg;
	import_svg.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_svg);

	Ref<ResourceImporterTextureAtlas> import_texture_atlas;
	import_texture_atlas.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_texture_atlas);

	Ref<ResourceImporterDynamicFont> import_font_dynamic;
	import_font_dynamic.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_font_dynamic);

	Ref<ResourceImporterBMFont> import_font_bmfont;
	import_font_bmfont.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_font_bmfont);

	Ref<ResourceImporterImageFont> import_font_image;
	import_font_image.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_font_image);

	Ref<ResourceImporterCSVTranslation> import_csv;
	import_csv.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_csv);

	Ref<ResourceImporterWAV> import_wav;
	import_wav.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_wav);

	Ref<ResourceImporterOBJ> import_obj;
	import_obj.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_obj);

	Ref<ResourceImporterShaderFile> import_shader_file;
	import_shader_file.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_shader_file);

	Ref<ResourceImporterScene> import_scene;
	import_scene.instantiate("PackedScene");
	ResourceFormatImporter::get_singleton()->add_importer(import_scene);

	Ref<ResourceImporterScene> import_anim;
	import_anim.instantiate("AnimationLibrary");
	ResourceFormatImporter::get_singleton()->add_importer(import_anim);

	Ref<ResourceImporterBitMap> import_bitmap;
	import_bitmap.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(import_bitmap);

	print_line("[ImportSessionManager] Resource importers registered.");
#else
	WARN_PRINT("[ImportSessionManager] Importers not available in non-editor builds.");
#endif
}

void ImportSessionManager::scan_and_import() {
#ifdef TOOLS_ENABLED
	ERR_FAIL_COND_MSG(!bound, "[ImportSessionManager] Not bound to a project root.");

	register_importers();

	if (!editor_file_system) {
		editor_file_system = memnew(EditorFileSystem);
		// EditorFileSystem needs to be in the tree to process.
		SceneTree *tree = SceneTree::get_singleton();
		if (tree && tree->get_root()) {
			tree->get_root()->add_child(editor_file_system);
		}
	}

	print_line("[ImportSessionManager] Starting EditorFileSystem scan and import...");
	editor_file_system->scan();
	print_line("[ImportSessionManager] Scan triggered. Import will run asynchronously.");
#else
	WARN_PRINT("[ImportSessionManager] Import not available in non-editor builds.");
#endif
}

void ImportSessionManager::import_pending_assets() {
	ERR_FAIL_COND_MSG(!bound, "[ImportSessionManager] Not bound to a project root. Call bind_project_root() first.");

#ifdef TOOLS_ENABLED
	scan_and_import();
#else
	if (files_missing_import_metadata > 0) {
		WARN_PRINT("[ImportSessionManager] " + itos(files_missing_import_metadata) +
				" assets are missing .import metadata. Open this project in the Godot editor to import them.");
	} else {
		print_line("[ImportSessionManager] No pending imports (all assets have .import metadata).");
	}
#endif
}

void ImportSessionManager::clear_import_state() {
	print_line("[ImportSessionManager] Clearing import state...");

	project_root = String();
	bound = false;
	total_importable_files = 0;
	files_with_import_metadata = 0;
	files_missing_import_metadata = 0;
	import_cache_exists = false;
	missing_imports.clear();

	print_line("[ImportSessionManager] Import state cleared.");
}

ImportSessionManager::ImportSessionManager() {
	singleton = this;
}

ImportSessionManager::~ImportSessionManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}
