/*
 *  SPDX-FileCopyrightText: 2024 Denys Madureira <denysmb@zoho.com>
 *  SPDX-FileCopyrightText: 2025 Bruno Gonçalves <bigbruno@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Layouts

import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.plasmoid
import org.kde.kirigami as Kirigami

import "../lib/chatai"
import "sites.js" as Sites

// Main plasmoid item that contains all the widget functionality
PlasmoidItem {
    id: root

    // Chat services: the built-in ones (sites.js) plus the user's custom
    // sites, which are always listed.
    property var models: Sites.predefined.concat(Sites.parseCustom(plasmoid.configuration.customSites).map(site => ({
        "id": site.text.toLowerCase().replace(/\s+/g, '-'),
        "url": site.url,
        "text": site.text,
        "prop": "showCustom_" + site.text.toLowerCase().replace(/\s+/g, '_')
    })))

    // The web area (WebView.qml), once the popup has been created.
    readonly property var webviewRoot: fullRepresentationItem ? fullRepresentationItem.webviewRoot : null

    // Right-click menu of the widget icon: works even with the header hidden.
    Plasmoid.contextualActions: [
        PlasmaCore.Action {
            text: i18n("Go to Start Page")
            icon.name: "go-home"
            onTriggered: {
                root.expanded = true;
                if (root.webviewRoot)
                    root.webviewRoot.goBackToHomePage();
            }
        },
        PlasmaCore.Action {
            text: i18n("Reload Page")
            icon.name: "view-refresh"
            enabled: root.webviewRoot !== null
            onTriggered: root.webviewRoot.reloadPage()
        },
        PlasmaCore.Action {
            text: i18n("Keep Open")
            icon.name: "window-pin"
            checkable: true
            checked: plasmoid.configuration.pin
            onTriggered: plasmoid.configuration.pin = checked
        }
    ]

    // Initialize the plasmoid and check if it should load on startup
    Component.onCompleted: {
        // If loadOnStartup is enabled in configuration
        if (plasmoid.configuration.loadOnStartup) {
            webviewLoader.active = true; // Activate the WebView loader
            root.expanded = true; // Expand the plasmoid
        }
    }

    // Widget appearance when collapsed (icon only)
    compactRepresentation: CompactRepresentation {
        id: compactRep

        models: root.models
        webview: root.webviewRoot ? root.webviewRoot.webview : null
    }

    // Widget appearance when expanded (full view)
    // Popup: glass background (blur + tint + grain) under the header/page
    // layout. Kept outside the ColumnLayout: reorderComponents() re-parents
    // every child of the layout.
    fullRepresentation: Item {
        id: popupRoot

        readonly property alias webviewRoot: mainLayout.webviewRoot
        readonly property bool glassVisible: plasmoid.configuration.transparentBackground

        // Default dimensions (used when no saved size exists)
        readonly property int defaultWidth: Kirigami.Units.gridUnit * 28
        readonly property int defaultHeight: Kirigami.Units.gridUnit * 39

        Layout.minimumWidth: Kirigami.Units.gridUnit * 20
        Layout.minimumHeight: Kirigami.Units.gridUnit * 28
        // Use saved dimensions if available, otherwise use defaults
        Layout.preferredWidth: plasmoid.configuration.dialogWidth > 0 ? plasmoid.configuration.dialogWidth : defaultWidth
        Layout.preferredHeight: plasmoid.configuration.dialogHeight > 0 ? plasmoid.configuration.dialogHeight : defaultHeight

        // Blur behind the popup on/off (Plasma's default is on). The option
        // belongs to the see-through page: with an opaque page Plasma's own
        // popup look applies.
        PopupBlur {
            blur: plasmoid.configuration.blurBehind || !plasmoid.configuration.transparentBackground
        }

        GlassBackground {
            anchors.fill: parent
            visible: popupRoot.glassVisible
            strength: plasmoid.configuration.tintStrength / 100
            grain: plasmoid.configuration.glassGrain
            headerHeight: headerRoot.visible ? headerRoot.height + Kirigami.Units.smallSpacing : 0
            headerAtBottom: mainLayout.reverseLayout
        }

    ColumnLayout {
        id: mainLayout

        anchors.fill: parent

        // Expose WebView root for other components
        property alias webviewRoot: webviewLoader.item
        // Update the property to use Types.Location and add the change monitor
        property bool reverseLayout: plasmoid.location === PlasmaCore.Types.TopEdge

        // Function to reorder components
        function reorderComponents() {
            let components = reverseLayout ? [webviewLoader, headerMouseArea, headerRoot] : [headerRoot, headerMouseArea, webviewLoader];
            // Clear and re-add the components in the correct order
            for (let i = children.length - 1; i >= 0; i--) {
                children[i].parent = null;
            }
            components.forEach(component => {
                component.parent = mainLayout;
            });
            // Update the anchors of headerMouseArea
            if (headerRoot && headerMouseArea) {
                if (reverseLayout) {
                    headerMouseArea.Layout.alignment = Qt.AlignBottom;
                } else {
                    headerMouseArea.Layout.alignment = Qt.AlignTop;
                }
            }
        }


        Component.onCompleted: {
            reorderComponents();
        }

        // Save window size when user resizes
        // Use a timer to debounce saves (avoid saving on every pixel change)
        Timer {
            id: saveSizeTimer
            interval: 500
            repeat: false
            onTriggered: {
                // Only save if the size is valid and different from saved value
                const currentWidth = Math.round(mainLayout.width);
                const currentHeight = Math.round(mainLayout.height);
                const savedWidth = plasmoid.configuration.dialogWidth;
                const savedHeight = plasmoid.configuration.dialogHeight;
                
                // Save if dimensions are valid and different from what's saved
                if (currentWidth > 0 && currentHeight > 0 &&
                    (currentWidth !== savedWidth || currentHeight !== savedHeight)) {
                    plasmoid.configuration.dialogWidth = currentWidth;
                    plasmoid.configuration.dialogHeight = currentHeight;
                }
            }
        }

        onWidthChanged: saveSizeTimer.restart()
        onHeightChanged: saveSizeTimer.restart()

        spacing: 0

        // Add monitor for plasmoid location change
        Connections {
            function onLocationChanged() {
                mainLayout.reverseLayout = plasmoid.location === PlasmaCore.Types.TopEdge;
                reorderComponents();
            }

            target: plasmoid
        }

        // Header component with auto-hide behavior
        Header {
            id: headerRoot

            property bool headerVisible: false
            property bool isInteracting: false
            property bool shouldBeVisible: {
                if (plasmoid.configuration.hideHeader)
                    return false;

                if (!plasmoid.configuration.autoHideHeader)
                    return true;

                return headerVisible || isInteracting || headerMouseArea.containsMouse;
            }

            models: root.models
            Layout.fillWidth: true
            z: 2 // Increase the z-index to ensure it is above the MouseArea
            // Callback to close the WebView and collapse the widget
            closeWebViewCallback: function () {
                webviewLoader.active = false;
                root.expanded = false;
            }
            // Handle navigation
            onGoBackToHomePage: webviewRoot.goBackToHomePage()
            onReloadPageRequested: webviewRoot.reloadPage()
            onNavigateBackRequested: webviewRoot.goBack()
            onNavigateForwardRequested: webviewRoot.goForward()
            onPrintPageRequested: webviewRoot.printPage()
            onNewTabRequested: webviewRoot.newTab()
            onToggleSearchRequested: {
                if (webviewRoot && webviewRoot.findBarVisible !== undefined) {
                    webviewRoot.findBarVisible = !webviewRoot.findBarVisible;
                }
            }
            Layout.preferredHeight: shouldBeVisible ? implicitHeight : 0
            Layout.maximumHeight: Layout.preferredHeight
            Layout.minimumHeight: 0
            Layout.bottomMargin: shouldBeVisible ? Kirigami.Units.smallSpacing : 0
            visible: Layout.preferredHeight > 0
            opacity: Layout.preferredHeight > 0 ? 1 : 0
            clip: true
            // Adjust the anchors of headerMouseArea based on the panel position
            Component.onCompleted: {
                if (mainLayout.reverseLayout) {
                    headerMouseArea.Layout.alignment = Qt.AlignBottom;
                } else {
                    headerMouseArea.Layout.alignment = Qt.AlignTop;
                }
            }

            // Timer for hiding
            Timer {
                id: hideTimer

                interval: 2000
                onTriggered: {
                    if (!headerRoot.isInteracting)
                        headerRoot.headerVisible = false;
                }
            }

            // Timer to check interactions
            Timer {
                id: interactionTimer

                interval: 500
                repeat: true
                running: headerRoot.isInteracting
                onTriggered: {
                    // Check if there is still interaction with any component
                    let stillInteracting = false;
                    for (let i = 0; i < headerRoot.children.length; i++) {
                        let child = headerRoot.children[i];
                        if (child.activeFocus || (child.hasOwnProperty("pressed") && child.pressed)) {
                            stillInteracting = true;
                            break;
                        }
                    }
                    headerRoot.isInteracting = stillInteracting;
                    if (!stillInteracting && !headerMouseArea.containsMouse)
                        hideTimer.restart();
                }
            }

            // Connections to monitor interactions
            Connections {
                function onActiveFocusChanged() {
                    if (target.activeFocus) {
                        headerRoot.isInteracting = true;
                        hideTimer.stop();
                    }
                }

                target: headerRoot
            }

            // Intercept mouse events
            MouseArea {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignTop
                hoverEnabled: true
                propagateComposedEvents: true
                onEntered: {
                    headerRoot.headerVisible = true;
                    hideTimer.stop();
                }
                onExited: {
                    if (!headerRoot.isInteracting)
                        hideTimer.restart();
                }
                onPressed: mouse => {
                    headerRoot.isInteracting = true;
                    mouse.accepted = false;
                }
                onReleased: mouse => {
                    headerRoot.isInteracting = false;
                    if (!containsMouse)
                        hideTimer.restart();

                    mouse.accepted = false;
                }
            }

            // Animations
            Behavior on Layout.preferredHeight {
                NumberAnimation {
                    duration: 400
                    easing.type: Easing.InOutCubic
                }
            }

            Behavior on opacity {
                NumberAnimation {
                    duration: 400
                    easing.type: Easing.InOutQuad
                }
            }
        }

        // Mouse detection area
        MouseArea {
            id: headerMouseArea

            height: 2
            hoverEnabled: true
            z: 1 // Place below the header
            propagateComposedEvents: true
            onEntered: {
                headerRoot.headerVisible = true;
                hideTimer.stop();
            }
            onExited: {
                if (!headerRoot.isInteracting)
                    hideTimer.restart();
            }
            // Pass mouse events to child components
            onClicked: mouse => mouse.accepted = false
            onPressed: mouse => mouse.accepted = false
            onReleased: mouse => mouse.accepted = false
            onDoubleClicked: mouse => mouse.accepted = false
            onPositionChanged: mouse => mouse.accepted = false
            onPressAndHold: mouse => mouse.accepted = false

            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
        }

        // WebView loader that manages the web content
        Loader {
            id: webviewLoader

            // Improved the loading of the WebView & Added Error Handling
            active: root.expanded || item !== null || plasmoid.configuration.loadOnStartup
            asynchronous: true
            source: "WebView.qml"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 0

            // Add status handling
            onStatusChanged: {
                if (status === Loader.Error) {
                    console.error("Failed to load WebView.qml");
                }
            }
        }

        // While the popup is closed the page is throttled (hidden), and after
        // `unloadAfterMinutes` the engine process is shut down entirely; the
        // session (cookies, storage) lives on disk and the last URL reloads.
        Binding {
            target: webviewLoader.item ? webviewLoader.item.webview : null
            property: "suspended"
            value: !root.expanded
            restoreMode: Binding.RestoreNone
        }

        Timer {
            id: idleUnloadTimer

            interval: Math.max(1, plasmoid.configuration.unloadAfterMinutes) * 60000
            running: !root.expanded && webviewLoader.item !== null && plasmoid.configuration.unloadAfterMinutes > 0 && !plasmoid.configuration.loadOnStartup
            onTriggered: {
                if (webviewLoader.item)
                    webviewLoader.item.webview.active = false;
            }
        }

        // Monitor plasmoid expansion state
        Connections {
            // Activate WebView when plasmoid is expanded
            function onExpandedChanged() {
                if (!root.expanded)
                    return;
                webviewLoader.active = true;
                if (webviewLoader.item) {
                    webviewLoader.item.webview.active = true;
                    webviewLoader.item.webview.forceActiveFocus();
                }
            }

            target: root
        }
    }
    }
}
