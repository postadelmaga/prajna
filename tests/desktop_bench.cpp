/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// prajna-desktop-bench: the plasmoid's rendering path on the real desktop.
//
// prajna_bench.py measures the engine alone (headless). This puts a
// WebSurface in a translucent, KWin-blurred window like the plasmoid popup
// (tinted glass + grain + transparent page), drives it, and measures what
// the headless run can't see: the host process (plasmashell's role: frame
// copy, texture upload, Qt Quick rendering) and kwin_wayland (composition,
// blur), next to the engine processes. Not run by ctest: it opens a window.
//
//   prajna-desktop-bench --engine <prajna-engine> --page <bench_page.html>
//       [--scenario static|type12|stream] [--seconds 8] [--transparent 1]
//       [--blur 1] [--gpu 1] [--fps 30] [--nodes 58] [--grain <grain.png>]
//       [--import <dir with io/github/postadelmaga/prajna>]  (another build)
//
// Prints one JSON object. Run each configuration several times: KWin's
// numbers include whatever else the desktop is doing.

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QTemporaryDir>
#include <KWindowEffects>
#include <rhi/qrhi.h>

#include <unistd.h>

namespace
{
const long kClk = sysconf(_SC_CLK_TCK);

double procCpu(const QString &statPath)
{
    QFile f(statPath);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray s = f.readAll();
    const QList<QByteArray> fields = s.mid(s.lastIndexOf(')') + 2).split(' ');
    return fields.size() > 12 ? (fields[11].toDouble() + fields[12].toDouble()) / kClk : 0;
}

QString comm(const QString &dir)
{
    QFile f(dir + QStringLiteral("/comm"));
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll().trimmed()) : QString();
}

int parentOf(int pid)
{
    QFile f(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray s = f.readAll();
    return s.mid(s.lastIndexOf(')') + 2).split(' ').value(1).toInt();
}

// CPU seconds by process name: this process' threads by name, its
// descendants (engine, web, network process) and kwin_wayland.
QHash<QString, double> sample()
{
    QHash<QString, double> t;
    const int self = getpid();
    const QDir task(QStringLiteral("/proc/self/task"));
    for (const QString &tid : task.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = task.filePath(tid);
        QString name = comm(dir);
        if (tid.toInt() == self)
            name = QStringLiteral("gui");
        t[QStringLiteral("host:") + name] += procCpu(dir + QStringLiteral("/stat"));
    }
    QSet<int> tree{self};
    const QStringList pids = QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (bool grew = true; grew;) {
        grew = false;
        for (const QString &p : pids) {
            const int pid = p.toInt();
            if (pid > 0 && !tree.contains(pid) && tree.contains(parentOf(pid))) {
                tree.insert(pid);
                grew = true;
            }
        }
    }
    for (int pid : tree)
        if (pid != self)
            t[comm(QStringLiteral("/proc/%1").arg(pid))] += procCpu(QStringLiteral("/proc/%1/stat").arg(pid));
    for (const QString &p : pids)
        if (comm(QStringLiteral("/proc/") + p) == QLatin1String("kwin_wayland"))
            t[QStringLiteral("kwin_wayland")] += procCpu(QStringLiteral("/proc/%1/stat").arg(p));
    return t;
}

qint64 rssKb(int pid)
{
    QFile f(QStringLiteral("/proc/%1/status").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    for (const QByteArray &l : f.readAll().split('\n'))
        if (l.startsWith("VmRSS:"))
            return l.mid(6).trimmed().split(' ').value(0).toLongLong();
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    QCommandLineParser cl;
    cl.addHelpOption();
    const QCommandLineOption oEngine(QStringLiteral("engine"), QStringLiteral("prajna-engine path"), QStringLiteral("path"));
    const QCommandLineOption oPage(QStringLiteral("page"), QStringLiteral("bench_page.html"), QStringLiteral("path"));
    const QCommandLineOption oGrain(QStringLiteral("grain"), QStringLiteral("grain.png"), QStringLiteral("path"));
    const QCommandLineOption oScenario(QStringLiteral("scenario"), QStringLiteral("static|type12|stream"), QStringLiteral("name"), QStringLiteral("type12"));
    const QCommandLineOption oSeconds(QStringLiteral("seconds"), QStringLiteral("measured seconds"), QStringLiteral("s"), QStringLiteral("8"));
    const QCommandLineOption oTransparent(QStringLiteral("transparent"), QStringLiteral("0/1"), QStringLiteral("b"), QStringLiteral("1"));
    const QCommandLineOption oBlur(QStringLiteral("blur"), QStringLiteral("0/1"), QStringLiteral("b"), QStringLiteral("1"));
    const QCommandLineOption oGpu(QStringLiteral("gpu"), QStringLiteral("0/1"), QStringLiteral("b"), QStringLiteral("1"));
    const QCommandLineOption oFps(QStringLiteral("fps"), QStringLiteral("max fps"), QStringLiteral("n"), QStringLiteral("30"));
    const QCommandLineOption oNodes(QStringLiteral("nodes"), QStringLiteral("chat turns"), QStringLiteral("n"), QStringLiteral("58"));
    const QCommandLineOption oImport(QStringLiteral("import"), QStringLiteral("QML import dir of the plugin"), QStringLiteral("dir"),
                                     QStringLiteral(PRAJNA_QML_IMPORT_DIR));
    cl.addOptions({oEngine, oPage, oGrain, oScenario, oSeconds, oTransparent, oBlur, oGpu, oFps, oNodes, oImport});
    cl.process(app);
    const bool transparent = cl.value(oTransparent) == QLatin1String("1");
    const bool blur = cl.value(oBlur) == QLatin1String("1") && transparent;

    QTemporaryDir profile;
    QQmlEngine engine;
    engine.addImportPath(cl.value(oImport));
    QQmlComponent c(&engine);
    // The popup's look: theme-tinted glass and a faint grain over KWin's blur.
    c.setData(QStringLiteral(R"(
        import QtQuick
        import io.github.postadelmaga.prajna
        Item {
            property alias web: web
            Rectangle { anchors.fill: parent; visible: %1; radius: 6
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.rgba(0.12, 0.12, 0.13, 0.52) }
                    GradientStop { position: 0.06; color: Qt.rgba(0.12, 0.12, 0.13, 0.4) }
                    GradientStop { position: 1; color: Qt.rgba(0.12, 0.12, 0.13, 0.4) } } }
            Image { anchors.fill: parent; visible: %1; source: "%2"; fillMode: Image.Tile; opacity: 0.045 }
            WebSurface { id: web; anchors.fill: parent; property var testResult
                transparentBackground: %1; gpuRendering: %3; maxFps: %4; darkMode: true }
        }
    )")
                  .arg(transparent ? QStringLiteral("true") : QStringLiteral("false"),
                       QUrl::fromLocalFile(cl.value(oGrain)).toString(),
                       cl.value(oGpu) == QLatin1String("1") ? QStringLiteral("true") : QStringLiteral("false"),
                       cl.value(oFps))
                  .toUtf8(),
              QUrl());
    QObject *root = c.beginCreate(engine.rootContext());
    if (!root) {
        qCritical() << c.errorString();
        return 2;
    }
    auto *item = qobject_cast<QQuickItem *>(root);
    auto *web = item->property("web").value<QQuickItem *>();
    web->setProperty("enginePath", cl.value(oEngine));
    web->setProperty("profileDirectory", profile.path());
    web->setProperty("url", QUrl(QUrl::fromLocalFile(cl.value(oPage)).toString() + QStringLiteral("#n=4")));
    QQuickWindow window;
    window.setColor(transparent ? Qt::transparent : Qt::black);
    window.resize(600, 900);
    window.setTitle(QStringLiteral("prajna-desktop-bench"));
    item->setParentItem(window.contentItem());
    item->setSize(window.size());
    c.completeCreate();
    window.show();
    if (blur)
        KWindowEffects::enableBlurBehind(&window, true);

    int swaps = 0;
    QObject::connect(&window, &QQuickWindow::frameSwapped, &app, [&] { ++swaps; }, Qt::DirectConnection);

    auto js = [&](const QString &script) {
        web->setProperty("testResult", QVariant());
        QJSValue setter = engine.evaluate(QStringLiteral("(function (item) { return function (r) { item.testResult = r; }; })"))
                              .call({engine.newQObject(web)});
        QMetaObject::invokeMethod(web, "runJavaScript", Q_ARG(QString, script), Q_ARG(QJSValue, setter));
        (void)QTest::qWaitFor([&] { return web->property("testResult").isValid(); }, 5000);
        return web->property("testResult").toString();
    };
    auto settle = [&] { // until the window stops repainting for 1.5 s (max 20 s)
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 20000) {
            const int n = swaps;
            QTest::qWait(1500);
            if (swaps == n)
                return;
        }
    };

    if (!QTest::qWaitFor([&] { return js(QStringLiteral("document.readyState")) == QLatin1String("complete"); }, 20000)) {
        qCritical() << "page did not load";
        return 3;
    }
    js(QStringLiteral("benchBuild(%1)").arg(cl.value(oNodes).toInt()));
    QTest::qWait(2500);
    settle();
    // click into the composer
    const QStringList xy = js(QStringLiteral("(() => { const r = document.getElementById('prompt-textarea').getBoundingClientRect();"
                                             " return Math.round(r.x + r.width / 2) + ',' + Math.round(r.y + r.height / 2) })()"))
                               .split(QLatin1Char(','));
    const QPoint at(xy.value(0).toInt(), xy.value(1).toInt());
    QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, at);
    web->forceActiveFocus();
    QTest::qWait(500);
    settle();

    const QString scenario = cl.value(oScenario);
    const double seconds = cl.value(oSeconds).toDouble();
    const QHash<QString, double> c0 = sample();
    const int swaps0 = swaps;
    QElapsedTimer clock;
    clock.start();
    if (scenario == QLatin1String("stream"))
        js(QStringLiteral("benchStream(60)"));
    if (scenario == QLatin1String("type12")) {
        static const struct {
            quint32 sym, code;
            Qt::Key key;
            char text;
        } keys[] = {{'a', 38, Qt::Key_A, 'a'}, {'s', 39, Qt::Key_S, 's'}, {'d', 40, Qt::Key_D, 'd'}, {'f', 41, Qt::Key_F, 'f'},
                    {'g', 42, Qt::Key_G, 'g'}, {'h', 43, Qt::Key_H, 'h'}, {'j', 44, Qt::Key_J, 'j'}, {'k', 45, Qt::Key_K, 'k'}};
        for (int i = 0; clock.elapsed() < seconds * 1000; ++i) {
            const auto &k = keys[i % 8];
            QKeyEvent press(QEvent::KeyPress, k.key, Qt::NoModifier, k.code, k.sym, 0, QString(QLatin1Char(k.text)));
            QKeyEvent release(QEvent::KeyRelease, k.key, Qt::NoModifier, k.code, k.sym, 0, QString(QLatin1Char(k.text)));
            QCoreApplication::sendEvent(web, &press);
            QCoreApplication::sendEvent(web, &release);
            QTest::qWait(83);
        }
    } else {
        QTest::qWait(int(seconds * 1000));
    }
    const double dt = clock.elapsed() / 1000.0;
    const QHash<QString, double> c1 = sample();
    const int swapCount = swaps - swaps0;
    if (scenario == QLatin1String("stream"))
        js(QStringLiteral("benchStream(0)"));

    QJsonObject cpu;
    double hostTotal = 0;
    for (auto it = c1.begin(); it != c1.end(); ++it) {
        const double pct = 100 * (it.value() - c0.value(it.key())) / dt;
        if (pct >= 0.1)
            cpu[it.key()] = qRound(pct * 10) / 10.0;
        if (it.key().startsWith(QLatin1String("host:")))
            hostTotal += pct;
    }
    cpu[QStringLiteral("host")] = qRound(hostTotal * 10) / 10.0;
    QJsonObject out{{QStringLiteral("scenario"), scenario},
                    {QStringLiteral("transparent"), transparent},
                    {QStringLiteral("blur"), blur},
                    {QStringLiteral("fps"), cl.value(oFps).toInt()},
                    {QStringLiteral("dpr"), window.effectiveDevicePixelRatio()},
                    {QStringLiteral("seconds"), dt},
                    {QStringLiteral("window_fps"), qRound(swapCount / dt * 10) / 10.0},
                    {QStringLiteral("exposed"), window.isExposed()},
                    {QStringLiteral("scene_graph"), window.rhi() ? QString::fromLatin1(window.rhi()->backendName()) : QStringLiteral("software")},
                    {QStringLiteral("cpu"), cpu},
                    {QStringLiteral("host_rss_mb"), rssKb(getpid()) / 1024},
                    {QStringLiteral("typed"), js(QStringLiteral("String(document.getElementById('prompt-textarea').textContent.length)")).toInt()}};
    printf("%s\n", QJsonDocument(out).toJson(QJsonDocument::Compact).constData());
    fflush(stdout);
    delete root;
    return 0;
}
