/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// clear_background.js against the real engine: which elements get cleared
// and when, after the kinds of changes chat sites make (new nodes, class and
// state changes, surfaces that stop being surfaces, hidden parts shown,
// style sheets, theme switches), and that catch-all walks are debounced and
// sliced: no long main-thread task on a large page. Run with
// QT_QPA_PLATFORM=offscreen; no network needed.

#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

class TransparentBackgroundTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
    QQuickItem *m_web = nullptr;
    QTemporaryDir m_profile;

    QString js(const QString &script)
    {
        m_web->setProperty("testResult", QVariant());
        QJSValue setter = m_engine->evaluate(QStringLiteral("(function (item) { return function (r) { item.testResult = r; }; })"))
                              .call({m_engine->newQObject(m_web)});
        QMetaObject::invokeMethod(m_web, "runJavaScript", Q_ARG(QString, script), Q_ARG(QJSValue, setter));
        if (!QTest::qWaitFor([&] { return m_web->property("testResult").isValid(); }, 5000))
            return QStringLiteral("<timeout>");
        return m_web->property("testResult").toString();
    }

    // "1" when the element is cleared (either mark), "0" otherwise
    QString cleared(const char *id)
    {
        return js(QStringLiteral("(e => e && (e.hasAttribute('data-prajna-clear') || e.hasAttribute('data-prajna-clear-i')) ? '1' : '0')"
                                 "(document.getElementById('%1'))")
                      .arg(QLatin1String(id)));
    }

    int stat(const char *name)
    {
        return js(QStringLiteral("String(Math.round(__prajnaClear.stats.%1))").arg(QLatin1String(name))).toInt();
    }

    void load(const QString &html)
    {
        QMetaObject::invokeMethod(m_web, "load", Q_ARG(QUrl, QUrl(QStringLiteral("data:text/html,") + html)));
        QVERIFY(QTest::qWaitFor([&] { return js(QStringLiteral("document.title")) == QLatin1String("ready"); }, 10000));
        // past the load-time walks (kick windows)
        QTest::qWait(3500);
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
                transparentBackground: true
                url: "about:blank"
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
    }

    void cleanupTestCase()
    {
        delete m_web;
        delete m_window;
    }

    void invalidation()
    {
        load(QStringLiteral(
            "<title>ready</title><style>body{background:%23202020;margin:0;color:white}"
            ".base{background:%23202020}.surf{background:%23505050}.route %23a{background:%23505050}.clear{background:transparent}.hide{display:none}</style>"
            "<div id=a class=base>a</div>"
            "<div id=s class=surf><div id=s1 class=base>inside a surface</div></div>"
            "<div id=h class=hide><div id=h1 class=base>hidden</div></div>"
            "<div id=c>plain</div><div id=d class=late>late</div>"
            "<div id=w style='background:%23f0f0f0'>light</div>"
            "<button id=btn class=base>button</button>"
            "<div id=editor class=surf><div id=ed contenteditable=true>x</div></div>"));

        // first walk
        QCOMPARE(cleared("a"), QStringLiteral("1"));
        QCOMPARE(cleared("btn"), QStringLiteral("1"));
        QCOMPARE(js(QStringLiteral("String(document.getElementById('btn').hasAttribute('data-prajna-clear-i'))")), QStringLiteral("true"));
        QCOMPARE(cleared("s1"), QStringLiteral("0"));
        QCOMPARE(cleared("h1"), QStringLiteral("0"));
        QCOMPARE(cleared("c"), QStringLiteral("0"));
        QCOMPARE(cleared("w"), QStringLiteral("0"));

        // Changes are handled before the next paint, well before the
        // catch-all walk (QUIET_MS = 1500 ms) could have run.
        // new node
        js(QStringLiteral("(() => { const n = document.createElement('div'); n.id = 'n'; n.className = 'base';"
                          " n.textContent = 'new'; document.body.appendChild(n); return 1 })()"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("n"), QStringLiteral("1"), 800);
        // class change on an existing node
        js(QStringLiteral("document.getElementById('c').className = 'base', 1"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("c"), QStringLiteral("1"), 800);
        // a surface stops being one: its subtree is now on the page background
        js(QStringLiteral("document.getElementById('s').className = 'clear', 1"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("s1"), QStringLiteral("1"), 800);
        // a hidden part is shown
        js(QStringLiteral("document.getElementById('h').className = '', 1"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("h1"), QStringLiteral("1"), 800);

        // A style sheet rule no mutation reports: picked up by the debounced
        // catch-all walk that the next DOM change elsewhere requests.
        // (wait out the catch-all the class changes above requested)
        QTest::qWait(5500);
        js(QStringLiteral("__prajnaClear.stats.reset(), 1"));
        QTest::qWait(2000);
        QCOMPARE(stat("fullScans"), 0);
        js(QStringLiteral("document.styleSheets[0].insertRule('.late{background:#202020}', 0),"
                          " document.body.appendChild(document.createElement('p')), 1"));
        QCOMPARE(cleared("d"), QStringLiteral("0"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("d"), QStringLiteral("1"), 4000);

        // A route/state class on <body> that keeps the page colour but
        // paints a cleared element: its mark must go, before the next paint.
        js(QStringLiteral("document.body.className = 'route', 1"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("a"), QStringLiteral("0"), 500);
        QCOMPARE(cleared("c"), QStringLiteral("1")); // still base-coloured: re-cleared
        js(QStringLiteral("document.body.className = '', 1"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("a"), QStringLiteral("1"), 800);

        // Theme switch: the page colour changes, every mark is redone at once.
        js(QStringLiteral("document.body.style.background = '#f0f0f0', 1"));
        QTRY_COMPARE_WITH_TIMEOUT(cleared("w"), QStringLiteral("1"), 1000);
        QCOMPARE(cleared("a"), QStringLiteral("0"));
        QCOMPARE(cleared("c"), QStringLiteral("0"));
    }

    void catchAllIsDebounced()
    {
        js(QStringLiteral("document.body.style.background = '#202020', 1"));
        QTest::qWait(2500);
        js(QStringLiteral("__prajnaClear.stats.reset(), 1"));
        // Typing inside a surface (the composer): no catch-all walk at all.
        js(QStringLiteral("(() => { const e = document.getElementById('ed'); e.focus();"
                          " for (let i = 0; i < 10; i++) document.execCommand('insertHTML', false, '<b>abc</b> ');"
                          " return document.getElementById('ed').getElementsByTagName('b').length })()"));
        QTest::qWait(2200);
        QCOMPARE(stat("fullScans"), 0);
        // A burst of changes on the page background: one walk, after it.
        js(QStringLiteral("(() => { let i = 0; const t = setInterval(() => { const p = document.createElement('p');"
                          " p.textContent = i; document.body.appendChild(p); if (++i == 10) clearInterval(t); }, 100); return 1 })()"));
        QTest::qWait(1000);
        QCOMPARE(stat("fullScans"), 0); // still changing: nothing yet
        QTRY_COMPARE_WITH_TIMEOUT(stat("fullScans"), 1, 3000);
        QTest::qWait(2000);
        QCOMPARE(stat("fullScans"), 1);
    }

    void largePageHasNoLongTasks()
    {
        // ~24k elements on the page background, like a long conversation
        const QString build = QStringLiteral(
            "(() => { const blk = '<div><div><div class=md><p>lorem <b>ipsum</b></p><ul><li>a</li><li>b</li></ul>"
            "<pre><code>x</code></pre><div><div><span>t</span></div></div></div></div></div>';"
            " const d = document.createElement('div'); d.innerHTML = blk.repeat(1600); document.body.appendChild(d);"
            " return String(document.getElementsByTagName('*').length) })()");
        QVERIFY(js(build).toInt() > 20000);
        QTest::qWait(3000); // the insertion's own (budgeted) walk and the catch-all after it
        js(QStringLiteral("__prajnaClear.stats.reset(), 1"));
        js(QStringLiteral("document.body.appendChild(document.createElement('p')), 1"));
        QTRY_VERIFY_WITH_TIMEOUT(stat("fullScans") == 1 && js(QStringLiteral("String(__prajnaClear.stats.visited > 15000)")) == QLatin1String("true"),
                                 8000);
        // the walk was spread over frames (budget: 5-8 ms per frame)
        const int maxFrame = stat("maxFrameMs");
        qInfo() << "catch-all over" << stat("visited") << "elements, longest frame slice" << maxFrame << "ms";
        // (the first read of a slice may also pay a pending style/layout
        // recalculation of the page itself: allow for slow runners)
        QVERIFY2(maxFrame < 60, "a catch-all walk blocked the main thread");
    }
};

QTEST_MAIN(TransparentBackgroundTest)
#include "transparent_background_test.moc"
