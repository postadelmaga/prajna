/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM

// The out-of-process WebKit engine: when it runs and how much it may use.
KCM.SimpleKCM {
    id: page

    property bool cfg_loadOnStartup
    property int cfg_unloadAfterMinutes
    property bool cfg_gpuRendering
    property bool cfg_lowMemoryJavascript
    property int cfg_maxFps
    property int cfg_memoryLimitMb
    property string cfg_userAgent
    property bool cfg_spatialNavigationEnabled

    component Hint: QQC2.Label {
        Layout.fillWidth: true
        Layout.maximumWidth: Kirigami.Units.gridUnit * 22
        wrapMode: Text.Wrap
        font: Kirigami.Theme.smallFont
        opacity: 0.7
    }

    Kirigami.FormLayout {
        QQC2.CheckBox {
            Kirigami.FormData.label: i18n("Start:")
            text: i18n("Load the chat when Plasma starts")
            checked: page.cfg_loadOnStartup
            onToggled: page.cfg_loadOnStartup = checked
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: i18n("Free memory after:")
            enabled: !page.cfg_loadOnStartup
            readonly property var minutes: [1, 5, 15, 30, 60, 0]
            model: minutes.map(m => m === 0 ? i18n("Never") : i18np("%1 minute closed", "%1 minutes closed", m))
            currentIndex: {
                const i = minutes.indexOf(page.cfg_unloadAfterMinutes);
                return i >= 0 ? i : 2;
            }
            onActivated: index => page.cfg_unloadAfterMinutes = minutes[index]
        }
        Hint {
            text: page.cfg_loadOnStartup
                ? i18n("The chat stays loaded while \"Load when Plasma starts\" is on.")
                : i18n("While the popup is closed the page is paused; after this time the engine stops and all its memory is freed. Your login is kept.")
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Performance")
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18n("Drawing:")
            text: i18n("Use the integrated GPU")
            checked: page.cfg_gpuRendering
            onToggled: page.cfg_gpuRendering = checked
        }
        Hint {
            text: i18n("About half the memory of drawing on the CPU. Never uses a discrete NVIDIA card, which stays asleep.")
        }

        QQC2.ComboBox {
            Kirigami.FormData.label: i18n("Frame rate:")
            readonly property var rates: [15, 20, 30, 60]
            model: rates.map(r => r === 30 ? i18n("%1 fps (recommended)", r) : i18n("%1 fps", r))
            currentIndex: Math.max(0, rates.indexOf(page.cfg_maxFps))
            onActivated: index => page.cfg_maxFps = rates[index]
        }
        Hint {
            text: i18n("Lower values save CPU during animations such as the \"thinking\" spinner.")
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18n("JavaScript:")
            text: i18n("Save memory (slower pages)")
            checked: page.cfg_lowMemoryJavascript
            onToggled: page.cfg_lowMemoryJavascript = checked
        }
        Hint {
            text: i18n("Turns off the JavaScript compiler: about 150–200 MB less on large chat sites, but pages react more slowly.")
        }

        QQC2.SpinBox {
            Kirigami.FormData.label: i18n("Memory limit:")
            from: 0
            to: 16384
            stepSize: 256
            value: page.cfg_memoryLimitMb
            textFromValue: (v, locale) => v === 0 ? i18n("None") : i18n("%1 MB", v)
            valueFromText: (t, locale) => parseInt(t) || 0
            onValueModified: page.cfg_memoryLimitMb = value
        }
        Hint {
            text: i18n("Approaching the limit, the engine frees caches and memory more aggressively.")
        }

        Kirigami.Separator {
            Kirigami.FormData.isSection: true
            Kirigami.FormData.label: i18n("Advanced")
        }

        QQC2.TextField {
            Kirigami.FormData.label: i18n("User agent:")
            Layout.fillWidth: true
            Layout.minimumWidth: Kirigami.Units.gridUnit * 16
            text: page.cfg_userAgent
            placeholderText: i18n("Default (Safari compatible)")
            onEditingFinished: page.cfg_userAgent = text.trim()
        }

        QQC2.CheckBox {
            Kirigami.FormData.label: i18n("Keyboard:")
            text: i18n("Move between links and buttons with the arrow keys")
            checked: page.cfg_spatialNavigationEnabled
            onToggled: page.cfg_spatialNavigationEnabled = checked
        }
    }
}
