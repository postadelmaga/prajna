/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.iconthemes as KIconThemes
import org.kde.kcmutils as KCM

import "../lib/chatai"

KCM.SimpleKCM {
    id: page

    // Icon mode constants (must match CompactRepresentation.qml)
    readonly property int iconModeCustom: 7

    property bool cfg_transparentBackground
    property bool cfg_blurBehind
    property int cfg_iconMode
    property string cfg_customIcon
    property bool cfg_hideHeader
    property bool cfg_autoHideHeader
    property bool cfg_hideNavigationButtons
    property bool cfg_hideKeepOpen
    property bool cfg_hideCloseButton
    property bool cfg_hideHomeButton
    property bool cfg_hideRefreshButton
    property bool cfg_hidePrintButton // the auto-hide toggle (historical name)
    property bool cfg_hideDownloadButton

    // Small grey explanation under a control.
    component Hint: QQC2.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        opacity: 0.7
    }

    PopupBlur {
        id: blurInfo
        visible: false
    }

    Kirigami.FormLayout {
        QQC2.CheckBox {
            Kirigami.FormData.label: i18n("Popup:")
            text: i18n("Transparent page background")
            checked: page.cfg_transparentBackground
            onToggled: page.cfg_transparentBackground = checked
        }
        Hint {
            text: i18n("The site's own background disappears; messages, menus and the text box keep their colours.")
        }

        QQC2.CheckBox {
            text: i18n("Blur what is behind the popup")
            checked: page.cfg_blurBehind
            onToggled: page.cfg_blurBehind = checked
            enabled: blurInfo.available
        }
        Hint {
            visible: blurInfo.available && page.cfg_transparentBackground && !page.cfg_blurBehind
            text: i18n("Without blur the desktop shows through sharply, which can make text harder to read.")
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 22
            type: Kirigami.MessageType.Warning
            visible: !blurInfo.available
            text: i18n("The Blur desktop effect is turned off.")
            actions: Kirigami.Action {
                icon.name: "preferences-desktop-effects"
                text: i18n("Desktop Effects…")
                onTriggered: KCM.KCMLauncher.openSystemSettings("kcm_kwin_effects")
            }
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Panel icon")
        }

        RowLayout {
            Kirigami.FormData.label: i18n("Icon:")

            QQC2.ComboBox {
                model: [
                    i18n("Website icon"),
                    i18n("Adaptive"),
                    i18n("Dark"),
                    i18n("Light"),
                    i18n("Outlined"),
                    i18n("Filled"),
                    i18n("Colorful"),
                    i18n("Custom…")
                ]
                currentIndex: page.cfg_iconMode
                onActivated: index => page.cfg_iconMode = index
            }

            QQC2.Button {
                visible: page.cfg_iconMode === page.iconModeCustom
                icon.name: page.cfg_customIcon || "help-about"
                text: i18n("Choose…")
                onClicked: iconDialog.open()

                KIconThemes.IconDialog {
                    id: iconDialog
                    onIconNameChanged: if (iconName) page.cfg_customIcon = iconName
                }
            }
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Top bar")
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: i18n("Show:")
            model: [i18n("Always"), i18n("When the mouse is near the top"), i18n("Never")]
            currentIndex: page.cfg_hideHeader ? 2 : (page.cfg_autoHideHeader ? 1 : 0)
            onActivated: index => {
                page.cfg_hideHeader = index === 2;
                page.cfg_autoHideHeader = index === 1;
            }
        }
        Hint {
            visible: page.cfg_hideHeader
            text: i18n("Start page, reload and \"keep open\" stay available by right-clicking the panel icon.")
        }

        ColumnLayout {
            Kirigami.FormData.label: i18n("Buttons:")
            enabled: !page.cfg_hideHeader
            spacing: 0

            QQC2.CheckBox {
                text: i18n("Back and forward")
                checked: !page.cfg_hideNavigationButtons
                onToggled: page.cfg_hideNavigationButtons = !checked
            }
            QQC2.CheckBox {
                text: i18n("Keep open")
                checked: !page.cfg_hideKeepOpen
                onToggled: page.cfg_hideKeepOpen = !checked
            }
            QQC2.CheckBox {
                text: i18n("Close")
                checked: !page.cfg_hideCloseButton
                onToggled: page.cfg_hideCloseButton = !checked
            }
        }

        ColumnLayout {
            Kirigami.FormData.label: i18n("In the ⋯ menu:")
            enabled: !page.cfg_hideHeader
            spacing: 0

            QQC2.CheckBox {
                text: i18n("Start page")
                checked: !page.cfg_hideHomeButton
                onToggled: page.cfg_hideHomeButton = !checked
            }
            QQC2.CheckBox {
                text: i18n("Reload")
                checked: !page.cfg_hideRefreshButton
                onToggled: page.cfg_hideRefreshButton = !checked
            }
            QQC2.CheckBox {
                text: i18n("Auto-hide toggle")
                checked: !page.cfg_hidePrintButton
                onToggled: page.cfg_hidePrintButton = !checked
            }
            QQC2.CheckBox {
                text: i18n("Downloads")
                checked: !page.cfg_hideDownloadButton
                onToggled: page.cfg_hideDownloadButton = !checked
            }
        }
    }
}
