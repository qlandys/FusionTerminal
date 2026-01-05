#include "ConnectionsWindow.h"

#include "ConnectionStore.h"

#include <QCheckBox>
#include <QAction>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QClipboard>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QPainter>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QPixmap>
#include <QIcon>
#include <QScrollArea>
#include <QSaveFile>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTextCursor>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QWidgetAction>
#include <QCloseEvent>

static QString proxyUrlForEnv(const QString &proxyType, const QString &raw);
static QString systemProxyUrlForTarget(const QUrl &target);
#include <QCursor>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QElapsedTimer>
#include <QComboBox>
#include <QGraphicsOpacityEffect>
#include <QGraphicsDropShadowEffect>
#include <QCoreApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <cstdint>
#endif

static void stripSecretsForStorage(ConnectionStore::Profile profile, MexcCredentials &creds);

namespace {
#ifdef _WIN32
static void applyWindowsBackdrop(HWND hwnd, const QColor &tint)
{
    if (!hwnd) {
        return;
    }

    // Prefer Windows 11 system backdrop type (GPU accelerated).
    // Use transient window backdrop (acrylic-like).
    constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
    constexpr int DWMSBT_TRANSIENTWINDOW = 3;

    const HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        auto *setAttr = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (setAttr) {
            const BOOL dark = TRUE;
            setAttr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            const int backdrop = DWMSBT_TRANSIENTWINDOW;
            if (SUCCEEDED(setAttr(hwnd,
                                  DWMWA_SYSTEMBACKDROP_TYPE,
                                  &backdrop,
                                  static_cast<DWORD>(sizeof(backdrop))))) {
                FreeLibrary(dwm);
                return;
            }
        }
        FreeLibrary(dwm);
    }

    // Fallback: Windows 10 acrylic blur via undocumented SetWindowCompositionAttribute.
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    using SetWindowCompositionAttributeFn = BOOL(WINAPI *)(HWND, void *);
    auto *setWca =
        reinterpret_cast<SetWindowCompositionAttributeFn>(GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!setWca) {
        return;
    }

    enum ACCENT_STATE : int {
        ACCENT_DISABLED = 0,
        ACCENT_ENABLE_GRADIENT = 1,
        ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
        ACCENT_ENABLE_BLURBEHIND = 3,
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
        ACCENT_ENABLE_HOSTBACKDROP = 5,
    };
    struct ACCENT_POLICY {
        int AccentState;
        int AccentFlags;
        int GradientColor;
        int AnimationId;
    };
    struct WINDOWCOMPOSITIONATTRIBDATA {
        int Attrib;
        void *pvData;
        SIZE_T cbData;
    };
    constexpr int WCA_ACCENT_POLICY = 19;

    const int a = std::clamp(tint.alpha(), 0, 255);
    const int r = std::clamp(tint.red(), 0, 255);
    const int g = std::clamp(tint.green(), 0, 255);
    const int b = std::clamp(tint.blue(), 0, 255);
    // ABGR
    const int gradientColor = (a << 24) | (b << 16) | (g << 8) | (r);

    ACCENT_POLICY policy{};
    policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.AccentFlags = 2; // draw all
    policy.GradientColor = gradientColor;
    policy.AnimationId = 0;

    WINDOWCOMPOSITIONATTRIBDATA data{};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof(policy);
    setWca(hwnd, &data);
}
#endif

class StatusDot final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(qreal glow READ glow WRITE setGlow)

public:
    explicit StatusDot(QWidget *parent = nullptr) : QWidget(parent)
    {
        // Keep some padding so the glow isn't clipped into a square.
        setFixedSize(24, 24);
        setAttribute(Qt::WA_TranslucentBackground, true);
        m_fadeAnim = new QPropertyAnimation(this, "glow", this);
        m_fadeAnim->setEasingCurve(QEasingCurve::InOutSine);
        m_fadeAnim->setDuration(320);

        m_pulseAnim = new QPropertyAnimation(this, "glow", this);
        m_pulseAnim->setEasingCurve(QEasingCurve::InOutSine);
        m_pulseAnim->setLoopCount(-1);
    }

    qreal glow() const { return m_glow; }
    void setGlow(qreal v)
    {
        v = std::clamp(v, 0.0, 1.0);
        if (qFuzzyCompare(m_glow, v)) {
            return;
        }
        m_glow = v;
        update();
    }

    void setStatus(TradeManager::ConnectionState state, bool isActiveProfile)
    {
        QColor base = QColor("#8b93a1"); // offline grey
        qreal targetGlow = 0.0;
        bool pulse = false;
        int pulseMs = 520;

        if (!isActiveProfile) {
            base = QColor("#d2a84a"); // inactive yellow
            targetGlow = 0.0;
        } else {
            switch (state) {
            case TradeManager::ConnectionState::Connected:
                base = QColor("#34c759");
                targetGlow = 0.70;
                break;
            case TradeManager::ConnectionState::Connecting:
                base = QColor("#34c759");
                targetGlow = 0.80;
                pulse = true;
                pulseMs = 600;
                break;
            case TradeManager::ConnectionState::Error:
                base = QColor("#ff453a");
                targetGlow = 0.85;
                pulse = true;
                pulseMs = 420;
                break;
            case TradeManager::ConnectionState::Disconnected:
            default:
                base = QColor("#8b93a1");
                targetGlow = 0.0;
                break;
            }
        }

        if (m_state == state && m_isActive == isActiveProfile && m_base == base) {
            return;
        }
        m_state = state;
        m_isActive = isActiveProfile;
        m_base = base;

        if (pulse) {
            m_fadeAnim->stop();
            m_pulseAnim->stop();
            m_pulseAnim->setDuration(pulseMs);
            // Flicker-ish pulse (not a smooth sine-only breathe).
            m_pulseAnim->setStartValue(0.20);
            m_pulseAnim->setKeyValueAt(0.25, 1.0);
            m_pulseAnim->setKeyValueAt(0.50, 0.45);
            m_pulseAnim->setKeyValueAt(0.75, 1.0);
            m_pulseAnim->setEndValue(0.20);
            m_pulseAnim->start();
            return;
        }

        m_pulseAnim->stop();
        m_fadeAnim->stop();
        const qreal start = m_glow;
        m_fadeAnim->setStartValue(start);
        if (targetGlow > start) {
            // Flicker up.
            m_fadeAnim->setKeyValueAt(0.25, targetGlow * 1.00);
            m_fadeAnim->setKeyValueAt(0.50, targetGlow * 0.55);
            m_fadeAnim->setKeyValueAt(0.75, targetGlow * 1.00);
        } else {
            // Flicker down.
            m_fadeAnim->setKeyValueAt(0.25, start * 0.55);
            m_fadeAnim->setKeyValueAt(0.50, start * 0.20);
            m_fadeAnim->setKeyValueAt(0.75, targetGlow * 0.35);
        }
        m_fadeAnim->setEndValue(targetGlow);
        m_fadeAnim->start();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF outer = QRectF(rect()).adjusted(1, 1, -1, -1);
        const QPointF c = outer.center();
        const qreal dotR = 4.2;

        if (m_glow > 0.001) {
            const qreal glowR = 6.0 + 6.0 * m_glow; // fits in 24x24 without clipping
            QRadialGradient g(c, glowR);
            QColor outerColor = m_base;
            outerColor.setAlphaF(0.0);
            QColor mid = m_base;
            mid.setAlphaF(0.18 * m_glow);
            QColor inner = m_base;
            inner.setAlphaF(0.50 * m_glow);
            g.setColorAt(0.0, inner);
            g.setColorAt(0.45, mid);
            g.setColorAt(1.0, outerColor);
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(c, glowR, glowR);
        }

        QColor fill = m_base;
        fill.setAlphaF(0.95);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawEllipse(c, dotR, dotR);
    }

private:
    TradeManager::ConnectionState m_state{TradeManager::ConnectionState::Disconnected};
    bool m_isActive{true};
    QColor m_base{QColor("#8b93a1")};
    qreal m_glow{0.0};
    QPropertyAnimation *m_fadeAnim{nullptr};
    QPropertyAnimation *m_pulseAnim{nullptr};
};

QString resolveAssetPath(const QString &relative)
{
    const QString rel = QDir::fromNativeSeparators(relative);

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList bases = {
        appDir,
        QDir(appDir).filePath(QStringLiteral("img")),
        QDir(appDir).filePath(QStringLiteral("assets/img")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("assets/img/icons/logos")),
        QDir(appDir).filePath(QStringLiteral("../img")),
        QDir(appDir).filePath(QStringLiteral("../assets/img")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../assets/img/icons/logos")),
        QDir(appDir).filePath(QStringLiteral("../../img")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons/outline")),
        QDir(appDir).filePath(QStringLiteral("../../assets/img/icons/logos"))
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

QString statusColor(TradeManager::ConnectionState state)
{
    switch (state) {
    case TradeManager::ConnectionState::Connected:
        return QStringLiteral("#2e7d32");
    case TradeManager::ConnectionState::Connecting:
        return QStringLiteral("#f9a825");
    case TradeManager::ConnectionState::Error:
        return QStringLiteral("#c62828");
    case TradeManager::ConnectionState::Disconnected:
    default:
        return QStringLiteral("#616161");
    }
}

QString exchangeTitle(ConnectionStore::Profile profile)
{
    switch (profile) {
    case ConnectionStore::Profile::MexcFutures:
        return QObject::tr("MEXC Futures");
    case ConnectionStore::Profile::Lighter:
        return QObject::tr("Lighter");
    case ConnectionStore::Profile::BinanceSpot:
        return QObject::tr("Binance Spot");
    case ConnectionStore::Profile::BinanceFutures:
        return QObject::tr("Binance Futures");
    case ConnectionStore::Profile::UzxSwap:
        return QObject::tr("UZX Swap");
    case ConnectionStore::Profile::UzxSpot:
        return QObject::tr("UZX Spot");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QObject::tr("MEXC Spot");
    }
}

QString exchangeIconRelative(ConnectionStore::Profile profile)
{
    switch (profile) {
    case ConnectionStore::Profile::BinanceSpot:
    case ConnectionStore::Profile::BinanceFutures:
        return QStringLiteral("icons/logos/binance.png");
    case ConnectionStore::Profile::UzxSpot:
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("icons/logos/UZX.png");
    case ConnectionStore::Profile::Lighter:
        return QStringLiteral("icons/logos/lighter.png");
    case ConnectionStore::Profile::MexcSpot:
    case ConnectionStore::Profile::MexcFutures:
    default:
        return QStringLiteral("icons/logos/mexc.png");
    }
}

QIcon loadTintedSvgIcon(const QString &relativePath, const QColor &color, int px)
{
    const QString path = resolveAssetPath(relativePath);
    if (path.isEmpty()) {
        return QIcon();
    }
    const QIcon base(path);
    QPixmap pix = base.pixmap(px, px);
    if (pix.isNull()) {
        return base;
    }
    QImage img = pix.toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    {
        QPainter p(&img);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), color);
    }
    return QIcon(QPixmap::fromImage(img));
}

constexpr const char *kLighterVaultMagic = "FUSION_LIGHTER_VAULT_V1\n";
constexpr const char *kLighterVaultMagicLegacy = "PLASMA_LIGHTER_VAULT_V1\n";

#ifdef _WIN32
QString dpapiProtectToBase64Local(const QString &plain)
{
    if (plain.isEmpty()) {
        return {};
    }
    const QByteArray in = plain.toUtf8();
    DATA_BLOB inputBlob;
    inputBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(in.data()));
    inputBlob.cbData = static_cast<DWORD>(in.size());

    DATA_BLOB outputBlob;
    outputBlob.pbData = nullptr;
    outputBlob.cbData = 0;

    const BOOL ok = CryptProtectData(&inputBlob,
                                    L"FusionTerminal Lighter vault",
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    CRYPTPROTECT_UI_FORBIDDEN,
                                    &outputBlob);
    if (!ok || !outputBlob.pbData || outputBlob.cbData == 0) {
        if (outputBlob.pbData) {
            LocalFree(outputBlob.pbData);
        }
        return {};
    }

    const QByteArray out(reinterpret_cast<const char *>(outputBlob.pbData),
                         static_cast<int>(outputBlob.cbData));
    LocalFree(outputBlob.pbData);
    return QString::fromLatin1(out.toBase64(QByteArray::Base64Encoding));
}

QString dpapiUnprotectFromBase64Local(const QString &b64)
{
    if (b64.trimmed().isEmpty()) {
        return {};
    }
    const QByteArray enc = QByteArray::fromBase64(b64.toLatin1(), QByteArray::Base64Encoding);
    if (enc.isEmpty()) {
        return {};
    }

    DATA_BLOB inputBlob;
    inputBlob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(enc.data()));
    inputBlob.cbData = static_cast<DWORD>(enc.size());

    DATA_BLOB outputBlob;
    outputBlob.pbData = nullptr;
    outputBlob.cbData = 0;

    const BOOL ok = CryptUnprotectData(&inputBlob,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      CRYPTPROTECT_UI_FORBIDDEN,
                                      &outputBlob);
    if (!ok || !outputBlob.pbData || outputBlob.cbData == 0) {
        if (outputBlob.pbData) {
            LocalFree(outputBlob.pbData);
        }
        return {};
    }

    const QByteArray out(reinterpret_cast<const char *>(outputBlob.pbData),
                         static_cast<int>(outputBlob.cbData));
    LocalFree(outputBlob.pbData);
    return QString::fromUtf8(out);
}
#endif

bool writeLighterVaultFile(const QString &path, const QJsonObject &obj, QString *outErr)
{
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
#ifdef _WIN32
    const QString enc = dpapiProtectToBase64Local(QString::fromUtf8(json));
    if (enc.isEmpty()) {
        if (outErr) *outErr = QStringLiteral("DPAPI encrypt failed");
        return false;
    }
    const QByteArray payload = QByteArray(kLighterVaultMagic) + enc.toUtf8();
#else
    const QByteArray payload = QByteArray(kLighterVaultMagic) + json.toBase64(QByteArray::Base64Encoding);
#endif

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (outErr) *outErr = QStringLiteral("Failed to write file");
        return false;
    }
    f.write(payload);
    if (!f.commit()) {
        if (outErr) *outErr = QStringLiteral("Failed to commit file");
        return false;
    }
    return true;
}

bool readLighterVaultFile(const QString &path, QJsonObject *outObj, QString *outErr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (outErr) *outErr = QStringLiteral("File not found");
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (!data.startsWith(kLighterVaultMagic)
        && !data.startsWith(kLighterVaultMagicLegacy)) {
        // Legacy: plain JSON file (we'll accept and re-encrypt on load).
        const QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            if (outErr) *outErr = QStringLiteral("Invalid vault format");
            return false;
        }
        if (outObj) *outObj = doc.object();
        return true;
    }
    const int magicLen = data.startsWith(kLighterVaultMagic)
                             ? static_cast<int>(std::strlen(kLighterVaultMagic))
                             : static_cast<int>(std::strlen(kLighterVaultMagicLegacy));
    const QByteArray rest = data.mid(magicLen);
    const QString b64 = QString::fromUtf8(rest).trimmed();
#ifdef _WIN32
    const QString plain = dpapiUnprotectFromBase64Local(b64);
    if (plain.isEmpty()) {
        if (outErr) *outErr = QStringLiteral("DPAPI decrypt failed");
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(plain.toUtf8());
#else
    const QByteArray plain = QByteArray::fromBase64(rest.trimmed(), QByteArray::Base64Encoding);
    const QJsonDocument doc = QJsonDocument::fromJson(plain);
#endif
    if (!doc.isObject()) {
        if (outErr) *outErr = QStringLiteral("Invalid JSON in vault");
        return false;
    }
    if (outObj) *outObj = doc.object();
    return true;
}

static QString normalizeProxyTypeForStorage(QString type)
{
    type = type.trimmed().toLower();
    if (type == QStringLiteral("https")) {
        type = QStringLiteral("http");
    }
    if (type != QStringLiteral("http") && type != QStringLiteral("socks5")) {
        type = QStringLiteral("http");
    }
    return type;
}

struct SavedProxyPreset {
    QString name;
    QString proxy;
    QString type;
};

static QList<SavedProxyPreset> loadSavedProxyPresets()
{
    QList<SavedProxyPreset> out;
    QSettings s;
    s.beginGroup(QStringLiteral("proxyPresets"));
    const int count = s.beginReadArray(QStringLiteral("items"));
    out.reserve(count);
    for (int i = 0; i < count; ++i) {
        s.setArrayIndex(i);
        SavedProxyPreset p;
        p.name = s.value(QStringLiteral("name")).toString().trimmed();
        p.proxy = s.value(QStringLiteral("proxy")).toString().trimmed();
        p.type = normalizeProxyTypeForStorage(s.value(QStringLiteral("type")).toString());
        if (!p.name.isEmpty() && !p.proxy.isEmpty()) {
            out.push_back(p);
        }
    }
    s.endArray();
    s.endGroup();
    return out;
}

static void saveSavedProxyPresets(const QList<SavedProxyPreset> &items)
{
    QSettings s;
    s.beginGroup(QStringLiteral("proxyPresets"));
    s.remove(QString());
    s.beginWriteArray(QStringLiteral("items"));
    int idx = 0;
    for (const SavedProxyPreset &p : items) {
        if (p.name.isEmpty() || p.proxy.isEmpty()) {
            continue;
        }
        s.setArrayIndex(idx++);
        s.setValue(QStringLiteral("name"), p.name);
        s.setValue(QStringLiteral("proxy"), p.proxy);
        s.setValue(QStringLiteral("type"), normalizeProxyTypeForStorage(p.type));
    }
    s.endArray();
    s.endGroup();
    s.sync();
}

static void upsertSavedProxyPreset(const SavedProxyPreset &preset)
{
    if (preset.name.trimmed().isEmpty() || preset.proxy.trimmed().isEmpty()) {
        return;
    }
    QList<SavedProxyPreset> items = loadSavedProxyPresets();
    const QString target = preset.name.trimmed();
    bool replaced = false;
    for (SavedProxyPreset &item : items) {
        if (item.name.compare(target, Qt::CaseInsensitive) == 0) {
            item.name = preset.name.trimmed();
            item.proxy = preset.proxy.trimmed();
            item.type = normalizeProxyTypeForStorage(preset.type);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        items.prepend(SavedProxyPreset{preset.name.trimmed(),
                                       preset.proxy.trimmed(),
                                       normalizeProxyTypeForStorage(preset.type)});
    }
    saveSavedProxyPresets(items);
}

static bool removeSavedProxyPreset(const QString &name)
{
    const QString target = name.trimmed();
    if (target.isEmpty()) {
        return false;
    }
    QList<SavedProxyPreset> items = loadSavedProxyPresets();
    const int before = items.size();
    items.erase(std::remove_if(items.begin(),
                               items.end(),
                               [&](const SavedProxyPreset &p) {
                                   return p.name.compare(target, Qt::CaseInsensitive) == 0;
                               }),
                items.end());
    if (items.size() == before) {
        return false;
    }
    saveSavedProxyPresets(items);
    return true;
}

static bool savedProxyNameExists(const QString &name)
{
    const QString target = name.trimmed();
    if (target.isEmpty()) {
        return false;
    }
    const QList<SavedProxyPreset> items = loadSavedProxyPresets();
    for (const SavedProxyPreset &p : items) {
        if (p.name.compare(target, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString idFromProfile(ConnectionStore::Profile profile)
{
    switch (profile) {
    case ConnectionStore::Profile::MexcFutures:
        return QStringLiteral("mexcFutures");
    case ConnectionStore::Profile::Lighter:
        return QStringLiteral("lighter");
    case ConnectionStore::Profile::BinanceSpot:
        return QStringLiteral("binanceSpot");
    case ConnectionStore::Profile::BinanceFutures:
        return QStringLiteral("binanceFutures");
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("uzxSwap");
    case ConnectionStore::Profile::UzxSpot:
        return QStringLiteral("uzxSpot");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QStringLiteral("mexcSpot");
    }
}

QString defaultColorForProfile(ConnectionStore::Profile profile)
{
    switch (profile) {
    case ConnectionStore::Profile::MexcFutures:
        return QStringLiteral("#f5b642");
    case ConnectionStore::Profile::Lighter:
        return QStringLiteral("#38bdf8");
    case ConnectionStore::Profile::BinanceSpot:
        return QStringLiteral("#f0b90b");
    case ConnectionStore::Profile::BinanceFutures:
        return QStringLiteral("#f5b642");
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("#ff7f50");
    case ConnectionStore::Profile::UzxSpot:
        return QStringLiteral("#8bc34a");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QStringLiteral("#4c9fff");
    }
}

QString badgeStyle(const QString &color)
{
    return QStringLiteral(
        "QLabel#ConnectionStatusBadge {"
        "  border-radius: 0px;"
        "  padding: 2px 10px;"
        "  color: #ffffff;"
        "  background: %1;"
        "}").arg(color);
}

void styleColorButton(QToolButton *btn, const QColor &c)
{
    if (!btn) return;
    const QString color = c.isValid() ? c.name(QColor::HexRgb) : QStringLiteral("#999999");
    btn->setStyleSheet(
        QStringLiteral("QToolButton { background:%1; border:1px solid #3a3a3a; border-radius:4px; }")
            .arg(color));
}
} // namespace

ConnectionsWindow::ConnectionsWindow(ConnectionStore *store, TradeManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_manager(manager)
{
    setWindowTitle(tr("Подключения - Fusion Terminal"));
    setModal(false);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    // Avoid layered/translucent top-level windows: Windows disables subpixel ClearType
    // which makes fonts look "wrong". We keep the backdrop effect via DWM while
    // rendering an opaque window surface.
    setAttribute(Qt::WA_DeleteOnClose, false);
    setCursor(Qt::ArrowCursor);
    setMinimumWidth(420);
    setMinimumHeight(560);
    resize(760, 680);

    setStyleSheet(QStringLiteral(
        "QDialog { background: transparent; }"
        "#ConnectionsPanel {"
        "  background: rgba(33,35,39,230);"
        "  border: 1px solid #2f3338;"
        "  border-radius: 0px;"
        "}"
        "#ConnectionsTitleBar {"
        "  background: qlineargradient(x1:0,y1:0,x2:0,y2:1,"
        "    stop:0 rgba(40,42,46,240), stop:1 rgba(34,36,40,235));"
        "  border-top-left-radius: 0px;"
        "  border-top-right-radius: 0px;"
        "  border-bottom: 1px solid #2f3338;"
        "}"
        "#ConnectionsTitle { color:#e6e6e6; font-weight: 650; }"
        "#ConnectionsMeta { color:#9aa4b2; font-weight: 600; }"
        "QToolButton#ConnectionsHeaderButton {"
        "  background: rgba(255,255,255,0.05);"
        "  border: 1px solid rgba(255,255,255,0.12);"
        "  border-radius: 6px;"
        "  padding: 3px 10px;"
        "}"
        "QToolButton#ConnectionsHeaderButton:hover { background: rgba(255,255,255,0.10); }"
        "QToolButton#ConnectionsHeaderButton:pressed { background: rgba(255,255,255,0.16); }"
        "QToolButton#WindowCloseButton {"
        "  background: rgba(255,255,255,0.04);"
        "  border: 1px solid rgba(255,255,255,0.10);"
        "  border-radius: 6px;"
        "  padding: 0px;"
        "}"
        "QToolButton#WindowCloseButton:hover { background: rgba(255,255,255,0.10); }"
        "QToolButton#WindowCloseButton:pressed { background: rgba(255,255,255,0.16); }"
        "QToolButton { color:#e0e0e0; }"
        "QLabel { color:#cfcfcf; }"
        "QPlainTextEdit {"
        "  background:#1f2125;"
        "  border:1px solid #2f3338;"
        "  border-radius:0px;"
        "  color:#e6e6e6;"
        "}"
        "QScrollArea { background: transparent; }"
    ));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("ConnectionsPanel"));
    panel->setCursor(Qt::ArrowCursor);
    layout->addWidget(panel);

    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(10, 10, 10, 10);
    panelLayout->setSpacing(8);

    if (m_store) {
        m_store->ensureProfilesSchema();
    }

    auto *titleBar = new QFrame(panel);
    titleBar->setObjectName(QStringLiteral("ConnectionsTitleBar"));
    titleBar->setFixedHeight(40);
    m_titleBar = titleBar;
    m_titleBar->installEventFilter(this);

    auto *header = new QHBoxLayout(titleBar);
    header->setContentsMargins(12, 0, 10, 0);
    header->setSpacing(10);

    auto *titleLabel = new QLabel(tr("Connections"), titleBar);
    titleLabel->setObjectName(QStringLiteral("ConnectionsTitle"));
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    header->addWidget(titleLabel);

    auto *headerActions = new QWidget(titleBar);
    auto *headerActionsLayout = new QHBoxLayout(headerActions);
    headerActionsLayout->setContentsMargins(0, 0, 0, 0);
    headerActionsLayout->setSpacing(8);
    header->addWidget(headerActions);
    header->addStretch(1);
    m_titleLabel = new QLabel(titleBar);
    m_titleLabel->setObjectName(QStringLiteral("ConnectionsMeta"));
    QFont statusFont = m_titleLabel->font();
    statusFont.setPointSizeF(statusFont.pointSizeF() + 1.0);
    statusFont.setBold(true);
    m_titleLabel->setFont(statusFont);
    header->addWidget(m_titleLabel);

    auto *addButton = new QToolButton(titleBar);
    addButton->setText(tr("+ Профиль"));
    addButton->setObjectName(QStringLiteral("ConnectionsHeaderButton"));
    addButton->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(addButton);
    auto addProfile = [this](ConnectionStore::Profile type) {
        if (!m_store) {
            return;
        }
        const int n = m_store->profiles(type).size() + 1;
        QString label = exchangeTitle(type);
        if (n > 1) {
            label = QStringLiteral("%1 #%2").arg(label).arg(n);
        }
        const auto created = m_store->createProfile(type, label);
        rebuildCardsFromStore();
        for (auto *c : m_cards) {
            if (c && c->profileId == created.id) {
                setCardExpanded(c, true);
                break;
            }
        }
        refreshUi();
    };
    menu->addAction(tr("MEXC Spot"), this, [addProfile]() { addProfile(ConnectionStore::Profile::MexcSpot); });
    menu->addAction(tr("MEXC Futures"), this, [addProfile]() { addProfile(ConnectionStore::Profile::MexcFutures); });
    menu->addAction(tr("Lighter"), this, [addProfile]() { addProfile(ConnectionStore::Profile::Lighter); });
    menu->addAction(tr("Binance Spot"), this, [addProfile]() { addProfile(ConnectionStore::Profile::BinanceSpot); });
    menu->addAction(tr("Binance Futures"), this, [addProfile]() { addProfile(ConnectionStore::Profile::BinanceFutures); });
    menu->addAction(tr("UZX Swap"), this, [addProfile]() { addProfile(ConnectionStore::Profile::UzxSwap); });
    menu->addAction(tr("UZX Spot"), this, [addProfile]() { addProfile(ConnectionStore::Profile::UzxSpot); });
    addButton->setMenu(menu);
    auto *logToggle = new QToolButton(titleBar);
    logToggle->setText(tr("Лог"));
    logToggle->setCheckable(true);
    logToggle->setChecked(false);
    logToggle->setObjectName(QStringLiteral("ConnectionsHeaderButton"));
    auto *lighterButton = new QToolButton(titleBar);
    lighterButton->setText(tr("Lighter setup"));
    lighterButton->setToolTip(tr("Auto-install SDK and generate Lighter API keys"));
    lighterButton->setVisible(false);

    auto *closeBtn = new QToolButton(titleBar);
    closeBtn->setObjectName(QStringLiteral("WindowCloseButton"));
    closeBtn->setToolTip(tr("Close"));
    closeBtn->setIcon(loadTintedSvgIcon(QStringLiteral("icons/outline/x-white.svg"), QColor("#e6ecf4"), 16));
    closeBtn->setIconSize(QSize(16, 16));
    closeBtn->setFixedSize(28, 28);
    connect(closeBtn, &QToolButton::clicked, this, &QDialog::close);
    header->addWidget(closeBtn);

    panelLayout->addWidget(titleBar);
    headerActionsLayout->addWidget(addButton);
    headerActionsLayout->addWidget(logToggle);
    headerActionsLayout->addWidget(lighterButton);

#ifdef _WIN32
    // Backdrop is applied on first show to avoid blocking/flicker during creation.
#endif

    m_cardsContainer = new QWidget(panel);
    m_cardsLayout = new QVBoxLayout(m_cardsContainer);
    m_cardsLayout->setContentsMargins(0, 0, 0, 0);
    m_cardsLayout->setSpacing(6);

    auto *scroll = new QScrollArea(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(m_cardsContainer);
    panelLayout->addWidget(scroll, 1);

    auto *logLabel = new QLabel(tr("Лог"), panel);
    panelLayout->addWidget(logLabel);
    m_logView = new QPlainTextEdit(panel);
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(500);
    m_logView->setVisible(false);
    logLabel->setVisible(false);
    panelLayout->addWidget(m_logView, 0);

    connect(logToggle, &QToolButton::toggled, this, [this, logLabel](bool on) {
        if (logLabel) {
            logLabel->setVisible(on);
        }
        if (m_logView) {
            m_logView->setVisible(on);
        }
    });

    connect(lighterButton, &QToolButton::clicked, this, [this]() { launchLighterSetup(); });

    if (m_manager) {
        connect(m_manager,
                &TradeManager::connectionStateChanged,
                this,
                &ConnectionsWindow::handleManagerStateChanged);
    }

    // Defer the expensive initial refresh until after the window is shown so the main UI doesn't "blink" on first open.
    QTimer::singleShot(10, this, [this]() {
        rebuildCardsFromStore();
        refreshUi();

#ifdef _WIN32
        if (!m_backdropQueued) {
            m_backdropQueued = true;
            applyWindowsBackdrop(reinterpret_cast<HWND>(winId()), QColor(20, 20, 20, 180));
        }
#endif
    });
}

void ConnectionsWindow::showEvent(QShowEvent *event)
{
    if (m_firstShow) {
        m_firstShow = false;
    }
    QDialog::showEvent(event);
}

bool ConnectionsWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleBar) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = true;
                m_dragStartGlobal = me->globalPosition().toPoint();
                m_dragStartWindowPos = frameGeometry().topLeft();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            if (!m_dragging) {
                break;
            }
            auto *me = static_cast<QMouseEvent *>(event);
            const QPoint delta = me->globalPosition().toPoint() - m_dragStartGlobal;
            move(m_dragStartWindowPos + delta);
            return true;
        }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = false;
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

ConnectionsWindow::CardWidgets *ConnectionsWindow::createCard(const QString &profileId, ConnectionStore::Profile type)
{
    auto *card = new CardWidgets();
    card->profileId = profileId;
    card->type = type;
    card->color = QColor(defaultColorForProfile(type));
    const bool isLighter = (type == ConnectionStore::Profile::Lighter);

    auto *frame = new QFrame(this);
    card->frame = frame;
    frame->setObjectName(QStringLiteral("ConnectionCard"));
    frame->setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    frame->setStyleSheet(QStringLiteral(
        "QFrame#ConnectionCard {"
        "  border:1px solid #2f3338;"
        "  border-radius:0px;"
        "  background: #26282c;"
        "}"
        "#ConnectionCard QLineEdit, #ConnectionCard QSpinBox {"
        "  background:#1f2125;"
        "  border:1px solid #34383d;"
        "  border-radius:0px;"
        "  padding:4px 6px;"
        "  font-size:12px;"
        "  color:#e0e0e0;"
        "  selection-background-color:#3a3f45;"
        "}"
        "#ConnectionCard QLineEdit:focus, #ConnectionCard QSpinBox:focus { border-color:#4a4f55; }"
        "#ConnectionCard QPushButton {"
        "  font-family: \"Inter\";"
        "  font-weight: 600;"
        "  font-size: 12px;"
        "  color: #e6e6e6;"
        "  background-color: #2a2d31;"
        "  border: 1px solid #3a3f45;"
        "  border-radius: 0px;"
        "  padding: 4px 10px;"
        "}"
        "#ConnectionCard QPushButton:hover { background-color: #30343a; border-color:#42474e; }"
        "#ConnectionCard QPushButton:pressed { background-color: #24282d; border-color:#42474e; }"
        "#ConnectionCard QPushButton:disabled {"
        "  color: rgba(233,237,243,0.40);"
        "  background-color: rgba(42,45,49,0.55);"
        "  border-color: rgba(58,63,69,0.55);"
        "}"
        "#ConnectionCard QToolButton#CardExpandButton { background: transparent; border: none; }"
        "#ConnectionCard QToolButton#CardExpandButton:hover { background: transparent; border: none; }"
        "#ConnectionCard QToolButton#CardPill {"
        "  background:rgba(255,255,255,0.04);"
        "  border:1px solid rgba(255,255,255,0.06);"
        "  border-radius:0px;"
        "  padding:4px 8px;"
        "  color:#cfcfcf;"
        "}"));

    auto *v = new QVBoxLayout(frame);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);

    auto *top = new QHBoxLayout();
    card->expandButton = new QToolButton(frame);
    card->expandButton->setCheckable(true);
    card->expandButton->setChecked(false);
    card->expandButton->setObjectName(QStringLiteral("CardExpandButton"));
    card->expandButton->setAutoRaise(true);
    const QIcon expandCollapsed =
        loadTintedSvgIcon(QStringLiteral("icons/outline/circle-chevron-right.svg"), QColor("#d3d9e4"), 18);
    const QIcon expandExpanded =
        loadTintedSvgIcon(QStringLiteral("icons/outline/circle-chevron-up.svg"), QColor("#d3d9e4"), 18);
    card->expandButton->setProperty("iconCollapsed", expandCollapsed);
    card->expandButton->setProperty("iconExpanded", expandExpanded);
    card->expandButton->setIcon(expandCollapsed);
    card->expandButton->setIconSize(QSize(18, 18));
    card->expandButton->setFixedSize(20, 20);
    top->addWidget(card->expandButton);

    card->iconLabel = new QLabel(frame);
    card->iconLabel->setFixedSize(18, 18);
    card->iconLabel->setScaledContents(true);
    {
        const QString iconPath = resolveAssetPath(exchangeIconRelative(type));
        QPixmap pix(iconPath);
        if (!pix.isNull()) {
            card->iconLabel->setPixmap(pix);
        }
    }
    top->addWidget(card->iconLabel);

    card->colorButton = new QToolButton(frame);
    card->colorButton->setAutoRaise(false);
    card->colorButton->setToolTip(tr("Выбрать цвет аккаунта"));
    card->colorButton->setFixedSize(18, 18);
    styleColorButton(card->colorButton, card->color);
    top->addWidget(card->colorButton);

    auto *labels = new QVBoxLayout();
    labels->setContentsMargins(0, 0, 0, 0);
    labels->setSpacing(0);

    card->nameLabel = new QLabel(exchangeTitle(type), frame);
    QFont nameFont = card->nameLabel->font();
    nameFont.setBold(true);
    card->nameLabel->setFont(nameFont);
    labels->addWidget(card->nameLabel);

    card->exchangeLabel = new QLabel(exchangeTitle(type), frame);
    card->exchangeLabel->setStyleSheet(QStringLiteral("color:#9aa4b2; font-size:11px;"));
    labels->addWidget(card->exchangeLabel);

    top->addLayout(labels);
    top->addStretch(1);

    card->statusBadge = new QLabel(tr("Отключено"), frame);
    card->statusDot = new StatusDot(frame);
    static_cast<StatusDot *>(card->statusDot)->setStatus(TradeManager::ConnectionState::Disconnected, true);
    top->addWidget(card->statusDot);

    card->statusBadge->setObjectName(QStringLiteral("ConnectionStatusText"));
    card->statusBadge->setStyleSheet(QStringLiteral("color:#b7beca; padding:0 6px;"));
    card->statusBadge->setFixedWidth(86);
    card->statusBadge->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    card->statusBadge->setText(QStringLiteral("offline"));
    top->addWidget(card->statusBadge);

    // Single toggle button (connect/disconnect). Keep legacy disconnect button hidden.
    card->disconnectButton = new QPushButton(tr("Отключить"), frame);
    card->disconnectButton->setVisible(false);

    card->connectButton = new QPushButton(tr("Подключить"), frame);
    card->disconnectButton->setFixedHeight(22);
    card->connectButton->setFixedHeight(22);
    card->connectButton->setFixedWidth(100);
    top->addWidget(card->connectButton);

    card->menuButton = new QToolButton(frame);
    card->menuButton->setText(QStringLiteral("⋯"));
    card->menuButton->setPopupMode(QToolButton::InstantPopup);
    auto *cardMenu = new QMenu(card->menuButton);
    cardMenu->addAction(tr("Сделать активным"), this, [this, card]() {
        if (!m_store || !card) {
            return;
        }
        m_store->setActiveProfileId(card->type, card->profileId);
        refreshUi();
    });
    cardMenu->addSeparator();
    cardMenu->addAction(tr("Переименовать..."), this, [this, card]() {
        if (!m_store || !card) {
            return;
        }
        const auto cur = m_store->profileById(card->profileId);
        bool ok = false;
        const QString next =
            QInputDialog::getText(this, tr("Название профиля"), tr("Название:"), QLineEdit::Normal, cur.creds.label, &ok)
                .trimmed();
        if (!ok || next.isEmpty()) {
            return;
        }
        ConnectionStore::StoredProfile updated = cur;
        updated.creds.label = next;
        m_store->saveProfile(updated);
        refreshUi();
    });
    cardMenu->addAction(tr("Дублировать"), this, [this, card]() {
        if (!m_store || !card) {
            return;
        }
        const auto cur = m_store->profileById(card->profileId);
        MexcCredentials copy = cur.creds;
        if (copy.label.trimmed().isEmpty()) {
            copy.label = exchangeTitle(card->type);
        }
        copy.label = copy.label + QStringLiteral(" (copy)");
        const auto created = m_store->createProfile(card->type, copy.label, copy);
        rebuildCardsFromStore();
        for (auto *c : m_cards) {
            if (c && c->profileId == created.id) {
                setCardExpanded(c, true);
                break;
            }
        }
        refreshUi();
    });
    cardMenu->addSeparator();
    cardMenu->addAction(tr("Выше"), this, [this, card]() { moveCard(card, -1); });
    cardMenu->addAction(tr("Ниже"), this, [this, card]() { moveCard(card, +1); });
    cardMenu->addSeparator();
    cardMenu->addAction(tr("Удалить"), this, [this, card]() { deleteCard(card); });
    card->menuButton->setMenu(cardMenu);
    top->addWidget(card->menuButton);
    v->addLayout(top);

    card->body = new QWidget(frame);
    auto *bodyLayout = new QVBoxLayout(card->body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(8);
    card->body->setVisible(false);

    card->profileNameEdit = new QLineEdit(card->body);
    card->profileNameEdit->setPlaceholderText(tr("Название профиля"));
    bodyLayout->addWidget(card->profileNameEdit);

    auto *sectionLabel = new QLabel(tr("Параметры"), card->body);
    sectionLabel->setStyleSheet(QStringLiteral("color:#cfd7e4; font-weight:600; padding-left:2px;"));
    bodyLayout->addWidget(sectionLabel);

    auto applyIconToToolButton = [](QToolButton *btn, const QString &relativeSvg, const QSize &px) {
        if (!btn) {
            return;
        }
        const QIcon icon = loadTintedSvgIcon(relativeSvg, QColor("#ffffff"), std::max(px.width(), px.height()));
        if (!icon.isNull()) {
            btn->setIcon(icon);
            btn->setIconSize(px);
        }
    };

    auto applyColoredIconToToolButton = [](QToolButton *btn, const QString &relativeSvg, const QColor &color, const QSize &px) {
        if (!btn) {
            return;
        }
        const QIcon icon = loadTintedSvgIcon(relativeSvg, color, std::max(px.width(), px.height()));
        if (!icon.isNull()) {
            btn->setIcon(icon);
            btn->setIconSize(px);
        }
    };

    if (isLighter) {
        auto *cfgRow = new QWidget(card->body);
        auto *cfgLayout = new QHBoxLayout(cfgRow);
        cfgLayout->setContentsMargins(0, 0, 0, 0);
        cfgLayout->setSpacing(6);

        card->lighterConfigPathEdit = new QLineEdit(cfgRow);
        card->lighterConfigPathEdit->setReadOnly(true);
        card->lighterConfigPathEdit->setFocusPolicy(Qt::NoFocus);
        card->lighterConfigPathEdit->setPlaceholderText(tr("Lighter vault (encrypted)"));
        card->lighterConfigPathEdit->setToolTip(tr("Хранится локально и зашифровано (Windows DPAPI)."));
        card->lighterConfigPathEdit->setText(tr("Lighter vault (encrypted)"));
        card->lighterConfigPathEdit->setStyleSheet(
            QStringLiteral("QLineEdit { background: rgba(255,255,255,0.03); border:1px solid #273143; border-radius:8px; padding:6px 10px; }"));

        card->lighterConfigReloadButton = new QToolButton(cfgRow);
        card->lighterConfigReloadButton->setAutoRaise(true);
        card->lighterConfigReloadButton->setToolTip(tr("Обновить"));
        applyIconToToolButton(card->lighterConfigReloadButton,
                              QStringLiteral("icons/outline/refresh.svg"),
                              QSize(18, 18));

        card->lighterConfigEditButton = new QToolButton(cfgRow);
        card->lighterConfigEditButton->setAutoRaise(true);
        card->lighterConfigEditButton->setToolTip(tr("Редактировать"));
        applyIconToToolButton(card->lighterConfigEditButton,
                              QStringLiteral("icons/outline/edit.svg"),
                              QSize(18, 18));

        cfgLayout->addWidget(card->lighterConfigPathEdit, 1);
        cfgLayout->addWidget(card->lighterConfigReloadButton);
        cfgLayout->addWidget(card->lighterConfigEditButton);
        bodyLayout->addWidget(cfgRow);
    }

    card->apiKeyEdit = new QLineEdit(card->body);
    card->apiKeyEdit->setPlaceholderText(tr("API key"));
    card->secretEdit = new QLineEdit(card->body);
    card->secretEdit->setPlaceholderText(tr("API secret"));
    card->secretEdit->setEchoMode(QLineEdit::Password);
    card->passphraseEdit = new QLineEdit(card->body);
    card->passphraseEdit->setPlaceholderText(tr("Passphrase (UZX)"));
    card->uidEdit = new QLineEdit(card->body);
    const bool isMexcFutures = (type == ConnectionStore::Profile::MexcFutures);
    if (isMexcFutures) {
        card->uidEdit->setPlaceholderText(tr("Authorization token (WEB...)"));
        card->uidEdit->setEchoMode(QLineEdit::Password);
    } else {
        card->uidEdit->setPlaceholderText(tr("U_ID (unused)"));
    }
    card->proxyEdit = new QLineEdit(card->body);
    card->proxyEdit->setPlaceholderText(tr("Прокси (http://user:pass@host:port)"));
    card->proxyEdit->setToolTip(
        tr("Прокси для подключений (HTTP CONNECT).\n\n"
           "Форматы:\n"
           "- host:port\n"
           "- host:port:login:pass\n"
           "- login:pass:host:port\n"
           "- login:pass@host:port\n"
           "- host:port@login:pass\n"
           "- http://login:pass@host:port\n"));

    card->proxyTypeCombo = new QComboBox(card->body);
    card->proxyTypeCombo->addItem(QStringLiteral("HTTP"), QStringLiteral("http"));
    card->proxyTypeCombo->addItem(QStringLiteral("SOCKS5"), QStringLiteral("socks5"));
    card->proxyTypeCombo->setToolTip(tr("Тип прокси"));
    card->proxyTypeCombo->setFixedWidth(90);

    card->proxyPresetButton = new QToolButton(card->body);
    card->proxyPresetButton->setAutoRaise(true);
    card->proxyPresetButton->setToolTip(tr("Выбрать сохраненный прокси"));
    applyColoredIconToToolButton(card->proxyPresetButton,
                                 QStringLiteral("icons/outline/caret-down.svg"),
                                 QColor("#9aa4b2"),
                                 QSize(16, 16));

    card->baseUrlEdit = new QLineEdit(card->body);
    card->baseUrlEdit->setPlaceholderText(tr("Base URL (Lighter)"));
    card->baseUrlEdit->setToolTip(tr("Lighter API endpoint (mainnet)."));
    card->accountIndexEdit = new QLineEdit(card->body);
    card->accountIndexEdit->setPlaceholderText(tr("Account index (Lighter)"));
    card->accountIndexEdit->setToolTip(tr("Account index inside Lighter (usually 0)."));
    card->apiKeyIndexEdit = new QLineEdit(card->body);
    card->apiKeyIndexEdit->setPlaceholderText(tr("API key index (0..253, Lighter)"));
    card->apiKeyIndexEdit->setToolTip(tr("Index of the API key within your Lighter account (0..253)."));

    // Seed phrase entry is handled only via the Lighter setup dialog (Create API).
    card->seedPhraseEdit = nullptr;

    bodyLayout->addWidget(card->apiKeyEdit);
    bodyLayout->addWidget(card->secretEdit);
    bodyLayout->addWidget(card->passphraseEdit);
    bodyLayout->addWidget(card->uidEdit);
    {
        auto *proxyRow = new QWidget(card->body);
        auto *proxyLayout = new QHBoxLayout(proxyRow);
        proxyLayout->setContentsMargins(0, 0, 0, 0);
        proxyLayout->setSpacing(6);

        card->proxyGoogleIcon = new QToolButton(proxyRow);
        card->proxyGoogleIcon->setAutoRaise(true);
        card->proxyGoogleIcon->setEnabled(false);
        card->proxyGoogleIcon->setToolTip(QStringLiteral("google.com"));
        applyColoredIconToToolButton(card->proxyGoogleIcon,
                                     QStringLiteral("icons/outline/brand-google.svg"),
                                     QColor("#8b93a1"),
                                     QSize(18, 18));

        card->proxyFacebookIcon = new QToolButton(proxyRow);
        card->proxyFacebookIcon->setAutoRaise(true);
        card->proxyFacebookIcon->setEnabled(false);
        card->proxyFacebookIcon->setToolTip(QStringLiteral("facebook.com"));
        applyColoredIconToToolButton(card->proxyFacebookIcon,
                                     QStringLiteral("icons/outline/brand-facebook.svg"),
                                     QColor("#8b93a1"),
                                     QSize(18, 18));

        card->proxyYandexIcon = new QToolButton(proxyRow);
        card->proxyYandexIcon->setAutoRaise(true);
        card->proxyYandexIcon->setEnabled(false);
        card->proxyYandexIcon->setToolTip(QStringLiteral("yandex.ru"));
        applyColoredIconToToolButton(card->proxyYandexIcon,
                                     QStringLiteral("icons/outline/brand-yandex.svg"),
                                     QColor("#8b93a1"),
                                     QSize(18, 18));

        card->proxyCheckButton = new QToolButton(proxyRow);
        card->proxyCheckButton->setAutoRaise(true);
        card->proxyCheckButton->setToolTip(tr("Проверить прокси"));
        applyColoredIconToToolButton(card->proxyCheckButton,
                                     QStringLiteral("icons/outline/mobiledata.svg"),
                                     QColor("#ffffff"),
                                     QSize(18, 18));

        card->proxyGeoWidget = new QWidget(proxyRow);
        card->proxyGeoWidget->setToolTip(tr("Геолокация прокси"));
        // Reserve space so the proxy input doesn't "jump" when geo text appears.
        card->proxyGeoWidget->setFixedWidth(210);
        card->proxyGeoWidget->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        auto *geoLayout = new QHBoxLayout(card->proxyGeoWidget);
        geoLayout->setContentsMargins(0, 0, 0, 0);
        geoLayout->setSpacing(6);
        geoLayout->addStretch(1); // right-align geo contents

        card->proxyGeoIconLabel = new QLabel(card->proxyGeoWidget);
        // Flags are not square; keep aspect ratio inside a fixed box.
        card->proxyGeoIconLabel->setFixedSize(20, 14);
        card->proxyGeoIconLabel->setScaledContents(false);
        card->proxyGeoIconLabel->setAlignment(Qt::AlignCenter);
        geoLayout->addWidget(card->proxyGeoIconLabel);

        card->proxyGeoTextLabel = new QLabel(card->proxyGeoWidget);
        card->proxyGeoTextLabel->setStyleSheet(QStringLiteral("color:#cfd7e4;"));
        geoLayout->addWidget(card->proxyGeoTextLabel);

        // Default globe icon.
        {
            const QIcon icon =
                loadTintedSvgIcon(QStringLiteral("icons/outline/globe.svg"), QColor("#8b93a1"), 18);
            if (!icon.isNull()) {
                const QSize box = card->proxyGeoIconLabel->size();
                QPixmap pm = icon.pixmap(std::max(box.width(), box.height()));
                card->proxyGeoIconLabel->setPixmap(pm.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            }
            card->proxyGeoTextLabel->setText(QString());
        }

        proxyLayout->addWidget(card->proxyTypeCombo);
        if (card->proxyPresetButton) {
            proxyLayout->addWidget(card->proxyPresetButton);
        }
        proxyLayout->addWidget(card->proxyEdit, 1);
        proxyLayout->addWidget(card->proxyGeoWidget);
        proxyLayout->addWidget(card->proxyGoogleIcon);
        proxyLayout->addWidget(card->proxyFacebookIcon);
        proxyLayout->addWidget(card->proxyYandexIcon);
        proxyLayout->addWidget(card->proxyCheckButton);
        bodyLayout->addWidget(proxyRow);
    }
    bodyLayout->addWidget(card->baseUrlEdit);
    bodyLayout->addWidget(card->accountIndexEdit);
    bodyLayout->addWidget(card->apiKeyIndexEdit);

    Q_UNUSED(isLighter);

    auto *options = new QHBoxLayout();
    options->setSpacing(0);
    QWidget *optWrap = new QWidget(card->body);
    optWrap->setStyleSheet(QStringLiteral("background:rgba(255,255,255,0.03); border:1px solid #273143; border-radius:8px;"));
    auto *optLayout = new QHBoxLayout(optWrap);
    optLayout->setContentsMargins(10, 6, 10, 6);
    optLayout->setSpacing(12);
    card->saveSecretCheck = new QCheckBox(tr("Сохранить secret"), optWrap);
    card->viewOnlyCheck = new QCheckBox(tr("Только просмотр"), optWrap);
    card->autoConnectCheck = new QCheckBox(tr("Автоподключение"), optWrap);
    optLayout->addWidget(card->saveSecretCheck);
    optLayout->addWidget(card->viewOnlyCheck);
    optLayout->addWidget(card->autoConnectCheck);
    card->proxyNameEdit = new QLineEdit(optWrap);
    card->proxyNameEdit->setPlaceholderText(tr("Название прокси"));
    card->proxyNameEdit->setToolTip(tr("Название для сохраненного прокси"));
    card->proxyNameEdit->setFixedWidth(180);
    const QIcon saveIcon =
        loadTintedSvgIcon(QStringLiteral("icons/outline/device-floppy.svg"), QColor("#9aa4b2"), 16);
    if (!saveIcon.isNull()) {
        card->proxySaveAction = card->proxyNameEdit->addAction(saveIcon, QLineEdit::TrailingPosition);
        if (card->proxySaveAction) {
            card->proxySaveAction->setToolTip(tr("Сохранить прокси"));
        }
    }
    optLayout->addWidget(card->proxyNameEdit);
    card->proxyStatusLabel = new QLabel(optWrap);
    card->proxyStatusLabel->setStyleSheet(QStringLiteral("color:#9aa4b2; background: transparent; border: none;"));
    optLayout->addWidget(card->proxyStatusLabel);
    card->proxySaveButton = new QToolButton(optWrap);
    card->proxySaveButton->setAutoRaise(true);
    card->proxySaveButton->setToolTip(tr("Сохранить прокси"));
    applyColoredIconToToolButton(card->proxySaveButton,
                                 QStringLiteral("icons/outline/device-floppy.svg"),
                                 QColor("#9aa4b2"),
                                 QSize(18, 18));
    card->proxySaveButton->setVisible(false);
    optLayout->addStretch(1);
    options->addWidget(optWrap);
    bodyLayout->addLayout(options);
    auto *tools = new QHBoxLayout();
    tools->addStretch(1);
    if (isLighter) {
        card->lighterSetupButton = new QToolButton(card->body);
        card->lighterSetupButton->setText(QStringLiteral("⚙"));
        card->lighterSetupButton->setAutoRaise(true);
        card->lighterSetupButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        card->lighterSetupButton->setToolTip(tr("создать api"));
        card->lighterSetupButton->setFixedSize(34, 28);
        const QIcon apiIcon =
            loadTintedSvgIcon(QStringLiteral("icons/outline/api.svg"), QColor("#ffffff"), 22);
        if (!apiIcon.isNull()) {
            card->lighterSetupButton->setIcon(apiIcon);
            card->lighterSetupButton->setIconSize(QSize(22, 22));
            card->lighterSetupButton->setText(QString());
        } else {
            card->lighterSetupButton->setText(QStringLiteral("API"));
        }
        tools->addWidget(card->lighterSetupButton);

        card->lighterTestnetButton = new QToolButton(card->body);
        card->lighterTestnetButton->setCheckable(true);
        card->lighterTestnetButton->setText(tr("Testnet"));
        card->lighterTestnetButton->setToolTip(tr("Use testnet endpoint"));
        card->lighterTestnetButton->setVisible(false);
        tools->addWidget(card->lighterTestnetButton);

        card->lighterMainnetButton = new QToolButton(card->body);
        card->lighterMainnetButton->setCheckable(true);
        card->lighterMainnetButton->setText(tr("Mainnet"));
        card->lighterMainnetButton->setToolTip(tr("Use mainnet endpoint (real funds)"));
        card->lighterMainnetButton->setVisible(false);
        tools->addWidget(card->lighterMainnetButton);

    }
    card->importButton = new QToolButton(card->body);
    card->importButton->setText(tr("Import Lighter config"));
    card->importButton->setToolTip(tr("Load lighter_api_key_config.json created by Lighter setup wizard"));
    if (!isLighter) {
        tools->addWidget(card->importButton);
    } else {
        card->importButton->setVisible(false);
    }
    bodyLayout->addLayout(tools);

    v->addWidget(card->body);

    m_cards.push_back(card);
    rebuildLayout();

    connect(card->connectButton, &QPushButton::clicked, this, [this, profileId, card]() {
        if (!card) {
            handleConnectClicked(profileId);
            return;
        }
        const auto state = card->currentState;
        if (state == TradeManager::ConnectionState::Connected || state == TradeManager::ConnectionState::Connecting) {
            handleDisconnectClicked(profileId);
        } else {
            handleConnectClicked(profileId);
        }
    });
    connect(card->expandButton, &QToolButton::clicked, this, [this, card]() {
        setCardExpanded(card, !card->expanded);
    });
    connect(card->colorButton, &QToolButton::clicked, this, [this, card]() {
        const QColor picked = QColorDialog::getColor(card->color, this, tr("Выбрать цвет"));
        if (picked.isValid()) {
            card->color = picked;
            styleColorButton(card->colorButton, picked);
            persistCard(*card);
        }
    });
    auto registerPersist = [this, card]() {
        if (card && card->profileNameEdit && card->nameLabel) {
            const QString name = card->profileNameEdit->text().trimmed();
            if (!name.isEmpty()) {
                card->nameLabel->setText(name);
            }
        }
        persistCard(*card);
    };
    connect(card->saveSecretCheck, &QCheckBox::toggled, this, registerPersist);
    connect(card->viewOnlyCheck, &QCheckBox::toggled, this, registerPersist);
    connect(card->autoConnectCheck, &QCheckBox::toggled, this, registerPersist);
    connect(card->profileNameEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->apiKeyEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->secretEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->passphraseEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->uidEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->proxyEdit, &QLineEdit::textChanged, this, registerPersist);
    if (isLighter && card->proxyEdit) {
        connect(card->proxyEdit, &QLineEdit::editingFinished, this, [this, card]() {
            if (!card) {
                return;
            }
            saveLighterProxyToVaultIfPossible(collectCredentials(*card), false);
        });
    }
    if (card->proxyTypeCombo) {
        connect(card->proxyTypeCombo, &QComboBox::currentIndexChanged, this, registerPersist);
        if (isLighter) {
            connect(card->proxyTypeCombo, &QComboBox::currentIndexChanged, this, [this, card]() {
                if (!card) {
                    return;
                }
                saveLighterProxyToVaultIfPossible(collectCredentials(*card), false);
            });
        }
    }
    auto setProxyStatus = [card](const QString &text, const QColor &color) {
        if (!card || !card->proxyStatusLabel) {
            return;
        }
        card->proxyStatusLabel->setText(text);
        card->proxyStatusLabel->setStyleSheet(QStringLiteral("color:%1;").arg(color.name(QColor::HexRgb)));
        QTimer::singleShot(1800, card->proxyStatusLabel, [card]() {
            if (card && card->proxyStatusLabel) {
                card->proxyStatusLabel->clear();
            }
        });
    };

    if (card->proxyPresetButton) {
        auto *menu = new QMenu(card->proxyPresetButton);
        card->proxyPresetButton->setMenu(menu);
        card->proxyPresetButton->setPopupMode(QToolButton::InstantPopup);
        connect(menu, &QMenu::aboutToShow, this, [this, card, menu, setProxyStatus]() {
            menu->clear();
            const QList<SavedProxyPreset> presets = loadSavedProxyPresets();
            if (presets.isEmpty()) {
                QAction *empty = menu->addAction(tr("Нет сохраненных прокси"));
                empty->setEnabled(false);
                return;
            }
            for (const SavedProxyPreset &preset : presets) {
                const QString typeLabel =
                    (normalizeProxyTypeForStorage(preset.type) == QStringLiteral("socks5"))
                        ? QStringLiteral("SOCKS5")
                        : QStringLiteral("HTTP");
                auto *rowAction = new QWidgetAction(menu);
                auto *row = new QWidget(menu);
                row->setFixedHeight(24);
                auto *rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(6, 0, 6, 0);
                rowLayout->setSpacing(8);

                const QIcon delIcon =
                    loadTintedSvgIcon(QStringLiteral("icons/outline/x.svg"), QColor("#9aa4b2"), 14);
                auto *delBtn = new QToolButton(row);
                delBtn->setAutoRaise(true);
                delBtn->setIcon(delIcon);
                delBtn->setToolTip(tr("Удалить прокси"));
                delBtn->setFixedSize(18, 18);
                rowLayout->addWidget(delBtn, 0, Qt::AlignVCenter);

                auto *selectBtn = new QToolButton(row);
                selectBtn->setAutoRaise(true);
                selectBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
                selectBtn->setText(QStringLiteral("%1 (%2)").arg(preset.name, typeLabel));
                selectBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                selectBtn->setFixedHeight(22);
                selectBtn->setToolTip(QStringLiteral("%1 (%2)").arg(preset.name, typeLabel));
                rowLayout->addWidget(selectBtn, 1, Qt::AlignVCenter);

                rowAction->setDefaultWidget(row);
                menu->addAction(rowAction);

                connect(selectBtn, &QToolButton::clicked, this, [card, preset]() {
                    if (!card) {
                        return;
                    }
                    if (card->proxyEdit) {
                        card->proxyEdit->setText(preset.proxy);
                    }
                    if (card->proxyTypeCombo) {
                        const int idx = card->proxyTypeCombo->findData(normalizeProxyTypeForStorage(preset.type));
                        if (idx >= 0) {
                            card->proxyTypeCombo->setCurrentIndex(idx);
                        }
                    }
                    if (card->proxyNameEdit && !preset.name.isEmpty()) {
                        card->proxyNameEdit->setText(preset.name);
                    }
                });
                connect(delBtn, &QToolButton::clicked, this, [this, rowAction, row, preset, setProxyStatus]() {
                    if (removeSavedProxyPreset(preset.name)) {
                        appendLogMessage(tr("Прокси удален: %1").arg(preset.name));
                        setProxyStatus(tr("Удалено"), QColor("#f59e0b"));
                        if (rowAction) {
                            rowAction->setVisible(false);
                        }
                        if (row) {
                            row->deleteLater();
                        }
                    } else {
                        appendLogMessage(tr("Прокси с таким названием не найден."));
                    }
                });
            }
        });
    }
    auto pulseProxySaved = [card](const QColor &color) {
        if (!card || !card->proxyNameEdit) {
            return;
        }
        auto *effect = qobject_cast<QGraphicsDropShadowEffect *>(card->proxyNameEdit->graphicsEffect());
        if (!effect) {
            effect = new QGraphicsDropShadowEffect(card->proxyNameEdit);
            effect->setOffset(0, 0);
            effect->setBlurRadius(14);
            card->proxyNameEdit->setGraphicsEffect(effect);
        }
        effect->setColor(color);
        auto *anim = new QPropertyAnimation(effect, "color", effect);
        anim->setDuration(520);
        anim->setStartValue(color);
        QColor end = color;
        end.setAlpha(0);
        anim->setEndValue(end);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
    };

    auto saveProxy = [this, card, isLighter, pulseProxySaved, setProxyStatus]() {
        if (!card) {
            return;
        }
        const auto profile = card->type;
        const MexcCredentials creds = collectCredentials(*card);
        const QString proxyName = card->proxyNameEdit ? card->proxyNameEdit->text().trimmed() : QString();
        const QString proxyRaw = card->proxyEdit ? card->proxyEdit->text().trimmed() : QString();
        const QString proxyType =
            (card->proxyTypeCombo ? card->proxyTypeCombo->currentData().toString() : QStringLiteral("http"));
        if (m_store) {
            m_store->setActiveProfileId(profile, card->profileId);
        }
        persistCard(*card);
        if (isLighter) {
            saveLighterProxyToVaultIfPossible(creds, true);
        } else {
            appendLogMessage(tr("Proxy saved"));
        }
        if (!proxyRaw.isEmpty()) {
            if (proxyName.isEmpty()) {
                appendLogMessage(tr("Введите название прокси и нажмите Сохранить еще раз."));
                pulseProxySaved(QColor("#ef4444"));
                setProxyStatus(tr("Нужно название"), QColor("#ef4444"));
                if (card->proxyNameEdit) {
                    card->proxyNameEdit->setFocus(Qt::OtherFocusReason);
                }
            } else {
                if (savedProxyNameExists(proxyName)) {
                    appendLogMessage(tr("Название прокси уже занято."));
                    pulseProxySaved(QColor("#ef4444"));
                    setProxyStatus(tr("Название уже существует"), QColor("#ef4444"));
                    return;
                }
                upsertSavedProxyPreset(SavedProxyPreset{proxyName, proxyRaw, proxyType});
                appendLogMessage(tr("Сохранено"));
                appendLogMessage(tr("Прокси сохранен: %1").arg(proxyName));
                pulseProxySaved(QColor("#22c55e"));
                setProxyStatus(tr("Сохранено"), QColor("#22c55e"));
            }
        }

        if (card->proxySaveButton) {
            // Visual feedback only (no reconnect).
            card->proxySaveButton->setEnabled(false);
            QTimer::singleShot(900, this, [card]() {
                if (!card || !card->proxySaveButton) {
                    return;
                }
                card->proxySaveButton->setEnabled(true);
            });
        }
    };
    if (card->proxySaveButton) {
        connect(card->proxySaveButton, &QToolButton::clicked, this, saveProxy);
    }
    if (card->proxySaveAction) {
        connect(card->proxySaveAction, &QAction::triggered, this, saveProxy);
    }
    connect(card->baseUrlEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->accountIndexEdit, &QLineEdit::textChanged, this, registerPersist);
    connect(card->apiKeyIndexEdit, &QLineEdit::textChanged, this, registerPersist);
    if (card->seedPhraseEdit) {
        connect(card->seedPhraseEdit, &QLineEdit::textChanged, this, registerPersist);
    }

    if (card->lighterSetupButton) {
        connect(card->lighterSetupButton, &QToolButton::clicked, this, [this]() {
            launchLighterSetup();
        });
    }
    if (card->lighterConfigReloadButton) {
        connect(card->lighterConfigReloadButton, &QToolButton::clicked, this, [this, card]() {
            loadLighterConfigIntoCard(card, lighterConfigPath(), true);
        });
    }
    if (card->lighterConfigEditButton) {
        connect(card->lighterConfigEditButton, &QToolButton::clicked, this, [this, card]() {
            openLighterConfigEditor(card);
        });
    }

    auto parseProxy = [](const QString &raw, const QString &type, QNetworkProxy &outProxy, QString &err) -> bool {
        const QString s = raw.trimmed();
        if (s.isEmpty()) {
            outProxy = QNetworkProxy(QNetworkProxy::NoProxy);
            return true;
        }

        const QString t = type.trimmed().toLower();
        const bool isSocks = (t == QStringLiteral("socks5") || t == QStringLiteral("socks"));

        auto make = [&](bool socks, const QString &host, int port, const QString &user, const QString &pass) -> bool {
            if (host.trimmed().isEmpty() || port <= 0 || port > 65535) {
                err = QStringLiteral("Invalid host/port");
                return false;
            }
            QNetworkProxy p(socks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                            host.trimmed(),
                            static_cast<quint16>(port));
            if (!user.isEmpty()) {
                p.setUser(user);
                p.setPassword(pass);
            }
            outProxy = p;
            return true;
        };

        // Full URL
        if (s.contains(QStringLiteral("://"))) {
            const QUrl u(s);
            if (!u.isValid() || u.host().isEmpty() || u.port() <= 0) {
                err = QStringLiteral("Invalid proxy URL");
                return false;
            }
            const QString scheme = u.scheme().toLower();
            const bool urlSocks = scheme.contains(QStringLiteral("socks"));
            return make(urlSocks || isSocks, u.host(), u.port(), u.userName(), u.password());
        }

        // With @ either user:pass@host:port or host:port@user:pass
        if (s.contains(QLatin1Char('@'))) {
            const QStringList parts = s.split(QLatin1Char('@'));
            if (parts.size() != 2) {
                err = QStringLiteral("Invalid proxy format");
                return false;
            }
            const QString a = parts[0];
            const QString b = parts[1];

            auto parseHostPort = [](const QString &hp, QString &host, int &port) -> bool {
                const QStringList t = hp.split(QLatin1Char(':'), Qt::KeepEmptyParts);
                if (t.size() != 2) return false;
                bool ok = false;
                const int p = t[1].toInt(&ok);
                if (!ok) return false;
                host = t[0];
                port = p;
                return true;
            };
            auto parseUserPass = [](const QString &up, QString &user, QString &pass) -> bool {
                const QStringList t = up.split(QLatin1Char(':'), Qt::KeepEmptyParts);
                if (t.size() != 2) return false;
                user = t[0];
                pass = t[1];
                return true;
            };

            QString host;
            int port = 0;
            QString user, pass;
            if (parseHostPort(a, host, port)) {
                if (!parseUserPass(b, user, pass)) {
                    err = QStringLiteral("Invalid credentials format");
                    return false;
                }
                return make(isSocks, host, port, user, pass);
            }
            if (parseHostPort(b, host, port)) {
                if (!parseUserPass(a, user, pass)) {
                    err = QStringLiteral("Invalid credentials format");
                    return false;
                }
                return make(isSocks, host, port, user, pass);
            }
            err = QStringLiteral("Invalid proxy format");
            return false;
        }

        // Colon-separated
        const QStringList tokens = s.split(QLatin1Char(':'), Qt::KeepEmptyParts);
        if (tokens.size() == 2) {
            bool ok = false;
            const int port = tokens[1].toInt(&ok);
            if (!ok) {
                err = QStringLiteral("Invalid port");
                return false;
            }
            return make(isSocks, tokens[0], port, QString(), QString());
        }
        if (tokens.size() == 4) {
            // host:port:user:pass OR user:pass:host:port
            bool ok1 = false;
            const int port1 = tokens[1].toInt(&ok1);
            if (ok1) {
                return make(isSocks, tokens[0], port1, tokens[2], tokens[3]);
            }
            bool ok2 = false;
            const int port2 = tokens[3].toInt(&ok2);
            if (ok2) {
                return make(isSocks, tokens[2], port2, tokens[0], tokens[1]);
            }
            err = QStringLiteral("Invalid proxy format");
            return false;
        }

        err = QStringLiteral("Invalid proxy format");
        return false;
    };

    if (card->proxyCheckButton) {
        connect(card->proxyCheckButton, &QToolButton::clicked, this, [this, card, parseProxy, applyColoredIconToToolButton]() {
            if (!card || !card->proxyEdit || !card->proxyCheckButton) {
                return;
            }
            const QString proxyText = card->proxyEdit->text().trimmed();
            const QString proxyType =
                (card->proxyTypeCombo ? card->proxyTypeCombo->currentData().toString() : QStringLiteral("http"));
            QNetworkProxy proxy;
            QString parseErr;
            if (!parseProxy(proxyText, proxyType, proxy, parseErr)) {
                applyColoredIconToToolButton(card->proxyCheckButton,
                                             QStringLiteral("icons/outline/mobiledata-off.svg"),
                                             QColor("#ff4d4d"),
                                             QSize(18, 18));
                card->proxyCheckButton->setToolTip(tr("Прокси невалидный: %1").arg(parseErr));
                return;
            }

            if (card->proxyGeoWidget && card->proxyGeoIconLabel && card->proxyGeoTextLabel) {
                // Reset geo display to "loading".
                const QIcon icon =
                    loadTintedSvgIcon(QStringLiteral("icons/outline/world-search.svg"), QColor("#8b93a1"), 18);
                if (!icon.isNull()) {
                    const QSize box = card->proxyGeoIconLabel->size();
                    QPixmap pm = icon.pixmap(std::max(box.width(), box.height()));
                    card->proxyGeoIconLabel->setPixmap(pm.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                }
                card->proxyGeoTextLabel->setText(tr("..."));
                card->proxyGeoWidget->setToolTip(tr("Геолокация прокси (loading...)"));
            }

            struct Site {
                QString name;
                QUrl url;
                QToolButton *icon = nullptr;
                QString iconSvg;
            };
            QVector<Site> sites;
            sites.push_back(Site{QStringLiteral("google"), QUrl(QStringLiteral("https://www.google.com/generate_204")), card->proxyGoogleIcon, QStringLiteral("icons/outline/brand-google.svg")});
            sites.push_back(Site{QStringLiteral("facebook"), QUrl(QStringLiteral("https://www.facebook.com/favicon.ico")), card->proxyFacebookIcon, QStringLiteral("icons/outline/brand-facebook.svg")});
            sites.push_back(Site{QStringLiteral("yandex"), QUrl(QStringLiteral("https://yandex.ru")), card->proxyYandexIcon, QStringLiteral("icons/outline/brand-yandex.svg")});

            auto *nam = new QNetworkAccessManager(card->proxyCheckButton);
            nam->setProxy(proxy);

            // Extra proxy geo check: ipwhois.app
            {
                const QString hostIp = proxy.hostName().trimmed();
                if (!hostIp.isEmpty() && card->proxyGeoWidget && card->proxyGeoIconLabel && card->proxyGeoTextLabel) {
                    const QUrl whoisUrl(QStringLiteral("https://ipwhois.app/json/%1").arg(hostIp));
                    QNetworkRequest req(whoisUrl);
                    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                    auto *reply = nam->get(req);

                    auto *timeout = new QTimer(reply);
                    timeout->setSingleShot(true);
                    timeout->setInterval(6500);
                    connect(timeout, &QTimer::timeout, reply, [reply]() {
                        if (reply) reply->abort();
                    });
                    timeout->start();

                    auto renderSvgIcon = [](const QByteArray &svg, int px) -> QIcon {
                        QPixmap pm;
                        if (!pm.loadFromData(svg)) {
                            pm.loadFromData(svg, "SVG");
                        }
                        if (pm.isNull()) {
                            return {};
                        }
                        pm = pm.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                        return QIcon(pm);
                    };

                    connect(reply, &QNetworkReply::finished, this, [this, reply, card, hostIp, renderSvgIcon]() {
                        if (!card || !card->proxyGeoWidget || !card->proxyGeoIconLabel || !card->proxyGeoTextLabel) {
                            reply->deleteLater();
                            return;
                        }
                        const auto err = reply->error();
                        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        const QByteArray raw = reply->readAll();
                        reply->deleteLater();

                        auto setGeoIcon = [&](const QIcon &icon) {
                            if (!icon.isNull() && card && card->proxyGeoIconLabel) {
                                const QSize box = card->proxyGeoIconLabel->size();
                                QPixmap pm = icon.pixmap(std::max(box.width(), box.height()));
                                card->proxyGeoIconLabel->setPixmap(pm.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                            }
                        };

                        if (err != QNetworkReply::NoError || status >= 400) {
                            const QIcon icon =
                                loadTintedSvgIcon(QStringLiteral("icons/outline/globe-off.svg"), QColor("#ff4d4d"), 18);
                            setGeoIcon(icon);
                            card->proxyGeoTextLabel->setText(QString());
                            card->proxyGeoWidget->setToolTip(tr("ipwhois failed for %1").arg(hostIp));
                            return;
                        }

                        const QJsonDocument doc = QJsonDocument::fromJson(raw);
                        if (!doc.isObject()) {
                            const QIcon icon =
                                loadTintedSvgIcon(QStringLiteral("icons/outline/globe-off.svg"), QColor("#ff4d4d"), 18);
                            setGeoIcon(icon);
                            card->proxyGeoTextLabel->setText(QString());
                            card->proxyGeoWidget->setToolTip(tr("ipwhois invalid JSON for %1").arg(hostIp));
                            return;
                        }
                        const QJsonObject obj = doc.object();
                        const bool ok = obj.value(QStringLiteral("success")).toBool(false);
                        if (!ok) {
                            const QIcon icon =
                                loadTintedSvgIcon(QStringLiteral("icons/outline/globe-off.svg"), QColor("#ff4d4d"), 18);
                            setGeoIcon(icon);
                            card->proxyGeoTextLabel->setText(QString());
                            card->proxyGeoWidget->setToolTip(tr("ipwhois returned success=false for %1").arg(hostIp));
                            return;
                        }
                        const QString countryCode = obj.value(QStringLiteral("country_code")).toString().trimmed();
                        const QString city = obj.value(QStringLiteral("city")).toString().trimmed();
                        QString flagUrl = obj.value(QStringLiteral("country_flag")).toString().trimmed();
                        // ipwhois returns SVG flags; Qt SVG plugin might not be available in this build.
                        // Use a PNG fallback URL based on country_code when possible.
                        if (!countryCode.isEmpty() && flagUrl.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive)) {
                            flagUrl = QStringLiteral("https://flagcdn.com/w20/%1.png").arg(countryCode.toLower());
                        }
                        const QString text =
                            city.isEmpty() ? countryCode : QStringLiteral("%1, %2").arg(countryCode, city);
                        auto elide = [&](const QString &s) -> QString {
                            if (!card || !card->proxyGeoWidget || !card->proxyGeoTextLabel) return s;
                            const int w =
                                std::max(40, card->proxyGeoWidget->width() - (card->proxyGeoIconLabel ? card->proxyGeoIconLabel->width() : 18) - 12);
                            return card->proxyGeoTextLabel->fontMetrics().elidedText(s, Qt::ElideRight, w);
                        };

                        // Default to globe icon, replace with country flag once fetched.
                        {
                            const QIcon icon =
                                loadTintedSvgIcon(QStringLiteral("icons/outline/globe.svg"), QColor("#ffffff"), 18);
                            setGeoIcon(icon);
                        }
                        card->proxyGeoTextLabel->setText(elide(text));
                        card->proxyGeoWidget->setToolTip(tr("%1 (%2)").arg(text, hostIp));

                        if (flagUrl.isEmpty()) {
                            return;
                        }
                        QNetworkRequest flagReq{QUrl(flagUrl)};
                        flagReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                        auto *flagNam = new QNetworkAccessManager(card->proxyGeoWidget);
                        auto *flagReply = flagNam->get(flagReq);
                        auto *flagTimeout = new QTimer(flagReply);
                        flagTimeout->setSingleShot(true);
                        flagTimeout->setInterval(6500);
                        connect(flagTimeout, &QTimer::timeout, flagReply, [flagReply]() {
                            if (flagReply) flagReply->abort();
                        });
                        flagTimeout->start();
                        connect(flagReply, &QNetworkReply::finished, this, [flagReply, card, renderSvgIcon]() {
                            if (!card || !card->proxyGeoWidget || !card->proxyGeoIconLabel) {
                                flagReply->deleteLater();
                                return;
                            }
                            const auto err2 = flagReply->error();
                            const int status2 = flagReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                            const QByteArray svg = flagReply->readAll();
                            flagReply->deleteLater();
                            if (err2 != QNetworkReply::NoError || status2 >= 400) {
                                return;
                            }
                            const QIcon icon = renderSvgIcon(svg, 64);
                            if (!icon.isNull()) {
                                QPixmap pm = icon.pixmap(64, 64);
                                const QSize box = card->proxyGeoIconLabel->size();
                                card->proxyGeoIconLabel->setPixmap(pm.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                            }
                        });
                    });
                }
            }

            auto setupBlink = [](QToolButton *btn) {
                if (!btn) return;
                auto *opacity = new QGraphicsOpacityEffect(btn);
                opacity->setOpacity(1.0);
                btn->setGraphicsEffect(opacity);
            };
            auto setGlow = [](QToolButton *btn, const QColor &c) {
                if (!btn) return;
                auto *shadow = new QGraphicsDropShadowEffect(btn);
                shadow->setBlurRadius(18);
                shadow->setOffset(0, 0);
                shadow->setColor(c);
                btn->setGraphicsEffect(shadow);
            };

            // Start blinking green
            for (auto &s : sites) {
                if (!s.icon) continue;
                applyColoredIconToToolButton(s.icon, s.iconSvg, QColor("#22c55e"), QSize(18, 18));
                setGlow(s.icon, QColor("#22c55e"));
            }
            applyColoredIconToToolButton(card->proxyCheckButton,
                                         QStringLiteral("icons/outline/mobiledata.svg"),
                                         QColor("#22c55e"),
                                         QSize(18, 18));
            setGlow(card->proxyCheckButton, QColor("#22c55e"));

            auto *blinkTimer = new QTimer(card->proxyCheckButton);
            blinkTimer->setInterval(180);
            blinkTimer->setProperty("fusion_blink", true);
            connect(blinkTimer, &QTimer::timeout, this, [card]() {
                auto toggle = [](QToolButton *btn) {
                    if (!btn) return;
                    auto *eff = btn->graphicsEffect();
                    auto *op = qobject_cast<QGraphicsOpacityEffect*>(eff);
                    if (!op) {
                        op = new QGraphicsOpacityEffect(btn);
                        op->setOpacity(1.0);
                        btn->setGraphicsEffect(op);
                    }
                    const qreal cur = op->opacity();
                    op->setOpacity(cur > 0.6 ? 0.35 : 1.0);
                };
                toggle(card->proxyGoogleIcon);
                toggle(card->proxyFacebookIcon);
                toggle(card->proxyYandexIcon);
            });
            blinkTimer->start();

            struct ResultState {
                int pending = 0;
                int ok = 0;
                int fail = 0;
                QElapsedTimer t;
            };
            auto *state = new ResultState();
            state->pending = sites.size();
            state->t.start();

            auto finishAll = [card, state, blinkTimer, applyColoredIconToToolButton, setGlow]() {
                if (blinkTimer) {
                    blinkTimer->stop();
                    blinkTimer->deleteLater();
                }

                auto applyFinal = [&](QToolButton *btn, const QString &svg) {
                    if (!btn) return;
                    const bool ok = btn->property("fusion_proxy_ok").toBool();
                    if (ok) {
                        // Successful: white icon + white glow.
                        applyColoredIconToToolButton(btn, svg, QColor("#ffffff"), QSize(18, 18));
                        setGlow(btn, QColor("#ffffff"));
                    } else {
                        // Failed: red icon, no glow.
                        applyColoredIconToToolButton(btn, svg, QColor("#ff4d4d"), QSize(18, 18));
                        btn->setGraphicsEffect(nullptr);
                    }
                };
                applyFinal(card->proxyGoogleIcon, QStringLiteral("icons/outline/brand-google.svg"));
                applyFinal(card->proxyFacebookIcon, QStringLiteral("icons/outline/brand-facebook.svg"));
                applyFinal(card->proxyYandexIcon, QStringLiteral("icons/outline/brand-yandex.svg"));

                const bool allOk = (state->ok > 0 && state->fail == 0);
                const bool someOk = (state->ok > 0 && state->fail > 0);
                if (allOk) {
                    applyColoredIconToToolButton(card->proxyCheckButton,
                                                 QStringLiteral("icons/outline/mobiledata.svg"),
                                                 QColor("#22c55e"),
                                                 QSize(18, 18));
                    card->proxyCheckButton->setToolTip(tr("Прокси OK (%1 ms)").arg(state->t.elapsed()));
                } else if (someOk) {
                    applyColoredIconToToolButton(card->proxyCheckButton,
                                                 QStringLiteral("icons/outline/mobiledata-off.svg"),
                                                 QColor("#f59e0b"),
                                                 QSize(18, 18));
                    card->proxyCheckButton->setToolTip(tr("Прокси частично работает (%1/%2 OK)")
                                                           .arg(state->ok)
                                                           .arg(state->ok + state->fail));
                } else {
                    applyColoredIconToToolButton(card->proxyCheckButton,
                                                 QStringLiteral("icons/outline/mobiledata-off.svg"),
                                                 QColor("#ff4d4d"),
                                                 QSize(18, 18));
                    card->proxyCheckButton->setToolTip(tr("Прокси не работает"));
                }
                card->proxyCheckButton->setGraphicsEffect(nullptr);
                delete state;
            };

            for (auto &s : sites) {
                if (!s.icon) {
                    state->pending--;
                    continue;
                }
                QNetworkRequest req(s.url);
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = nam->get(req);
                reply->setProperty("fusion_site", s.name);
                reply->setProperty("fusion_svg", s.iconSvg);
                reply->setProperty("fusion_btn_ptr", QVariant::fromValue<void*>(s.icon));

                auto *timeout = new QTimer(reply);
                timeout->setSingleShot(true);
                timeout->setInterval(6500);
                connect(timeout, &QTimer::timeout, reply, [reply]() {
                    if (reply) reply->abort();
                });
                timeout->start();

                connect(reply, &QNetworkReply::finished, this, [reply, card, state, applyColoredIconToToolButton, finishAll]() mutable {
                    const auto err = reply->error();
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QString svg = reply->property("fusion_svg").toString();
                    auto *btn = static_cast<QToolButton*>(reply->property("fusion_btn_ptr").value<void*>());
                    reply->deleteLater();

                    const bool ok = (err == QNetworkReply::NoError && status > 0 && status < 400);
                    if (btn) {
                        btn->setProperty("fusion_proxy_ok", ok);
                    }
                    if (ok) state->ok++; else state->fail++;
                    state->pending--;
                    if (state->pending <= 0) {
                        finishAll();
                    }
                });
            }
        });
    }

    auto setLighterBaseUrl = [this](const QString &url) {
        if (!m_store) {
            return;
        }
        Q_UNUSED(url);
        QTimer::singleShot(0, this, [this]() { refreshUi(); });
    };

    if (card->lighterTestnetButton) {
        connect(card->lighterTestnetButton, &QToolButton::clicked, this, [card, setLighterBaseUrl]() {
            if (card->lighterMainnetButton) {
                card->lighterMainnetButton->setChecked(false);
            }
            card->lighterTestnetButton->setChecked(true);
            setLighterBaseUrl(QStringLiteral("https://testnet.zklighter.elliot.ai"));
        });
    }
    if (card->lighterMainnetButton) {
        connect(card->lighterMainnetButton, &QToolButton::clicked, this, [card, setLighterBaseUrl]() {
            if (card->lighterTestnetButton) {
                card->lighterTestnetButton->setChecked(false);
            }
            card->lighterMainnetButton->setChecked(true);
            setLighterBaseUrl(QStringLiteral("https://mainnet.zklighter.elliot.ai"));
        });
    }

    connect(card->importButton, &QToolButton::clicked, this, [this, card]() {
        if (!card) {
            return;
        }
        const QString baseDir =
            m_store ? m_store->storagePath() : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        const QString cfgDir = baseDir.isEmpty() ? QDir::homePath() : baseDir;
        const QString path = QDir(cfgDir).filePath(QStringLiteral("lighter_api_key_config.json"));
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            appendLogMessage(tr("Lighter import: file not found: %1").arg(path));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject()) {
            appendLogMessage(tr("Lighter import: invalid JSON: %1").arg(path));
            return;
        }
        const QJsonObject root = doc.object();
        const QString baseUrl = root.value(QStringLiteral("baseUrl")).toString().trimmed();
        const int accountIndex = root.value(QStringLiteral("accountIndex")).toInt(0);
        const QJsonObject keysObj = root.value(QStringLiteral("privateKeys")).toObject();
        if (card->baseUrlEdit) {
            card->baseUrlEdit->setText(baseUrl);
        }
        if (card->accountIndexEdit) {
            card->accountIndexEdit->setText(QString::number(accountIndex));
        }
        if (card->apiKeyIndexEdit && !keysObj.isEmpty()) {
            int bestIndex = -1;
            QString bestKey;
            for (const QString &k : keysObj.keys()) {
                bool ok = false;
                const int idx = k.toInt(&ok);
                if (!ok) {
                    continue;
                }
                if (bestIndex < 0 || idx < bestIndex) {
                    bestIndex = idx;
                    bestKey = k;
                }
            }
            if (bestIndex >= 0) {
                card->apiKeyIndexEdit->setText(QString::number(bestIndex));
                const QString priv = keysObj.value(bestKey).toString().trimmed();
                if (card->apiKeyEdit) {
                    card->apiKeyEdit->setText(priv);
                }
            }
        }
        if (card->saveSecretCheck) {
            card->saveSecretCheck->setChecked(true);
        }
        persistCard(*card);
        appendLogMessage(tr("Lighter import: loaded %1").arg(path));
    });

    const bool isUZX =
        (type == ConnectionStore::Profile::UzxSpot || type == ConnectionStore::Profile::UzxSwap);
    card->passphraseEdit->setVisible(isUZX);
    // MEXC futures needs WEB auth token for REST trading endpoints; other profiles don't.
    card->uidEdit->setVisible(isMexcFutures);
    if (card->saveSecretCheck) {
        card->saveSecretCheck->setVisible(true);
    }
    if (card->viewOnlyCheck) {
        card->viewOnlyCheck->setVisible(!isLighter);
    }
    if (card->autoConnectCheck) {
        card->autoConnectCheck->setVisible(true);
        if (isLighter) {
            card->autoConnectCheck->setChecked(true);
        }
    }

    // Visibility is handled in refreshUi() based on Lighter mode.
    if (card->importButton) {
        card->importButton->setVisible(false);
    }
    if (isLighter) {
        card->secretEdit->setVisible(false);
        card->proxyEdit->setVisible(true);
        if (card->proxyCheckButton) {
            card->proxyCheckButton->setVisible(true);
        }
        card->uidEdit->setVisible(false);
        card->apiKeyEdit->setVisible(false);
        card->accountIndexEdit->setVisible(false);
        card->apiKeyIndexEdit->setVisible(false);
        card->baseUrlEdit->setVisible(false);
        if (card->baseUrlEdit && card->baseUrlEdit->text().trimmed().isEmpty()) {
            card->baseUrlEdit->setText(QStringLiteral("https://mainnet.zklighter.elliot.ai"));
        }
    } else {
        card->baseUrlEdit->setVisible(false);
        card->accountIndexEdit->setVisible(false);
        card->apiKeyIndexEdit->setVisible(false);
        if (card->seedPhraseEdit) {
            card->seedPhraseEdit->setVisible(false);
        }
        if (card->importButton) {
            card->importButton->setVisible(false);
        }
    }

    return card;
}

QString ConnectionsWindow::lighterConfigPath() const
{
    const QString baseDir =
        m_store ? m_store->storagePath() : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString cfgDir = baseDir.isEmpty() ? QDir::homePath() : baseDir;
    return QDir(cfgDir).filePath(QStringLiteral("lighter_vault.dat"));
}

void ConnectionsWindow::saveLighterProxyToVaultIfPossible(const MexcCredentials &creds, bool log)
{
    const QString vaultPath = lighterConfigPath();
    if (!QFile::exists(vaultPath)) {
        return;
    }

    QJsonObject root;
    QString err;
    if (!readLighterVaultFile(vaultPath, &root, &err) || root.isEmpty()) {
        if (log) {
            appendLogMessage(tr("Lighter vault: failed to read (%1)").arg(err));
        }
        return;
    }

    const QString proxyRaw = creds.proxy.trimmed();
    const QString proxyType = normalizeProxyTypeForStorage(creds.proxyType);

    const QString existingProxy = root.value(QStringLiteral("proxy")).toString().trimmed();
    const QString existingType = normalizeProxyTypeForStorage(root.value(QStringLiteral("proxyType")).toString());

    bool changed = false;
    if (proxyRaw.isEmpty()) {
        if (!existingProxy.isEmpty() || root.contains(QStringLiteral("proxyType"))) {
            root.remove(QStringLiteral("proxy"));
            root.remove(QStringLiteral("proxyType"));
            changed = true;
        }
    } else {
        if (existingProxy != proxyRaw) {
            root.insert(QStringLiteral("proxy"), proxyRaw);
            changed = true;
        }
        if (existingType != proxyType) {
            root.insert(QStringLiteral("proxyType"), proxyType);
            changed = true;
        }
    }

    if (!changed) {
        return;
    }

    if (!writeLighterVaultFile(vaultPath, root, &err)) {
        if (log) {
            appendLogMessage(tr("Lighter vault: failed to save proxy (%1)").arg(err));
        }
        return;
    }
    if (log) {
        appendLogMessage(tr("Lighter vault: proxy saved"));
    }
}

bool ConnectionsWindow::loadLighterConfigIntoCard(CardWidgets *card, const QString &path, bool log)
{
    if (!card) {
        return false;
    }
    QJsonObject root;
    QString err;
    if (!readLighterVaultFile(path, &root, &err)) {
        // Migration path: if legacy JSON exists, import it and write the encrypted vault.
        const QString legacyJson = QFileInfo(path).dir().filePath(QStringLiteral("lighter_api_key_config.json"));
        QFile legacy(legacyJson);
        if (legacy.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QJsonDocument legacyDoc = QJsonDocument::fromJson(legacy.readAll());
            legacy.close();
            if (legacyDoc.isObject()) {
                root = legacyDoc.object();
                QString werr;
                (void)writeLighterVaultFile(path, root, &werr);
            }
        }
        if (root.isEmpty()) {
            if (log) {
                appendLogMessage(tr("Lighter vault: not found or unreadable."));
            }
            return false;
        }
    } else {
        // If the file was a legacy plain JSON, re-encrypt it in-place.
        QFile raw(path);
        if (raw.open(QIODevice::ReadOnly)) {
            const QByteArray data = raw.readAll();
            raw.close();
            if (!data.startsWith(kLighterVaultMagic)
                && !data.startsWith(kLighterVaultMagicLegacy)) {
                QString werr;
                (void)writeLighterVaultFile(path, root, &werr);
            }
        }
    }

    const QString baseUrl = root.value(QStringLiteral("baseUrl")).toString().trimmed();
    const int accountIndex = root.value(QStringLiteral("accountIndex")).toInt(0);
    const QJsonObject keysObj = root.value(QStringLiteral("privateKeys")).toObject();
    const QString vaultProxy = root.value(QStringLiteral("proxy")).toString().trimmed();
    const QString vaultProxyType = normalizeProxyTypeForStorage(root.value(QStringLiteral("proxyType")).toString());
    if (card->baseUrlEdit) {
        QSignalBlocker b(card->baseUrlEdit);
        card->baseUrlEdit->setText(!baseUrl.isEmpty() ? baseUrl : QStringLiteral("https://mainnet.zklighter.elliot.ai"));
    }
    if (card->accountIndexEdit) {
        QSignalBlocker b(card->accountIndexEdit);
        card->accountIndexEdit->setText(QString::number(accountIndex));
    }
    if (card->apiKeyIndexEdit && !keysObj.isEmpty()) {
        int bestIndex = -1;
        QString bestKey;
        for (const QString &k : keysObj.keys()) {
            bool ok = false;
            const int idx = k.toInt(&ok);
            if (!ok) {
                continue;
            }
            if (bestIndex < 0 || idx < bestIndex) {
                bestIndex = idx;
                bestKey = k;
            }
        }
        if (bestIndex >= 0) {
            {
                QSignalBlocker b(card->apiKeyIndexEdit);
                card->apiKeyIndexEdit->setText(QString::number(bestIndex));
            }
            const QString priv = keysObj.value(bestKey).toString().trimmed();
            if (card->apiKeyEdit) {
                QSignalBlocker b(card->apiKeyEdit);
                card->apiKeyEdit->setText(priv);
            }
        }
    }
    if (card->proxyEdit && !vaultProxy.isEmpty()) {
        QSignalBlocker b(card->proxyEdit);
        card->proxyEdit->setText(vaultProxy);
    }
    if (card->proxyTypeCombo && !vaultProxy.isEmpty()) {
        const int idx = card->proxyTypeCombo->findData(vaultProxyType);
        QSignalBlocker b(card->proxyTypeCombo);
        if (idx >= 0) {
            card->proxyTypeCombo->setCurrentIndex(idx);
        }
    }
    if (card->lighterConfigPathEdit) {
        card->lighterConfigPathEdit->setText(tr("Lighter vault (encrypted)"));
    }
    persistCard(*card);
    if (log) {
        appendLogMessage(tr("Lighter vault: loaded"));
    }
    return true;
}

void ConnectionsWindow::openLighterConfigEditor(CardWidgets *card)
{
    if (!card) {
        return;
    }
    const QString vaultPath = lighterConfigPath();

    QJsonObject existing;
    QString readErr;
    (void)readLighterVaultFile(vaultPath, &existing, &readErr);
    if (existing.isEmpty()) {
        // Try legacy JSON once for convenience.
        const QString legacyJson = QFileInfo(vaultPath).dir().filePath(QStringLiteral("lighter_api_key_config.json"));
        QFile legacy(legacyJson);
        if (legacy.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QJsonDocument legacyDoc = QJsonDocument::fromJson(legacy.readAll());
            legacy.close();
            if (legacyDoc.isObject()) {
                existing = legacyDoc.object();
            }
        }
    }

    int existingAccountIndex = existing.value(QStringLiteral("accountIndex")).toInt(0);
    int existingApiKeyIndex = -1;
    QString existingPriv;
    const QString existingProxy = existing.value(QStringLiteral("proxy")).toString().trimmed();
    const QString existingProxyType = normalizeProxyTypeForStorage(existing.value(QStringLiteral("proxyType")).toString());
    const QJsonObject keysObj = existing.value(QStringLiteral("privateKeys")).toObject();
    if (!keysObj.isEmpty()) {
        int bestIndex = -1;
        QString bestKey;
        for (const QString &k : keysObj.keys()) {
            bool ok = false;
            const int idx = k.toInt(&ok);
            if (!ok) continue;
            if (bestIndex < 0 || idx < bestIndex) {
                bestIndex = idx;
                bestKey = k;
            }
        }
        if (bestIndex >= 0) {
            existingApiKeyIndex = bestIndex;
            existingPriv = keysObj.value(bestKey).toString().trimmed();
        }
    }

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Lighter vault"));
    dlg->setModal(true);
    dlg->resize(560, 340);

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    auto *hint = new QLabel(tr("Config is stored locally and encrypted for this Windows user."), dlg);
    hint->setStyleSheet(QStringLiteral("color:#cfd7e4;"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto *box = new QGroupBox(tr("Keys"), dlg);
    auto *form = new QFormLayout(box);

    auto *accountIndexEdit = new QLineEdit(box);
    accountIndexEdit->setPlaceholderText(tr("e.g. 692716"));
    accountIndexEdit->setText(existingAccountIndex > 0 ? QString::number(existingAccountIndex) : QString());

    auto *apiKeyIndexEdit = new QLineEdit(box);
    apiKeyIndexEdit->setPlaceholderText(tr("0..253"));
    apiKeyIndexEdit->setText(existingApiKeyIndex >= 0 ? QString::number(existingApiKeyIndex) : QString());

    auto *apiPrivEdit = new QLineEdit(box);
    apiPrivEdit->setEchoMode(QLineEdit::Password);
    apiPrivEdit->setPlaceholderText(tr("hex private key"));
    apiPrivEdit->setText(existingPriv);

    auto *proxyTypeCombo = new QComboBox(box);
    proxyTypeCombo->addItem(QStringLiteral("HTTP"), QStringLiteral("http"));
    proxyTypeCombo->addItem(QStringLiteral("SOCKS5"), QStringLiteral("socks5"));
    const QString initialType = normalizeProxyTypeForStorage(!card->proxyTypeCombo ? existingProxyType
                                                                                   : card->proxyTypeCombo->currentData().toString());
    const int typeIdx = proxyTypeCombo->findData(initialType);
    if (typeIdx >= 0) {
        proxyTypeCombo->setCurrentIndex(typeIdx);
    }

    auto *proxyEdit = new QLineEdit(box);
    proxyEdit->setPlaceholderText(tr("Proxy (host:port or user:pass@host:port)"));
    const QString initialProxy =
        (card->proxyEdit && !card->proxyEdit->text().trimmed().isEmpty()) ? card->proxyEdit->text().trimmed() : existingProxy;
    proxyEdit->setText(initialProxy);

    form->addRow(tr("Account index:"), accountIndexEdit);
    form->addRow(tr("API key index:"), apiKeyIndexEdit);
    form->addRow(tr("API private key:"), apiPrivEdit);
    form->addRow(tr("Proxy type:"), proxyTypeCombo);
    form->addRow(tr("Proxy:"), proxyEdit);
    root->addWidget(box);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    auto *saveBtn = new QPushButton(tr("Save"), dlg);
    buttons->addButton(saveBtn, QDialogButtonBox::ActionRole);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, dlg, [this, dlg, card, vaultPath, accountIndexEdit, apiKeyIndexEdit, apiPrivEdit, proxyEdit, proxyTypeCombo]() {
        bool okAi = false;
        const int accountIndex = accountIndexEdit->text().trimmed().toInt(&okAi);
        bool okIdx = false;
        const int apiKeyIndex = apiKeyIndexEdit->text().trimmed().toInt(&okIdx);
        const QString priv = apiPrivEdit->text().trimmed();
        if (!okAi || accountIndex < 0) {
            QMessageBox::warning(dlg, tr("Lighter vault"), tr("Invalid accountIndex."));
            return;
        }
        if (!okIdx || apiKeyIndex < 0 || apiKeyIndex > 253) {
            QMessageBox::warning(dlg, tr("Lighter vault"), tr("Invalid apiKeyIndex (0..253)."));
            return;
        }
        if (priv.isEmpty()) {
            QMessageBox::warning(dlg, tr("Lighter vault"), tr("API private key is required."));
            return;
        }

        QJsonObject rootObj;
        rootObj.insert(QStringLiteral("baseUrl"), QStringLiteral("https://mainnet.zklighter.elliot.ai"));
        rootObj.insert(QStringLiteral("accountIndex"), accountIndex);
        QJsonObject keys;
        keys.insert(QString::number(apiKeyIndex), priv);
        rootObj.insert(QStringLiteral("privateKeys"), keys);

        const QString proxyRaw = proxyEdit ? proxyEdit->text().trimmed() : QString();
        if (!proxyRaw.isEmpty()) {
            rootObj.insert(QStringLiteral("proxy"), proxyRaw);
            rootObj.insert(QStringLiteral("proxyType"),
                           normalizeProxyTypeForStorage(proxyTypeCombo ? proxyTypeCombo->currentData().toString()
                                                                       : QStringLiteral("http")));
        } else {
            rootObj.remove(QStringLiteral("proxy"));
            rootObj.remove(QStringLiteral("proxyType"));
        }

        QString err;
        if (!writeLighterVaultFile(vaultPath, rootObj, &err)) {
            QMessageBox::warning(dlg, tr("Lighter vault"), tr("Failed to save vault."));
            return;
        }
        loadLighterConfigIntoCard(card, vaultPath, true);
        refreshUi();
        dlg->accept();
    });

    dlg->exec();
    dlg->deleteLater();
}

void ConnectionsWindow::rebuildCardsFromStore()
{
    if (!m_cardsLayout) {
        return;
    }

    for (auto *card : m_cards) {
        if (!card) {
            continue;
        }
        if (card->frame) {
            card->frame->deleteLater();
        }
        delete card;
    }
    m_cards.clear();

    if (m_store) {
        QVector<ConnectionStore::StoredProfile> profiles = m_store->allProfiles();
        if (profiles.isEmpty()) {
            m_store->ensureProfilesSchema();
            profiles = m_store->allProfiles();
        }
        for (const auto &p : profiles) {
            createCard(p.id, p.type);
        }
    } else {
        createCard(QStringLiteral("mexcSpot:default"), ConnectionStore::Profile::MexcSpot);
        createCard(QStringLiteral("mexcFutures:default"), ConnectionStore::Profile::MexcFutures);
        createCard(QStringLiteral("lighter:default"), ConnectionStore::Profile::Lighter);
        createCard(QStringLiteral("binanceSpot:default"), ConnectionStore::Profile::BinanceSpot);
        createCard(QStringLiteral("binanceFutures:default"), ConnectionStore::Profile::BinanceFutures);
        createCard(QStringLiteral("uzxSwap:default"), ConnectionStore::Profile::UzxSwap);
        createCard(QStringLiteral("uzxSpot:default"), ConnectionStore::Profile::UzxSpot);
    }

    rebuildLayout();
}

void ConnectionsWindow::deleteCard(CardWidgets *card)
{
    if (!card) {
        return;
    }
    if (!m_store) {
        return;
    }
    const QString label = (card->nameLabel && !card->nameLabel->text().trimmed().isEmpty())
                              ? card->nameLabel->text().trimmed()
                              : card->profileId;

    const auto reply = QMessageBox::question(this,
                                             tr("Удалить профиль"),
                                             tr("Удалить профиль \"%1\"?").arg(label),
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }

    const QString activeId = m_store->activeProfileId(card->type);
    const bool wasActive = (activeId == card->profileId);
    if (wasActive && m_manager) {
        m_manager->disconnect(card->type);
    }
    m_store->deleteProfile(card->profileId);
    rebuildCardsFromStore();
    refreshUi();
}

ConnectionsWindow::CardWidgets *ConnectionsWindow::ensureCard(const QString &profileId, ConnectionStore::Profile type)
{
    for (auto *c : m_cards) {
        if (c->profileId == profileId) {
            return c;
        }
    }
    return createCard(profileId, type);
}

void ConnectionsWindow::refreshUi()
{
    for (auto *card : m_cards) {
        if (!card) {
            continue;
        }

        MexcCredentials creds;
        if (m_store) {
            creds = m_store->profileById(card->profileId).creds;
        } else {
            creds = MexcCredentials{};
        }
        if (creds.colorHex.isEmpty()) {
            creds.colorHex = defaultColorForProfile(card->type);
        }

        const bool isUZX =
            (card->type == ConnectionStore::Profile::UzxSpot || card->type == ConnectionStore::Profile::UzxSwap);
        const bool isMexcFutures = (card->type == ConnectionStore::Profile::MexcFutures);
        const bool isLighter = (card->type == ConnectionStore::Profile::Lighter);

        const QString activeId = m_store ? m_store->activeProfileId(card->type) : QString();
        const bool isActive = !m_store || activeId.isEmpty() || (activeId == card->profileId);

        const QString storedLabel = creds.label.trimmed();
        const QString typeKey = idFromProfile(card->type);
        const QString displayName =
            (!storedLabel.isEmpty() && storedLabel != typeKey) ? storedLabel : exchangeTitle(card->type);
        if (card->nameLabel) {
            card->nameLabel->setText(displayName);
        }
        if (card->exchangeLabel) {
            card->exchangeLabel->setText(isActive ? tr("%1 • активный").arg(exchangeTitle(card->type))
                                                  : tr("%1 • не активный").arg(exchangeTitle(card->type)));
        }
        if (card->profileNameEdit) {
            QSignalBlocker b(card->profileNameEdit);
            card->profileNameEdit->setText(displayName);
        }

        card->passphraseEdit->setVisible(isUZX);
        card->uidEdit->setVisible(isMexcFutures && !isLighter);
        card->secretEdit->setVisible(!isLighter);
        card->proxyEdit->setVisible(true);
        if (card->proxyCheckButton) {
            card->proxyCheckButton->setVisible(true);
        }

        if (isLighter) {
            // If proxy is stored only in the encrypted vault, show it in the UI and
            // migrate it into connections.json so it persists visibly across restarts.
            if (creds.proxy.trimmed().isEmpty()) {
                const QString vaultPath = lighterConfigPath();
                if (QFile::exists(vaultPath)) {
                    QJsonObject vault;
                    QString verr;
                    if (readLighterVaultFile(vaultPath, &vault, &verr) && !vault.isEmpty()) {
                        const QString vaultProxy = vault.value(QStringLiteral("proxy")).toString().trimmed();
                        if (!vaultProxy.isEmpty()) {
                            creds.proxy = vaultProxy;
                            creds.proxyType = normalizeProxyTypeForStorage(
                                vault.value(QStringLiteral("proxyType")).toString());

                            if (m_store) {
                                ConnectionStore::StoredProfile p = m_store->profileById(card->profileId);
                                if (p.type == ConnectionStore::Profile::Lighter
                                    && p.creds.proxy.trimmed().isEmpty()) {
                                    p.creds.proxy = creds.proxy;
                                    p.creds.proxyType = creds.proxyType;
                                    stripSecretsForStorage(ConnectionStore::Profile::Lighter, p.creds);
                                    m_store->saveProfile(p);
                                }
                            }
                        }
                    }
                }
            }

            if (m_store) {
                const bool hasStoredSecrets = !creds.apiKey.trimmed().isEmpty()
                                              || !creds.secretKey.trimmed().isEmpty()
                                              || !creds.seedPhrase.trimmed().isEmpty()
                                              || creds.saveSecret;
                if (hasStoredSecrets) {
                    ConnectionStore::StoredProfile p = m_store->profileById(card->profileId);
                    stripSecretsForStorage(ConnectionStore::Profile::Lighter, p.creds);
                    m_store->saveProfile(p);
                    creds = p.creds;
                }
            }
            if (card->lighterMainnetButton) {
                QSignalBlocker b(card->lighterMainnetButton);
                card->lighterMainnetButton->setVisible(false);
            }
            if (card->lighterTestnetButton) {
                QSignalBlocker b(card->lighterTestnetButton);
                card->lighterTestnetButton->setVisible(false);
            }
            // Default (and only) mode: connect using API private key + indices.
            if (creds.preferSeedPhrase) {
                creds.preferSeedPhrase = false;
                if (m_store) {
                    ConnectionStore::StoredProfile p = m_store->profileById(card->profileId);
                    p.creds.preferSeedPhrase = false;
                    m_store->saveProfile(p);
                }
            }
            if (card->lighterConfigPathEdit) {
                card->lighterConfigPathEdit->setVisible(true);
                card->lighterConfigPathEdit->setText(tr("Lighter vault (encrypted)"));
            }
            card->apiKeyEdit->setVisible(false);
            card->accountIndexEdit->setVisible(false);
            card->apiKeyIndexEdit->setVisible(false);
            card->baseUrlEdit->setVisible(false);
            if (card->seedPhraseEdit) {
                card->seedPhraseEdit->setVisible(false);
            }
        } else {
            if (card->lighterMainnetButton) {
                card->lighterMainnetButton->setVisible(false);
            }
            if (card->lighterTestnetButton) {
                card->lighterTestnetButton->setVisible(false);
            }
            card->apiKeyEdit->setVisible(true);
            card->baseUrlEdit->setVisible(false);
            card->accountIndexEdit->setVisible(false);
            card->apiKeyIndexEdit->setVisible(false);
            if (card->seedPhraseEdit) {
                card->seedPhraseEdit->setVisible(false);
            }
            if (card->lighterConfigPathEdit) {
                card->lighterConfigPathEdit->setVisible(false);
            }
        }

        card->saveSecretCheck->setVisible(!isLighter);
        card->viewOnlyCheck->setVisible(!isLighter);
        card->autoConnectCheck->setVisible(true);
        if (card->importButton) {
            card->importButton->setVisible(false);
        }

        if (card->apiKeyEdit) {
            QSignalBlocker b(card->apiKeyEdit);
            card->apiKeyEdit->setText(creds.apiKey);
        }
        if (card->secretEdit) {
            QSignalBlocker b(card->secretEdit);
            card->secretEdit->setText(creds.saveSecret ? creds.secretKey : QString());
        }
        if (card->passphraseEdit) {
            QSignalBlocker b(card->passphraseEdit);
            card->passphraseEdit->setText(isUZX ? creds.passphrase : QString());
        }
        if (card->uidEdit) {
            QSignalBlocker b(card->uidEdit);
            card->uidEdit->setText(isMexcFutures ? creds.uid : QString());
        }
        if (card->proxyEdit) {
            QSignalBlocker b(card->proxyEdit);
            card->proxyEdit->setText(creds.proxy);
        }
        if (card->proxyTypeCombo) {
            QString t = creds.proxyType.trimmed().isEmpty() ? QStringLiteral("http") : creds.proxyType.trimmed().toLower();
            if (t == QStringLiteral("https")) {
                t = QStringLiteral("http");
            }
            const int idx = card->proxyTypeCombo->findData(t);
            QSignalBlocker b(card->proxyTypeCombo);
            if (idx >= 0) {
                card->proxyTypeCombo->setCurrentIndex(idx);
            } else {
                card->proxyTypeCombo->setCurrentIndex(0);
            }
        }
        if (card->baseUrlEdit) {
            QSignalBlocker b(card->baseUrlEdit);
            card->baseUrlEdit->setText(creds.baseUrl);
        }
        if (card->accountIndexEdit) {
            QSignalBlocker b(card->accountIndexEdit);
            card->accountIndexEdit->setText(QString::number(creds.accountIndex));
        }
        if (card->apiKeyIndexEdit) {
            QSignalBlocker b(card->apiKeyIndexEdit);
            card->apiKeyIndexEdit->setText(creds.apiKeyIndex >= 0 ? QString::number(creds.apiKeyIndex) : QString());
        }
        if (card->seedPhraseEdit) {
            QSignalBlocker b(card->seedPhraseEdit);
            card->seedPhraseEdit->setText(creds.saveSecret ? creds.seedPhrase : QString());
        }

        if (card->saveSecretCheck) {
            QSignalBlocker b(card->saveSecretCheck);
            card->saveSecretCheck->setChecked(creds.saveSecret);
        }
        if (card->viewOnlyCheck) {
            QSignalBlocker b(card->viewOnlyCheck);
            card->viewOnlyCheck->setChecked(creds.viewOnly);
        }
        if (card->autoConnectCheck) {
            QSignalBlocker b(card->autoConnectCheck);
            bool autoConnect = creds.autoConnect;
            if (isLighter && !autoConnect) {
                autoConnect = true;
                if (m_store) {
                    ConnectionStore::StoredProfile p = m_store->profileById(card->profileId);
                    p.creds.autoConnect = true;
                    m_store->saveProfile(p);
                }
            }
            card->autoConnectCheck->setChecked(autoConnect);
        }

        card->color = QColor(creds.colorHex);
        styleColorButton(card->colorButton, card->color);

        setCardExpanded(card, card->expanded);
    }

    auto applyProfileState = [&](ConnectionStore::Profile profile) {
        const auto state = m_manager ? m_manager->state(profile) : TradeManager::ConnectionState::Disconnected;
        applyState(profile, state, QString());
    };
    applyProfileState(ConnectionStore::Profile::MexcSpot);
    applyProfileState(ConnectionStore::Profile::MexcFutures);
    applyProfileState(ConnectionStore::Profile::UzxSwap);
    applyProfileState(ConnectionStore::Profile::UzxSpot);
    applyProfileState(ConnectionStore::Profile::Lighter);
    applyProfileState(ConnectionStore::Profile::BinanceSpot);
    applyProfileState(ConnectionStore::Profile::BinanceFutures);
    rebuildLayout();
}

void ConnectionsWindow::handleManagerStateChanged(ConnectionStore::Profile profile,
                                                  TradeManager::ConnectionState state,
                                                  const QString &message)
{
    applyState(profile, state, message);
    if (!message.isEmpty()) {
        appendLogMessage(message);
    }
}

void ConnectionsWindow::closeEvent(QCloseEvent *event)
{
    m_dragging = false;
    while (QGuiApplication::overrideCursor()) {
        QGuiApplication::restoreOverrideCursor();
    }
    QDialog::closeEvent(event);
}

void ConnectionsWindow::updateTitleCounts()
{
    if (!m_titleLabel) {
        return;
    }
    int total = 0;
    int active = 0;
    for (auto *c : m_cards) {
        if (!c) {
            continue;
        }
        ++total;
        if (c->currentState == TradeManager::ConnectionState::Connected) {
            ++active;
        }
    }
    m_titleLabel->setText(QStringLiteral("Active: %1 / Total: %2").arg(active).arg(total));
}

void ConnectionsWindow::appendLogMessage(const QString &message)
{
    if (!m_logView) {
        return;
    }
    const QString line = QStringLiteral("[%1] %2")
                             .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")))
                             .arg(message);
    m_logView->appendPlainText(line);
    QTextCursor cursor = m_logView->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_logView->setTextCursor(cursor);
}

void ConnectionsWindow::handleConnectClicked(const QString &profileId)
{
    CardWidgets *card = nullptr;
    for (auto *c : m_cards) {
        if (c && c->profileId == profileId) {
            card = c;
            break;
        }
    }
    if (!card || !m_manager) {
        return;
    }
    MexcCredentials creds = collectCredentials(*card);
    const auto profile = card->type;

    if (profile == ConnectionStore::Profile::Lighter) {
        saveLighterProxyToVaultIfPossible(creds, false);
        if (creds.apiKeyIndex < 0 || creds.apiKey.trimmed().isEmpty() || creds.accountIndex < 0) {
            if (loadLighterConfigIntoCard(card, lighterConfigPath(), false)) {
                creds = collectCredentials(*card);
            }
        }
        if (creds.apiKeyIndex < 0 || creds.apiKey.trimmed().isEmpty()) {
            appendLogMessage(tr("Lighter: enter apiKeyIndex and API private key. Use the 'Create API' icon if you need to generate keys."));
            applyState(profile, TradeManager::ConnectionState::Error, QString());
            return;
        }

    }

    if (m_store) {
        m_store->setActiveProfileId(profile, profileId);
        ConnectionStore::StoredProfile p;
        p.id = profileId;
        p.type = profile;
        p.creds = creds;
        stripSecretsForStorage(profile, p.creds);
        m_store->saveProfile(p);
    }
    m_manager->setCredentials(profile, creds);
    applyState(profile, TradeManager::ConnectionState::Connecting, QString());
    m_manager->connectToExchange(profile);
}

void ConnectionsWindow::handleDisconnectClicked(const QString &profileId)
{
    if (!m_manager) {
        return;
    }
    CardWidgets *card = nullptr;
    for (auto *c : m_cards) {
        if (c && c->profileId == profileId) {
            card = c;
            break;
        }
    }
    if (!card) {
        return;
    }
    const auto profile = card->type;
    const QString activeId = m_store ? m_store->activeProfileId(profile) : QString();
    if (m_store && !activeId.isEmpty() && activeId != profileId) {
        // Disconnect only applies to the active profile for this exchange type.
        return;
    }
    m_manager->disconnect(profile);
    applyState(profile, TradeManager::ConnectionState::Disconnected, QString());
}

void ConnectionsWindow::applyState(ConnectionStore::Profile profile,
                                   TradeManager::ConnectionState state,
                                   const QString &message)
{
    Q_UNUSED(message);
    QString activeId = m_store ? m_store->activeProfileId(profile) : QString();
    if (activeId.isEmpty()) {
        for (auto *c : m_cards) {
            if (c && c->type == profile) {
                activeId = c->profileId;
                break;
            }
        }
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool delayQuickConnect = (profile == ConnectionStore::Profile::Lighter);

    for (auto *card : m_cards) {
        if (!card || card->type != profile) {
            continue;
        }
        const bool isActive = activeId.isEmpty() || card->profileId == activeId;
        if (!isActive) {
            card->currentState = TradeManager::ConnectionState::Disconnected;
            if (card->statusBadge) {
                card->statusBadge->setText(tr("Не активно"));
                card->statusBadge->setStyleSheet(badgeStyle(statusColor(TradeManager::ConnectionState::Disconnected)));
                card->statusBadge->setText(QStringLiteral("inactive"));
                card->statusBadge->setStyleSheet(QStringLiteral("color:#d2a84a; padding:0 6px;"));
            }
            if (card->statusDot) {
                static_cast<StatusDot *>(card->statusDot)->setStatus(TradeManager::ConnectionState::Disconnected, false);
            }
            if (card->connectButton) {
                card->connectButton->setText(tr("Подключить"));
                card->connectButton->setEnabled(true);
                card->connectButton->setToolTip(tr("Сделать профиль активным и подключиться"));
            }
            if (card->disconnectButton) {
                card->disconnectButton->setEnabled(false);
                card->disconnectButton->setVisible(false);
            }
            continue;
        }

        if (state == TradeManager::ConnectionState::Connecting) {
            card->lastConnectingAtMs = nowMs;
            card->pendingConnectedApply = false;
        } else if (state == TradeManager::ConnectionState::Connected) {
            const bool shouldDelay =
                delayQuickConnect && card->currentState == TradeManager::ConnectionState::Connecting && !card->pendingConnectedApply &&
                card->lastConnectingAtMs > 0 && (nowMs - card->lastConnectingAtMs) < 450;
            if (shouldDelay) {
                card->pendingConnectedApply = true;
                const int delayMs = int(450 - (nowMs - card->lastConnectingAtMs));
                QTimer::singleShot(delayMs, this, [this, profile, message]() {
                    if (!m_manager) {
                        return;
                    }
                    if (m_manager->state(profile) != TradeManager::ConnectionState::Connected) {
                        return;
                    }
                    applyState(profile, TradeManager::ConnectionState::Connected, message);
                });
                continue;
            }
        }

        card->currentState = state;
        card->pendingConnectedApply = false;
        const QString color = statusColor(state);
        QString text;
        switch (state) {
        case TradeManager::ConnectionState::Connected:
            text = tr("Подключено");
            break;
        case TradeManager::ConnectionState::Connecting:
            text = tr("Подключение...");
            break;
        case TradeManager::ConnectionState::Error:
            text = tr("Ошибка");
            break;
        case TradeManager::ConnectionState::Disconnected:
        default:
            text = tr("Отключено");
            break;
        }
        if (card->statusBadge) {
            card->statusBadge->setText(text);
            card->statusBadge->setStyleSheet(badgeStyle(color));
        }
        if (card->statusDot) {
            static_cast<StatusDot *>(card->statusDot)->setStatus(state, true);
        }
        if (card->statusBadge) {
            QString sText;
            QString sStyle = QStringLiteral("color:#b7beca; padding:0 6px;");
            switch (state) {
            case TradeManager::ConnectionState::Connected:
                sText = QStringLiteral("online");
                sStyle = QStringLiteral("color:#34c759; padding:0 6px;");
                break;
            case TradeManager::ConnectionState::Connecting:
                sText = QStringLiteral("connecting");
                sStyle = QStringLiteral("color:#d3d9e4; padding:0 6px;");
                break;
            case TradeManager::ConnectionState::Error:
                sText = QStringLiteral("error");
                sStyle = QStringLiteral("color:#ff453a; padding:0 6px;");
                break;
            case TradeManager::ConnectionState::Disconnected:
            default:
                sText = QStringLiteral("offline");
                sStyle = QStringLiteral("color:#8b93a1; padding:0 6px;");
                break;
            }
            card->statusBadge->setText(sText);
            card->statusBadge->setStyleSheet(sStyle);
        }

        if (card->connectButton) {
            const bool disconnectMode =
                (state == TradeManager::ConnectionState::Connected || state == TradeManager::ConnectionState::Connecting);
            card->connectButton->setText(disconnectMode ? tr("Отключить") : tr("Подключить"));
            card->connectButton->setEnabled(true);
            card->connectButton->setToolTip(QString());
        }
        if (card->disconnectButton) {
            card->disconnectButton->setEnabled(false);
            card->disconnectButton->setVisible(false);
        }
    }

    updateTitleCounts();
}

MexcCredentials ConnectionsWindow::collectCredentials(const CardWidgets &card) const
{
    const bool isLighter = (card.type == ConnectionStore::Profile::Lighter);
    MexcCredentials creds = m_store ? m_store->profileById(card.profileId).creds : MexcCredentials{};
    if (isLighter) {
        creds.preferSeedPhrase = false;
    }

    if (card.apiKeyEdit) {
        const QString typed = card.apiKeyEdit->text().trimmed();
        // API key is stored encrypted; don't wipe it accidentally when switching modes.
        if (!typed.isEmpty()) {
            creds.apiKey = typed;
        }
    }
    if (card.secretEdit) {
        creds.secretKey = card.secretEdit->text().trimmed();
    }

    const bool isUZX =
        (card.type == ConnectionStore::Profile::UzxSpot || card.type == ConnectionStore::Profile::UzxSwap);
    creds.passphrase = isUZX && card.passphraseEdit ? card.passphraseEdit->text().trimmed() : QString();
    creds.uid =
        (card.type == ConnectionStore::Profile::MexcFutures && card.uidEdit) ? card.uidEdit->text().trimmed() : QString();
    creds.proxy = card.proxyEdit ? card.proxyEdit->text().trimmed() : QString();
    if (card.proxyTypeCombo) {
        creds.proxyType = card.proxyTypeCombo->currentData().toString();
    }

    if (isLighter) {
        creds.baseUrl = QStringLiteral("https://mainnet.zklighter.elliot.ai");
        // These can be populated by setup even while the UI hides them in seed mode.
        if (card.accountIndexEdit) {
            bool ok = false;
            const int ai = card.accountIndexEdit->text().trimmed().toInt(&ok);
            if (ok) {
                creds.accountIndex = ai;
            }
        }
        if (card.apiKeyIndexEdit) {
            bool ok = false;
            const int idx = card.apiKeyIndexEdit->text().trimmed().toInt(&ok);
            if (ok) {
                creds.apiKeyIndex = idx;
            }
        }
        // Seed phrase is used only by the setup dialog; it is not required for connecting.
    }

    creds.colorHex = card.color.isValid() ? card.color.name(QColor::HexRgb) : defaultColorForProfile(card.type);
    const QString label = card.profileNameEdit ? card.profileNameEdit->text().trimmed() : QString();
    creds.label = !label.isEmpty() ? label : exchangeTitle(card.type);
    creds.saveSecret = card.saveSecretCheck ? card.saveSecretCheck->isChecked() : creds.saveSecret;
    creds.viewOnly = card.viewOnlyCheck ? card.viewOnlyCheck->isChecked() : creds.viewOnly;
    creds.autoConnect = card.autoConnectCheck ? card.autoConnectCheck->isChecked() : creds.autoConnect;
    return creds;
}

static void stripSecretsForStorage(ConnectionStore::Profile profile, MexcCredentials &creds)
{
    if (profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    creds.apiKey.clear();
    creds.secretKey.clear();
    creds.seedPhrase.clear();
    creds.saveSecret = false;
}

void ConnectionsWindow::persistCard(const CardWidgets &card)
{
    if (!m_store) {
        return;
    }
    ConnectionStore::StoredProfile p;
    p.id = card.profileId;
    p.type = card.type;
    p.creds = collectCredentials(card);
    stripSecretsForStorage(p.type, p.creds);
    m_store->saveProfile(p);
}

void ConnectionsWindow::setCardExpanded(CardWidgets *card, bool expanded)
{
    if (!card || !card->body || !card->expandButton) {
        return;
    }
    auto setExpandIcon = [](QToolButton *btn, bool isExpanded) {
        if (!btn) {
            return;
        }
        const QVariant v = btn->property(isExpanded ? "iconExpanded" : "iconCollapsed");
        if (v.isValid()) {
            btn->setIcon(v.value<QIcon>());
        }
    };
    if (expanded) {
        for (auto *other : m_cards) {
            if (!other || other == card) {
                continue;
            }
            if (other->body) {
                other->body->setVisible(false);
            }
            other->expanded = false;
            if (other->expandButton) {
                other->expandButton->setChecked(false);
                setExpandIcon(other->expandButton, false);
            }
        }
    }
    card->expanded = expanded;
    card->body->setVisible(expanded);
    card->expandButton->setChecked(expanded);
    setExpandIcon(card->expandButton, expanded);
}

void ConnectionsWindow::moveCard(CardWidgets *card, int delta)
{
    if (!card) {
        return;
    }
    if (m_store) {
        m_store->moveProfile(card->profileId, delta);
        rebuildCardsFromStore();
        refreshUi();
        return;
    }
    const int idx = m_cards.indexOf(card);
    if (idx < 0) {
        return;
    }
    const int newIdx = idx + delta;
    if (newIdx < 0 || newIdx >= m_cards.size()) {
        return;
    }
    m_cards.move(idx, newIdx);
    rebuildLayout();
}

void ConnectionsWindow::rebuildLayout()
{
    if (!m_cardsLayout) return;
    QLayoutItem *child;
    while ((child = m_cardsLayout->takeAt(0)) != nullptr) {
        if (child->widget()) {
            child->widget()->setParent(nullptr);
        }
        delete child;
    }
    for (auto *card : m_cards) {
        if (card && card->frame) {
            m_cardsLayout->addWidget(card->frame);
        }
    }
    m_cardsLayout->addStretch(1);
}

QString ConnectionsWindow::writeLighterWizardScript(const QString &dirPath)
{
    QDir().mkpath(dirPath);
    const QString path = QDir(dirPath).filePath(QStringLiteral("lighter_setup_wizard.py"));

    const QByteArray script =
        QByteArrayLiteral(R"PY(import asyncio
import argparse
import json
import os
import sys
import time

import faulthandler
faulthandler.enable()
faulthandler.dump_traceback_later(15, repeat=True)

print("Wizard booting...")
sys.stdout.flush()

# Work around rare Windows asyncio ProactorEventLoop hangs (socketpair fallback / accept).
if sys.platform.startswith("win"):
    try:
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
        print("Using WindowsSelectorEventLoopPolicy")
        sys.stdout.flush()
    except Exception:
        pass

print("Importing aiohttp...")
sys.stdout.flush()
import aiohttp
print("Imported aiohttp")
sys.stdout.flush()

print("Importing eth_account...")
sys.stdout.flush()
import eth_account
print("Imported eth_account")
sys.stdout.flush()

print("Importing lighter...")
sys.stdout.flush()
import lighter
print("Imported lighter")
sys.stdout.flush()
from eth_account.messages import encode_defunct
from urllib.parse import urlparse

try:
    print("Importing aiohttp_socks...")
    sys.stdout.flush()
    from aiohttp_socks import ProxyConnector
    print("Imported aiohttp_socks")
    sys.stdout.flush()
except Exception:
    ProxyConnector = None


def _is_socks_proxy(proxy_url: str | None) -> bool:
    if not proxy_url:
        return False
    u = proxy_url.lower()
    return u.startswith("socks5://") or u.startswith("socks://")


def _chain_id_from_url(url: str) -> int:
    return 304 if "mainnet" in url.lower() else 300


async def _make_session(proxy_url: str | None):
    if _is_socks_proxy(proxy_url):
        if ProxyConnector is None:
            raise RuntimeError("aiohttp-socks is required for SOCKS proxy support")
        connector = ProxyConnector.from_url(proxy_url)
        return aiohttp.ClientSession(connector=connector), None
    return aiohttp.ClientSession(), proxy_url


async def _get_json(session: aiohttp.ClientSession, url: str, params: dict | None, http_proxy_url: str | None):
    kwargs = {"params": params, "timeout": 20}
    if http_proxy_url:
        kwargs["proxy"] = http_proxy_url
    async with session.get(url, **kwargs) as r:
        text = await r.text()
        if r.status != 200:
            raise RuntimeError(f"HTTP {r.status}: {text}")
        return await r.json()


async def _post_send_tx(session: aiohttp.ClientSession, base_url: str, tx_type: int, tx_info: str, http_proxy_url: str | None):
    form = aiohttp.FormData()
    # Lighter API expects multipart fields named `tx_type` and `tx_info` (see openapi `ReqSendTx`).
    form.add_field("tx_type", str(int(tx_type)))
    form.add_field("tx_info", tx_info)
    form.add_field("price_protection", "true")
    url = base_url.rstrip("/") + "/api/v1/sendTx"
    kwargs = {"data": form, "timeout": 30}
    if http_proxy_url:
        kwargs["proxy"] = http_proxy_url
    async with session.post(url, **kwargs) as r:
        text = await r.text()
        if r.status != 200:
            raise RuntimeError(f"HTTP {r.status}: {text}")
        return await r.json()


def prompt(msg: str, default: str | None = None, secret: bool = False) -> str:
    if default is not None and default != "":
        msg = f"{msg} [{default}]"
    msg = msg + ": "
    v = input(msg).strip()
    if not v and default is not None:
        return default
    return v


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("--out", default="./lighter_api_key_config.json")
    p.add_argument("--base-url", default=None)
    p.add_argument("--eth-private-key", default=None)
    p.add_argument("--eth-private-key-env", default=None)
    p.add_argument("--mnemonic-env", default=None)
    p.add_argument("--address-index", type=int, default=0)
    p.add_argument("--api-key-index", type=int, default=None)
    p.add_argument("--num-api-keys", type=int, default=None)
    p.add_argument("--account-index", type=int, default=None)
    args = p.parse_args()

    out_path = args.out

    print("Lighter setup wizard")
    print("- testnet = fake funds, safe to learn")
    print("- mainnet = real funds")
    print("")

    base_url = args.base_url or prompt("Base URL", "https://testnet.zklighter.elliot.ai")

    proxy_url = os.environ.get("HTTPS_PROXY") or os.environ.get("HTTP_PROXY") or os.environ.get("ALL_PROXY")
    if proxy_url:
        try:
            p = urlparse(proxy_url)
            if p.hostname and p.port:
                auth = " (auth)" if p.username else ""
                print(f"Using proxy: {p.scheme}://{p.hostname}:{p.port}{auth}")
            else:
                print("Using proxy: (configured)")
        except Exception:
            print("Using proxy: (configured)")

    eth_private_key = None
    mnemonic = None
    if args.mnemonic_env:
        mnemonic = os.environ.get(args.mnemonic_env)
    if mnemonic:
        eth_account.Account.enable_unaudited_hdwallet_features()
        acct = eth_account.Account.from_mnemonic(
            mnemonic, account_path=f"m/44'/60'/0'/0/{int(args.address_index)}"
        )
        eth_private_key = acct.key.hex()
        eth_address = acct.address
        print(f"L1 address (derived): {eth_address}")
    else:
        eth_private_key = args.eth_private_key
        if eth_private_key is None and args.eth_private_key_env:
            eth_private_key = os.environ.get(args.eth_private_key_env)
        if eth_private_key is None:
            eth_private_key = prompt("ETH_PRIVATE_KEY (hex, no 0x prefix)")
        eth_acc = eth_account.Account.from_key(eth_private_key)
        eth_address = eth_acc.address
        print(f"L1 address: {eth_address}")

    api_key_index = args.api_key_index
    if api_key_index is None:
        api_key_index = int(prompt("API_KEY_INDEX (0..253, not 0 recommended)", "3"))

    num_api_keys = args.num_api_keys
    if num_api_keys is None:
        num_api_keys = int(prompt("NUM_API_KEYS (how many keys to create)", "1"))

    account_index_override = args.account_index

    session = None
    http_proxy_url = None
    try:
        session, http_proxy_url = await _make_session(proxy_url)

        # Best-effort: show egress IP (proxy vs direct).
        try:
            data = await _get_json(session, "https://api.ipify.org", {"format": "json"}, http_proxy_url)
            ip = data.get("ip")
            if ip:
                print(f"Egress IP ({'proxy' if proxy_url else 'direct'}): {ip}")
        except Exception as e:
            print(f"Proxy egress IP lookup failed: {type(e).__name__}: {e!r}")

        if account_index_override is not None:
            account_index = int(account_index_override)
        else:
            accounts = await _get_json(
                session,
                base_url.rstrip("/") + "/api/v1/accountsByL1Address",
                {"l1_address": eth_address},
                http_proxy_url,
            )
            subs = accounts.get("sub_accounts") or []
            if not subs:
                print(f"error: account not found for {eth_address}")
                return 1
            if len(subs) > 1:
                for sub in subs:
                    print(f"found accountIndex: {sub.get('index')}")
                account_index = min(int(sub.get("index")) for sub in subs)
                print(f"multiple accounts found, using the master account {account_index}")
            else:
                account_index = int(subs[0].get("index"))

        chain_id = _chain_id_from_url(base_url)
        from lighter.signer_client import get_signer
        signer = get_signer()

        acct = eth_account.Account.from_key(eth_private_key)
        private_keys: dict[int, str] = {}

        for i in range(num_api_keys):
            idx = api_key_index + i
            priv, pub, err = lighter.create_api_key()
            if err is not None:
                raise Exception(err)
            private_keys[idx] = priv

            nonce_resp = await _get_json(
                session,
                base_url.rstrip("/") + "/api/v1/nextNonce",
                {"account_index": str(int(account_index)), "api_key_index": str(int(idx))},
                http_proxy_url,
            )
            nonce = int(nonce_resp.get("nonce"))

            err_ptr = signer.CreateClient(
                base_url.encode("utf-8"),
                priv.encode("utf-8"),
                int(chain_id),
                int(idx),
                int(account_index),
            )
            if err_ptr:
                raise RuntimeError(err_ptr.decode("utf-8"))

            result = signer.SignChangePubKey(
                pub.encode("utf-8"),
                int(nonce),
                int(idx),
                int(account_index),
            )
            if result.err:
                raise RuntimeError(result.err.decode("utf-8"))

            tx_type = int(result.txType)
            tx_info_str = result.txInfo.decode("utf-8") if result.txInfo else None
            msg_to_sign = result.messageToSign.decode("utf-8") if result.messageToSign else None
            if not tx_info_str or not msg_to_sign:
                raise RuntimeError("Signer returned empty txInfo/messageToSign")

            message = encode_defunct(text=msg_to_sign)
            signature = acct.sign_message(message).signature.to_0x_hex()
            tx_info = json.loads(tx_info_str)
            tx_info["L1Sig"] = signature
            tx_info_signed = json.dumps(tx_info)

            print(f"Changing API key index {idx}...")
            await _post_send_tx(session, base_url, tx_type, tx_info_signed, http_proxy_url)

        print("Waiting for server to update API keys...")
        time.sleep(10)

        cfg = {
            "baseUrl": base_url,
            "accountIndex": int(account_index),
            "privateKeys": {str(k): v for k, v in private_keys.items()},
        }
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(cfg, f, ensure_ascii=False, indent=2)

        print("Saved config.")
        return 0
    finally:
        try:
            if session is not None:
                await session.close()
        except Exception:
            pass


 )PY")
        + QByteArrayLiteral(R"PY(

def main_sync():
    import requests

    p = argparse.ArgumentParser()
    p.add_argument("--out", default="./lighter_api_key_config.json")
    p.add_argument("--base-url", default=None)
    p.add_argument("--eth-private-key", default=None)
    p.add_argument("--eth-private-key-env", default=None)
    p.add_argument("--mnemonic-env", default=None)
    p.add_argument("--address-index", type=int, default=0)
    p.add_argument("--api-key-index", type=int, default=None)
    p.add_argument("--num-api-keys", type=int, default=None)
    p.add_argument("--account-index", type=int, default=None)
    args = p.parse_args()

    out_path = args.out

    print("Lighter setup wizard")
    print("- testnet = fake funds, safe to learn")
    print("- mainnet = real funds")
    print("")

    base_url = args.base_url or prompt("Base URL", "https://testnet.zklighter.elliot.ai")

    proxy_url = os.environ.get("HTTPS_PROXY") or os.environ.get("HTTP_PROXY") or os.environ.get("ALL_PROXY")
    if proxy_url:
        try:
            pxy = urlparse(proxy_url)
            if pxy.hostname and pxy.port:
                auth = " (auth)" if pxy.username else ""
                print(f"Using proxy: {pxy.scheme}://{pxy.hostname}:{pxy.port}{auth}")
            else:
                print("Using proxy: (configured)")
        except Exception:
            print("Using proxy: (configured)")

    eth_private_key = None
    mnemonic = None
    if args.mnemonic_env:
        mnemonic = os.environ.get(args.mnemonic_env)
    if mnemonic:
        eth_account.Account.enable_unaudited_hdwallet_features()
        acct = eth_account.Account.from_mnemonic(
            mnemonic, account_path=f"m/44'/60'/0'/0/{int(args.address_index)}"
        )
        eth_private_key = acct.key.hex()
        eth_address = acct.address
        print(f"L1 address (derived): {eth_address}")
    else:
        eth_private_key = args.eth_private_key
        if eth_private_key is None and args.eth_private_key_env:
            eth_private_key = os.environ.get(args.eth_private_key_env)
        if eth_private_key is None:
            eth_private_key = prompt("ETH_PRIVATE_KEY (hex, no 0x prefix)")
        eth_acc = eth_account.Account.from_key(eth_private_key)
        eth_address = eth_acc.address
        print(f"L1 address: {eth_address}")

    api_key_index = args.api_key_index
    if api_key_index is None:
        api_key_index = int(prompt("API_KEY_INDEX (0..253, not 0 recommended)", "3"))

    num_api_keys = args.num_api_keys
    if num_api_keys is None:
        num_api_keys = int(prompt("NUM_API_KEYS (how many keys to create)", "1"))

    session = requests.Session()
    session.trust_env = False
    if proxy_url:
        session.proxies.update({"http": proxy_url, "https": proxy_url})

    # Best-effort: show egress IP.
    try:
        r = session.get("https://api.ipify.org?format=json", timeout=10)
        if r.ok:
            ip = r.json().get("ip")
            if ip:
                print(f"Egress IP ({'proxy' if proxy_url else 'direct'}): {ip}")
    except Exception as e:
        print(f"Proxy egress IP lookup failed: {type(e).__name__}: {e!r}")

    def get_json(path: str, params: dict | None = None) -> dict:
        url = base_url.rstrip("/") + path
        r = session.get(url, params=params, timeout=20)
        if r.status_code != 200:
            raise RuntimeError(f"HTTP {r.status_code}: {r.text}")
        return r.json()

    def post_send_tx(tx_type: int, tx_info: str) -> dict:
        url = base_url.rstrip("/") + "/api/v1/sendTx"
        files = {
            "tx_type": (None, str(int(tx_type))),
            "tx_info": (None, tx_info),
            "price_protection": (None, "true"),
        }
        r = session.post(url, files=files, timeout=30)
        if r.status_code != 200:
            raise RuntimeError(f"HTTP {r.status_code}: {r.text}")
        return r.json()

    if args.account_index is not None:
        account_index = int(args.account_index)
    else:
        accounts = get_json("/api/v1/accountsByL1Address", {"l1_address": eth_address})
        subs = accounts.get("sub_accounts") or []
        if not subs:
            print(f"error: account not found for {eth_address}")
            return 1
        if len(subs) > 1:
            for sub in subs:
                print(f"found accountIndex: {sub.get('index')}")
            account_index = min(int(sub.get("index")) for sub in subs)
            print(f"multiple accounts found, using the master account {account_index}")
        else:
            account_index = int(subs[0].get("index"))

    chain_id = 304 if "mainnet" in base_url.lower() else 300
    signer = lighter.signer_client.get_signer()
    acct = eth_account.Account.from_key(eth_private_key)

    private_keys = {}
    for i in range(num_api_keys):
        idx = int(api_key_index) + i
        private_key, public_key, err = lighter.create_api_key()
        if err is not None:
            raise Exception(err)
        private_keys[idx] = private_key

        nonce_resp = get_json("/api/v1/nextNonce", {"account_index": str(int(account_index)), "api_key_index": str(int(idx))})
        nonce = int(nonce_resp.get("nonce"))

        err_ptr = signer.CreateClient(
            base_url.encode("utf-8"),
            private_key.encode("utf-8"),
            int(chain_id),
            int(idx),
            int(account_index),
        )
        if err_ptr:
            raise RuntimeError(err_ptr.decode("utf-8"))

        result = signer.SignChangePubKey(public_key.encode("utf-8"), int(nonce), int(idx), int(account_index))
        if result.err:
            raise RuntimeError(result.err.decode("utf-8"))

        tx_type = int(result.txType)
        tx_info_str = result.txInfo.decode("utf-8") if result.txInfo else None
        msg_to_sign = result.messageToSign.decode("utf-8") if result.messageToSign else None
        if not tx_info_str or not msg_to_sign:
            raise RuntimeError("Signer returned empty txInfo/messageToSign")

        message = encode_defunct(text=msg_to_sign)
        signature = acct.sign_message(message).signature.to_0x_hex()
        tx_info = json.loads(tx_info_str)
        tx_info["L1Sig"] = signature
        tx_info_signed = json.dumps(tx_info)

        print(f"Changing API key index {idx}...")
        post_send_tx(tx_type, tx_info_signed)

    print("Waiting for server to update API keys...")
    time.sleep(10)

    cfg = {
        "baseUrl": base_url,
        "accountIndex": int(account_index),
        "privateKeys": {str(k): v for k, v in private_keys.items()},
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)

    print("Saved config.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main_sync())
    except KeyboardInterrupt:
        raise SystemExit(130)
)PY");

    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return QString();
    }
    f.write(script);
    if (!f.commit()) {
        return QString();
    }
    return path;
}

void ConnectionsWindow::launchLighterSetup()
{
    const QString baseDir =
        m_store ? m_store->storagePath() : QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString cfgDir = baseDir.isEmpty() ? QDir::homePath() : baseDir;
    const QString outPath = QDir(cfgDir).filePath(QStringLiteral("lighter_api_key_config.json"));

    const QString wizardPath = writeLighterWizardScript(cfgDir);
    if (wizardPath.isEmpty()) {
        appendLogMessage(tr("Failed to write Lighter setup wizard script."));
        return;
    }

    auto *dlg = new QDialog(this);
    dlg->setWindowTitle(tr("Lighter setup"));
    dlg->setModal(true);
    dlg->resize(720, 540);

    auto *root = new QVBoxLayout(dlg);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    auto *formBox = new QGroupBox(tr("Settings"), dlg);
    auto *form = new QFormLayout(formBox);

    const MexcCredentials lighterCreds =
        m_store ? m_store->loadMexcCredentials(ConnectionStore::Profile::Lighter) : MexcCredentials{};

    auto *baseUrlEdit = new QLineEdit(formBox);
    baseUrlEdit->setText(!lighterCreds.baseUrl.trimmed().isEmpty() ? lighterCreds.baseUrl.trimmed()
                                                                    : QStringLiteral("https://mainnet.zklighter.elliot.ai"));
    baseUrlEdit->setToolTip(tr("Mainnet uses real funds."));
    baseUrlEdit->setReadOnly(true);
    auto *netRow = new QWidget(formBox);
    auto *netRowLayout = new QHBoxLayout(netRow);
    netRowLayout->setContentsMargins(0, 0, 0, 0);
    netRowLayout->setSpacing(8);

    auto *testnetBtn = new QToolButton(netRow);
    testnetBtn->setText(tr("Testnet"));
    testnetBtn->setCheckable(true);
    auto *mainnetBtn = new QToolButton(netRow);
    mainnetBtn->setText(tr("Mainnet"));
    mainnetBtn->setCheckable(true);
    testnetBtn->setVisible(false);
    mainnetBtn->setVisible(false);
    netRowLayout->addWidget(testnetBtn);
    netRowLayout->addWidget(mainnetBtn);
    netRowLayout->addStretch(1);

    const bool isMainnet = baseUrlEdit->text().contains(QStringLiteral("mainnet"), Qt::CaseInsensitive);
    testnetBtn->setChecked(!isMainnet);
    mainnetBtn->setChecked(isMainnet);
    QObject::connect(testnetBtn, &QToolButton::clicked, dlg, [testnetBtn, mainnetBtn, baseUrlEdit]() {
        testnetBtn->setChecked(true);
        mainnetBtn->setChecked(false);
        baseUrlEdit->setText(QStringLiteral("https://mainnet.zklighter.elliot.ai"));
    });
    QObject::connect(mainnetBtn, &QToolButton::clicked, dlg, [testnetBtn, mainnetBtn, baseUrlEdit]() {
        testnetBtn->setChecked(false);
        mainnetBtn->setChecked(true);
        baseUrlEdit->setText(QStringLiteral("https://mainnet.zklighter.elliot.ai"));
    });

    auto *seedEdit = new QLineEdit(formBox);
    seedEdit->setEchoMode(QLineEdit::Password);
    seedEdit->setPlaceholderText(tr("seed words..."));
    seedEdit->setToolTip(tr("BIP39 seed phrase. Used only to generate API keys; not stored by the terminal."));

    auto *setupProxyTypeCombo = new QComboBox(formBox);
    setupProxyTypeCombo->addItem(QStringLiteral("HTTP"), QStringLiteral("http"));
    setupProxyTypeCombo->addItem(QStringLiteral("SOCKS5"), QStringLiteral("socks5"));
    setupProxyTypeCombo->setToolTip(tr("Setup proxy type"));
    setupProxyTypeCombo->setFixedWidth(90);

    auto *setupProxyEdit = new QLineEdit(formBox);
    setupProxyEdit->setPlaceholderText(tr("host:port or user:pass@host:port"));
    setupProxyEdit->setToolTip(tr("Proxy used ONLY for Lighter setup (API key registration)."));
    setupProxyEdit->setText(lighterCreds.proxy.trimmed());

    auto *setupProxyRow = new QWidget(formBox);
    auto *setupProxyLayout = new QHBoxLayout(setupProxyRow);
    setupProxyLayout->setContentsMargins(0, 0, 0, 0);
    setupProxyLayout->setSpacing(8);
    setupProxyLayout->addWidget(setupProxyTypeCombo);
    setupProxyLayout->addWidget(setupProxyEdit, 1);

    auto *seedAddrIndexEdit = new QSpinBox(formBox);
    seedAddrIndexEdit->setRange(0, 1000000);
    seedAddrIndexEdit->setValue(lighterCreds.seedAddressIndex);
    seedAddrIndexEdit->setToolTip(tr("Derivation index for path m/44'/60'/0'/0/{index}."));

    auto *apiKeyIndexEdit = new QSpinBox(formBox);
    apiKeyIndexEdit->setRange(0, 253);
    apiKeyIndexEdit->setValue(3);

    auto *numKeysEdit = new QSpinBox(formBox);
    numKeysEdit->setRange(1, 10);
    numKeysEdit->setValue(1);

    auto *accountIndexEdit = new QLineEdit(formBox);
    accountIndexEdit->setPlaceholderText(tr("Optional (blank = auto)"));

    auto *printSecretsCheck = new QCheckBox(tr("Print API keys to log (unsafe)"), formBox);
    printSecretsCheck->setToolTip(tr("WARNING: This prints your Lighter API private keys into the setup log and copies them to clipboard."));
    printSecretsCheck->setChecked(false);

    auto *storageLabel = new QLabel(tr("Lighter vault (encrypted)"), formBox);
    storageLabel->setToolTip(tr("Хранится локально и зашифровано (Windows DPAPI)."));

    form->addRow(tr("Network:"), netRow);
    form->addRow(tr("Base URL:"), baseUrlEdit);
    form->addRow(tr("Setup proxy:"), setupProxyRow);
    form->addRow(tr("Seed phrase:"), seedEdit);
    form->addRow(tr("Seed address index:"), seedAddrIndexEdit);
    form->addRow(tr("API key index:"), apiKeyIndexEdit);
    form->addRow(tr("Number of keys:"), numKeysEdit);
    form->addRow(tr("Account index:"), accountIndexEdit);
    form->addRow(QString(), printSecretsCheck);
    form->addRow(tr("Storage:"), storageLabel);
    root->addWidget(formBox);

    auto *log = new QPlainTextEdit(dlg);
    log->setReadOnly(true);
    root->addWidget(log, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    auto *runBtn = new QPushButton(tr("Run setup"), dlg);
    buttons->addButton(runBtn, QDialogButtonBox::ActionRole);
    root->addWidget(buttons);

    auto append = [log](const QString &line) {
        if (!line.isEmpty()) {
            log->appendPlainText(line);
        }
        QTextCursor c = log->textCursor();
        c.movePosition(QTextCursor::End);
        log->setTextCursor(c);
    };

    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);

    auto *proc = new QProcess(dlg);
    proc->setProcessChannelMode(QProcess::MergedChannels);

    struct RunnerState {
        QString venvDir;
        QString venvPython;
        QString wizardPath;
        QString outPath;
        QString baseUrl;
        QString ethPriv;
        QString mnemonic;
        int mnemonicAddressIndex = 0;
        bool useMnemonic = false;
        int apiKeyIndex = 3;
        int numKeys = 1;
        QString accountIndexText;
        int step = 0;
        bool depsChecked = false;
        bool installedPySocks = false;
        QString lastAction;
        bool printSecrets = false;
        QString proxyType;
        QString proxyEnvUrl;
        QString setupProxyRaw;
    };
    auto *st = new RunnerState();
    st->venvDir = QDir(cfgDir).filePath(QStringLiteral("lighter_venv"));
    st->venvPython = QDir(st->venvDir).filePath(QStringLiteral("Scripts/python.exe"));
    st->wizardPath = wizardPath;
    st->outPath = outPath;

    auto applySetupProxyEnv = [st](QProcessEnvironment &env, bool enableProxy) {
        static const char *keys[] = {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"};

        // Always start by clearing proxy from environment to avoid surprises.
        for (const char *k : keys) {
            env.remove(QString::fromLatin1(k));
        }
        if (!enableProxy || st->proxyEnvUrl.isEmpty()) {
            return;
        }
        // Setup wizard reads these env vars; SOCKS requires aiohttp-socks in the venv.
        env.insert(QStringLiteral("HTTP_PROXY"), st->proxyEnvUrl);
        env.insert(QStringLiteral("HTTPS_PROXY"), st->proxyEnvUrl);
        env.insert(QStringLiteral("ALL_PROXY"), st->proxyEnvUrl);
        env.insert(QStringLiteral("http_proxy"), st->proxyEnvUrl);
        env.insert(QStringLiteral("https_proxy"), st->proxyEnvUrl);
        env.insert(QStringLiteral("all_proxy"), st->proxyEnvUrl);
    };

    auto startStep = [proc, st, cfgDir, append, applySetupProxyEnv]() {
        while (true) {
            if (st->step == 0) {
                if (QFile::exists(st->venvPython)) {
                    st->step = 1;
                    continue;
                }
                append(QObject::tr("Step 1/3: creating venv..."));
                proc->setWorkingDirectory(cfgDir);
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                applySetupProxyEnv(env, false);
                proc->setProcessEnvironment(env);
                st->lastAction = QStringLiteral("venv");
                proc->start(QStringLiteral("python.exe"),
                            {QStringLiteral("-m"), QStringLiteral("venv"), st->venvDir});
                return;
            }

            if (st->step == 1) {
                if (!QFile::exists(st->venvPython)) {
                    append(QObject::tr("Failed: venv python not found: %1").arg(st->venvPython));
                    return;
                }

                if (!st->depsChecked) {
                    append(QObject::tr("Step 2/3: checking venv packages..."));
                    proc->setWorkingDirectory(cfgDir);
                    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                    applySetupProxyEnv(env, false);
                    proc->setProcessEnvironment(env);
                    st->lastAction = QStringLiteral("check");
                    QString checkCode =
                        QStringLiteral("import lighter, eth_account; import lighter.signer_client; print('OK')");
                    if (!st->proxyEnvUrl.isEmpty() && st->proxyType == QStringLiteral("socks5")) {
                        // requests requires PySocks ("socks" module) for socks5:// proxies.
                        checkCode =
                            QStringLiteral("import lighter, eth_account; import lighter.signer_client; import socks; print('OK')");
                    }
                    proc->start(st->venvPython, {QStringLiteral("-c"), checkCode});
                    return;
                }

                append(QObject::tr("Step 2/3: installing lighter-sdk into venv..."));
                proc->setWorkingDirectory(cfgDir);
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                applySetupProxyEnv(env, false);
                proc->setProcessEnvironment(env);
                QStringList pipArgs;
                pipArgs << QStringLiteral("-m")
                        << QStringLiteral("pip")
                        << QStringLiteral("install")
                        << QStringLiteral("--upgrade")
                        << QStringLiteral("lighter-sdk==1.0.2");
                if (!st->proxyEnvUrl.isEmpty() && st->proxyType == QStringLiteral("socks5")) {
                    // Add SOCKS support for `requests` via PySocks.
                    pipArgs << QStringLiteral("requests[socks]");
                }
                st->lastAction = QStringLiteral("pip");
                proc->start(st->venvPython, pipArgs);
                return;
            }

        if (st->step == 2) {
            append(QObject::tr("Step 3/3: running wizard..."));
            proc->setWorkingDirectory(cfgDir);
            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            applySetupProxyEnv(env, true);
            // Ensure Python prints show up immediately in the UI log (stdout is buffered when piped).
            env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
            if (st->useMnemonic) {
                env.insert(QStringLiteral("LIGHTER_MNEMONIC"), st->mnemonic);
            } else {
                env.insert(QStringLiteral("LIGHTER_ETH_PRIVATE_KEY"), st->ethPriv);
            }
            proc->setProcessEnvironment(env);
            QStringList args;
            args << QStringLiteral("-u") << st->wizardPath
                 << QStringLiteral("--out")
                 << st->outPath
                 << QStringLiteral("--base-url")
                 << st->baseUrl
                     << (st->useMnemonic ? QStringLiteral("--mnemonic-env") : QStringLiteral("--eth-private-key-env"))
                     << (st->useMnemonic ? QStringLiteral("LIGHTER_MNEMONIC")
                                         : QStringLiteral("LIGHTER_ETH_PRIVATE_KEY"))
                     << QStringLiteral("--api-key-index")
                     << QString::number(st->apiKeyIndex)
                     << QStringLiteral("--num-api-keys")
                     << QString::number(st->numKeys);
                if (st->useMnemonic) {
                    args << QStringLiteral("--address-index") << QString::number(st->mnemonicAddressIndex);
                }
                const QString ai = st->accountIndexText.trimmed();
                if (!ai.isEmpty()) {
                    bool ok = false;
                    ai.toInt(&ok);
                    if (ok) {
                        args << QStringLiteral("--account-index") << ai;
                    }
                }
                st->lastAction = QStringLiteral("wizard");
                proc->start(st->venvPython, args);
                return;
            }

            append(QObject::tr("Failed: internal wizard state is invalid."));
            return;
        }
    };

    QObject::connect(proc, &QProcess::readyRead, dlg, [proc, append]() {
        const QByteArray data = proc->readAll();
        if (!data.isEmpty()) {
            append(QString::fromUtf8(data).trimmed());
        }
    });

    QObject::connect(proc,
                     qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
                     dlg,
                     [this, proc, st, append, startStep, runBtn](int code, QProcess::ExitStatus) mutable {
                         if (code != 0) {
                             if (st->lastAction == QStringLiteral("check")) {
                                 st->depsChecked = true;
                                 startStep();
                                 return;
                             }
                             append(QObject::tr("Failed (exit code %1).").arg(code));
                             runBtn->setEnabled(true);
                             return;
                         }

                         if (st->lastAction == QStringLiteral("venv")) {
                             st->step = 1;
                             startStep();
                             return;
                         }
                         if (st->lastAction == QStringLiteral("check")) {
                             st->step = 2;
                             startStep();
                             return;
                         }
                         if (st->lastAction == QStringLiteral("pip")) {
                             st->step = 2;
                             startStep();
                             return;
                         }

                         if (st->lastAction == QStringLiteral("wizard")) {
                              QFile f(st->outPath);
                              if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                                  const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
                                  f.close();
                                  if (doc.isObject()) {
                                      const QJsonObject root = doc.object();
                                      QString werr;
                                      const QString vaultPath = lighterConfigPath();
                                      if (!writeLighterVaultFile(vaultPath, root, &werr)) {
                                          append(QObject::tr("Failed to save encrypted vault: %1").arg(werr));
                                          runBtn->setEnabled(true);
                                          return;
                                      }

                                      if (st->printSecrets) {
                                          QStringList lines;
                                          lines << QObject::tr("Lighter setup details (KEEP SECRET):");
                                          const QString b = root.value(QStringLiteral("baseUrl")).toString();
                                          if (!b.isEmpty()) {
                                              lines << QObject::tr("Base URL: %1").arg(b);
                                          }
                                          const int accountIndex = root.value(QStringLiteral("accountIndex")).toInt();
                                          lines << QObject::tr("Account index: %1").arg(accountIndex);
                                          lines << QObject::tr("API key index start: %1").arg(st->apiKeyIndex);

                                          const QJsonObject pk = root.value(QStringLiteral("privateKeys")).toObject();
                                          QList<int> idxs;
                                          idxs.reserve(pk.size());
                                          for (const QString &k : pk.keys()) {
                                              bool ok = false;
                                              const int v = k.toInt(&ok);
                                              if (ok) {
                                                  idxs.push_back(v);
                                              }
                                          }
                                          std::sort(idxs.begin(), idxs.end());
                                          lines << QObject::tr("API private keys:");
                                          for (int idx : idxs) {
                                              const QString key = QString::number(idx);
                                              const QString priv = pk.value(key).toString();
                                              lines << QStringLiteral("  %1: %2").arg(key, priv);
                                          }

                                          const QString payload = lines.join(QStringLiteral("\n"));
                                          append(payload);
                                          if (auto *cb = QGuiApplication::clipboard()) {
                                              cb->setText(payload);
                                          }
                                          append(QObject::tr("Copied details to clipboard."));
                                      }

                                      (void)QFile::remove(st->outPath);
                                      append(QObject::tr("Done. Saved to local encrypted vault."));

                                      QString lighterId = m_store ? m_store->activeProfileId(ConnectionStore::Profile::Lighter)
                                                                  : QStringLiteral("lighter:default");
                                      if (m_store && lighterId.isEmpty()) {
                                          lighterId =
                                              m_store->createProfile(ConnectionStore::Profile::Lighter,
                                                                     exchangeTitle(ConnectionStore::Profile::Lighter))
                                                  .id;
                                          rebuildCardsFromStore();
                                      }
                                      CardWidgets *card = nullptr;
                                      for (auto *c : m_cards) {
                                          if (c && c->profileId == lighterId) {
                                              card = c;
                                              break;
                                          }
                                      }
                                       if (!card) {
                                           card = ensureCard(lighterId, ConnectionStore::Profile::Lighter);
                                       }
                                       if (card) {
                                           (void)loadLighterConfigIntoCard(card, vaultPath, false);
                                           refreshUi();
                                           handleConnectClicked(lighterId);
                                      }
                                  }
                              }
                              runBtn->setEnabled(true);
                              return;
                         }

                         startStep();
                     });

    QObject::connect(runBtn, &QPushButton::clicked, dlg, [=]() mutable {
        st->useMnemonic = true;
        const QString mnemonic = seedEdit->text().trimmed();
        if (mnemonic.isEmpty()) {
            append(QObject::tr("Seed phrase is required."));
            return;
        }
        st->mnemonic = mnemonic;
        st->mnemonicAddressIndex = seedAddrIndexEdit->value();
        st->ethPriv.clear();
        st->baseUrl = baseUrlEdit->text().trimmed();

        if (m_store) {
            MexcCredentials cur = m_store->loadMexcCredentials(ConnectionStore::Profile::Lighter);
            cur.baseUrl = st->baseUrl;
            cur.seedAddressIndex = st->mnemonicAddressIndex;
            m_store->saveMexcCredentials(cur, ConnectionStore::Profile::Lighter);
        }

        st->apiKeyIndex = apiKeyIndexEdit->value();
        st->numKeys = numKeysEdit->value();
        st->accountIndexText = accountIndexEdit->text();
        st->printSecrets = printSecretsCheck->isChecked();
        st->setupProxyRaw = setupProxyEdit->text().trimmed();
        st->proxyType = setupProxyTypeCombo->currentData().toString();
        st->proxyEnvUrl = QString();
        if (!st->setupProxyRaw.isEmpty()) {
            st->proxyEnvUrl = proxyUrlForEnv(st->proxyType, st->setupProxyRaw);
            if (st->proxyEnvUrl.isEmpty()) {
                append(QObject::tr("Setup proxy: could not parse proxy string."));
                return;
            }
        } else {
            st->proxyEnvUrl = systemProxyUrlForTarget(QUrl(st->baseUrl));
        }

        if (st->proxyEnvUrl.isEmpty()) {
            append(QObject::tr("Setup proxy: (direct)"));
        } else {
            const QUrl u(st->proxyEnvUrl);
            const QString auth = u.userName().isEmpty() ? QString() : QStringLiteral(" (auth)");
            append(QObject::tr("Setup proxy: %1://%2:%3%4")
                       .arg(u.scheme(), u.host(), QString::number(u.port()), auth));
        }
        st->step = 0;
        st->depsChecked = false;
        st->installedPySocks = false;
        st->lastAction.clear();
        runBtn->setEnabled(false);
        append(QObject::tr("Starting..."));
        startStep();
    });

    QObject::connect(dlg, &QDialog::finished, dlg, [proc, st](int) {
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
        }
        delete st;
    });

    dlg->show();
    appendLogMessage(tr("Opened Lighter setup dialog."));
}

static QString proxyUrlForEnv(const QString &proxyType, const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty()) {
        return QString();
    }
    if (s.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0
        || s.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
        || s.compare(QStringLiteral("direct"), Qt::CaseInsensitive) == 0) {
        return QString();
    }
    const QString t = proxyType.trimmed().toLower();
    const QString scheme =
        (t == QStringLiteral("socks5")) ? QStringLiteral("socks5")
        : (t == QStringLiteral("https")) ? QStringLiteral("https")
                                         : QStringLiteral("http");
    if (s.contains(QStringLiteral("://"))) {
        const QUrl u(s);
        if (!u.isValid() || u.host().isEmpty() || u.port() <= 0) {
            return s;
        }
        // Re-encode with our desired scheme (fully-encoded user/pass).
        QUrl out;
        out.setScheme(!u.scheme().isEmpty() ? u.scheme() : scheme);
        out.setHost(u.host());
        out.setPort(u.port());
        if (!u.userName().isEmpty()) {
            out.setUserName(u.userName());
            out.setPassword(u.password());
        }
        return out.toString(QUrl::FullyEncoded);
    }
    auto make = [&](const QString &host, int port, const QString &user, const QString &pass) -> QString {
        if (host.isEmpty() || port <= 0) {
            return QString();
        }
        QUrl out;
        out.setScheme(scheme);
        out.setHost(host);
        out.setPort(port);
        if (!user.isEmpty()) {
            out.setUserName(user);
            out.setPassword(pass);
        }
        return out.toString(QUrl::FullyEncoded);
    };

    if (s.contains(QLatin1Char('@'))) {
        const QStringList parts = s.split(QLatin1Char('@'));
        if (parts.size() != 2) {
            return QString();
        }
        const QString upRaw = parts[0];
        const int upSep = upRaw.indexOf(QLatin1Char(':'));
        const QStringList hp = parts[1].split(QLatin1Char(':'), Qt::KeepEmptyParts);
        if (hp.size() != 2) {
            return QString();
        }
        const QString host = hp[0].trimmed();
        bool ok = false;
        const int port = hp[1].trimmed().toInt(&ok);
        if (!ok || port <= 0) {
            return QString();
        }
        const QString user = (upSep >= 0) ? upRaw.left(upSep).trimmed() : upRaw.trimmed();
        const QString pass = (upSep >= 0) ? upRaw.mid(upSep + 1) : QString();
        return make(host, port, user, pass);
    }

    const QStringList parts = s.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (parts.size() == 2) {
        bool ok = false;
        const int port = parts[1].trimmed().toInt(&ok);
        if (!ok || port <= 0) {
            return QString();
        }
        return make(parts[0].trimmed(), port, QString(), QString());
    }
    if (parts.size() >= 4) {
        bool ok = false;
        const int port = parts[1].trimmed().toInt(&ok);
        if (!ok || port <= 0) {
            return QString();
        }
        const QString user = parts[2].trimmed();
        const QString pass = parts.mid(3).join(QStringLiteral(":"));
        return make(parts[0].trimmed(), port, user, pass);
    }
    return QString();
}

static QString systemProxyUrlForTarget(const QUrl &target)
{
    if (!target.isValid() || target.scheme().isEmpty()) {
        return QString();
    }

    QNetworkProxyFactory::setUseSystemConfiguration(true);
    const QList<QNetworkProxy> proxies =
        QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(target));
    for (const QNetworkProxy &p : proxies) {
        if (p.type() == QNetworkProxy::NoProxy || p.type() == QNetworkProxy::DefaultProxy) {
            continue;
        }
        const QString scheme = (p.type() == QNetworkProxy::Socks5Proxy) ? QStringLiteral("socks5")
                              : (p.type() == QNetworkProxy::HttpProxy)  ? QStringLiteral("http")
                                                                        : QString();
        if (scheme.isEmpty() || p.hostName().isEmpty() || p.port() <= 0) {
            continue;
        }
        QUrl out;
        out.setScheme(scheme);
        out.setHost(p.hostName());
        out.setPort(p.port());
        if (!p.user().isEmpty()) {
            out.setUserName(p.user());
            out.setPassword(p.password());
        }
        return out.toString(QUrl::FullyEncoded);
    }
    return QString();
}

#include "ConnectionsWindow.moc"
