# FastEditor

A blazing-fast, lightweight text editor built with **C++**, **OpenGL**, and **Dear ImGui**. Inspired by VS Code's UX but designed for maximum speed and minimal resource usage.

![C++](https://img.shields.io/badge/C++-17-blue.svg)
![OpenGL](https://img.shields.io/badge/OpenGL-3.0-green.svg)
![Platform](https://img.shields.io/badge/Platform-Windows-lightgrey.svg)
![License](https://img.shields.io/badge/License-MIT-yellow.svg)

## Features

- **Ultra-fast rendering** – GPU-accelerated with OpenGL, only draws visible lines
- **Gap Buffer data structure** – O(1) insertions and deletions
- **VS Code-style interface** – Tabs, menu bar, status bar, and modern dark/light themes
- **Syntax highlighting** – C/C++ keywords, numbers, strings, comments, and preprocessor directives
- **Mouse & keyboard support**
  - Click to place cursor
  - Double-click to select word
  - Click + drag for selection
  - Double-click + drag for word-by-word selection
  - Scroll with mouse wheel
- **Full editing capabilities**
  - Arrow keys, Home/End navigation
  - Ctrl+Arrow for word navigation
  - Ctrl+Backspace to delete word
  - Selection with Shift + arrows
  - Copy, Cut, Paste (Ctrl+C/X/V)
  - Select All (Ctrl+A)
  - Find (Ctrl+F)
- **File operations**
  - New File (Ctrl+N)
  - Open File (Ctrl+O) – native Windows dialog
  - Save (Ctrl+S)
  - Save As (Ctrl+Shift+S)
- **Multiple tabs** – Work with several files simultaneously
- **Light/Dark theme toggle**
- **VSync enabled** – No GPU burning at 1000 FPS

## Screenshots

*(Add screenshots here)*

## Building

### Requirements

- Windows
- MinGW-w64 (comes with CodeBlocks) or any GCC compiler
- OpenGL (already included in Windows)

### Option 1: Build with CodeBlocks

1. Open `FastEditor.cbp` in CodeBlocks
2. Press **F9** to build and run

### Option 2: Build with build.bat

```bash
cd FastEditor
build.bat
```

### Option 3: Manual compile

```bash
g++ -std=c++14 -Iimgui main.cpp imgui/imgui.cpp imgui/imgui_demo.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp imgui/imgui_widgets.cpp imgui/imgui_impl_win32.cpp imgui/imgui_impl_opengl3.cpp -lopengl32 -lgdi32 -ldwmapi -lcomdlg32 -mwindows -o FastEditor.exe
```

## Architecture

```
FastEditor/
├── main.cpp              # Main application, editor logic, rendering
├── imgui/                # Dear ImGui source files
│   ├── imgui.cpp
│   ├── imgui_draw.cpp
│   ├── imgui_widgets.cpp
│   ├── imgui_tables.cpp
│   ├── imgui_impl_win32.cpp
│   └── imgui_impl_opengl3.cpp
├── FastEditor.cbp        # CodeBlocks project file
├── build.bat             # Quick build script
├── .gitignore
└── README.md
```

### Key Design Decisions

| Component | Technology | Why |
|-----------|-----------|-----|
| GUI Framework | Dear ImGui | Immediate mode, zero overhead, perfect for editors |
| Graphics | OpenGL 3 | Fast, portable, hardware-accelerated text rendering |
| Text Buffer | Gap Buffer | O(1) insert/delete, simple and fast for typical editing |
| File Dialogs | Win32 API | Native look and feel on Windows |
| Clipboard | Win32 API | Native system integration |

## Controls

| Shortcut | Action |
|----------|--------|
| `Ctrl+N` | New File |
| `Ctrl+O` | Open File |
| `Ctrl+S` | Save |
| `Ctrl+Shift+S` | Save As |
| `Ctrl+F` | Find |
| `Ctrl+A` | Select All |
| `Ctrl+C` | Copy |
| `Ctrl+X` | Cut |
| `Ctrl+V` | Paste |
| `Ctrl+Backspace` | Delete word left |
| `Ctrl+←/→` | Move by word |
| `Shift+←→↑↓` | Select text |
| `Double Click` | Select word |
| `Double Click + Drag` | Select by words |

## Roadmap

- [ ] Undo/Redo (Ctrl+Z / Ctrl+Y)
- [ ] Drag & drop files
- [ ] Minimap (like VS Code)
- [ ] More language syntax highlighting
- [ ] Auto-indentation
- [ ] Settings/preferences file
- [ ] Cross-platform (Linux, macOS)

## License

MIT License - feel free to use, modify, and distribute.

## Credits

- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut
- Built with ❤️ and C++
