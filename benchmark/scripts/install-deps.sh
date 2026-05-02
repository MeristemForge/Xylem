#!/usr/bin/env bash
set -euo pipefail

# Installs benchmark dependencies on Debian/Ubuntu (including WSL).
# All C libraries are built from latest release source with -O3 + LTO.
# Run as: ./install-deps.sh
# If not root, re-execs itself as root (handles WSL passwordless root).

if [ "$(id -u)" -ne 0 ]; then
    echo "[info] not root — re-running as root..."
    exec sudo "$0" "$@"
fi

LIBUV_VERSION="1.49.2"
LIBEVENT_VERSION="2.1.12-stable"
LIBHV_VERSION="1.3.2"
BOOST_VERSION="1.87.0"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="/usr/local"
JOBS="$(nproc)"

info() { printf "\033[1;34m[info]\033[0m %s\n" "$1"; }
ok()   { printf "\033[1;32m[ok]\033[0m %s\n" "$1"; }
warn() { printf "\033[1;33m[warn]\033[0m %s\n" "$1"; }

check_cmd() {
    command -v "$1" >/dev/null 2>&1
}

# --- apt base packages -------------------------------------------------------

APT_PKGS=(
    build-essential
    cmake
    ninja-build
    pkg-config
    autoconf
    automake
    libtool
    libssl-dev
    golang-go
    curl
    git
)

install_apt_deps() {
    local missing=()
    for pkg in "${APT_PKGS[@]}"; do
        if ! dpkg -s "$pkg" >/dev/null 2>&1; then
            missing+=("$pkg")
        fi
    done

    if [ ${#missing[@]} -eq 0 ]; then
        ok "all apt base packages already installed"
    else
        info "installing apt packages: ${missing[*]}"
        apt-get update -qq
        apt-get install -y -qq "${missing[@]}"
        ok "apt packages installed"
    fi
}

# --- rust --------------------------------------------------------------------

install_rust() {
    if check_cmd cargo; then
        ok "rust/cargo already installed ($(cargo --version))"
    else
        info "installing rust via rustup..."
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --quiet
        source "$HOME/.cargo/env"
        ok "rust installed"
    fi
}

# --- libuv (from source) ----------------------------------------------------

install_libuv() {
    if [ -f "$PREFIX/lib/libuv.a" ]; then
        ok "libuv already installed"
        return
    fi

    info "building libuv v${LIBUV_VERSION} from source..."
    local tmpdir
    tmpdir="$(mktemp -d)"

    cd "$tmpdir"
    curl -sSL "https://github.com/libuv/libuv/archive/refs/tags/v${LIBUV_VERSION}.tar.gz" | tar xz
    cd "libuv-${LIBUV_VERSION}"
    mkdir build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-O3 -DNDEBUG -flto" \
        -DBUILD_TESTING=OFF \
        -DLIBUV_BUILD_SHARED=OFF \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
    make -j"$JOBS"
    make install
    ldconfig

    rm -rf "$tmpdir"
    ok "libuv v${LIBUV_VERSION} installed"
}

# --- libevent (from source) --------------------------------------------------

install_libevent() {
    if [ -f "$PREFIX/lib/libevent.a" ]; then
        ok "libevent already installed"
        return
    fi

    info "building libevent v${LIBEVENT_VERSION} from source..."
    local tmpdir
    tmpdir="$(mktemp -d)"

    cd "$tmpdir"
    curl -sSL "https://github.com/libevent/libevent/releases/download/release-${LIBEVENT_VERSION}/libevent-${LIBEVENT_VERSION}.tar.gz" | tar xz
    cd "libevent-${LIBEVENT_VERSION}"
    mkdir build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-O3 -DNDEBUG -flto" \
        -DEVENT__DISABLE_BENCHMARK=ON \
        -DEVENT__DISABLE_TESTS=ON \
        -DEVENT__DISABLE_SAMPLES=ON \
        -DEVENT__LIBRARY_TYPE=STATIC \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
    make -j"$JOBS"
    make install
    ldconfig

    rm -rf "$tmpdir"
    ok "libevent v${LIBEVENT_VERSION} installed"
}

# --- libhv (from source) ----------------------------------------------------

install_libhv() {
    if [ -f "$PREFIX/lib/libhv.a" ] || [ -f "$PREFIX/include/hv/hloop.h" ]; then
        ok "libhv already installed"
        return
    fi

    info "building libhv v${LIBHV_VERSION} from source..."
    local tmpdir
    tmpdir="$(mktemp -d)"

    cd "$tmpdir"
    curl -sSL "https://github.com/ithewei/libhv/archive/refs/tags/v${LIBHV_VERSION}.tar.gz" | tar xz
    cd "libhv-${LIBHV_VERSION}"
    mkdir build && cd build
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-O3 -DNDEBUG -flto" \
        -DWITH_OPENSSL=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_UNITTEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$PREFIX"
    make -j"$JOBS"
    make install
    ldconfig

    rm -rf "$tmpdir"
    ok "libhv v${LIBHV_VERSION} installed"
}

# --- boost (from source, header-only + system lib) ---------------------------

install_boost() {
    if [ -f "$PREFIX/lib/libboost_system.a" ]; then
        ok "boost already installed"
        return
    fi

    info "building boost v${BOOST_VERSION} from source..."
    local tmpdir
    tmpdir="$(mktemp -d)"
    local boost_underscore="${BOOST_VERSION//./_}"

    cd "$tmpdir"
    curl -sSL "https://archives.boost.io/release/${BOOST_VERSION}/source/boost_${boost_underscore}.tar.gz" | tar xz
    cd "boost_${boost_underscore}"

    ./bootstrap.sh --prefix="$PREFIX" --with-libraries=system
    ./b2 install \
        variant=release \
        optimization=speed \
        link=static \
        cxxflags="-O3 -DNDEBUG -flto" \
        linkflags="-flto" \
        -j"$JOBS" \
        --prefix="$PREFIX" 2>/dev/null
    sudo ldconfig

    rm -rf "$tmpdir"
    ok "boost v${BOOST_VERSION} installed"
}

# --- main --------------------------------------------------------------------

main() {
    info "checking benchmark dependencies..."
    echo ""
    install_apt_deps
    install_rust
    install_libuv
    install_libevent
    install_libhv
    install_boost
    echo ""
    ok "all dependencies ready (all built with -O3 + LTO)"
}

main "$@"
