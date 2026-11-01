# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# ChatAI plasmoid, lightweight edition:
#  - engine/  chatai-webwpe: out-of-process WPE WebKit renderer (C)
#  - plugin/  org.kde.plasma.private.chatai: QML item that shows its frames
#  - contents/ + metadata.json: the plasmoid package
# Engine and plugin are installed INSIDE the package (contents/lib/chatai).
cmake_minimum_required(VERSION 3.22)
project(chatai-plasmoid VERSION 3.0.0 LANGUAGES C CXX)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQmlModule)
include(FeatureSummary)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WPE REQUIRED IMPORTED_TARGET wpe-webkit-2.0>=2.48 wpe-platform-2.0>=2.48)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

set(CHATAI_PLASMOID_ID "ChatAI-Plasmoid")
set(CHATAI_PACKAGE_DIR ${KDE_INSTALL_DATADIR}/plasma/plasmoids/${CHATAI_PLASMOID_ID})
set(CHATAI_PACKAGE_LIBDIR ${CHATAI_PACKAGE_DIR}/contents/lib/chatai)

add_subdirectory(engine)
add_subdirectory(plugin)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_subdirectory(tests)
endif()

# The plasmoid package itself (pure QML + assets + translations).
install(FILES metadata.json DESTINATION ${CHATAI_PACKAGE_DIR})
install(DIRECTORY contents DESTINATION ${CHATAI_PACKAGE_DIR})

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)
