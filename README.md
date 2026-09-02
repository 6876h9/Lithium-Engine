# Lithium Engine

A custom game engine built in C++20. It features an interactive editor, a deferred PBR renderer (OpenGL 4.5), an offline path tracer, Jolt physics, three scripting paths (Lua, C-Minus visual scripting, and native C++), audio, navigation, and more.

## Getting Started

### Running the Portable Version (Linux)
The portable version is distributed as an AppImage. This means it contains all the necessary libraries and dependencies to run on most modern Linux distributions (Debian, Ubuntu, Arch, Fedora, etc.).

1. **Extract the ZIP file** you downloaded.
2. Inside the `linux` folder, locate the file named `Lithium_Engine-x86_64.AppImage`.
3. **Make the file executable**:
   - Right-click the file, go to **Properties**, and check the box that says "Allow executing file as program" (or similar, depending on your desktop environment).
   - Alternatively, open a terminal in that folder and run: 
     `chmod +x Lithium_Engine-x86_64.AppImage`
4. **Run the Engine**:
   - Double-click the file to open the engine editor.
   - Alternatively, run it from the terminal: 
     `./Lithium_Engine-x86_64.AppImage`

### Command-Line Options
- `--selftest` : Runs the headless subsystem checks and exits with the failure count.
- `--index-assets <dirs...>` : Mints the `.meta` GUID sidecars for a content tree and
  exits. Run this after adding assets, and commit the sidecars - they are what lets a
  scene keep its references when you rename or move a file. Defaults to
  `Content EngineContent`.

The renderer targets the OpenGL 4.5 core profile. There is no second backend; see
`docs/Engine_Limitations.md`.

## Building from Source

To compile the engine yourself, you need CMake and a C++20 compatible compiler.

1. Create a build directory: `mkdir build && cd build`
2. Run CMake: `cmake ..`
3. Compile: `make -j$(nproc)`

### Installing Permanently (Linux)
If you built the engine using CMake, you can generate a `.deb` installer package (on Debian/Ubuntu systems):
1. In the build directory, run: `cpack -G DEB`
2. Install the generated `.deb` file: `sudo dpkg -i Lithium_Engine-1.0.0-Linux.deb`

## Exporting Your Game
To export your game as a standalone executable (without the Editor UI):
1. Open your project in the Lithium Engine Editor.
2. Go to **File > Export Project...** in the top menu bar.
3. Select an empty folder where you want your game to be exported.
4. The engine will compile a standalone executable and package your assets into that directory.

## License

Lithium Engine is free software: you can redistribute it and/or modify it under
the terms of the **GNU General Public License version 3** as published by the
Free Software Foundation. See [LICENSE](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

### What this means for a game you build with it

The GPL is a *copyleft* licence, and that has a real consequence worth stating
plainly rather than leaving for you to discover at release: a game that links
the engine is a derivative work of it, so **shipping that game means offering
its source under the GPL too**. Your own art, audio, levels and other assets are
not affected - they are data the engine loads, not code linked into it.

If you want to build something closed-source on this, the GPL is the wrong
licence for you and you should not use this engine for it.

### Third-party components

The engine builds against several libraries under their own, GPL-compatible
terms. They are fetched at configure time or vendored under `thirdparty/`, and
are **not** covered by this project's licence - each carries its own:

| Component | License |
| --- | --- |
| Dear ImGui, imnodes | MIT |
| Jolt Physics | MIT |
| Lua | MIT |
| miniaudio | MIT / public domain |
| ENet | MIT |
| assimp | BSD-3-Clause |
| SDL2 | zlib |
| Embree | Apache-2.0 |
| stb_image, stb_image_write | MIT / public domain |

All of the above are one-way compatible with GPLv3, which is what makes the
combination distributable. Apache-2.0 in particular is compatible with GPLv3
but *not* with GPLv2, so this project cannot be relicensed downward to v2.
