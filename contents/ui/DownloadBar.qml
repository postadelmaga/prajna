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

import "../lib/chatai"

Column {
    id: downloadsBar

    property var downloadsModel
    property var webviewItem

    visible: downloadsModel && downloadsModel.count > 0
    spacing: 4

    anchors {
        left: parent.left
        right: parent.right
        bottom: parent.bottom
    }

    Repeater {
        model: downloadsModel

        delegate: Rectangle {
            id: row

            required property int index
            required property int downloadId
            required property int dlState
            required property string fileName
            required property string fullPath
            required property real progress
            required property real receivedBytes
            required property real totalBytes

            readonly property bool inProgress: dlState === WebSurface.DownloadProgress || dlState === WebSurface.DownloadStarted
            readonly property bool finished: dlState === WebSurface.DownloadFinished

            width: parent.width
            height: 40
            color: Kirigami.Theme.backgroundColor
            opacity: 0.95

            RowLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 8

                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: {
                        if (row.finished)
                            return i18n("%1 - Completed", row.fileName);
                        if (row.dlState === WebSurface.DownloadFailed)
                            return i18n("%1 - Failed", row.fileName);
                        if (row.dlState === WebSurface.DownloadCancelled)
                            return i18n("%1 - Cancelled", row.fileName);
                        let size = "";
                        if (row.totalBytes > 0)
                            size = ` (${(row.receivedBytes / 1048576).toFixed(1)}/${(row.totalBytes / 1048576).toFixed(1)} MB)`;
                        return i18n("%1 - %2%%3", row.fileName, Math.round(row.progress * 100), size);
                    }
                }

                PlasmaComponents3.ProgressBar {
                    Layout.fillWidth: true
                    visible: row.inProgress
                    indeterminate: row.totalBytes <= 0
                    from: 0
                    to: 1
                    value: row.progress
                }

                PlasmaComponents3.Button {
                    icon.name: "document-open"
                    visible: row.finished
                    PlasmaComponents3.ToolTip.text: i18n("Open file")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: Qt.openUrlExternally("file://" + row.fullPath)
                }

                PlasmaComponents3.Button {
                    icon.name: "folder-open"
                    visible: row.finished
                    PlasmaComponents3.ToolTip.text: i18n("Open folder")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: Qt.openUrlExternally("file://" + row.fullPath.substring(0, row.fullPath.lastIndexOf("/")))
                }

                PlasmaComponents3.Button {
                    icon.name: "dialog-cancel"
                    visible: row.inProgress
                    PlasmaComponents3.ToolTip.text: i18n("Cancel")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: webviewItem.cancelDownload(row.downloadId)
                }

                PlasmaComponents3.Button {
                    icon.name: "dialog-close"
                    visible: !row.inProgress
                    PlasmaComponents3.ToolTip.text: i18n("Close")
                    PlasmaComponents3.ToolTip.visible: hovered
                    onClicked: downloadsModel.remove(row.index)
                }
            }
        }
    }
}
