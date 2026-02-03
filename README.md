# PMG Record for OBS Studio

Plugin for OBS Studio that simplifies OBS into a capture-focused tool. Prompts to rename recordings and strips the UI down to just the essentials.

Based on [Record Rename](https://obsproject.com/forum/resources/record-rename.2134/) by Exeldro.

## Features

All settings are accessible via **Tools > PMG Record** in OBS.

### Capture Mode (on by default)
Simplifies the OBS interface for a recording-only workflow:
- Hides the Scenes dock and Scene Transitions dock
- Disables Studio Mode (prevents preview/program split)
- Leaves the preview, sources, audio mixer, and record controls visible

### Hide Non-Record Controls (on by default)
Removes streaming and virtual camera UI elements:
- Hides the Start Streaming button
- Hides the Start Virtual Camera button and its gear icon
- Hides the Studio Mode toggle button

### Recording Rename
- **Rename on Record** — prompt to rename recordings when they finish
- **User Confirmation** — show a rename dialog before renaming (on by default)
- **Filename Format** — customize the filename template with autocomplete (supports `%TITLE` and `%EXECUTABLE` from hooked game/window capture sources)
- **Auto-Remux to MP4** — automatically remux recordings to MP4 after saving

### WebSocket API
Exposes a `pmg-record` vendor for [obs-websocket](https://github.com/obsproject/obs-websocket), allowing external tools to set the next filename via a `set_filename` request.

## Build
- Build OBS Studio: https://obsproject.com/wiki/Install-Instructions
- Check out this repository to UI/frontend-plugins/pmg-record
- Add `add_subdirectory(pmg-record)` to UI/frontend-plugins/CMakeLists.txt
- Rebuild OBS Studio
