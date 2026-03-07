#!/bin/bash
set -e

OS="$(uname -s)"

case "$OS" in
    Linux*)
        echo "Installing dependencies for Linux..."
        sudo apt install -y build-essential cmake lcov clang-format gdb
        ;;
    Darwin*)
        echo "Installing dependencies for macOS..."
        brew install cmake gcc lcov clang-format
        xcode-select --install 2>/dev/null || echo "Xcode CLT already installed."
        ;;
    *)
        echo "Unsupported OS: $OS"
        echo "For Windows, use install-deps.ps1 instead."
        exit 1
        ;;
esac

echo "Done."
