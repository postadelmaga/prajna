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
#include <QStorageInfo>
#include <QStandardPaths>
#include <QTest>
#include <rhi/qrhi.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <QDir>
#include <unistd.h>

// RssShmem (kB) of our prajna-engine child: the frame slots it touched.
static qint64 engineShmemKb()
{
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &p : pids) {
        QFile stat(QStringLiteral("/proc/%1/stat").arg(p)), comm(QStringLiteral("/proc/%1/comm").arg(p));
        if (!stat.open(QIODevice::ReadOnly) || !comm.open(QIODevice::ReadOnly) || comm.readAll().trimmed() != "prajna-engine")
            continue;
        const QByteArray s = stat.readAll();
        if (s.mid(s.lastIndexOf(')') + 2).split(' ').value(1).toInt() != getpid())
            continue;
        QFile status(QStringLiteral("/proc/%1/status").arg(p));
        if (status.open(QIODevice::ReadOnly))
            for (const QByteArray &l : status.readAll().split('\n'))
                if (l.startsWith("RssShmem:"))
                    return l.mid(9).trimmed().split(' ').value(0).toLongLong();
    }
    return -1;
}

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
        m_engine->addImportPath(QStringLiteral(PRAJNA_QML_IMPORT_DIR));
        QQmlComponent c(m_engine);
        c.setData(R"(
            import QtQuick
            import io.github.postadelmaga.prajna
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
        // A real compositor only renders windows it shows: with the screen
        // off or locked nothing would be drawn (offscreen always renders).
        if (qEnvironmentVariableIsSet("PRAJNA_TEST_REQUIRE_RHI")) {
            // two frames: the second one needs the compositor's frame
            // callback, which never comes while the outputs are off
            QSignalSpy swapped(m_window, &QQuickWindow::frameSwapped);
            for (int i = 0; i < 2; ++i) {
                m_window->update();
                if (!QTest::qWaitFor([&] { return swapped.count() > i; }, 3000))
                    QSKIP("the compositor doesn't let the window render (screen off?): the RHI path can't be tested");
            }
        }
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
        QVERIFY(QTest::qWaitFor([&] { return js(QStringLiteral("document.readyState")) == QLatin1String("complete"); }, 5000));
        bool focused = false;
        for (int i = 0; i < 5 && !focused; ++i) {
            QTest::mouseClick(m_window, Qt::LeftButton, Qt::NoModifier, QPoint(50, 50));
            QTest::qWait(150);
            focused = js(QStringLiteral("document.activeElement.id")) == QLatin1String("t");
        }
        QVERIFY2(focused, "click did not focus the textarea");

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

    void partialUpdatesMatchFullCopy()
    {
        qInfo() << "scene graph:" << (m_window->rhi() ? m_window->rhi()->backendName() : "software");
        // the partial-upload path (FrameTexture) needs the RHI; offscreen
        // runs only cover the QImage fallback
        if (qEnvironmentVariableIsSet("PRAJNA_TEST_REQUIRE_RHI"))
            QVERIFY2(m_window->rhi(), "no RHI scene graph: FrameTexture not exercised");
        // Many small random repaints → damage-limited copies and uploads.
        m_web->setProperty("testResult", QVariant());
        QMetaObject::invokeMethod(m_web, "load", Q_ARG(QUrl, QUrl(QStringLiteral(
            "data:text/html,<body style='margin:0;font:14px sans-serif'><div id=g style='display:grid;grid-template-columns:repeat(8,48px)'></div>"
            "<script>const g=document.getElementById('g');const c=[];for(let i=0;i<64;i++){const d=document.createElement('div');"
            "d.style.height='30px';d.textContent='.';g.appendChild(d);c.push(d);}let s=0,seed=7;const r=()=>(seed=(seed*16807)%2147483647)/2147483647;"
            "const t=setInterval(()=>{for(let k=0;k<3;k++){const e=c[Math.floor(r()*64)];e.textContent=String(s%97);"
            "e.style.background='hsl('+(s*37%360)+' 60% 70%)';}if(++s==120){clearInterval(t);document.title='done';}},12);</script>"))));
        QVERIFY(QTest::qWaitFor([&] { return m_web->property("title").toString() == QLatin1String("done"); }, 15000));
        QTest::qWait(500);
        const QImage partial = m_window->grabWindow();

        // Hide/show keeps the texture and catches up with the damage.
        QVERIFY(m_web->property("frameResident").toBool());
        m_web->setProperty("suspended", true);
        QVERIFY(QTest::qWaitFor([&] { return !m_web->property("frameResident").toBool(); }, 3000));
        m_web->setProperty("suspended", false);
        QVERIFY(QTest::qWaitFor([&] { return m_web->property("frameResident").toBool(); }, 3000));
        QTest::qWait(200);
        QVERIFY2(m_window->grabWindow() == partial, "hide/show changed the view");

        // A new scene-graph node copies the whole frame from the shm slot.
        m_web->setParentItem(nullptr);
        QTest::qWait(100);
        m_web->setParentItem(m_window->contentItem());
        QVERIFY(QTest::qWaitFor([&] { return m_web->property("frameResident").toBool(); }, 3000));
        QTest::qWait(300);
        const QImage full = m_window->grabWindow();

        if (partial != full && qEnvironmentVariableIsSet("PRAJNA_TEST_DUMP")) {
            partial.save(qEnvironmentVariable("PRAJNA_TEST_DUMP") + QStringLiteral("/partial.png"));
            full.save(qEnvironmentVariable("PRAJNA_TEST_DUMP") + QStringLiteral("/full.png"));
            QRect diff;
            for (int y = 0; y < qMin(partial.height(), full.height()); ++y)
                for (int x = 0; x < qMin(partial.width(), full.width()); ++x)
                    if (partial.pixel(x, y) != full.pixel(x, y))
                        diff |= QRect(x, y, 1, 1);
            qWarning() << "diff region" << diff << "sizes" << partial.size() << full.size();
        }
        QCOMPARE(partial.size(), full.size());
        QVERIFY2(partial == full, "partial updates diverged from a full copy");
    }

    void quickHideShowKeepsFrame()
    {
        // Hide immediately followed by show, before a render can happen:
        // the frame must stay visible (regression: the item went blank).
        const QImage before = m_window->grabWindow();
        m_web->setProperty("suspended", true);
        m_web->setProperty("suspended", false);
        QTest::qWait(300);
        QVERIFY(m_web->property("frameResident").toBool());
        QVERIFY2(m_window->grabWindow() == before, "quick hide/show left the view blank");
    }

    void releasedSlotsKeepFrame()
    {
        // Hidden long enough, the engine gives the memory of the unpublished
        // shm slot back (after PRAJNA_SLOT_RELEASE_DELAY_S=1 s here).
        // The view must come back unchanged, also when its texture has to
        // be rebuilt, and keep receiving frames.
        const QImage before = m_window->grabWindow();
        const qint64 shmBefore = engineShmemKb();
        m_web->setProperty("suspended", true);
        // g_timeout_add_seconds() fires on whole-second boundaries: 1-2 s
        qint64 shmHidden = shmBefore;
        (void)QTest::qWaitFor([&] { return (shmHidden = engineShmemKb()) < shmBefore; }, 5000);
        qInfo() << "engine shm while shown" << shmBefore << "kB, hidden" << shmHidden << "kB";
        // only tmpfs gives pages back (a runtime dir on disk, as in some
        // containers, isn't shared memory at all)
        if (QStorageInfo(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).fileSystemType() == "tmpfs")
            QVERIFY2(shmHidden < shmBefore, "the unpublished slot was not released");
        else
            qInfo() << "runtime dir not on tmpfs: release not checked";
        m_web->setProperty("suspended", false);
        QTest::qWait(300);
        const QImage after = m_window->grabWindow();
        if (after != before && qEnvironmentVariableIsSet("PRAJNA_TEST_DUMP")) {
            before.save(qEnvironmentVariable("PRAJNA_TEST_DUMP") + QStringLiteral("/before.png"));
            after.save(qEnvironmentVariable("PRAJNA_TEST_DUMP") + QStringLiteral("/after.png"));
        }
        QVERIFY2(after == before, "the view changed after the slots were released");

        m_web->setProperty("suspended", true);
        const qint64 shmShown = engineShmemKb();
        (void)QTest::qWaitFor([&] { return engineShmemKb() < shmShown; }, 5000);
        m_web->setParentItem(nullptr); // the texture is gone: a full copy is needed
        QTest::qWait(100);
        m_web->setProperty("suspended", false);
        m_web->setParentItem(m_window->contentItem());
        QTRY_VERIFY_WITH_TIMEOUT(m_window->grabWindow() == before, 3000);

        QCOMPARE(js(QStringLiteral("document.body.style.background = 'rgb(10, 200, 30)', 'ok'")), QStringLiteral("ok"));
        QTRY_COMPARE_WITH_TIMEOUT(m_window->grabWindow().pixelColor(395, 295), QColor(10, 200, 30), 3000);
        // Small changes land in both slots (the released one was refilled
        // whole); a texture rebuilt from the slot must match what partial
        // updates produced.
        for (int i = 0; i < 2; ++i) {
            QCOMPARE(js(QStringLiteral("(() => { const d = document.createElement('div'); d.textContent = 'x%1';"
                                       " d.style.cssText = 'position:fixed;left:%2px;top:5px'; document.body.appendChild(d); return 'ok' })()")
                            .arg(i)
                            .arg(5 + 30 * i)),
                     QStringLiteral("ok"));
            QTest::qWait(300);
        }
        const QImage partial = m_window->grabWindow();
        m_web->setParentItem(nullptr);
        QTest::qWait(100);
        m_web->setParentItem(m_window->contentItem());
        QTRY_VERIFY_WITH_TIMEOUT(m_window->grabWindow() == partial, 3000);
    }

    // Last: leaves extra tabs behind until it cleans up.
    void tabs()
    {
        auto count = [&] { return m_web->property("tabs").toList().size(); };
        auto current = [&] { return m_web->property("currentTab").toInt(); };
        auto title = [&] { return m_web->property("title").toString(); };
        QCOMPARE(count(), 1);
        m_web->setProperty("linksInTabs", true);

        // new tab: appended and shown
        QMetaObject::invokeMethod(m_web, "newTab",
                                  Q_ARG(QUrl, QUrl(QStringLiteral("data:text/html,<title>Two</title><a id=l target=_blank href='about:blank%23link'>x</a>"))));
        QTRY_COMPARE_WITH_TIMEOUT(count(), 2, 5000);
        QCOMPARE(current(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Two"), 5000);

        // a target=_blank link opens a tab right after its page
        QCOMPARE(js(QStringLiteral("document.getElementById('l').click(), 'ok'")), QStringLiteral("ok"));
        QTRY_COMPARE_WITH_TIMEOUT(count(), 3, 5000);
        QCOMPARE(current(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(m_web->property("url").toUrl().fragment(), QStringLiteral("link"), 5000);
        QVERIFY(!m_web->property("tabs").toList().at(2).toMap().value(QStringLiteral("popup")).toBool());
        // (pages may not open data: URLs at top level: replace it with one)
        QMetaObject::invokeMethod(m_web, "load", Q_ARG(QUrl, QUrl(QStringLiteral("data:text/html,<title>Three</title>3"))));
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Three"), 5000);

        // switching updates the page state
        QMetaObject::invokeMethod(m_web, "switchTab", Q_ARG(int, 1));
        QTRY_COMPARE_WITH_TIMEOUT(current(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Two"), 5000);
        QCOMPARE(m_web->property("tabUrls").toStringList().size(), 3);

        // an engine restart reopens the tabs and the current one
        QMetaObject::invokeMethod(m_web, "restartEngine");
        QTRY_VERIFY_WITH_TIMEOUT(!m_web->property("running").toBool(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(m_web->property("running").toBool(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(count(), 3, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(current(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Two"), 5000);
        // the other restored tab knows its address but is not loaded yet
        auto tab = [&](int i) { return m_web->property("tabs").toList().at(i).toMap(); };
        QTest::qWait(500);
        QVERIFY(tab(2).value(QStringLiteral("title")).toString().isEmpty());
        QVERIFY(tab(2).value(QStringLiteral("url")).toUrl().toString().contains(QLatin1String("Three")));
        QCOMPARE(m_web->property("tabUrls").toStringList().size(), 3);
        // ...until shown
        QMetaObject::invokeMethod(m_web, "switchTab", Q_ARG(int, 2));
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Three"), 5000);
        QMetaObject::invokeMethod(m_web, "switchTab", Q_ARG(int, 1));
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Two"), 5000);

        // closing the current tab shows its neighbour
        QMetaObject::invokeMethod(m_web, "closeTab", Q_ARG(int, 1));
        QTRY_COMPARE_WITH_TIMEOUT(count(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(title(), QStringLiteral("Three"), 5000);
        QCOMPARE(current(), 1);
        QMetaObject::invokeMethod(m_web, "closeTab", Q_ARG(int, 1));
        QTRY_COMPARE_WITH_TIMEOUT(count(), 1, 5000);
        // the last tab cannot be closed
        QMetaObject::invokeMethod(m_web, "closeTab", Q_ARG(int, 0));
        QTest::qWait(300);
        QCOMPARE(count(), 1);
    }
};

QTEST_MAIN(WebSurfaceInputTest)
#include "websurface_input_test.moc"
