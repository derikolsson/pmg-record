# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

OBS Studio frontend plugin (C++/Qt) that simplifies OBS into a capture-focused tool. Prompts to rename recordings and replay buffer saves, strips the UI down to essentials, and exposes a WebSocket vendor API for external filename control. Based on [Record Rename](https://obsproject.com/forum/resources/record-rename.2134/) by Exeldro.

## OBS Plugin Documentation

- [OBS Plugin API Reference](https://docs.obsproject.com/plugins)
- [OBS Developer Guide](https://obsproject.com/kb/developer-guide)

## Build Commands

### Local macOS build (without Xcode)

Requires: `cmake`, `jq` (install via `brew install cmake jq`).

**1. Download deps and clone OBS source (first time only):**
```bash
./.github/scripts/build-macos.zsh --skip-build
```
This downloads pre-built deps to `../obs-build-dependencies/` and clones OBS source to `../obs-studio/`. The OBS source build will fail without full Xcode -- that's expected, skip it.

**2. Create cmake config for obs-frontend-api (first time only):**

The plugin links against OBS.app's installed frameworks. Create this file at `../obs-build-dependencies/plugin-deps-2022-08-02-qt6-arm64/lib/cmake/obs-frontend-api/obs-frontend-apiConfig.cmake`:
```cmake
if(NOT TARGET OBS::obs-frontend-api)
  add_library(OBS::obs-frontend-api SHARED IMPORTED)
  set_target_properties(OBS::obs-frontend-api PROPERTIES
    IMPORTED_LOCATION "/Applications/OBS.app/Contents/Frameworks/libobs-frontend-api.1.dylib"
    INTERFACE_INCLUDE_DIRECTORIES "/Users/derik/Code/obs-studio/UI/obs-frontend-api"
  )
endif()
```

**3. Configure:**
```bash
DEPS_DIR="../obs-build-dependencies/plugin-deps-2022-08-02-qt6-arm64"
CMAKE_PREFIX_PATH="$DEPS_DIR" cmake -S . -B build_arm64 -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DQT_VERSION=6 \
  -DCMAKE_FRAMEWORK_PATH="$DEPS_DIR/Frameworks" \
  -DCMAKE_OSX_SYSROOT="/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk" \
  -Wno-dev
```
Note: Use a macOS 15.x SDK (not 26.x) -- the pre-built Qt6 deps require the AGL framework which was removed in the macOS 26 SDK.

**4. Build:**
```bash
cmake --build build_arm64 --config RelWithDebInfo
```

**5. Install to OBS:**
```bash
cp -r build_arm64/pmg-record.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

**Rebuild shortcut (steps 4+5):**
```bash
cmake --build build_arm64 --config RelWithDebInfo && cp -r build_arm64/pmg-record.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

### CI build (all platforms)

Push to `master` or open a PR against `master` to trigger GitHub Actions. Builds macOS (x86_64, arm64, universal), Linux, and Windows (x64, x86). Artifacts are downloadable from the Actions tab.

### In-tree build (inside OBS source)

Check out to `UI/frontend-plugins/pmg-record`, add `add_subdirectory(pmg-record)` to `UI/frontend-plugins/CMakeLists.txt`, rebuild OBS.

### Format check
```bash
./.github/scripts/check-format.sh
```
Uses clang-format with 132-column limit (config in `.clang-format`).

## Architecture

The plugin lives in two source files plus a vendored header:

- **pmg-record.cpp** -- All plugin logic: signal handlers, file renaming, remuxing, config, menus, UI manipulation, WebSocket API
- **pmg-record.hpp** -- Two Qt dialog classes: `RenameFileDialog` (filename input) and `FilenameFormatDialog` (format string editor with autocomplete)
- **obs-websocket-api.h** -- Vendored obs-websocket header for the vendor API (do not modify)

Other key files:
- **data/locale/en-US.ini** -- All user-facing strings (use `obs_module_text()` to reference them)
- **data/images/pmg-logo.svg** -- Logo displayed in the controls area
- **version.h.in** -- CMake template that generates `version.h` with `PROJECT_VERSION`

### Signal-Driven Pipeline

1. `obs_module_load()` creates Tools menu items and starts a QTimer that calls `loadOutputs()` every 10 seconds to connect signal handlers to active OBS outputs
2. When recording stops or replay buffer saves, OBS fires signals ("stop"/"saved"/"file_changed") which invoke `record_stop()`, `replay_saved()`, or `file_changed()`
3. These handlers use `obs_queue_task()` to schedule UI work on the main thread, calling `ask_rename_file_UI()` or `ask_rename_files_UI()`
4. The rename functions apply filename format templates (including `%TITLE`/`%EXECUTABLE` from hooked game/window capture sources), sanitize invalid characters, show a rename dialog (if user confirmation is enabled), and rename the file
5. If auto-remux is enabled and the file isn't already MP4, a pthread is spawned to remux via `media_remux_job_create()`

### UI Manipulation (Capture Mode)

The plugin heavily manipulates the OBS main window via Qt object names:
- `apply_controls_visibility()` -- Hides stream/vcam/studio-mode buttons, scenes/transitions docks based on settings
- `apply_capture_layout()` -- Rearranges docks (controls right, mixer below, sources tabbed) on first load
- `apply_dock_lock()` -- Disables all dock features (move/close/float) when lock is enabled
- `apply_record_button_style()` -- Applies red background during recording
- `add_pmg_logo()` -- Inserts PMG logo above the record button

These find widgets by `objectName` (e.g., `"recordButton"`, `"streamButton"`, `"scenesDock"`). Changes to OBS's internal widget names will break these.

### Key Patterns

- **Threading:** All UI must go through `obs_queue_task(OBS_TASK_UI, ...)`. Remuxing runs on separate pthreads to avoid blocking.
- **Split recordings:** The `output_files` map (keyed by `obs_output_t*`) tracks multiple files per output, handled via the "file_changed" signal.
- **Config persistence:** Settings stored in OBS profile config under `[PMGRecord]` section via `config_get_bool()`/`config_set_bool()`. Defaults set in the `OBS_FRONTEND_EVENT_FINISHED_LOADING` handler.
- **WebSocket vendor API:** Registered as "pmg-record" vendor in `obs_module_post_load()`; `vendor_set_filename()` handles `set_filename` requests with optional `force` flag.
- **Frontend events:** `frontend_event()` handles profile changes, recording start/stop, studio mode, and exit. It reloads config and re-applies UI state on profile change or finished loading.
