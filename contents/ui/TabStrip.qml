/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Layouts

import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

// The open tabs, shown only when there is more than one. Click to switch,
// middle-click or × to close, + for a new tab with the start page.
Item {
    id: strip

    property var webviewItem
    readonly property var tabs: webviewItem ? webviewItem.tabs : []
    readonly property int current: webviewItem ? webviewItem.currentTab : -1

    signal newTabRequested()

    implicitHeight: Kirigami.Units.gridUnit + Kirigami.Units.smallSpacing * 3

    function hostOf(url) {
        const s = url ? url.toString() : "";
        return s.replace(/^[a-z]+:\/\//i, "").split(/[/?#]/)[0].replace(/^www\./, "");
    }

    RowLayout {
        anchors.fill: parent
        spacing: Kirigami.Units.smallSpacing

        ListView {
            id: list

            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: ListView.Horizontal
            spacing: Kirigami.Units.smallSpacing / 2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: strip.tabs
            currentIndex: strip.current
            highlightFollowsCurrentItem: false
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            // Equal widths that shrink with more tabs, within limits.
            readonly property real tabWidth: Math.max(Kirigami.Units.gridUnit * 4,
                Math.min(Kirigami.Units.gridUnit * 10, (width - spacing * (count - 1)) / Math.max(1, count)))

            delegate: PlasmaComponents3.AbstractButton {
                id: tab

                required property var modelData
                required property int index
                readonly property bool isCurrent: index === strip.current
                readonly property string label: modelData.title || strip.hostOf(modelData.url) || i18n("New Tab")

                width: list.tabWidth
                height: list.height
                hoverEnabled: true

                PlasmaComponents3.ToolTip.text: modelData.title ? modelData.title + "\n" + strip.hostOf(modelData.url) : strip.hostOf(modelData.url)
                PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
                PlasmaComponents3.ToolTip.visible: hovered && !closeButton.hovered

                onClicked: strip.webviewItem.switchTab(index)

                // middle click closes
                TapHandler {
                    acceptedButtons: Qt.MiddleButton
                    onTapped: strip.webviewItem.closeTab(tab.index)
                }

                background: Rectangle {
                    radius: Kirigami.Units.cornerRadius
                    color: tab.isCurrent ? Qt.alpha(Kirigami.Theme.textColor, 0.13)
                         : tab.hovered ? Qt.alpha(Kirigami.Theme.textColor, 0.06) : "transparent"
                    border.width: tab.isCurrent ? 1 : 0
                    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.10)

                    Behavior on color {
                        ColorAnimation { duration: Kirigami.Units.shortDuration }
                    }
                }

                contentItem: RowLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        visible: tab.modelData.popup
                        source: "window-new"
                        Layout.leftMargin: Kirigami.Units.smallSpacing
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    PlasmaComponents3.Label {
                        Layout.fillWidth: true
                        Layout.leftMargin: tab.modelData.popup ? 0 : Kirigami.Units.smallSpacing * 2
                        text: tab.label
                        elide: Text.ElideRight
                        font: Kirigami.Theme.smallFont
                        opacity: tab.isCurrent ? 1 : 0.75
                    }
                    PlasmaComponents3.ToolButton {
                        id: closeButton
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small + Kirigami.Units.smallSpacing * 2
                        Layout.preferredHeight: Layout.preferredWidth
                        icon.name: "window-close-symbolic"
                        icon.width: Kirigami.Units.iconSizes.small
                        icon.height: Kirigami.Units.iconSizes.small
                        display: PlasmaComponents3.AbstractButton.IconOnly
                        text: i18n("Close Tab")
                        // always reachable on the current tab, on hover elsewhere
                        opacity: tab.isCurrent || tab.hovered ? 1 : 0
                        enabled: opacity > 0
                        onClicked: strip.webviewItem.closeTab(tab.index)
                    }
                }
            }
        }

        PlasmaComponents3.ToolButton {
            Layout.preferredHeight: list.height
            Layout.preferredWidth: list.height
            icon.name: "list-add"
            display: PlasmaComponents3.AbstractButton.IconOnly
            text: i18n("New Tab (Ctrl+T)")
            PlasmaComponents3.ToolTip.text: text
            PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
            PlasmaComponents3.ToolTip.visible: hovered
            onClicked: strip.newTabRequested()
        }
    }
}
