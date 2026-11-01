/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Dialogs
import QtQuick.Layouts

import org.kde.plasma.components as PlasmaComponents3
import org.kde.kirigami 2.20 as Kirigami
import org.kde.kcmutils as KCM

import Qt.labs.platform 1.1

// Main configuration component for general settings
KCM.SimpleKCM {
    id: configRoot
    readonly property string effectiveProfileName: plasmoid.configuration.webEngineProfileName && plasmoid.configuration.webEngineProfileName.length ? plasmoid.configuration.webEngineProfileName : "chat-ai"
    readonly property string profilePath: StandardPaths.writableLocation(StandardPaths.GenericDataLocation).toString().replace(/^file:\/\//, '') + "/chatai-plasmoid/" + effectiveProfileName

    // Parse the comma-separated string and add valid entries to the model
    function loadSitesFromConfig() {
        customSitesModel.clear();
        let savedSites = plasmoid.configuration.customSites || "";
        if (savedSites)
            savedSites.split(',').forEach(site => {
                if (site && site.includes('|'))
                    customSitesModel.append({
                        "siteData": site
                    });
            });
    }

    // Converts the model data into a comma-separated string
    function updateConfiguration() {
        let sites = [];
        for (let i = 0; i < customSitesModel.count; i++) {
            sites.push(customSitesModel.get(i).siteData);
        }
        plasmoid.configuration.customSites = sites.join(',');
    }

    // Validates input, checks for duplicates, and updates configuration
    function addCustomSite() {
        if (customSiteNameField.text && customSiteUrlField.text) {
            let siteName = customSiteNameField.text.trim();
            let siteUrl = customSiteUrlField.text.trim();
            // Add https:// if not present
            if (!/^https?:\/\//i.test(siteUrl))
                siteUrl = "https://" + siteUrl;

            let newSite = siteName + "|" + siteUrl;
            // Check for duplicates
            let isDuplicate = false;
            for (let i = 0; i < customSitesModel.count; i++) {
                if (customSitesModel.get(i).siteData === newSite) {
                    isDuplicate = true;
                    break;
                }
            }
            if (!isDuplicate) {
                customSitesModel.append({
                    "siteData": newSite
                });
                updateConfiguration();
                // Clear input fields
                customSiteNameField.text = "";
                customSiteUrlField.text = "";
            }
        }
    }

    // Updates the model and saves changes to configuration
    function removeCustomSite(index) {
        customSitesModel.remove(index);
        updateConfiguration();
    }

    // Model for managing custom sites in a dynamic list
    ListModel {
        id: customSitesModel

        Component.onCompleted: loadSitesFromConfig()
    }

    // Main scroll view for all configuration options
    QQC2.ScrollView {
        id: scrollView

        anchors.fill: parent
        // Enable vertical scrollbar
        Component.onCompleted: {
            QQC2.ScrollBar.vertical.policy = QQC2.ScrollBar.AlwaysOn;
        }

        Item {
            width: scrollView.width
            implicitHeight: formLayout.implicitHeight

            // Form layout containing all configuration sections
            Kirigami.FormLayout {
                id: formLayout

                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    leftMargin: Kirigami.Units.largeSpacing
                    rightMargin: Kirigami.Units.largeSpacing
                }

                // Predefined Sites Section
                // List of built-in chat services that can be enabled/disabled
                QQC2.Label {
                    text: i18n("Predefined Sites")
                    font.bold: true
                    Layout.fillWidth: true
                }

                // Dynamic list of predefined sites with checkboxes
                Repeater {
                    // List of supported chat services with their configuration properties

                    model: [
                        {
                            "id": "showDuckDuckGoChat",
                            "text": "DuckDuckGo Chat"
                        },
                        {
                            "id": "showChatGPT",
                            "text": "ChatGPT"
                        },
                        {
                            "id": "showHugginChat",
                            "text": "HuggingChat"
                        },
                        {
                            "id": "showGoogleGemini",
                            "text": "Google Gemini"
                        },
                        {
                            "id": "showYou",
                            "text": "You"
                        },
                        {
                            "id": "showPerplexity",
                            "text": "Perplexity"
                        },
                        {
                            "id": "showBlackBox",
                            "text": "BlackBox AI"
                        },
                        {
                            "id": "showBingCopilot",
                            "text": "Bing Copilot"
                        },
                        {
                            "id": "showBigAGI",
                            "text": "Big AGI"
                        },
                        {
                            "id": "showLobeChat",
                            "text": "LobeChat"
                        },
                        {
                            "id": "showClaude",
                            "text": "Claude"
                        },
                        {
                            "id": "showDeepSeek",
                            "text": "DeepSeek"
                        },
                        {
                            "id": "showMetaAI",
                            "text": "Meta AI"
                        },
                        {
                            "id": "showGrok",
                            "text": "Grok"
                        },
                        {
                            "id": "showT3Chat",
                            "text": "T3 Chat"
                        }
                    ]

                    delegate: ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        QQC2.CheckBox {
                            text: modelData.text
                            checked: plasmoid.configuration[modelData.id]
                            onCheckedChanged: plasmoid.configuration[modelData.id] = checked
                            Layout.fillWidth: true
                        }

                        Kirigami.InlineMessage {
                            Layout.fillWidth: true
                            type: Kirigami.MessageType.Information
                            text: i18n("Claude.ai only allows account creation or Google login in well-known browsers. To use it in this Plasmoid, you need to use login credentials previously created in a traditional browser.")
                            visible: modelData.id === "showClaude" && plasmoid.configuration.showClaude
                        }
                    }
                }

                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Layout.fillWidth: true
                }

                // Allows users to add their own chat services
                QQC2.Label {
                    text: i18n("Custom Sites")
                    font.bold: true
                    Layout.fillWidth: true
                }

                // Input fields and list for custom sites
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    spacing: Kirigami.Units.smallSpacing

                    // Custom site input fields
                    RowLayout {
                        Layout.fillWidth: true

                        QQC2.TextField {
                            id: customSiteNameField

                            placeholderText: i18n("Site Name")
                            Layout.fillWidth: false
                            onAccepted: configRoot.addCustomSite()
                        }

                        QQC2.TextField {
                            id: customSiteUrlField

                            placeholderText: i18n("Site URL")
                            Layout.fillWidth: true
                            onAccepted: configRoot.addCustomSite()
                        }

                        QQC2.Button {
                            icon.name: "list-add"
                            onClicked: configRoot.addCustomSite()
                        }
                    }

                    // Custom sites list using Cards
                    Column {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Repeater {
                            model: customSitesModel

                            delegate: Kirigami.Card {
                                width: parent.width
                                verticalPadding: 0

                                contentItem: RowLayout {
                                    spacing: Kirigami.Units.smallSpacing
                                    anchors.verticalCenter: parent.verticalCenter

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Layout.alignment: Qt.AlignVCenter

                                        PlasmaComponents3.Label {
                                            text: model.siteData.split("|")[0]
                                            font.bold: true
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }

                                        PlasmaComponents3.Label {
                                            text: model.siteData.split("|")[1]
                                            font.pointSize: theme.smallestFont.pointSize
                                            opacity: 0.7
                                            Layout.fillWidth: true
                                            wrapMode: Text.WordWrap
                                        }
                                    }

                                    PlasmaComponents3.Button {
                                        icon.name: "list-remove"
                                        onClicked: removeCustomSite(model.index)
                                        display: PlasmaComponents3.AbstractButton.IconOnly
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                }
                            }
                        }

                        // Message when empty
                        Kirigami.PlaceholderMessage {
                            width: parent.width
                            visible: customSitesModel.count === 0
                            text: i18n("No custom sites added yet")
                        }
                    }
                }

                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Layout.fillWidth: true
                }

                // Permissions Section
                QQC2.Label {
                    text: i18n("Permissions")
                    font.bold: true
                    Layout.fillWidth: true
                }

                // Startup behavior option
                QQC2.CheckBox {
                    id: loadOnStartup

                    text: i18n("Load website on Plasma startup")
                    checked: plasmoid.configuration.loadOnStartup
                    onCheckedChanged: plasmoid.configuration.loadOnStartup = checked
                    Layout.fillWidth: true
                }

                // Media permissions options
                QQC2.CheckBox {
                    id: notificationsEnabled
                    text: i18n("Allow notifications")
                    checked: plasmoid.configuration.notificationsEnabled
                    onCheckedChanged: plasmoid.configuration.notificationsEnabled = checked
                    Layout.fillWidth: true
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    type: Kirigami.MessageType.Information
                    text: i18n("If notifications are not working create the file:") + `
~/.local/share/knotifications6/chatai_plasmoid.notifyrc ` + i18n("containing the following text:") + `

[Global]
IconName=applications-internet
DesktopEntry=ChatAI
Comment=ChatAI
[Event/notification]
Name=ChatAI
Action=Popup`
                    visible: notificationsEnabled.checked
                }

                QQC2.CheckBox {
                    id: geolocationEnabled
                    text: i18n("Allow geolocation access")
                    checked: plasmoid.configuration.geolocationEnabled
                    onCheckedChanged: plasmoid.configuration.geolocationEnabled = checked
                    Layout.fillWidth: true
                }

                QQC2.CheckBox {
                    id: microphoneEnabled

                    text: i18n("Allow using microphone")
                    checked: plasmoid.configuration.microphoneEnabled
                    onCheckedChanged: plasmoid.configuration.microphoneEnabled = checked
                    Layout.fillWidth: true
                }

                QQC2.CheckBox {
                    id: webcamEnabled

                    text: i18n("Allow using webcam")
                    checked: plasmoid.configuration.webcamEnabled
                    onCheckedChanged: plasmoid.configuration.webcamEnabled = checked
                    Layout.fillWidth: true
                }

                QQC2.CheckBox {
                    id: screenShareEnabled

                    text: i18n("Allow screen sharing")
                    checked: plasmoid.configuration.screenShareEnabled
                    onCheckedChanged: plasmoid.configuration.screenShareEnabled = checked
                    Layout.fillWidth: true
                }

                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Layout.fillWidth: true
                }

                // Web Features Section
                QQC2.Label {
                    text: i18n("Web Features")
                    font.bold: true
                    Layout.fillWidth: true
                }

                QQC2.CheckBox {
                    id: spatialNavigationEnabled
                    text: i18n("Enable spatial navigation")
                    checked: plasmoid.configuration.spatialNavigationEnabled
                    onCheckedChanged: plasmoid.configuration.spatialNavigationEnabled = checked
                    Layout.fillWidth: true
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    type: Kirigami.MessageType.Information
                    text: i18n("Allows navigation between focusable elements using arrow keys")
                    visible: spatialNavigationEnabled.checked
                }

                QQC2.CheckBox {
                    id: javascriptCanOpenWindows
                    text: i18n("Allow JavaScript to open new windows")
                    checked: plasmoid.configuration.javascriptCanOpenWindows
                    onCheckedChanged: plasmoid.configuration.javascriptCanOpenWindows = checked
                    Layout.fillWidth: true
                }

                QQC2.CheckBox {
                    id: javascriptCanAccessClipboard
                    text: i18n("Allow JavaScript to access clipboard")
                    checked: plasmoid.configuration.javascriptCanAccessClipboard
                    onCheckedChanged: plasmoid.configuration.javascriptCanAccessClipboard = checked
                    Layout.fillWidth: true
                }

                QQC2.CheckBox {
                    id: playbackRequiresUserGesture
                    text: i18n("Require user gesture for media playback")
                    checked: plasmoid.configuration.playbackRequiresUserGesture
                    onCheckedChanged: plasmoid.configuration.playbackRequiresUserGesture = checked
                    Layout.fillWidth: true
                }


                // Notifications Section
                QQC2.Label {
                    text: i18n("Notification Settings")
                    font.bold: true
                    Layout.fillWidth: true
                }

                // Downloads Section
                // Configuration for download location and behavior
                QQC2.Label {
                    text: i18n("Download Folder")
                    font.bold: true
                    Layout.fillWidth: true
                }

                // Download path input and folder selection
                RowLayout {
                    Layout.fillWidth: true

                    QQC2.TextField {
                        id: downloadPath

                        Layout.fillWidth: true
                        text: plasmoid.configuration.downloadPath || StandardPaths.writableLocation(StandardPaths.DownloadLocation)
                        placeholderText: StandardPaths.writableLocation(StandardPaths.DownloadLocation)
                        onTextChanged: {
                            if (text)
                                plasmoid.configuration.downloadPath = text;
                        }
                    }

                    QQC2.Button {
                        icon.name: "folder"
                        onClicked: downloadFolderDialog.open()
                    }
                }

                // Folder selection dialog
                FolderDialog {
                    id: downloadFolderDialog

                    currentFolder: downloadPath.text
                    onAccepted: {
                        downloadPath.text = selectedFolder;
                        plasmoid.configuration.downloadPath = selectedFolder;
                    }
                }

                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Layout.fillWidth: true
                }

                // Engine Section (WPE WebKit, out of process)
                QQC2.Label {
                    text: i18n("Web Engine")
                    font.bold: true
                    Layout.fillWidth: true
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    type: Kirigami.MessageType.Information
                    text: i18n("Pages are rendered by a separate lightweight WebKit process, never inside Plasma. While the popup is closed the page is paused; after the time below the process is stopped and its memory released (your login is kept).")
                    visible: true
                }

                RowLayout {
                    Layout.fillWidth: true
                    QQC2.Label {
                        text: i18n("Stop the engine after the popup is closed for:")
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                    QQC2.SpinBox {
                        from: 0
                        to: 1440
                        value: plasmoid.configuration.unloadAfterMinutes
                        textFromValue: (v, locale) => v === 0 ? i18n("Never") : i18np("%1 minute", "%1 minutes", v)
                        valueFromText: (t, locale) => parseInt(t) || 0
                        onValueModified: plasmoid.configuration.unloadAfterMinutes = value
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    QQC2.Label {
                        text: i18n("Memory limit for the web engine:")
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                    }
                    QQC2.SpinBox {
                        from: 0
                        to: 16384
                        stepSize: 128
                        value: plasmoid.configuration.memoryLimitMb
                        textFromValue: (v, locale) => v === 0 ? i18n("No limit") : i18n("%1 MB", v)
                        valueFromText: (t, locale) => parseInt(t) || 0
                        onValueModified: plasmoid.configuration.memoryLimitMb = value
                    }
                }

                QQC2.Label {
                    text: i18n("User agent (empty = default Safari-compatible):")
                    Layout.fillWidth: true
                }

                QQC2.TextField {
                    Layout.fillWidth: true
                    text: plasmoid.configuration.userAgent
                    placeholderText: "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Safari/605.1.15"
                    onEditingFinished: plasmoid.configuration.userAgent = text.trim()
                }

                RowLayout {
                    Layout.fillWidth: true
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: i18n("Profile location: %1", configRoot.profilePath)
                    }
                    QQC2.Button {
                        text: i18n("Open Profile Folder")
                        icon.name: "folder"
                        onClicked: Qt.openUrlExternally("file://" + configRoot.profilePath)
                    }
                }

                Kirigami.Separator {
                    Kirigami.FormData.isSection: true
                    Layout.fillWidth: true
                }

                QQC2.Label {
                    text: i18n("Profile Storage Name")
                    font.bold: true
                    Layout.fillWidth: true
                }

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    type: Kirigami.MessageType.Information
                    text: i18n("Each widget instance stores cookies, logins and cache using the profile name. Use a unique name when you want this instance to keep its own session data.")
                    visible: true
                }

                QQC2.TextField {
                    id: profileNameField
                    Layout.fillWidth: true
                    placeholderText: "chat-ai"
                    text: plasmoid.configuration.webEngineProfileName
                    onEditingFinished: {
                        const trimmed = text.trim();
                        const value = trimmed.length ? trimmed : "chat-ai";
                        if (text !== value)
                            text = value;
                        plasmoid.configuration.webEngineProfileName = value;
                    }
                }
            }
        }
    }
}
