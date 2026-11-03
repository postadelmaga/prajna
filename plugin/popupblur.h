/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QPointer>
#include <QQuickItem>
#include <QWindow>

/*
 * Turns KWin's blur behind the window this item lives in (the plasmoid
 * popup) on or off, independently of the Plasma theme.
 *
 * Plasma sets the popup's blur itself, with the shape of the dialog frame,
 * whenever that shape changes (the frame item's maskChanged). PopupBlur
 * listens to the same signal and re-applies its choice right after Plasma,
 * using the same shape when turning blur back on.
 *
 *   PopupBlur { blur: plasmoid.configuration.blurBehind }
 */
class PopupBlur : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool blur READ blur WRITE setBlur NOTIFY blurChanged)
    // Whether KWin's blur effect is available at all (Desktop Effects).
    Q_PROPERTY(bool available READ isAvailable NOTIFY availableChanged)

public:
    explicit PopupBlur(QQuickItem *parent = nullptr);

    bool blur() const { return m_blur; }
    void setBlur(bool on);
    bool isAvailable() const;

Q_SIGNALS:
    void blurChanged();
    void availableChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private Q_SLOTS:
    void schedule();

private:
    void attach(QQuickWindow *window);
    void apply();
    QObject *findFrame(QQuickItem *item) const;

    bool m_blur = true;
    bool m_restore = false; // blur was turned back on: re-apply once
    bool m_pending = false;
    QPointer<QQuickWindow> m_window;
    QPointer<QObject> m_frame;
    QList<QMetaObject::Connection> m_connections;
};
