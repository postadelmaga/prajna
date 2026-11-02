/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtCore
import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

import org.kde.plasma.components as PlasmaComponents3
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami
import org.kde.notification

import "../lib/chatai"

import "."

// The web area. Rendering happens in a separate WPE WebKit process
// (chatai-webwpe); WebSurface only shows its frames and forwards input.
Item {
    id: webViewRoot

    // Kept under this name for Header/CompactRepresentation.
    property alias webview: webview
    property bool findBarVisible: false
    // True while a native dialog is up: the popup must not auto-close.
    readonly property bool dialogOpen: fileDialog.visible

    readonly property string effectiveProfileName: plasmoid.configuration.webEngineProfileName && plasmoid.configuration.webEngineProfileName.length ? plasmoid.configuration.webEngineProfileName : "chat-ai"
    readonly property string downloadDirectory: {
        const p = plasmoid.configuration.downloadPath ? plasmoid.configuration.downloadPath.toString() : "";
        return p.length ? p.replace(/^file:\/\//, '') : StandardPaths.writableLocation(StandardPaths.DownloadLocation).toString().replace(/^file:\/\//, '');
    }

    function goBackToHomePage() {
        const url = plasmoid.configuration.url;
        if (!url || typeof url !== 'string' || url.length === 0) {
            console.error("Invalid or empty URL configuration");
            return;
        }
        webview.load(url);
    }

    function goBack() {
        webview.goBack();
    }

    function goForward() {
        webview.goForward();
    }

    function reloadPage() {
        webview.reloadAndBypassCache();
    }

    function printPage() {
        // WPE has no print backend; let the page's own print path run.
        webview.runJavaScript("window.print()");
    }

    function isDarkTheme() {
        const color = Kirigami.Theme.backgroundColor;
        return (0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b) < 0.5;
    }

    function showNotification(title, message, icon = "dialog-information") {
        webNotification.title = title || i18n("ChatAI");
        webNotification.text = message;
        webNotification.iconName = icon;
        webNotification.sendEvent();
    }

    function permissionAllowed(kind) {
        switch (kind) {
        case WebSurface.PermissionNotifications:
            return plasmoid.configuration.notificationsEnabled;
        case WebSurface.PermissionGeolocation:
            return plasmoid.configuration.geolocationEnabled;
        case WebSurface.PermissionMicrophone:
            return plasmoid.configuration.microphoneEnabled;
        case WebSurface.PermissionCamera:
            return plasmoid.configuration.webcamEnabled;
        case WebSurface.PermissionMicrophoneAndCamera:
            return plasmoid.configuration.microphoneEnabled && plasmoid.configuration.webcamEnabled;
        case WebSurface.PermissionScreen:
            return plasmoid.configuration.screenShareEnabled;
        case WebSurface.PermissionClipboard:
            return plasmoid.configuration.javascriptCanAccessClipboard;
        default:
            return false;
        }
    }

    // MIME types from <input accept> → FileDialog name filters.
    function nameFiltersFor(mimeTypes) {
        const patterns = {
            "image/*": "*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg *.heic *.avif",
            "image/png": "*.png", "image/jpeg": "*.jpg *.jpeg", "image/gif": "*.gif", "image/webp": "*.webp",
            "application/pdf": "*.pdf",
            "text/*": "*.txt *.md *.csv *.json *.xml *.html *.log",
            "text/plain": "*.txt *.log", "text/markdown": "*.md", "text/csv": "*.csv",
            "audio/*": "*.mp3 *.wav *.ogg *.flac *.m4a *.opus",
            "video/*": "*.mp4 *.webm *.mkv *.mov"
        };
        let globs = [];
        for (const m of mimeTypes) {
            if (patterns[m])
                globs.push(patterns[m]);
            else if (m.startsWith("."))
                globs.push("*" + m);
        }
        let filters = [];
        if (globs.length)
            filters.push(i18n("Supported files (%1)", globs.join(" ")));
        filters.push(i18n("All files (*)"));
        return filters;
    }

    function checkAndUpdateFavicon() {
        if (Plasmoid.configuration.lastFavIcon)
            Plasmoid.configuration.favIcon = Plasmoid.configuration.lastFavIcon;
        webview.runJavaScript(`
            (function () {
                const icons = [];
                for (const m of document.getElementsByTagName('meta'))
                    if (m.getAttribute('property') === 'og:image' && m.getAttribute('content'))
                        icons.push({ href: m.getAttribute('content'), size: 64 });
                for (const l of document.getElementsByTagName('link')) {
                    const rel = l.getAttribute('rel');
                    if (rel && (rel.includes('icon') || rel.includes('shortcut')) && l.getAttribute('href')) {
                        const sizes = l.getAttribute('sizes');
                        icons.push({ href: l.getAttribute('href'), size: sizes ? parseInt(sizes.split('x')[0]) : 32 });
                    }
                }
                if (!icons.length) icons.push({ href: '/favicon.ico', size: 32 });
                icons.sort((a, b) => b.size - a.size);
                return new URL(icons[0].href, window.location.origin).href;
            })()
        `, function (result) {
            if (result && result.startsWith("http")) {
                Plasmoid.configuration.favIcon = result;
                Plasmoid.configuration.lastFavIcon = result;
            }
        });
    }

    Layout.fillWidth: true
    Layout.fillHeight: true

    Notification {
        id: webNotification
        componentName: "chatai_plasmoid"
        eventId: "notification"
        title: i18n("ChatAI")
        iconName: "dialog-information"
    }

    Shortcut {
        sequence: StandardKey.Find
        onActivated: webViewRoot.findBarVisible = true
    }

    ContextMenu {
        id: contextMenu
        webviewItem: webview
        onReloadRequested: webViewRoot.reloadPage()
    }

    FileDialog {
        id: fileDialog

        property int requestId: -1

        title: i18n("Choose a file to upload")
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        onAccepted: {
            webview.replyFileChooser(requestId, fileMode === FileDialog.OpenFiles ? selectedFiles : [selectedFile]);
            requestId = -1;
        }
        onRejected: {
            webview.replyFileChooser(requestId, []);
            requestId = -1;
        }
    }

    ListModel {
        id: downloadsModel

        function indexOfId(id) {
            for (let i = 0; i < count; i++)
                if (get(i).downloadId === id)
                    return i;
            return -1;
        }
    }

    WebSurface {
        id: webview

        anchors.fill: parent
        anchors.topMargin: (findBar.visible ? findBar.height : 0) + (popupBar.visible ? popupBar.height : 0)

        url: plasmoid.configuration.url
        profileDirectory: StandardPaths.writableLocation(StandardPaths.GenericDataLocation).toString().replace(/^file:\/\//, '') + "/chatai-plasmoid/" + webViewRoot.effectiveProfileName
        memoryLimitMb: plasmoid.configuration.memoryLimitMb
        maxFps: plasmoid.configuration.maxFps
        gpuRendering: plasmoid.configuration.gpuRendering
        userAgent: plasmoid.configuration.userAgent || "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.4 Safari/605.1.15"
        downloadDirectory: webViewRoot.downloadDirectory
        darkMode: webViewRoot.isDarkTheme()
        javascriptCanOpenWindows: plasmoid.configuration.javascriptCanOpenWindows
        javascriptCanAccessClipboard: plasmoid.configuration.javascriptCanAccessClipboard
        playbackRequiresUserGesture: plasmoid.configuration.playbackRequiresUserGesture
        spatialNavigation: plasmoid.configuration.spatialNavigationEnabled
        popupsInPlace: true

        property bool wasLoading: false
        onLoadStateChanged: {
            if (wasLoading && !loading)
                webViewRoot.checkAndUpdateFavicon();
            wasLoading = loading;
        }

        onOpenExternalRequested: url => Qt.openUrlExternally(url)

        onNotificationReceived: (title, body) => {
            if (plasmoid.configuration.notificationsEnabled)
                webViewRoot.showNotification(title, body);
        }

        onPermissionRequested: (requestId, kind, origin) => {
            replyPermission(requestId, webViewRoot.permissionAllowed(kind));
        }

        onFileChooserRequested: (requestId, multiple, mimeTypes) => {
            if (fileDialog.requestId >= 0)
                replyFileChooser(fileDialog.requestId, []);
            fileDialog.requestId = requestId;
            fileDialog.fileMode = multiple ? FileDialog.OpenFiles : FileDialog.OpenFile;
            fileDialog.nameFilters = webViewRoot.nameFiltersFor(mimeTypes);
            fileDialog.open();
        }

        onContextMenuRequested: (x, y, flags, link, mediaUrl) => {
            contextMenu.flags = flags;
            contextMenu.link = link;
            contextMenu.mediaUrl = mediaUrl;
            contextMenu.open(x, y);
        }

        onDownloadUpdated: (id, state, received, total, path) => {
            const fileName = path.substring(path.lastIndexOf("/") + 1);
            const row = {
                "downloadId": id,
                "dlState": state,
                "receivedBytes": received,
                "totalBytes": total,
                "progress": total > 0 ? received / total : 0,
                "fullPath": path,
                "fileName": fileName
            };
            const i = downloadsModel.indexOfId(id);
            if (i < 0)
                downloadsModel.append(row);
            else
                downloadsModel.set(i, row);
            if (state === WebSurface.DownloadFinished)
                webViewRoot.showNotification(i18n("Download completed"), fileName, "download");
            else if (state === WebSurface.DownloadFailed)
                webViewRoot.showNotification(i18n("Download failed"), fileName, "dialog-error");
        }

        onFindResult: matches => findBar.matches = matches

        onEngineCrashed: console.warn("ChatAI: web engine restarted after a crash")
    }

    // Shown while the engine is not up yet (first start, or after an idle unload).
    PlasmaComponents3.BusyIndicator {
        anchors.centerIn: parent
        running: visible
        visible: webview.active && !webview.running && webview.lastError === ""
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        visible: webview.lastError !== ""
        icon.name: "dialog-error"
        text: i18n("The web engine could not start")
        explanation: webview.lastError
        helpfulAction: Kirigami.Action {
            icon.name: "view-refresh"
            text: i18n("Retry")
            onTriggered: webview.restartEngine()
        }
    }

    // OAuth / "Sign in with ..." windows open on top of the chat.
    Rectangle {
        id: popupBar

        visible: webview.popupOpen
        height: visible ? popupRow.implicitHeight + Kirigami.Units.smallSpacing * 2 : 0
        color: Kirigami.Theme.backgroundColor
        z: 5
        anchors {
            top: findBar.visible ? findBar.bottom : parent.top
            left: parent.left
            right: parent.right
        }

        RowLayout {
            id: popupRow
            anchors.fill: parent
            anchors.margins: Kirigami.Units.smallSpacing

            Kirigami.Icon {
                source: "window-new"
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: webview.title || i18n("Popup window")
            }
            PlasmaComponents3.Button {
                icon.name: "window-close"
                text: i18n("Close")
                onClicked: webview.closePopup()
            }
        }
    }

    PlasmaComponents3.ProgressBar {
        id: loadingProgressBar

        z: 10
        visible: webview.loading && webview.loadProgress < 100
        height: visible ? 3 : 0
        anchors {
            top: webview.top
            left: parent.left
            right: parent.right
        }
        from: 0
        to: 100
        value: webview.loadProgress
    }

    Rectangle {
        id: statusBubble

        property int padding: 8

        color: Kirigami.Theme.backgroundColor
        visible: webview.hoveredLink !== ""
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: Math.min(statusText.implicitWidth + padding, parent.width)
        height: statusText.implicitHeight + padding
        z: 6

        PlasmaComponents3.Label {
            id: statusText
            anchors.centerIn: parent
            width: parent.width - parent.padding
            elide: Text.ElideMiddle
            text: webview.hoveredLink
        }
    }

    DownloadBar {
        z: 7
        downloadsModel: downloadsModel
        webviewItem: webview
    }

    FindBar {
        id: findBar
        findBarVisible: webViewRoot.findBarVisible
        webviewItem: webview

        onCloseRequested: webViewRoot.findBarVisible = false
        onFindBarVisibleChanged: {
            if (findBarVisible) {
                findBar.focusAndSelect();
            } else {
                findBar.clearSearch();
                webview.forceActiveFocus();
            }
        }
    }
}
