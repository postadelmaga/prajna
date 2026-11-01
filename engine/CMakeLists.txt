# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
add_executable(chatai-webwpe chatai_webwpe.c)
set_target_properties(chatai-webwpe PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_link_libraries(chatai-webwpe PRIVATE PkgConfig::WPE)
install(TARGETS chatai-webwpe DESTINATION ${CHATAI_PACKAGE_LIBDIR})
