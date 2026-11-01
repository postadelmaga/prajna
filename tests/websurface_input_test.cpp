/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// End-to-end check of WebSurface: starts the real engine on a data: page,
// feeds Qt input events (with native keysyms/keycodes, as Wayland delivers
// them) and reads the result back through runJavaScript. No network needed.
// Run with QT_QPA_PLATFORM=offscreen.

#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <xkbcommon/xkbcommon-keysyms.h>

class WebSurfaceInputTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_web = nullptr;
    QTemporaryDir m_profile;

    QString js(const QString &script)
    {
        // The callback stores the answer on the item; poll for it.
        m_web->setProperty("testResult", QVariant());
        QJSValue setter = m_engine->evaluate(QStringLiteral("(function (item) { return function (r) { item.testResult = r; }; })"))
                              .call({m_engine->newQObject(m_web)});
        QMetaObject::invokeMethod(m_web, "runJavaScript", Q_ARG(QString, script), Q_ARG(QJSValue, setter));
        if (!QTest::qWaitFor([&] { return m_web->property("testResult").isValid(); }, 5000))
            return QStringLiteral("<timeout>");
        return m_web->property("testResult").toString();
    }

    void key(quint32 keysym, quint32 keycode, Qt::Key qtKey, const QString &text, Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        QKeyEvent press(QEvent::KeyPress, qtKey, mods, keycode, keysym, 0, text);
        QKeyEvent release(QEvent::KeyRelease, qtKey, mods, keycode, keysym, 0, text);
        QCoreApplication::sendEvent(m_web, &press);
        QCoreApplication::sendEvent(m_web, &release);
        QTest::qWait(30);
    }

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_profile.isValid());
        m_engine = new QQmlEngine(this);
        m_engine->addImportPath(QStringLiteral(CHATAI_QML_IMPORT_DIR));
        QQmlComponent c(m_engine);
        c.setData(R"(
            import QtQuick
            import org.kde.plasma.private.chatai
            WebSurface {
                property var testResult
                width: 400; height: 300
                url: "data:text/html,<textarea id=t style='width:380px;height:200px'></textarea>"
            }
        )",
                  QUrl());
        QObject *obj = c.beginCreate(m_engine->rootContext());
        QVERIFY2(obj, qPrintable(c.errorString()));
        m_web = qobject_cast<QQuickItem *>(obj);
        m_web->setProperty("profileDirectory", m_profile.path());
        m_window = new QQuickWindow;
        m_window->resize(400, 300);
        m_web->setParentItem(m_window->contentItem());
        c.completeCreate();
        m_window->show();
        QVERIFY(QTest::qWaitFor([&] { return m_web->property("running").toBool(); }, 15000));
        QVERIFY(QTest::qWaitFor([&] { return !m_web->property("loading").toBool() && m_web->property("loadProgress").toInt() == 100; }, 15000));
    }

    void cleanupTestCase()
    {
        delete m_web;
        delete m_window;
    }

    void typing()
    {
        // click into the textarea → focus both in Qt and in the page
        QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, QPoint(50, 50));
        QTest::qWait(200);
        QCOMPARE(js(QStringLiteral("document.activeElement.id")), QStringLiteral("t"));

        // keycodes are evdev+8 (XKB), as Qt reports them on Wayland
        key(XKB_KEY_c, 54, Qt::Key_C, QStringLiteral("c"));
        key(XKB_KEY_i, 31, Qt::Key_I, QStringLiteral("i"));
        key(XKB_KEY_a, 38, Qt::Key_A, QStringLiteral("a"));
        key(XKB_KEY_o, 32, Qt::Key_O, QStringLiteral("o"));
        key(XKB_KEY_A, 38, Qt::Key_A, QStringLiteral("A"), Qt::ShiftModifier);
        // Italian layout: è has its own keysym
        key(XKB_KEY_egrave, 34, Qt::Key_Egrave, QStringLiteral("è"));
        // dead key + e composed by the client into ê
        key(XKB_KEY_dead_circumflex, 21, Qt::Key_Dead_Circumflex, QString());
        key(XKB_KEY_e, 26, Qt::Key_E, QStringLiteral("ê"));
        key(XKB_KEY_BackSpace, 22, Qt::Key_Backspace, QStringLiteral("\b"));
        key(XKB_KEY_Return, 36, Qt::Key_Return, QStringLiteral("\r"));
        key(XKB_KEY_z, 52, Qt::Key_Z, QStringLiteral("z"));
        QTest::qWait(200);
        QCOMPARE(js(QStringLiteral("document.getElementById('t').value")), QStringLiteral("ciaoAè\nz"));
    }

    void selectAllAndDelete()
    {
        key(XKB_KEY_a, 38, Qt::Key_A, QStringLiteral("\x01"), Qt::ControlModifier);
        key(XKB_KEY_Delete, 119, Qt::Key_Delete, QStringLiteral("\x7f"));
        QTest::qWait(200);
        QCOMPARE(js(QStringLiteral("document.getElementById('t').value")), QString());
    }

    void inputMethodCommit()
    {
        QInputMethodEvent ev;
        ev.setCommitString(QStringLiteral("日本語"));
        QCoreApplication::sendEvent(m_web, &ev);
        QTest::qWait(200);
        QCOMPARE(js(QStringLiteral("document.getElementById('t').value")), QStringLiteral("日本語"));
    }

    void framesArrive()
    {
        // the item must have received at least one frame from the engine
        const QImage grab = m_window->grabWindow();
        QVERIFY(!grab.isNull());
        QCOMPARE(grab.pixelColor(390, 290).alpha(), 255);
    }
};

QTEST_MAIN(WebSurfaceInputTest)
#include "websurface_input_test.moc"
