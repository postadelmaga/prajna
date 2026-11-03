# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
#
# The plugin is not installed into the global QML import path: it ships inside
# the plasmoid package (contents/lib/prajna/, imported as "../lib/prajna"),
# together with the engine, so a per-user install needs no root and no
# QML_IMPORT_PATH tweaks.
ecm_add_qml_module(prajnaplugin URI "io.github.postadelmaga.prajna" GENERATE_PLUGIN_SOURCE)

target_sources(prajnaplugin PRIVATE websurface.cpp websurface.h frametexture.cpp frametexture.h popupblur.cpp popupblur.h)
target_compile_definitions(prajnaplugin PRIVATE
    PRAJNA_ENGINE_BUILD_PATH="$<TARGET_FILE:prajna-engine>")
target_link_libraries(prajnaplugin PRIVATE Qt::Quick Qt::Qml Qt::Gui Qt::GuiPrivate KF6::WindowSystem PkgConfig::XKBCOMMON ${CMAKE_DL_LIBS})
add_dependencies(prajnaplugin prajna-engine)

install(TARGETS prajnaplugin LIBRARY DESTINATION ${PRAJNA_PACKAGE_LIBDIR})
install(FILES
    ${CMAKE_BINARY_DIR}/bin/io/github/postadelmaga/prajna/qmldir
    ${CMAKE_BINARY_DIR}/bin/io/github/postadelmaga/prajna/prajnaplugin.qmltypes
    DESTINATION ${PRAJNA_PACKAGE_LIBDIR})
