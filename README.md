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
