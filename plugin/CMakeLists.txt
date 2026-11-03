# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# The plugin is not installed into the global QML import path: it ships inside
# the plasmoid package (contents/lib/chatai/, imported as "../lib/chatai"),
# together with the engine, so a per-user install needs no root and no
# QML_IMPORT_PATH tweaks.
ecm_add_qml_module(chataiwebplugin URI "org.kde.plasma.private.chatai" GENERATE_PLUGIN_SOURCE)

target_sources(chataiwebplugin PRIVATE websurface.cpp websurface.h frametexture.cpp frametexture.h popupblur.cpp popupblur.h)
target_compile_definitions(chataiwebplugin PRIVATE
    CHATAI_ENGINE_BUILD_PATH="$<TARGET_FILE:chatai-webwpe>")
target_link_libraries(chataiwebplugin PRIVATE Qt::Quick Qt::Qml Qt::Gui Qt::GuiPrivate KF6::WindowSystem PkgConfig::XKBCOMMON ${CMAKE_DL_LIBS})
add_dependencies(chataiwebplugin chatai-webwpe)

install(TARGETS chataiwebplugin LIBRARY DESTINATION ${CHATAI_PACKAGE_LIBDIR})
install(FILES
    ${CMAKE_BINARY_DIR}/bin/org/kde/plasma/private/chatai/qmldir
    ${CMAKE_BINARY_DIR}/bin/org/kde/plasma/private/chatai/chataiwebplugin.qmltypes
    DESTINATION ${CHATAI_PACKAGE_LIBDIR})
