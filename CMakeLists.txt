# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# Prajna plasmoid, lightweight edition:
#  - engine/  prajna-engine: out-of-process WPE WebKit renderer (C)
#  - plugin/  io.github.postadelmaga.prajna: QML item that shows its frames
#  - contents/ + metadata.json: the plasmoid package
# Engine and plugin are installed INSIDE the package (contents/lib/prajna).
cmake_minimum_required(VERSION 3.22)
project(prajna VERSION 3.1.0 LANGUAGES C CXX)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQmlModule)
include(FeatureSummary)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick)
# QRhi (partial texture uploads) is semi-public: <rhi/qrhi.h> lives in the
# private headers, so the plugin must be rebuilt for each Qt minor release.
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
find_package(KF6WindowSystem 6.0 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0>=2.48 wpe-platform-2.0>=2.48)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

set(PRAJNA_PLASMOID_ID "io.github.postadelmaga.prajna")
set(PRAJNA_PACKAGE_DIR ${KDE_INSTALL_DATADIR}/plasma/plasmoids/${PRAJNA_PLASMOID_ID})
set(PRAJNA_PACKAGE_LIBDIR ${PRAJNA_PACKAGE_DIR}/contents/lib/prajna)

add_subdirectory(engine)
add_subdirectory(plugin)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

# The plasmoid package itself (pure QML + assets + translations).
install(FILES metadata.json DESTINATION ${PRAJNA_PACKAGE_DIR})
install(DIRECTORY contents DESTINATION ${PRAJNA_PACKAGE_DIR})
# Lets website notifications show up without any manual setup.
install(FILES data/prajna.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
