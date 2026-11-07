/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "websurface.h"

#include "../engine/engine_proto.h"
#include "frametexture.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QMimeData>
#include <QProcess>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QScreen>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QtEndian>

#include <cerrno>
#include <dlfcn.h>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

Q_LOGGING_CATEGORY(PRAJNA_WEB, "prajna.web", QtInfoMsg)

namespace
{
constexpr int kMaxCrashRestarts = 5;
constexpr qsizetype kMaxClipboardBytes = 15 * 1024 * 1024;

// GCC flags the shared-null QByteArray path as out of bounds even though
// the size check guards it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
qint32 readI32(const QByteArray &b, int index)
{
    qint32 v = 0;
    if (b.size() >= (index + 1) * 4)
        memcpy(&v, b.constData() + index * 4, 4);
    return qFromLittleEndian(v);
}

qint64 readI64(const QByteArray &b, int offset)
{
    qint64 v = 0;
    if (b.size() >= offset + 8)
        memcpy(&v, b.constData() + offset, 8);
    return qFromLittleEndian(v);
}
#pragma GCC diagnostic pop

void appendI32(QByteArray &b, qint32 v)
{
    char buf[4];
    qToLittleEndian<qint32>(v, buf);
    b.append(buf, 4);
}

void appendBlobEntry(QByteArray &blob, const QByteArray &mime, const QByteArray &data)
{
    appendI32(blob, qint32(mime.size()));
    blob.append(mime);
    appendI32(blob, qint32(data.size()));
    blob.append(data);
}

Qt::CursorShape cursorForCss(const QByteArray &n)
{
    static const QHash<QByteArray, Qt::CursorShape> map = {
        {"default", Qt::ArrowCursor},
        {"auto", Qt::ArrowCursor},
        {"context-menu", Qt::ArrowCursor},
        {"pointer", Qt::PointingHandCursor},
        {"text", Qt::IBeamCursor},
        {"vertical-text", Qt::IBeamCursor},
        {"wait", Qt::WaitCursor},
        {"progress", Qt::BusyCursor},
        {"help", Qt::WhatsThisCursor},
        {"crosshair", Qt::CrossCursor},
        {"cell", Qt::CrossCursor},
        {"move", Qt::SizeAllCursor},
        {"all-scroll", Qt::SizeAllCursor},
        {"not-allowed", Qt::ForbiddenCursor},
        {"no-drop", Qt::ForbiddenCursor},
        {"grab", Qt::OpenHandCursor},
        {"grabbing", Qt::ClosedHandCursor},
        {"copy", Qt::DragCopyCursor},
        {"alias", Qt::DragLinkCursor},
        {"none", Qt::BlankCursor},
        {"n-resize", Qt::SizeVerCursor},
        {"s-resize", Qt::SizeVerCursor},
        {"ns-resize", Qt::SizeVerCursor},
        {"row-resize", Qt::SplitVCursor},
        {"e-resize", Qt::SizeHorCursor},
        {"w-resize", Qt::SizeHorCursor},
        {"ew-resize", Qt::SizeHorCursor},
        {"col-resize", Qt::SplitHCursor},
        {"ne-resize", Qt::SizeBDiagCursor},
        {"sw-resize", Qt::SizeBDiagCursor},
        {"nesw-resize", Qt::SizeBDiagCursor},
        {"nw-resize", Qt::SizeFDiagCursor},
        {"se-resize", Qt::SizeFDiagCursor},
        {"nwse-resize", Qt::SizeFDiagCursor},
    };
    return map.value(n, Qt::ArrowCursor);
}

// Fallback keysyms for key events without native data (synthesized events).
quint32 keysymForQtKey(int key)
{
    switch (key) {
    case Qt::Key_Escape: return XKB_KEY_Escape;
    case Qt::Key_Tab: return XKB_KEY_Tab;
    case Qt::Key_Backtab: return XKB_KEY_ISO_Left_Tab;
    case Qt::Key_Backspace: return XKB_KEY_BackSpace;
    case Qt::Key_Return: return XKB_KEY_Return;
    case Qt::Key_Enter: return XKB_KEY_KP_Enter;
    case Qt::Key_Insert: return XKB_KEY_Insert;
    case Qt::Key_Delete: return XKB_KEY_Delete;
    case Qt::Key_Home: return XKB_KEY_Home;
    case Qt::Key_End: return XKB_KEY_End;
    case Qt::Key_Left: return XKB_KEY_Left;
    case Qt::Key_Up: return XKB_KEY_Up;
    case Qt::Key_Right: return XKB_KEY_Right;
    case Qt::Key_Down: return XKB_KEY_Down;
    case Qt::Key_PageUp: return XKB_KEY_Page_Up;
    case Qt::Key_PageDown: return XKB_KEY_Page_Down;
    case Qt::Key_Shift: return XKB_KEY_Shift_L;
    case Qt::Key_Control: return XKB_KEY_Control_L;
    case Qt::Key_Alt: return XKB_KEY_Alt_L;
    case Qt::Key_Meta: return XKB_KEY_Super_L;
    case Qt::Key_Space: return XKB_KEY_space;
    default:
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
            return XKB_KEY_F1 + (key - Qt::Key_F1);
        if (key >= Qt::Key_A && key <= Qt::Key_Z)
            return XKB_KEY_a + (key - Qt::Key_A);
        if (key >= Qt::Key_0 && key <= Qt::Key_9)
            return XKB_KEY_0 + (key - Qt::Key_0);
        return 0;
    }
}

bool isDeadKeysym(quint32 ks)
{
    return ks >= XKB_KEY_dead_grave && ks <= XKB_KEY_dead_greek;
}
} // namespace

WebSurface::WebSurface(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setFlag(ItemAcceptsInputMethod, true);
    setFlag(ItemIsFocusScope, false);
    setActiveFocusOnTab(true);
    setAcceptHoverEvents(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton | Qt::BackButton | Qt::ForwardButton);
    setClip(true);

    m_profileDir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/prajna/default");
    m_downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    m_restartTimer = new QTimer(this);
    m_restartTimer->setSingleShot(true);
    connect(m_restartTimer, &QTimer::timeout, this, &WebSurface::startEngine);

    // Keep the engine's clipboard current so in-page paste (Ctrl+V,
    // navigator.clipboard.read) sees what the user copied elsewhere.
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (!m_settingClipboard && hasActiveFocus())
            pushClipboard();
    });
}

WebSurface::~WebSurface()
{
    if (m_process) {
        // Graceful: let WebKit flush cookies/localStorage before exiting.
        send(ZURF_OP_SHUTDOWN);
        onWritable();
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
        m_process->disconnect(this);
        if (!m_process->waitForFinished(1500))
            m_process->kill();
    }
    if (m_shm)
        munmap(m_shm, ZURF_SHM_SIZE);
    if (!m_shmPath.isEmpty())
        ::unlink(QFile::encodeName(m_shmPath).constData());
}

// ---------------------------------------------------------------------------
// properties

void WebSurface::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    Q_EMIT activeChanged();
    if (active) {
        m_crashCount = 0;
        startEngine();
    } else {
        stopEngine(true);
    }
}

void WebSurface::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
    m_suspended = suspended;
    Q_EMIT suspendedChanged();
    sendVisibility();
}

void WebSurface::setEnginePath(const QString &path)
{
    if (m_enginePath == path)
        return;
    m_enginePath = path;
    Q_EMIT enginePathChanged();
}

void WebSurface::setProfileDirectory(const QString &dir)
{
    QString d = dir;
    if (d.startsWith(QLatin1String("file://")))
        d = QUrl(d).toLocalFile();
    if (d.isEmpty() || m_profileDir == d)
        return;
    m_profileDir = d;
    Q_EMIT profileDirectoryChanged();
    if (m_process)
        restartEngine(); // a different profile needs a different network session
}

void WebSurface::setMemoryLimitMb(int mb)
{
    if (m_memoryLimitMb == mb)
        return;
    m_memoryLimitMb = mb;
    Q_EMIT memoryLimitMbChanged();
}

void WebSurface::setGpuRendering(bool on)
{
    if (m_gpuRendering == on)
        return;
    m_gpuRendering = on;
    Q_EMIT gpuRenderingChanged();
}

void WebSurface::setJavascriptJit(bool on)
{
    if (m_jsJit == on)
        return;
    m_jsJit = on;
    Q_EMIT javascriptJitChanged();
}

void WebSurface::setUrl(const QUrl &url)
{
    if (!url.isValid() || m_url == url)
        return;
    m_url = url;
    Q_EMIT urlChanged();
    if (m_ready)
        send(ZURF_OP_NAVIGATE, url.toEncoded());
}

#define WS_PREF_SETTER(Setter, member, Type)                                                                                                                   \
    void WebSurface::Setter(Type v)                                                                                                                            \
    {                                                                                                                                                          \
        if (member == v)                                                                                                                                       \
            return;                                                                                                                                            \
        member = v;                                                                                                                                            \
        Q_EMIT prefsChanged();                                                                                                                                 \
        sendPrefs();                                                                                                                                           \
    }

WS_PREF_SETTER(setUserAgent, m_userAgent, const QString &)
WS_PREF_SETTER(setDarkMode, m_darkMode, bool)
WS_PREF_SETTER(setJavascriptCanOpenWindows, m_jsOpenWindows, bool)
WS_PREF_SETTER(setJavascriptCanAccessClipboard, m_jsClipboard, bool)
WS_PREF_SETTER(setPlaybackRequiresUserGesture, m_mediaGesture, bool)
WS_PREF_SETTER(setSpatialNavigation, m_spatialNav, bool)
WS_PREF_SETTER(setPopupsInPlace, m_popupsInPlace, bool)
WS_PREF_SETTER(setLinksInTabs, m_linksInTabs, bool)
WS_PREF_SETTER(setTransparentBackground, m_transparent, bool)

void WebSurface::setMaxFps(int fps)
{
    fps = qBound(5, fps, 240);
    if (m_maxFps == fps)
        return;
    m_maxFps = fps;
    Q_EMIT prefsChanged();
    sendPrefs();
}
#undef WS_PREF_SETTER

void WebSurface::setDownloadDirectory(const QString &dir)
{
    QString d = dir;
    if (d.startsWith(QLatin1String("file://")))
        d = QUrl(d).toLocalFile();
    if (m_downloadDir == d)
        return;
    m_downloadDir = d;
    Q_EMIT prefsChanged();
    sendPrefs();
}

void WebSurface::setLastError(const QString &error)
{
    if (m_lastError == error)
        return;
    m_lastError = error;
    if (!error.isEmpty())
        qCWarning(PRAJNA_WEB) << error;
    Q_EMIT lastErrorChanged();
}

// ---------------------------------------------------------------------------
// invokables

void WebSurface::load(const QUrl &url)
{
    if (!url.isValid())
        return;
    if (m_url != url) {
        m_url = url;
        Q_EMIT urlChanged();
    }
    if (m_ready)
        send(ZURF_OP_NAVIGATE, url.toEncoded());
}

void WebSurface::goBack()
{
    send(ZURF_OP_GO_BACK);
}

void WebSurface::goForward()
{
    send(ZURF_OP_GO_FORWARD);
}

void WebSurface::reload()
{
    send(ZURF_OP_RELOAD);
}

void WebSurface::reloadAndBypassCache()
{
    send(ZURF_OP_RELOAD_BYPASS);
}

void WebSurface::stop()
{
    send(ZURF_OP_STOP);
}

void WebSurface::runJavaScript(const QString &script, const QJSValue &callback)
{
    if (!m_ready) {
        if (callback.isCallable())
            QJSValue(callback).call({QJSValue(QString())});
        return;
    }
    if (!callback.isCallable()) {
        send(ZURF_OP_EVAL_JS, script.toUtf8());
        return;
    }
    const int id = m_nextJsId++;
    m_jsCallbacks.insert(id, callback);
    sendInts(ZURF_OP_EVAL_JS_RET, {id}, script.toUtf8());
}

void WebSurface::findText(const QString &text, bool backwards, bool caseSensitive)
{
    const qint32 flags = (backwards ? ZURF_FIND_BACKWARDS : 0) | (caseSensitive ? ZURF_FIND_CASE_SENSITIVE : 0);
    sendInts(ZURF_OP_FIND, {flags}, text.toUtf8());
}

void WebSurface::stopFinding()
{
    sendInts(ZURF_OP_FIND, {0});
}

void WebSurface::editCommand(const QString &command)
{
    if (command == QLatin1String("Paste"))
        pushClipboard();
    send(ZURF_OP_EDIT, command.toUtf8());
}

void WebSurface::insertText(const QString &text)
{
    send(ZURF_OP_INSERT_TEXT, text.toUtf8());
}

void WebSurface::closePopup()
{
    if (isPopupOpen())
        closeTab(currentTab());
}

// ---------------------------------------------------------------------------
// tabs

int WebSurface::indexOfTab(int id) const
{
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].id == id)
            return i;
    return -1;
}

bool WebSurface::isPopupOpen() const
{
    const int i = currentTab();
    return i >= 0 && m_tabs[i].popup;
}

QVariantList WebSurface::tabs() const
{
    QVariantList list;
    list.reserve(m_tabs.size());
    for (const Tab &t : m_tabs)
        list << QVariantMap{{QStringLiteral("id"), t.id},
                            {QStringLiteral("title"), t.title},
                            {QStringLiteral("url"), t.url},
                            {QStringLiteral("popup"), t.popup}};
    return list;
}

QStringList WebSurface::tabUrls() const
{
    QStringList urls;
    for (const Tab &t : m_tabs)
        if (!t.popup && t.url.isValid() && !t.url.isEmpty() && t.url.scheme() != QLatin1String("about"))
            urls << QString::fromUtf8(t.url.toEncoded());
    return urls;
}

void WebSurface::setRestoreTabs(const QStringList &urls)
{
    if (m_restoreTabs == urls)
        return;
    m_restoreTabs = urls;
    Q_EMIT restoreTabsChanged();
}

void WebSurface::newTab(const QUrl &url)
{
    if (m_ready)
        send(ZURF_OP_TAB_NEW, (url.isValid() ? url : m_url).toEncoded());
}

void WebSurface::switchTab(int index)
{
    if (index >= 0 && index < m_tabs.size() && m_tabs[index].id != m_activeTab)
        sendInts(ZURF_OP_TAB_SWITCH, {m_tabs[index].id});
}

void WebSurface::cycleTab(int delta)
{
    const int n = m_tabs.size();
    if (n > 1)
        switchTab(((currentTab() + delta) % n + n) % n);
}

void WebSurface::closeTab(int index)
{
    if (index < 0 || index >= m_tabs.size() || m_tabs.size() < 2)
        return;
    const Tab &t = m_tabs[index];
    if (t.id == m_activeTab) {
        // Show the opener of a popup, else the neighbour (like a browser),
        // before the engine picks one itself.
        int next = indexOfTab(t.opener);
        if (next < 0)
            next = index + 1 < m_tabs.size() ? index + 1 : index - 1;
        sendInts(ZURF_OP_TAB_SWITCH, {m_tabs[next].id});
    }
    sendInts(ZURF_OP_TAB_CLOSE, {t.id});
}

// Reopen the tabs of the previous engine run (see restoreTabs).
void WebSurface::restoreSession()
{
    const QStringList extra = m_session.mid(1);
    m_session.clear();
    if (extra.isEmpty())
        return;
    m_sessionExpected = extra.size() + 1;
    // Background tabs load only when switched to.
    for (const QString &u : extra)
        send(ZURF_OP_TAB_NEW_BACKGROUND, u.toUtf8());
}

void WebSurface::replyFileChooser(int requestId, const QList<QUrl> &files)
{
    QStringList paths;
    for (const QUrl &u : files) {
        const QString p = u.isLocalFile() ? u.toLocalFile() : u.toString();
        if (!p.isEmpty())
            paths << p;
    }
    sendInts(ZURF_OP_FILE_CHOOSER_REPLY, {requestId}, paths.join(QLatin1Char('\n')).toUtf8());
}

void WebSurface::replyPermission(int requestId, bool allow)
{
    sendInts(ZURF_OP_PERMISSION_REPLY, {requestId, allow ? 1 : 0});
}

void WebSurface::cancelDownload(int downloadId)
{
    sendInts(ZURF_OP_DOWNLOAD_CANCEL, {downloadId});
}

void WebSurface::copyToClipboard(const QString &text)
{
    m_settingClipboard = true;
    QGuiApplication::clipboard()->setText(text);
    m_settingClipboard = false;
}

void WebSurface::restartEngine()
{
    if (!m_process) {
        startEngine();
        return;
    }
    stopEngine(true);
    m_restartTimer->start(300);
}

// ---------------------------------------------------------------------------
// engine lifecycle

QString WebSurface::resolvedEnginePath() const
{
    QStringList candidates;
    if (!m_enginePath.isEmpty())
        candidates << m_enginePath;
    const QString env = qEnvironmentVariable("PRAJNA_WEB_ENGINE");
    if (!env.isEmpty())
        candidates << env;
    // Installed layout: the engine sits next to this plugin inside the
    // plasmoid package (contents/lib/prajna/).
    Dl_info info{};
    if (dladdr(reinterpret_cast<void *>(&cursorForCss), &info) && info.dli_fname)
        candidates << QFileInfo(QFile::decodeName(info.dli_fname)).absolutePath() + QStringLiteral("/prajna-engine");
#ifdef PRAJNA_ENGINE_BUILD_PATH
    candidates << QStringLiteral(PRAJNA_ENGINE_BUILD_PATH);
#endif
    for (const QString &c : std::as_const(candidates)) {
        const QFileInfo fi(c);
        if (fi.isFile() && fi.isExecutable())
            return fi.absoluteFilePath();
    }
    return {};
}

void WebSurface::componentComplete()
{
    QQuickItem::componentComplete();
    m_complete = true;
    if (m_active)
        startEngine();
}

void WebSurface::startEngine()
{
    if (!m_complete || !m_active || m_process)
        return;

    const QString engine = resolvedEnginePath();
    if (engine.isEmpty()) {
        setLastError(QStringLiteral("prajna-engine engine not found (set PRAJNA_WEB_ENGINE or reinstall the plasmoid)"));
        return;
    }

    // Frame slot. Sparse on tmpfs: only the pages actually written cost RAM.
    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty())
        runtimeDir = QStringLiteral("/dev/shm");
    static int s_counter = 0;
    m_shmPath = QStringLiteral("%1/prajna-%2-%3.shm").arg(runtimeDir).arg(QCoreApplication::applicationPid()).arg(++s_counter);
    const QByteArray shmPathC = QFile::encodeName(m_shmPath);
    int shmFd = ::open(shmPathC.constData(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (shmFd < 0 || ::ftruncate(shmFd, ZURF_SHM_SIZE) != 0) {
        setLastError(QStringLiteral("cannot create frame buffer %1: %2").arg(m_shmPath, QString::fromLocal8Bit(strerror(errno))));
        if (shmFd >= 0)
            ::close(shmFd);
        ::unlink(shmPathC.constData());
        m_shmPath.clear();
        return;
    }
    m_shm = mmap(nullptr, ZURF_SHM_SIZE, PROT_READ, MAP_SHARED, shmFd, 0);
    ::close(shmFd);
    if (m_shm == MAP_FAILED) {
        m_shm = nullptr;
        ::unlink(shmPathC.constData());
        m_shmPath.clear();
        setLastError(QStringLiteral("cannot map frame buffer"));
        return;
    }

    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) {
        setLastError(QStringLiteral("socketpair failed"));
        stopEngine(false);
        return;
    }
    m_fd = sv[0];
    const int childFd = sv[1];
    ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);

    QDir().mkpath(m_profileDir);
    const qreal dpr = devicePixelRatio();
    const QSize logical(qMax(1, qRound(width())), qMax(1, qRound(height())));

    QStringList args{
        QStringLiteral("--zurf-control-fd=3"),
        QStringLiteral("--zurf-shm=%1").arg(m_shmPath),
        QStringLiteral("--zurf-cache=%1").arg(m_profileDir),
        QStringLiteral("--zurf-w=%1").arg(logical.width() > 1 ? logical.width() : 800),
        QStringLiteral("--zurf-h=%1").arg(logical.height() > 1 ? logical.height() : 600),
        QStringLiteral("--zurf-scale=%1").arg(qRound(dpr * 100)),
        QStringLiteral("--zurf-frame-events"),
        QStringLiteral("--max-fps=%1").arg(m_maxFps),
    };
    if (m_session.isEmpty() && !m_restoreUsed && !m_restoreTabs.isEmpty()) {
        m_session = QStringList{QString::fromUtf8(m_url.toEncoded())} + m_restoreTabs;
        m_sessionCurrent = 0;
    }
    m_restoreUsed = true;
    m_sessionExpected = 0;
    if (!m_session.isEmpty())
        args << QStringLiteral("--zurf-url=%1").arg(m_session.first());
    else if (m_url.isValid())
        args << QStringLiteral("--zurf-url=%1").arg(QString::fromUtf8(m_url.toEncoded()));
    if (m_memoryLimitMb > 0)
        args << QStringLiteral("--memory-limit-mb=%1").arg(m_memoryLimitMb);
    if (m_gpuRendering)
        args << QStringLiteral("--gpu");
    if (!m_jsJit)
        args << QStringLiteral("--no-jit");

    m_process = new QProcess(this);
    m_process->setProgram(engine);
    m_process->setArguments(args);
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    m_process->setChildProcessModifier([childFd] {
        // async-signal-safe only: we are between fork and exec.
        if (childFd == 3) {
            ::fcntl(3, F_SETFD, 0);
        } else {
            ::dup2(childFd, 3);
        }
        ::prctl(PR_SET_PDEATHSIG, SIGTERM); // never outlive plasmashell
    });
    connect(m_process, &QProcess::finished, this, &WebSurface::onEngineFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            setLastError(QStringLiteral("cannot start web engine: %1").arg(m_process->errorString()));
            onEngineFinished();
        }
    });

    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &WebSurface::onReadable);
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &WebSurface::onWritable);

    m_pendingFull = true;
    m_pendingDamage = QRect();
    m_sentSize = logical;
    m_sentScale = qRound(dpr * 100);
    m_sentVisible = -1;
    m_tabs.clear();
    m_activeTab = 0;

    qCInfo(PRAJNA_WEB) << "starting" << engine << "profile" << m_profileDir;
    m_startClock.start();
    m_loggedFrame = m_loggedLoad = false;
    m_process->start();
    ::close(childFd);
}

void WebSurface::stopEngine(bool graceful)
{
    m_restartTimer->stop();
    const bool wasRunning = m_ready;
    m_ready = false;

    if (m_process) {
        QProcess *proc = m_process;
        m_process = nullptr;
        proc->disconnect(this);
        if (graceful && proc->state() != QProcess::NotRunning) {
            send(ZURF_OP_SHUTDOWN);
            onWritable();
            // Closing our end also makes the engine quit (HUP); the timer is
            // the backstop for a wedged web process.
            auto *killTimer = new QTimer(proc);
            killTimer->setSingleShot(true);
            connect(killTimer, &QTimer::timeout, proc, &QProcess::kill);
            killTimer->start(4000);
            connect(proc, &QProcess::finished, proc, &QObject::deleteLater);
        } else {
            proc->kill();
            proc->waitForFinished(500);
            proc->deleteLater();
        }
    }

    delete m_readNotifier;
    m_readNotifier = nullptr;
    delete m_writeNotifier;
    m_writeNotifier = nullptr;
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_in.clear();
    m_out.clear();
    if (m_shm) {
        munmap(m_shm, ZURF_SHM_SIZE);
        m_shm = nullptr;
    }
    if (!m_shmPath.isEmpty()) {
        ::unlink(QFile::encodeName(m_shmPath).constData());
        m_shmPath.clear();
    }
    // Pending JS callbacks will never be answered.
    const auto callbacks = std::exchange(m_jsCallbacks, {});
    for (QJSValue cb : callbacks)
        cb.call({QJSValue(QString())});

    // Remember the open tabs: the next start reopens them. (With a sign-in
    // popup in front, `m_url` is the popup's: the session keeps the page.)
    if (m_tabs.size() > 1) {
        m_session.clear();
        m_sessionCurrent = 0;
        for (const Tab &t : std::as_const(m_tabs)) {
            if (t.popup || !t.url.isValid() || t.url.isEmpty() || t.url.scheme() == QLatin1String("about"))
                continue;
            if (t.id == m_activeTab)
                m_sessionCurrent = m_session.size();
            m_session << QString::fromUtf8(t.url.toEncoded());
        }
        if (m_sessionCurrent >= m_session.size())
            m_sessionCurrent = 0;
    }
    const bool hadPopup = isPopupOpen();
    const bool hadTabs = !m_tabs.isEmpty();
    m_tabs.clear();
    m_activeTab = 0;
    if (hadPopup)
        Q_EMIT popupOpenChanged();
    if (hadTabs)
        Q_EMIT tabsChanged();
    if (m_loading) {
        m_loading = false;
        Q_EMIT loadStateChanged();
    }
    if (wasRunning)
        Q_EMIT runningChanged();
}

void WebSurface::onEngineFinished()
{
    if (!m_process)
        return;
    const int code = m_process->exitCode();
    const bool crashed = m_process->exitStatus() == QProcess::CrashExit || code != 0;
    stopEngine(false);
    if (!m_active)
        return;
    if (crashed && m_crashCount < kMaxCrashRestarts) {
        ++m_crashCount;
        qCWarning(PRAJNA_WEB) << "web engine exited (code" << code << "), restarting, attempt" << m_crashCount;
        Q_EMIT engineCrashed();
        m_restartTimer->start(500 * m_crashCount);
    } else if (crashed) {
        setLastError(QStringLiteral("web engine keeps crashing, giving up (exit code %1)").arg(code));
    }
}

// ---------------------------------------------------------------------------
// control plane

void WebSurface::send(quint8 op, const QByteArray &payload)
{
    if (m_fd < 0)
        return;
    zurf_msg_header h{};
    h.len = quint32(payload.size());
    h.op = op;
    m_out.append(reinterpret_cast<const char *>(&h), sizeof(h));
    m_out.append(payload);
    onWritable();
}

void WebSurface::sendInts(quint8 op, std::initializer_list<qint32> ints, const QByteArray &tail)
{
    QByteArray b;
    b.reserve(int(ints.size()) * 4 + tail.size());
    for (qint32 v : ints)
        appendI32(b, v);
    b.append(tail);
    send(op, b);
}

void WebSurface::onWritable()
{
    while (m_fd >= 0 && !m_out.isEmpty()) {
        const ssize_t n = ::write(m_fd, m_out.constData(), size_t(m_out.size()));
        if (n > 0) {
            m_out.remove(0, n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) {
            if (m_writeNotifier)
                m_writeNotifier->setEnabled(true);
            return;
        }
        m_out.clear(); // peer gone; onEngineFinished cleans up
        break;
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
}

void WebSurface::onReadable()
{
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(m_fd, buf, sizeof(buf));
        if (n > 0) {
            m_in.append(buf, n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 && m_readNotifier)
            m_readNotifier->setEnabled(false); // EOF: wait for QProcess::finished
        break;
    }
    qsizetype off = 0;
    while (m_in.size() - off >= qsizetype(sizeof(zurf_msg_header))) {
        zurf_msg_header h;
        memcpy(&h, m_in.constData() + off, sizeof(h));
        if (h.len > ZURF_MSG_MAX) {
            qCWarning(PRAJNA_WEB) << "protocol error: oversized message" << h.len;
            m_in.clear();
            restartEngine();
            return;
        }
        if (m_in.size() - off - qsizetype(sizeof(h)) < qsizetype(h.len))
            break;
        const QByteArray payload = m_in.mid(off + sizeof(h), h.len);
        off += sizeof(h) + h.len;
        handleEvent(h.op, payload);
        if (m_fd < 0)
            return; // handler stopped the engine
    }
    m_in.remove(0, off);
}

void WebSurface::handleEvent(quint8 op, const QByteArray &p)
{
    switch (op) {
    case ZURF_EV_READY:
        m_ready = true;
        m_crashCount = 0;
        setLastError({});
        // The engine has mapped the slot; the name is no longer needed.
        if (!m_shmPath.isEmpty()) {
            ::unlink(QFile::encodeName(m_shmPath).constData());
            m_shmPath.clear();
        }
        sendPrefs();
        m_sentSize = {};
        m_sentScale = 0;
        sendGeometry();
        sendVisibility();
        sendInts(ZURF_OP_FOCUS, {hasActiveFocus() ? 1 : 0});
        if (hasActiveFocus())
            pushClipboard();
        restoreSession();
        qCInfo(PRAJNA_WEB) << "engine ready in" << m_startClock.elapsed() << "ms";
        Q_EMIT runningChanged();
        break;
    case ZURF_EV_FRAME:
        if (!m_loggedFrame) {
            m_loggedFrame = true;
            qCInfo(PRAJNA_WEB) << "first frame in" << m_startClock.elapsed() << "ms";
        }
        if (p.size() >= 16)
            m_pendingDamage |= QRect(readI32(p, 0), readI32(p, 1), readI32(p, 2), readI32(p, 3));
        else
            m_pendingFull = true;
        m_frameDirty = true;
        update();
        break;
    case ZURF_EV_TITLE: {
        const int id = readI32(p, 0);
        const QString t = QString::fromUtf8(p.mid(4));
        const int i = indexOfTab(id);
        if (i >= 0 && m_tabs[i].title != t) {
            m_tabs[i].title = t;
            Q_EMIT tabsChanged();
        }
        if (id == m_activeTab && t != m_title) {
            m_title = t;
            Q_EMIT titleChanged();
        }
        break;
    }
    case ZURF_EV_URL: {
        const int id = readI32(p, 0);
        const QUrl u = QUrl::fromEncoded(p.mid(4));
        if (!u.isValid() || u.isEmpty())
            break;
        const int i = indexOfTab(id);
        if (i >= 0 && m_tabs[i].url != u) {
            m_tabs[i].url = u;
            Q_EMIT tabsChanged();
        }
        if (id == m_activeTab && u != m_url) {
            m_url = u;
            Q_EMIT urlChanged();
        }
        break;
    }
    case ZURF_EV_TAB: {
        const int id = readI32(p, 0);
        const bool background = p.size() >= 12 && (readI32(p, 2) & ZURF_TAB_BACKGROUND);
        const bool hadPopup = isPopupOpen();
        if (indexOfTab(id) < 0) {
            Tab t;
            t.id = id;
            t.opener = p.size() >= 8 ? readI32(p, 1) : 0;
            t.popup = p.size() >= 12 && (readI32(p, 2) & ZURF_TAB_POPUP);
            // Opened from a page: right after it (and its other children).
            int at = m_tabs.size();
            const int opener = indexOfTab(t.opener);
            if (opener >= 0) {
                at = opener + 1;
                while (at < m_tabs.size() && m_tabs[at].opener == t.opener)
                    ++at;
            }
            m_tabs.insert(at, t);
        }
        if (!background)
            m_activeTab = id;
        // Restored session complete: show the tab that was current.
        if (m_sessionExpected && m_tabs.size() >= m_sessionExpected) {
            m_sessionExpected = 0;
            if (m_sessionCurrent > 0 && m_sessionCurrent < m_tabs.size())
                sendInts(ZURF_OP_TAB_SWITCH, {m_tabs[m_sessionCurrent].id});
        }
        if (hadPopup != isPopupOpen())
            Q_EMIT popupOpenChanged();
        Q_EMIT tabsChanged();
        break;
    }
    case ZURF_EV_TAB_CLOSED: {
        const bool hadPopup = isPopupOpen();
        const int i = indexOfTab(readI32(p, 0));
        if (i >= 0) {
            m_tabs.removeAt(i);
            Q_EMIT tabsChanged();
        }
        if (hadPopup != isPopupOpen())
            Q_EMIT popupOpenChanged();
        break;
    }
    case ZURF_EV_LOAD_STATE: {
        const int progress = qBound(0, readI32(p, 0) / 10, 100);
        const bool loading = readI32(p, 1);
        const bool back = readI32(p, 2);
        const bool fwd = readI32(p, 3);
        if (progress != m_progress || loading != m_loading || back != m_canGoBack || fwd != m_canGoForward) {
            m_progress = progress;
            m_loading = loading;
            m_canGoBack = back;
            m_canGoForward = fwd;
            if (!loading && !m_loggedLoad && m_ready) {
                m_loggedLoad = true;
                qCInfo(PRAJNA_WEB) << "page loaded in" << m_startClock.elapsed() << "ms";
            }
            Q_EMIT loadStateChanged();
        }
        break;
    }
    case ZURF_EV_CURSOR:
        applyCursor(p);
        break;
    case ZURF_EV_LINK_HOVER: {
        const QString link = QString::fromUtf8(p);
        if (link != m_hoveredLink) {
            m_hoveredLink = link;
            Q_EMIT hoveredLinkChanged();
        }
        break;
    }
    case ZURF_EV_FILE_CHOOSER: {
        const QString accept = QString::fromUtf8(p.mid(8));
        Q_EMIT fileChooserRequested(readI32(p, 0), readI32(p, 1) != 0, accept.split(QLatin1Char(','), Qt::SkipEmptyParts));
        break;
    }
    case ZURF_EV_PERMISSION:
        Q_EMIT permissionRequested(readI32(p, 0), readI32(p, 1), QString::fromUtf8(p.mid(8)));
        break;
    case ZURF_EV_NOTIFICATION: {
        const int nul = p.indexOf('\0');
        const QString title = QString::fromUtf8(nul >= 0 ? p.left(nul) : p);
        const QString body = nul >= 0 ? QString::fromUtf8(p.mid(nul + 1)) : QString();
        Q_EMIT notificationReceived(title, body);
        break;
    }
    case ZURF_EV_CONTEXT_MENU: {
        const QList<QByteArray> parts = p.mid(12).split('\0');
        Q_EMIT contextMenuRequested(readI32(p, 0),
                                    readI32(p, 1),
                                    readI32(p, 2),
                                    QString::fromUtf8(parts.value(0)),
                                    QString::fromUtf8(parts.value(1)));
        break;
    }
    case ZURF_EV_OPEN_EXTERNAL:
        Q_EMIT openExternalRequested(QUrl::fromEncoded(p));
        break;
    case ZURF_EV_DOWNLOAD:
        if (p.size() >= 24) {
            Q_EMIT downloadUpdated(readI32(p, 0), readI32(p, 1), readI64(p, 8), readI64(p, 16), QString::fromUtf8(p.mid(24)));
        }
        break;
    case ZURF_EV_FIND_RESULT:
        Q_EMIT findResult(readI32(p, 0));
        break;
    case ZURF_EV_CLIPBOARD:
        copyToClipboard(QString::fromUtf8(p));
        break;
    case ZURF_EV_CLIPBOARD_SET: {
        auto *md = new QMimeData;
        qsizetype off = 0;
        while (off + 4 <= p.size()) {
            qint32 ml;
            memcpy(&ml, p.constData() + off, 4);
            ml = qFromLittleEndian(ml);
            off += 4;
            if (ml < 0 || off + ml + 4 > p.size())
                break;
            const QByteArray mime = p.mid(off, ml);
            off += ml;
            qint32 dl;
            memcpy(&dl, p.constData() + off, 4);
            dl = qFromLittleEndian(dl);
            off += 4;
            if (dl < 0 || off + dl > p.size())
                break;
            const QByteArray data = p.mid(off, dl);
            off += dl;
            if (mime.startsWith("text/plain"))
                md->setText(QString::fromUtf8(data));
            else
                md->setData(QString::fromUtf8(mime), data);
        }
        m_settingClipboard = true;
        QGuiApplication::clipboard()->setMimeData(md);
        m_settingClipboard = false;
        break;
    }
    case ZURF_EV_JS_RESULT: {
        QJSValue cb = m_jsCallbacks.take(readI32(p, 0));
        if (cb.isCallable())
            cb.call({QJSValue(QString::fromUtf8(p.mid(4)))});
        break;
    }
    case ZURF_EV_CRASHED:
        Q_EMIT engineCrashed();
        break;
    default:
        break;
    }
}

void WebSurface::sendPrefs()
{
    if (!m_ready)
        return;
    auto b = [](bool v) {
        return v ? "1" : "0";
    };
    QByteArray prefs;
    prefs += "user-agent=" + m_userAgent.toUtf8() + '\n';
    prefs += "download-dir=" + QFile::encodeName(m_downloadDir) + '\n';
    prefs += QByteArray("dark-mode=") + b(m_darkMode) + '\n';
    prefs += QByteArray("js-open-windows=") + b(m_jsOpenWindows) + '\n';
    prefs += QByteArray("js-clipboard=") + b(m_jsClipboard) + '\n';
    prefs += QByteArray("media-gesture=") + b(m_mediaGesture) + '\n';
    prefs += QByteArray("spatial-nav=") + b(m_spatialNav) + '\n';
    prefs += QByteArray("new-window-policy=") + (!m_popupsInPlace ? "external" : m_linksInTabs ? "tabs" : "popup") + '\n';
    prefs += QByteArray("transparent=") + (m_transparent ? "1" : "0") + '\n';
    prefs += "max-fps=" + QByteArray::number(m_maxFps) + '\n';
    send(ZURF_OP_SET_PREFS, prefs);
}

void WebSurface::sendGeometry()
{
    if (!m_ready)
        return;
    const QSize logical(qMax(1, qRound(width())), qMax(1, qRound(height())));
    const int scale = qRound(devicePixelRatio() * 100);
    if (scale != m_sentScale) {
        m_sentScale = scale;
        sendInts(ZURF_OP_SET_SCALE, {scale});
    }
    if (logical != m_sentSize && width() >= 1 && height() >= 1) {
        m_sentSize = logical;
        sendInts(ZURF_OP_RESIZE, {logical.width(), logical.height()});
    }
}

void WebSurface::sendVisibility()
{
    if (!m_ready)
        return;
    const bool visible = isVisible() && window() && window()->isVisible() && !m_suspended;
    if (int(visible) == m_sentVisible)
        return;
    m_sentVisible = visible;
    sendInts(ZURF_OP_SET_VISIBLE, {visible ? 1 : 0});
    if (!visible) {
        // No host-side copy of the frame outlives an upload (the staging
        // buffer goes to the GPU and is freed), except the fallback image
        // of a scene graph without partial uploads: freed here. It is only
        // touched in updatePaintNode, i.e. during sync while this (GUI)
        // thread is blocked, so this can't race with the render thread.
        // The texture stays on the GPU; the damage reported while hidden is
        // copied on show (all of it if the texture was lost meanwhile).
        m_fallbackImage = QImage();
        m_frameResident = false;
        update(); // one more sync, if the window still renders: drops staging
    } else {
        m_frameDirty = true; // catch up even if the page doesn't repaint on show
        update();
    }
}

void WebSurface::pushClipboard()
{
    if (!m_ready)
        return;
    const QMimeData *md = QGuiApplication::clipboard()->mimeData();
    if (!md)
        return;
    QByteArray blob;
    if (md->hasText())
        appendBlobEntry(blob, "text/plain;charset=utf-8", md->text().toUtf8());
    if (md->hasFormat(QStringLiteral("text/html")))
        appendBlobEntry(blob, "text/html", md->data(QStringLiteral("text/html")));
    if (md->hasFormat(QStringLiteral("image/png"))) {
        const QByteArray png = md->data(QStringLiteral("image/png"));
        if (png.size() < kMaxClipboardBytes)
            appendBlobEntry(blob, "image/png", png);
    } else if (md->hasImage()) {
        const QImage img = qvariant_cast<QImage>(md->imageData());
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (!img.isNull() && img.save(&buf, "PNG") && png.size() < kMaxClipboardBytes)
            appendBlobEntry(blob, "image/png", png);
    }
    send(ZURF_OP_CLIPBOARD_SET, blob);
}

void WebSurface::applyCursor(const QByteArray &cssName)
{
    setCursor(QCursor(cursorForCss(cssName)));
}

qreal WebSurface::devicePixelRatio() const
{
    if (window())
        return window()->effectiveDevicePixelRatio();
    return qApp ? qApp->devicePixelRatio() : 1.0;
}

// ---------------------------------------------------------------------------
// rendering

WebSurface::CopyResult WebSurface::copyFrame(FrameTexture *texture)
{
    if (!m_shm)
        return CopyResult::NoFrame;
    auto *hdr = static_cast<zurf_shm_header *>(m_shm);
    const auto *slots = static_cast<const uchar *>(m_shm) + sizeof(zurf_shm_header);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const quint32 s1 = __atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1u)
            continue;
        const quint32 w = __atomic_load_n(&hdr->width, __ATOMIC_RELAXED);
        const quint32 h = __atomic_load_n(&hdr->height, __ATOMIC_RELAXED);
        if (w == 0 || h == 0 || w > ZURF_SHM_MAX_W || h > ZURF_SHM_MAX_H)
            return CopyResult::NoFrame; // none published yet
        const QRect whole(0, 0, int(w), int(h));
        // Both slots always hold complete frames, so copying only what the
        // engine reported as changed keeps the copy exact. A frame whose
        // event hasn't been read yet is caught up when its event arrives.
        const QRect want = m_pendingFull || whole.size() != m_frameSize ? whole : m_pendingDamage.intersected(whole);
        QRect r;
        uchar *dst = nullptr;
        size_t dstStride = 0;
        if (texture) {
            r = texture->beginUpdate(whole.size(), want);
            dst = texture->staging();
            dstStride = size_t(r.width()) * 4;
        } else {
            bool fresh = false;
            if (m_fallbackImage.size() != whole.size()) {
                m_fallbackImage = QImage(whole.size(), QImage::Format_ARGB32_Premultiplied);
                fresh = true;
            }
            r = fresh ? whole : want;
            dstStride = size_t(m_fallbackImage.bytesPerLine());
            dst = m_fallbackImage.bits() + dstStride * size_t(r.y()) + size_t(r.x()) * 4;
        }
        const uchar *src = slots + size_t((s1 / 2) % 2) * ZURF_SHM_SLOT_SIZE;
        const size_t row = size_t(w) * 4;
        if (r.isEmpty()) {
        } else if (r.width() == int(w) && dstStride == row) {
            memcpy(dst, src + row * size_t(r.y()), row * size_t(r.height()));
        } else {
            const size_t off = size_t(r.x()) * 4, len = size_t(r.width()) * 4;
            for (int y = 0; y < r.height(); ++y)
                memcpy(dst + dstStride * size_t(y), src + row * size_t(r.y() + y) + off, len);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        // Accept only if nothing was published meanwhile: then both the
        // header and the slot belong to the same frame. Otherwise the
        // engine may have been writing this very slot: copy again.
        if (__atomic_load_n(&hdr->seq, __ATOMIC_ACQUIRE) == s1) {
            if (texture)
                texture->commitStaging();
            m_frameSize = whole.size();
            m_pendingDamage = QRect();
            m_pendingFull = false;
            return CopyResult::Copied;
        }
    }
    return CopyResult::Torn;
}

QSGNode *WebSurface::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (m_sentVisible == 0) {
        // hidden: keep what's on the GPU (if anything), copy nothing, and
        // drop a staging buffer that never made it to an upload
        if (auto *tex = node ? qobject_cast<FrameTexture *>(node->texture()) : nullptr)
            tex->dropStaging();
        return node;
    }
    if (!window()) {
        delete node;
        return nullptr;
    }
    QRhi *rhi = window()->rhi();
    const bool partial = FrameTexture::isSupported(rhi);
    if (node && partial != bool(qobject_cast<FrameTexture *>(node->texture()))) {
        delete node; // scene graph backend changed under us
        node = nullptr;
    }
    const bool newNode = !node;
    if (!node) {
        node = new QSGSimpleTextureNode;
        node->setOwnsTexture(true);
        node->setFiltering(QSGTexture::Linear);
        if (partial)
            node->setTexture(new FrameTexture(rhi));
        m_textureDirty = true;
    }
    if (m_textureDirty) {
        // new or lost texture: everything comes from the shm slot again
        m_pendingFull = true;
        m_frameDirty = true;
    }
    bool copied = false;
    if (m_frameDirty) {
        m_frameDirty = false;
        switch (copyFrame(partial ? static_cast<FrameTexture *>(node->texture()) : nullptr)) {
        case CopyResult::Copied:
            copied = true;
            m_textureDirty = false;
            break;
        case CopyResult::Torn: // the engine published meanwhile: next frame
            if (partial)
                static_cast<FrameTexture *>(node->texture())->dropStaging();
            m_frameDirty = true;
            QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
            break;
        case CopyResult::NoFrame: // wait for ZURF_EV_FRAME
            m_frameDirty = m_textureDirty;
            break;
        }
    }
    if (m_frameSize.isEmpty() || (m_textureDirty && newNode)) {
        delete node; // nothing to show yet
        return nullptr;
    }
    // (an existing node whose re-copy failed keeps showing its texture)
    if (copied) {
        if (partial)
            node->markDirty(QSGNode::DirtyMaterial);
        else
            node->setTexture(window()->createTextureFromImage(m_fallbackImage)); // node deletes the old one
    }
    m_frameResident = true;
    // Map device pixels to logical ones; while a resize is in flight the
    // stale frame keeps its size instead of being stretched.
    const qreal dpr = devicePixelRatio();
    node->setRect(QRectF(0, 0, m_frameSize.width() / dpr, m_frameSize.height() / dpr));
    return node;
}

void WebSurface::releaseResources()
{
    m_textureDirty = true;
}

void WebSurface::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        sendGeometry();
}

void WebSurface::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    switch (change) {
    case ItemSceneChange:
        if (value.window) {
            connect(value.window, &QWindow::visibleChanged, this, &WebSurface::sendVisibility, Qt::UniqueConnection);
            connect(value.window, &QWindow::screenChanged, this, &WebSurface::sendGeometry, Qt::UniqueConnection);
        }
        sendVisibility();
        sendGeometry();
        break;
    case ItemVisibleHasChanged:
        sendVisibility();
        break;
    case ItemDevicePixelRatioHasChanged:
        sendGeometry();
        update();
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// input

int WebSurface::modifiers(Qt::KeyboardModifiers mods, Qt::MouseButtons buttons) const
{
    int m = 0;
    if (mods & Qt::ShiftModifier)
        m |= ZURF_MOD_SHIFT;
    if (mods & Qt::ControlModifier)
        m |= ZURF_MOD_CTRL;
    if (mods & Qt::AltModifier)
        m |= ZURF_MOD_ALT;
    if (mods & Qt::MetaModifier)
        m |= ZURF_MOD_META;
    if (buttons & Qt::LeftButton)
        m |= ZURF_MOD_BUTTON_LEFT;
    if (buttons & Qt::MiddleButton)
        m |= ZURF_MOD_BUTTON_MIDDLE;
    if (buttons & Qt::RightButton)
        m |= ZURF_MOD_BUTTON_RIGHT;
    return m;
}

static int protoButton(Qt::MouseButton b)
{
    switch (b) {
    case Qt::MiddleButton:
        return 1;
    case Qt::RightButton:
        return 2;
    default:
        return 0;
    }
}

// Keys the host handles itself even while the page has focus: find and the
// tab shortcuts (switching/closing only when there are tabs).
bool WebSurface::isHostShortcut(QKeyEvent *ke) const
{
    if (ke->matches(QKeySequence::Find) || ke->matches(QKeySequence::AddTab))
        return true;
    if (m_tabs.size() < 2)
        return false;
    const bool ctrl = ke->modifiers() & Qt::ControlModifier;
    return ke->matches(QKeySequence::Close) || ke->matches(QKeySequence::NextChild) || ke->matches(QKeySequence::PreviousChild)
        || (ctrl && (ke->key() == Qt::Key_PageUp || ke->key() == Qt::Key_PageDown || ke->key() == Qt::Key_Backtab));
}

bool WebSurface::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        // The page gets every key while focused (Ctrl+C, Ctrl+A, Esc, ...),
        // except the host's own find shortcut.
        auto *ke = static_cast<QKeyEvent *>(e);
        if (!isHostShortcut(ke)) {
            e->accept();
            return true;
        }
    }
    return QQuickItem::event(e);
}

void WebSurface::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::BackButton) {
        goBack();
        Q_EMIT backRequested();
        return;
    }
    if (e->button() == Qt::ForwardButton) {
        goForward();
        Q_EMIT forwardRequested();
        return;
    }
    forceActiveFocus(Qt::MouseFocusReason);
    const QPointF p = e->position();
    sendInts(ZURF_OP_MOUSE_BUTTON, {qRound(p.x()), qRound(p.y()), protoButton(e->button()), 1, 0, modifiers(e->modifiers(), e->buttons())});
    e->accept();
}

void WebSurface::mouseDoubleClickEvent(QMouseEvent *e)
{
    // Qt replaces the second press with this; the engine counts clicks itself.
    mousePressEvent(e);
}

void WebSurface::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::BackButton || e->button() == Qt::ForwardButton)
        return;
    const QPointF p = e->position();
    sendInts(ZURF_OP_MOUSE_BUTTON, {qRound(p.x()), qRound(p.y()), protoButton(e->button()), 0, 0, modifiers(e->modifiers(), e->buttons())});
    e->accept();
}

void WebSurface::mouseMoveEvent(QMouseEvent *e)
{
    const QPointF p = e->position();
    sendInts(ZURF_OP_MOUSE_MOVE, {qRound(p.x()), qRound(p.y()), modifiers(e->modifiers(), e->buttons())});
    e->accept();
}

void WebSurface::hoverMoveEvent(QHoverEvent *e)
{
    const QPointF p = e->position();
    sendInts(ZURF_OP_MOUSE_MOVE, {qRound(p.x()), qRound(p.y()), modifiers(e->modifiers(), Qt::NoButton)});
}

void WebSurface::hoverLeaveEvent(QHoverEvent *)
{
    send(ZURF_OP_MOUSE_LEAVE);
    if (!m_hoveredLink.isEmpty()) {
        m_hoveredLink.clear();
        Q_EMIT hoveredLinkChanged();
    }
}

void WebSurface::wheelEvent(QWheelEvent *e)
{
    const QPointF p = e->position();
    const bool precise = !e->pixelDelta().isNull();
    QPointF d = precise ? QPointF(e->pixelDelta()) : QPointF(e->angleDelta());
    if (e->inverted())
        d = -d;
    // Shift+wheel scrolls horizontally, like browsers do.
    if (!precise && (e->modifiers() & Qt::ShiftModifier) && d.x() == 0)
        d = QPointF(d.y(), 0);
    sendInts(ZURF_OP_SCROLL, {qRound(p.x()), qRound(p.y()), qRound(d.x() * 100), qRound(d.y() * 100), precise ? 1 : 0});
    e->accept();
}

void WebSurface::sendKey(QKeyEvent *e, bool down)
{
    quint32 keysym = e->nativeVirtualKey();
    const quint32 keycode = e->nativeScanCode();
    const int mods = modifiers(e->modifiers(), Qt::NoButton);
    const QString text = e->text();
    const bool chord = mods & (ZURF_MOD_CTRL | ZURF_MOD_ALT | ZURF_MOD_META);

    if (keysym == 0)
        keysym = keysymForQtKey(e->key());
    if (keysym == 0) {
        // No key identity at all (virtual keyboard, synthetic event): text only.
        if (down && !text.isEmpty() && !chord)
            insertText(text);
        return;
    }
    if (isDeadKeysym(keysym))
        return; // the composed character arrives with the next key

    if (down && !chord && !text.isEmpty()) {
        // Compose / dead-key results carry text the keysym doesn't produce.
        const quint32 cp = xkb_keysym_to_utf32(keysym);
        const QList<uint> ucs = text.toUcs4();
        if (ucs.size() != 1 || ucs.first() != cp) {
            if (!(ucs.size() == 1 && ucs.first() < 0x20)) { // control chars: let the key through
                insertText(text);
                return;
            }
        }
    }
    if (down && chord && (mods & ZURF_MOD_CTRL) && (keysym == XKB_KEY_v || keysym == XKB_KEY_V))
        pushClipboard(); // make sure the page pastes what the user just copied

    sendInts(ZURF_OP_KEY_NATIVE, {qint32(keysym), qint32(keycode), down ? 1 : 0, mods});
}

void WebSurface::keyPressEvent(QKeyEvent *e)
{
    sendKey(e, true);
    e->accept();
}

void WebSurface::keyReleaseEvent(QKeyEvent *e)
{
    sendKey(e, false);
    e->accept();
}

void WebSurface::inputMethodEvent(QInputMethodEvent *e)
{
    if (!e->commitString().isEmpty())
        insertText(e->commitString());
    e->accept();
}

QVariant WebSurface::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImEnabled:
        return true;
    case Qt::ImHints:
        return int(Qt::ImhNone);
    case Qt::ImCursorRectangle:
        return QRectF(0, height() - 1, 1, 1);
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

void WebSurface::focusInEvent(QFocusEvent *e)
{
    QQuickItem::focusInEvent(e);
    sendInts(ZURF_OP_FOCUS, {1});
    pushClipboard();
}

void WebSurface::focusOutEvent(QFocusEvent *e)
{
    QQuickItem::focusOutEvent(e);
    sendInts(ZURF_OP_FOCUS, {0});
}
