/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick

import org.kde.plasma.extras as PlasmaExtras

import "../lib/chatai"

// WPE has no native context menu: the engine reports what was clicked and
// we draw a Plasma menu for it.
PlasmaExtras.Menu {
    id: contextMenu

    property var webviewItem
    property int flags: 0
    property string link: ""
    property string mediaUrl: ""

    readonly property bool editable: flags & WebSurface.ContextEditable
    readonly property bool hasSelection: flags & WebSurface.ContextSelection
    readonly property bool hasLink: link !== ""
    readonly property bool hasMedia: mediaUrl !== "" && !mediaUrl.startsWith("data:") && !mediaUrl.startsWith("blob:")

    signal reloadRequested()

    visualParent: webviewItem

    PlasmaExtras.MenuItem {
        text: i18n("Cut")
        icon: "edit-cut"
        visible: contextMenu.editable && contextMenu.hasSelection
        onClicked: webviewItem.editCommand("Cut")
    }

    PlasmaExtras.MenuItem {
        text: i18n("Copy")
        icon: "edit-copy"
        visible: contextMenu.hasSelection
        onClicked: webviewItem.editCommand("Copy")
    }

    PlasmaExtras.MenuItem {
        text: i18n("Paste")
        icon: "edit-paste"
        visible: contextMenu.editable
        onClicked: webviewItem.editCommand("Paste")
    }

    PlasmaExtras.MenuItem {
        text: i18n("Select All")
        icon: "edit-select-all"
        visible: contextMenu.editable
        onClicked: webviewItem.editCommand("SelectAll")
    }

    PlasmaExtras.MenuItem {
        separator: true
        visible: contextMenu.editable || contextMenu.hasSelection
    }

    PlasmaExtras.MenuItem {
        text: i18n("Open Link in Browser")
        icon: "internet-web-browser"
        visible: contextMenu.hasLink
        onClicked: Qt.openUrlExternally(contextMenu.link)
    }

    PlasmaExtras.MenuItem {
        text: i18n("Copy Link Address")
        icon: "edit-copy"
        visible: contextMenu.hasLink
        onClicked: webviewItem.copyToClipboard(contextMenu.link)
    }

    PlasmaExtras.MenuItem {
        text: i18n("Open Media in Browser")
        icon: "image-x-generic"
        visible: contextMenu.hasMedia
        onClicked: Qt.openUrlExternally(contextMenu.mediaUrl)
    }

    PlasmaExtras.MenuItem {
        text: i18n("Copy Media Address")
        icon: "edit-copy"
        visible: contextMenu.hasMedia
        onClicked: webviewItem.copyToClipboard(contextMenu.mediaUrl)
    }

    PlasmaExtras.MenuItem {
        separator: true
        visible: contextMenu.hasLink || contextMenu.hasMedia
    }

    PlasmaExtras.MenuItem {
        text: i18n("Back")
        icon: "go-previous"
        enabled: webviewItem ? webviewItem.canGoBack : false
        onClicked: webviewItem.goBack()
    }

    PlasmaExtras.MenuItem {
        text: i18n("Forward")
        icon: "go-next"
        enabled: webviewItem ? webviewItem.canGoForward : false
        onClicked: webviewItem.goForward()
    }

    PlasmaExtras.MenuItem {
        text: i18n("Reload")
        icon: "view-refresh"
        onClicked: contextMenu.reloadRequested()
    }

    PlasmaExtras.MenuItem {
        text: i18n("Open Page in Browser")
        icon: "internet-web-browser"
        visible: !contextMenu.hasLink
        onClicked: Qt.openUrlExternally(webviewItem.url)
    }
}
