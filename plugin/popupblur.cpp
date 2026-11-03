/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "popupblur.h"

#include <KWindowEffects>
#include <QLoggingCategory>
#include <QMetaMethod>
#include <QQuickWindow>
#include <QRegion>
#include <QTimer>

Q_LOGGING_CATEGORY(lcBlur, "chatai.blur", QtInfoMsg)

PopupBlur::PopupBlur(QQuickItem *parent)
    : QQuickItem(parent)
{
    updateAvailable();
    for (int ms : {200, 1000, 3000})
        QTimer::singleShot(ms, this, &PopupBlur::updateAvailable);
}

// On Wayland KWin announces the blur protocol asynchronously, so the first
// answer can be "no": check again shortly after.
void PopupBlur::updateAvailable()
{
    const bool now = KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    if (now != m_available) {
        m_available = now;
        Q_EMIT availableChanged();
    }
}

void PopupBlur::setBlur(bool on)
{
    if (m_blur == on)
        return;
    m_blur = on;
    m_restore = on;
    Q_EMIT blurChanged();
    schedule();
}

void PopupBlur::itemChange(ItemChange change, const ItemChangeData &data)
{
    if (change == ItemSceneChange)
        attach(data.window);
    QQuickItem::itemChange(change, data);
}

void PopupBlur::attach(QQuickWindow *window)
{
    for (const auto &c : std::as_const(m_connections))
        disconnect(c);
    m_connections.clear();
    m_window = window;
    m_frame = nullptr;
    updateAvailable();
    if (!window)
        return;
    // Plasma re-applies its own blur on these; follow right after it.
    m_connections << connect(window, &QWindow::visibleChanged, this, &PopupBlur::schedule)
                  << connect(window, &QWindow::widthChanged, this, &PopupBlur::schedule)
                  << connect(window, &QWindow::heightChanged, this, &PopupBlur::schedule);
    schedule();
}

// The dialog background (a KSvg FrameSvgItem with a `mask` property) that
// Plasma takes the blur shape from.
QObject *PopupBlur::findFrame(QQuickItem *item) const
{
    if (!item)
        return nullptr;
    const QMetaObject *mo = item->metaObject();
    if (mo->indexOfProperty("mask") >= 0 && mo->indexOfSignal("maskChanged()") >= 0
        && QByteArray(mo->className()).contains("FrameSvgItem"))
        return item;
    const auto children = item->childItems();
    for (QQuickItem *child : children)
        if (QObject *found = findFrame(child))
            return found;
    return nullptr;
}

void PopupBlur::schedule()
{
    if (m_pending)
        return;
    m_pending = true;
    // Queued: runs after Plasma's own (direct) reaction to the same change.
    QTimer::singleShot(0, this, [this] {
        m_pending = false;
        apply();
    });
}

void PopupBlur::apply()
{
    if (!m_window)
        return;
    if (!m_frame) {
        m_frame = findFrame(m_window->contentItem());
        if (m_frame) {
            const QMetaObject *mo = m_frame->metaObject();
            const QMetaMethod changed = mo->method(mo->indexOfSignal("maskChanged()"));
            const QMetaMethod slot = metaObject()->method(metaObject()->indexOfMethod("schedule()"));
            if (changed.isValid() && slot.isValid())
                m_connections << connect(m_frame, changed, this, slot);
        }
        if (m_frame)
            qCInfo(lcBlur) << "popup frame found, blur" << m_blur;
    }
    if (m_blur) {
        // Plasma's blur is the default: only restore it after we removed it.
        if (!m_restore)
            return;
        m_restore = false;
        qCInfo(lcBlur) << "blur restored";
        const QRegion shape = m_frame ? m_frame->property("mask").value<QRegion>() : QRegion();
        KWindowEffects::enableBlurBehind(m_window, true, shape);
    } else {
        KWindowEffects::enableBlurBehind(m_window, false);
    }
}
