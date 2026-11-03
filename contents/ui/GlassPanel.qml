/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick

import org.kde.kirigami as Kirigami

// A floating panel over the page (find bar, downloads, login popup bar,
// link preview): rounded, a denser tint of the theme colour than the glass
// behind it, and a hairline edge so it reads as a separate layer.
Rectangle {
    color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.92)
    radius: Kirigami.Units.cornerRadius
    border.width: 1
    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.12)
}
