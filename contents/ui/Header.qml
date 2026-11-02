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
    property bool showCustomURLInput: false        // Toggle between URL selector and custom URL input
    property var webview: (parent && parent.webviewRoot) ? parent.webviewRoot.webview : null

    // Utility function to ensure URL has a valid protocol
    function sanitizeUrl(url) {
        if (!url || typeof url !== 'string') return '';
        return url.match(/^https?:\/\//i) ? url : "https://" + url;
    }

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

    // URL Selection components
    PlasmaComponents3.ComboBox {
        id: urlComboBox
        Layout.fillWidth: true
        flat: !editable
        PlasmaComponents3.ToolTip.text: i18n("Select or enter chat website URL")
        PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
        PlasmaComponents3.ToolTip.visible: hovered && !pressed
        model: []
        editable: currentIndex === count - 1

        property string customUrlText: ""

        displayText: {
            if (currentIndex === count - 1 && editable) {
                return customUrlText || "";
            }
            return currentText;
        }

        onActivated: {
            if (currentIndex === count - 1) {
                editable = true;
                customUrlText = "";
                editText = "";
                Qt.callLater(() => {
                    forceActiveFocus();
                    if (urlComboBox.hasOwnProperty("textField")) {
                        urlComboBox.textField.forceActiveFocus();
                    }
                });
            } else {
                editable = false;
                handleModelSelection();
            }
        }

        onEditTextChanged: {
            if (currentIndex === count - 1) {
                customUrlText = editText;
            }
        }

        Keys.onReturnPressed: event => {
            if (currentIndex === count - 1 && editText) {
                plasmoid.configuration.url = sanitizeUrl(editText);
                goBackToHomePage();
                event.accepted = true;
            }
        }

        onAccepted: {
            if (currentIndex === count - 1 && editText) {
                plasmoid.configuration.url = sanitizeUrl(editText);
                goBackToHomePage();
            }
        }

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

    // Helper Functions
    // Returns the number of available chat models
    function getModelsLength() {
        return urlComboBox.model.length;
    }

    // Updates the chat model list and current selection
    // Handles both predefined and custom chat models
    function renderChatModel() {
        // Create model list from enabled predefined models
        const chatModel = models.filter(model => !model.prop.startsWith("showCustom_") && plasmoid.configuration[model.prop]).map(model => model.text)
        // Add custom sites to the model list
        .concat((plasmoid.configuration.customSites || "").split(',').filter(site => site?.includes('|')).map(site => site.split('|')[0])).concat([i18n("Custom URL...")]);

        // Update ComboBox model and select current item
        urlComboBox.model = chatModel;

        const currentUrl = plasmoid.configuration.url;
        const currentModel = models.find(model => !model.prop.startsWith("showCustom_") && model.url === currentUrl);

        if (currentModel) {
            const index = chatModel.indexOf(currentModel.text);
            urlComboBox.currentIndex = index;
            urlComboBox.editable = false;
        } else {
            const customSite = (plasmoid.configuration.customSites || "").split(',').find(site => site?.includes('|') && site.split('|')[1] === currentUrl);

            if (customSite) {
                const siteName = customSite.split('|')[0];
                const index = chatModel.indexOf(siteName);
                urlComboBox.currentIndex = index;
                urlComboBox.editable = false;
            } else {
                urlComboBox.currentIndex = chatModel.length - 1;
                urlComboBox.customUrlText = currentUrl;
                urlComboBox.editText = currentUrl;
                urlComboBox.editable = true;
            }
        }
    }

    // Handles model selection from ComboBox
    // Updates current URL and navigates to selected chat
    function handleModelSelection() {
        if (urlComboBox.currentIndex === urlComboBox.count - 1) {
            // Custom URL handling
            const url = urlComboBox.editText;
            if (url) {
                plasmoid.configuration.url = sanitizeUrl(url);
                goBackToHomePage();
            }
            return;
        }

        const selectedText = urlComboBox.currentValue;
        if (!selectedText)
            return;
        urlComboBox.displayText = selectedText;

        const selectedModel = models.find(model => !model.prop.startsWith("showCustom_") && model.text === selectedText);
        if (selectedModel) {
            plasmoid.configuration.url = selectedModel.url;
            goBackToHomePage();
            return;
        }

        const customSite = (plasmoid.configuration.customSites || "").split(',').find(site => site?.split('|')[0] === selectedText);
        if (customSite) {
            plasmoid.configuration.url = customSite.split('|')[1];
            goBackToHomePage();
        }
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

    // Configuration change handler - debounced to prevent multiple rapid re-renders
    Connections {
        target: plasmoid.configuration
        function onCustomSitesChanged() { modelUpdateTimer.restart() }
        function onShowT3ChatChanged() { modelUpdateTimer.restart() }
        function onShowDuckDuckGoChatChanged() { modelUpdateTimer.restart() }
        function onShowChatGPTChanged() { modelUpdateTimer.restart() }
        function onShowHugginChatChanged() { modelUpdateTimer.restart() }
        function onShowGoogleGeminiChanged() { modelUpdateTimer.restart() }
        function onShowYouChanged() { modelUpdateTimer.restart() }
        function onShowPerplexityChanged() { modelUpdateTimer.restart() }
        function onShowLobeChatChanged() { modelUpdateTimer.restart() }
        function onShowBigAGIChanged() { modelUpdateTimer.restart() }
        function onShowBlackBoxChanged() { modelUpdateTimer.restart() }
        function onShowBingCopilotChanged() { modelUpdateTimer.restart() }
        function onShowClaudeChanged() { modelUpdateTimer.restart() }
        function onShowDeepSeekChanged() { modelUpdateTimer.restart() }
        function onShowMetaAIChanged() { modelUpdateTimer.restart() }
        function onShowGrokChanged() { modelUpdateTimer.restart() }
    }
}
