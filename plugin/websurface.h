/*
 *  SPDX-FileCopyrightText: 2026 Francesco Magazzu
 *
 *  SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJSValue>
#include <QPointer>
#include <QQuickItem>
#include <QUrl>
#include <QtQml/qqmlregistration.h>

class QProcess;
class QSocketNotifier;
class QTimer;

/*
 * WebSurface — a QQuickItem that shows a web page rendered by an
 * out-of-process engine (chatai-webwpe, WPE WebKit) speaking the Zurf engine
 * protocol. Nothing web-related is loaded into the host process: the item
 * only maps a shared-memory frame slot, uploads it as a texture and forwards
 * input over a socket.
 *
 * Setting `active` to false shuts the engine down (cookies and storage stay
 * on disk under `profileDirectory`); setting it back starts a fresh one.
 */
class WebSurface : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT

    // engine lifecycle
    Q_PROPERTY(bool active READ isActive WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(bool suspended READ isSuspended WRITE setSuspended NOTIFY suspendedChanged)
    Q_PROPERTY(QString enginePath READ enginePath WRITE setEnginePath NOTIFY enginePathChanged)
    Q_PROPERTY(QString profileDirectory READ profileDirectory WRITE setProfileDirectory NOTIFY profileDirectoryChanged)
    Q_PROPERTY(int memoryLimitMb READ memoryLimitMb WRITE setMemoryLimitMb NOTIFY memoryLimitMbChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

    // page state
    Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool loading READ isLoading NOTIFY loadStateChanged)
    Q_PROPERTY(int loadProgress READ loadProgress NOTIFY loadStateChanged)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY loadStateChanged)
    Q_PROPERTY(bool canGoForward READ canGoForward NOTIFY loadStateChanged)
    Q_PROPERTY(QString hoveredLink READ hoveredLink NOTIFY hoveredLinkChanged)
    Q_PROPERTY(bool popupOpen READ isPopupOpen NOTIFY popupOpenChanged)

    // preferences (applied live)
    Q_PROPERTY(QString userAgent READ userAgent WRITE setUserAgent NOTIFY prefsChanged)
    Q_PROPERTY(QString downloadDirectory READ downloadDirectory WRITE setDownloadDirectory NOTIFY prefsChanged)
    Q_PROPERTY(bool darkMode READ darkMode WRITE setDarkMode NOTIFY prefsChanged)
    Q_PROPERTY(bool javascriptCanOpenWindows READ javascriptCanOpenWindows WRITE setJavascriptCanOpenWindows NOTIFY prefsChanged)
    Q_PROPERTY(bool javascriptCanAccessClipboard READ javascriptCanAccessClipboard WRITE setJavascriptCanAccessClipboard NOTIFY prefsChanged)
    Q_PROPERTY(bool playbackRequiresUserGesture READ playbackRequiresUserGesture WRITE setPlaybackRequiresUserGesture NOTIFY prefsChanged)
    Q_PROPERTY(bool spatialNavigation READ spatialNavigation WRITE setSpatialNavigation NOTIFY prefsChanged)
    Q_PROPERTY(bool popupsInPlace READ popupsInPlace WRITE setPopupsInPlace NOTIFY prefsChanged)

public:
    enum Permission {
        PermissionNotifications = 1,
        PermissionGeolocation = 2,
        PermissionMicrophone = 3,
        PermissionCamera = 4,
        PermissionMicrophoneAndCamera = 5,
        PermissionScreen = 6,
        PermissionClipboard = 7,
        PermissionOther = 8,
    };
    Q_ENUM(Permission)

    enum ContextFlag {
        ContextEditable = 1,
        ContextSelection = 2,
        ContextLink = 4,
        ContextImage = 8,
        ContextMedia = 16,
    };
    Q_ENUM(ContextFlag)

    enum DownloadState {
        DownloadProgress = 0,
        DownloadFinished = 1,
        DownloadFailed = 2,
        DownloadCancelled = 3,
        DownloadStarted = 4,
    };
    Q_ENUM(DownloadState)

    explicit WebSurface(QQuickItem *parent = nullptr);
    ~WebSurface() override;

    bool isActive() const { return m_active; }
    void setActive(bool active);
    bool isRunning() const { return m_ready; }
    bool isSuspended() const { return m_suspended; }
    void setSuspended(bool suspended);
    QString enginePath() const { return m_enginePath; }
    void setEnginePath(const QString &path);
    QString profileDirectory() const { return m_profileDir; }
    void setProfileDirectory(const QString &dir);
    int memoryLimitMb() const { return m_memoryLimitMb; }
    void setMemoryLimitMb(int mb);
    QString lastError() const { return m_lastError; }

    QUrl url() const { return m_url; }
    void setUrl(const QUrl &url);
    QString title() const { return m_title; }
    bool isLoading() const { return m_loading; }
    int loadProgress() const { return m_progress; }
    bool canGoBack() const { return m_canGoBack; }
    bool canGoForward() const { return m_canGoForward; }
    QString hoveredLink() const { return m_hoveredLink; }
    bool isPopupOpen() const { return m_tabs.size() > 1; }

    QString userAgent() const { return m_userAgent; }
    void setUserAgent(const QString &ua);
    QString downloadDirectory() const { return m_downloadDir; }
    void setDownloadDirectory(const QString &dir);
    bool darkMode() const { return m_darkMode; }
    void setDarkMode(bool on);
    bool javascriptCanOpenWindows() const { return m_jsOpenWindows; }
    void setJavascriptCanOpenWindows(bool on);
    bool javascriptCanAccessClipboard() const { return m_jsClipboard; }
    void setJavascriptCanAccessClipboard(bool on);
    bool playbackRequiresUserGesture() const { return m_mediaGesture; }
    void setPlaybackRequiresUserGesture(bool on);
    bool spatialNavigation() const { return m_spatialNav; }
    void setSpatialNavigation(bool on);
    bool popupsInPlace() const { return m_popupsInPlace; }
    void setPopupsInPlace(bool on);

    Q_INVOKABLE void load(const QUrl &url);
    Q_INVOKABLE void goBack();
    Q_INVOKABLE void goForward();
    Q_INVOKABLE void reload();
    Q_INVOKABLE void reloadAndBypassCache();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void runJavaScript(const QString &script, const QJSValue &callback = QJSValue());
    Q_INVOKABLE void findText(const QString &text, bool backwards = false, bool caseSensitive = false);
    Q_INVOKABLE void stopFinding();
    Q_INVOKABLE void editCommand(const QString &command); // Copy, Cut, Paste, SelectAll, Undo, Redo
    Q_INVOKABLE void insertText(const QString &text);
    Q_INVOKABLE void closePopup();
    Q_INVOKABLE void replyFileChooser(int requestId, const QList<QUrl> &files);
    Q_INVOKABLE void replyPermission(int requestId, bool allow);
    Q_INVOKABLE void cancelDownload(int downloadId);
    Q_INVOKABLE void copyToClipboard(const QString &text);
    Q_INVOKABLE void restartEngine();

Q_SIGNALS:
    void activeChanged();
    void runningChanged();
    void suspendedChanged();
    void enginePathChanged();
    void profileDirectoryChanged();
    void memoryLimitMbChanged();
    void lastErrorChanged();
    void urlChanged();
    void titleChanged();
    void loadStateChanged();
    void hoveredLinkChanged();
    void popupOpenChanged();
    void prefsChanged();

    void fileChooserRequested(int requestId, bool multiple, const QStringList &mimeTypes);
    void permissionRequested(int requestId, int kind, const QString &origin);
    void notificationReceived(const QString &title, const QString &body);
    void contextMenuRequested(qreal x, qreal y, int flags, const QString &link, const QString &mediaUrl);
    void openExternalRequested(const QUrl &url);
    void downloadUpdated(int downloadId, int state, qint64 received, qint64 total, const QString &path);
    void findResult(int matches);
    void engineCrashed();
    void backRequested();
    void forwardRequested();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    void componentComplete() override;
    void releaseResources() override;

    bool event(QEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void hoverMoveEvent(QHoverEvent *e) override;
    void hoverLeaveEvent(QHoverEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void inputMethodEvent(QInputMethodEvent *e) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void focusInEvent(QFocusEvent *e) override;
    void focusOutEvent(QFocusEvent *e) override;

private:
    void startEngine();
    void stopEngine(bool graceful);
    void onEngineFinished();
    void onReadable();
    void onWritable();
    void handleEvent(quint8 op, const QByteArray &payload);
    void send(quint8 op, const QByteArray &payload = {});
    void sendInts(quint8 op, std::initializer_list<qint32> ints, const QByteArray &tail = {});
    void sendPrefs();
    void sendGeometry();
    void sendVisibility();
    void pushClipboard();
    bool copyFrame();
    qreal devicePixelRatio() const;
    int modifiers(Qt::KeyboardModifiers mods, Qt::MouseButtons buttons) const;
    void sendKey(QKeyEvent *e, bool down);
    void setLastError(const QString &error);
    void applyCursor(const QByteArray &cssName);
    QString resolvedEnginePath() const;

    // engine process + channels
    QProcess *m_process = nullptr;
    int m_fd = -1;
    QSocketNotifier *m_readNotifier = nullptr;
    QSocketNotifier *m_writeNotifier = nullptr;
    QByteArray m_in;
    QByteArray m_out;
    QString m_shmPath;
    void *m_shm = nullptr;
    QTimer *m_killTimer = nullptr;
    QTimer *m_restartTimer = nullptr;
    int m_crashCount = 0;

    // frame
    QImage m_frame;
    quint32 m_frameSeq = 0;
    bool m_frameDirty = false;
    bool m_textureDirty = false;

    // state
    bool m_active = true;
    bool m_complete = false;
    bool m_ready = false;
    bool m_suspended = false;
    QString m_enginePath;
    QString m_profileDir;
    int m_memoryLimitMb = 0;
    QString m_lastError;

    QUrl m_url;
    QString m_title;
    bool m_loading = false;
    int m_progress = 0;
    bool m_canGoBack = false;
    bool m_canGoForward = false;
    QString m_hoveredLink;
    QList<int> m_tabs;
    int m_activeTab = 0;

    QString m_userAgent;
    QString m_downloadDir;
    bool m_darkMode = false;
    bool m_jsOpenWindows = true;
    bool m_jsClipboard = true;
    bool m_mediaGesture = false;
    bool m_spatialNav = false;
    bool m_popupsInPlace = true;

    QSize m_sentSize;
    int m_sentScale = 0;
    int m_sentVisible = -1;
    bool m_settingClipboard = false;
    int m_nextJsId = 1;
    QHash<int, QJSValue> m_jsCallbacks;
    Qt::MouseButtons m_buttons;
};
