# Lithium Engine — Launcher Instructions

Two ways to run the engine. Both work from any folder; nothing installs into the system.

---

## Option 1 — Portable folder (recommended)

```
LithiumEngine/
├── launch_engine.sh
├── Lithium_Engine
├── lib/
├── EngineContent/
└── Content/
```

### Grant permissions

The archive does store Unix permissions, and command-line `unzip` restores them. Some
graphical and cross-platform extraction tools discard them, so if you get
`Permission denied`:

```bash
chmod +x launch_engine.sh Lithium_Engine
```

Builds obtained through itch.io's **butler** always have correct permissions.

### Run

```bash
./launch_engine.sh
```

Any arguments are passed through to the engine:

```bash
./launch_engine.sh MyScene.lithium
./launch_engine.sh --opengl
```

### What the launcher checks before starting

1. **Missing shared libraries.** It asks the dynamic loader (`ldd`) what this exact
   binary needs on this exact machine, rather than comparing against a fixed list.
2. **OpenGL driver.** SDL loads the driver at runtime, so it never appears in `ldd`
   and is checked separately. A missing driver is the most common first-run failure.
3. **Bundled vs. system libraries.** Anything shipped inside `lib/` cannot be
   installed from a repository, so if one is absent the launcher reports the download
   as incomplete instead of sending you after a package that does not exist.
4. **Package manager.** Detects `apt-get`, `pacman`, `dnf`, `yum`, `zypper` or `apk`,
   prints the exact install command, and offers to run it.
5. **Re-check.** After any install it verifies again and refuses to launch if
   something is still missing, rather than dropping you into a loader crash.

Sample output when a dependency is absent:

```
Lithium Engine - checking system dependencies...

The following libraries are missing on this system:
   - libSDL2-2.0.so.0

Detected package manager: apt-get
Suggested command:
    sudo apt-get install -y libsdl2-2.0-0

Install these now? [y/N]
```

The prompt only appears when a terminal is attached. Launched from a file manager it
prints the command and continues without hanging on input nobody can supply.

---

## Option 2 — AppImage (no extraction)

```bash
chmod +x Lithium_Engine-x86_64.AppImage
./Lithium_Engine-x86_64.AppImage
```

Self-contained: engine, assets, and the Embree/SYCL/TBB runtime are inside the single
file. It still uses the host's graphics driver, which is deliberate — see
`Engine_Limitations.md`.

If it will not start, some minimal systems lack FUSE:

```bash
# Debian / Ubuntu
sudo apt-get install -y libfuse2

# Or run without FUSE entirely
./Lithium_Engine-x86_64.AppImage --appimage-extract-and-run
```

---

## Choosing between them

| | Portable folder | AppImage |
| --- | --- | --- |
| Dependency help | Yes, with install prompts | No |
| Needs FUSE | No | Yes (or `--appimage-extract-and-run`) |
| Edit/replace assets | Yes | No, image is read-only |
| Add your own sky HDRIs | Yes, drop into `EngineContent/Skies` | No |
| Single file | No | Yes |

Use the portable folder for actually working in the engine; the AppImage for handing
someone a build that just runs.

---

## Where files are written

The engine anchors its working directory to the executable at startup, so it always
finds its own assets no matter where you launch it from. Runtime files written next
to the binary:

- `LithiumEngine_Startup.log` — all engine output; the first thing to check.
- `engine_config.json` — your settings, including Options and Developer preferences.
- `Content/Bakes/` — baked static lighting.
- `imgui.ini` — editor panel layout.

Inside an AppImage the mount is read-only, so those go to the working directory you
launched from instead.
