/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick

import org.kde.kirigami as Kirigami

// Glass material behind the popup content, on top of the (KWin-blurred)
// Plasma dialog background:
//   tint   the theme's window colour at `strength` (a dark veil on dark
//          themes, so text stays readable over any wallpaper); a little
//          denser under the top bar
//   grain  a faint noise texture: gives the glass some material and hides
//          banding in the blurred gradients
// Only drawn when the page itself is transparent.
Item {
    id: glass

    property real strength: 0.4      // 0..1
    property bool grain: true
    property real headerHeight: 0
    property bool headerAtBottom: false

    readonly property color tint: Kirigami.Theme.backgroundColor
    readonly property real headerBoost: Math.min(1, strength + 0.12)

    Rectangle {
        anchors.fill: parent
        radius: Kirigami.Units.cornerRadius
        gradient: Gradient {
            GradientStop {
                position: 0
                color: Qt.alpha(glass.tint, glass.headerAtBottom ? glass.strength : glass.headerBoost)
            }
            GradientStop {
                position: glass.height > 0 ? Math.min(0.5, glass.headerHeight / glass.height) : 0
                color: Qt.alpha(glass.tint, glass.strength)
            }
            GradientStop {
                position: glass.height > 0 ? Math.max(0.5, 1 - glass.headerHeight / glass.height) : 1
                color: Qt.alpha(glass.tint, glass.strength)
            }
            GradientStop {
                position: 1
                color: Qt.alpha(glass.tint, glass.headerAtBottom ? glass.headerBoost : glass.strength)
            }
        }
    }

    // hairline between the top bar and the page
    Rectangle {
        visible: glass.headerHeight > 0
        x: Kirigami.Units.smallSpacing
        width: parent.width - 2 * x
        height: 1
        y: glass.headerAtBottom ? parent.height - glass.headerHeight : glass.headerHeight - 1
        color: Qt.alpha(Kirigami.Theme.textColor, 0.10)
    }

    Image {
        anchors.fill: parent
        visible: glass.grain
        source: "assets/grain.png"
        fillMode: Image.Tile
        smooth: false
        opacity: 0.045
    }
}
