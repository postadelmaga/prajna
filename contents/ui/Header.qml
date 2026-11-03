/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

import org.kde.plasma.components 3.0 as PlasmaComponents3
import org.kde.kirigami as Kirigami

import Qt.labs.platform 1.1

import "sites.js" as Sites

RowLayout {
    Layout.fillWidth: true

    // Signals for communication with parent components
    signal goBackToHomePage
    signal reloadPageRequested
    signal navigateBackRequested
    signal navigateForwardRequested
    signal printPageRequested
    signal toggleSearchRequested

    // Properties for managing component state
    property var closeWebViewCallback: undefined    // Callback function for closing webview
    property var models                            // Available chat models
    property var webview: (parent && parent.webviewRoot) ? parent.webviewRoot.webview : null

    spacing: Kirigami.Units.smallSpacing / 2

    // Flat, icon-only header button with a tooltip.
    component HeaderButton: PlasmaComponents3.ToolButton {
        property string tip
        display: PlasmaComponents3.AbstractButton.IconOnly
        z: 3
        PlasmaComponents3.ToolTip.text: tip
        PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
        PlasmaComponents3.ToolTip.visible: hovered && tip !== ""
    }

    // Navigation buttons
    HeaderButton {
        icon.name: "go-previous"
        tip: i18n("Go back to previous page")
        enabled: !webview || webview.canGoBack
        visible: !plasmoid.configuration.hideNavigationButtons
        onClicked: navigateBackRequested()
    }

    HeaderButton {
        icon.name: "go-next"
        tip: i18n("Go forward to next page")
        enabled: !webview || webview.canGoForward
        visible: !plasmoid.configuration.hideNavigationButtons
        onClicked: navigateForwardRequested()
    }

    // Site selector. Entries are {text, url}; the optional last entry
    // ("Custom URL…", empty url) turns the box into an address field.
    PlasmaComponents3.ComboBox {
        id: urlComboBox

        readonly property bool customSelected: currentIndex >= 0 && currentIndex < model.length && model[currentIndex].url === ""

        Layout.fillWidth: true
        flat: !editable
        model: []
        textRole: "text"
        valueRole: "url"
        editable: customSelected
        PlasmaComponents3.ToolTip.text: i18n("Choose the chat site")
        PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
        PlasmaComponents3.ToolTip.visible: hovered && !pressed && !editable

        onActivated: {
            if (customSelected) {
                editText = "";
                Qt.callLater(() => urlComboBox.forceActiveFocus());
            } else if (currentValue) {
                openSite(currentValue);
            }
        }
        onAccepted: {
            if (customSelected && editText.trim())
                openSite(Sites.sanitizeUrl(editText));
        }
        Keys.onEscapePressed: renderChatModel()

        Component.onCompleted: renderChatModel()
    }

    // Search button - Toggles the find bar
    HeaderButton {
        icon.name: "search"
        tip: i18n("Find in page (Ctrl+F)")
        onClicked: toggleSearchRequested()
    }

    // Pin button - Keeps the widget open when focused is lost
    HeaderButton {
        icon.name: "window-pin"
        checkable: true
        checked: Boolean(plasmoid.configuration.pin)
        onToggled: plasmoid.configuration.pin = checked
        visible: !Boolean(plasmoid.configuration.hideKeepOpen)
        tip: checked ? i18n("Widget will stay open when clicking outside") : i18n("Widget will close when clicking outside")
    }

    // Less frequent actions live in an overflow menu to keep the bar light.
    HeaderButton {
        id: moreButton
        icon.name: "overflow-menu"
        tip: i18n("More actions")
        visible: homeItem.visible || reloadItem.visible || autoHideItem.visible || !plasmoid.configuration.hideDownloadButton
        onClicked: moreMenu.popup(moreButton, 0, moreButton.height)

        PlasmaComponents3.Menu {
            id: moreMenu

            PlasmaComponents3.MenuItem {
                id: homeItem
                icon.name: "go-home"
                text: i18n("Return to homepage")
                visible: !plasmoid.configuration.hideHomeButton
                height: visible ? implicitHeight : 0
                onTriggered: goBackToHomePage()
            }
            PlasmaComponents3.MenuItem {
                id: reloadItem
                icon.name: "view-refresh"
                text: i18n("Reload page")
                visible: !plasmoid.configuration.hideRefreshButton
                height: visible ? implicitHeight : 0
                onTriggered: reloadPageRequested()
            }
            PlasmaComponents3.MenuItem {
                id: autoHideItem
                icon.name: "view-visible"
                text: i18n("Auto-hide this bar")
                checkable: true
                checked: plasmoid.configuration.autoHideHeader
                visible: !plasmoid.configuration.hidePrintButton
                height: visible ? implicitHeight : 0
                onToggled: plasmoid.configuration.autoHideHeader = checked
            }
            PlasmaComponents3.MenuSeparator {
                visible: !plasmoid.configuration.hideDownloadButton
                height: visible ? implicitHeight : 0
            }
            PlasmaComponents3.MenuItem {
                icon.name: "folder-open"
                text: i18n("Open Download Folder")
                visible: !plasmoid.configuration.hideDownloadButton
                height: visible ? implicitHeight : 0
                onTriggered: Qt.openUrlExternally(plasmoid.configuration.downloadPath?.replace(/^file:\/+/, '/') || StandardPaths.writableLocation(StandardPaths.DownloadLocation))
            }
            PlasmaComponents3.MenuItem {
                icon.name: "folder-download"
                text: i18n("Choose Download Folder…")
                visible: !plasmoid.configuration.hideDownloadButton
                height: visible ? implicitHeight : 0
                onTriggered: folderDialog.open()
            }
        }
    }

    // Dialog for selecting download folder location
    FolderDialog {
        id: folderDialog
        currentFolder: plasmoid.configuration.downloadPath || StandardPaths.writableLocation(StandardPaths.DownloadLocation)
        onAccepted: plasmoid.configuration.downloadPath = selectedFolder
    }

    // Close button - Closes the webview and collapses the widget
    HeaderButton {
        icon.name: "window-close"
        tip: i18n("Close the webview and release memory")
        visible: !plasmoid.configuration.hideCloseButton
        onClicked: closeWebViewCallback?.()
    }

    function openSite(url) {
        plasmoid.configuration.url = url;
        goBackToHomePage();
    }

    // Rebuilds the site list and selects the current site.
    function renderChatModel() {
        const current = plasmoid.configuration.url;
        const list = models
            .filter(m => m.prop.startsWith("showCustom_") || plasmoid.configuration[m.prop] || m.url === current)
            .map(m => ({ "text": m.text, "url": m.url }));
        // A typed address: show it as its host name.
        if (current && !list.some(m => m.url === current)) {
            const host = current.replace(/^[a-z]+:\/\//i, "").split("/")[0];
            list.push({ "text": host || current, "url": current });
        }
        if (!plasmoid.configuration.hideCustomURL)
            list.push({ "text": i18n("Custom URL…"), "url": "" });
        urlComboBox.model = list;
        urlComboBox.currentIndex = list.findIndex(m => m.url === current);
    }

    Binding {
        target: root
        property: "hideOnWindowDeactivate"
        value: !plasmoid.configuration.pin && !(parent && parent.webviewRoot && parent.webviewRoot.dialogOpen)
        restoreMode: Binding.RestoreBinding
    }

    // Debounce timer for model re-rendering
    Timer {
        id: modelUpdateTimer
        interval: 50
        onTriggered: renderChatModel()
    }

    // Rebuild when the url or any site setting changes (debounced).
    Connections {
        target: plasmoid.configuration
        function onValueChanged(key, value) {
            if (key === "url" || key === "customSites" || key === "hideCustomURL" || key.startsWith("show"))
                modelUpdateTimer.restart();
        }
    }
}
