# Lithium Engine

A powerful, high-performance custom game engine built in C++. It features an interactive editor, advanced rendering (OpenGL & Vulkan stubs), Native C++ Scripting, C-Minus visual scripting, physics, audio, and more.

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
You can force the engine to start with a specific graphics API by launching it from the terminal with the following flags:
- `./Lithium_Engine-x86_64.AppImage --opengl` : Forces the OpenGL Renderer (Default)
- `./Lithium_Engine-x86_64.AppImage --vulkan` : Forces the Vulkan Renderer

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
