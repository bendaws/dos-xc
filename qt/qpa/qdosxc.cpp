/*
 * DOS-XC /qt/qpa/qdosxc.cpp
 * Copyright (c) 2026 Ben Daws - see LICENSE.txt
 */

#include "qdosxc.h"
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qwindowsysteminterface.h>
#include <QtGui/QFont>
#include <QtGui/QGuiApplication>
#include <dirent.h>
#include <strings.h>
#include <unistd.h>

QT_BEGIN_NAMESPACE

static pthread_mutex_t gui_lock = PTHREAD_MUTEX_INITIALIZER;
static QList<struct window *> popup_pool;
static Qt::MouseButtons held;
static QWindow *mouse_window;
QDosxcIntegration *QDosxcIntegration::s_instance;

static QString exe_folder()
{
    char path[512];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return QString();
    path[n] = 0;
    char *slash = strrchr(path, '\\');
    char *other = strrchr(path, '/');
    if (other > slash) slash = other;
    if (slash) *slash = 0;
    return QString::fromLocal8Bit(path);
}

static QDosxcWindow *of(struct window *w) { return static_cast<QDosxcWindow *>(w->data); }

static Qt::KeyboardModifiers modifiers_of(int m)
{
    Qt::KeyboardModifiers mods;
    if (m & 1) mods |= Qt::ShiftModifier;
    if (m & 2) mods |= Qt::ControlModifier;
    if (m & 4) mods |= Qt::AltModifier;
    return mods;
}

static Qt::KeyboardModifiers held_modifiers()
{
    return modifiers_of((int)dosxc_sys_console(DOSXC_CONSOLE_MODIFIERS, 0, 0));
}

static void on_paint(struct window *w, const struct canvas *)
{
    if (of(w)) of(w)->repaintAll();
}

static void on_mouse(struct window *w, int x, int y, int kind, int value)
{
    QDosxcWindow *q = of(w);
    if (!q) return;
    QWindow *target = q->window();
    QPointF local(x, y), global(w->r.x + x, w->r.y + y);
    Qt::KeyboardModifiers mods = held_modifiers();
    if (mouse_window != target) {
        QWindowSystemInterface::handleEnterLeaveEvent(target, mouse_window, local, global);
        mouse_window = target;
    }
    switch (kind) {
    case M_DOWN:
    case M_DOUBLE:
        held |= Qt::LeftButton;
        QWindowSystemInterface::handleMouseEvent(target, local, global, held, Qt::LeftButton, QEvent::MouseButtonPress, mods);
        break;
    case M_UP:
        held &= ~Qt::LeftButton;
        QWindowSystemInterface::handleMouseEvent(target, local, global, held, Qt::LeftButton, QEvent::MouseButtonRelease, mods);
        break;
    case M_MOVE:
        QWindowSystemInterface::handleMouseEvent(target, local, global, held, Qt::NoButton, QEvent::MouseMove, mods);
        break;
    case M_RIGHT:
        QWindowSystemInterface::handleMouseEvent(target, local, global, held | Qt::RightButton, Qt::RightButton, QEvent::MouseButtonPress, mods);
        QWindowSystemInterface::handleMouseEvent(target, local, global, held, Qt::RightButton, QEvent::MouseButtonRelease, mods);
        break;
    case M_WHEEL:
        QWindowSystemInterface::handleWheelEvent(target, local, global, QPoint(), QPoint(0, value * 120), mods);
        break;
    }
}

static int key_code(int key, int m, QString *text)
{
    static const int special[] = {
        Qt::Key_Up, Qt::Key_Down, Qt::Key_Right, Qt::Key_Left, Qt::Key_Home, Qt::Key_End, Qt::Key_Insert, Qt::Key_Delete,
        Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_F1, Qt::Key_F2, Qt::Key_F3, Qt::Key_F4, Qt::Key_F5, Qt::Key_F6,
        Qt::Key_F7, Qt::Key_F8, Qt::Key_F9, Qt::Key_F10, Qt::Key_F11, Qt::Key_F12,
    };
    if (key >= 0x101 && key <= 0x116) return special[key - 0x101];
    switch (key) {
    case 8: return Qt::Key_Backspace;
    case 9: return (m & 1) ? Qt::Key_Backtab : Qt::Key_Tab;
    case 10:
    case 13: *text = QStringLiteral("\r"); return Qt::Key_Return;
    case 27: return Qt::Key_Escape;
    case 127: return Qt::Key_Delete;
    }
    if (key >= 1 && key <= 26) return Qt::Key_A + key - 1;
    if (key < 32 || key > 0xff) return 0;
    QChar c(key);
    if (!(m & 6)) *text = QString(c);
    return c.toUpper().unicode();
}

static void on_key(struct window *w, int key)
{
    QDosxcWindow *q = of(w);
    if (!q) return;
    QString text;
    int code = key_code(key, gui_key_modifiers, &text);
    if (!code) return;
    Qt::KeyboardModifiers mods = modifiers_of(gui_key_modifiers);
    QWindow *target = QGuiApplication::focusWindow() ? QGuiApplication::focusWindow() : q->window();
    QWindowSystemInterface::handleKeyEvent(target, QEvent::KeyPress, code, mods, text);
    QWindowSystemInterface::handleKeyEvent(target, QEvent::KeyRelease, code, mods, text);
}

static void on_resized(struct window *w)
{
    QDosxcWindow *q = of(w);
    if (!q) return;
    q->reportGeometry();
    QWindowSystemInterface::handleExposeEvent(q->window(), QRect(QPoint(), q->geometry().size()));
}

static int on_closing(struct window *w)
{
    if (of(w)) QWindowSystemInterface::handleCloseEvent(of(w)->window());
    return 0;
}

static const struct app_ops qt_ops = {on_paint, on_mouse, on_key, nullptr, nullptr, nullptr, on_resized, on_closing};

/* ---- screen, fonts, clipboard -------------------------------------------- */

QRect QDosxcScreen::geometry() const
{
    struct gui_board *b = gui_board();
    return QRect(0, 0, b && b->screen_w ? b->screen_w : 1024, b && b->screen_h ? b->screen_h : 768);
}

QRect QDosxcScreen::availableGeometry() const { return geometry().adjusted(0, 0, 0, -27); }

QSizeF QDosxcScreen::physicalSize() const { return QSizeF(geometry().size()) * 25.4 / 96; }

static void add_fonts(const QString &folder)
{
    QByteArray dir = folder.toLocal8Bit();
    DIR *d = opendir(dir.constData());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf") && strcasecmp(dot, ".ttc"))) continue;
        QFreeTypeFontDatabase::addTTFile(QByteArray(), dir + '\\' + e->d_name);
    }
    closedir(d);
}

void QDosxcFontDatabase::populateFontDatabase()
{
    add_fonts(exe_folder() + QLatin1String("\\FONTS"));
    add_fonts(QStringLiteral("\\SYSTEM\\FONTS"));
}

QFont QDosxcFontDatabase::defaultFont() const { return QFont(QStringLiteral("DejaVu Sans"), 9); }

QMimeData *QDosxcClipboard::mimeData(QClipboard::Mode mode)
{
    if (mode != QClipboard::Clipboard) return nullptr;
    QByteArray text(65536, Qt::Uninitialized);
    text.resize(gui_clipboard_get(text.data(), text.size()));
    m_data.setText(QString::fromUtf8(text));
    return &m_data;
}

void QDosxcClipboard::setMimeData(QMimeData *data, QClipboard::Mode mode)
{
    if (mode != QClipboard::Clipboard) return;
    QByteArray text = data && data->hasText() ? data->text().toUtf8() : QByteArray();
    gui_clipboard_set(text.constData(), text.size());
    if (data != &m_data) delete data;
    emitChanged(mode);
}

/* ---- the event loop ------------------------------------------------------ */

bool QDosxcEventDispatcher::processEvents(QEventLoop::ProcessEventsFlags flags)
{
    if (!m_ticking) {
        QAbstractEventDispatcher::registerTimer(10, Qt::CoarseTimer, this);   /* so a wait ends in time to look at the desktop again */
        m_ticking = true;
    }
    if (QDosxcIntegration::instance()) QDosxcIntegration::instance()->pump();
    if (QWindowSystemInterface::windowSystemEventsQueued()) flags &= ~QEventLoop::WaitForMoreEvents;
    return QUnixEventDispatcherQPA::processEvents(flags);
}

QDosxcIntegration::QDosxcIntegration()
{
    if (gui_connect()) qFatal("This program runs on the desktop.");
    QString icon = exe_folder() + QLatin1String("\\ICON.PNG");
    if (!access(icon.toLocal8Bit().constData(), R_OK)) gui_set_icon_file(icon.toLocal8Bit().constData());
    s_instance = this;
    m_screen = new QDosxcScreen;
    QWindowSystemInterface::handleScreenAdded(m_screen);
}

QDosxcIntegration::~QDosxcIntegration()
{
    QWindowSystemInterface::handleScreenRemoved(m_screen);
    delete m_fonts;
    delete m_clipboard;
    s_instance = nullptr;
}

bool QDosxcIntegration::hasCapability(Capability cap) const
{
    switch (cap) {
    case ThreadedPixmaps:
    case MultipleWindows:
    case NonFullScreenWindows:
        return true;
    default:
        return QPlatformIntegration::hasCapability(cap);
    }
}

QPlatformWindow *QDosxcIntegration::createPlatformWindow(QWindow *window) const { return new QDosxcWindow(window); }
QPlatformBackingStore *QDosxcIntegration::createPlatformBackingStore(QWindow *window) const { return new QDosxcBackingStore(window); }
QAbstractEventDispatcher *QDosxcIntegration::createEventDispatcher() const { return new QDosxcEventDispatcher; }

QPlatformFontDatabase *QDosxcIntegration::fontDatabase() const
{
    if (!m_fonts) m_fonts = new QDosxcFontDatabase;
    return m_fonts;
}

QPlatformClipboard *QDosxcIntegration::clipboard() const
{
    if (!m_clipboard) m_clipboard = new QDosxcClipboard;
    return m_clipboard;
}

bool QDosxcIntegration::pump()
{
    pthread_mutex_lock(&gui_lock);
    bool did = gui_poll();
    for (QDosxcWindow *w : qAsConst(windows)) w->syncEmbed();
    pthread_mutex_unlock(&gui_lock);
    for (QDosxcWindow *w : qAsConst(windows))
        if (w->gw && !w->popup) w->reportGeometry();

    struct gui_board *b = gui_board();
    int pid = dosxc_sys_getpid();
    QDosxcWindow *active = nullptr;
    bool popup_shown = false;
    for (QDosxcWindow *w : qAsConst(windows)) popup_shown |= w->gw && w->popup;
    for (int i = 0; i < b->window_count && i < GUI_WINDOWS; ++i)
        if (b->windows[i].pid == pid && b->windows[i].active)
            for (QDosxcWindow *w : qAsConst(windows))
                if ((uintptr_t)w->gw == b->windows[i].window) active = w;
    if (active != m_active && (active || !popup_shown)) {
        m_active = active;
        QWindowSystemInterface::handleWindowActivated(active ? active->window() : nullptr);
        did = true;
    }
    return did;
}

/* ---- windows ------------------------------------------------------------- */

QDosxcWindow::QDosxcWindow(QWindow *window) : QPlatformWindow(window)
{
    Qt::WindowType type = window->type();
    frameless = type == Qt::Popup || type == Qt::ToolTip || type == Qt::SplashScreen || (window->flags() & Qt::FramelessWindowHint);
    popup = frameless;
    memset(&embed, 0, sizeof(embed));
    embed.lock = &gui_lock;
    QDosxcIntegration::instance()->windows.append(this);
}

QDosxcWindow::~QDosxcWindow()
{
    closeWindow();
    if (mouse_window == window()) mouse_window = nullptr;
    QDosxcIntegration::instance()->windows.removeAll(this);
    pthread_mutex_lock(&gui_lock);
    embed.top = nullptr;
    embed.visible = 0;
    pthread_mutex_unlock(&gui_lock);
}

QDosxcWindow *QDosxcWindow::topLevel()
{
    QWindow *w = window();
    while (w->parent()) w = w->parent();
    return static_cast<QDosxcWindow *>(w->handle());
}

QPoint QDosxcWindow::offsetInTop() const
{
    QPoint at;
    for (QWindow *w = window(); w->parent(); w = w->parent()) at += w->geometry().topLeft();
    return at;
}

void QDosxcWindow::openWindow()
{
    if (gw || window()->parent()) return;
    QRect r = geometry();
    int width = qMax(r.width(), 1), height = qMax(r.height(), 1);
    QByteArray title = window()->title().toLocal8Bit();
    pthread_mutex_lock(&gui_lock);
    if (popup && !popup_pool.isEmpty()) {
        gw = popup_pool.takeLast();
        if (gw->content.w != width || gw->content.h != height) surface_size(&gw->content, width, height);
        gw->r = (struct rect){r.x(), r.y(), width, height};
        gw->data = this;
        gw->needs_paint = 1;
    } else {
        if (popup) window_popup_next(r.x(), r.y());
        gw = window_open(width, height, title.constData(), ICON_PROGRAM, &qt_ops, this);
    }
    if (gw) {
        gw->track_mouse = 1;
        gw->resizable = window()->minimumSize() != window()->maximumSize();
        if (popup) window_raise(gw);
    }
    pthread_mutex_unlock(&gui_lock);
    reported = r;
}

void QDosxcWindow::closeWindow()
{
    if (!gw) return;
    pthread_mutex_lock(&gui_lock);
    if (popup) {
        gw->data = nullptr;
        gw->r.x = gw->r.y = -32000;
        popup_pool.append(gw);
    } else {
        window_close(gw);
    }
    gw = nullptr;
    pthread_mutex_unlock(&gui_lock);
}

void QDosxcWindow::setVisible(bool visible)
{
    shown = visible;
    QRect r = geometry();
    if (visible) openWindow();
    else closeWindow();
    QWindowSystemInterface::handleExposeEvent(window(), visible ? QRegion(QRect(QPoint(), r.size())) : QRegion());
    if (QWindow *parent = window()->parent())
        if (!visible && parent->handle())
            QWindowSystemInterface::handleExposeEvent(parent, QRect(r.topLeft(), r.size()));
}

bool QDosxcWindow::isExposed() const
{
    if (window()->parent()) return shown;
    return gw != nullptr;
}

void QDosxcWindow::setGeometry(const QRect &rect)
{
    if (!gw) {
        QPlatformWindow::setGeometry(rect);
        if (window()->parent() && shown) {
            QWindowSystemInterface::handleGeometryChange(window(), rect);
            QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), rect.size()));
        }
        return;
    }
    if (popup) {
        pthread_mutex_lock(&gui_lock);
        int width = qMax(rect.width(), 1), height = qMax(rect.height(), 1);
        if (gw->content.w != width || gw->content.h != height) surface_size(&gw->content, width, height);
        gw->r = (struct rect){rect.x(), rect.y(), width, height};
        pthread_mutex_unlock(&gui_lock);
        QPlatformWindow::setGeometry(rect);
        reported = rect;
        QWindowSystemInterface::handleGeometryChange(window(), rect);
        QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), rect.size()));
        return;
    }
    if (rect.size() != QSize(gw->r.w, gw->r.h)) window_request_size(gw, rect.width(), rect.height());
}

void QDosxcWindow::reportGeometry()
{
    if (!gw) return;
    QRect now(gw->r.x, gw->r.y, gw->r.w, gw->r.h);
    if (now == reported) return;
    reported = now;
    QPlatformWindow::setGeometry(now);
    QWindowSystemInterface::handleGeometryChange(window(), now);
}

void QDosxcWindow::setWindowTitle(const QString &title)
{
    if (gw) window_title(gw, title.toLocal8Bit().constData());
}

void QDosxcWindow::setParent(const QPlatformWindow *) {}

/* Full screen is a borderless window over the whole screen. */
void QDosxcWindow::setWindowState(Qt::WindowStates state)
{
    bool full = state & Qt::WindowFullScreen;
    if (full == fullscreen || window()->parent()) return;
    bool open = gw != nullptr;
    closeWindow();
    fullscreen = full;
    popup = full || frameless;
    if (full) normal = geometry();
    QRect r = full ? screen()->geometry() : normal;
    QPlatformWindow::setGeometry(r);
    if (open) {
        openWindow();
        requestActivateWindow();
    }
    QWindowSystemInterface::handleGeometryChange(window(), r);
    QWindowSystemInterface::handleExposeEvent(window(), QRect(QPoint(), r.size()));
}

void QDosxcWindow::requestActivateWindow()
{
    if (!gw || (popup && !fullscreen)) return;
    struct gui_window_info info;
    memset(&info, 0, sizeof(info));
    info.window = (uintptr_t)gw;
    gui_activate(&info);
}

void QDosxcWindow::raise()
{
    if (gw) window_raise(gw);
}

WId QDosxcWindow::winId() const
{
    foreign = true;
    return (WId)&embed;
}

void QDosxcWindow::syncEmbed()
{
    if (!window()->parent()) return;
    QDosxcWindow *top = topLevel();
    QPoint at = offsetInTop();
    embed.top = top ? top->gw : nullptr;
    embed.x = at.x();
    embed.y = at.y();
    embed.w = geometry().width();
    embed.h = geometry().height();
    bool visible = shown;
    for (QWindow *w = window()->parent(); w; w = w->parent()) visible &= w->isVisible();
    embed.visible = visible && embed.top;
}

/* Copies FROM of IMAGE to TO in the desktop window, leaving out children
   that are drawn by another thread.  Called with gui_lock held. */
void QDosxcWindow::copyOut(const QRect &from, const QPoint &to, const QImage &image)
{
    QDosxcWindow *top = topLevel();
    struct window *g = top ? top->gw : nullptr;
    if (!g || !g->content.px) return;
    QRegion region(QRect(to, from.size()));
    region &= QRect(0, 0, g->content.w, g->content.h);
    for (QDosxcWindow *w : qAsConst(QDosxcIntegration::instance()->windows))
        if (w != this && w->foreign && w->embed.visible && w->embed.top == g)
            region -= QRect(w->embed.x, w->embed.y, w->embed.w, w->embed.h);
    QPoint shift = from.topLeft() - to;
    for (const QRect &r : region) {
        QRect src = r.translated(shift) & image.rect();
        if (src.isEmpty()) continue;
        for (int y = 0; y < src.height(); ++y)
            memcpy(g->content.px + (size_t)(r.y() + y) * g->content.w + r.x(),
                   image.constScanLine(src.y() + y) + src.x() * 4, (size_t)src.width() * 4);
    }
    ++g->seq;
}

void QDosxcWindow::repaintAll()
{
    if (image) copyOut(image->rect(), QPoint(), *image);
}

/* ---- backing store ------------------------------------------------------- */

QDosxcBackingStore::~QDosxcBackingStore()
{
    if (auto *w = static_cast<QDosxcWindow *>(window()->handle()))
        if (w->image == &m_image) w->image = nullptr;
}

void QDosxcBackingStore::resize(const QSize &size, const QRegion &)
{
    if (m_image.size() != size) {
        pthread_mutex_lock(&gui_lock);
        m_image = QImage(size, QImage::Format_RGB32);
        pthread_mutex_unlock(&gui_lock);
    }
    if (auto *w = static_cast<QDosxcWindow *>(window()->handle())) w->image = &m_image;
}

void QDosxcBackingStore::flush(QWindow *window, const QRegion &region, const QPoint &offset)
{
    auto *w = static_cast<QDosxcWindow *>(window->handle());
    if (!w) return;
    QPoint at = w->offsetInTop();
    pthread_mutex_lock(&gui_lock);
    for (const QRect &r : region) w->copyOut(r.translated(offset), r.topLeft() + at, m_image);
    pthread_mutex_unlock(&gui_lock);
}

QT_END_NAMESPACE
