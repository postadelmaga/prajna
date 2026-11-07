# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
add_executable(websurface_input_test websurface_input_test.cpp)
target_link_libraries(websurface_input_test PRIVATE Qt::Test Qt::Quick Qt::Qml Qt::GuiPrivate PkgConfig::XKBCOMMON)
target_compile_definitions(websurface_input_test PRIVATE PRAJNA_QML_IMPORT_DIR="${CMAKE_BINARY_DIR}/bin")
add_dependencies(websurface_input_test prajnaplugin prajna-engine)
add_test(NAME websurface_input_test COMMAND websurface_input_test)
set_tests_properties(websurface_input_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;PRAJNA_SLOT_RELEASE_DELAY_S=1" TIMEOUT 120)
# The same on a Wayland session, where Qt Quick uses the RHI and the frames
# go through FrameTexture's partial uploads (offscreen has no GPU scene
# graph). Skipped without a compositor, e.g. in CI.
add_test(NAME websurface_input_test_rhi
         COMMAND sh -c "[ -n \"$WAYLAND_DISPLAY\" ] || exit 77; exec \"$1\"" sh $<TARGET_FILE:websurface_input_test>)
set_tests_properties(websurface_input_test_rhi PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=wayland;PRAJNA_SLOT_RELEASE_DELAY_S=1;PRAJNA_TEST_REQUIRE_RHI=1"
    SKIP_RETURN_CODE 77 TIMEOUT 120)

add_executable(transparent_background_test transparent_background_test.cpp)
target_link_libraries(transparent_background_test PRIVATE Qt::Test Qt::Quick Qt::Qml)
target_compile_definitions(transparent_background_test PRIVATE PRAJNA_QML_IMPORT_DIR="${CMAKE_BINARY_DIR}/bin")
add_dependencies(transparent_background_test prajnaplugin prajna-engine)
add_test(NAME transparent_background_test COMMAND transparent_background_test)
set_tests_properties(transparent_background_test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 120)

# Rendering cost on the real desktop (host process + KWin); opens a window,
# so it is a tool, not a test. See the header of desktop_bench.cpp.
add_executable(prajna-desktop-bench desktop_bench.cpp)
target_link_libraries(prajna-desktop-bench PRIVATE Qt::Test Qt::Quick Qt::Qml Qt::GuiPrivate KF6::WindowSystem)
target_compile_definitions(prajna-desktop-bench PRIVATE PRAJNA_QML_IMPORT_DIR="${CMAKE_BINARY_DIR}/bin")
add_dependencies(prajna-desktop-bench prajnaplugin prajna-engine)
