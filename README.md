# mpvx

A high-performance, modern media player desktop application built with **C++26**, **Slint UI**, and **`libmpv`**.

---

## Features

- **Modern C++26**: Built with the latest C++26 standard (`-std=c++26`, GCC 16+ / Clang 19+), utilizing `std::println`, `<expected>`, concepts, and designated initializers.
- **Slint UI Frontend**: Reactive, declarative, and hardware-accelerated user interface featuring a dark theme, auto-hiding playback controls, progress seeking, and volume control.
- **libmpv Backend**: Robust media playback engine for seamless local and remote video/audio playback.
- **CPM.cmake Package Management**: Integrated CMake Package Manager (CPM) for zero-setup, reproducible dependency resolution and caching with zero external CLI requirements.
- **Standard Tooling**: Clang-Format, Clang-Tidy, and CMake Presets for Debug, Release, and Sanitizers.

---

## Project Structure

```
mpvx/
├── CMakeLists.txt            # Main CMake build definition
├── CMakePresets.json         # Build & test presets (dev-debug, dev-release, dev-sanitize)
├── .clang-format             # Standard formatting rules
├── .clang-tidy               # Static analysis rules
├── cmake/
│   ├── CPM.cmake             # CMake Package Manager
│   ├── Dependencies.cmake    # Dependency declarations (Slint, Catch2, libmpv)
│   └── Findmpv.cmake         # libmpv find module
├── scripts/
│   └── setup-deps.sh         # Dependency helper for libmpv (supports zero-sudo extraction)
├── include/
│   └── mpvx/
│       ├── core.hpp          # Common types, expected/error types
│       ├── player.hpp        # RAII MPV player controller
│       ├── events.hpp        # Event observer signatures
│       └── renderer.hpp      # Slint SharedPixelBuffer video bridge
├── src/
│   ├── main.cpp              # Entry point and UI/player binding
│   ├── player.cpp            # libmpv wrapper implementation
│   └── renderer.cpp          # Slint frame rendering logic
├── ui/
│   ├── app-window.slint      # Root application window
│   ├── theme.slint           # Theme palette & metrics
│   └── components/
│       ├── controls.slint    # Playback control bar
│       └── video-view.slint  # Video viewport & idle placeholder
└── tests/
    ├── CMakeLists.txt        # Test targets using Catch2
    └── test_player.cpp       # Core player unit tests
```

---

## Prerequisites

- **C++ Compiler**: GCC 14+ (tested on GCC 16.2.1) or Clang 18+ supporting C++26.
- **CMake**: Version 3.25 or newer.
- **`libmpv`**:
  - Fedora: `sudo dnf install -y mpv-devel` (or runs automatically via `scripts/setup-deps.sh`).
  - Ubuntu/Debian: `sudo apt-get install -y libmpv-dev`.
  - Arch Linux: `sudo pacman -S mpv`.

---

## Building and Running

### 1. Configure
Using CMake presets:
```bash
cmake --preset dev-debug
```

### 2. Build
```bash
cmake --build --preset dev-debug
```

### 3. Run Unit Tests
```bash
ctest --preset dev-debug
```

### 4. Launch Application
```bash
# Launch player
./build/dev-debug/mpvx

# Or open a media file or URL directly:
./build/dev-debug/mpvx "https://commondatastorage.googleapis.com/gtv-videos-bucket/sample/BigBuckBunny.mp4"
```

---

## License

This project is licensed under the **GNU General Public License v3.0** (`GPL-3.0-or-later`) — see the [LICENSE](LICENSE) file (or <https://www.gnu.org/licenses/gpl-3.0.html>) for details.
