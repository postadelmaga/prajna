# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
# Embed the page scripts as C byte arrays (re-generated when they change).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/clear_background.js _js HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _js "${_js}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/clear_background_js.h
     "static const char CLEAR_BACKGROUND_JS[] = {${_js} 0};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS clear_background.js)

add_executable(prajna-engine prajna_engine.c)
target_include_directories(prajna-engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
set_target_properties(prajna-engine PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_link_libraries(prajna-engine PRIVATE PkgConfig::WPE)
install(TARGETS prajna-engine DESTINATION ${PRAJNA_PACKAGE_LIBDIR})
