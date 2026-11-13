/*
 * DOS-XC /qt/qpa/qdosxc.h
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#pragma once
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformscreen.h>
#include <qpa/qplatformwindow.h>
#include <qpa/qplatformbackingstore.h>
#include <qpa/qplatformclipboard.h>
#include <QtFontDatabaseSupport/private/qfreetypefontdatabase_p.h>
#include <QtEventDispatcherSupport/private/qunixeventdispatcher_qpa_p.h>
#include <QtCore/QMimeData>
#include <QtGui/QImage>

extern "C" {
#include "embed.h"
}

QT_BEGIN_NAMESPACE

class QDosxcWindow;

class QDosxcScreen : public QPlatformScreen
{
public:
    QRect geometry() const override;
    QRect availableGeometry() const override;
    int depth() const override { return 32; }
    QImage::Format format() const override { return QImage::Format_RGB32; }
    QSizeF physicalSize() const override;
    QDpi logicalDpi() const override { return QDpi(96, 96); }
};

class QDosxcFontDatabase : public QFreeTypeFontDatabase
{
public:
    void populateFontDatabase() override;
    QFont defaultFont() const override;
};

class QDosxcClipboard : public QPlatformClipboard
{
public:
    QMimeData *mimeData(QClipboard::Mode mode) override;
    void setMimeData(QMimeData *data, QClipboard::Mode mode) override;
    bool supportsMode(QClipboard::Mode mode) const override { return mode == QClipboard::Clipboard; }
private:
    QMimeData m_data;
};

class QDosxcEventDispatcher : public QUnixEventDispatcherQPA
{
public:
    bool processEvents(QEventLoop::ProcessEventsFlags flags) override;
private:
    bool m_ticking = false;
};

class QDosxcIntegration : public QPlatformIntegration
{
public:
    QDosxcIntegration();
    ~QDosxcIntegration();
    bool hasCapability(Capability cap) const override;
    QPlatformWindow *createPlatformWindow(QWindow *window) const override;
    QPlatformBackingStore *createPlatformBackingStore(QWindow *window) const override;
    QAbstractEventDispatcher *createEventDispatcher() const override;
    QPlatformFontDatabase *fontDatabase() const override;
    QPlatformClipboard *clipboard() const override;

    static QDosxcIntegration *instance() { return s_instance; }
    bool pump();
    QList<QDosxcWindow *> windows;

private:
    static QDosxcIntegration *s_instance;
    QDosxcScreen *m_screen;
    mutable QDosxcFontDatabase *m_fonts = nullptr;
    mutable QDosxcClipboard *m_clipboard = nullptr;
    QDosxcWindow *m_active = nullptr;
};

class QDosxcWindow : public QPlatformWindow
{
public:
    explicit QDosxcWindow(QWindow *window);
    ~QDosxcWindow();
    void setVisible(bool visible) override;
    void setGeometry(const QRect &rect) override;
    void setWindowTitle(const QString &title) override;
    void setParent(const QPlatformWindow *parent) override;
    void setWindowState(Qt::WindowStates state) override;
    void requestActivateWindow() override;
    void raise() override;
    WId winId() const override;
    bool isExposed() const override;

    QDosxcWindow *topLevel();
    QPoint offsetInTop() const;
    void openWindow();
    void closeWindow();
    void copyOut(const QRect &from, const QPoint &to, const QImage &image);
    void repaintAll();
    void syncEmbed();
    void reportGeometry();

    struct window *gw = nullptr;     /* a top-level's desktop window, while shown */
    const QImage *image = nullptr;   /* its backing store */
    mutable struct embed_window embed;
    mutable bool foreign = false;    /* another thread draws it, through winId() */
    bool popup = false;
    bool frameless = false;          /* popup by type, not because it is full screen */
    bool fullscreen = false;
    QRect normal;
    bool shown = false;
    QRect reported;
};

class QDosxcBackingStore : public QPlatformBackingStore
{
public:
    explicit QDosxcBackingStore(QWindow *window) : QPlatformBackingStore(window) {}
    ~QDosxcBackingStore();
    QPaintDevice *paintDevice() override { return &m_image; }
    void flush(QWindow *window, const QRegion &region, const QPoint &offset) override;
    void resize(const QSize &size, const QRegion &staticContents) override;
    QImage toImage() const override { return m_image; }
private:
    QImage m_image;
};

QT_END_NAMESPACE
