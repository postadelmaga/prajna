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
import org.kde.kcmutils as KCM

import "sites.js" as Sites

// Which chat services the header offers, and which one opens first.
KCM.SimpleKCM {
    id: page

    property string cfg_url
    property string cfg_customSites
    property bool cfg_hideCustomURL

    property bool cfg_showChatGPT
    property bool cfg_showClaude
    property bool cfg_showGoogleGemini
    property bool cfg_showDeepSeek
    property bool cfg_showDuckDuckGoChat
    property bool cfg_showBingCopilot
    property bool cfg_showPerplexity
    property bool cfg_showGrok
    property bool cfg_showMetaAI
    property bool cfg_showHugginChat
    property bool cfg_showT3Chat
    property bool cfg_showYou
    property bool cfg_showBlackBox
    property bool cfg_showLobeChat
    property bool cfg_showBigAGI

    readonly property var customSites: Sites.parseCustom(cfg_customSites)

    // Everything the start page can be: enabled built-ins + custom sites.
    readonly property var startChoices: {
        const list = Sites.predefined.filter(s => page["cfg_" + s.prop] || s.url === cfg_url)
            .map(s => ({ text: s.text, url: s.url }))
            .concat(customSites);
        if (cfg_url && !list.some(s => s.url === cfg_url))
            list.push({ text: cfg_url, url: cfg_url });
        return list;
    }

    function addCustomSite() {
        const name = nameField.text.trim();
        const url = Sites.sanitizeUrl(urlField.text);
        if (!name || !url)
            return;
        if (customSites.some(s => s.url === url || s.text === name))
            return;
        cfg_customSites = Sites.serializeCustom(customSites.concat([{ text: name, url: url }]));
        nameField.text = "";
        urlField.text = "";
        nameField.forceActiveFocus();
    }

    function removeCustomSite(index) {
        const list = customSites.slice();
        list.splice(index, 1);
        cfg_customSites = Sites.serializeCustom(list);
    }

    Kirigami.FormLayout {
        QQC2.ComboBox {
            Kirigami.FormData.label: i18n("Start page:")
            Layout.minimumWidth: Kirigami.Units.gridUnit * 14
            model: page.startChoices
            textRole: "text"
            valueRole: "url"
            currentIndex: page.startChoices.findIndex(s => s.url === page.cfg_url)
            onActivated: page.cfg_url = currentValue
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Sites in the list")
        }

        GridLayout {
            Kirigami.FormData.label: i18n("Built-in:")
            columns: 2
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: 0

            Repeater {
                model: Sites.predefined

                QQC2.CheckBox {
                    required property var modelData
                    text: modelData.text
                    checked: page["cfg_" + modelData.prop]
                    onToggled: page["cfg_" + modelData.prop] = checked
                }
            }
        }

        QQC2.CheckBox {
            text: i18n("\"Custom URL…\" entry to type any address")
            checked: !page.cfg_hideCustomURL
            onToggled: page.cfg_hideCustomURL = !checked
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Your sites")
        }

        Repeater {
            model: page.customSites

            RowLayout {
                required property var modelData
                required property int index
                Kirigami.FormData.label: index === 0 ? i18n("Added:") : ""
                Layout.fillWidth: true

                Kirigami.Icon {
                    source: "internet-web-browser"
                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                }
                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true
                    QQC2.Label {
                        text: modelData.text
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                    }
                    QQC2.Label {
                        text: modelData.url
                        font: Kirigami.Theme.smallFont
                        opacity: 0.7
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                    }
                }
                QQC2.ToolButton {
                    icon.name: "edit-delete-remove"
                    display: QQC2.AbstractButton.IconOnly
                    text: i18n("Remove %1", modelData.text)
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    onClicked: page.removeCustomSite(index)
                }
            }
        }

        RowLayout {
            Kirigami.FormData.label: page.customSites.length ? i18n("Add:") : i18n("Add a site:")
            Layout.fillWidth: true

            QQC2.TextField {
                id: nameField
                placeholderText: i18n("Name")
                Layout.preferredWidth: Kirigami.Units.gridUnit * 7
                onAccepted: urlField.forceActiveFocus()
            }
            QQC2.TextField {
                id: urlField
                placeholderText: i18n("Address, e.g. chat.mistral.ai")
                Layout.fillWidth: true
                Layout.minimumWidth: Kirigami.Units.gridUnit * 10
                onAccepted: page.addCustomSite()
            }
            QQC2.Button {
                icon.name: "list-add"
                text: i18n("Add")
                enabled: nameField.text.trim() !== "" && urlField.text.trim() !== ""
                onClicked: page.addCustomSite()
            }
        }
    }
}
