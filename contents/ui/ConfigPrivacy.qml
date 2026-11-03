/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtCore
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// What websites may do, where files go, where the session is stored.
KCM.SimpleKCM {
    id: page

    property bool cfg_notificationsEnabled
    property bool cfg_microphoneEnabled
    property bool cfg_webcamEnabled
    property bool cfg_screenShareEnabled
    property bool cfg_geolocationEnabled
    property bool cfg_javascriptCanAccessClipboard
    property bool cfg_javascriptCanOpenWindows
    property bool cfg_playbackRequiresUserGesture
    property string cfg_downloadPath
    property string cfg_webEngineProfileName

    readonly property string dataRoot: StandardPaths.writableLocation(StandardPaths.GenericDataLocation).toString().replace(/^file:\/\//, "") + "/prajna/"
    readonly property string profileName: cfg_webEngineProfileName.trim() || "default"
    readonly property string downloadFolder: (cfg_downloadPath || StandardPaths.writableLocation(StandardPaths.DownloadLocation).toString()).replace(/^file:\/\//, "")

    component Hint: QQC2.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        opacity: 0.7
    }

    Kirigami.FormLayout {
        ColumnLayout {
            Kirigami.FormData.label: i18n("Websites may:")
            spacing: 0

            QQC2.CheckBox {
                text: i18n("Show notifications")
                checked: page.cfg_notificationsEnabled
                onToggled: page.cfg_notificationsEnabled = checked
            }
            QQC2.CheckBox {
                text: i18n("Use the microphone")
                checked: page.cfg_microphoneEnabled
                onToggled: page.cfg_microphoneEnabled = checked
            }
            QQC2.CheckBox {
                text: i18n("Use the camera")
                checked: page.cfg_webcamEnabled
                onToggled: page.cfg_webcamEnabled = checked
            }
            QQC2.CheckBox {
                text: i18n("Share the screen")
                checked: page.cfg_screenShareEnabled
                onToggled: page.cfg_screenShareEnabled = checked
            }
            QQC2.CheckBox {
                text: i18n("Know your location")
                checked: page.cfg_geolocationEnabled
                onToggled: page.cfg_geolocationEnabled = checked
            }
            QQC2.CheckBox {
                text: i18n("Read the clipboard (\"paste\" buttons)")
                checked: page.cfg_javascriptCanAccessClipboard
                onToggled: page.cfg_javascriptCanAccessClipboard = checked
            }
            QQC2.CheckBox {
                text: i18n("Open windows by themselves")
                checked: page.cfg_javascriptCanOpenWindows
                onToggled: page.cfg_javascriptCanOpenWindows = checked
            }
            QQC2.CheckBox {
                text: i18n("Play audio and video without a click")
                checked: !page.cfg_playbackRequiresUserGesture
                onToggled: page.cfg_playbackRequiresUserGesture = !checked
            }
        }
        Hint {
            text: i18n("Login windows (\"Sign in with Google…\") always open inside the popup.")
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Files and session")
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Downloads:")

            QQC2.TextField {
                Layout.minimumWidth: Kirigami.Units.gridUnit * 14
                text: page.downloadFolder
                onEditingFinished: page.cfg_downloadPath = text.trim()
            }
            QQC2.Button {
                icon.name: "document-open-folder"
                display: QQC2.AbstractButton.IconOnly
                text: i18n("Choose folder…")
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                onClicked: folderDialog.open()
            }
            FolderDialog {
                id: folderDialog
                currentFolder: "file://" + page.downloadFolder
                onAccepted: page.cfg_downloadPath = selectedFolder.toString().replace(/^file:\/\//, "")
            }
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Profile:")

            QQC2.TextField {
                Layout.minimumWidth: Kirigami.Units.gridUnit * 10
                placeholderText: "default"
                text: page.cfg_webEngineProfileName
                onEditingFinished: page.cfg_webEngineProfileName = text.trim() || "default"
            }
            QQC2.Button {
                icon.name: "folder-open"
                display: QQC2.AbstractButton.IconOnly
                text: i18n("Open profile folder")
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                onClicked: Qt.openUrlExternally("file://" + page.dataRoot + page.profileName)
            }
        }
        Hint {
            text: i18n("Logins and cookies are kept in %1. Give each widget its own name to keep separate accounts.", page.dataRoot + page.profileName)
        }
    }
}
