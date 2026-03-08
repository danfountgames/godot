//! GodotBeam Desktop Sync Agent
//!
//! Watches a local Godot project directory for file changes and syncs them
//! to a connected GodotBeam DevPlayer device via WebSocket.
//!
//! Protocol messages (JSON over WebSocket):
//!   hello, manifest, write_small_file, write_large_file_begin,
//!   reload_hint, sync_complete

use base64::Engine as _;
use clap::Parser;
use futures_util::{SinkExt, StreamExt};
use log::{error, info, warn};
use notify::{Config, Event, EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokio::sync::{mpsc, Mutex};
use tokio_tungstenite::connect_async;
use walkdir::WalkDir;

#[derive(Parser, Debug)]
#[command(name = "godotbeam-sync-agent")]
#[command(about = "Desktop sync agent for GodotBeam DevPlayer")]
struct Args {
    /// Path to the Godot project directory to watch
    #[arg(short, long)]
    project: PathBuf,

    /// Device IP address and port (e.g., 192.168.1.100:6850)
    #[arg(short, long, default_value = "localhost:6850")]
    device: String,

    /// Only sync, don't watch for changes
    #[arg(long)]
    sync_once: bool,
}

#[derive(Serialize, Deserialize, Debug)]
struct ProtocolMessage {
    #[serde(rename = "type")]
    msg_type: String,
    #[serde(flatten)]
    data: serde_json::Value,
}

#[derive(Serialize, Debug)]
struct HelloMessage {
    #[serde(rename = "type")]
    msg_type: String,
    host_name: String,
    project_name: String,
    project_root_hash: String,
}

#[derive(Serialize, Debug)]
struct ManifestFile {
    path: String,
    sha256: String,
    size: u64,
}

#[derive(Serialize, Debug)]
struct ManifestMessage {
    #[serde(rename = "type")]
    msg_type: String,
    files: Vec<ManifestFile>,
}

#[derive(Serialize, Debug)]
struct WriteSmallFileMessage {
    #[serde(rename = "type")]
    msg_type: String,
    path: String,
    sha256: String,
    content_base64: String,
}

#[derive(Serialize, Debug)]
struct ReloadHintMessage {
    #[serde(rename = "type")]
    msg_type: String,
    reload_tier: u8,
    reason: String,
    changed_paths: Vec<String>,
}

#[derive(Serialize, Debug)]
struct SyncCompleteMessage {
    #[serde(rename = "type")]
    msg_type: String,
}

/// File extensions that are considered content (Tier 1 reload).
const CONTENT_EXTENSIONS: &[&str] = &["png", "jpg", "jpeg", "svg", "wav", "ogg", "mp3", "tres"];

/// File extensions that are considered scripts (Tier 2 reload).
const SCRIPT_EXTENSIONS: &[&str] = &["gd", "tscn", "gdshader"];

/// File extensions/names that trigger full restart (Tier 3 reload).
const PROJECT_FILES: &[&str] = &["project.godot"];

/// Directories to skip when scanning/watching.
const SKIP_DIRS: &[&str] = &[".godot", ".git", ".import", "__pycache__"];

fn should_skip(path: &Path) -> bool {
    for component in path.components() {
        if let std::path::Component::Normal(name) = component {
            let name_str = name.to_string_lossy();
            if SKIP_DIRS.iter().any(|d| name_str == *d) {
                return true;
            }
        }
    }
    false
}

fn hash_file(path: &Path) -> std::io::Result<String> {
    let data = std::fs::read(path)?;
    let hash = Sha256::digest(&data);
    Ok(format!("{:x}", hash))
}

fn get_relative_path(base: &Path, full: &Path) -> Option<String> {
    full.strip_prefix(base)
        .ok()
        .map(|p| p.to_string_lossy().replace('\\', "/"))
}

fn determine_reload_tier(path: &str) -> u8 {
    let lower = path.to_lowercase();

    // Check Tier 3 first (project files).
    for pf in PROJECT_FILES {
        if lower.ends_with(pf) {
            return 3;
        }
    }

    // Check Tier 2 (scripts).
    for ext in SCRIPT_EXTENSIONS {
        if lower.ends_with(&format!(".{ext}")) {
            return 2;
        }
    }

    // Check Tier 1 (content).
    for ext in CONTENT_EXTENSIONS {
        if lower.ends_with(&format!(".{ext}")) {
            return 1;
        }
    }

    // Default: Tier 2 (session relaunch) for unknown file types.
    2
}

fn scan_project(project_dir: &Path) -> Vec<ManifestFile> {
    let mut files = Vec::new();

    for entry in WalkDir::new(project_dir)
        .into_iter()
        .filter_entry(|e| !should_skip(e.path()))
    {
        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };

        if !entry.file_type().is_file() {
            continue;
        }

        let rel_path = match get_relative_path(project_dir, entry.path()) {
            Some(p) => p,
            None => continue,
        };

        let sha256 = match hash_file(entry.path()) {
            Ok(h) => h,
            Err(_) => continue,
        };

        let size = entry.metadata().map(|m| m.len()).unwrap_or(0);

        files.push(ManifestFile {
            path: rel_path,
            sha256,
            size,
        });
    }

    files
}

fn get_project_name(project_dir: &Path) -> String {
    // Try to read from project.godot.
    let project_file = project_dir.join("project.godot");
    if let Ok(content) = std::fs::read_to_string(&project_file) {
        for line in content.lines() {
            if line.starts_with("config/name=") {
                let name = line
                    .trim_start_matches("config/name=")
                    .trim_matches('"');
                return name.to_string();
            }
        }
    }
    // Fallback to directory name.
    project_dir
        .file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_string()
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::init();

    let args = Args::parse();

    if !args.project.exists() {
        error!("Project directory does not exist: {:?}", args.project);
        std::process::exit(1);
    }

    let project_dir = args.project.canonicalize()?;
    let project_name = get_project_name(&project_dir);

    info!("GodotBeam Sync Agent");
    info!("Project: {} ({:?})", project_name, project_dir);
    info!("Device: {}", args.device);

    // Build initial manifest.
    info!("Scanning project files...");
    let manifest = scan_project(&project_dir);
    info!("Found {} files in project", manifest.len());

    // Build hash map for comparison.
    let file_hashes: Arc<Mutex<HashMap<String, String>>> = Arc::new(Mutex::new(
        manifest
            .iter()
            .map(|f| (f.path.clone(), f.sha256.clone()))
            .collect(),
    ));

    // Connect to device.
    let url = format!("ws://{}", args.device);
    info!("Connecting to {}...", url);

    let (ws_stream, _) = connect_async(&url).await?;
    info!("Connected to device!");

    let (mut write, mut read) = ws_stream.split();

    // Send hello.
    let hello = HelloMessage {
        msg_type: "hello".to_string(),
        host_name: hostname::get()
            .unwrap_or_default()
            .to_string_lossy()
            .to_string(),
        project_name: project_name.clone(),
        project_root_hash: {
            let hash = Sha256::digest(project_dir.to_string_lossy().as_bytes());
            format!("{:x}", hash)[..12].to_string()
        },
    };
    write
        .send(tokio_tungstenite::tungstenite::Message::Text(
            serde_json::to_string(&hello)?,
        ))
        .await?;
    info!("Sent hello handshake");

    // Send manifest.
    let manifest_msg = ManifestMessage {
        msg_type: "manifest".to_string(),
        files: manifest,
    };
    write
        .send(tokio_tungstenite::tungstenite::Message::Text(
            serde_json::to_string(&manifest_msg)?,
        ))
        .await?;
    info!("Sent file manifest");

    if args.sync_once {
        info!("Sync-once mode: done.");
        return Ok(());
    }

    // Set up file watcher.
    let (tx, mut rx) = mpsc::channel::<Vec<PathBuf>>(100);

    let project_dir_clone = project_dir.clone();
    let _watcher_handle = tokio::task::spawn_blocking(move || {
        let tx = tx;
        let mut watcher: RecommendedWatcher =
            Watcher::new(
                move |res: Result<Event, notify::Error>| {
                    if let Ok(event) = res {
                        match event.kind {
                            EventKind::Create(_) | EventKind::Modify(_) | EventKind::Remove(_) => {
                                let paths: Vec<PathBuf> = event
                                    .paths
                                    .into_iter()
                                    .filter(|p| !should_skip(p) && p.is_file())
                                    .collect();
                                if !paths.is_empty() {
                                    let _ = tx.blocking_send(paths);
                                }
                            }
                            _ => {}
                        }
                    }
                },
                Config::default(),
            )
            .expect("Failed to create file watcher");

        watcher
            .watch(&project_dir_clone, RecursiveMode::Recursive)
            .expect("Failed to watch project directory");

        info!("File watcher started. Watching for changes...");

        // Keep the watcher alive.
        loop {
            std::thread::sleep(std::time::Duration::from_secs(3600));
        }
    });

    // Process file changes and sync to device.
    let write = Arc::new(Mutex::new(write));

    loop {
        tokio::select! {
            Some(changed_paths) = rx.recv() => {
                let mut changed_files: Vec<(String, u8)> = Vec::new();
                let mut max_tier: u8 = 0;

                for path in &changed_paths {
                    let rel_path = match get_relative_path(&project_dir, path) {
                        Some(p) => p,
                        None => continue,
                    };

                    if !path.exists() {
                        // File was deleted -- skip for now (could send delete message).
                        warn!("File deleted: {}", rel_path);
                        continue;
                    }

                    let content = match std::fs::read(path) {
                        Ok(c) => c,
                        Err(e) => {
                            error!("Failed to read {}: {}", rel_path, e);
                            continue;
                        }
                    };

                    let sha256 = {
                        let hash = Sha256::digest(&content);
                        format!("{:x}", hash)
                    };

                    // Check if file actually changed.
                    {
                        let hashes = file_hashes.lock().await;
                        if let Some(old_hash) = hashes.get(&rel_path) {
                            if *old_hash == sha256 {
                                continue; // No actual change.
                            }
                        }
                    }

                    let tier = determine_reload_tier(&rel_path);
                    max_tier = max_tier.max(tier);

                    info!("Changed: {} (tier {})", rel_path, tier);

                    // Send the file.
                    let msg = WriteSmallFileMessage {
                        msg_type: "write_small_file".to_string(),
                        path: rel_path.clone(),
                        sha256: sha256.clone(),
                        content_base64: base64::engine::general_purpose::STANDARD.encode(&content),
                    };

                    let mut w = write.lock().await;
                    if let Err(e) = w.send(tokio_tungstenite::tungstenite::Message::Text(
                        serde_json::to_string(&msg).unwrap(),
                    )).await {
                        error!("Failed to send file: {}", e);
                        break;
                    }

                    // Update hash map.
                    file_hashes.lock().await.insert(rel_path.clone(), sha256);
                    changed_files.push((rel_path, tier));
                }

                if !changed_files.is_empty() {
                    // Send reload hint.
                    let reload = ReloadHintMessage {
                        msg_type: "reload_hint".to_string(),
                        reload_tier: max_tier,
                        reason: match max_tier {
                            1 => "content_changed".to_string(),
                            2 => "gdscript_changed".to_string(),
                            3 => "project_config_changed".to_string(),
                            _ => "unknown".to_string(),
                        },
                        changed_paths: changed_files.iter().map(|(p, _)| p.clone()).collect(),
                    };

                    let mut w = write.lock().await;
                    let _ = w.send(tokio_tungstenite::tungstenite::Message::Text(
                        serde_json::to_string(&reload).unwrap(),
                    )).await;

                    // Send sync complete.
                    let complete = SyncCompleteMessage {
                        msg_type: "sync_complete".to_string(),
                    };
                    let _ = w.send(tokio_tungstenite::tungstenite::Message::Text(
                        serde_json::to_string(&complete).unwrap(),
                    )).await;

                    info!("Synced {} files (max reload tier: {})", changed_files.len(), max_tier);
                }
            }
            msg = read.next() => {
                match msg {
                    Some(Ok(msg)) => {
                        if msg.is_text() {
                            info!("Device message: {}", msg.to_text().unwrap_or(""));
                        }
                    }
                    Some(Err(e)) => {
                        error!("WebSocket error: {}", e);
                        break;
                    }
                    None => {
                        info!("Device disconnected");
                        break;
                    }
                }
            }
        }
    }

    info!("Sync agent shutting down");
    Ok(())
}
