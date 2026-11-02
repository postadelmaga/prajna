# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
add_executable(websurface_input_test websurface_input_test.cpp)
target_link_libraries(websurface_input_test PRIVATE Qt::Test Qt::Quick Qt::Qml Qt::GuiPrivate PkgConfig::XKBCOMMON)
target_compile_definitions(websurface_input_test PRIVATE CHATAI_QML_IMPORT_DIR="${CMAKE_BINARY_DIR}/bin")
add_dependencies(websurface_input_test chataiwebplugin chatai-webwpe)
add_test(NAME websurface_input_test COMMAND websurface_input_test)
set_tests_properties(websurface_input_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
