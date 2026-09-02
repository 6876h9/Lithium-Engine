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

Lithium Engine is released under the **MIT License**. See [LICENSE](LICENSE) for
the full text.

In short: use it for anything, including commercial and closed-source work. Keep
the copyright notice somewhere in your distribution and you have met the whole
obligation. There is no requirement to open-source your game, publish your
changes to the engine, or pay anything.

The engine is provided as-is, with no warranty.

### Third-party components

The engine builds against several libraries under their own permissive terms.
They are fetched at configure time or vendored under `thirdparty/`, and are
**not** covered by this project's licence - each carries its own, and each asks
you to preserve its notice when you distribute a build:

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

All of these are permissive, so a game shipped on Lithium can be closed-source.
The only real obligation is attribution: bundle the notices (a `THIRD-PARTY.txt`
in your game folder is the usual way). Embree's Apache-2.0 is the strictest of
them and still only asks for the notice and a statement of changes.
