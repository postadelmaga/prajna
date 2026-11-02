/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Layouts

import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami as Kirigami

Rectangle {
    id: findBar

    property bool findBarVisible: false
    property var webviewItem
    property alias findText: findField.text
    property int matches: -1

    signal closeRequested()

    visible: findBarVisible
    height: visible ? findBarRow.height + Kirigami.Units.smallSpacing * 2 : 0
    color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.88)
    radius: Kirigami.Units.cornerRadius
    z: 5

    anchors {
        top: parent.top
        left: parent.left
        right: parent.right
    }

    RowLayout {
        id: findBarRow

        anchors {
            left: parent.left
            right: parent.right
            top: parent.top
            margins: Kirigami.Units.smallSpacing
        }

        spacing: Kirigami.Units.smallSpacing

        PlasmaComponents3.TextField {
            id: findField

            Layout.fillWidth: true
            placeholderText: i18n("Find in page...")
            onTextChanged: {
                findBar.matches = -1;
                if (!webviewItem)
                    return;
                if (text)
                    webviewItem.findText(text);
                else
                    webviewItem.stopFinding();
            }
            onAccepted: if (webviewItem && text) webviewItem.findText(text)
            Keys.onEscapePressed: findBar.closeRequested()
        }

        PlasmaComponents3.Label {
            visible: findField.text !== "" && findBar.matches >= 0
            text: findBar.matches === 0 ? i18n("No matches") : i18np("%1 match", "%1 matches", findBar.matches)
            opacity: 0.7
        }

        PlasmaComponents3.ToolButton {
            icon.name: "go-up"
            display: PlasmaComponents3.AbstractButton.IconOnly
            onClicked: if (webviewItem) webviewItem.findText(findField.text, true)
            PlasmaComponents3.ToolTip.text: i18n("Find previous")
            PlasmaComponents3.ToolTip.visible: hovered
            enabled: findField.text !== ""
        }

        PlasmaComponents3.ToolButton {
            icon.name: "go-down"
            display: PlasmaComponents3.AbstractButton.IconOnly
            onClicked: if (webviewItem) webviewItem.findText(findField.text)
            PlasmaComponents3.ToolTip.text: i18n("Find next")
            PlasmaComponents3.ToolTip.visible: hovered
            enabled: findField.text !== ""
        }

        PlasmaComponents3.ToolButton {
            icon.name: "dialog-close"
            display: PlasmaComponents3.AbstractButton.IconOnly
            PlasmaComponents3.ToolTip.text: i18n("Close")
            PlasmaComponents3.ToolTip.visible: hovered
            onClicked: findBar.closeRequested()
        }
    }

    function focusAndSelect() {
        findField.forceActiveFocus();
        findField.selectAll();
    }

    function clearSearch() {
        findField.text = "";
        if (webviewItem)
            webviewItem.stopFinding();
    }
}
