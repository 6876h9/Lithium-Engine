#!/usr/bin/env bash
#
# Lithium Engine - portable Linux launcher
#
# Checks that the host actually has the shared libraries the engine needs, offers to
# install anything missing through whichever package manager the distro uses, and then
# starts the engine.
#
# Everything is resolved relative to this script, so the folder can be extracted
# anywhere and run without installation.

set -o pipefail

# ---------------------------------------------------------------------------
# Locate ourselves. Follows symlinks so a link in ~/bin still finds the payload.
# ---------------------------------------------------------------------------
SOURCE="${BASH_SOURCE[0]}"
while [ -L "$SOURCE" ]; do
    DIR="$(cd -P "$(dirname "$SOURCE")" && pwd)"
    SOURCE="$(readlink "$SOURCE")"
    [[ "$SOURCE" != /* ]] && SOURCE="$DIR/$SOURCE"
done
APP_DIR="$(cd -P "$(dirname "$SOURCE")" && pwd)"

BINARY="$APP_DIR/Lithium_Engine"
BUNDLED_LIBS="$APP_DIR/lib"

# Bundled libraries take precedence, but the system's own graphics drivers must win
# for GL - never ship those.
export LD_LIBRARY_PATH="$BUNDLED_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

C_RED=$'\033[1;31m'; C_YEL=$'\033[1;33m'; C_GRN=$'\033[1;32m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'

say()  { printf '%s\n' "$*"; }
warn() { printf '%s%s%s\n' "$C_YEL" "$*" "$C_OFF"; }
err()  { printf '%s%s%s\n' "$C_RED" "$*" "$C_OFF" >&2; }
ok()   { printf '%s%s%s\n' "$C_GRN" "$*" "$C_OFF"; }

if [ ! -x "$BINARY" ]; then
    err "Lithium_Engine not found next to this script ($APP_DIR)."
    err "Extract the whole archive and keep the files together."
    exit 1
fi

# ---------------------------------------------------------------------------
# Which libraries are actually missing?
#
# Asking the loader is more reliable than checking a hand-written list: ldd reports
# exactly what this binary needs on this machine, including transitive dependencies,
# and stays correct if the engine's dependencies change.
# ---------------------------------------------------------------------------
detect_missing() {
    if ! command -v ldd >/dev/null 2>&1; then
        warn "ldd not available - skipping the dependency check."
        return 0
    fi
    LD_LIBRARY_PATH="$BUNDLED_LIBS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        ldd "$BINARY" 2>/dev/null | awk '/not found/ {print $1}' | sort -u
}

# A GL driver is loaded at runtime by SDL, so it never shows up in ldd. Check for it
# separately - a missing driver is the single most common failure on a fresh install.
opengl_present() {
    { ldconfig -p 2>/dev/null | grep -qE 'libGL\.so\.1|libGLX\.so\.0'; } && return 0
    for d in /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu; do
        [ -e "$d/libGL.so.1" ] && return 0
    done
    return 1
}

# ---------------------------------------------------------------------------
# Map a missing soname to a package name, per distro family.
# ---------------------------------------------------------------------------
package_for() {
    local soname="$1" family="$2"
    case "$family" in
      debian)
        case "$soname" in
          libGL.so*|libGLX.so*|libOpenGL.so*|libGLdispatch.so*) echo "libgl1 libglx-mesa0 libglvnd0" ;;
          libX11.so*)            echo "libx11-6" ;;
          libX11-xcb.so*)        echo "libx11-xcb1" ;;
          libxcb*.so*)           echo "libxcb1" ;;
          libwayland-*.so*)      echo "libwayland-client0 libwayland-cursor0 libwayland-egl1" ;;
          libSDL2*.so*)          echo "libsdl2-2.0-0" ;;
          libasound.so*)         echo "libasound2" ;;
          libpulse*.so*)         echo "libpulse0" ;;
          libstdc++.so*)         echo "libstdc++6" ;;
          libgcc_s.so*)          echo "libgcc-s1" ;;
          libdrm.so*)            echo "libdrm2" ;;
          libgbm.so*)            echo "libgbm1" ;;
          libdecor-0.so*)        echo "libdecor-0-0" ;;
          *)                     echo "" ;;
        esac ;;
      arch)
        case "$soname" in
          libGL.so*|libGLX.so*|libOpenGL.so*|libGLdispatch.so*) echo "libglvnd mesa" ;;
          libX11*.so*)           echo "libx11" ;;
          libxcb*.so*)           echo "libxcb" ;;
          libwayland-*.so*)      echo "wayland" ;;
          libSDL2*.so*)          echo "sdl2" ;;
          libasound.so*)         echo "alsa-lib" ;;
          libpulse*.so*)         echo "libpulse" ;;
          libstdc++.so*|libgcc_s.so*) echo "gcc-libs" ;;
          libdrm.so*)            echo "libdrm" ;;
          libgbm.so*)            echo "mesa" ;;
          libdecor-0.so*)        echo "libdecor" ;;
          *)                     echo "" ;;
        esac ;;
      fedora)
        case "$soname" in
          libGL.so*|libGLX.so*|libOpenGL.so*|libGLdispatch.so*) echo "libglvnd-glx mesa-libGL" ;;
          libX11*.so*)           echo "libX11" ;;
          libxcb*.so*)           echo "libxcb" ;;
          libwayland-*.so*)      echo "wayland-libs" ;;
          libSDL2*.so*)          echo "SDL2" ;;
          libasound.so*)         echo "alsa-lib" ;;
          libpulse*.so*)         echo "pulseaudio-libs" ;;
          libstdc++.so*)         echo "libstdc++" ;;
          libgcc_s.so*)          echo "libgcc" ;;
          libdrm.so*)            echo "libdrm" ;;
          libgbm.so*)            echo "mesa-libgbm" ;;
          libdecor-0.so*)        echo "libdecor" ;;
          *)                     echo "" ;;
        esac ;;
      suse)
        case "$soname" in
          libGL.so*|libGLX.so*|libOpenGL.so*|libGLdispatch.so*) echo "Mesa-libGL1 libglvnd" ;;
          libX11*.so*)           echo "libX11-6" ;;
          libSDL2*.so*)          echo "libSDL2-2_0-0" ;;
          libasound.so*)         echo "alsa" ;;
          libpulse*.so*)         echo "libpulse0" ;;
          libstdc++.so*)         echo "libstdc++6" ;;
          *)                     echo "" ;;
        esac ;;
      alpine)
        case "$soname" in
          libGL.so*|libGLX.so*|libOpenGL.so*) echo "mesa-gl libglvnd" ;;
          libX11*.so*)           echo "libx11" ;;
          libSDL2*.so*)          echo "sdl2" ;;
          libstdc++.so*|libgcc_s.so*) echo "libstdc++" ;;
          *)                     echo "" ;;
        esac ;;
      *) echo "" ;;
    esac
}

detect_package_manager() {
    if   command -v apt-get >/dev/null 2>&1; then echo "debian apt-get:install:-y"
    elif command -v pacman  >/dev/null 2>&1; then echo "arch pacman:-S:--needed"
    elif command -v dnf     >/dev/null 2>&1; then echo "fedora dnf:install:-y"
    elif command -v yum     >/dev/null 2>&1; then echo "fedora yum:install:-y"
    elif command -v zypper  >/dev/null 2>&1; then echo "suse zypper:install:-y"
    elif command -v apk     >/dev/null 2>&1; then echo "alpine apk:add:"
    else echo "unknown ::"
    fi
}

# ---------------------------------------------------------------------------
# Run the checks
# ---------------------------------------------------------------------------
say "${C_DIM}Lithium Engine - checking system dependencies...${C_OFF}"

MISSING="$(detect_missing)"
NEED_GL=0
if ! opengl_present; then
    NEED_GL=1
    MISSING="$(printf '%s\nlibGL.so.1' "$MISSING")"
fi
MISSING="$(printf '%s\n' "$MISSING" | sed '/^$/d' | sort -u)"

if [ -z "$MISSING" ]; then
    ok "All dependencies satisfied."
else
    warn ""
    warn "The following libraries are missing on this system:"
    while IFS= read -r lib; do [ -n "$lib" ] && say "   - $lib"; done <<< "$MISSING"
    [ "$NEED_GL" -eq 1 ] && warn "   (no OpenGL driver detected - the engine cannot render without one)"

    read -r FAMILY PM_SPEC <<< "$(detect_package_manager)"
    PM="${PM_SPEC%%:*}"; REST="${PM_SPEC#*:}"; PM_CMD="${REST%%:*}"; PM_FLAGS="${REST#*:}"

    # Split the missing set in two. Anything the engine ships in lib/ cannot be fixed
    # by a package manager - if it is absent the download is incomplete, and telling
    # the user to apt-get it would send them chasing a package that does not exist.
    PKGS=""
    BUNDLED_MISSING=""
    while IFS= read -r lib; do
        [ -z "$lib" ] && continue
        p="$(package_for "$lib" "$FAMILY")"
        if [ -n "$p" ]; then
            PKGS="$PKGS $p"
        else
            BUNDLED_MISSING="$BUNDLED_MISSING $lib"
        fi
    done <<< "$MISSING"
    PKGS="$(printf '%s\n' $PKGS | sort -u | tr '\n' ' ')"

    if [ -n "$(printf '%s' "$BUNDLED_MISSING" | tr -d ' ')" ]; then
        err ""
        err "These libraries ship with the engine but are not present:"
        for lib in $BUNDLED_MISSING; do err "   - $lib"; done
        err ""
        err "The download looks incomplete or the 'lib' folder was moved."
        err "Re-extract the full archive, keeping its folder structure intact."
        exit 1
    fi

    if [ "$FAMILY" = "unknown" ] || [ -z "$(printf '%s' "$PKGS" | tr -d ' ')" ]; then
        err ""
        err "Could not map these to packages for your distribution automatically."
        err "Install the libraries listed above using your package manager, then re-run."
        exit 1
    fi

    INSTALL_CMD="sudo $PM $PM_CMD $PM_FLAGS $PKGS"
    say ""
    say "Detected package manager: ${C_GRN}${PM}${C_OFF}"
    say "Suggested command:"
    say "    ${C_GRN}${INSTALL_CMD}${C_OFF}"
    say ""

    # Only prompt when there is a terminal to answer on; a double-clicked launcher
    # must not hang forever waiting for input nobody can give.
    if [ -t 0 ]; then
        printf 'Install these now? [y/N] '
        read -r reply
        case "$reply" in
            [yY]*)
                say "Running: $INSTALL_CMD"
                # shellcheck disable=SC2086
                if ! $INSTALL_CMD; then
                    err "Installation failed. Install the packages manually and re-run."
                    exit 1
                fi
                ok "Dependencies installed."
                ;;
            *)  warn "Skipping installation - the engine may fail to start." ;;
        esac
    else
        warn "Not running in a terminal; cannot prompt. Run the command above, then retry."
    fi
fi

# ---------------------------------------------------------------------------
# Launch. exec replaces this shell so the engine owns the process and signals.
# ---------------------------------------------------------------------------
# Re-check before launching. Starting the engine when the loader is still going to
# fail just replaces a clear message with a cryptic one.
STILL_MISSING="$(detect_missing)"
if [ -n "$STILL_MISSING" ]; then
    err ""
    err "Still missing after the dependency check:"
    while IFS= read -r lib; do [ -n "$lib" ] && err "   - $lib"; done <<< "$STILL_MISSING"
    err "Install these and run this script again."
    exit 1
fi

say ""
ok "Starting Lithium Engine..."
cd "$APP_DIR" || exit 1
exec "$BINARY" "$@"
