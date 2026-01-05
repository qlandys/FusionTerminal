#include "MainWindow.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <QSurfaceFormat>

#ifdef Q_OS_WIN
#include <ShObjIdl.h>
#include <Windows.h>
#include <ShlObj.h>
#endif

static QString resolveAssetPath(const QString &relative)
{
    const QString rel = QDir::fromNativeSeparators(relative);
    const QString appDir = QCoreApplication::applicationDirPath();
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

#ifdef Q_OS_WIN
static void refreshPinnedTaskbarShortcutIcons(const QString &exePath)
{
    const QString appData = qEnvironmentVariable("APPDATA");
    if (appData.isEmpty()) {
        return;
    }

    const QDir pinnedDir(QDir(appData).filePath(
        QStringLiteral("Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar")));
    if (!pinnedDir.exists()) {
        return;
    }

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (initHr == RPC_E_CHANGED_MODE) {
        return;
    }

    const QString exeNative = QDir::toNativeSeparators(QFileInfo(exePath).absoluteFilePath());

    const QStringList links = pinnedDir.entryList(QStringList() << QStringLiteral("*.lnk"), QDir::Files);
    for (const QString &lnkFile : links) {
        const QString lnkPath = QDir::toNativeSeparators(pinnedDir.filePath(lnkFile));

        IShellLinkW *shellLink = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink,
                                      nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW,
                                      reinterpret_cast<void **>(&shellLink));
        if (FAILED(hr) || !shellLink) {
            continue;
        }

        IPersistFile *persist = nullptr;
        hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&persist));
        if (FAILED(hr) || !persist) {
            shellLink->Release();
            continue;
        }

        const std::wstring lnkW = lnkPath.toStdWString();
        hr = persist->Load(lnkW.c_str(), STGM_READWRITE);
        if (FAILED(hr)) {
            persist->Release();
            shellLink->Release();
            continue;
        }

        wchar_t targetW[MAX_PATH] = {0};
        hr = shellLink->GetPath(targetW, MAX_PATH, nullptr, SLGP_RAWPATH);
        if (SUCCEEDED(hr) && targetW[0] != L'\0') {
            const QString targetNative = QDir::toNativeSeparators(QString::fromWCharArray(targetW));
            if (QString::compare(targetNative, exeNative, Qt::CaseInsensitive) == 0) {
                const std::wstring exeW = exeNative.toStdWString();
                shellLink->SetIconLocation(exeW.c_str(), 0);
                persist->Save(lnkW.c_str(), TRUE);
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, lnkW.c_str(), nullptr);
            }
        }

        persist->Release();
        shellLink->Release();
    }

    if (SUCCEEDED(initHr)) {
        CoUninitialize();
    }
}

static void maybeRefreshShellIconCache()
{
    const QFileInfo exeInfo(QCoreApplication::applicationFilePath());
    const QString stamp = exeInfo.lastModified().toString(Qt::ISODateWithMs);

    QSettings settings;
    const QString key = QStringLiteral("shell/iconRefreshStamp");
    const QString last = settings.value(key).toString();
    if (last == stamp) {
        return;
    }

    settings.setValue(key, stamp);
    settings.sync();

    // Ask Explorer to refresh icon cache without requiring manual restart.
    // This helps when the embedded EXE icon changes but pinned/taskbar icons stay stale.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    SendMessageTimeoutW(HWND_BROADCAST,
                        WM_SETTINGCHANGE,
                        0,
                        reinterpret_cast<LPARAM>(L"Software\\Classes"),
                        SMTO_ABORTIFHUNG,
                        2000,
                        nullptr);

    // Also update icon location for any already-pinned shortcut that targets this EXE.
    refreshPinnedTaskbarShortcutIcons(exeInfo.absoluteFilePath());
}
#endif

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QCoreApplication::setAttribute(Qt::AA_UseStyleSheetPropagationInWidgetStyles);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::NoProfile);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSamples(4);
    fmt.setSwapInterval(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);

#ifdef Q_OS_WIN
    // On Windows with light OS theme, the default palette bleeds into unstyled widgets and
    // breaks the intended dark UI (white navbars/panes). Force a stable dark palette.
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    {
        QPalette pal;
        pal.setColor(QPalette::Window, QColor("#181818"));
        pal.setColor(QPalette::WindowText, QColor("#e4e4e4"));
        pal.setColor(QPalette::Base, QColor("#141414"));
        pal.setColor(QPalette::AlternateBase, QColor("#1f1f1f"));
        pal.setColor(QPalette::ToolTipBase, QColor("#1f1f1f"));
        pal.setColor(QPalette::ToolTipText, QColor("#e4e4e4"));
        pal.setColor(QPalette::Text, QColor("#e4e4e4"));
        pal.setColor(QPalette::Button, QColor("#1f1f1f"));
        pal.setColor(QPalette::ButtonText, QColor("#e4e4e4"));
        pal.setColor(QPalette::BrightText, QColor("#ffffff"));
        pal.setColor(QPalette::Highlight, QColor("#2860ff"));
        pal.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        pal.setColor(QPalette::Link, QColor("#4aa3ff"));
        pal.setColor(QPalette::LinkVisited, QColor("#9b7bff"));
        app.setPalette(pal);
    }
#endif
    QCoreApplication::setOrganizationName(QStringLiteral("Fusion"));
    QCoreApplication::setApplicationName(QStringLiteral("FusionTerminal"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Fusion Terminal"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.20-a"));

#ifdef Q_OS_WIN
    // Make Windows use a stable AppUserModelID so pinned/taskbar identity and icon refresh are consistent.
    // Keep AppUserModelID stable to preserve pinned/taskbar identity for existing users.
    SetCurrentProcessExplicitAppUserModelID(L"FusionTerminal.FusionTerminal");
    maybeRefreshShellIconCache();
#endif

    const QString appDir = QCoreApplication::applicationDirPath();
    // Also set the window icon explicitly. On Windows, pinned taskbar icons use the shortcut/exe,
    // but the running window/taskbar icon comes from WM_SETICON; without this Qt can show a blank icon.
    QString logoPath = resolveAssetPath(QStringLiteral("logo/favicon-32x32.png"));
    if (logoPath.isEmpty()) {
        logoPath = resolveAssetPath(QStringLiteral("logo/favicon.ico"));
    }
    if (!logoPath.isEmpty()) {
        app.setWindowIcon(QIcon(logoPath));
    }

    app.addLibraryPath(app.applicationDirPath() + "/plugins");

    QString appFontFamily;
    const QStringList fontDirs = {
        QDir(appDir).filePath(QStringLiteral("font")),
        QDir(appDir).filePath(QStringLiteral("../font")),
        QDir(appDir).filePath(QStringLiteral("../../font"))};
    const QStringList preferredFiles = {
        QStringLiteral("InterVariable.ttf"),
        QStringLiteral("Inter-Regular.ttf"),
        QStringLiteral("Inter_18pt-Regular.ttf")};
    for (const QString &dirPath : fontDirs) {
        QDir dir(dirPath);
        if (!dir.exists()) continue;
        QStringList files;
        for (const QString &pf : preferredFiles) {
            if (dir.exists(pf)) files << pf;
        }
        files << dir.entryList(QStringList() << QStringLiteral("Inter*.ttf"), QDir::Files);
        for (const QString &file : files) {
            const int id = QFontDatabase::addApplicationFont(dir.filePath(file));
            if (id < 0) continue;
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            for (const QString &fam : families) {
                if (fam.startsWith(QStringLiteral("Inter"), Qt::CaseInsensitive)) {
                    appFontFamily = fam;
                    break;
                }
            }
            if (!appFontFamily.isEmpty()) break;
        }
        if (!appFontFamily.isEmpty()) break;
    }

    // If the fonts are not deployed next to the exe (common when sharing builds),
    // fall back to embedded fonts from the Qt resource system.
    if (appFontFamily.isEmpty()) {
        const QStringList embeddedFonts = {
            QStringLiteral(":/font/Inter_18pt-Regular.ttf"),
            QStringLiteral(":/font/Inter-Regular.ttf"),
            QStringLiteral(":/font/JetBrainsMono-Regular.ttf"),
            QStringLiteral(":/font/JetBrainsMono-Bold.ttf")
        };
        for (const auto &path : embeddedFonts) {
            const int id = QFontDatabase::addApplicationFont(path);
            if (id < 0) {
                continue;
            }
            const QStringList families = QFontDatabase::applicationFontFamilies(id);
            for (const QString &fam : families) {
                if (fam.startsWith(QStringLiteral("Inter"), Qt::CaseInsensitive)) {
                    appFontFamily = fam;
                    break;
                }
            }
            if (!appFontFamily.isEmpty()) {
                break;
            }
        }
    }

    QFont appFont = appFontFamily.isEmpty() ? QFont(QStringLiteral("Segoe UI"), 10)
                                            : QFont(appFontFamily, 10, QFont::Normal);
    const auto strategy = static_cast<QFont::StyleStrategy>(QFont::PreferAntialias | QFont::PreferQuality);
    appFont.setStyleHint(QFont::SansSerif, strategy);
    appFont.setStyleStrategy(strategy);
    appFont.setHintingPreference(QFont::PreferNoHinting);
    appFont.setKerning(true);
    app.setFont(appFont);

    QString backendPath = QStringLiteral("orderbook_backend.exe");
    QString symbol;
    int levels = 500; // 500 per side => ~1000 levels total

    // very simple CLI parsing: --symbol XXX --levels N --backend-path PATH
    for (int i = 1; i < argc; ++i) {
        QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == "--symbol" && i + 1 < argc) {
            symbol = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == "--levels" && i + 1 < argc) {
            levels = QString::fromLocal8Bit(argv[++i]).toInt();
        } else if (arg == "--backend-path" && i + 1 < argc) {
            backendPath = QString::fromLocal8Bit(argv[++i]);
        }
    }

    auto resolveBackendPath = [](const QString &backendArg) -> QString {
        const QString arg = backendArg.trimmed();
        if (arg.isEmpty()) {
            return arg;
        }
        QFileInfo argInfo(arg);
        if (argInfo.isAbsolute() && argInfo.exists()) {
            return argInfo.absoluteFilePath();
        }

        const QDir appDir(QCoreApplication::applicationDirPath());
        const QStringList candidates = {
            appDir.filePath(arg),
            appDir.filePath(QStringLiteral("orderbook_backend.exe")),
            appDir.filePath(QStringLiteral("../orderbook_backend.exe")),
            appDir.filePath(QStringLiteral("../Release/orderbook_backend.exe")),
            appDir.filePath(QStringLiteral("../../Release/orderbook_backend.exe"))
        };
        for (const QString &c : candidates) {
            QFileInfo fi(c);
            if (fi.exists()) {
                return fi.absoluteFilePath();
            }
        }
        return argInfo.absoluteFilePath();
    };

    backendPath = resolveBackendPath(backendPath);

    MainWindow win(backendPath, symbol, levels);
    win.show();
    return app.exec();
}


















