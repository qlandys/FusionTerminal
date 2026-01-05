#include "MainWindow.h"
#include "DomWidget.h"
#include "LadderClient.h"
#include "PluginsWindow.h"
#include "SettingsWindow.h"
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QScreen>
#include <QColor>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPainter>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <QDebug>
#include <QFile>
#include <QTextStream>
#include <QProcessEnvironment>
#include <QPropertyAnimation>
#include <QStandardPaths>
#include <QEasingCurve>
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <windowsx.h>
#endif

// Simple append-only file logger to capture geometry/state traces when
// DebugView is not available. Helpful for reproducing issues on user's
// machine where reading OutputDebugString may be inconvenient.
static QString fusionLogFilePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QLatin1String("/.fusion_terminal");
    }
    QDir().mkpath(base);
    const QString logDir = QDir(base).filePath(QStringLiteral("logs"));
    QDir().mkpath(logDir);
    return QDir(logDir).filePath(QStringLiteral("fusion_terminal.log"));
}

static void logToFile(const QString &msg)
{
    const QString path = fusionLogFilePath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODate) << " " << msg << "\n";
}

static QString rectToString(const QRect &r)
{
    return QString("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

#ifdef Q_OS_WIN
static void applyNativeSnapStyleForHwnd(HWND hwnd)
{
    if (!hwnd) return;
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~WS_CAPTION;
    style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
#endif

// Smoothly animate window opacity from slightly transparent to fully opaque.
// Used to mask small visual jumps when we programmatically change geometry
// / window flags during restore.
static void animateWindowFadeIn(QWidget *w)
{
    if (!w) return;
    // Ensure starting opacity is slightly less than 1 so animation is visible
    w->setWindowOpacity(0.96);
    auto *anim = new QPropertyAnimation(w, "windowOpacity");
    anim->setDuration(180);
    anim->setStartValue(0.96);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::InOutCubic);
    QObject::connect(anim, &QPropertyAnimation::finished, anim, &QObject::deleteLater);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

// Ensure native window bounds match widget geometry; on some broken restore
// sequences the native HWND rect can diverge from QWidget geometry causing
// the visible area to be clipped. This helper forces the native bounds to
// the provided geometry when on Windows.
static void ensureNativeBounds(QWidget *w, const QRect &desired)
{
#ifdef Q_OS_WIN
    if (!w) return;
    HWND hwnd = reinterpret_cast<HWND>(w->winId());
    if (!hwnd) return;
    RECT r;
    if (GetWindowRect(hwnd, &r)) {
        QRect nativeRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
        // If native rect and desired geometry differ significantly, force it.
        const int dx = std::abs(nativeRect.x() - desired.x());
        const int dy = std::abs(nativeRect.y() - desired.y());
        const int dw = std::abs(nativeRect.width() - desired.width());
        const int dh = std::abs(nativeRect.height() - desired.height());
        if (dx > 4 || dy > 4 || dw > 8 || dh > 8) {
            // Log and set native bounds using SetWindowPos to avoid weird clipping.
            QString s = QStringLiteral("[MainWindow] Forcing native bounds to %1 (native was %2)").arg(rectToString(desired)).arg(rectToString(nativeRect));
            qDebug() << s;
            logToFile(s);
            SetWindowPos(hwnd, NULL, desired.x(), desired.y(), desired.width(), desired.height(), SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
#endif
}

MainWindow::MainWindow(const QString &backendPath,
                       const QString &symbol,
                       int levels,
                       QWidget *parent)
    : QMainWindow(parent)
    , m_backendPath(backendPath)
    , m_symbols(QStringList{symbol})
    , m_levels(levels)
    , m_workspaceTabs(nullptr)
    , m_workspaceStack(nullptr)
    , m_addTabButton(nullptr)
    , m_connectionIndicator(nullptr)
    , m_timeLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_pingLabel(nullptr)
    , m_orderPanel(nullptr)
    , m_orderSymbolLabel(nullptr)
    , m_orderPriceEdit(nullptr)
    , m_orderQuantityEdit(nullptr)
    , m_buyButton(nullptr)
    , m_sellButton(nullptr)
    , m_pluginsWindow(nullptr)
    , m_settingsWindow(nullptr)
    , m_tabs()
    , m_nextTabId(1)
    , m_timeTimer(nullptr)
        , m_topBar(nullptr)
        , m_minButton(nullptr)
    , m_maxButton(nullptr)
    , m_closeButton(nullptr)
{
    setWindowTitle(QStringLiteral("Ghost"));
    resize(1600, 900);
    setMinimumSize(800, 400);

    // Use frameless window but keep system menu and min/max buttons so
    // native snap & window controls behave as expected.
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint
                   | Qt::WindowMinMaxButtonsHint);
    // Use custom TitleBar instead of native OS strip.
    setWindowFlag(Qt::FramelessWindowHint);

    buildUi();
    createInitialWorkspace();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    QWidget *topBar = buildTopBar(central);
    QWidget *mainArea = buildMainArea(central);

    rootLayout->addWidget(topBar);
    rootLayout->addWidget(mainArea, 1);

    // Status bar removed (ping not needed)
    statusBar()->hide();

    // Time label update timer (UTC clock + offset).
    m_timeTimer = new QTimer(this);
    m_timeTimer->setInterval(1000);
    connect(m_timeTimer, &QTimer::timeout, this, &MainWindow::updateTimeLabel);
    m_timeTimer->start();
    updateTimeLabel();

    // Window buttons style + TitleBar bottom border, Р±Р»РёР·РєРѕ Рє VSCode.
    // Р”РµР»Р°Рј Р±РѕСЂС‚РёРє С‡СѓС‚СЊ СЃРІРµС‚Р»РµРµ Рё РґРѕР±Р°РІР»СЏРµРј С‚Р°РєРѕР№ Р¶Рµ Р±РѕСЂС‚РёРє Сѓ Р±РѕРєРѕРІРѕР№ РїР°РЅРµР»Рё.
    setStyleSheet(
        "QFrame#TitleBar {"
        "  background-color: #252526;"
        "  border-bottom: none;" /* РЈРґР°Р»СЏРµРј РЅРёР¶РЅСЋСЋ РіСЂР°РЅРёС†Сѓ Сѓ TitleBar */
        "}"
        "QFrame#SideToolbar {"
        "  background-color: transparent;"
        "  border-right: 1px solid #444444;"
        "}"
        "QFrame#MainAreaFrame {"
        "  border-top: 1px solid #444444;"
        "}"
        "QToolButton#WindowButtonMin, QToolButton#WindowButtonMax, QToolButton#WindowButtonClose {"
        "  background: transparent;"
        "  border: none;"
        "  color: #ffffff;"
        "  padding: 0px;"
        "}"
        "QToolButton#WindowButtonMin:hover, QToolButton#WindowButtonMax:hover {"
        "  background-color: #2a2a2a;"
        "}"
        "QToolButton#WindowButtonMin:pressed, QToolButton#WindowButtonMax:pressed {"
        "  background-color: #1f1f1f;"
        "}"
        "QToolButton#WindowButtonClose:hover {"
        "  background-color: #e81123;"
        "  color: #ffffff;"
        "}"
        "QToolButton#WindowButtonClose:pressed {"
        "  background-color: #c50f1f;"
        "  color: #ffffff;"
        "}"
        // РЈР±РёСЂР°РµРј hover-Р±СЌРєРіСЂР°СѓРЅРґ Сѓ Р±РѕРєРѕРІРѕР№ РїР°РЅРµР»Рё вЂ” РёРєРѕРЅРєР° СЃР°РјР° СЃС‚Р°РЅРµС‚ Р±РµР»РѕР№ РїСЂРё РЅР°РІРµРґРµРЅРёРё.
        "QToolButton#SideNavButton {"
        "  background: transparent;"
        "  border: none;"
        "  padding: 6px 0;"
        "}"
        // РЎС‚РёР»Рё РґР»СЏ РІРєР»Р°РґРѕРє РІ СЃС‚РёР»Рµ VSCode
        "QFrame#TabsContainer {"
        "  background-color: #252526;"          /* С‚РѕС‚ Р¶Рµ С„РѕРЅ, С‡С‚Рѕ Рё Сѓ TitleBar */
        "  border-bottom: none;"   /* РЈРґР°Р»СЏРµРј РЅРёР¶РЅСЋСЋ РіСЂР°РЅРёС†Сѓ Сѓ TabsContainer */
        "}"
        "QTabBar {"
        "  border: none;" /* РЈР±РёСЂР°РµРј РІСЃРµ РіСЂР°РЅРёС†С‹ Сѓ СЃР°РјРѕРіРѕ QTabBar */
        "  background-color: #252526;" /* РЈСЃС‚Р°РЅР°РІР»РёРІР°РµРј С„РѕРЅ QTabBar С‚Р°РєРёРј Р¶Рµ, РєР°Рє Сѓ TitleBar */
        "}"
        "QTabBar::tab {"
        "  background-color: #252526;" /* Р¤РѕРЅ РЅРµР°РєС‚РёРІРЅРѕР№ РІРєР»Р°РґРєРё, РєР°Рє Сѓ С‚Р°Р№С‚Р»-Р±Р°СЂР° */
        "  color: #cccccc;" /* Р¦РІРµС‚ С‚РµРєСЃС‚Р° РЅРµР°РєС‚РёРІРЅРѕР№ РІРєР»Р°РґРєРё */
        "  padding: 0px 12px;" /* РћС‚СЃС‚СѓРїС‹, С‡С‚РѕР±С‹ РІРєР»Р°РґРєРё Р·Р°РЅРёРјР°Р»Рё РІСЃСЋ РІС‹СЃРѕС‚Сѓ */
        "  border: none;" /* РЈР±РёСЂР°РµРј РІСЃРµ Р±РѕСЂС‚РёРєРё Сѓ РІРєР»Р°РґРѕРє */
        "  margin-left: 0px;" /* РЈР±РёСЂР°РµРј РѕС‚СЃС‚СѓРї РјРµР¶РґСѓ РІРєР»Р°РґРєР°РјРё */
        "  height: 100%;" /* Р’РєР»Р°РґРєР° РІРѕ РІСЃСЋ РІС‹СЃРѕС‚Сѓ С‚Р°Р№С‚Р»-Р±Р°СЂР° */
        "}"
        "QTabBar::tab:selected {"
        "  background-color: #1e1e1e;" /* Р¤РѕРЅ Р°РєС‚РёРІРЅРѕР№ РІРєР»Р°РґРєРё */
        "  color: #ffffff;" /* Р¦РІРµС‚ С‚РµРєСЃС‚Р° Р°РєС‚РёРІРЅРѕР№ РІРєР»Р°РґРєРё */
        "  border-top: 2px solid #007acc;" /* РЎРёРЅСЏСЏ РїРѕР»РѕСЃРєР° СЃРІРµСЂС…Сѓ */
        "  border: none;" /* РЈР±РёСЂР°РµРј РІСЃРµ Р±РѕСЂС‚РёРєРё Сѓ Р°РєС‚РёРІРЅРѕР№ РІРєР»Р°РґРєРё, РєСЂРѕРјРµ border-top */
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background-color: #2d2d2d;" /* Р¤РѕРЅ РїСЂРё РЅР°РІРµРґРµРЅРёРё РЅР° РЅРµР°РєС‚РёРІРЅСѓСЋ РІРєР»Р°РґРєСѓ */
        "}"
        "QTabBar::close-button {"
        "  border: none;"
        "  background: transparent;"
        "  margin: 2px;"
        "  padding: 0;"
        "}"
        "QTabBar::close-button:hover {"
        "  background: #555555;"
        "}"
        "");

    // Р”РѕРїРѕР»РЅРёС‚РµР»СЊРЅС‹Р№ СЃС‚РёР»СЊ: СѓР±РёСЂР°РµРј СЃС‚Р°РЅРґР°СЂС‚РЅСѓСЋ Р»РёРЅРёСЋ QTabBar::pane,
    // С‡С‚РѕР±С‹ РЅРµ Р±С‹Р»Рѕ РІС‚РѕСЂРѕР№ СЃРІРµС‚Р»РѕР№ РїРѕР»РѕСЃС‹ РїРѕРґ РІРєР»Р°РґРєР°РјРё.
    const QString extraTabPaneStyle = QStringLiteral(
        "QFrame#TabsContainer {"
        "  background-color: #252526;"
        "  border: none;"
        "}"
        "QTabBar {"
        "  border: none;"
        "  background: transparent;"
        "  qproperty-drawBase: 0;"
        "}"
        "QTabBar::pane {"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}"
        "QTabBar::tab {"
        "  background: #252526;"
        "  color: #cccccc;"
        "  padding: 3px 10px;"
        "  margin-right: 6px;"
        "  border: 1px solid #3c3c3c;"
        "  border-radius: 2px;"
        "}"
        "QTabBar::tab:selected {"
        "  background: #2f2f2f;"
        "  color: #ffffff;"
        "  border: 1px solid #4a4a4a;"
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background: #2a2a2a;"
        "}"
        "QTabBar::close-button, QTabBar::close-button:hover {"
        "  background: transparent;"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}");

    setStyleSheet(styleSheet() + extraTabPaneStyle);

    // Финальный VSCode?подобный стиль для вкладок,
    // который перекрывает предыдущие настройки.
    const QString vscodeTabsStyle = QStringLiteral(
        "QTabBar {"
        "  border: none;"
        "  background-color: #252526;"
        "  qproperty-drawBase: 0;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}"
        "QTabBar::tab {"
        "  background-color: #252526;"
        "  color: #cccccc;"
        "  padding: 0px 12px;"
        "  border: none;"
        "  border-radius: 0px;"
        "  margin-left: 0px;"
        "  margin-right: 0px;"
        "  height: 100%;"
        "}"
        "QTabBar::tab:selected {"
        "  background-color: #1e1e1e;"
        "  color: #ffffff;"
        "  border-top: 2px solid #007acc;"
        "  border-left: 1px solid #444444;"
        "  border-right: 1px solid #444444;"
        "  border-bottom: none;"
        "}"
        "QTabBar::tab:first:selected {"
        "  border-left: none;"
        "}"
        "QTabBar::tab:!selected:hover {"
        "  background-color: #2d2d2d;"
        "}"
        "QTabBar::close-button, QTabBar::close-button:hover {"
        "  background: transparent;"
        "  border: none;"
        "  margin: 0px;"
        "  padding: 0px;"
        "}");

    setStyleSheet(styleSheet() + vscodeTabsStyle);

    }

    QWidget *MainWindow::buildTopBar(QWidget *parent)
{
    auto *top = new QFrame(parent);
    top->setObjectName(QStringLiteral("TitleBar"));
    m_topBar = top;
    auto *mainTopLayout = new QHBoxLayout(top);
    mainTopLayout->setContentsMargins(0, 0, 0, 0);
    mainTopLayout->setSpacing(0);
    // Дополнительно уменьшаем высоту тайтлбара,
    // чтобы не было лишних отступов вокруг кнопок.
    top->setFixedHeight(32);

    top->setFixedHeight(36); // РЈРІРµР»РёС‡РёРІР°РµРј РІС‹СЃРѕС‚Сѓ С‚Р°Р№С‚Р»-Р±Р°СЂР°, С‡С‚РѕР±С‹ РЅРµ РѕР±СЂРµР·Р°Р»Р°СЃСЊ РїРѕР»РѕСЃРєР°
    // top->setStyleSheet("border-bottom: 1px solid #444444;"); /* Р”РѕР±Р°РІР»СЏРµРј РµРґРёРЅСѓСЋ РїРѕР»РѕСЃРєСѓ РїРѕРґ РІСЃРµРј С‚Р°Р№С‚Р»Р±Р°СЂРѕРј */

    // Р›РµРІР°СЏ СЃРµРєС†РёСЏ РґР»СЏ Р»РѕРіРѕС‚РёРїР° (РІС‹СЂРѕРІРЅРµРЅР° СЃ РЅР°РІР±Р°СЂРѕРј РЅРёР¶Рµ)
    auto *logoContainer = new QFrame(top);
    logoContainer->setObjectName(QStringLiteral("SideToolbar"));
    logoContainer->setFixedWidth(42); // РўР° Р¶Рµ С€РёСЂРёРЅР°, С‡С‚Рѕ Рё Сѓ SideToolbar
    auto *logoLayout = new QHBoxLayout(logoContainer);
    logoLayout->setContentsMargins(0, 0, 0, 0);
    logoLayout->setSpacing(0);
    logoLayout->setAlignment(Qt::AlignCenter); // Р¦РµРЅС‚СЂРёСЂСѓРµРј Р»РѕРіРѕС‚РёРї РІРµСЂС‚РёРєР°Р»СЊРЅРѕ Рё РіРѕСЂРёР·РѕРЅС‚Р°Р»СЊРЅРѕ

    auto *logoLabel = new QLabel(logoContainer);
    logoLabel->setFixedSize(28, 28);
    const QString logoPath = resolveAssetPath(QStringLiteral("logo/favicon-32x32.png"));
    if (!logoPath.isEmpty()) {
        QPixmap pix(logoPath);
        logoLabel->setPixmap(pix.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        logoLabel->setText(tr("Ghost"));
    }
    logoLayout->addWidget(logoLabel);
    mainTopLayout->addWidget(logoContainer);

    // РЎСЂРµРґРЅСЏСЏ СЃРµРєС†РёСЏ РґР»СЏ РІРєР»Р°РґРѕРє
    // РЎСЂРµРґРЅСЏСЏ СЃРµРєС†РёСЏ РґР»СЏ РІРєР»Р°РґРѕРє
    auto *tabsContainer = new QFrame(top);
    tabsContainer->setObjectName(QStringLiteral("TabsContainer")); // << РґРѕР±Р°РІРёР»Рё РёРјСЏ
    auto *tabsLayout = new QHBoxLayout(tabsContainer);
    tabsLayout->setContentsMargins(0, 0, 0, 0);
    tabsLayout->setSpacing(0);


    m_workspaceTabs = new QTabBar(tabsContainer);
    m_workspaceTabs->setExpanding(false);
    m_workspaceTabs->setTabsClosable(true);
    m_workspaceTabs->setMovable(true);
    m_workspaceTabs->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); // Р’РєР»Р°РґРєРё Р·Р°РЅРёРјР°СЋС‚ РІСЃРµ РґРѕСЃС‚СѓРїРЅРѕРµ РїСЂРѕСЃС‚СЂР°РЅСЃС‚РІРѕ
    connect(m_workspaceTabs, &QTabBar::currentChanged, this, &MainWindow::handleTabChanged);
    connect(m_workspaceTabs,
            &QTabBar::tabCloseRequested,
            this,
            &MainWindow::handleTabCloseRequested);
    tabsLayout->addWidget(m_workspaceTabs);

    m_addTabButton = new QToolButton(tabsContainer);
    m_addTabButton->setText("+");
    m_addTabButton->setAutoRaise(true);

    auto *menu = new QMenu(m_addTabButton);
    QAction *newTabAction = menu->addAction(tr("New workspace tab"));
    QAction *newLadderAction = menu->addAction(tr("Add ladder column"));
    connect(newTabAction, &QAction::triggered, this, &MainWindow::handleNewTabRequested);
    connect(newLadderAction, &QAction::triggered, this, &MainWindow::handleNewLadderRequested);
    m_addTabButton->setMenu(menu);
    m_addTabButton->setPopupMode(QToolButton::InstantPopup);
    tabsLayout->addWidget(m_addTabButton);
    mainTopLayout->addWidget(tabsContainer, 1); // Р’РєР»Р°РґРєРё Р·Р°РЅРёРјР°СЋС‚ РѕСЃС‚Р°РІС€РµРµСЃСЏ РїСЂРѕСЃС‚СЂР°РЅСЃС‚РІРѕ

    auto *right = new QHBoxLayout();
    // Р‘РµР· РіРѕСЂРёР·РѕРЅС‚Р°Р»СЊРЅС‹С… Р·Р°Р·РѕСЂРѕРІ РјРµР¶РґСѓ РєРЅРѕРїРєР°РјРё РѕРєРЅР°.
    right->setSpacing(0);

    m_connectionIndicator = new QLabel(tr("LIVE"), top);
    m_connectionIndicator->hide();

    auto *settingsButton = new QToolButton(top);
    settingsButton->setAutoRaise(true);
    settingsButton->setIcon(loadIcon(QStringLiteral("settings")));
    settingsButton->setIconSize(QSize(18, 18));
    connect(settingsButton, &QToolButton::clicked, this, &MainWindow::openSettingsWindow);
    settingsButton->hide();

    m_timeLabel = new QLabel(top);
    m_timeLabel->setCursor(Qt::PointingHandCursor);
    right->addWidget(m_timeLabel);

    auto makeWinButton = [top](const QString &text, const char *objectName) {
        auto *btn = new QToolButton(top);
        btn->setText(text);
        btn->setObjectName(QLatin1String(objectName));
        btn->setAutoRaise(true);
        btn->setFixedSize(42, 32);
        // Р§СѓС‚СЊ РІС‹С€Рµ, Р±Р»РёР¶Рµ Рє VSCode.
        btn->setFixedSize(42, 28);
        return btn;
    };

    // Helper to fix window geometry if it ends up off-screen or clipped.
    auto fixWindowGeometry = [this]() {
        QRect geom = geometry();
        QScreen *scr = QGuiApplication::screenAt(geom.center());
        if (!scr) scr = QGuiApplication::primaryScreen();
        if (!scr) return;
        const QRect work = scr->availableGeometry();

        const QRect inter = work.intersected(geom);
        // If intersection is too small or window is outside work area, recenter/resize
        if (inter.width() < geom.width() / 2 || inter.height() < geom.height() / 2 || !work.contains(geom)) {
            int w = std::min(geom.width(), work.width());
            int h = std::min(geom.height(), work.height());
            int x = work.x() + (work.width() - w) / 2;
            int y = work.y() + (work.height() - h) / 2;
            qDebug() << "[MainWindow] correcting geometry to" << QRect(x, y, w, h) << " work=" << work << " geom=" << geom;
            setGeometry(x, y, w, h);
            raise();
            activateWindow();
        }
    };

    auto *minButton = makeWinButton(QString(), "WindowButtonMin");
    auto *maxButton = makeWinButton(QString(), "WindowButtonMax");
    auto *closeButton = makeWinButton(QString(), "WindowButtonClose");

    const QSize winIconSize(16, 16);
    const QSize maxIconSize(14, 14);
    const QColor winIconColor("#ffffff");
    minButton->setIcon(loadIconTinted(QStringLiteral("minus"), winIconColor, winIconSize));
    minButton->setIconSize(winIconSize);
    maxButton->setIcon(loadIconTinted(QStringLiteral("square"), winIconColor, maxIconSize));
    maxButton->setIconSize(maxIconSize);
    closeButton->setIcon(loadIconTinted(QStringLiteral("x"), winIconColor, winIconSize));
    closeButton->setIconSize(winIconSize);

    // store pointers to buttons so we can update icons on state changes
    m_minButton = minButton;
    m_maxButton = maxButton;
    m_closeButton = closeButton;

    QObject::connect(minButton, &QToolButton::clicked, this, [this]() {
        qDebug() << "[MainWindow] Min button clicked. windowState=" << windowState();
        logToFile(QStringLiteral("Min button clicked. state=%1 geometry=%2").arg(QString::number((int)windowState())).arg(rectToString(geometry())));
        showMinimized();
        // schedule a delayed check when window is restored later
        QTimer::singleShot(200, this, [this]() {
            // If we have a saved normal geometry, ensure it's valid after restore
            if (m_haveLastNormalGeometry && !isMaximized()) {
                setGeometry(m_lastNormalGeometry);
                ensureNativeBounds(this, m_lastNormalGeometry);
                logToFile(QStringLiteral("Applied saved normal geometry after minimize-restore: %1").arg(rectToString(m_lastNormalGeometry)));
            } else if (!isMaximized()) {
                // fallback correction
                QRect geom = geometry();
                QScreen *scr = QGuiApplication::screenAt(geom.center());
                if (!scr) scr = QGuiApplication::primaryScreen();
                if (scr) {
                    const QRect work = scr->availableGeometry();
                    const QRect inter = work.intersected(geom);
                    if (inter.width() < geom.width() / 2 || inter.height() < geom.height() / 2 || !work.contains(geom)) {
                        int w = std::min(geom.width(), work.width());
                        int h = std::min(geom.height(), work.height());
                        int x = work.x() + (work.width() - w) / 2;
                        int y = work.y() + (work.height() - h) / 2;
                        QRect desired(x, y, w, h);
                        setGeometry(desired);
                        ensureNativeBounds(this, desired);
                        logToFile(QStringLiteral("Fallback geometry correction after minimize-restore: %1").arg(rectToString(desired)));
                        raise();
                        activateWindow();
                    }
                }
            }
        });
    });

    QObject::connect(maxButton, &QToolButton::clicked, this, [this]() {
        const bool maximized = isMaximized();
        qDebug() << "[MainWindow] Max button clicked. before isMaximized=" << maximized
                 << " geometry=" << geometry() << " windowState=" << windowState();
        {
            const QString s = QStringLiteral("[MainWindow] Max clicked before: isMaximized=") + (maximized ? QStringLiteral("1") : QStringLiteral("0"));
            const std::wstring ws = s.toStdWString();
            OutputDebugStringW(ws.c_str());
        }

        if (!maximized) {
            // About to maximize: save current normal geometry so we can restore it later
            m_lastNormalGeometry = geometry();
            m_haveLastNormalGeometry = true;
                logToFile(QStringLiteral("Saving normal geometry before maximize: %1").arg(rectToString(m_lastNormalGeometry)));
            showMaximized();
        } else {
            // Restore from maximized: use saved normal geometry if available
            // To avoid native geometry/desync issues with frameless windows,
            // temporarily disable the frameless flag so the system applies
            // normal window decoration and bounds, then re-enable our
            // frameless UI and apply the saved geometry.
            if (m_haveLastNormalGeometry) {
                // Hide heavy content to mask visual jumps during restore
                QWidget *central = centralWidget();
                if (central) central->setVisible(false);

                logToFile(QStringLiteral("Restoring from maximized: temporarily disabling FramelessWindowHint"));
                // disable frameless so Windows will restore native bounds
                setWindowFlag(Qt::FramelessWindowHint, false);
                showNormal();
                // allow the windowing system to settle, then reapply our geometry
                QTimer::singleShot(180, this, [this, central]() {
                    setGeometry(m_lastNormalGeometry);
                    ensureNativeBounds(this, m_lastNormalGeometry);
                    // re-enable frameless and show again to apply our custom titlebar
                    setWindowFlag(Qt::FramelessWindowHint, true);
                    // Calling show() will update flags; ensure the window is active
                    show();
                    // Reapply native snap style bits in case they were changed
#ifdef Q_OS_WIN
                    applyNativeSnapStyleForHwnd(reinterpret_cast<HWND>(winId()));
#endif
                    raise();
                    activateWindow();
                    logToFile(QStringLiteral("Restored saved normal geometry after un-maximize (frameless-toggle): %1").arg(rectToString(m_lastNormalGeometry)));

                    // Reveal content with a quick fade to mask remaining jumps
                    if (central) {
                        animateWindowFadeIn(this);
                        central->setVisible(true);
                    } else {
                        animateWindowFadeIn(this);
                    }
                });
            } else {
                // fallback small correction after restore
                QTimer::singleShot(50, this, [this]() {
                    QRect geom = geometry();
                    QScreen *scr = QGuiApplication::screenAt(geom.center());
                    if (!scr) scr = QGuiApplication::primaryScreen();
                    if (!scr) return;
                    const QRect work = scr->availableGeometry();
                    const QRect inter = work.intersected(geom);
                    if (inter.width() < geom.width() / 2 || inter.height() < geom.height() / 2 || !work.contains(geom)) {
                        int w = std::min(geom.width(), work.width());
                        int h = std::min(geom.height(), work.height());
                        int x = work.x() + (work.width() - w) / 2;
                        int y = work.y() + (work.height() - h) / 2;
                        QRect correctedDesired(x, y, w, h); // РћР±СЉСЏРІР»СЏРµРј desired Р·РґРµСЃСЊ
                        setGeometry(correctedDesired);
                        ensureNativeBounds(this, correctedDesired);
                        logToFile(QStringLiteral("Fallback geometry correction after minimize-restore: %1").arg(rectToString(correctedDesired)));
                        raise();
                        activateWindow();
                    }
                });
            }
        }

        updateMaximizeIcon();
        QTimer::singleShot(100, this, [this]() {
            qDebug() << "[MainWindow] geometry after action:" << geometry() << " state=" << windowState();
            logToFile(QStringLiteral("Post-max action geometry: %1 state=%2").arg(rectToString(geometry())).arg((int)windowState()));
        });
    });
    QObject::connect(closeButton, &QToolButton::clicked, this, &QWidget::close);

    // set initial maximize/restore icon
    updateMaximizeIcon();

    right->addWidget(minButton);
    right->addWidget(maxButton);
    right->addWidget(closeButton);

    mainTopLayout->addLayout(right);

    // Caption area for dragging/maximizing.
    top->installEventFilter(this);
    m_workspaceTabs->installEventFilter(this);
    m_addTabButton->installEventFilter(this);
    m_connectionIndicator->installEventFilter(this);
    m_timeLabel->installEventFilter(this);

    return top;
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event && event->type() == QEvent::WindowStateChange) {
        // Handle transitions between Minimized / Maximized / Normal reliably.
        const Qt::WindowStates cur = windowState();
        // If we just became maximized (possibly via system/Aero Snap), save
        // the normal geometry reported by the window system so we can restore
        // it later.
        if (cur.testFlag(Qt::WindowMaximized)) {
            // QWidget::normalGeometry() is available on the widget and is
            // preferred here (some Qt versions don't expose normalGeometry()
            // on QWindow).
            const QRect normal = normalGeometry();
            if (normal.isValid()) {
                m_lastNormalGeometry = normal;
                m_haveLastNormalGeometry = true;
                qDebug() << "[MainWindow] saved normalGeometry from widget() :" << normal;
                logToFile(QStringLiteral("changeEvent: saved normalGeometry: %1").arg(rectToString(normal)));
            }
        }

        // If we transitioned from Minimized -> Normal, restore previous
        // maximized/normal behaviour depending on what we had before.
        if (m_prevWindowState.testFlag(Qt::WindowMinimized) && !cur.testFlag(Qt::WindowMinimized)) {
            // We are restoring from minimize.
            if (m_prevWindowState.testFlag(Qt::WindowMaximized)) {
                // If previously maximized, restore maximized but hide content briefly
                QWidget *central = centralWidget();
                if (central) central->setVisible(false);
                QTimer::singleShot(0, this, [this, central]() {
                    showMaximized();
                    QTimer::singleShot(160, this, [this, central]() {
                        if (central) {
                            animateWindowFadeIn(this);
                            central->setVisible(true);
                        } else {
                            animateWindowFadeIn(this);
                        }
                    });
                });
            } else if (m_haveLastNormalGeometry) {
                // If previously normal, restore saved normal geometry with content hidden
                QWidget *central = centralWidget();
                if (central) central->setVisible(false);
                QTimer::singleShot(120, this, [this, central]() {
                    setGeometry(m_lastNormalGeometry);
                    ensureNativeBounds(this, m_lastNormalGeometry);
                    logToFile(QStringLiteral("changeEvent: restored saved geometry after un-minimize: %1").arg(rectToString(m_lastNormalGeometry)));
                    raise();
                    activateWindow();
                    if (central) {
                        animateWindowFadeIn(this);
                        central->setVisible(true);
                    } else {
                        animateWindowFadeIn(this);
                    }
                });
            }
        }

        updateMaximizeIcon();
        m_prevWindowState = cur;
    }
    QMainWindow::changeEvent(event);
}

void MainWindow::updateMaximizeIcon()
{
    if (!m_maxButton) return;
    const bool maximized = windowState().testFlag(Qt::WindowMaximized);
    const QSize maxIconSize(14, 14);
    if (maximized) {
        m_maxButton->setIcon(loadIconTinted(QStringLiteral("squares"), QColor("#ffffff"), maxIconSize));
    } else {
        m_maxButton->setIcon(loadIconTinted(QStringLiteral("square"), QColor("#ffffff"), maxIconSize));
    }
}

#ifdef Q_OS_WIN
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(eventType);
    MSG *msg = static_cast<MSG *>(message);
    if (!msg) return QMainWindow::nativeEvent(eventType, message, result);

    if (msg->message == WM_NCCALCSIZE) {
        // Remove standard title bar and non-client area вЂ” we draw our own TitleBar.
        // For maximized/snapped windows Windows may leave space for the standard
        // non-client frame. When that happens we must explicitly set the client
        // rect to the monitor work area so there are no stray gaps on the right/bottom
        // (classic frameless-window vs Aero Snap issue).
        if (msg->wParam == TRUE && msg->lParam) {
            NCCALCSIZE_PARAMS* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
            RECT &r = params->rgrc[0];

            HWND hwnd = msg->hwnd;
            if (hwnd && IsZoomed(hwnd)) {
                // Window is maximized (this also covers Aero Snap to top):
                // align client rect to monitor work area (respects taskbar).
                HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMonitor, &mi)) {
                    // Use rcWork to avoid covering the taskbar
                    r = mi.rcWork;
                    logToFile(QStringLiteral("WM_NCCALCSIZE applied rcWork: %1").arg(rectToString(QRect(r.left, r.top, r.right - r.left, r.bottom - r.top))));
                }
            }

            *result = 0;
            return true;
        }
        *result = 0;
        return true;
    }

    if (msg->message == WM_NCHITTEST) {
        // When window is maximized (or snapped to top) don't expose resize
        // borders вЂ” Windows may still attempt to treat edges as non-client,
        // which leads to spare margins being left inside the window.
        HWND hwnd = msg->hwnd;
        if (hwnd && IsZoomed(hwnd)) {
            *result = HTCLIENT;
            return true;
        }

        const LONG x = GET_X_LPARAM(msg->lParam);
        const LONG y = GET_Y_LPARAM(msg->lParam);
        const QPoint globalPt(x, y);

        const QRect g = frameGeometry();
        const QPoint localPt = mapFromGlobal(globalPt);

        const int w = g.width();
        const int h = g.height();
        const int bw = m_resizeBorderWidth;

        // Corners first
        if (localPt.x() < bw && localPt.y() < bw) { *result = HTTOPLEFT; return true; }
        if (localPt.x() >= w - bw && localPt.y() < bw) { *result = HTTOPRIGHT; return true; }
        if (localPt.x() < bw && localPt.y() >= h - bw) { *result = HTBOTTOMLEFT; return true; }
        if (localPt.x() >= w - bw && localPt.y() >= h - bw) { *result = HTBOTTOMRIGHT; return true; }

        // Edges
        if (localPt.x() < bw) { *result = HTLEFT; return true; }
        if (localPt.x() >= w - bw) { *result = HTRIGHT; return true; }
        if (localPt.y() < bw) { *result = HTTOP; return true; }
        if (localPt.y() >= h - bw) { *result = HTBOTTOM; return true; }

        // Titlebar (allow dragging) вЂ” but don't intercept clicks over interactive widgets
        if (m_topBar) {
            const QRect tRect = m_topBar->rect();
            const QPoint tTopLeft = m_topBar->mapToGlobal(QPoint(0, 0));
            QRect tGlobal = tRect.translated(tTopLeft);
            if (tGlobal.contains(globalPt)) {
                // If it's over a child (button) вЂ” don't treat as caption
                QPoint localInTop = m_topBar->mapFromGlobal(globalPt);
                QWidget *child = m_topBar->childAt(localInTop); // РћР±СЉСЏРІР»СЏРµРј child Р·РґРµСЃСЊ
                // Р•СЃР»Рё РєР»РёРє РЅРµ РЅР° РєРЅРѕРїРєР°С… РѕРєРЅР°, С‚Рѕ СЌС‚Рѕ РѕР±Р»Р°СЃС‚СЊ РґР»СЏ РїРµСЂРµС‚Р°СЃРєРёРІР°РЅРёСЏ
                if (child && child != m_minButton && child != m_maxButton && child != m_closeButton) {
                    *result = HTCAPTION;
                    return true;
                }
                *result = HTCLIENT;
                return true;
            }
        }

        *result = HTCLIENT;
        return true;
    }

    if (msg->message == WM_GETMINMAXINFO) {
        // Ensure maximized size/position match the monitor work area so
        // Windows maximization (and Aero Snap) doesn't leave gaps or shift
        // the window. Classic fix for frameless windows.
        if (msg->lParam) {
            MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
            HMONITOR hMonitor = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMonitor, &mi)) {
                const RECT &rcWork = mi.rcWork;
                const RECT &rcMonitor = mi.rcMonitor;

                mmi->ptMaxPosition.x = rcWork.left - rcMonitor.left;
                mmi->ptMaxPosition.y = rcWork.top - rcMonitor.top;
                mmi->ptMaxSize.x = rcWork.right - rcWork.left;
                mmi->ptMaxSize.y = rcWork.bottom - rcWork.top;
                mmi->ptMaxTrackSize = mmi->ptMaxSize;
                logToFile(QStringLiteral("WM_GETMINMAXINFO set ptMaxSize=%1 pos=%2").arg(QString::number(mmi->ptMaxSize.x) + 'x' + QString::number(mmi->ptMaxSize.y), QString::number(mmi->ptMaxPosition.x) + "," + QString::number(mmi->ptMaxPosition.y)));
            }
            *result = 0;
            return true;
        }
    }

    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif

QWidget *MainWindow::buildMainArea(QWidget *parent)
{
    auto *main = new QFrame(parent);
    main->setObjectName(QStringLiteral("MainAreaFrame"));
    auto *layout = new QHBoxLayout(main);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *sidebar = new QFrame(main);
    sidebar->setObjectName(QStringLiteral("SideToolbar"));
    // С‡СѓС‚СЊ С€РёСЂРµ, РєР°Рє РІ VSCode (СѓРјРµРЅСЊС€РµРЅРѕ РЅР° 2px РїРѕ РїСЂРѕСЃСЊР±Рµ)
        sidebar->setFixedWidth(42);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 8, 0, 8);
    sideLayout->setSpacing(12);

    const QSize navIconSize(28, 28);
    const QColor navIconColor("#c0c0c0");

    auto makeSideButton = [this, sidebar, navIconSize, navIconColor](const QString &iconName,
                                                                     const QString &tooltip) {
        auto *btn = new QToolButton(sidebar);
        btn->setObjectName(QStringLiteral("SideNavButton"));
        btn->setAutoRaise(true);
        // Default icon: light gray
        btn->setIcon(loadIconTinted(iconName, navIconColor, navIconSize));
        btn->setIconSize(navIconSize);
        btn->setToolTip(tooltip);
        // Make cursor indicate clickable and allow handling hover in eventFilter
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("iconName", iconName);
        btn->installEventFilter(this);
        return btn;
    };

    // РџРѕРґРєР»СЋС‡РµРЅРёРµ / СЃС‚Р°С‚СѓСЃ
    {
        QToolButton *b = makeSideButton(QStringLiteral("plug-connected"), tr("Connection"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
    }

    // Р¤РёРЅР°РЅСЃРѕРІС‹Р№ СЂРµР·СѓР»СЊС‚Р°С‚ / PnL
    {
        QToolButton *b = makeSideButton(QStringLiteral("report-money"), tr("P&L / Results"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
    }

    // РЎРґРµР»РєРё / РѕСЂРґРµСЂР°
    {
        QToolButton *b = makeSideButton(QStringLiteral("arrows-exchange"), tr("Trades"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
    }

    // РњРѕРґС‹ (РёРєРѕРЅРєР° cube-plus)
    auto *modsButton = makeSideButton(QStringLiteral("cube-plus"), tr("Mods"));
    sideLayout->addWidget(modsButton, 0, Qt::AlignHCenter);
    connect(modsButton, &QToolButton::clicked, this, &MainWindow::openPluginsWindow);

    // РђР»РµСЂС‚С‹
    {
        QToolButton *b = makeSideButton(QStringLiteral("bell"), tr("Alerts"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
    }

    // РўР°Р№РјРµСЂ / СЂР°СЃРїРёСЃР°РЅРёРµ
    {
        QToolButton *b = makeSideButton(QStringLiteral("alarm"), tr("Timer"));
        sideLayout->addWidget(b, 0, Qt::AlignHCenter);
    }

    sideLayout->addStretch(1);

    // РќР°СЃС‚СЂРѕР№РєРё РІ СЃР°РјРѕРј РЅРёР·Сѓ (С„РёРєСЃРёСЂСѓРµРј РІРЅРёР·Сѓ)
    auto *settingsNav = makeSideButton(QStringLiteral("settings"), tr("Settings"));
    sideLayout->addWidget(settingsNav, 0, Qt::AlignHCenter | Qt::AlignBottom);
    connect(settingsNav, &QToolButton::clicked, this, &MainWindow::openSettingsWindow);

    layout->addWidget(sidebar);

    m_workspaceStack = new QStackedWidget(main);
    layout->addWidget(m_workspaceStack, 1);

    return main;
}

void MainWindow::createInitialWorkspace()
{
    createWorkspaceTab();
}

void MainWindow::createWorkspaceTab()
{
    const int tabId = m_nextTabId++;

    auto *workspace = new QFrame(m_workspaceStack);
    auto *wsLayout = new QVBoxLayout(workspace);
    wsLayout->setContentsMargins(12, 8, 12, 8);
    wsLayout->setSpacing(8);

    auto *columnsSplitter = new QSplitter(Qt::Horizontal, workspace);
    columnsSplitter->setChildrenCollapsible(false);

    WorkspaceTab tab;
    tab.id = tabId;
    tab.workspace = workspace;
    tab.columns = columnsSplitter;

    for (const QString &sym : m_symbols) {
        DomColumn col = createDomColumn(sym, tab);
        tab.columnsData.push_back(col);
        columnsSplitter->addWidget(col.container);
    }

    wsLayout->addWidget(columnsSplitter, 1);

    const int stackIndex = m_workspaceStack->addWidget(workspace);

    const int tabIndex = m_workspaceTabs->addTab(QStringLiteral("Вкладка %1").arg(tabId));
    m_workspaceTabs->setTabData(tabIndex, tabId);
    m_workspaceTabs->setTabText(tabIndex, QStringLiteral("Вкладка %1").arg(tabId));

    m_tabs.push_back(tab);

    m_workspaceTabs->setCurrentIndex(tabIndex);
    m_workspaceStack->setCurrentIndex(stackIndex);
}

MainWindow::DomColumn MainWindow::createDomColumn(const QString &symbol, WorkspaceTab &tab)
{
    DomColumn result;
    result.symbol = symbol.toUpper();

    auto *column = new QFrame(tab.workspace);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    auto *header = new QFrame(column);
    auto *hLayout = new QHBoxLayout(header);
    hLayout->setContentsMargins(6, 2, 6, 2);
    hLayout->setSpacing(6);

    auto *tickerLabel = new QLabel(result.symbol, header);
    hLayout->addWidget(tickerLabel);
    hLayout->addStretch(1);

    layout->addWidget(header);

    auto *statusLabel = new QLabel(column);
    statusLabel->setText(tr("Starting backend..."));
    layout->addWidget(statusLabel);

    auto *dom = new DomWidget(column);
    layout->addWidget(dom, 1);

    auto *client = new LadderClient(m_backendPath, result.symbol, m_levels, dom, column);

    connect(client,
            &LadderClient::statusMessage,
            this,
            &MainWindow::handleLadderStatusMessage);
    connect(client, &LadderClient::pingUpdated, this, &MainWindow::handleLadderPingUpdated);

    connect(dom,
            &DomWidget::rowClicked,
            this,
            &MainWindow::handleDomRowClicked);

    connect(client, &LadderClient::statusMessage, statusLabel, &QLabel::setText);

    result.container = column;
    result.dom = dom;
    result.client = client;
    return result;
}

MainWindow::WorkspaceTab *MainWindow::currentWorkspaceTab()
{
    if (!m_workspaceTabs) {
        return nullptr;
    }
    const int index = m_workspaceTabs->currentIndex();
    if (index < 0) {
        return nullptr;
    }

    const QVariant data = m_workspaceTabs->tabData(index);
    const int id = data.isValid() ? data.toInt() : 0;
    for (auto &tab : m_tabs) {
        if (tab.id == id) {
            return &tab;
        }
    }
    return nullptr;
}

int MainWindow::findTabIndexById(int id) const
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs[i].id == id) {
            return i;
        }
    }
    return -1;
}

void MainWindow::handleTabChanged(int index)
{
    if (!m_workspaceTabs || !m_workspaceStack) {
        return;
    }
    if (index < 0) {
        return;
    }
    const QVariant data = m_workspaceTabs->tabData(index);
    const int id = data.isValid() ? data.toInt() : 0;
    const int stackIndex = findTabIndexById(id);
    if (stackIndex >= 0) {
        m_workspaceStack->setCurrentIndex(stackIndex);
    }
}

void MainWindow::handleTabCloseRequested(int index)
{
    if (!m_workspaceTabs || !m_workspaceStack) {
        return;
    }
    if (m_workspaceTabs->count() <= 1 || index < 0) {
        return;
    }

    const QVariant data = m_workspaceTabs->tabData(index);
    const int id = data.isValid() ? data.toInt() : 0;
    const int tabIdx = findTabIndexById(id);
    if (tabIdx < 0) {
        return;
    }

    WorkspaceTab tab = m_tabs.takeAt(tabIdx);

    QWidget *wsWidget = tab.workspace;
    const int stackIndex = m_workspaceStack->indexOf(wsWidget);
    if (stackIndex >= 0) {
        QWidget *widget = m_workspaceStack->widget(stackIndex);
        m_workspaceStack->removeWidget(widget);
        widget->deleteLater();
    }

    m_workspaceTabs->removeTab(index);

    if (m_workspaceTabs->count() > 0) {
        const int newIndex = std::min(index, m_workspaceTabs->count() - 1);
        m_workspaceTabs->setCurrentIndex(newIndex);
    }
}

void MainWindow::handleNewTabRequested()
{
    createWorkspaceTab();
}

void MainWindow::handleNewLadderRequested()
{
    WorkspaceTab *tab = currentWorkspaceTab();
    if (!tab || !tab->columns) {
        return;
    }

    const QString defaultSymbol = !m_symbols.isEmpty() ? m_symbols.first() : QStringLiteral("BIOUSDT");

    bool ok = false;
    const QString symbol =
        QInputDialog::getText(this, tr("Add ladder"), tr("Symbol:"), QLineEdit::Normal, defaultSymbol, &ok)
            .trimmed()
            .toUpper();

    if (!ok || symbol.isEmpty()) {
        return;
    }

    DomColumn col = createDomColumn(symbol, *tab);
    tab->columnsData.push_back(col);
    tab->columns->addWidget(col.container);
}

void MainWindow::handleLadderStatusMessage(const QString &msg)
{
    Q_UNUSED(msg);
}

void MainWindow::handleLadderPingUpdated(int ms)
{
    Q_UNUSED(ms);
}

void MainWindow::handleDomRowClicked(int row, double price, double bidQty, double askQty)
{
    Q_UNUSED(row);
    const QString side = (askQty > 0.0 || bidQty > 0.0)
                             ? (askQty >= bidQty ? QStringLiteral("ask") : QStringLiteral("bid"))
                             : QStringLiteral("none");

    if (m_orderPriceEdit) {
        m_orderPriceEdit->setText(QString::number(price, 'f', 5));
    }
    if (m_orderQuantityEdit) {
        m_orderQuantityEdit->setText(QStringLiteral(""));
    }

    if (m_orderSymbolLabel) {
        m_orderSymbolLabel->setText(side);
    }

    statusBar()->showMessage(
        tr("Row clicked: price %1, bid %2, ask %3, side %4")
            .arg(price)
            .arg(bidQty)
            .arg(askQty)
            .arg(side),
        3000);
}

void MainWindow::updateTimeLabel()
{
    if (!m_timeLabel) {
        return;
    }
    QDateTime now = QDateTime::currentDateTimeUtc();
    if (m_timeOffsetMinutes != 0) {
        now = now.addSecs(m_timeOffsetMinutes * 60);
    }

    const int hoursOffset = m_timeOffsetMinutes / 60;
    QString suffix;
    if (hoursOffset == 0) {
        suffix = QStringLiteral("UTC");
    } else if (hoursOffset > 0) {
        suffix = QStringLiteral("Вкладка %1").arg(hoursOffset);
    } else {
        suffix = QStringLiteral("Вкладка %1").arg(hoursOffset); // hoursOffset already negative
    }

    m_timeLabel->setText(now.toString(QStringLiteral("HH:mm:ss '") + suffix + QLatin1Char('\'')));
}

QIcon MainWindow::loadIcon(const QString &name) const
{
    // РС‰РµРј РїСЂРѕСЃС‚Рѕ "<name>.svg" РІ РЅРµСЃРєРѕР»СЊРєРёС… Р±Р°Р·РѕРІС‹С… РјРµСЃС‚Р°С… (appDir, img/, img/icons/, img/icons/outline/).
    const QString relFile = QStringLiteral("%1.svg").arg(name);
    const QString path = resolveAssetPath(relFile);
    if (!path.isEmpty()) {
        return QIcon(path);
    }
    return QIcon();
}

QIcon MainWindow::loadIconTinted(const QString &name, const QColor &color, const QSize &size) const
{
    const QIcon base = loadIcon(name);
    if (base.isNull()) {
        return base;
    }

    const QSize iconSize = size.isValid() ? size : QSize(16, 16);
    QPixmap src = base.pixmap(iconSize);
    if (src.isNull()) {
        return base;
    }

    QPixmap tinted(src.size());
    tinted.fill(Qt::transparent);

    QPainter p(&tinted);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(tinted.rect(), color);
    p.end();

    QIcon result;
    result.addPixmap(tinted);
    return result;
}

QString MainWindow::resolveAssetPath(const QString &relative) const
{
    const QString rel = QDir::fromNativeSeparators(relative);
    const QString appDir = QCoreApplication::applicationDirPath();

    // РС‰РµРј С‚РѕР»СЊРєРѕ РІ РґРёСЂРµРєС‚РѕСЂРёРё РїСЂРёР»РѕР¶РµРЅРёСЏ Рё РµРµ РїРѕРґРґРёСЂРµРєС‚РѕСЂРёСЏС…
    const QStringList bases = {
        appDir,
        QDir(appDir).filePath(QStringLiteral("assets")),
        QDir(appDir).filePath(QStringLiteral("assets/img")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("assets/img/outline")),
        QDir(appDir).filePath(QStringLiteral("../assets")),
        QDir(appDir).filePath(QStringLiteral("../assets/img")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/outline")),
        QDir(appDir).filePath(QStringLiteral("../../assets")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/outline"))
    };

    for (const QString &base : bases) {
        const QString candidate = QDir(base).filePath(rel);
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    const QString resPath = QStringLiteral(":/") + rel;
    if (QFile::exists(resPath)) {
        return resPath;
    }

    return QString();
}

void MainWindow::openPluginsWindow()
{
    if (!m_pluginsWindow) {
        m_pluginsWindow = new PluginsWindow(this);
    }
    m_pluginsWindow->show();
    m_pluginsWindow->raise();
    m_pluginsWindow->activateWindow();
}

void MainWindow::openSettingsWindow()
{
    if (!m_settingsWindow) {
        m_settingsWindow = new SettingsWindow(this);
    }
    m_settingsWindow->show();
    m_settingsWindow->raise();
    m_settingsWindow->activateWindow();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (!m_topBar) {
        return QMainWindow::eventFilter(obj, event);
    }

    // Quick hover handling for side nav buttons: РјРіРЅРѕРІРµРЅРЅРѕ РїРµСЂРµРєР»СЋС‡Р°РµРј РёРєРѕРЅРєСѓ
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        auto *btn = qobject_cast<QToolButton *>(obj);
        if (btn && btn->objectName() == QLatin1String("SideNavButton")) {
            const QString iconName = btn->property("iconName").toString();
            if (!iconName.isEmpty()) {
                const QSize iconSize(28, 28);
                if (event->type() == QEvent::Enter) {
                    btn->setIcon(loadIconTinted(iconName, QColor("#ffffff"), iconSize));
                } else {
                    btn->setIcon(loadIconTinted(iconName, QColor("#c0c0c0"), iconSize));
                }
            }
            // Let other handlers (like tooltips) proceed.
            return QMainWindow::eventFilter(obj, event);
        }
    }

    const bool isTopBarObject =
        obj == m_topBar || obj == m_workspaceTabs || obj == m_addTabButton
        || obj == m_timeLabel;

    if (!isTopBarObject) {
        return QMainWindow::eventFilter(obj, event);
    }

    // Click on time label opens offset menu instead of dragging.
    if (obj == m_timeLabel && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            QMenu menu;
            struct OffsetEntry {
                QString label;
                int minutes;
            };
            const QList<OffsetEntry> entries = {
                {QStringLiteral("UTC-2"), -120},
                {QStringLiteral("UTC-1"), -60},
                {QStringLiteral("UTC"), 0},
                {QStringLiteral("UTC+1"), 60},
                {QStringLiteral("UTC+2"), 120},
            };
            QAction *currentAction = nullptr;
            for (const auto &entry : entries) {
                QAction *act = menu.addAction(entry.label);
                act->setData(entry.minutes);
                if (entry.minutes == m_timeOffsetMinutes) {
                    act->setCheckable(true);
                    act->setChecked(true);
                    currentAction = act;
                }
            }
            if (!currentAction && m_timeOffsetMinutes == 0 && !menu.actions().isEmpty()) {
                menu.actions().first()->setCheckable(true);
                menu.actions().first()->setChecked(true);
            }

            const QPoint globalPos = m_timeLabel->mapToGlobal(
                QPoint(m_timeLabel->width() / 2, m_timeLabel->height()));
            QAction *chosen = menu.exec(globalPos);
            if (chosen) {
                m_timeOffsetMinutes = chosen->data().toInt();
                updateTimeLabel();
            }
            return true;
        }
    }

    // Double-click on title bar: ignored (keep single-click system behavior)

    if (event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            // Only start native system move when the click is on an empty
            // area of the title bar (not on interactive child widgets such
            // as minimize/maximize/close buttons, tabs, etc.). This prevents
            // starting a move operation when the user actually clicks a button
            // which can lead to stuck/invalid move state after minimize.
            if (m_topBar) {
                const QPoint globalPt = me->globalPos();
                const QPoint localInTop = m_topBar->mapFromGlobal(globalPt);
                if (m_topBar->rect().contains(localInTop)) {
                    if (QWidget *child = m_topBar->childAt(localInTop)) {
                        // Click is over an interactive child вЂ” let the child handle it.
                        return QMainWindow::eventFilter(obj, event);
                    }
                }
            }

            if (QWindow *w = windowHandle()) {
                w->startSystemMove();
                return false;
            }
        }
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
#ifdef Q_OS_WIN
    // Enable native snapping/resizing, but keep frameless (no system caption).
    if (!m_nativeSnapEnabled) {
        WId id = winId();
        HWND hwnd = reinterpret_cast<HWND>(id);
        if (hwnd) {
            LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
            // Keep WS_CAPTION off so system doesn't draw its title bar; enable thick frame
            // so native snapping and resize work.
            style &= ~WS_CAPTION;
            style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            SetWindowLongPtr(hwnd, GWL_STYLE, style);
            SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        m_nativeSnapEnabled = true;
    }
#endif
}








