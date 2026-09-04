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

## Optional: TESLA denoising

TESLA's path tracer is unbiased, so its error appears as noise rather than as a
wrong image - and halving that noise costs four times the samples. Intel Open
Image Denoise buys most of it back for a fixed cost.

It is optional and not vendored (the SDK is a 57 MB download). To enable it:

```sh
cd thirdparty
curl -L -o oidn.tar.gz https://github.com/RenderKit/oidn/releases/download/v2.3.0/oidn-2.3.0.x86_64.linux.tar.gz
tar xzf oidn.tar.gz && mv oidn-2.3.0.x86_64.linux oidn && rm oidn.tar.gz
cd ../build && cmake .. && make
```

CMake reports `Open Image Denoise found - TESLA denoising enabled` when it picks
it up. Without it the engine builds and runs exactly as before, with the option
greyed out in **Rendering -> TESLA Settings**.

The engine feeds the filter first-hit albedo and shading normals alongside the
radiance estimate, which is what lets it keep edges the noisy estimate cannot
resolve on its own. Measured on a synthetic step edge, that is a 6.1x noise
reduction against 5.0x for colour alone - and since noise falls as the square
root of sample count, 6.1x is worth roughly 37x the samples.

The accumulation buffer is never modified, so the underlying estimate stays
unbiased and sampling continues from where it was; the denoiser only changes the
image being displayed.

## License

Lithium Engine is released under the **Mozilla Public License 2.0**. See
[LICENSE](LICENSE) for the full text.

MPL-2.0 is a middle ground between MIT and the GPL, chosen deliberately. It is
*file-level* copyleft:

- **Your game stays yours.** Building a game on Lithium - including a commercial,
  closed-source one - carries no obligation to publish your own code. Your game
  logic, art, audio and levels are yours, under whatever terms you like.
- **Improvements to the engine come back.** If you modify a file that is part of
  Lithium and then distribute the result, that file has to stay under MPL-2.0 and
  its source has to be available. You cannot take the engine, improve it, and
  ship the improved engine as a closed product.

The line is drawn per *file*, not by linking. A new file you write is yours even
if it sits in this tree and calls straight into the engine; a Lithium file you
edited is not. That is what makes MPL workable here where the LGPL would not be:
`Lithium_Game` is compiled from the same sources as the editor, so a shipped game
is a copy of the engine rather than a program linking a library, and the LGPL's
relinking requirement has no sensible meaning in that shape.

In practice: **make whatever you want with it, and if you fix a bug in the engine
itself, share the fix.**

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

All of these are permissive and combine freely with MPL-2.0. The obligation they
carry is attribution: bundle their notices with a shipped game, which a
`THIRD-PARTY.txt` in the game folder satisfies.
