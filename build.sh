#!/bin/bash
# Build Prajna (engine + QML plugin + package) and pack it as prajna.plasmoid.
#
#   ./build.sh               build, run the tests, create prajna.plasmoid
#   ./build.sh --skip-tests  build and package only
#
# The package contains compiled parts (contents/lib/prajna/: the QML plugin
# and the prajna-engine binary): it runs on systems with the same Qt, KF6 and
# WPE WebKit versions it was built against (e.g. a build on Arch for Arch).

set -e

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
ok()   { echo -e "${GREEN}✓${NC} $*"; }
info() { echo -e "${CYAN}[${1}/4]${NC} ${2}"; }
die()  { echo -e "\n${RED}✗ ERROR:${NC} $*\n" >&2; show_help; exit 1; }

show_help() {
    echo -e "${BOLD}Missing dependencies? On Arch / CachyOS / Manjaro:${NC}"
    echo "    sudo pacman -S --needed base-devel cmake extra-cmake-modules qt6-base qt6-declarative \\"
    echo "        kwindowsystem wpewebkit libxkbcommon zip"
    echo "  WPE WebKit >= 2.48 with the WPE Platform API is required."
}

RUN_TESTS=true
for arg in "$@"; do
    case "$arg" in
        --skip-tests) RUN_TESTS=false ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) die "Unknown argument: ${arg}\nUsage: $0 [--skip-tests]" ;;
    esac
done

cd "$(dirname "$0")"
PROJECT_DIR=$(pwd)
BUILD_DIR="${PROJECT_DIR}/build-release"
STAGE_DIR="${BUILD_DIR}/stage"
PLASMOID_ID=$(sed -n 's/.*"Id": *"\([^"]*\)".*/\1/p' metadata.json)
PLASMOID_FILE="prajna.plasmoid"
[ -n "${PLASMOID_ID}" ] || die "cannot read the plugin Id from metadata.json"

echo -e "${BOLD}Prajna — build & packaging (${PLASMOID_ID})${NC}"

info 1 "Configuring..."
command -v cmake &>/dev/null || die "cmake not found."
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=$([ "${RUN_TESTS}" = true ] && echo ON || echo OFF) \
    || die "CMake configuration failed: check the development packages."

info 2 "Compiling..."
cmake --build "${BUILD_DIR}" -j"$(nproc)" || die "Compilation failed."
ok "Engine and plugin built."

if [ "${RUN_TESTS}" = true ]; then
    info 3 "Running the tests..."
    ctest --test-dir "${BUILD_DIR}" --output-on-failure || die "Tests failed."
    ok "Tests passed."
else
    info 3 "Skipping the tests (--skip-tests)."
fi

info 4 "Creating ${PLASMOID_FILE}..."
command -v zip &>/dev/null || die "'zip' not found."
rm -rf "${STAGE_DIR}" "${PLASMOID_FILE}"
DESTDIR="${STAGE_DIR}" cmake --install "${BUILD_DIR}" >/dev/null
PKG_DIR="${STAGE_DIR}/usr/share/plasma/plasmoids/${PLASMOID_ID}"
[ -x "${PKG_DIR}/contents/lib/prajna/prajna-engine" ] || die "the engine is missing from the package"
# metadata.json at the root of the archive, as kpackagetool expects
(cd "${PKG_DIR}" && zip -qr -X "${PROJECT_DIR}/${PLASMOID_FILE}" .)

SIZE=$(du -h "${PLASMOID_FILE}" | cut -f1)
echo
echo -e "${GREEN}${BOLD}✓ BUILD COMPLETE${NC}  ${PLASMOID_FILE} (${SIZE})"
echo
echo "Install:  kpackagetool6 -t Plasma/Applet -i ${PLASMOID_FILE}"
echo "Upgrade:  kpackagetool6 -t Plasma/Applet -u ${PLASMOID_FILE}"
echo "Remove:   kpackagetool6 -t Plasma/Applet -r ${PLASMOID_ID}"
echo "Then restart Plasma: systemctl --user restart plasma-plasmashell"
