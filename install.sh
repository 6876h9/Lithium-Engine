#!/bin/bash

# Lithium Engine System Installer
# Run this script with sudo to install Lithium Engine system-wide on Linux.

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root (sudo ./install.sh)"
  exit 1
fi

INSTALL_DIR="/opt/lithium_engine"
BIN_LINK="/usr/local/bin/lithium_engine"
DESKTOP_ENTRY="/usr/share/applications/lithium_engine.desktop"

echo "Installing Lithium Engine to $INSTALL_DIR..."

# Create install directory
mkdir -p "$INSTALL_DIR"

# Copy binary and resources
# Ensure the engine was built first
if [ ! -f "build/Lithium_Engine" ]; then
    echo "Error: build/Lithium_Engine not found. Please compile the engine first."
    exit 1
fi

cp build/Lithium_Engine "$INSTALL_DIR/"
cp -r assets "$INSTALL_DIR/" 2>/dev/null || true
cp -r shaders "$INSTALL_DIR/" 2>/dev/null || true

# Set permissions
chmod +x "$INSTALL_DIR/Lithium_Engine"

# Create symlink
echo "Creating symlink at $BIN_LINK..."
ln -sf "$INSTALL_DIR/Lithium_Engine" "$BIN_LINK"

# Create desktop entry
echo "Creating desktop entry..."
cat <<EOF > "$DESKTOP_ENTRY"
[Desktop Entry]
Version=1.0
Name=Lithium Engine
Comment=3D Game Engine
Exec=$INSTALL_DIR/Lithium_Engine
Path=$INSTALL_DIR
Terminal=false
Type=Application
Categories=Development;
EOF

chmod +x "$DESKTOP_ENTRY"

echo "Installation complete! You can now run 'lithium_engine' from the terminal or find it in your application menu."
