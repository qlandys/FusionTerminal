#include "TradeManager.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLibrary>
#include <QMessageAuthenticationCode>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QHttpMultiPart>
#include <QUrl>
#include <QStringList>
#include <QSet>
#include <QFile>
#include <QProcessEnvironment>
#include <QDir>
#include <QTextStream>
#include <QSaveFile>
#include <QCoreApplication>
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <cstdlib>
#include <cstddef>
#include <QStandardPaths>
#include <QSet>
#include <QRandomGenerator>
#include <atomic>
#include <cmath>
#include <QElapsedTimer>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {
// Lighter HTTP endpoints are rate-limited pretty aggressively; keep baseline polling conservative
// and use short "burst" polling around order actions when we need fast UI updates.
constexpr int kLighterAccountPollMs = 4000;
constexpr int kLighterTradesPollMs = 4000;
constexpr int kLighterPollMaxBackoffMs = 30000;
constexpr int kLighterWsSendTxTimeoutMs = 8000;
constexpr double kLighterMarketMaxSlippage = 0.01;
constexpr auto kLighterVaultMagic = "FUSION_LIGHTER_VAULT_V1\n";
constexpr auto kLighterVaultMagicLegacy = "PLASMA_LIGHTER_VAULT_V1\n";
constexpr int kLighterVaultMagicLen = 24;

static QString lighterOrderIdForCache(const QJsonObject &o)
{
    const QStringList keys = {
        QStringLiteral("client_order_index"),
        QStringLiteral("clientOrderIndex"),
        QStringLiteral("order_index"),
        QStringLiteral("orderIndex"),
        QStringLiteral("order_id"),
        QStringLiteral("orderId"),
    };
    for (const auto &k : keys) {
        if (!o.contains(k)) continue;
        const QString s = o.value(k).toVariant().toString().trimmed();
        if (!s.isEmpty()) return s;
    }
    return QString();
}

QString tradeConfigDir();
QString legacyPlasmaConfigDir(QStandardPaths::StandardLocation loc);

#ifdef _WIN32
static QByteArray dpapiUnprotectBytesLocal(const QByteArray &cipher, QString *err)
{
    if (cipher.isEmpty()) {
        if (err) *err = QStringLiteral("empty payload");
        return {};
    }
    DATA_BLOB in;
    in.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(cipher.data()));
    in.cbData = static_cast<DWORD>(cipher.size());
    DATA_BLOB out;
    ZeroMemory(&out, sizeof(out));
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) {
        if (err) *err = QStringLiteral("CryptUnprotectData failed (%1)").arg(GetLastError());
        return {};
    }
    QByteArray plain(reinterpret_cast<const char *>(out.pbData), static_cast<int>(out.cbData));
    if (out.pbData) {
        LocalFree(out.pbData);
    }
    return plain;
}

static QByteArray dpapiUnprotectFromBase64Local(const QByteArray &b64, QString *err)
{
    const QByteArray cipher = QByteArray::fromBase64(b64);
    if (cipher.isEmpty()) {
        if (err) *err = QStringLiteral("invalid base64 payload");
        return {};
    }
    return dpapiUnprotectBytesLocal(cipher, err);
}
#endif

static bool readLighterVaultJson(QJsonObject *out, QString *err)
{
    if (!out) {
        if (err) *err = QStringLiteral("null output");
        return false;
    }
    const QString vaultPath = tradeConfigDir() + QDir::separator() + QStringLiteral("lighter_vault.dat");
    QFile f(vaultPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("vault not found");
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    QByteArray jsonBytes;
    if (data.startsWith(kLighterVaultMagic)) {
        const QByteArray payloadB64 = data.mid(kLighterVaultMagicLen).trimmed();
#ifdef _WIN32
        jsonBytes = dpapiUnprotectFromBase64Local(payloadB64, err);
#else
        if (err) *err = QStringLiteral("encrypted vault requires Windows");
        return false;
#endif
    } else if (data.startsWith(kLighterVaultMagicLegacy)) {
        const QByteArray payloadB64 = data.mid(kLighterVaultMagicLen).trimmed();
#ifdef _WIN32
        jsonBytes = dpapiUnprotectFromBase64Local(payloadB64, err);
#else
        if (err) *err = QStringLiteral("encrypted vault requires Windows");
        return false;
#endif
    } else {
        jsonBytes = data;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("invalid vault JSON");
        return false;
    }
    *out = doc.object();
    return true;
}

static bool fillLighterCredsFromVault(MexcCredentials *creds, QString *err)
{
    if (!creds) {
        if (err) *err = QStringLiteral("null creds");
        return false;
    }
    QJsonObject root;
    if (!readLighterVaultJson(&root, err) || root.isEmpty()) {
        return false;
    }
    const QString baseUrl = root.value(QStringLiteral("baseUrl")).toString().trimmed();
    const int accountIndex = root.value(QStringLiteral("accountIndex")).toInt(0);
    const QJsonObject keysObj = root.value(QStringLiteral("privateKeys")).toObject();
    const QString proxyRaw = root.value(QStringLiteral("proxy")).toString().trimmed();
    QString proxyType = root.value(QStringLiteral("proxyType")).toString().trimmed().toLower();
    if (proxyType == QStringLiteral("https")) {
        proxyType = QStringLiteral("http");
    }
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
    if (bestIndex < 0) {
        if (err) *err = QStringLiteral("no api keys in vault");
        return false;
    }
    const QString apiPriv = keysObj.value(bestKey).toString().trimmed();
    if (apiPriv.isEmpty()) {
        if (err) *err = QStringLiteral("empty api key in vault");
        return false;
    }

    creds->baseUrl = baseUrl;
    creds->accountIndex = accountIndex;
    creds->apiKeyIndex = bestIndex;
    creds->apiKey = apiPriv;
    if (!proxyRaw.isEmpty()) {
        creds->proxy = proxyRaw;
        creds->proxyType = proxyType.isEmpty() ? QStringLiteral("http") : proxyType;
    }
    return true;
}

QString tradeConfigDir()
{
    const QString roaming = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    const QString local = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);

    auto hasVaultOrHistory = [](const QString &dir) -> bool {
        if (dir.trimmed().isEmpty()) {
            return false;
        }
        return QFile::exists(dir + QLatin1String("/lighter_vault.dat"))
               || QFile::exists(dir + QLatin1String("/trades_history.jsonl"))
               || QFile::exists(dir + QLatin1String("/connections.json"));
    };

    QString path;
    if (hasVaultOrHistory(roaming)) {
        path = roaming;
    } else if (hasVaultOrHistory(local)) {
        path = local;
    } else if (!roaming.isEmpty()) {
        path = roaming;
    } else if (!local.isEmpty()) {
        path = local;
    } else {
        path = QDir::homePath() + QLatin1String("/.fusion_terminal");
    }

    QDir().mkpath(path);
    const QString legacyRoaming = legacyPlasmaConfigDir(QStandardPaths::AppConfigLocation);
    const QString legacyLocal = legacyPlasmaConfigDir(QStandardPaths::AppLocalDataLocation);
    auto copyLegacyFileIfMissing = [&](const QString &legacyDir, const QString &name) {
        if (legacyDir.isEmpty()) {
            return;
        }
        const QString legacyPath = legacyDir + QDir::separator() + name;
        const QString newPath = path + QDir::separator() + name;
        if (QFile::exists(legacyPath) && !QFile::exists(newPath)) {
            QFile::copy(legacyPath, newPath);
        }
    };
    copyLegacyFileIfMissing(legacyRoaming, QStringLiteral("lighter_vault.dat"));
    copyLegacyFileIfMissing(legacyRoaming, QStringLiteral("trades_history.jsonl"));
    copyLegacyFileIfMissing(legacyLocal, QStringLiteral("lighter_vault.dat"));
    copyLegacyFileIfMissing(legacyLocal, QStringLiteral("trades_history.jsonl"));
    return path;
}

QStringList probeLegacyAppConfigDirs(const QString &primaryDir)
{
    QStringList out;
    const QFileInfo fi(primaryDir);
    const QDir parentDir = fi.dir();
    if (!parentDir.exists()) {
        return out;
    }
    const QStringList entries =
        parentDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        if (!name.contains(QStringLiteral("shah"), Qt::CaseInsensitive)) {
            continue;
        }
        out << parentDir.absoluteFilePath(name);
    }
    return out;
}

QString legacyPlasmaConfigDir(QStandardPaths::StandardLocation loc)
{
    const QString current = QStandardPaths::writableLocation(loc);
    if (current.isEmpty()) {
        return {};
    }
    QDir appDir(current);
    if (!appDir.cdUp()) {
        return {};
    }
    QDir orgDir(appDir);
    if (!orgDir.cdUp()) {
        return {};
    }
    const QString root = orgDir.absolutePath();
    return root + QDir::separator()
        + QStringLiteral("Plasma")
        + QDir::separator()
        + QStringLiteral("PlasmaTerminal");
}

QString resolveTradeHistoryPath(const QString &primaryDir)
{
    const QString primaryFile =
        primaryDir + QDir::separator() + QStringLiteral("trades_history.jsonl");
    if (QFile::exists(primaryFile)) {
        return primaryFile;
    }
    QStringList candidates;
    candidates << (QDir::homePath() + QLatin1String("/.fusion_terminal/trades_history.jsonl"));
    const QString plasmaRoaming = legacyPlasmaConfigDir(QStandardPaths::AppConfigLocation);
    if (!plasmaRoaming.isEmpty()) {
        candidates << (plasmaRoaming + QDir::separator() + QStringLiteral("trades_history.jsonl"));
    }
    const QString plasmaLocal = legacyPlasmaConfigDir(QStandardPaths::AppLocalDataLocation);
    if (!plasmaLocal.isEmpty()) {
        candidates << (plasmaLocal + QDir::separator() + QStringLiteral("trades_history.jsonl"));
    }
    candidates << (QDir::homePath() + QLatin1String("/.plasma_terminal/trades_history.jsonl"));
    candidates << (QDir::homePath() + QLatin1String("/.ghost_terminal/trades_history.jsonl"));
    candidates << (QDir::homePath() + QLatin1String("/.shah_terminal/trades_history.jsonl"));
    const QStringList legacyDirs = probeLegacyAppConfigDirs(primaryDir);
    for (const QString &legacyDir : legacyDirs) {
        candidates << (legacyDir + QDir::separator() + QStringLiteral("trades_history.jsonl"));
    }
    for (const QString &candidate : candidates) {
        if (QFile::exists(candidate)) {
            return candidate;
        }
    }
    return primaryFile;
}

QString normalizedSymbol(const QString &symbol)
{
    QString s = symbol.trimmed().toUpper();
    return s;
}

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

static void logTradeManagerEvent(const QString &msg)
{
    static const bool enabled =
        qEnvironmentVariableIntValue("FUSION_TM_DEBUG_LOG") > 0
        || qEnvironmentVariableIntValue("PLASMA_TM_DEBUG_LOG") > 0;
    if (!enabled) {
        return;
    }
    const QString path = fusionLogFilePath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODate) << " [trade] " << msg << "\n";
}

QString trimDecimalZeros(QString s)
{
    const int dot = s.indexOf(QLatin1Char('.'));
    if (dot < 0) {
        return s;
    }
    while (s.endsWith(QLatin1Char('0'))) {
        s.chop(1);
    }
    if (s.endsWith(QLatin1Char('.'))) {
        s.chop(1);
    }
    return s;
}

struct LighterStrOrErr
{
    char *str = nullptr;
    char *err = nullptr;
};

struct LighterSignedTxResponse
{
    quint8 txType = 0;
    char *txInfo = nullptr;
    char *txHash = nullptr;
    char *messageToSign = nullptr;
    char *err = nullptr;
};

namespace lighter_signer
{
    using CreateClientFn = char *(*)(char *cUrl, char *cPrivateKey, int cChainId, int cApiKeyIndex, long long cAccountIndex);
    using CheckClientFn = char *(*)(int cApiKeyIndex, long long cAccountIndex);
    using CreateAuthTokenFn = LighterStrOrErr (*)(long long cDeadline, int cApiKeyIndex, long long cAccountIndex);
    using SignCreateOrderFn = LighterSignedTxResponse (*)(int cMarketIndex,
                                                          long long cClientOrderIndex,
                                                          long long cBaseAmount,
                                                          int cPrice,
                                                          int cIsAsk,
                                                          int cOrderType,
                                                          int cTimeInForce,
                                                          int cReduceOnly,
                                                          int cTriggerPrice,
                                                          long long cOrderExpiry,
                                                          long long cNonce,
                                                          int cApiKeyIndex,
                                                          long long cAccountIndex);
    using SignCancelAllOrdersFn = LighterSignedTxResponse (*)(int cTimeInForce,
                                                             long long cTime,
                                                             long long cNonce,
                                                             int cApiKeyIndex,
                                                             long long cAccountIndex);
    using SignCancelOrderFn = LighterSignedTxResponse (*)(int cMarketIndex,
                                                         long long cOrderIndex,
                                                         long long cNonce,
                                                         int cApiKeyIndex,
                                                         long long cAccountIndex);
    using SignUpdateLeverageFn = LighterSignedTxResponse (*)(int cMarketIndex,
                                                            int cInitialMarginFraction,
                                                            int cMarginMode,
                                                            long long cNonce,
                                                            int cApiKeyIndex,
                                                            long long cAccountIndex);
}

struct LighterSignerApi
{
    QLibrary lib;
    lighter_signer::CreateClientFn createClient = nullptr;
    lighter_signer::CheckClientFn checkClient = nullptr;
    lighter_signer::CreateAuthTokenFn createAuthToken = nullptr;
    lighter_signer::SignCreateOrderFn signCreateOrder = nullptr;
    lighter_signer::SignCancelAllOrdersFn signCancelAllOrders = nullptr;
    lighter_signer::SignCancelOrderFn signCancelOrder = nullptr;
    lighter_signer::SignUpdateLeverageFn signUpdateLeverage = nullptr;

    bool ensureLoaded(const QString &path, QString &errOut)
    {
        if (lib.isLoaded()) {
            return true;
        }
        lib.setFileName(path);
        lib.setLoadHints(QLibrary::ResolveAllSymbolsHint);
        if (!lib.load()) {
            errOut = lib.errorString();
            return false;
        }
        createClient = reinterpret_cast<lighter_signer::CreateClientFn>(lib.resolve("CreateClient"));
        checkClient = reinterpret_cast<lighter_signer::CheckClientFn>(lib.resolve("CheckClient"));
        createAuthToken = reinterpret_cast<lighter_signer::CreateAuthTokenFn>(lib.resolve("CreateAuthToken"));
        signCreateOrder = reinterpret_cast<lighter_signer::SignCreateOrderFn>(lib.resolve("SignCreateOrder"));
        signCancelAllOrders = reinterpret_cast<lighter_signer::SignCancelAllOrdersFn>(lib.resolve("SignCancelAllOrders"));
        signCancelOrder = reinterpret_cast<lighter_signer::SignCancelOrderFn>(lib.resolve("SignCancelOrder"));
        signUpdateLeverage = reinterpret_cast<lighter_signer::SignUpdateLeverageFn>(lib.resolve("SignUpdateLeverage"));
        if (!createClient || !checkClient || !createAuthToken || !signCreateOrder || !signCancelAllOrders || !signCancelOrder || !signUpdateLeverage) {
            errOut = QStringLiteral("Missing exports in signer DLL (CreateClient/CheckClient/CreateAuthToken/SignCreateOrder/SignCancelAllOrders/SignCancelOrder/SignUpdateLeverage).");
            lib.unload();
            return false;
        }
        return true;
    }
};

LighterSignerApi &lighterSigner()
{
    static LighterSignerApi api;
    return api;
}

QString normalizeLighterUrl(QString url)
{
    url = url.trimmed();
    while (url.endsWith(QLatin1Char('/'))) {
        url.chop(1);
    }
    return url;
}

int lighterChainIdForUrl(const QString &url)
{
    return url.contains(QStringLiteral("mainnet"), Qt::CaseInsensitive) ? 304 : 300;
}

QString strip0xPrefix(QString s)
{
    s = s.trimmed();
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        s = s.mid(2);
    }
    return s;
}

struct LighterMarketMeta
{
    int marketId = -1;
    int priceDecimals = 0;
    int sizeDecimals = 0;
    bool isSpot = false;
    double minBaseAmount = 0.0;
    double minQuoteAmount = 0.0;
    int minInitialMarginFraction = 0; // per Lighter API (e.g. 1000 = 10%)
    double lastTradePrice = 0.0; // parsed from orderBookDetails (quote units)
};

static qint64 pow10i(int decimals)
{
    qint64 out = 1;
    for (int i = 0; i < decimals; ++i) {
        out *= 10;
    }
    return out;
}

static QString lighterHttpErrorMessage(QNetworkReply *reply, const QByteArray &raw)
{
    if (reply && reply->property("fusion_timeout").toBool()) {
        return QStringLiteral("Timeout");
    }
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 400) {
        return QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(raw));
    }
    return reply->errorString();
}

static void applyReplyTimeout(QNetworkReply *reply, int timeoutMs)
{
    if (!reply || timeoutMs <= 0) {
        return;
    }
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, [reply]() {
        if (!reply || !reply->isRunning()) {
            return;
        }
        reply->setProperty("fusion_timeout", true);
        reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, timer, &QObject::deleteLater);
    timer->start(timeoutMs);
}

struct LighterMetaCache
{
    QHash<QString, LighterMarketMeta> bySymbol;
    QHash<int, QString> symbolByMarketId;
    bool inFlight = false;
    qint64 updatedMs = 0;
    QVector<std::function<void(QString err)>> waiters;
};

static QHash<QString, LighterMetaCache> &lighterMetaCacheByUrl()
{
    static QHash<QString, LighterMetaCache> cache;
    return cache;
}

static void addLighterMarketMeta(QHash<QString, LighterMarketMeta> &dst,
                                const QJsonArray &arr,
                                bool isSpot)
{
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        const QString symbol = normalizedSymbol(o.value(QStringLiteral("symbol")).toString());
        if (symbol.isEmpty()) {
            continue;
        }
        LighterMarketMeta meta;
        meta.marketId = o.value(QStringLiteral("market_id")).toInt(-1);
        meta.sizeDecimals = o.value(QStringLiteral("size_decimals")).toInt(0);
        meta.priceDecimals = o.value(QStringLiteral("price_decimals")).toInt(0);
        meta.minBaseAmount = o.value(QStringLiteral("min_base_amount")).toDouble(0.0);
        meta.minQuoteAmount = o.value(QStringLiteral("min_quote_amount")).toDouble(0.0);
        meta.minInitialMarginFraction = o.value(QStringLiteral("min_initial_margin_fraction")).toInt(0);
        const QJsonValue lastTradeVal = o.value(QStringLiteral("last_trade_price"));
        if (lastTradeVal.isString()) {
            QString lastStr = lastTradeVal.toString();
            lastStr.replace(QLatin1Char(','), QLatin1Char('.'));
            meta.lastTradePrice = lastStr.toDouble();
        } else if (lastTradeVal.isDouble()) {
            meta.lastTradePrice = lastTradeVal.toDouble();
        }
        meta.isSpot = isSpot;
        if (meta.marketId >= 0) {
            dst.insert(symbol, meta);
        }
    }
}

QString findLighterSignerDll(const QString &configDir)
{
    const QString fileName = QStringLiteral("lighter-signer-windows-amd64.dll");
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath(fileName),
        QDir(appDir).filePath(QStringLiteral("lighter-signer.dll")),
        QDir(configDir).filePath(fileName),
        QDir(configDir).filePath(QStringLiteral("lighter-signer.dll")),
    };
    for (const QString &p : candidates) {
        if (QFile::exists(p)) {
            return p;
        }
    }
    return QString();
}

QString defaultLighterSignerTargetPath(const QString &configDir)
{
    return QDir(configDir).filePath(QStringLiteral("lighter-signer-windows-amd64.dll"));
}

static QUrl lighterUrl(const QString &baseUrl, const QString &path)
{
    QUrl url(normalizeLighterUrl(baseUrl) + path);
    return url;
}

struct ProtoReader
{
    ProtoReader(const void *ptr, std::size_t len)
        : data(static_cast<const quint8 *>(ptr))
        , size(len)
    {
    }

    bool eof() const { return pos >= size; }

    bool readVarint(quint64 &out)
    {
        out = 0;
        int shift = 0;
        while (pos < size && shift < 64) {
            const quint8 byte = data[pos++];
            out |= quint64(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return true;
            }
            shift += 7;
        }
        return false;
    }

    bool readLengthDelimited(QByteArray &out)
    {
        quint64 len = 0;
        if (!readVarint(len)) {
            return false;
        }
        if (pos + len > size) {
            return false;
        }
        out = QByteArray(reinterpret_cast<const char *>(data + pos), static_cast<int>(len));
        pos += static_cast<std::size_t>(len);
        return true;
    }

    bool skipField(quint64 key)
    {
        const auto wire = key & 0x7;
        switch (wire) {
        case 0: {
            quint64 dummy = 0;
            return readVarint(dummy);
        }
        case 1:
            if (pos + 8 > size) {
                return false;
            }
            pos += 8;
            return true;
        case 2: {
            quint64 len = 0;
            if (!readVarint(len) || pos + len > size) {
                return false;
            }
            pos += static_cast<std::size_t>(len);
            return true;
        }
        case 5:
            if (pos + 4 > size) {
                return false;
            }
            pos += 4;
            return true;
        default:
            return false;
        }
    }

    const quint8 *data = nullptr;
    std::size_t size = 0;
    std::size_t pos = 0;
};

enum class PushBodyType { None, PrivateOrders, PrivateDeals, PrivateAccount };

struct PushMessage
{
    PushBodyType type = PushBodyType::None;
    QByteArray body;
    QString channel;
    QString symbol;
    qint64 sendTime = 0;
};

struct PrivateDealEvent
{
    double price = 0.0;
    double quantity = 0.0;
    int tradeType = 0;
    QString orderId;
    QString clientOrderId;
    qint64 time = 0;
    QString feeCurrency;
    double feeAmount = 0.0;
};

struct PrivateOrderEvent
{
    QString id;
    QString clientId;
    double price = 0.0;
    double quantity = 0.0;
    double avgPrice = 0.0;
    double remainQuantity = 0.0;
    double cumulativeQuantity = 0.0;
    double cumulativeAmount = 0.0;
    int status = 0;
    int tradeType = 0;
    qint64 createTime = 0;
};

struct PrivateAccountEvent
{
    QString asset;
    double balance = 0.0;
    double frozen = 0.0;
    QString changeType;
    qint64 time = 0;
};

double parseDecimal(const QByteArray &value)
{
    bool ok = false;
    const double v = QString::fromUtf8(value).toDouble(&ok);
    return ok ? v : 0.0;
}

QString parseString(const QByteArray &value)
{
    return QString::fromUtf8(value);
}

bool parsePushMessage(const QByteArray &payload, PushMessage &out)
{
    ProtoReader reader(payload.constData(), static_cast<std::size_t>(payload.size()));
    while (!reader.eof()) {
        quint64 key = 0;
        if (!reader.readVarint(key)) {
            return false;
        }
        const auto field = key >> 3;
        const auto wire = key & 0x7;
        if (wire == 2) {
            QByteArray value;
            if (!reader.readLengthDelimited(value)) {
                return false;
            }
            switch (field) {
            case 1:
                out.channel = parseString(value);
                break;
            case 3:
                out.symbol = parseString(value);
                break;
            case 304:
                out.type = PushBodyType::PrivateOrders;
                out.body = value;
                break;
            case 306:
                out.type = PushBodyType::PrivateDeals;
                out.body = value;
                break;
            case 307:
                out.type = PushBodyType::PrivateAccount;
                out.body = value;
                break;
            default:
                break;
            }
        } else if (wire == 0) {
            quint64 value = 0;
            if (!reader.readVarint(value)) {
                return false;
            }
            if (field == 6) {
                out.sendTime = static_cast<qint64>(value);
            }
        } else {
            if (!reader.skipField(key)) {
                return false;
            }
        }
    }
    return true;
}

bool parsePrivateDealBody(const QByteArray &payload, PrivateDealEvent &out)
{
    ProtoReader reader(payload.constData(), static_cast<std::size_t>(payload.size()));
    while (!reader.eof()) {
        quint64 key = 0;
        if (!reader.readVarint(key)) {
            return false;
        }
        const auto field = key >> 3;
        const auto wire = key & 0x7;
        if (wire == 2) {
            QByteArray value;
            if (!reader.readLengthDelimited(value)) {
                return false;
            }
            switch (field) {
            case 1: // price
                out.price = parseDecimal(value);
                break;
            case 2: // quantity
                out.quantity = parseDecimal(value);
                break;
            case 8: // clientOrderId
                out.clientOrderId = parseString(value);
                break;
            case 9: // orderId
                out.orderId = parseString(value);
                break;
            case 10: // feeAmount
                out.feeAmount = parseDecimal(value);
                break;
            case 11: // feeCurrency
                out.feeCurrency = parseString(value);
                break;
            default:
                break;
            }
        } else if (wire == 0) {
            quint64 v = 0;
            if (!reader.readVarint(v)) {
                return false;
            }
            switch (field) {
            case 4: // tradeType
                out.tradeType = static_cast<int>(v);
                break;
            case 12: // time
                out.time = static_cast<qint64>(v);
                break;
            default:
                break;
            }
        } else {
            if (!reader.skipField(key)) {
                return false;
            }
        }
    }
    return true;
}

bool parsePrivateOrderBody(const QByteArray &payload, PrivateOrderEvent &out)
{
    ProtoReader reader(payload.constData(), static_cast<std::size_t>(payload.size()));
    while (!reader.eof()) {
        quint64 key = 0;
        if (!reader.readVarint(key)) {
            return false;
        }
        const auto field = key >> 3;
        const auto wire = key & 0x7;
        if (wire == 2) {
            QByteArray value;
            if (!reader.readLengthDelimited(value)) {
                return false;
            }
            switch (field) {
            case 1:
                out.id = parseString(value);
                break;
            case 2:
                out.clientId = parseString(value);
                break;
            case 3:
                out.price = parseDecimal(value);
                break;
            case 4:
                out.quantity = parseDecimal(value);
                break;
            case 5:
                out.avgPrice = parseDecimal(value);
                break;
            case 11:
                out.remainQuantity = parseDecimal(value);
                break;
            case 13:
                out.cumulativeQuantity = parseDecimal(value);
                break;
            case 14:
                out.cumulativeAmount = parseDecimal(value);
                break;
            default:
                break;
            }
        } else if (wire == 0) {
            quint64 v = 0;
            if (!reader.readVarint(v)) {
                return false;
            }
            switch (field) {
            case 8: // tradeType
                out.tradeType = static_cast<int>(v);
                break;
            case 15: // status
                out.status = static_cast<int>(v);
                break;
            case 16: // createTime
                out.createTime = static_cast<qint64>(v);
                break;
            default:
                break;
            }
        } else {
            if (!reader.skipField(key)) {
                return false;
            }
        }
    }
    return true;
}

bool parsePrivateAccountBody(const QByteArray &payload, PrivateAccountEvent &out)
{
    ProtoReader reader(payload.constData(), static_cast<std::size_t>(payload.size()));
    while (!reader.eof()) {
        quint64 key = 0;
        if (!reader.readVarint(key)) {
            return false;
        }
        const auto field = key >> 3;
        const auto wire = key & 0x7;
        if (wire == 2) {
            QByteArray value;
            if (!reader.readLengthDelimited(value)) {
                return false;
            }
            switch (field) {
            case 1: // vcoinName
                out.asset = parseString(value);
                break;
            case 3: // balanceAmount
                out.balance = parseDecimal(value);
                break;
            case 5: // frozenAmount
                out.frozen = parseDecimal(value);
                break;
            case 7: // type
                out.changeType = parseString(value);
                break;
            default:
                break;
            }
        } else if (wire == 0) {
            quint64 v = 0;
            if (!reader.readVarint(v)) {
                return false;
            }
            if (field == 8) { // time
                out.time = static_cast<qint64>(v);
            }
        } else {
            if (!reader.skipField(key)) {
                return false;
            }
        }
    }
    return true;
}

QString statusText(int status)
{
    switch (status) {
    case 2:
        return QStringLiteral("FILLED");
    case 4:
        return QStringLiteral("CANCELED");
    case 5:
        return QStringLiteral("PARTIALLY_CANCELED");
    default:
        return QString::number(status);
    }
}

QString contextTag(const QString &accountName)
{
    QString label = accountName;
    if (label.isEmpty()) {
        label = QStringLiteral("account");
    }
    return QStringLiteral("[%1]").arg(label);
}

QString uzxWireSymbol(const QString &userSymbol, bool isSwap)
{
    QString sym = userSymbol.trimmed().toUpper();
    if (sym.isEmpty()) return sym;
    if (isSwap) {
        return sym.replace(QStringLiteral("-"), QString());
    }
    if (!sym.contains(QLatin1Char('-'))) {
        static const QStringList quotes = {
            QStringLiteral("USDT"), QStringLiteral("USDC"), QStringLiteral("USDR"),
            QStringLiteral("USDQ"), QStringLiteral("EURQ"), QStringLiteral("EURR"),
            QStringLiteral("BTC"), QStringLiteral("ETH")};
        for (const QString &q : quotes) {
            if (sym.endsWith(q, Qt::CaseInsensitive)) {
                const QString base = sym.left(sym.size() - q.size());
                if (!base.isEmpty()) {
                    sym = base + QLatin1Char('-') + q;
                }
                break;
            }
        }
    }
    return sym;
}

QString uzxUserSymbol(const QString &wireSymbol)
{
    QString sym = wireSymbol.trimmed().toUpper();
    sym.remove(QLatin1Char('-'));
    return sym;
}

QString uzxBaseAsset(const QString &userSymbol)
{
    QString wire = uzxWireSymbol(userSymbol, false);
    const int dash = wire.indexOf(QLatin1Char('-'));
    if (dash > 0) {
        return wire.left(dash).trimmed().toUpper();
    }
    return wire.trimmed().toUpper();
}


QString uzxErrorDescription(int code)
{
    static const QHash<int, QString> map = {
        {1080001, QStringLiteral("Invalid parameter")},
        {1080002, QStringLiteral("Trading pair cannot be empty")},
        {1080003, QStringLiteral("Invalid cancel order type")},
        {1080004, QStringLiteral("Invalid order type")},
        {1080005, QStringLiteral("Invalid quantity")},
        {1080006, QStringLiteral("Invalid take-profit or stop-loss quantity")},
        {1080007, QStringLiteral("Invalid position direction")},
        {1080008, QStringLiteral("Trade direction does not match position direction")},
        {1080009, QStringLiteral("Invalid position mode")},
        {1080010, QStringLiteral("Invalid leverage")},
        {1080011, QStringLiteral("Invalid take-profit or stop-loss parameters")},
        {1080012, QStringLiteral("Invalid margin quantity")},
        {1080013, QStringLiteral("Invalid price")},
        {1080014, QStringLiteral("Invalid take-profit or stop-loss type")},
        {1080017, QStringLiteral("Invalid order ID")},
        {1080020, QStringLiteral("Exceeded maximum bulk order quantity")},
        {1080021, QStringLiteral("Inconsistent products in bulk orders")},
        {1080023, QStringLiteral("Cannot modify margin mode while holding positions")},
        {1080024, QStringLiteral("Cannot modify margin mode while orders exist")},
        {2080001, QStringLiteral("Trading pair not found")},
        {2080003, QStringLiteral("Trading pair not enabled")},
        {2080004, QStringLiteral("Internal server error")},
        {2080005, QStringLiteral("Internal server error")},
        {2080006, QStringLiteral("Internal server error")},
        {2080007, QStringLiteral("Internal server error")},
        {3080001, QStringLiteral("Internal server error")},
        {3080002, QStringLiteral("Internal server error")},
        {1010002, QStringLiteral("Parameter validation failed")},
        {1010003, QStringLiteral("Parameter validation failed")},
        {1010004, QStringLiteral("Insufficient available assets")},
        {1010005, QStringLiteral("Existing positions or orders with different leverage")},
        {1010006, QStringLiteral("Order frequency too high")},
        {1010007, QStringLiteral("Existing positions or orders with different margin modes")},
        {1010008, QStringLiteral("Insufficient margin")},
        {1010009, QStringLiteral("Exceeded maximum order quantity")},
        {1010010, QStringLiteral("Exceeded maximum position size")},
        {1010011, QStringLiteral("Insufficient position to close")},
        {1010012, QStringLiteral("Price validation failed")},
        {1010013, QStringLiteral("Order rejected due to price being worse than liquidation price")},
        {1010014, QStringLiteral("Margin freeze failed")},
        {1010015, QStringLiteral("Order cancellation not allowed for this type")},
        {1010016, QStringLiteral("Position not found")},
        {1010017, QStringLiteral("Take-profit or stop-loss quantity exceeds position size")},
        {1010018, QStringLiteral("One-click close position timeout")},
        {1010019, QStringLiteral("Only one full-position take-profit or stop-loss allowed per position")},
        {1010020, QStringLiteral("Existing orders or take-profit/stop-loss orders with different leverage")},
        {1010021, QStringLiteral("Take-profit or stop-loss price validation failed")},
        {1010022, QStringLiteral("Margin unfreeze failed")},
        {1010023, QStringLiteral("Existing positions or orders with different margin modes")},
        {1010024, QStringLiteral("Limit order price cannot be higher than the highest bid")},
        {1010025, QStringLiteral("Limit order price cannot be lower than the lowest ask")},
        {1010026, QStringLiteral("Margin mode validation failed")},
        {1010027, QStringLiteral("Leverage validation failed")},
        {1010028, QStringLiteral("Order placement failed")},
        {1010029, QStringLiteral("Parameter validation failed")},
        {1010030, QStringLiteral("Order placement failed")},
        {1010031, QStringLiteral("Price comparison failed; please set a reasonable take-profit trigger price")},
        {1010032, QStringLiteral("Price comparison failed; please set a reasonable stop-loss trigger price")},
        {1010033, QStringLiteral("Parameter validation failed")},
        {1010034, QStringLiteral("Limit take-profit order price must be higher than the last traded price")},
        {1010035, QStringLiteral("Limit take-profit order price must be lower than the last traded price")},
        {1010036, QStringLiteral("Please set the order price for limit orders")},
        {1010037, QStringLiteral("Please set a take-profit or stop-loss trigger")},
        {1010038, QStringLiteral("Parameter validation failed")},
        {1010039, QStringLiteral("Parameter validation failed")},
        {1010040, QStringLiteral("Trailing stop-loss/take-profit callback range is 0.1-100")},
        {1010041, QStringLiteral("Trailing stop-loss/take-profit price distance must be a positive integer")},
        {1010042, QStringLiteral("Trailing stop-loss/take-profit activation price must be a positive number")},
        {1010043, QStringLiteral("Parameter validation failed")},
        {1010044, QStringLiteral("Parameter validation failed")},
        {1010045, QStringLiteral("Internal server error")},
        {1010046, QStringLiteral("Parameter validation failed")},
        {1010047, QStringLiteral("Increase or decrease margin failed")},
        {1010048, QStringLiteral("Order not found")},
        {1010049, QStringLiteral("Internal server error")},
        {1010050, QStringLiteral("Operation on position not allowed in the current state")},
        {1010051, QStringLiteral("Account is in liquidation state and cannot perform operations")},
        {1011001, QStringLiteral("Trading pair is offline")},
        {1011002, QStringLiteral("Trading pair is not open for trading")},
        {1011003, QStringLiteral("Failed to take trading pair offline")},
        {1011004, QStringLiteral("Successfully took trading pair offline")},
        {1011005, QStringLiteral("Trading pair is suspended from trading")},
        {1011006, QStringLiteral("Only limit sell orders are allowed before the market opens")},
        {1011007, QStringLiteral("Parameter validation failed")},
        {1011008, QStringLiteral("Minimum trade amount not met")},
        {1011009, QStringLiteral("Maximum trade amount exceeded")},
        {1011010, QStringLiteral("Minimum trade quantity not met")},
        {1011011, QStringLiteral("Maximum trade quantity exceeded")},
        {1011200, QStringLiteral("Internal server error")},
        {1011201, QStringLiteral("Internal server error")},
        {3010001, QStringLiteral("Internal server error")},
        {3010005, QStringLiteral("Internal server error")}
    };
    const auto it = map.find(code);
    if (it != map.end()) {
        return it.value();
    }
    return QString();
}

QString uzxFormatError(int code, const QString &message)
{
    const QString trimmed = message.trimmed();
    const QString desc = uzxErrorDescription(code);
    if (trimmed.isEmpty() || trimmed == QStringLiteral("request error")) {
        if (!desc.isEmpty()) {
            return desc;
        }
        if (!trimmed.isEmpty()) {
            return trimmed;
        }
        return QStringLiteral("UZX request error");
    }
    if (!desc.isEmpty() && !trimmed.contains(desc, Qt::CaseInsensitive)) {
        return QStringLiteral("%1 (%2)").arg(trimmed, desc);
    }
    return trimmed;
}

} // namespace

qint64 TradeManager::nextLighterClientOrderIndex(Context &ctx)
{
    // Lighter signer expects a client order index (used by private WS as `clientId`).
    // Keep it positive and monotonic (best-effort) to avoid collisions.
    static constexpr qint64 kMax = 0x7fffffffLL;
    qint64 idx = QDateTime::currentMSecsSinceEpoch() & kMax;
    if (idx <= 0) {
        idx = 1;
    }
    if (ctx.lighterLastClientOrderIndex > 0 && idx <= ctx.lighterLastClientOrderIndex) {
        idx = ctx.lighterLastClientOrderIndex + 1;
    }
    if (idx > kMax) {
        idx = 1;
    }
    ctx.lighterLastClientOrderIndex = idx;
    return idx;
}

static QUrl lighterStreamUrlFromBaseUrl(const QString &baseUrl)
{
    // baseUrl: https://mainnet.zklighter.elliot.ai  -> wss://mainnet.zklighter.elliot.ai/stream
    QString url = baseUrl.trimmed();
    if (url.isEmpty()) {
        return {};
    }
    if (url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        url.replace(0, 5, QStringLiteral("wss"));
    } else if (url.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive)) {
        url.replace(0, 4, QStringLiteral("ws"));
    }
    if (!url.endsWith(QStringLiteral("/stream"), Qt::CaseInsensitive)) {
        if (!url.endsWith(QLatin1Char('/'))) {
            url += QLatin1Char('/');
        }
        url += QStringLiteral("stream");
    }
    return QUrl(url);
}

static bool parseHttpProxy(const QString &raw, const QString &proxyType, QNetworkProxy &outProxy, QString *errOut)
{
    const QString s = raw.trimmed();
    if (s.isEmpty()) {
        // Empty field means: use OS/system proxy settings (instead of forcing direct).
        outProxy = QNetworkProxy(QNetworkProxy::DefaultProxy);
        return true;
    }
    if (s.compare(QStringLiteral("disabled"), Qt::CaseInsensitive) == 0
        || s.compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0
        || s.compare(QStringLiteral("direct"), Qt::CaseInsensitive) == 0) {
        outProxy = QNetworkProxy(QNetworkProxy::NoProxy);
        return true;
    }

    const QString t = proxyType.trimmed().toLower();
    const bool isSocks = (t == QStringLiteral("socks5") || t == QStringLiteral("socks"));

    auto setErr = [&](const QString &msg) {
        if (errOut) {
            *errOut = msg;
        }
    };

    auto make = [&](const QString &host, int port, const QString &user, const QString &pass) -> bool {
        if (host.trimmed().isEmpty() || port <= 0 || port > 65535) {
            setErr(QStringLiteral("Invalid host/port"));
            return false;
        }
        QNetworkProxy p(isSocks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                        host.trimmed(),
                        static_cast<quint16>(port));
        if (!user.isEmpty()) {
            p.setUser(user);
            p.setPassword(pass);
        }
        outProxy = p;
        return true;
    };

    if (s.contains(QStringLiteral("://"))) {
        const QUrl u(s);
        if (!u.isValid() || u.host().isEmpty() || u.port() <= 0) {
            setErr(QStringLiteral("Invalid proxy URL"));
            return false;
        }
        const QString scheme = u.scheme().toLower();
        QString effectiveType = t;
        if (scheme.contains(QStringLiteral("socks"))) {
            effectiveType = QStringLiteral("socks5");
        }
        QNetworkProxy tmp;
        QString tmpErr;
        if (!parseHttpProxy(QStringLiteral("%1:%2").arg(u.host()).arg(u.port()), effectiveType, tmp, &tmpErr)) {
            setErr(tmpErr.isEmpty() ? QStringLiteral("Invalid proxy URL") : tmpErr);
            return false;
        }
        // preserve user/pass from URL
        return make(u.host(), u.port(), u.userName(), u.password());
    }

    if (s.contains(QLatin1Char('@'))) {
        const QStringList parts = s.split(QLatin1Char('@'));
        if (parts.size() != 2) {
            setErr(QStringLiteral("Invalid proxy format"));
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
                setErr(QStringLiteral("Invalid credentials format"));
                return false;
            }
            return make(host, port, user, pass);
        }
        if (parseHostPort(b, host, port)) {
            if (!parseUserPass(a, user, pass)) {
                setErr(QStringLiteral("Invalid credentials format"));
                return false;
            }
            return make(host, port, user, pass);
        }
        setErr(QStringLiteral("Invalid proxy format"));
        return false;
    }

    const QStringList tokens = s.split(QLatin1Char(':'), Qt::KeepEmptyParts);
    if (tokens.size() == 2) {
        bool ok = false;
        const int port = tokens[1].toInt(&ok);
        if (!ok) {
            setErr(QStringLiteral("Invalid port"));
            return false;
        }
        return make(tokens[0], port, QString(), QString());
    }
    if (tokens.size() == 4) {
        bool ok1 = false;
        const int port1 = tokens[1].toInt(&ok1);
        if (ok1) {
            return make(tokens[0], port1, tokens[2], tokens[3]);
        }
        bool ok2 = false;
        const int port2 = tokens[3].toInt(&ok2);
        if (ok2) {
            return make(tokens[2], port2, tokens[0], tokens[1]);
        }
        setErr(QStringLiteral("Invalid proxy format"));
        return false;
    }

    setErr(QStringLiteral("Invalid proxy format"));
    return false;
}

static QString proxySummaryForLog(const QNetworkProxy &proxy)
{
    if (proxy.type() == QNetworkProxy::NoProxy) {
        return QStringLiteral("disabled");
    }
    if (proxy.type() == QNetworkProxy::DefaultProxy) {
        return QStringLiteral("system");
    }
    const QString proto =
        (proxy.type() == QNetworkProxy::Socks5Proxy) ? QStringLiteral("SOCKS5") : QStringLiteral("HTTP");
    const QString auth = proxy.user().isEmpty() ? QString() : QStringLiteral(" auth");
    return QStringLiteral("%1 %2:%3%4")
        .arg(proto, proxy.hostName(), QString::number(proxy.port()), auth);
}

static void runProxyPreflight(QObject *owner,
                              const QNetworkProxy &proxy,
                              std::function<void(int okCount, const QHash<QString, bool> &results)> cb)
{
    auto *nam = new QNetworkAccessManager(owner);
    nam->setProxy(proxy);

    struct Target {
        QString name;
        QUrl url;
    };
    const QVector<Target> targets{
        {QStringLiteral("google"), QUrl(QStringLiteral("https://www.google.com/generate_204"))},
        {QStringLiteral("facebook"), QUrl(QStringLiteral("https://www.facebook.com/favicon.ico"))},
        {QStringLiteral("yandex"), QUrl(QStringLiteral("https://yandex.ru/"))},
    };

    auto remaining = std::make_shared<int>(targets.size());
    auto okCount = std::make_shared<int>(0);
    auto results = std::make_shared<QHash<QString, bool>>();

    auto finishOne = [nam, remaining, okCount, results, cb = std::move(cb)]() mutable {
        (*remaining)--;
        if (*remaining > 0) {
            return;
        }
        cb(*okCount, *results);
        nam->deleteLater();
    };

    for (const auto &t : targets) {
        QNetworkRequest req(t.url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto *reply = nam->get(req);

        auto *timer = new QTimer(reply);
        timer->setSingleShot(true);
        timer->start(3500);
        QObject::connect(timer, &QTimer::timeout, reply, [reply]() {
            reply->abort();
        });

        QObject::connect(reply, &QNetworkReply::finished, owner, [reply, t, okCount, results, finishOne]() mutable {
            const auto err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
            const bool ok = (err == QNetworkReply::NoError) && (status > 0) && (status < 400);
            (*results)[t.name] = ok;
            if (ok) {
                (*okCount)++;
            }
            finishOne();
        });
    }
}

QNetworkAccessManager *TradeManager::ensureMexcNetwork(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::MexcSpot && ctx.profile != ConnectionStore::Profile::MexcFutures) {
        return &m_network;
    }
    if (!ctx.mexcNetwork) {
        ctx.mexcNetwork = new QNetworkAccessManager(this);
    }

    QNetworkProxy proxy;
    QString err;
    const QString proxyRaw = ctx.credentials.proxy.trimmed();
    if (!parseHttpProxy(proxyRaw, ctx.credentials.proxyType, proxy, &err)) {
        if (!proxyRaw.isEmpty()) {
            emit logMessage(QStringLiteral("%1 Invalid proxy: %2").arg(contextTag(ctx.accountName), err));
        }
        proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    }

    ctx.mexcNetwork->setProxy(proxy);
    ctx.privateSocket.setProxy(proxy);

    const QString status = proxySummaryForLog(proxy);
    const QString logKey = QStringLiteral("mexc:%1").arg(status);
    if (ctx.proxyStatusLog != logKey) {
        ctx.proxyStatusLog = logKey;
        emit logMessage(QStringLiteral("%1 Proxy: %2").arg(contextTag(ctx.accountName), status));
    }

    return ctx.mexcNetwork;
}

QNetworkAccessManager *TradeManager::ensureLighterNetwork(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return &m_network;
    }
    if (!ctx.lighterNetwork) {
        ctx.lighterNetwork = new QNetworkAccessManager(this);
    }
    QNetworkProxy proxy;
    QString err;
    const QString proxyRaw = ctx.credentials.proxy.trimmed();
    if (!parseHttpProxy(proxyRaw, ctx.credentials.proxyType, proxy, &err)) {
        if (!proxyRaw.isEmpty()) {
            emit logMessage(QStringLiteral("%1 Invalid proxy: %2").arg(contextTag(ctx.accountName), err));
        }
        proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    }
    ctx.lighterNetwork->setProxy(proxy);
    ctx.lighterStreamSocket.setProxy(proxy);

    const QString status = proxySummaryForLog(proxy);
    const QString logKey = QStringLiteral("lighter:%1").arg(status);
    if (ctx.proxyStatusLog != logKey) {
        ctx.proxyStatusLog = logKey;
        emit logMessage(QStringLiteral("%1 Proxy: %2").arg(contextTag(ctx.accountName), status));
    }
    return ctx.lighterNetwork;
}

void TradeManager::ensureLighterStreamWired(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (ctx.lighterStreamSocket.property("fusion_wired").toBool()) {
        return;
    }
    ctx.lighterStreamSocket.setProperty("fusion_wired", true);

    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);

    auto httpSend = [this, &ctx, baseUrl](int txType,
                                    const QString &txInfo,
                                    std::function<void(QString txHash, QString err)> cb) {
        QElapsedTimer t;
        t.start();
        QNetworkRequest req(lighterUrl(baseUrl, QStringLiteral("/api/v1/sendTx")));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        QHttpPart typePart;
        typePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QStringLiteral("form-data; name=\"tx_type\"")));
        typePart.setBody(QByteArray::number(txType));
        multi->append(typePart);

        QHttpPart infoPart;
        infoPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QStringLiteral("form-data; name=\"tx_info\"")));
        infoPart.setBody(txInfo.toUtf8());
        multi->append(infoPart);

        QNetworkReply *reply = ensureLighterNetwork(ctx)->post(req, multi);
        applyReplyTimeout(reply, 4500);
        multi->setParent(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, cb = std::move(cb), t]() mutable {
            const QNetworkReply::NetworkError err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                cb(QString(), lighterHttpErrorMessage(reply, raw));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isNull() || !doc.isObject()) {
                cb(QString(), QStringLiteral("Invalid Lighter sendTx response"));
                return;
            }
            const QJsonObject obj = doc.object();
            const int code = obj.value(QStringLiteral("code")).toInt(0);
            if (code != 200) {
                cb(QString(),
                   obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter sendTx failed")));
                return;
            }
            emit logMessage(QStringLiteral("Lighter sendTx RTT: %1 ms").arg(t.elapsed()));
            cb(obj.value(QStringLiteral("tx_hash")).toString(), QString());
        });
    };

    connect(&ctx.lighterStreamSocket, &QWebSocket::connected, this, [this, ctxPtr = &ctx]() {
        if (!ctxPtr) {
            return;
        }
        ctxPtr->lighterStreamConnecting = false;
        ctxPtr->lighterStreamConnected = true;
        ctxPtr->lighterStreamReady = false; // wait for "connected" message from server
        emit logMessage(QStringLiteral("%1 Lighter WS connected").arg(contextTag(ctxPtr->accountName)));
    });

    connect(&ctx.lighterStreamSocket,
            &QWebSocket::textMessageReceived,
            this,
            [this, ctxPtr = &ctx, httpSend](const QString &message) mutable {
                if (!ctxPtr) {
                    return;
                }
                QJsonParseError pe;
                const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &pe);
                if (doc.isNull() || !doc.isObject()) {
                    return;
                }
                const QJsonObject obj = doc.object();
                const QString type = obj.value(QStringLiteral("type")).toString();
                if (type == QStringLiteral("connected")) {
                    ctxPtr->lighterStreamReady = true;
                    subscribeLighterPrivateWs(*ctxPtr);
                    // Send any queued tx now that the server is ready.
                    for (auto it = ctxPtr->lighterWsTxWaiters.begin(); it != ctxPtr->lighterWsTxWaiters.end(); ++it) {
                        if (it.value().sent) {
                            continue;
                        }
                        const QJsonDocument txDoc2 = QJsonDocument::fromJson(it.value().txInfo.toUtf8());
                        if (txDoc2.isNull() || !txDoc2.isObject()) {
                            continue;
                        }
                        QJsonObject msg2;
                        msg2.insert(QStringLiteral("type"), QStringLiteral("jsonapi/sendtx"));
                        QJsonObject data2;
                        data2.insert(QStringLiteral("id"), it.key());
                        data2.insert(QStringLiteral("tx_type"), it.value().txType);
                        data2.insert(QStringLiteral("tx_info"), txDoc2.object());
                        msg2.insert(QStringLiteral("data"), data2);
                        it.value().startMs = QDateTime::currentMSecsSinceEpoch();
                        ctxPtr->lighterStreamSocket.sendTextMessage(
                            QString::fromUtf8(QJsonDocument(msg2).toJson(QJsonDocument::Compact)));
                        it.value().sent = true;
                    }
                    return;
                }
                if (type == QStringLiteral("ping")) {
                    ctxPtr->lighterStreamSocket.sendTextMessage(QStringLiteral("{\"type\":\"pong\"}"));
                    return;
                }

                const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
                QString id = data.value(QStringLiteral("id")).toString();
                if (id.isEmpty()) {
                    id = obj.value(QStringLiteral("id")).toString();
                }
                if (id.isEmpty()) {
                    return;
                }
                auto it = ctxPtr->lighterWsTxWaiters.find(id);
                if (it == ctxPtr->lighterWsTxWaiters.end()) {
                    return;
                }
                auto cb2 = std::move(it.value().cb);
                const qint64 startMs = it.value().startMs > 0 ? it.value().startMs : QDateTime::currentMSecsSinceEpoch();
                const qint64 elapsed = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startMs);
                ctxPtr->lighterWsTxWaiters.erase(it);

                const int code = data.value(QStringLiteral("code")).toInt(obj.value(QStringLiteral("code")).toInt(0));
                const QString txHash = data.value(QStringLiteral("tx_hash")).toString(obj.value(QStringLiteral("tx_hash")).toString());
                const QString msg = data.value(QStringLiteral("message")).toString(obj.value(QStringLiteral("message")).toString());
                if (code != 200) {
                    if (code == 0 && !txHash.isEmpty()) {
                        emit logMessage(QStringLiteral("Lighter WS sendTx RTT: %1 ms").arg(elapsed));
                        cb2(txHash, QString());
                        return;
                    }
                    const QString rawMsg = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
                    cb2(QString(), msg.isEmpty() ? QStringLiteral("Lighter WS sendTx failed: %1").arg(rawMsg)
                                                 : QStringLiteral("%1 (%2)").arg(msg, rawMsg));
                    return;
                }
                emit logMessage(QStringLiteral("Lighter WS sendTx RTT: %1 ms").arg(elapsed));
                cb2(txHash, QString());
            });

    connect(&ctx.lighterStreamSocket, &QWebSocket::disconnected, this, [this, ctxPtr = &ctx, httpSend]() mutable {
        if (!ctxPtr) {
            return;
        }
        ctxPtr->lighterStreamConnected = false;
        ctxPtr->lighterStreamConnecting = false;
        ctxPtr->lighterStreamReady = false;
        ctxPtr->lighterPrivateSubscribed = false;
        emit logMessage(QStringLiteral("%1 Lighter WS disconnected").arg(contextTag(ctxPtr->accountName)));

        // Fast fallback for any in-flight tx.
        const auto pending = ctxPtr->lighterWsTxWaiters.keys();
        for (const auto &id : pending) {
            auto it = ctxPtr->lighterWsTxWaiters.find(id);
            if (it == ctxPtr->lighterWsTxWaiters.end()) {
                continue;
            }
            auto cb2 = std::move(it.value().cb);
            const int txType2 = it.value().txType;
            const QString txInfo2 = it.value().txInfo;
            ctxPtr->lighterWsTxWaiters.erase(it);
            emit logMessage(QStringLiteral("%1 Lighter WS disconnected; falling back to HTTP").arg(contextTag(ctxPtr->accountName)));
            httpSend(txType2, txInfo2, std::move(cb2));
        }
    });

    connect(&ctx.lighterStreamSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
            this,
            [this, ctxPtr = &ctx, httpSend](QAbstractSocket::SocketError) mutable {
                if (!ctxPtr) {
                    return;
                }
                ctxPtr->lighterStreamConnected = false;
                ctxPtr->lighterStreamConnecting = false;
                ctxPtr->lighterStreamReady = false;
                ctxPtr->lighterPrivateSubscribed = false;
                emit logMessage(QStringLiteral("%1 Lighter WS error: %2")
                                    .arg(contextTag(ctxPtr->accountName), ctxPtr->lighterStreamSocket.errorString()));

                // Some handshake failures never emit `disconnected`; fall back immediately so
                // order placement doesn't stall behind the WS timeout.
                const auto pending = ctxPtr->lighterWsTxWaiters.keys();
                for (const auto &id : pending) {
                    auto it = ctxPtr->lighterWsTxWaiters.find(id);
                    if (it == ctxPtr->lighterWsTxWaiters.end()) {
                        continue;
                    }
                    auto cb2 = std::move(it.value().cb);
                    const int txType2 = it.value().txType;
                    const QString txInfo2 = it.value().txInfo;
                    ctxPtr->lighterWsTxWaiters.erase(it);
                    emit logMessage(QStringLiteral("%1 Lighter WS error; falling back to HTTP").arg(contextTag(ctxPtr->accountName)));
                    httpSend(txType2, txInfo2, std::move(cb2));
                }
            });
}

void TradeManager::ensureLighterStreamOpen(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    ensureLighterNetwork(ctx);
    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    if (baseUrl.isEmpty()) {
        return;
    }
    if (ctx.lighterStreamConnected || ctx.lighterStreamConnecting) {
        return;
    }
    ctx.lighterStreamConnecting = true;
    const QUrl wsUrl = lighterStreamUrlFromBaseUrl(baseUrl);
    emit logMessage(QStringLiteral("%1 Lighter WS connecting: %2")
                        .arg(contextTag(ctx.accountName), wsUrl.toString()));
    ctx.lighterStreamSocket.open(wsUrl);
}

void TradeManager::sendLighterTx(Context &ctx,
                                int txType,
                                const QString &txInfo,
                                std::function<void(QString txHash, QString err)> cb)
{
    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    auto httpSend = [this, &ctx, baseUrl](int txType2,
                                    const QString &txInfo2,
                                    std::function<void(QString txHash, QString err)> cb2) {
        QElapsedTimer t;
        t.start();
        QNetworkRequest req(lighterUrl(baseUrl, QStringLiteral("/api/v1/sendTx")));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        QHttpPart typePart;
        typePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QStringLiteral("form-data; name=\"tx_type\"")));
        typePart.setBody(QByteArray::number(txType2));
        multi->append(typePart);

        QHttpPart infoPart;
        infoPart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QVariant(QStringLiteral("form-data; name=\"tx_info\"")));
        infoPart.setBody(txInfo2.toUtf8());
        multi->append(infoPart);

        QNetworkReply *reply = ensureLighterNetwork(ctx)->post(req, multi);
        applyReplyTimeout(reply, 4500);
        multi->setParent(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, cb2 = std::move(cb2), t]() mutable {
            const QNetworkReply::NetworkError err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                cb2(QString(), lighterHttpErrorMessage(reply, raw));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isNull() || !doc.isObject()) {
                cb2(QString(), QStringLiteral("Invalid Lighter sendTx response"));
                return;
            }
            const QJsonObject obj = doc.object();
            const int code = obj.value(QStringLiteral("code")).toInt(0);
            if (code != 200) {
                cb2(QString(),
                    obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter sendTx failed")));
                return;
            }
            emit logMessage(QStringLiteral("Lighter sendTx RTT: %1 ms").arg(t.elapsed()));
            cb2(obj.value(QStringLiteral("tx_hash")).toString(), QString());
        });
    };

    if (baseUrl.isEmpty()) {
        httpSend(txType, txInfo, std::move(cb));
        return;
    }

    // If WS recently failed to connect, don't stall order placement behind a long WS timeout.
    // We'll still keep trying to reconnect the stream for private updates.
    static constexpr qint64 kWsErrorBackoffMs = 15000;
    const bool wsRecentlyFailed = (ctx.lighterStreamLastErrorMs > 0 && (nowMs - ctx.lighterStreamLastErrorMs) < kWsErrorBackoffMs);

    // Try WS sendTx (jsonapi/sendtx) first; if it doesn't reply quickly, fall back to HTTP.
    const QJsonDocument txDoc = QJsonDocument::fromJson(txInfo.toUtf8());
    if (txDoc.isNull() || !txDoc.isObject()) {
        httpSend(txType, txInfo, std::move(cb));
        return;
    }

    if (wsRecentlyFailed && !ctx.lighterStreamConnected) {
        emit logMessage(QStringLiteral("%1 Lighter WS recently failed (%2); using HTTP sendTx")
                            .arg(contextTag(ctx.accountName),
                                 ctx.lighterStreamLastError.isEmpty() ? QStringLiteral("unknown") : ctx.lighterStreamLastError));
        httpSend(txType, txInfo, std::move(cb));
        return;
    }

    auto ensureSocketWired = [this, &ctx, httpSend]() mutable {
        if (ctx.lighterStreamSocket.property("fusion_wired").toBool()) {
            return;
        }
        ctx.lighterStreamSocket.setProperty("fusion_wired", true);

        connect(&ctx.lighterStreamSocket, &QWebSocket::connected, this, [this, ctxPtr = &ctx]() {
            if (!ctxPtr) {
                return;
            }
            ctxPtr->lighterStreamConnecting = false;
            ctxPtr->lighterStreamConnected = true;
            ctxPtr->lighterStreamReady = false; // wait for "connected" message from server
            emit logMessage(QStringLiteral("%1 Lighter WS connected").arg(contextTag(ctxPtr->accountName)));
        });

        connect(&ctx.lighterStreamSocket,
                &QWebSocket::textMessageReceived,
                this,
                [this, ctxPtr = &ctx, httpSend](const QString &message) mutable {
                    if (!ctxPtr) {
                        return;
                    }
                    QJsonParseError pe;
                    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &pe);
                    if (doc.isNull() || !doc.isObject()) {
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    const QString type = obj.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("connected")) {
                        ctxPtr->lighterStreamReady = true;
                        subscribeLighterPrivateWs(*ctxPtr);
                        // Now it's safe to send pending tx requests.
                        for (auto it = ctxPtr->lighterWsTxWaiters.begin(); it != ctxPtr->lighterWsTxWaiters.end(); ++it) {
                            if (it.value().sent) {
                                continue;
                            }
                            const QJsonDocument txDoc2 = QJsonDocument::fromJson(it.value().txInfo.toUtf8());
                            if (txDoc2.isNull() || !txDoc2.isObject()) {
                                continue;
                            }
                            QJsonObject msg2;
                            msg2.insert(QStringLiteral("type"), QStringLiteral("jsonapi/sendtx"));
                            QJsonObject data2;
                            data2.insert(QStringLiteral("id"), it.key());
                            data2.insert(QStringLiteral("tx_type"), it.value().txType);
                            data2.insert(QStringLiteral("tx_info"), txDoc2.object());
                            msg2.insert(QStringLiteral("data"), data2);
                            it.value().startMs = QDateTime::currentMSecsSinceEpoch();
                            ctxPtr->lighterStreamSocket.sendTextMessage(
                                QString::fromUtf8(QJsonDocument(msg2).toJson(QJsonDocument::Compact)));
                            it.value().sent = true;
                        }
                        return;
                    }
                    if (type == QStringLiteral("ping")) {
                        ctxPtr->lighterStreamSocket.sendTextMessage(QStringLiteral("{\"type\":\"pong\"}"));
                        return;
                    }
                    if (type.startsWith(QStringLiteral("subscribed/")) || type.startsWith(QStringLiteral("update/"))) {
                        // Private WS updates (positions / orders / trades) are processed in subscribeLighterPrivateWs handlers.
                        // Keep this handler lightweight; ignore unknown message types.
                        // We still may receive tx responses here (with an id).
                    }
                    const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
                    QString id = data.value(QStringLiteral("id")).toString();
                    if (id.isEmpty()) {
                        id = obj.value(QStringLiteral("id")).toString();
                    }
                    if (id.isEmpty()) {
                        return;
                    }
                    auto it = ctxPtr->lighterWsTxWaiters.find(id);
                    if (it == ctxPtr->lighterWsTxWaiters.end()) {
                        return;
                    }
                    auto cb2 = std::move(it.value().cb);
                    const qint64 startMs = it.value().startMs > 0 ? it.value().startMs : QDateTime::currentMSecsSinceEpoch();
                    const qint64 elapsed = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - startMs);
                    ctxPtr->lighterWsTxWaiters.erase(it);

                    const int code = data.value(QStringLiteral("code")).toInt(obj.value(QStringLiteral("code")).toInt(0));
                    const QString txHash = data.value(QStringLiteral("tx_hash")).toString(obj.value(QStringLiteral("tx_hash")).toString());
                    const QString msg = data.value(QStringLiteral("message")).toString(obj.value(QStringLiteral("message")).toString());
                    if (code != 200) {
                        // Some WS responses might omit `code` but include `tx_hash`. Treat that as success.
                        if (code == 0 && !txHash.isEmpty()) {
                            emit logMessage(QStringLiteral("Lighter WS sendTx RTT: %1 ms").arg(elapsed));
                            cb2(txHash, QString());
                            return;
                        }
                        const QString rawMsg = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
                        cb2(QString(), msg.isEmpty() ? QStringLiteral("Lighter WS sendTx failed: %1").arg(rawMsg)
                                                     : QStringLiteral("%1 (%2)").arg(msg, rawMsg));
                        return;
                    }
                    emit logMessage(QStringLiteral("Lighter WS sendTx RTT: %1 ms").arg(elapsed));
                    cb2(txHash, QString());
                });

        connect(&ctx.lighterStreamSocket, &QWebSocket::disconnected, this, [this, ctxPtr = &ctx, httpSend]() mutable {
            if (!ctxPtr) {
                return;
            }
            ctxPtr->lighterStreamConnected = false;
            ctxPtr->lighterStreamConnecting = false;
            ctxPtr->lighterStreamReady = false;
            ctxPtr->lighterStreamLastErrorMs = QDateTime::currentMSecsSinceEpoch();
            ctxPtr->lighterStreamLastError = QStringLiteral("disconnected");
            emit logMessage(QStringLiteral("%1 Lighter WS disconnected").arg(contextTag(ctxPtr->accountName)));

            // Fallback for any in-flight requests.
            const auto pending = ctxPtr->lighterWsTxWaiters.keys();
            for (const auto &id : pending) {
                auto it = ctxPtr->lighterWsTxWaiters.find(id);
                if (it == ctxPtr->lighterWsTxWaiters.end()) {
                    continue;
                }
                auto cb2 = std::move(it.value().cb);
                const int txType2 = it.value().txType;
                const QString txInfo2 = it.value().txInfo;
                ctxPtr->lighterWsTxWaiters.erase(it);
                emit logMessage(QStringLiteral("%1 Lighter WS disconnected; falling back to HTTP").arg(contextTag(ctxPtr->accountName)));
                httpSend(txType2, txInfo2, std::move(cb2));
            }
        });

        connect(&ctx.lighterStreamSocket,
                QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
                this,
                [this, ctxPtr = &ctx, httpSend](QAbstractSocket::SocketError) mutable {
                    if (!ctxPtr) {
                        return;
                    }
                    ctxPtr->lighterStreamConnected = false;
                    ctxPtr->lighterStreamConnecting = false;
                    ctxPtr->lighterStreamReady = false;
                    ctxPtr->lighterPrivateSubscribed = false;
                    ctxPtr->lighterStreamLastErrorMs = QDateTime::currentMSecsSinceEpoch();
                    ctxPtr->lighterStreamLastError = ctxPtr->lighterStreamSocket.errorString();
                    emit logMessage(QStringLiteral("%1 Lighter WS error: %2")
                                        .arg(contextTag(ctxPtr->accountName), ctxPtr->lighterStreamSocket.errorString()));

                    // Some handshake failures never emit `disconnected`; fall back immediately so
                    // order placement doesn't stall behind the WS timeout.
                    const auto pending = ctxPtr->lighterWsTxWaiters.keys();
                    for (const auto &id : pending) {
                        auto it = ctxPtr->lighterWsTxWaiters.find(id);
                        if (it == ctxPtr->lighterWsTxWaiters.end()) {
                            continue;
                        }
                        auto cb2 = std::move(it.value().cb);
                        const int txType2 = it.value().txType;
                        const QString txInfo2 = it.value().txInfo;
                        ctxPtr->lighterWsTxWaiters.erase(it);
                        emit logMessage(QStringLiteral("%1 Lighter WS error; falling back to HTTP").arg(contextTag(ctxPtr->accountName)));
                        httpSend(txType2, txInfo2, std::move(cb2));
                    }
                });
    };

    ensureSocketWired();

    const QString requestId =
        QStringLiteral("fusion_%1_%2")
            .arg(QDateTime::currentMSecsSinceEpoch())
            .arg(QString::number(QRandomGenerator::global()->generate()));

    Context::LighterWsTxWaiter waiter;
    waiter.startMs = 0;
    waiter.sent = false;
    waiter.txType = txType;
    waiter.txInfo = txInfo;
    waiter.cb = std::move(cb);
    ctx.lighterWsTxWaiters.insert(requestId, waiter);

    // Ensure connection.
    if (!ctx.lighterStreamConnected && !ctx.lighterStreamConnecting) {
        ctx.lighterStreamConnecting = true;
        const QUrl wsUrl = lighterStreamUrlFromBaseUrl(baseUrl);
        emit logMessage(QStringLiteral("%1 Lighter WS connecting: %2")
                            .arg(contextTag(ctx.accountName), wsUrl.toString()));
        ctx.lighterStreamSocket.open(wsUrl);
    }

    // If already connected, send immediately.
    if (ctx.lighterStreamConnected && ctx.lighterStreamReady) {
        QJsonObject msg;
        msg.insert(QStringLiteral("type"), QStringLiteral("jsonapi/sendtx"));
        QJsonObject data;
        data.insert(QStringLiteral("id"), requestId);
        data.insert(QStringLiteral("tx_type"), txType);
        data.insert(QStringLiteral("tx_info"), txDoc.object());
        msg.insert(QStringLiteral("data"), data);
        auto it0 = ctx.lighterWsTxWaiters.find(requestId);
        if (it0 != ctx.lighterWsTxWaiters.end()) {
            it0.value().startMs = QDateTime::currentMSecsSinceEpoch();
        }
        ctx.lighterStreamSocket.sendTextMessage(
            QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        auto it = ctx.lighterWsTxWaiters.find(requestId);
        if (it != ctx.lighterWsTxWaiters.end()) {
            it.value().sent = true;
        }
    }

    // Timeout fallback.
    const int timeoutMs = (!ctx.lighterStreamConnected || !ctx.lighterStreamReady) ? 900 : kLighterWsSendTxTimeoutMs;
    QTimer::singleShot(timeoutMs, this, [this, ctxPtr = &ctx, requestId, httpSend]() mutable {
        if (!ctxPtr) {
            return;
        }
        auto it = ctxPtr->lighterWsTxWaiters.find(requestId);
        if (it == ctxPtr->lighterWsTxWaiters.end()) {
            return;
        }
        auto cb2 = std::move(it.value().cb);
        const int txType2 = it.value().txType;
        const QString txInfo2 = it.value().txInfo;
        ctxPtr->lighterWsTxWaiters.erase(it);
        emit logMessage(QStringLiteral("%1 Lighter WS sendTx timeout; falling back to HTTP")
                            .arg(contextTag(ctxPtr->accountName)));
        httpSend(txType2, txInfo2, std::move(cb2));
    });
}

void TradeManager::subscribeLighterPrivateWs(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (!ctx.lighterStreamConnected || !ctx.lighterStreamReady) {
        return;
    }
    if (ctx.lighterPrivateSubscribed) {
        return;
    }
    if (ctx.lighterAuthToken.isEmpty()) {
        return;
    }

    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    const qint64 accountIndex = static_cast<qint64>(ctx.credentials.accountIndex);
    if (baseUrl.isEmpty() || accountIndex <= 0) {
        return;
    }

    auto sendSub = [&ctx](const QString &channel, bool needsAuth) {
        QJsonObject msg;
        msg.insert(QStringLiteral("type"), QStringLiteral("subscribe"));
        msg.insert(QStringLiteral("channel"), channel);
        if (needsAuth) {
            msg.insert(QStringLiteral("auth"), ctx.lighterAuthToken);
        }
        ctx.lighterStreamSocket.sendTextMessage(
            QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
    };

    // Use dedicated private streams where possible (avoid flaky /api/v1/trades polling).
    sendSub(QStringLiteral("account_all_positions/%1").arg(accountIndex), true);
    sendSub(QStringLiteral("account_all_orders/%1").arg(accountIndex), true);
    sendSub(QStringLiteral("account_all_trades/%1").arg(accountIndex), true);

    ctx.lighterPrivateSubscribed = true;
    emit logMessage(QStringLiteral("%1 Lighter WS subscribed (private) account=%2")
                        .arg(contextTag(ctx.accountName))
                        .arg(accountIndex));

    // Disable the trades REST poll; private WS should provide all needed fills/finrez.
    if (ctx.lighterTradesTimer.isActive()) {
        ctx.lighterTradesTimer.stop();
    }

    // Wire handlers once: reuse the already-open stream socket.
    if (ctx.lighterStreamSocket.property("fusion_private_handlers").toBool()) {
        return;
    }
    ctx.lighterStreamSocket.setProperty("fusion_private_handlers", true);

    connect(&ctx.lighterStreamSocket,
            &QWebSocket::textMessageReceived,
            this,
            [this, ctxPtr = &ctx, baseUrl, accountIndex](const QString &message) {
                if (!ctxPtr) {
                    return;
                }
                QJsonParseError pe;
                const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &pe);
                if (doc.isNull() || !doc.isObject()) {
                    return;
                }
                const QJsonObject obj = doc.object();
                const QString type = obj.value(QStringLiteral("type")).toString();
                if (!(type.startsWith(QStringLiteral("subscribed/")) || type.startsWith(QStringLiteral("update/")))) {
                    return;
                }

                auto getChannel = [&]() -> QString {
                    const QString c = obj.value(QStringLiteral("channel")).toString();
                    if (!c.isEmpty()) return c;
                    return obj.value(QStringLiteral("subscription")).toString();
                };

                auto extractArray = [&](const QString &key) -> QJsonArray {
                    if (obj.value(key).isArray()) return obj.value(key).toArray();
                    const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
                    if (data.value(key).isArray()) return data.value(key).toArray();
                    return {};
                };

                const QString channel = getChannel();

                // Positions
                if (type.contains(QStringLiteral("account_all_positions"), Qt::CaseInsensitive)
                    || channel.startsWith(QStringLiteral("account_all_positions"), Qt::CaseInsensitive)) {
                    const QJsonArray positions = extractArray(QStringLiteral("positions"));
                    if (positions.isEmpty()) {
                        return;
                    }
                    QSet<QString> updatedSymbols;
                    for (const auto &value : positions) {
                        if (!value.isObject()) {
                            continue;
                        }
                        const QJsonObject p = value.toObject();
                        const QString symbol = normalizedSymbol(p.value(QStringLiteral("symbol")).toString());
                        if (symbol.isEmpty()) {
                            continue;
                        }
                        const double positionRaw = p.value(QStringLiteral("position")).toVariant().toDouble();
                        if (std::abs(positionRaw) <= 0.0) {
                            continue;
                        }
                        const int sign = p.value(QStringLiteral("sign")).toInt(positionRaw >= 0.0 ? 1 : -1);
                        TradePosition pos;
                        pos.hasPosition = true;
                        pos.side = sign < 0 ? OrderSide::Sell : OrderSide::Buy;
                        pos.quantity = std::abs(positionRaw);
                        pos.qtyMultiplier = 1.0;
                        pos.averagePrice = p.value(QStringLiteral("avg_entry_price")).toVariant().toDouble();
                        pos.realizedPnl = p.value(QStringLiteral("realized_pnl")).toVariant().toDouble();
                        ctxPtr->positions.insert(symbol, pos);
                        emitPositionChanged(*ctxPtr, symbol);
                        updatedSymbols.insert(symbol);
                    }
                    // Clear disappeared positions.
                    for (auto it = ctxPtr->positions.begin(); it != ctxPtr->positions.end();) {
                        if (!updatedSymbols.contains(it.key())) {
                            if (it.value().hasPosition) {
                                TradePosition empty;
                                it.value() = empty;
                                emitPositionChanged(*ctxPtr, it.key());
                            }
                            it = ctxPtr->positions.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    return;
                }

                // Trades (fills) for finrez + trade history
                if (type.contains(QStringLiteral("account_all_trades"), Qt::CaseInsensitive)
                    || channel.startsWith(QStringLiteral("account_all_trades"), Qt::CaseInsensitive)
                    || type.contains(QStringLiteral("pool_data"), Qt::CaseInsensitive)) {
                    QJsonArray trades = extractArray(QStringLiteral("trades"));
                    if (trades.isEmpty()) {
                        // some payloads might use "account_trades"
                        trades = extractArray(QStringLiteral("account_trades"));
                    }
                    if (trades.isEmpty()) {
                        return;
                    }
                    qint64 maxTradeId = ctxPtr->lighterLastTradeId;
                    const auto &cache = lighterMetaCacheByUrl().value(baseUrl);
                    auto symbolForMarket = [&](int marketId) -> QString {
                        return cache.symbolByMarketId.value(marketId);
                    };
                    auto toDouble = [](const QJsonValue &value) -> double {
                        if (value.isDouble()) return value.toDouble();
                        if (value.isString()) return value.toString().toDouble();
                        return value.toVariant().toDouble();
                    };
                    auto toLongLong = [](const QJsonValue &value) -> qint64 {
                        if (value.isDouble()) return static_cast<qint64>(value.toDouble());
                        if (value.isString()) return value.toString().toLongLong();
                        return value.toVariant().toLongLong();
                    };
                    for (const auto &value : trades) {
                        if (!value.isObject()) continue;
                        const QJsonObject t = value.toObject();
                        const qint64 tradeId = toLongLong(t.value(QStringLiteral("trade_id")));
                        if (tradeId <= ctxPtr->lighterLastTradeId) continue;
                        maxTradeId = std::max(maxTradeId, tradeId);
                        const int marketId = t.value(QStringLiteral("market_id")).toInt(-1);
                        const QString symbol = normalizedSymbol(symbolForMarket(marketId));
                        if (symbol.isEmpty()) continue;
                        const qint64 askAccount = toLongLong(t.value(QStringLiteral("ask_account_id")));
                        const qint64 bidAccount = toLongLong(t.value(QStringLiteral("bid_account_id")));
                        OrderSide side;
                        bool isMaker = false;
                        const bool isMakerAsk = t.value(QStringLiteral("is_maker_ask")).toBool(false);
                        if (accountIndex == bidAccount) {
                            side = OrderSide::Buy;
                            isMaker = !isMakerAsk;
                        } else if (accountIndex == askAccount) {
                            side = OrderSide::Sell;
                            isMaker = isMakerAsk;
                        } else {
                            continue;
                        }
                        const double price = toDouble(t.value(QStringLiteral("price")));
                        const double qty = toDouble(t.value(QStringLiteral("size")));
                        if (!(price > 0.0) || !(qty > 0.0)) continue;
                        qint64 timeMs = toLongLong(t.value(QStringLiteral("timestamp")));
                        if (timeMs > 0 && timeMs < 1000000000000LL) timeMs *= 1000;
                        if (timeMs <= 0) timeMs = QDateTime::currentMSecsSinceEpoch();

                        ExecutedTrade trade;
                        trade.accountName = ctxPtr->accountName;
                        trade.symbol = symbol;
                        trade.side = side;
                        trade.price = price;
                        trade.quantity = qty;
                        const double makerFee = toDouble(t.value(QStringLiteral("maker_fee")));
                        const double takerFee = toDouble(t.value(QStringLiteral("taker_fee")));
                        trade.feeAmount = isMaker ? makerFee : takerFee;
                        trade.feeCurrency = QStringLiteral("USDC");
                        double closedNotional = 0.0;
                        const double realizedDelta =
                            handleOrderFill(*ctxPtr, symbol, side, price, qty, &closedNotional);
                        trade.realizedPnl = realizedDelta;
                        trade.realizedPct =
                            (closedNotional > 0.0) ? (realizedDelta / closedNotional) * 100.0 : 0.0;
                        trade.timeMs = timeMs;
                        appendTradeHistory(trade);
                        recordFinrezTrade(*ctxPtr, trade);
                        emit tradeExecuted(trade);
                    }
                    ctxPtr->lighterLastTradeId = maxTradeId;
                    return;
                }

                // Orders (for markers + SL/TP visibility)
                if (type.contains(QStringLiteral("account_all_orders"), Qt::CaseInsensitive)
                    || channel.startsWith(QStringLiteral("account_all_orders"), Qt::CaseInsensitive)) {
                    QJsonArray orders = extractArray(QStringLiteral("orders"));
                    if (orders.isEmpty()) {
                        orders = extractArray(QStringLiteral("account_orders"));
                    }
                    if (orders.isEmpty()) {
                        return;
                    }

                    ensureLighterMetaLoaded(*ctxPtr, baseUrl, [this, ctxPtr, orders, baseUrl](const QString &metaErr) {
                        if (!ctxPtr) return;
                        if (!metaErr.isEmpty()) return;
                        const auto &cache = lighterMetaCacheByUrl().value(baseUrl);

                        auto parseBool = [](const QJsonValue &v) -> bool {
                            if (v.isBool()) return v.toBool();
                            if (v.isDouble()) return v.toInt() != 0;
                            if (v.isString()) return v.toString().trimmed().toInt() != 0;
                            return v.toVariant().toBool();
                        };
                        auto toDouble = [](const QJsonValue &v) -> double {
                            if (v.isDouble()) return v.toDouble();
                            if (v.isString()) return v.toString().toDouble();
                            return v.toVariant().toDouble();
                        };

                        auto parseTriggerPrice = [&](const QJsonObject &o) -> double {
                            const QStringList keys = {
                                QStringLiteral("trigger_price"),
                                QStringLiteral("triggerPrice"),
                                QStringLiteral("trigger_price_float"),
                                QStringLiteral("stop_price"),
                                QStringLiteral("stopPrice"),
                                QStringLiteral("avg_execution_price"),
                                QStringLiteral("avgExecutionPrice")
                            };
                            for (const auto &k : keys) {
                                if (o.contains(k)) {
                                    const double p = toDouble(o.value(k));
                                    if (p > 0.0) return p;
                                }
                            }
                            if (o.contains(QStringLiteral("trigger_price_int"))) {
                                const qint64 i = o.value(QStringLiteral("trigger_price_int")).toVariant().toLongLong();
                                if (i > 0) {
                                    const int marketId = o.value(QStringLiteral("market_id")).toInt(o.value(QStringLiteral("marketId")).toInt(-1));
                                    const QString sym = cache.symbolByMarketId.value(marketId);
                                    const auto metaIt = cache.bySymbol.constFind(sym);
                                    const int pd = metaIt != cache.bySymbol.constEnd() ? metaIt->priceDecimals : 5;
                                    return static_cast<double>(i) / static_cast<double>(pow10i(pd));
                                }
                            }
                            return 0.0;
                        };

                        enum class TriggerCond { Unknown, Above, Below };
                        auto parseTriggerCond = [&](const QJsonObject &o) -> TriggerCond {
                            const QStringList stringKeys = {
                                QStringLiteral("trigger_condition"),
                                QStringLiteral("triggerCondition"),
                                QStringLiteral("trigger_conditions"),
                                QStringLiteral("triggerConditions")
                            };
                            for (const auto &k : stringKeys) {
                                if (!o.contains(k)) continue;
                                const QString s = o.value(k).toVariant().toString().trimmed().toLower();
                                if (s.contains(QStringLiteral("below")) || s.contains(QStringLiteral("under"))) {
                                    return TriggerCond::Below;
                                }
                                if (s.contains(QStringLiteral("above")) || s.contains(QStringLiteral("over"))) {
                                    return TriggerCond::Above;
                                }
                                if (s == QStringLiteral("0") || s == QStringLiteral("below")) return TriggerCond::Below;
                                if (s == QStringLiteral("1") || s == QStringLiteral("above")) return TriggerCond::Above;
                            }
                            const QStringList boolAboveKeys = {
                                QStringLiteral("trigger_is_above"),
                                QStringLiteral("triggerIsAbove"),
                                QStringLiteral("is_trigger_above"),
                                QStringLiteral("isTriggerAbove"),
                                QStringLiteral("is_above"),
                                QStringLiteral("isAbove")
                            };
                            for (const auto &k : boolAboveKeys) {
                                if (!o.contains(k)) continue;
                                const QJsonValue v = o.value(k);
                                if (v.isBool()) return v.toBool() ? TriggerCond::Above : TriggerCond::Below;
                                if (v.isDouble()) return v.toInt() != 0 ? TriggerCond::Above : TriggerCond::Below;
                                if (v.isString()) return v.toString().trimmed().toInt() != 0 ? TriggerCond::Above : TriggerCond::Below;
                            }
                            // Some APIs use 0/1 enum in trigger_condition.
                            if (o.contains(QStringLiteral("trigger_condition")) && o.value(QStringLiteral("trigger_condition")).isDouble()) {
                                const int v = o.value(QStringLiteral("trigger_condition")).toInt(-1);
                                if (v == 0) return TriggerCond::Below;
                                if (v == 1) return TriggerCond::Above;
                            }
                            return TriggerCond::Unknown;
                        };

                        QHash<QString, QVector<DomWidget::LocalOrderMarker>> perSymbolMarkers;
                        struct StopState { bool hasSl{false}; bool hasTp{false}; double sl{0}; double tp{0}; };
                        QHash<QString, StopState> perSymbolStops;

                        auto resolveStopKind = [&](Context &ctx,
                                                   const QString &orderId,
                                                   Context::LighterStopKind explicitKind,
                                                   Context::LighterStopKind inferredKind,
                                                   qint64 nowMs) -> Context::LighterStopKind {
                            const QString id = orderId.trimmed();
                            if (id.isEmpty()) {
                                return (explicitKind != Context::LighterStopKind::Unknown) ? explicitKind : inferredKind;
                            }

                            static constexpr int kMaxCacheSize = 8000;
                            if (ctx.lighterStopKindByOrderId.size() > kMaxCacheSize) {
                                QVector<QString> toDrop;
                                toDrop.reserve(ctx.lighterStopKindByOrderId.size() / 4);
                                for (auto it = ctx.lighterStopKindByOrderId.constBegin(); it != ctx.lighterStopKindByOrderId.constEnd(); ++it) {
                                    if (!it.value().explicitType && it.value().updatedMs > 0
                                        && (nowMs - it.value().updatedMs) > 10LL * 60LL * 1000LL) {
                                        toDrop.push_back(it.key());
                                    }
                                }
                                for (const auto &k : toDrop) {
                                    ctx.lighterStopKindByOrderId.remove(k);
                                }
                                if (ctx.lighterStopKindByOrderId.size() > kMaxCacheSize) {
                                    ctx.lighterStopKindByOrderId.clear();
                                }
                            }

                            auto &info = ctx.lighterStopKindByOrderId[id];
                            if (explicitKind != Context::LighterStopKind::Unknown) {
                                info.kind = explicitKind;
                                info.explicitType = true;
                                info.updatedMs = nowMs;
                                return explicitKind;
                            }
                            if (info.explicitType && info.kind != Context::LighterStopKind::Unknown) {
                                info.updatedMs = nowMs;
                                return info.kind;
                            }
                            if (info.kind != Context::LighterStopKind::Unknown) {
                                info.updatedMs = nowMs;
                                return info.kind;
                            }
                            info.kind = inferredKind;
                            info.explicitType = false;
                            info.updatedMs = nowMs;
                            return inferredKind;
                        };

                        auto parseIsAskFlag = [&](const QJsonObject &o) -> int {
                            const QStringList boolKeys = {QStringLiteral("is_ask"), QStringLiteral("isAsk")};
                            for (const auto &k : boolKeys) {
                                if (!o.contains(k)) continue;
                                const QJsonValue v = o.value(k);
                                if (v.isBool()) return v.toBool() ? 1 : 0;
                                if (v.isDouble()) return v.toInt() != 0 ? 1 : 0;
                                if (v.isString()) {
                                    const QString s = v.toString().trimmed().toLower();
                                    if (s == QStringLiteral("ask") || s == QStringLiteral("sell") || s == QStringLiteral("true") || s == QStringLiteral("1")) return 1;
                                    if (s == QStringLiteral("bid") || s == QStringLiteral("buy") || s == QStringLiteral("false") || s == QStringLiteral("0")) return 0;
                                }
                            }
                            const QStringList sideKeys = {QStringLiteral("side"), QStringLiteral("order_side"), QStringLiteral("orderSide"), QStringLiteral("direction")};
                            for (const auto &k : sideKeys) {
                                if (!o.contains(k)) continue;
                                const QString s = o.value(k).toVariant().toString().trimmed().toLower();
                                if (s.contains(QStringLiteral("sell")) || s.contains(QStringLiteral("ask")) || s == QStringLiteral("short")) return 1;
                                if (s.contains(QStringLiteral("buy")) || s.contains(QStringLiteral("bid")) || s == QStringLiteral("long")) return 0;
                            }
                            return -1;
                        };

                        for (const auto &v : orders) {
                            if (!v.isObject()) continue;
                            const QJsonObject o = v.toObject();
                            int marketId = o.value(QStringLiteral("market_id")).toInt(-1);
                            if (marketId < 0) {
                                marketId = o.value(QStringLiteral("marketId")).toInt(-1);
                            }
                            QString sym = normalizedSymbol(o.value(QStringLiteral("symbol")).toString());
                            if (sym.isEmpty() && marketId >= 0) {
                                sym = cache.symbolByMarketId.value(marketId);
                            }
                            sym = normalizedSymbol(sym);
                            if (sym.isEmpty()) continue;

                            const int askFlag = parseIsAskFlag(o);
                            const bool isAsk = (askFlag == 1);
                            const OrderSide side = isAsk ? OrderSide::Sell : OrderSide::Buy;
                            const double price = toDouble(o.value(QStringLiteral("price")));
                            const bool reduceOnly = o.contains(QStringLiteral("reduce_only"))
                                                        ? parseBool(o.value(QStringLiteral("reduce_only")))
                                                        : parseBool(o.value(QStringLiteral("reduceOnly")));
                            const double triggerPx = parseTriggerPrice(o);
                            const bool isStopLike = (triggerPx > 0.0);

                            // Build SL/TP state
                            if (reduceOnly && isStopLike) {
                                StopState &st = perSymbolStops[sym];
                                bool inferredSl = false;
                                bool inferredTp = false;
                                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                                const QString stopOrderId = lighterOrderIdForCache(o);
                                // Prefer explicit order type if present.
                                int orderTypeCode = -1;
                                const QJsonValue orderTypeVal =
                                    o.contains(QStringLiteral("order_type")) ? o.value(QStringLiteral("order_type"))
                                    : (o.contains(QStringLiteral("orderType")) ? o.value(QStringLiteral("orderType"))
                                                                               : o.value(QStringLiteral("type")));
                                if (!orderTypeVal.isUndefined() && !orderTypeVal.isNull()) {
                                    orderTypeCode = orderTypeVal.toVariant().toInt();
                                }
                                const QString typeStr = o.value(QStringLiteral("type")).toVariant().toString().trimmed();
                                const QString typeLower = typeStr.toLower();
                                const bool isStopLossOrder =
                                    (orderTypeCode == 2)
                                    || typeLower.contains(QStringLiteral("stoploss"))
                                    || typeLower.contains(QStringLiteral("s/l"))
                                    || typeLower == QStringLiteral("sl")
                                    || typeLower.contains(QStringLiteral("sl "));
                                const bool isTakeProfitOrder =
                                    (orderTypeCode == 4)
                                    || typeLower.contains(QStringLiteral("take"))
                                    || typeLower.contains(QStringLiteral("t/p"))
                                    || typeLower == QStringLiteral("tp")
                                    || typeLower.contains(QStringLiteral("tp "))
                                    || typeLower.contains(QStringLiteral("profit"));
                                const Context::LighterStopKind explicitKind =
                                    isStopLossOrder
                                        ? Context::LighterStopKind::StopLoss
                                        : (isTakeProfitOrder ? Context::LighterStopKind::TakeProfit
                                                             : Context::LighterStopKind::Unknown);
                                if (explicitKind != Context::LighterStopKind::Unknown) {
                                    const auto kind =
                                        resolveStopKind(*ctxPtr, stopOrderId, explicitKind, Context::LighterStopKind::Unknown, nowMs);
                                    if (kind == Context::LighterStopKind::StopLoss) {
                                        st.hasSl = true;
                                        st.sl = triggerPx;
                                    } else if (kind == Context::LighterStopKind::TakeProfit) {
                                        st.hasTp = true;
                                        st.tp = triggerPx;
                                    }
                                    continue;
                                }
                                const TriggerCond cond = parseTriggerCond(o);
                                const TradePosition pos = ctxPtr->positions.value(sym, TradePosition{});
                                const bool hasPos = pos.hasPosition && pos.averagePrice > 0.0 && pos.quantity > 0.0;
                                if (cond != TriggerCond::Unknown) {
                                    if (askFlag != -1) {
                                        // Reduce-only close direction is stable:
                                        // - isAsk=true (SELL) closes a LONG
                                        // - isAsk=false (BUY) closes a SHORT
                                        const bool closesLong = (askFlag == 1);
                                        inferredSl = closesLong ? (cond == TriggerCond::Below) : (cond == TriggerCond::Above);
                                        inferredTp = closesLong ? (cond == TriggerCond::Above) : (cond == TriggerCond::Below);
                                    } else {
                                        // Fallback: use position side if we don't know order direction.
                                        if (hasPos && pos.side == OrderSide::Buy) {
                                            inferredSl = (cond == TriggerCond::Below);
                                            inferredTp = (cond == TriggerCond::Above);
                                        } else if (hasPos && pos.side == OrderSide::Sell) {
                                            inferredSl = (cond == TriggerCond::Above);
                                            inferredTp = (cond == TriggerCond::Below);
                                        }
                                    }
                                } else {
                                    // Fallback: compare with cached average price (can lag briefly).
                                    if (hasPos) {
                                        if (pos.side == OrderSide::Buy) {
                                            inferredSl = triggerPx < pos.averagePrice;
                                            inferredTp = triggerPx > pos.averagePrice;
                                        } else {
                                            inferredSl = triggerPx > pos.averagePrice;
                                            inferredTp = triggerPx < pos.averagePrice;
                                        }
                                    } else {
                                        // Last resort: keep deterministic slotting so colors stay consistent.
                                        inferredSl = !st.hasSl;
                                        inferredTp = !inferredSl && !st.hasTp;
                                    }
                                }
                                const Context::LighterStopKind inferredKind =
                                    inferredSl
                                        ? Context::LighterStopKind::StopLoss
                                        : (inferredTp ? Context::LighterStopKind::TakeProfit
                                                      : Context::LighterStopKind::Unknown);
                                const auto kind =
                                    resolveStopKind(*ctxPtr, stopOrderId, Context::LighterStopKind::Unknown, inferredKind, nowMs);
                                if (kind == Context::LighterStopKind::StopLoss && !st.hasSl) {
                                    st.hasSl = true;
                                    st.sl = triggerPx;
                                } else if (kind == Context::LighterStopKind::TakeProfit && !st.hasTp) {
                                    st.hasTp = true;
                                    st.tp = triggerPx;
                                }
                                continue;
                            }

                            // Regular order markers
                            double baseAmt = 0.0;
                            if (o.contains(QStringLiteral("base_amount"))) {
                                baseAmt = toDouble(o.value(QStringLiteral("base_amount")));
                            } else if (o.contains(QStringLiteral("initial_base_amount"))) {
                                baseAmt = toDouble(o.value(QStringLiteral("initial_base_amount")));
                            }
                            if (!(price > 0.0) || !(baseAmt > 0.0)) continue;

                            QString orderId;
                            const QJsonValue clientIndexVal =
                                o.contains(QStringLiteral("client_order_index"))
                                    ? o.value(QStringLiteral("client_order_index"))
                                    : o.value(QStringLiteral("clientOrderIndex"));
                            const qint64 clientIndex = clientIndexVal.toVariant().toLongLong();
                            if (clientIndex > 0) {
                                orderId = QString::number(clientIndex);
                            } else if (o.contains(QStringLiteral("order_index"))) {
                                orderId = o.value(QStringLiteral("order_index")).toVariant().toString();
                            }
                            if (orderId.isEmpty()) continue;

                            DomWidget::LocalOrderMarker marker;
                            marker.price = price;
                            marker.quantity = std::abs(baseAmt * price);
                            marker.side = side;
                            marker.createdMs = QDateTime::currentMSecsSinceEpoch();
                            marker.orderId = orderId;
                            perSymbolMarkers[sym].push_back(marker);
                        }

                        for (auto it = perSymbolMarkers.constBegin(); it != perSymbolMarkers.constEnd(); ++it) {
                            emit localOrdersUpdated(ctxPtr->accountName, it.key(), it.value());
                        }
                        for (auto it = perSymbolStops.constBegin(); it != perSymbolStops.constEnd(); ++it) {
                            emit lighterStopOrdersUpdated(ctxPtr->accountName, it.key(), it.value().hasSl, it.value().sl, it.value().hasTp, it.value().tp);
                        }
                    });
                }
            });
}

TradeManager::TradeManager(QObject *parent)
    : QObject(parent)
{
    // Allow Qt to resolve proxies via OS settings when `DefaultProxy` is used.
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    qRegisterMetaType<TradePosition>("TradePosition");
    qRegisterMetaType<MexcCredentials>("MexcCredentials");
    qRegisterMetaType<ExecutedTrade>("ExecutedTrade");
    loadTradeHistory();
}

TradeManager::~TradeManager()
{
    qDeleteAll(m_contexts);
}

void TradeManager::setCredentials(ConnectionStore::Profile profile, const MexcCredentials &creds)
{
    Context &ctx = ensureContext(profile);
    ctx.credentials = creds;
    QString account = creds.label.trimmed();
    if (account.isEmpty()) {
        account = defaultAccountName(profile);
    }
    ctx.accountName = account;
}

MexcCredentials TradeManager::credentials(ConnectionStore::Profile profile) const
{
    if (Context *ctx = contextForProfile(profile)) {
        return ctx->credentials;
    }
    return MexcCredentials{};
}

TradeManager::ConnectionState TradeManager::state(ConnectionStore::Profile profile) const
{
    if (Context *ctx = contextForProfile(profile)) {
        return ctx->state;
    }
    return ConnectionState::Disconnected;
}

TradeManager::ConnectionState TradeManager::overallState() const
{
    bool hasConnected = false;
    bool hasConnecting = false;
    for (auto it = m_contexts.constBegin(); it != m_contexts.constEnd(); ++it) {
        switch (it.value()->state) {
        case ConnectionState::Error:
            return ConnectionState::Error;
        case ConnectionState::Connecting:
            hasConnecting = true;
            break;
        case ConnectionState::Connected:
            hasConnected = true;
            break;
        case ConnectionState::Disconnected:
        default:
            break;
        }
    }
    if (hasConnecting) {
        return ConnectionState::Connecting;
    }
    if (hasConnected) {
        return ConnectionState::Connected;
    }
    return ConnectionState::Disconnected;
}

QString TradeManager::accountName(ConnectionStore::Profile profile) const
{
    return accountNameFor(profile);
}

QVector<TradeManager::FinrezRow> TradeManager::finrezRows(ConnectionStore::Profile profile) const
{
    const Context *ctx = contextForProfile(profile);
    if (!ctx) {
        return {};
    }

    QSet<QString> assets;
    for (auto it = ctx->balances.constBegin(); it != ctx->balances.constEnd(); ++it) {
        assets.insert(it.key());
    }
    for (auto it = ctx->realizedPnl.constBegin(); it != ctx->realizedPnl.constEnd(); ++it) {
        assets.insert(it.key());
    }
    for (auto it = ctx->commissions.constBegin(); it != ctx->commissions.constEnd(); ++it) {
        assets.insert(it.key());
    }

    QVector<FinrezRow> rows;
    rows.reserve(assets.size());
    for (const QString &asset : assets) {
        FinrezRow row;
        row.asset = asset;
        row.pnl = ctx->realizedPnl.value(asset, 0.0);
        row.commission = ctx->commissions.value(asset, 0.0);
        if (const auto it = ctx->balances.constFind(asset); it != ctx->balances.constEnd()) {
            row.available = it->available;
            row.locked = it->locked;
            row.funds = row.available + row.locked;
            row.updatedMs = it->updatedMs;
        }
        rows.push_back(row);
    }

    std::sort(rows.begin(), rows.end(), [](const FinrezRow &a, const FinrezRow &b) {
        return a.asset < b.asset;
    });
    return rows;
}

void TradeManager::resetFinrez(ConnectionStore::Profile profile)
{
    Context *ctx = contextForProfile(profile);
    if (!ctx) {
        return;
    }
    ctx->realizedPnl.clear();
    ctx->commissions.clear();
    emit finrezChanged(profile);
}

bool TradeManager::ensureCredentials(const Context &ctx) const
{
    if (ctx.profile == ConnectionStore::Profile::Lighter) {
        const QString url = normalizeLighterUrl(ctx.credentials.baseUrl);
        if (url.isEmpty()) {
            return false;
        }
        if (ctx.credentials.accountIndex < 0) {
            return false;
        }
        if (ctx.credentials.apiKeyIndex < 0 || ctx.credentials.apiKeyIndex > 253) {
            return false;
        }
        return !ctx.credentials.apiKey.trimmed().isEmpty();
    }
    if (ctx.profile == ConnectionStore::Profile::UzxSpot
        || ctx.profile == ConnectionStore::Profile::UzxSwap) {
        return !ctx.credentials.apiKey.isEmpty() && !ctx.credentials.secretKey.isEmpty()
               && !ctx.credentials.passphrase.isEmpty();
    }
    if (ctx.profile == ConnectionStore::Profile::MexcFutures) {
        return !ctx.credentials.apiKey.isEmpty() && !ctx.credentials.secretKey.isEmpty()
               && !ctx.credentials.uid.isEmpty();
    }
    return !ctx.credentials.apiKey.isEmpty() && !ctx.credentials.secretKey.isEmpty();
}

void TradeManager::connectToExchange(ConnectionStore::Profile profile)
{
    Context &ctx = ensureContext(profile);
    ctx.proxyStatusLog.clear();
    if (profile == ConnectionStore::Profile::Lighter) {
        if (ctx.credentials.apiKey.trimmed().isEmpty()) {
            MexcCredentials vaultCreds;
            QString vaultErr;
            if (fillLighterCredsFromVault(&vaultCreds, &vaultErr)) {
                if (!vaultCreds.baseUrl.trimmed().isEmpty()) {
                    ctx.credentials.baseUrl = vaultCreds.baseUrl;
                }
                ctx.credentials.accountIndex = vaultCreds.accountIndex;
                ctx.credentials.apiKeyIndex = vaultCreds.apiKeyIndex;
                ctx.credentials.apiKey = vaultCreds.apiKey;
                if (ctx.credentials.proxy.trimmed().isEmpty() && !vaultCreds.proxy.trimmed().isEmpty()) {
                    ctx.credentials.proxy = vaultCreds.proxy.trimmed();
                    ctx.credentials.proxyType = vaultCreds.proxyType.trimmed();
                }
            } else if (!vaultErr.isEmpty()) {
                emit logMessage(QStringLiteral("%1 Lighter vault: %2").arg(contextTag(ctx.accountName), vaultErr));
            }
        }
        if (!ensureCredentials(ctx)) {
            setState(ctx, ConnectionState::Error, tr("Missing Lighter credentials"));
            emit logMessage(QStringLiteral("%1 Provide Lighter baseUrl/accountIndex/apiKeyIndex and API key private key.")
                                .arg(contextTag(ctx.accountName)));
            return;
        }
        {
            const QString proxyRaw = ctx.credentials.proxy.trimmed();
            if (!proxyRaw.isEmpty()) {
                QNetworkProxy proxy;
                QString proxyErr;
                if (!parseHttpProxy(proxyRaw, ctx.credentials.proxyType, proxy, &proxyErr)) {
                    setState(ctx, ConnectionState::Error, tr("Invalid proxy"));
                    emit logMessage(QStringLiteral("%1 Invalid proxy: %2").arg(contextTag(ctx.accountName), proxyErr));
                    return;
                }
                const QString preflightKey = QStringLiteral("%1|%2|%3|%4")
                                                 .arg(ctx.credentials.proxyType.trimmed().toLower(),
                                                      proxy.hostName().trimmed().toLower(),
                                                      QString::number(proxy.port()),
                                                      proxy.user().isEmpty() ? QStringLiteral("0") : QStringLiteral("1"));
                if (ctx.proxyPreflightKey != preflightKey) {
                    ctx.proxyPreflightKey = preflightKey;
                    ctx.proxyPreflightPassed = false;
                    ctx.proxyPreflightInFlight = false;
                    ctx.proxyPreflightToken++;
                }
                if (ctx.state != ConnectionState::Connecting) {
                    ctx.proxyPreflightPassed = false;
                }
                if (!ctx.proxyPreflightPassed) {
                    if (ctx.proxyPreflightInFlight) {
                        return;
                    }
                    setState(ctx, ConnectionState::Connecting, tr("Checking proxy..."));
                    emit logMessage(QStringLiteral("%1 Proxy check started (%2)")
                                        .arg(contextTag(ctx.accountName), proxySummaryForLog(proxy)));
                    Context *ctxPtr = &ctx;
                    const quint64 token = ++ctx.proxyPreflightToken;
                    ctx.proxyPreflightInFlight = true;
                    runProxyPreflight(this,
                                      proxy,
                                      [this, ctxPtr, profile, token](int okCount, const QHash<QString, bool> &results) {
                        if (!ctxPtr) {
                            return;
                        }
                        if (ctxPtr->proxyPreflightToken != token) {
                            return;
                        }
                        ctxPtr->proxyPreflightInFlight = false;
                        if (ctxPtr->state != ConnectionState::Connecting) {
                            return;
                        }
                        const bool g = results.value(QStringLiteral("google"), false);
                        const bool f = results.value(QStringLiteral("facebook"), false);
                        const bool y = results.value(QStringLiteral("yandex"), false);
                        emit logMessage(QStringLiteral("%1 Proxy check result: google=%2 facebook=%3 yandex=%4")
                                            .arg(contextTag(ctxPtr->accountName),
                                                 g ? QStringLiteral("ok") : QStringLiteral("fail"),
                                                 f ? QStringLiteral("ok") : QStringLiteral("fail"),
                                                 y ? QStringLiteral("ok") : QStringLiteral("fail")));
                        if (okCount <= 0) {
                            setState(*ctxPtr, ConnectionState::Error, tr("Proxy check failed"));
                            emit logMessage(QStringLiteral("%1 Proxy check failed; not connecting")
                                                .arg(contextTag(ctxPtr->accountName)));
                            return;
                        }
                        ctxPtr->proxyPreflightPassed = true;
                        connectToExchange(profile);
                    });
                    return;
                }
            }
            ensureLighterNetwork(ctx);
        }

        closeWebSocket(ctx);
        ctx.openOrdersTimer.stop();
        ctx.futuresDealsTimer.stop();
        ctx.lighterAccountTimer.stop();
        ctx.lighterTradesTimer.stop();
        ctx.lighterBurstTimer.stop();
        ctx.listenKey.clear();
        ctx.lighterAuthToken.clear();
        ctx.hasSubscribed = false;
        ctx.trackedSymbols.clear();
        ctx.openOrdersPending = false;
        ctx.lighterAccountPending = false;
        ctx.lighterTradesPending = false;
        ctx.pendingCancelSymbols.clear();
        ctx.futuresDealsInFlight.clear();

        setState(ctx, ConnectionState::Connecting, tr("Connecting to Lighter..."));
        emit logMessage(QStringLiteral("%1 Connecting to Lighter signer...").arg(contextTag(ctx.accountName)));

        Context *ctxPtr = &ctx;
        const QString cfgDir = tradeConfigDir();
        const QString normalizedUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
        const int chainId = lighterChainIdForUrl(normalizedUrl);
        const int apiKeyIndex = ctx.credentials.apiKeyIndex;
        const long long accountIndex = static_cast<long long>(ctx.credentials.accountIndex);
        const QString apiPriv = strip0xPrefix(ctx.credentials.apiKey);

        auto finishConnect = [this, ctxPtr, normalizedUrl, chainId, apiKeyIndex, accountIndex, apiPriv, cfgDir](const QString &dllPath) {
            if (!ctxPtr) {
                return;
            }
            QString loadErr;
            if (!lighterSigner().ensureLoaded(dllPath, loadErr)) {
                setState(*ctxPtr, ConnectionState::Error, tr("Failed to load Lighter signer DLL"));
                emit logMessage(QStringLiteral("%1 Lighter signer load failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), loadErr));
                return;
            }

            QByteArray urlBytes = normalizedUrl.toUtf8();
            QByteArray keyBytes = apiPriv.toUtf8();
            char *errPtr = lighterSigner().createClient(urlBytes.data(),
                                                       keyBytes.data(),
                                                       chainId,
                                                       apiKeyIndex,
                                                       accountIndex);
            if (errPtr) {
                // NOTE: The signer DLL is built with Go/cgo (often MinGW toolchain on Windows).
                // Freeing its allocations with our CRT can crash; treat returned strings as borrowed.
                const QString errStr = QString::fromUtf8(errPtr);
                setState(*ctxPtr, ConnectionState::Error, tr("Lighter CreateClient failed"));
                emit logMessage(QStringLiteral("%1 Lighter CreateClient failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), errStr));
                return;
            }

            char *checkErrPtr = lighterSigner().checkClient(apiKeyIndex, accountIndex);
            if (checkErrPtr) {
                const QString errStr = QString::fromUtf8(checkErrPtr);
                setState(*ctxPtr, ConnectionState::Error, tr("Lighter CheckClient failed"));
                emit logMessage(QStringLiteral("%1 Lighter CheckClient failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), errStr));
                return;
            }

            const LighterStrOrErr tok = lighterSigner().createAuthToken(0, apiKeyIndex, accountIndex);
            if (tok.err) {
                const QString errStr = QString::fromUtf8(tok.err);
                emit logMessage(QStringLiteral("%1 Lighter auth token generation failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), errStr));
            } else if (tok.str) {
                ctxPtr->lighterAuthToken = QString::fromUtf8(tok.str);
            }

            setState(*ctxPtr, ConnectionState::Connected, tr("Connected"));
            ctxPtr->lighterTradesDisabled = false;
            emit logMessage(QStringLiteral("%1 Lighter connected (accountIndex=%2 apiKeyIndex=%3 chainId=%4)")
                                .arg(contextTag(ctxPtr->accountName))
                                .arg(accountIndex)
                                .arg(apiKeyIndex)
                                .arg(chainId));
            // Keep Lighter WS open to avoid first-order connection latency.
            ensureLighterStreamWired(*ctxPtr);
            ensureLighterStreamOpen(*ctxPtr);
            if (!ctxPtr->watchedSymbols.isEmpty()) {
                ctxPtr->lighterAccountBackoffMs = 0;
                ctxPtr->lighterTradesBackoffMs = 0;
                ctxPtr->lighterAccountTimer.setInterval(kLighterAccountPollMs);
                if (!ctxPtr->lighterAccountTimer.isActive()) {
                    ctxPtr->lighterAccountTimer.start();
                }
                pollLighterAccount(*ctxPtr);
            }
        };

        const QString existingDll = findLighterSignerDll(cfgDir);
        if (!existingDll.isEmpty()) {
            finishConnect(existingDll);
            return;
        }

        const QString targetPath = defaultLighterSignerTargetPath(cfgDir);
        emit logMessage(QStringLiteral("%1 Downloading Lighter signer DLL...").arg(contextTag(ctx.accountName)));
        QNetworkRequest req(QUrl(QStringLiteral("https://github.com/elliottech/lighter-go/releases/latest/download/lighter-signer-windows-amd64.dll")));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = ensureLighterNetwork(ctx)->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr, targetPath, finishConnect]() {
            const auto err = reply->error();
            const QByteArray data = reply->readAll();
            reply->deleteLater();
            if (!ctxPtr) {
                return;
            }
            if (err != QNetworkReply::NoError || data.size() < 1024) {
                setState(*ctxPtr, ConnectionState::Error, tr("Failed to download Lighter signer DLL"));
                emit logMessage(QStringLiteral("%1 Lighter signer download failed: %2")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(static_cast<int>(err)));
                return;
            }
            QSaveFile f(targetPath);
            if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
                setState(*ctxPtr, ConnectionState::Error, tr("Failed to save Lighter signer DLL"));
                emit logMessage(QStringLiteral("%1 Lighter signer save failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), targetPath));
                return;
            }
            finishConnect(targetPath);
        });
        return;
    }
    if (profile == ConnectionStore::Profile::BinanceSpot
        || profile == ConnectionStore::Profile::BinanceFutures) {
        setState(ctx, ConnectionState::Error, tr("Binance trading is not implemented yet"));
        emit logMessage(QStringLiteral("%1 Binance trading is not implemented yet")
                            .arg(contextTag(ctx.accountName)));
        return;
    }
    if (!ensureCredentials(ctx)) {
        setState(ctx, ConnectionState::Error, tr("Missing API credentials"));
        emit logMessage(QStringLiteral("%1 Provide API key/secret (and passphrase for UZX).")
                            .arg(contextTag(ctx.accountName)));
        return;
    }
    if (ctx.profile == ConnectionStore::Profile::MexcSpot || ctx.profile == ConnectionStore::Profile::MexcFutures) {
        const QString proxyRaw = ctx.credentials.proxy.trimmed();
        if (!proxyRaw.isEmpty()) {
            QNetworkProxy proxy;
            QString proxyErr;
            if (!parseHttpProxy(proxyRaw, ctx.credentials.proxyType, proxy, &proxyErr)) {
                setState(ctx, ConnectionState::Error, tr("Invalid proxy"));
                emit logMessage(QStringLiteral("%1 Invalid proxy: %2").arg(contextTag(ctx.accountName), proxyErr));
                return;
            }
            const QString preflightKey = QStringLiteral("%1|%2|%3|%4")
                                             .arg(ctx.credentials.proxyType.trimmed().toLower(),
                                                  proxy.hostName().trimmed().toLower(),
                                                  QString::number(proxy.port()),
                                                  proxy.user().isEmpty() ? QStringLiteral("0") : QStringLiteral("1"));
            if (ctx.proxyPreflightKey != preflightKey) {
                ctx.proxyPreflightKey = preflightKey;
                ctx.proxyPreflightPassed = false;
                ctx.proxyPreflightInFlight = false;
                ctx.proxyPreflightToken++;
            }
            if (ctx.state != ConnectionState::Connecting) {
                ctx.proxyPreflightPassed = false;
            }
            if (!ctx.proxyPreflightPassed) {
                if (ctx.proxyPreflightInFlight) {
                    return;
                }
                setState(ctx, ConnectionState::Connecting, tr("Checking proxy..."));
                emit logMessage(QStringLiteral("%1 Proxy check started (%2)")
                                    .arg(contextTag(ctx.accountName), proxySummaryForLog(proxy)));
                Context *ctxPtr = &ctx;
                const quint64 token = ++ctx.proxyPreflightToken;
                ctx.proxyPreflightInFlight = true;
                runProxyPreflight(this,
                                  proxy,
                                  [this, ctxPtr, profile, token](int okCount, const QHash<QString, bool> &results) {
                    if (!ctxPtr) {
                        return;
                    }
                    if (ctxPtr->proxyPreflightToken != token) {
                        return;
                    }
                    ctxPtr->proxyPreflightInFlight = false;
                    if (ctxPtr->state != ConnectionState::Connecting) {
                        return;
                    }
                    const bool g = results.value(QStringLiteral("google"), false);
                    const bool f = results.value(QStringLiteral("facebook"), false);
                    const bool y = results.value(QStringLiteral("yandex"), false);
                    emit logMessage(QStringLiteral("%1 Proxy check result: google=%2 facebook=%3 yandex=%4")
                                        .arg(contextTag(ctxPtr->accountName),
                                             g ? QStringLiteral("ok") : QStringLiteral("fail"),
                                             f ? QStringLiteral("ok") : QStringLiteral("fail"),
                                             y ? QStringLiteral("ok") : QStringLiteral("fail")));
                    if (okCount <= 0) {
                        setState(*ctxPtr, ConnectionState::Error, tr("Proxy check failed"));
                        emit logMessage(QStringLiteral("%1 Proxy check failed; not connecting")
                                            .arg(contextTag(ctxPtr->accountName)));
                        return;
                    }
                    ctxPtr->proxyPreflightPassed = true;
                    connectToExchange(profile);
                });
                return;
            }
        }
        ensureMexcNetwork(ctx);
    }
    closeWebSocket(ctx);
    ctx.openOrdersTimer.stop();
    ctx.futuresDealsTimer.stop();
    ctx.lighterAccountTimer.stop();
    ctx.lighterTradesTimer.stop();
    ctx.lighterBurstTimer.stop();
    ctx.listenKey.clear();
    ctx.hasSubscribed = false;
    ctx.trackedSymbols.clear();
    ctx.openOrdersPending = false;
    ctx.pendingCancelSymbols.clear();
    ctx.futuresDealsInFlight.clear();
    if (ctx.profile == ConnectionStore::Profile::MexcFutures) {
        ctx.futuresLoggedIn = false;
        ctx.hasSubscribed = false;
        setState(ctx, ConnectionState::Connecting, tr("Connecting to MEXC futures..."));
        emit logMessage(QStringLiteral("%1 Connecting to MEXC futures WebSocket...")
                            .arg(contextTag(ctx.accountName)));
        initializeFuturesWebSocket(ctx);
        return;
    }
    if (ctx.profile == ConnectionStore::Profile::UzxSpot
        || ctx.profile == ConnectionStore::Profile::UzxSwap) {
        setState(ctx, ConnectionState::Connecting, tr("Connecting to UZX..."));
        emit logMessage(QStringLiteral("%1 Connecting to UZX private WebSocket...")
                            .arg(contextTag(ctx.accountName)));
        initializeUzxWebSocket(ctx);
    } else {
        setState(ctx, ConnectionState::Connecting, tr("Requesting listen key..."));
        emit logMessage(QStringLiteral("%1 Requesting listen key from MEXC...")
                            .arg(contextTag(ctx.accountName)));
        requestListenKey(ctx);
    }
}

void TradeManager::disconnect(ConnectionStore::Profile profile)
{
    Context *ctx = contextForProfile(profile);
    if (!ctx) {
        return;
    }
    ctx->proxyPreflightToken++;
    ctx->proxyPreflightInFlight = false;
    closeWebSocket(*ctx);
    ctx->keepAliveTimer.stop();
    ctx->reconnectTimer.stop();
    ctx->wsPingTimer.stop();
    ctx->openOrdersTimer.stop();
    ctx->futuresDealsTimer.stop();
    ctx->lighterAccountTimer.stop();
    ctx->lighterTradesTimer.stop();
    ctx->lighterBurstTimer.stop();
    ctx->futuresDealsInFlight.clear();
    ctx->openOrdersPending = false;
    ctx->futuresPositionsPending = false;
    clearLocalOrderSnapshots(*ctx);
    ctx->listenKey.clear();
    ctx->lighterAuthToken.clear();
    ctx->hasSubscribed = false;
    setState(*ctx, ConnectionState::Disconnected, tr("Disconnected"));
    emit logMessage(QStringLiteral("%1 Disconnected").arg(contextTag(ctx->accountName)));
}

bool TradeManager::isConnected(ConnectionStore::Profile profile) const
{
    return state(profile) == ConnectionState::Connected;
}

QString TradeManager::accountNameFor(ConnectionStore::Profile profile) const
{
    if (Context *ctx = contextForProfile(profile)) {
        return ctx->accountName;
    }
    return defaultAccountName(profile);
}

ConnectionStore::Profile TradeManager::profileFromAccountName(const QString &accountName) const
{
    if (accountName.isEmpty()) {
        return ConnectionStore::Profile::MexcSpot;
    }
    const QString lower = accountName.trimmed().toLower();
    for (auto it = m_contexts.constBegin(); it != m_contexts.constEnd(); ++it) {
        if (it.value()->accountName.trimmed().toLower() == lower) {
            return it.key();
        }
    }
    if (lower.contains(QStringLiteral("binance"))) {
        if (lower.contains(QStringLiteral("futures")) || lower.contains(QStringLiteral("future"))) {
            return ConnectionStore::Profile::BinanceFutures;
        }
        return ConnectionStore::Profile::BinanceSpot;
    }
    if (lower.contains(QStringLiteral("lighter"))) {
        return ConnectionStore::Profile::Lighter;
    }
    if (lower.contains(QStringLiteral("futures"))) {
        return ConnectionStore::Profile::MexcFutures;
    }
    if (lower.contains(QStringLiteral("swap"))) {
        return ConnectionStore::Profile::UzxSwap;
    }
    if (lower.contains(QStringLiteral("spot")) && lower.contains(QStringLiteral("uzx"))) {
        return ConnectionStore::Profile::UzxSpot;
    }
    return ConnectionStore::Profile::MexcSpot;
}

TradePosition TradeManager::positionForSymbol(const QString &symbol, const QString &accountName) const
{
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    if (Context *ctx = contextForProfile(profile)) {
        return ctx->positions.value(normalizedSymbol(symbol), TradePosition{});
    }
    return TradePosition{};
}

QVector<ExecutedTrade> TradeManager::executedTrades() const
{
    return m_executedTrades;
}

void TradeManager::clearExecutedTrades()
{
    m_executedTrades.clear();
    QFile::remove(tradeHistoryPath());
    emit tradeHistoryCleared();
}

void TradeManager::setWatchedSymbols(const QString &accountName, const QSet<QString> &symbols)
{
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    Context &ctx = ensureContext(profile);

    QSet<QString> norm;
    for (const auto &s : symbols) {
        const QString sym = normalizedSymbol(s);
        if (!sym.isEmpty()) {
            norm.insert(sym);
        }
    }
    ctx.watchedSymbols = std::move(norm);

    if (profile != ConnectionStore::Profile::MexcSpot) {
        if (profile == ConnectionStore::Profile::MexcFutures && ctx.state == ConnectionState::Connected) {
            if (!ctx.futuresDealsTimer.isActive() && !ctx.watchedSymbols.isEmpty()) {
                ctx.futuresDealsTimer.start();
            }
            if (ctx.watchedSymbols.isEmpty()) {
                ctx.futuresDealsTimer.stop();
            } else {
                pollFuturesDeals(ctx);
            }
        } else if (profile == ConnectionStore::Profile::Lighter && ctx.state == ConnectionState::Connected) {
            if (!ctx.lighterAccountTimer.isActive() && !ctx.watchedSymbols.isEmpty()) {
                ctx.lighterAccountTimer.start();
            }
            if (!ctx.lighterTradesTimer.isActive() && !ctx.watchedSymbols.isEmpty()) {
                ctx.lighterTradesTimer.start();
            }
            if (ctx.watchedSymbols.isEmpty()) {
                ctx.lighterAccountTimer.stop();
                ctx.lighterTradesTimer.stop();
                ctx.lighterBurstTimer.stop();
            } else {
                pollLighterAccount(ctx);
                pollLighterTrades(ctx);
            }
        }
        if ((profile == ConnectionStore::Profile::UzxSwap
             || profile == ConnectionStore::Profile::UzxSpot)
            && ctx.state == ConnectionState::Connected) {
            if (!ctx.openOrdersTimer.isActive() && !ctx.watchedSymbols.isEmpty()) {
                ctx.openOrdersTimer.start();
            }
            if (ctx.watchedSymbols.isEmpty()) {
                ctx.openOrdersTimer.stop();
            } else {
                fetchOpenOrders(ctx);
                if (profile == ConnectionStore::Profile::UzxSwap) {
                    fetchUzxSwapPositions(ctx);
                }
            }
        }
        return;
    }
    if (ctx.state == ConnectionState::Connected) {
        if (!ctx.myTradesTimer.isActive() && !ctx.watchedSymbols.isEmpty()) {
            ctx.myTradesTimer.start();
        }
        if (ctx.watchedSymbols.isEmpty()) {
            ctx.myTradesTimer.stop();
        } else {
            pollMyTrades(ctx);
        }
    }
}
void TradeManager::placeLimitOrder(const QString &symbol,
                                   const QString &accountName,
                                   double price,
                                   double quantity,
                                   OrderSide side,
                                   int leverage)
{
    const QString sym = normalizedSymbol(symbol);
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    if (profile != ConnectionStore::Profile::Lighter
        && profile != ConnectionStore::Profile::UzxSwap
        && profile != ConnectionStore::Profile::UzxSpot) {
        return;
    }
    Context &ctx = ensureContext(profile);
    if (!ensureCredentials(ctx)) {
        emit orderFailed(ctx.accountName, sym, tr("Missing credentials"));
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        emit orderFailed(ctx.accountName, sym, tr("Connect to the exchange first"));
        return;
    }
    if (price <= 0.0 || quantity <= 0.0) {
        emit orderFailed(ctx.accountName, sym, tr("Invalid price or quantity"));
        return;
    }

    emit logMessage(QStringLiteral("%1 Placing limit order: %2 %3 @ %4 qty=%5")
                        .arg(contextTag(ctx.accountName))
                        .arg(sym)
                        .arg(side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                        .arg(QString::number(price, 'f', 6))
                        .arg(QString::number(quantity, 'f', 6)));

    if (profile == ConnectionStore::Profile::Lighter) {
        const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
        const QString account = ctx.accountName;
        const int apiKeyIndex = ctx.credentials.apiKeyIndex;
        const long long accountIndex = static_cast<long long>(ctx.credentials.accountIndex);
        Context *ctxPtrForLev = &ctx;

        auto fail = [this, account, sym](const QString &msg) {
            emit orderFailed(account, sym, msg);
            emit logMessage(QStringLiteral("%1 Lighter order error: %2")
                                .arg(contextTag(account), msg));
        };

        if (baseUrl.isEmpty()) {
            fail(tr("Missing Lighter base URL"));
            return;
        }

        auto logLastInactiveOrder = [this, baseUrl, &ctx, account, sym](int marketId) {
            if (ctx.lighterAuthToken.isEmpty()) {
                emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup skipped (missing auth token)")
                                    .arg(contextTag(account)));
                return;
            }
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/accountInactiveOrders"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("account_index"), QString::number(ctx.credentials.accountIndex));
            q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
            if (marketId >= 0) {
                q.addQueryItem(QStringLiteral("market_id"), QString::number(marketId));
            }
            q.addQueryItem(QStringLiteral("auth"), ctx.lighterAuthToken);
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req.setRawHeader("authorization", ctx.lighterAuthToken.toUtf8());
            QNetworkReply *reply = ensureLighterNetwork(ctx)->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, account, sym]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup failed: %2")
                                        .arg(contextTag(account), lighterHttpErrorMessage(reply, raw)));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup: invalid response")
                                        .arg(contextTag(account)));
                    return;
                }
                const QJsonObject obj = doc.object();
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (code != 200) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup error: %2")
                                        .arg(contextTag(account),
                                             obj.value(QStringLiteral("message"))
                                                 .toString(QStringLiteral("Unknown error"))));
                    return;
                }
                const QJsonArray orders = obj.value(QStringLiteral("orders")).toArray();
                if (orders.isEmpty()) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup: no orders found for %2")
                                        .arg(contextTag(account), sym));
                    return;
                }
                const QJsonObject o = orders.first().toObject();
                const QString type = o.value(QStringLiteral("type")).toString();
                const QString tif = o.value(QStringLiteral("time_in_force")).toString();
                const QString reduceOnly = o.value(QStringLiteral("reduce_only")).toVariant().toString();
                const QString price = o.value(QStringLiteral("price")).toVariant().toString();
                const QString baseSize = o.value(QStringLiteral("base_size")).toVariant().toString();
                const QString baseAmount = o.value(QStringLiteral("initial_base_amount")).toVariant().toString();
                const QString marketIndex = o.value(QStringLiteral("market_index")).toVariant().toString();
                const QString isAsk = o.value(QStringLiteral("is_ask")).toVariant().toString();
                const QString statusStr = o.value(QStringLiteral("status")).toVariant().toString();
                emit logMessage(QStringLiteral("%1 Lighter last inactive order: market=%2 type=%3 tif=%4 reduceOnly=%5 price=%6 base_size=%7 base_amount=%8 is_ask=%9 status=%10")
                                    .arg(contextTag(account),
                                         marketIndex,
                                         type,
                                         tif,
                                         reduceOnly,
                                         price,
                                         baseSize,
                                         baseAmount,
                                         isAsk,
                                         statusStr));
            });
        };

        // Avoid loading the full markets blob (it can be slow/hang on some networks).
        // For trading we only need meta for the specific symbol, so:
        // 1) `/api/v1/orderBooks?market_id=255` to map symbol -> market_id (fast)
        // 2) `/api/v1/orderBookDetails?market_id=...` to get decimals/margins for signing (small)
        auto ensureMetaLoaded = [this, baseUrl, ctxPtr = &ctx, sym](std::function<void(QString err)> cb) {
            auto &cache = lighterMetaCacheByUrl()[baseUrl];
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const bool freshEnough = (cache.updatedMs > 0 && (now - cache.updatedMs) < 20 * 60 * 1000);
            const auto it = cache.bySymbol.constFind(sym);
            if (it != cache.bySymbol.constEnd() && it->marketId >= 0 && it->priceDecimals > 0 && freshEnough) {
                cb(QString());
                return;
            }

            cache.waiters.push_back(std::move(cb));
            if (cache.inFlight) {
                return;
            }
            cache.inFlight = true;
            auto parseApiErr = [](const QByteArray &raw, int *outCode, QString *outMsg) {
                if (outCode) *outCode = 0;
                if (outMsg) outMsg->clear();
                if (raw.isEmpty()) return;
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (!doc.isObject()) return;
                const QJsonObject obj = doc.object();
                if (outCode) *outCode = obj.value(QStringLiteral("code")).toInt(0);
                if (outMsg) *outMsg = obj.value(QStringLiteral("message")).toString();
            };

            std::function<void(const QString &paramName, bool allowRetry)> fetchOrderBooks;
            fetchOrderBooks = [this, baseUrl, ctxPtr, sym, parseApiErr, &fetchOrderBooks](const QString &paramName, bool allowRetry) {
                QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBooks"));
                QUrlQuery q;
                q.addQueryItem(paramName, QStringLiteral("255"));
                url.setQuery(q);
                QNetworkRequest req(url);
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
                applyReplyTimeout(reply, 6000);
                connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr, baseUrl, sym, paramName, allowRetry, parseApiErr, &fetchOrderBooks]() {
                    auto &cache2 = lighterMetaCacheByUrl()[baseUrl];
                    auto flush = [&](const QString &errMsg) {
                        cache2.inFlight = false;
                        const auto waiters = std::move(cache2.waiters);
                        cache2.waiters.clear();
                        for (const auto &w : waiters) {
                            w(errMsg);
                        }
                    };

                    const QNetworkReply::NetworkError err = reply->error();
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QString errStr = reply->errorString();
                    const QByteArray raw = reply->readAll();
                    const bool timedOut = reply->property("fusion_timeout").toBool();
                    reply->deleteLater();

                    int apiCode = 0;
                    QString apiMsg;
                    parseApiErr(raw, &apiCode, &apiMsg);
                    const bool invalidParam =
                        (apiCode == 20001 || apiMsg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive));

                    if (allowRetry && invalidParam) {
                        const QString otherParam =
                            (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                        : QStringLiteral("market_id");
                        fetchOrderBooks(otherParam, false);
                        return;
                    }

                    if (timedOut && err == QNetworkReply::OperationCanceledError) {
                        flush(QStringLiteral("Lighter markets request timed out"));
                        return;
                    }
                    if (err != QNetworkReply::NoError || status >= 400) {
                        const QString body = raw.isEmpty() ? errStr : QString::fromUtf8(raw);
                        flush(QStringLiteral("Failed to load Lighter markets: %1").arg(body));
                        return;
                    }

                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isNull() || !doc.isObject()) {
                        flush(QStringLiteral("Failed to parse Lighter markets"));
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    const int code = obj.value(QStringLiteral("code")).toInt(0);
                    if (code != 200) {
                        flush(obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter markets error")));
                        return;
                    }

                    auto toInt = [](const QJsonValue &v) -> int {
                        if (v.isDouble()) {
                            return static_cast<int>(v.toDouble());
                        }
                        if (v.isString()) {
                            bool ok = false;
                            const int out = v.toString().trimmed().toInt(&ok);
                            return ok ? out : -1;
                        }
                        return v.toInt(-1);
                    };

                    int marketId = -1;
                    QJsonValue booksVal = obj.value(QStringLiteral("order_books"));
                    if (!booksVal.isArray()) {
                        booksVal = obj.value(QStringLiteral("orderBooks"));
                    }
                    const QJsonArray books = booksVal.toArray();
                    for (const QJsonValue &v : books) {
                        if (!v.isObject()) continue;
                        const QJsonObject o = v.toObject();
                        const QString s = normalizedSymbol(o.value(QStringLiteral("symbol")).toString());
                        if (s.compare(sym, Qt::CaseInsensitive) != 0) continue;
                        marketId = toInt(o.value(QStringLiteral("market_id")));
                        if (marketId < 0) {
                            marketId = toInt(o.value(QStringLiteral("market_index")));
                        }
                        if (marketId < 0) {
                            marketId = toInt(o.value(QStringLiteral("marketId")));
                        }
                        break;
                    }
                    if (marketId < 0) {
                        flush(QStringLiteral("Lighter market not found for %1").arg(sym));
                        return;
                    }

                    std::function<void(const QString &detailParam, bool allowDetailRetry)> fetchDetails;
                    fetchDetails = [this, ctxPtr, baseUrl, marketId, sym, parseApiErr, &fetchDetails](const QString &detailParam, bool allowDetailRetry) {
                        QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookDetails"));
                        QUrlQuery q;
                        q.addQueryItem(detailParam, QString::number(marketId));
                        url.setQuery(q);
                        QNetworkRequest req2(url);
                        req2.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                        QNetworkReply *reply2 = ensureLighterNetwork(*ctxPtr)->get(req2);
                        applyReplyTimeout(reply2, 6000);
                        connect(reply2, &QNetworkReply::finished, this, [this, reply2, baseUrl, marketId, sym, detailParam, allowDetailRetry, parseApiErr, &fetchDetails]() {
                            auto &cache3 = lighterMetaCacheByUrl()[baseUrl];
                            auto flush2 = [&](const QString &errMsg) {
                                cache3.inFlight = false;
                                const auto waiters = std::move(cache3.waiters);
                                cache3.waiters.clear();
                                for (const auto &w : waiters) {
                                    w(errMsg);
                                }
                            };

                            const QNetworkReply::NetworkError err = reply2->error();
                            const int status = reply2->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                            const QString errStr = reply2->errorString();
                            const QByteArray raw = reply2->readAll();
                            const bool timedOut = reply2->property("fusion_timeout").toBool();
                            reply2->deleteLater();

                            int apiCode = 0;
                            QString apiMsg;
                            parseApiErr(raw, &apiCode, &apiMsg);
                            const bool invalidParam =
                                (apiCode == 20001 || apiMsg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive));
                            if (allowDetailRetry && invalidParam) {
                                const QString otherParam =
                                    (detailParam == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                                  : QStringLiteral("market_id");
                                fetchDetails(otherParam, false);
                                return;
                            }

                            if (timedOut && err == QNetworkReply::OperationCanceledError) {
                                flush2(QStringLiteral("Lighter market details request timed out"));
                                return;
                            }
                            if (err != QNetworkReply::NoError || status >= 400) {
                                flush2(QStringLiteral("Failed to load Lighter market details: %1")
                                           .arg(raw.isEmpty() ? errStr : QString::fromUtf8(raw)));
                                return;
                            }
                            const QJsonDocument doc = QJsonDocument::fromJson(raw);
                            if (doc.isNull() || !doc.isObject()) {
                                flush2(QStringLiteral("Failed to parse Lighter market details"));
                                return;
                            }
                            const QJsonObject obj = doc.object();
                            const int code = obj.value(QStringLiteral("code")).toInt(0);
                            if (code != 200) {
                                flush2(obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter market error")));
                                return;
                            }
                            // Merge just the requested market into the cache.
                            addLighterMarketMeta(cache3.bySymbol, obj.value(QStringLiteral("order_book_details")).toArray(), false);
                            addLighterMarketMeta(cache3.bySymbol, obj.value(QStringLiteral("spot_order_book_details")).toArray(), true);
                            cache3.symbolByMarketId.insert(marketId, sym);
                            cache3.updatedMs = QDateTime::currentMSecsSinceEpoch();

                            const auto metaIt = cache3.bySymbol.constFind(sym);
                            if (metaIt == cache3.bySymbol.constEnd() || metaIt->marketId < 0 || metaIt->priceDecimals <= 0) {
                                flush2(QStringLiteral("Invalid Lighter meta for %1").arg(sym));
                                return;
                            }
                            flush2(QString());
                        });
                    };

                    fetchDetails(QStringLiteral("market_id"), true);
                });
            };

            fetchOrderBooks(QStringLiteral("market_id"), true);
        };

        auto fetchTopBookPrice = [this, baseUrl, ctxPtr = &ctx](int marketId, bool isAsk, std::function<void(double price, QString err)> cb) {
            auto parseErr = [](const QByteArray &raw, int *outCode, QString *outMsg) {
                if (outCode) {
                    *outCode = 0;
                }
                if (outMsg) {
                    outMsg->clear();
                }
                if (raw.isEmpty()) {
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (!doc.isObject()) {
                    return;
                }
                const QJsonObject obj = doc.object();
                if (outCode) {
                    *outCode = obj.value(QStringLiteral("code")).toInt(0);
                }
                if (outMsg) {
                    *outMsg = obj.value(QStringLiteral("message")).toString();
                }
            };

            std::function<void(const QString &paramName, bool allowRetry)> doRequest;
            doRequest = [this, baseUrl, marketId, isAsk, cb, parseErr, &doRequest, ctxPtr](const QString &paramName, bool allowRetry) {
                QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookOrders"));
                QUrlQuery q;
                q.addQueryItem(paramName, QString::number(marketId));
                q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
                url.setQuery(q);
                QNetworkRequest req(url);
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
                connect(reply, &QNetworkReply::finished, this, [reply, isAsk, cb, paramName, allowRetry, parseErr, &doRequest]() {
                    const QNetworkReply::NetworkError err = reply->error();
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QByteArray raw = reply->readAll();
                    reply->deleteLater();
                    int code = 0;
                    QString msg;
                    parseErr(raw, &code, &msg);
                    if (err != QNetworkReply::NoError || status >= 400) {
                        if (allowRetry && code == 20001
                            && msg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                            const QString otherParam =
                                (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                            : QStringLiteral("market_id");
                            doRequest(otherParam, false);
                            return;
                        }
                        const QString errMsg = msg.isEmpty() ? lighterHttpErrorMessage(reply, raw) : msg;
                        cb(0.0, errMsg);
                        return;
                    }
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isNull() || !doc.isObject()) {
                        cb(0.0, QStringLiteral("Invalid Lighter order book response"));
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    const int objCode = obj.value(QStringLiteral("code")).toInt(0);
                    if (objCode != 200) {
                        const QString objMsg = obj.value(QStringLiteral("message"))
                                                   .toString(QStringLiteral("Lighter order book error"));
                        if (allowRetry && objCode == 20001
                            && objMsg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                            const QString otherParam =
                                (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                            : QStringLiteral("market_id");
                            doRequest(otherParam, false);
                            return;
                        }
                        cb(0.0, objMsg);
                        return;
                    }
                    QJsonObject bookObj = obj;
                    if (obj.value(QStringLiteral("order_book_orders")).isObject()) {
                        bookObj = obj.value(QStringLiteral("order_book_orders")).toObject();
                    }
                    const QJsonArray bids = bookObj.value(QStringLiteral("bids")).toArray();
                    const QJsonArray asks = bookObj.value(QStringLiteral("asks")).toArray();
                    const QJsonArray side = isAsk ? bids : asks;
                    if (side.isEmpty()) {
                        cb(0.0, QStringLiteral("Empty Lighter order book"));
                        return;
                    }
                    const QJsonObject top = side.first().toObject();
                    const QJsonValue priceVal = top.value(QStringLiteral("price"));
                    bool ok = false;
                    double price = 0.0;
                    if (priceVal.isString()) {
                        price = priceVal.toString().toDouble(&ok);
                    } else if (priceVal.isDouble()) {
                        price = priceVal.toDouble();
                        ok = price > 0.0;
                    }
                    if (!ok || !(price > 0.0)) {
                        cb(0.0, QStringLiteral("Invalid Lighter order book price"));
                        return;
                    }
                    cb(price, QString());
                });
            };

            doRequest(QStringLiteral("market_id"), true);
        };

        auto fetchNonce = [this, baseUrl, apiKeyIndex, accountIndex, ctxPtr = &ctx](std::function<void(qint64 nonce, QString err)> cb) {
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/nextNonce"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("account_index"), QString::number(accountIndex));
            q.addQueryItem(QStringLiteral("api_key_index"), QString::number(apiKeyIndex));
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
            applyReplyTimeout(reply, 4500);
            connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    cb(0, lighterHttpErrorMessage(reply, raw));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    cb(0, QStringLiteral("Invalid Lighter nonce response"));
                    return;
                }
                const QJsonObject obj = doc.object();
                if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                    cb(0, obj.value(QStringLiteral("message")).toString(QStringLiteral("Failed to get nonce")));
                    return;
                }
                cb(static_cast<qint64>(obj.value(QStringLiteral("nonce")).toVariant().toLongLong()), QString());
            });
        };

        auto ensureNonce = [this, fetchNonce, ctxPtrForLev, fail](std::function<void(qint64 nonce)> cb) {
            if (!ctxPtrForLev) {
                fail(QStringLiteral("Internal error"));
                return;
            }
            if (ctxPtrForLev->lighterNextNonce > 0 && !ctxPtrForLev->lighterNonceInFlight) {
                const qint64 nonce = ctxPtrForLev->lighterNextNonce;
                ctxPtrForLev->lighterNextNonce = nonce + 1;
                cb(nonce);
                return;
            }
            ctxPtrForLev->lighterNonceWaiters.push_back([cb](qint64 nonce, const QString &err) {
                if (!err.isEmpty() || nonce <= 0) {
                    return;
                }
                cb(nonce);
            });
            if (ctxPtrForLev->lighterNonceInFlight) {
                return;
            }
            ctxPtrForLev->lighterNonceInFlight = true;
            fetchNonce([this, ctxPtrForLev, fail](qint64 nonce, const QString &err) {
                if (!ctxPtrForLev) {
                    return;
                }
                ctxPtrForLev->lighterNonceInFlight = false;
                if (!err.isEmpty() || nonce <= 0) {
                    ctxPtrForLev->lighterNextNonce = 0;
                    const auto waiters = std::move(ctxPtrForLev->lighterNonceWaiters);
                    ctxPtrForLev->lighterNonceWaiters.clear();
                    for (const auto &w : waiters) {
                        w(0, err.isEmpty() ? QStringLiteral("Failed to get nonce") : err);
                    }
                    if (!err.isEmpty()) {
                        fail(err);
                    }
                    return;
                }
                const auto waiters = std::move(ctxPtrForLev->lighterNonceWaiters);
                ctxPtrForLev->lighterNonceWaiters.clear();
                ctxPtrForLev->lighterNextNonce = nonce;
                for (const auto &w : waiters) {
                    const qint64 reserved = ctxPtrForLev->lighterNextNonce;
                    ctxPtrForLev->lighterNextNonce = reserved + 1;
                    w(reserved, QString());
                }
            });
        };

        auto sendTx = [this, ctxPtrForLev](int txType,
                                           const QString &txInfo,
                                           std::function<void(QString txHash, QString err)> cb) {
            if (!ctxPtrForLev) {
                cb(QString(), QStringLiteral("Internal error"));
                return;
            }
            sendLighterTx(*ctxPtrForLev, txType, txInfo, std::move(cb));
        };

        ensureMetaLoaded([this,
                          sym,
                          price,
                          quantity,
                          side,
                          leverage,
                          apiKeyIndex,
                          accountIndex,
                          baseUrl,
                          account,
                          ctxPtrForLev,
                          fail,
                          ensureNonce,
                          sendTx](const QString &metaErr) {
            if (!metaErr.isEmpty()) {
                fail(metaErr);
                return;
            }
            const auto &cache = lighterMetaCacheByUrl().value(baseUrl);
            const LighterMarketMeta meta = cache.bySymbol.value(sym, LighterMarketMeta{});
            if (meta.marketId < 0) {
                fail(QStringLiteral("Unknown Lighter symbol: %1").arg(sym));
                return;
            }

            const double quoteNotional = price * quantity;
            if (meta.minBaseAmount > 0.0 && quantity + 1e-12 < meta.minBaseAmount) {
                fail(QStringLiteral("Lighter min base amount for %1 is %2 (you tried %3). Increase order size.")
                         .arg(sym)
                         .arg(meta.minBaseAmount, 0, 'f', 8)
                         .arg(quantity, 0, 'f', 8));
                return;
            }
            if (meta.minQuoteAmount > 0.0 && quoteNotional + 1e-12 < meta.minQuoteAmount) {
                fail(QStringLiteral("Lighter min quote amount for %1 is %2 USDC (you tried %3). Increase order notional or deposit more.")
                         .arg(sym)
                         .arg(meta.minQuoteAmount, 0, 'f', 8)
                         .arg(quoteNotional, 0, 'f', 8));
                return;
            }

            const qint64 baseScale = pow10i(meta.sizeDecimals);
            const qint64 priceScale = pow10i(meta.priceDecimals);
            const qint64 baseAmount = qRound64(quantity * static_cast<double>(baseScale));
            const qint64 priceInt64 = qRound64(price * static_cast<double>(priceScale));
            if (baseAmount <= 0 || priceInt64 <= 0 || priceInt64 > std::numeric_limits<int>::max()) {
                fail(QStringLiteral("Invalid scaled price/qty for %1").arg(sym));
                return;
            }

            ensureNonce([this,
                         sym,
                         meta,
                         baseAmount,
                         priceInt64,
                         side,
                         leverage,
                         apiKeyIndex,
                         accountIndex,
                         price,
                         quantity,
                         account,
                         ctxPtrForLev,
                         fail,
                         sendTx](qint64 nonce) {
                if (!ctxPtrForLev || nonce <= 0) {
                    fail(QStringLiteral("Failed to get nonce"));
                    return;
                }

                auto sendOrderWithNonce = [this,
                                           sym,
                                           meta,
                                           baseAmount,
                                           priceInt64,
                                           side,
                                           apiKeyIndex,
                                           accountIndex,
                                           price,
                                           quantity,
                                           account,
                                           fail,
                                           sendTx,
                                           ctxPtrForLev](qint64 orderNonce) {
                    qint64 clientOrderIndex = 0;
                    if (ctxPtrForLev) {
                        clientOrderIndex = nextLighterClientOrderIndex(*ctxPtrForLev);
                    } else {
                        clientOrderIndex = QDateTime::currentMSecsSinceEpoch() & 0x7fffffffLL;
                        if (clientOrderIndex <= 0) clientOrderIndex = 1;
                    }
                    const int isAsk = (side == OrderSide::Sell) ? 1 : 0;
                    const int orderType = 0; // limit
                    const int timeInForce = 1; // good till time
                    const int reduceOnly = 0;
                    const int triggerPrice = 0;
                    const qint64 orderExpiry = -1; // 28d default in signer

                    const LighterSignedTxResponse signedTx =
                        lighterSigner().signCreateOrder(meta.marketId,
                                                        clientOrderIndex,
                                                        static_cast<long long>(baseAmount),
                                                        static_cast<int>(priceInt64),
                                                        isAsk,
                                                        orderType,
                                                        timeInForce,
                                                        reduceOnly,
                                                        triggerPrice,
                                                        static_cast<long long>(orderExpiry),
                                                        static_cast<long long>(orderNonce),
                                                        apiKeyIndex,
                                                        accountIndex);

                    if (signedTx.err) {
                        fail(QString::fromUtf8(signedTx.err));
                        return;
                    }
                    const QString txInfo = signedTx.txInfo ? QString::fromUtf8(signedTx.txInfo) : QString();
                    if (txInfo.isEmpty()) {
                        fail(QStringLiteral("Empty signed txInfo"));
                        return;
                    }

                    sendTx(static_cast<int>(signedTx.txType),
                           txInfo,
                           [this, sym, side, price, quantity, account, fail, ctxPtrForLev, orderNonce, clientOrderIndex](const QString &txHash, const QString &txErr) {
                        if (!txErr.isEmpty()) {
                            if (ctxPtrForLev) {
                                ctxPtrForLev->lighterNextNonce = 0;
                            }
                            fail(txErr);
                            return;
                        }
                        if (ctxPtrForLev) {
                            ctxPtrForLev->lighterNextNonce = orderNonce + 1;
                        }
                        // Use clientOrderIndex as the marker/order id so we can match it to
                        // private WS `clientId` and accountActiveOrders responses.
                        const QString orderId = QString::number(clientOrderIndex);
                        emit orderPlaced(account, sym, side, price, quantity, orderId);
                         emit logMessage(QStringLiteral("%1 Lighter order sent: %2 %3 @ %4 (tx=%5)")
                                             .arg(contextTag(account))
                                             .arg(side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"))
                                             .arg(quantity, 0, 'f', 6)
                                             .arg(price, 0, 'f', 8)
                                             .arg(txHash.left(12)));
                        if (ctxPtrForLev) {
                            armLighterBurst(*ctxPtrForLev, 8);
                        }
                    });
                };

                const bool isPerp = !meta.isSpot;
                const int levRequested = leverage > 0 ? leverage : 0;
                int lev = levRequested;
                if (isPerp && lev > 0) {
                    const int maxLev =
                        (meta.minInitialMarginFraction > 0)
                            ? std::max(1, 10000 / meta.minInitialMarginFraction)
                            : 10;
                    if (lev > maxLev) {
                        lev = maxLev;
                        emit logMessage(QStringLiteral("%1 Lighter leverage clamped for %2: requested %3x, max %4x")
                                            .arg(contextTag(account), sym)
                                            .arg(levRequested)
                                            .arg(maxLev));
                    }
                }

                const int lastLev = (ctxPtrForLev ? ctxPtrForLev->lighterLeverageByMarketId.value(meta.marketId, 0) : 0);
                if (!isPerp || lev <= 0 || lev == lastLev) {
                    sendOrderWithNonce(nonce);
                    return;
                }

                // Update leverage first, then place the order with nonce+1.
                int initialMarginFraction = static_cast<int>(qRound64(10000.0 / static_cast<double>(lev)));
                initialMarginFraction = std::clamp(initialMarginFraction, 1, 10000);
                if (meta.minInitialMarginFraction > 0) {
                    initialMarginFraction = std::max(initialMarginFraction, meta.minInitialMarginFraction);
                }
                const int marginMode = 0; // cross

                const LighterSignedTxResponse levTx =
                    lighterSigner().signUpdateLeverage(meta.marketId,
                                                       initialMarginFraction,
                                                       marginMode,
                                                       static_cast<long long>(nonce),
                                                       apiKeyIndex,
                                                       accountIndex);
                if (levTx.err) {
                    fail(QString::fromUtf8(levTx.err));
                    return;
                }
                const QString levInfo = levTx.txInfo ? QString::fromUtf8(levTx.txInfo) : QString();
                if (levInfo.isEmpty()) {
                    fail(QStringLiteral("Empty signed leverage txInfo"));
                    return;
                }

                const qint64 nextNonce = nonce + 1;
                sendTx(static_cast<int>(levTx.txType), levInfo, [this, sym, account, fail, sendOrderWithNonce, lev, meta, ctxPtrForLev, nextNonce](const QString &txHash, const QString &txErr) {
                    Q_UNUSED(txHash);
                    if (!txErr.isEmpty()) {
                        if (ctxPtrForLev) {
                            ctxPtrForLev->lighterNextNonce = 0;
                        }
                        fail(txErr);
                        return;
                    }
                    if (ctxPtrForLev) {
                        ctxPtrForLev->lighterLeverageByMarketId.insert(meta.marketId, lev);
                        ctxPtrForLev->lighterNextNonce = nextNonce;
                    }
                    emit logMessage(QStringLiteral("%1 Lighter leverage set for %2: %3x")
                                        .arg(contextTag(account), sym)
                                        .arg(lev));
                    sendOrderWithNonce(nextNonce);
                });
            });
        });
        return;
    }

    if (profile == ConnectionStore::Profile::MexcFutures) {
        placeMexcFuturesOrder(ctx, sym, price, quantity, side, leverage);
        return;
    }

    if (profile == ConnectionStore::Profile::UzxSwap
        || profile == ConnectionStore::Profile::UzxSpot) {
        const bool isSwap = (profile == ConnectionStore::Profile::UzxSwap);
        const QString wireSym = uzxWireSymbol(sym, isSwap);
        const QString priceStr = QString::number(price, 'f', 8);
        const QString amountStr = QString::number(quantity, 'f', 8);
        auto sendOrder = [this, &ctx, sym, side, price, quantity, wireSym, amountStr, priceStr, isSwap]() {
            QJsonObject payload;
            payload.insert(QStringLiteral("product_name"), wireSym);
            payload.insert(QStringLiteral("order_type"), 2);
            payload.insert(QStringLiteral("price"), priceStr);
            payload.insert(QStringLiteral("amount"), amountStr);
            payload.insert(QStringLiteral("order_buy_or_sell"), side == OrderSide::Buy ? 1 : 2);
            if (isSwap) {
                payload.insert(QStringLiteral("number"), amountStr);
                payload.insert(QStringLiteral("trade_ccy"), 1);
                payload.insert(QStringLiteral("pos_side"),
                               side == OrderSide::Buy ? QStringLiteral("LG") : QStringLiteral("ST"));
            }
            const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            emit logMessage(QStringLiteral("%1 UZX REST body: %2")
                                .arg(contextTag(ctx.accountName), QString::fromUtf8(body)));
            const QString path = isSwap ? QStringLiteral("/v2/trade/swap/order")
                                        : QStringLiteral("/v2/trade/spot/order");
            QNetworkRequest req = makeUzxRequest(path, body, QStringLiteral("POST"), QUrlQuery(), ctx);
            auto *reply = m_network.post(req, body);
            connect(reply,
                    &QNetworkReply::finished,
                    this,
                    [this, reply, sym, side, price, quantity, ctxPtr = &ctx]() {
                        const auto err = reply->error();
                        const int status =
                            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        const QByteArray raw = reply->readAll();
                        reply->deleteLater();
                        if (err != QNetworkReply::NoError || status >= 400) {
                            QString msg = reply->errorString();
                            if (status >= 400) {
                                msg = QStringLiteral("HTTP %1: %2")
                                          .arg(status)
                                          .arg(QString::fromUtf8(raw));
                            }
                            emit orderFailed(ctxPtr->accountName, sym, msg);
                            emit logMessage(QStringLiteral("%1 UZX order error: %2")
                                                .arg(contextTag(ctxPtr->accountName), msg));
                            return;
                        }
                        const QString resp = QString::fromUtf8(raw);
                        emit logMessage(QStringLiteral("%1 UZX order response: %2")
                                            .arg(contextTag(ctxPtr->accountName),
                                                 resp.isEmpty() ? QStringLiteral("<empty>") : resp));
                        bool accepted = true;
                        QJsonDocument uzxDoc = QJsonDocument::fromJson(raw);
                        if (!uzxDoc.isNull() && uzxDoc.isObject()) {
                            const QJsonObject obj = uzxDoc.object();
                            const int code = obj.value(QStringLiteral("code")).toInt(0);
                            if (code != 0 && code != 200) {
                                const QString rawMsg = obj.value(QStringLiteral("msg"))
                                                           .toString(QStringLiteral("request error"));
                                const QString msg = uzxFormatError(code, rawMsg);
                                const QString msgWithCode =
                                    QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                                emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                                emit logMessage(QStringLiteral("%1 UZX order rejected: %2")
                                                    .arg(contextTag(ctxPtr->accountName))
                                                    .arg(msgWithCode));
                                accepted = false;
                            }
                        }
                        QString extractedOrderId;
                        if (!uzxDoc.isObject() && !resp.trimmed().isEmpty()) {
                            emit logMessage(QStringLiteral("%1 UZX response not JSON, assuming success")
                                                .arg(contextTag(ctxPtr->accountName)));
                        }
                        if (uzxDoc.isObject()) {
                            const QJsonObject obj = uzxDoc.object();
                            const QJsonObject dataObj = obj.value(QStringLiteral("data")).toObject();
                            extractedOrderId = dataObj.value(QStringLiteral("order_id")).toString();
                            if (extractedOrderId.isEmpty()) {
                                extractedOrderId = dataObj.value(QStringLiteral("orderId")).toString();
                            }
                            if (extractedOrderId.isEmpty()) {
                                extractedOrderId = obj.value(QStringLiteral("order_id")).toString();
                            }
                            if (extractedOrderId.isEmpty()) {
                                extractedOrderId = obj.value(QStringLiteral("orderId")).toString();
                            }
                            if (extractedOrderId.isEmpty()) {
                                extractedOrderId = dataObj.value(QStringLiteral("id")).toString();
                            }
                        }
                        if (extractedOrderId.isEmpty()) {
                            extractedOrderId = QStringLiteral("uzx_%1")
                                                   .arg(QDateTime::currentMSecsSinceEpoch());
                        }
                        if (!accepted) {
                            return;
                        }
                        emit orderPlaced(ctxPtr->accountName, sym, side, price, quantity, extractedOrderId);
                        emit logMessage(QStringLiteral("%1 UZX order accepted: %2 %3 @ %4")
                                            .arg(contextTag(ctxPtr->accountName))
                                            .arg(side == OrderSide::Buy ? QStringLiteral("BUY")
                                                                        : QStringLiteral("SELL"))
                                            .arg(quantity, 0, 'f', 4)
                                            .arg(price, 0, 'f', 5));
                    });
        };

        if (isSwap && leverage > 0) {
            const int desiredLev = std::max(1, leverage);
            const int lastLev = ctx.uzxLeverageBySymbol.value(sym, 0);
            if (desiredLev != lastLev) {
                QJsonObject levPayload;
                levPayload.insert(QStringLiteral("product_name"), wireSym);
                levPayload.insert(QStringLiteral("leverage"), desiredLev);
                levPayload.insert(QStringLiteral("pos_side"),
                                  side == OrderSide::Buy ? QStringLiteral("LG") : QStringLiteral("ST"));
                levPayload.insert(QStringLiteral("mgn_mode"), 1);
                const QByteArray levBody =
                    QJsonDocument(levPayload).toJson(QJsonDocument::Compact);
                emit logMessage(QStringLiteral("%1 UZX set leverage: %2")
                                    .arg(contextTag(ctx.accountName), QString::fromUtf8(levBody)));
                QNetworkRequest levReq =
                    makeUzxRequest(QStringLiteral("/v2/trade/swap/position/set-leverage"),
                                   levBody,
                                   QStringLiteral("PUT"),
                                   QUrlQuery(),
                                   ctx);
                auto *levReply = m_network.sendCustomRequest(levReq, QByteArrayLiteral("PUT"), levBody);
                connect(levReply, &QNetworkReply::finished, this, [this, levReply, sym, desiredLev, sendOrder, ctxPtr = &ctx]() {
                    const auto err = levReply->error();
                    const int status =
                        levReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QByteArray raw = levReply->readAll();
                    levReply->deleteLater();
                    if (err != QNetworkReply::NoError || status >= 400) {
                        QString msg = levReply->errorString();
                        if (status >= 400) {
                            msg = QStringLiteral("HTTP %1: %2")
                                      .arg(status)
                                      .arg(QString::fromUtf8(raw));
                        }
                        emit logMessage(QStringLiteral("%1 UZX leverage error: %2")
                                            .arg(contextTag(ctxPtr->accountName), msg));
                        emit logMessage(QStringLiteral("%1 UZX leverage failed, placing order with current leverage")
                                            .arg(contextTag(ctxPtr->accountName)));
                        sendOrder();
                        return;
                    }
                    if (!raw.isEmpty()) {
                        const QJsonDocument doc = QJsonDocument::fromJson(raw);
                        if (doc.isObject()) {
                            const QJsonObject obj = doc.object();
                            const int code = obj.value(QStringLiteral("code")).toInt(0);
                            if (code != 0 && code != 200) {
                                const QString rawMsg = obj.value(QStringLiteral("msg"))
                                                           .toString(QStringLiteral("request error"));
                                const QString msg = uzxFormatError(code, rawMsg);
                                const QString msgWithCode =
                                    QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                                emit logMessage(QStringLiteral("%1 UZX leverage rejected: %2")
                                                    .arg(contextTag(ctxPtr->accountName))
                                                    .arg(msgWithCode));
                                emit logMessage(QStringLiteral("%1 UZX leverage rejected, placing order with current leverage")
                                                    .arg(contextTag(ctxPtr->accountName)));
                                sendOrder();
                                return;
                            }
                        }
                    }
                    ctxPtr->uzxLeverageBySymbol.insert(sym, desiredLev);
                    emit logMessage(QStringLiteral("%1 UZX leverage set for %2: %3x")
                                        .arg(contextTag(ctxPtr->accountName), sym)
                                        .arg(desiredLev));
                    sendOrder();
                });
                return;
            }
        }
        sendOrder();
        return;
    }

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), sym);
    query.addQueryItem(QStringLiteral("side"),
                       side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("LIMIT"));
    query.addQueryItem(QStringLiteral("timeInForce"), QStringLiteral("GTC"));
    query.addQueryItem(QStringLiteral("price"), trimDecimalZeros(QString::number(price, 'f', 8)));
    query.addQueryItem(QStringLiteral("quantity"),
                       trimDecimalZeros(QString::number(quantity, 'f', 8)));
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    query.addQueryItem(QStringLiteral("timestamp"),
                       QString::number(QDateTime::currentMSecsSinceEpoch()));

    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"),
                             QString::fromLatin1(signPayload(query, ctx)));

    QNetworkRequest request = makePrivateRequest(QStringLiteral("/api/v3/order"),
                                                 signedQuery,
                                                 QByteArray(),
                                                 ctx);
    auto *reply = ensureMexcNetwork(ctx)->post(request, QByteArray());
    connect(reply,
            &QNetworkReply::finished,
            this,
            [this, reply, sym, side, price, quantity, ctxPtr = &ctx]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    QString msg = reply->errorString();
                    if (status >= 400) {
                        msg = QStringLiteral("HTTP %1: %2")
                                  .arg(status)
                                  .arg(QString::fromUtf8(raw));
                    }
                    emit orderFailed(ctxPtr->accountName, sym, msg);
                    emit logMessage(QStringLiteral("%1 Order error: %2")
                                        .arg(contextTag(ctxPtr->accountName), msg));
                    return;
                }
                emit logMessage(QStringLiteral("%1 MEXC order response: %2")
                                    .arg(contextTag(ctxPtr->accountName),
                                         raw.isEmpty() ? QStringLiteral("<empty>")
                                                       : QString::fromUtf8(raw)));
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    emit orderFailed(ctxPtr->accountName, sym, tr("Invalid response"));
                    return;
                }
                const QJsonObject obj = doc.object();
                if (obj.contains(QStringLiteral("code"))
                    && obj.value(QStringLiteral("code")).toInt(0) != 0) {
                    const QString msg = obj.value(QStringLiteral("msg"))
                                             .toString(QStringLiteral("Unknown error"));
                    emit orderFailed(ctxPtr->accountName, sym, msg);
                    emit logMessage(QStringLiteral("%1 Order rejected: %2")
                                        .arg(contextTag(ctxPtr->accountName), msg));
                    return;
                }
                QString orderId = obj.value(QStringLiteral("orderId")).toVariant().toString();
                if (orderId.isEmpty()) {
                    orderId = obj.value(QStringLiteral("clientOrderId")).toString();
                }
                if (orderId.isEmpty()) {
                    orderId = QStringLiteral("mexc_%1")
                                  .arg(QDateTime::currentMSecsSinceEpoch());
                }
                emit orderPlaced(ctxPtr->accountName, sym, side, price, quantity, orderId);
                emit logMessage(QStringLiteral("%1 Order accepted: %2 %3 @ %4")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(side == OrderSide::Buy ? QStringLiteral("BUY")
                                                                : QStringLiteral("SELL"))
                                    .arg(quantity, 0, 'f', 4)
                                    .arg(price, 0, 'f', 5));
            });
}

void TradeManager::closePositionMarket(const QString &symbol,
                                       const QString &accountName,
                                       double priceHint)
{
    const QString sym = normalizedSymbol(symbol);
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    Context &ctx = ensureContext(profile);
    if (!ensureCredentials(ctx)) {
        emit orderFailed(ctx.accountName, sym, tr("Missing credentials"));
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        emit orderFailed(ctx.accountName, sym, tr("Connect to the exchange first"));
        return;
    }

    const TradePosition pos = ctx.positions.value(sym, TradePosition{});

    if (profile == ConnectionStore::Profile::Lighter) {
        const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
        const QString account = ctx.accountName;
        const int apiKeyIndex = ctx.credentials.apiKeyIndex;
        const long long accountIndex = static_cast<long long>(ctx.credentials.accountIndex);
        Context *ctxPtrForNonce = &ctx;

        auto fail = [this, account, sym](const QString &msg) {
            emit orderFailed(account, sym, msg);
            emit logMessage(QStringLiteral("%1 Lighter close error: %2")
                                .arg(contextTag(account), msg));
        };

        if (baseUrl.isEmpty()) {
            fail(tr("Missing Lighter base URL"));
            return;
        }

        auto logLastInactiveOrder = [this, baseUrl, &ctx, account, sym](int marketId) {
            if (ctx.lighterAuthToken.isEmpty()) {
                emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup skipped (missing auth token)")
                                    .arg(contextTag(account)));
                return;
            }
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/accountInactiveOrders"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("account_index"), QString::number(ctx.credentials.accountIndex));
            q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
            if (marketId >= 0) {
                q.addQueryItem(QStringLiteral("market_id"), QString::number(marketId));
            }
            q.addQueryItem(QStringLiteral("auth"), ctx.lighterAuthToken);
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req.setRawHeader("authorization", ctx.lighterAuthToken.toUtf8());
            QNetworkReply *reply = ensureLighterNetwork(ctx)->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, account, sym]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup failed: %2")
                                        .arg(contextTag(account), lighterHttpErrorMessage(reply, raw)));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup: invalid response")
                                        .arg(contextTag(account)));
                    return;
                }
                const QJsonObject obj = doc.object();
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (code != 200) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup error: %2")
                                        .arg(contextTag(account),
                                             obj.value(QStringLiteral("message"))
                                                 .toString(QStringLiteral("Unknown error"))));
                    return;
                }
                const QJsonArray orders = obj.value(QStringLiteral("orders")).toArray();
                if (orders.isEmpty()) {
                    emit logMessage(QStringLiteral("%1 Lighter inactive-order lookup: no orders found for %2")
                                        .arg(contextTag(account), sym));
                    return;
                }
                const QJsonObject o = orders.first().toObject();
                const QString type = o.value(QStringLiteral("type")).toString();
                const QString tif = o.value(QStringLiteral("time_in_force")).toString();
                const QString reduceOnly = o.value(QStringLiteral("reduce_only")).toVariant().toString();
                const QString price = o.value(QStringLiteral("price")).toVariant().toString();
                const QString baseSize = o.value(QStringLiteral("base_size")).toVariant().toString();
                const QString baseAmount = o.value(QStringLiteral("initial_base_amount")).toVariant().toString();
                const QString marketIndex = o.value(QStringLiteral("market_index")).toVariant().toString();
                const QString isAsk = o.value(QStringLiteral("is_ask")).toVariant().toString();
                const QString statusStr = o.value(QStringLiteral("status")).toVariant().toString();
                emit logMessage(QStringLiteral("%1 Lighter last inactive order: market=%2 type=%3 tif=%4 reduceOnly=%5 price=%6 base_size=%7 base_amount=%8 is_ask=%9 status=%10")
                                    .arg(contextTag(account),
                                         marketIndex,
                                         type,
                                         tif,
                                         reduceOnly,
                                         price,
                                         baseSize,
                                         baseAmount,
                                         isAsk,
                                         statusStr));
            });
        };

        if (!pos.hasPosition || !(pos.quantity > 0.0)) {
            logLastInactiveOrder(-1);
            emit orderFailed(ctx.accountName, sym, tr("No active position"));
            return;
        }

        auto ensureMetaLoaded = [this, baseUrl, ctxPtr = &ctx](std::function<void(QString err)> cb) {
            auto &cache = lighterMetaCacheByUrl()[baseUrl];
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const bool freshEnough = (cache.updatedMs > 0 && (now - cache.updatedMs) < 5 * 60 * 1000);
            if (!cache.bySymbol.isEmpty() && freshEnough) {
                cb(QString());
                return;
            }
            cache.waiters.push_back(cb);
            if (cache.inFlight) {
                return;
            }
            cache.inFlight = true;

            QNetworkRequest req(lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookDetails")));
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
            connect(reply, &QNetworkReply::finished, this, [reply, baseUrl]() {
                auto &cache2 = lighterMetaCacheByUrl()[baseUrl];
                cache2.inFlight = false;
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();

                QString outErr;
                if (err != QNetworkReply::NoError || status >= 400) {
                    outErr = QStringLiteral("Failed to load Lighter markets: %1")
                                 .arg(lighterHttpErrorMessage(reply, raw));
                } else {
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isNull() || !doc.isObject()) {
                        outErr = QStringLiteral("Failed to parse Lighter markets");
                    } else {
                        const QJsonObject obj = doc.object();
                        const int code = obj.value(QStringLiteral("code")).toInt(0);
                        if (code != 200) {
                            outErr = obj.value(QStringLiteral("message"))
                                         .toString(QStringLiteral("Lighter markets error"));
                        } else {
                            cache2.bySymbol.clear();
                            addLighterMarketMeta(cache2.bySymbol,
                                                 obj.value(QStringLiteral("order_book_details")).toArray(),
                                                 false);
                            addLighterMarketMeta(cache2.bySymbol,
                                                 obj.value(QStringLiteral("spot_order_book_details")).toArray(),
                                                 true);
                            cache2.updatedMs = QDateTime::currentMSecsSinceEpoch();
                        }
                    }
                }

                const auto waiters = std::move(cache2.waiters);
                cache2.waiters.clear();
                for (const auto &w : waiters) {
                    w(outErr);
                }
            });
        };

        auto fetchNonce = [this, baseUrl, apiKeyIndex, accountIndex, ctxPtr = &ctx](std::function<void(qint64 nonce, QString err)> cb) {
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/nextNonce"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("account_index"), QString::number(accountIndex));
            q.addQueryItem(QStringLiteral("api_key_index"), QString::number(apiKeyIndex));
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
            applyReplyTimeout(reply, 4500);
            connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    cb(0, lighterHttpErrorMessage(reply, raw));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    cb(0, QStringLiteral("Invalid Lighter nonce response"));
                    return;
                }
                const QJsonObject obj = doc.object();
                if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                    cb(0, obj.value(QStringLiteral("message")).toString(QStringLiteral("Failed to get nonce")));
                    return;
                }
                cb(static_cast<qint64>(obj.value(QStringLiteral("nonce")).toVariant().toLongLong()), QString());
            });
        };

        auto ensureNonce = [fetchNonce, ctxPtrForNonce, fail](std::function<void(qint64 nonce)> cb) {
            if (!ctxPtrForNonce) {
                fail(QStringLiteral("Internal error"));
                return;
            }
            if (ctxPtrForNonce->lighterNextNonce > 0 && !ctxPtrForNonce->lighterNonceInFlight) {
                const qint64 nonce = ctxPtrForNonce->lighterNextNonce;
                ctxPtrForNonce->lighterNextNonce = nonce + 1;
                cb(nonce);
                return;
            }
            if (ctxPtrForNonce->lighterNonceInFlight) {
                ctxPtrForNonce->lighterNonceWaiters.push_back(
                    [cb](qint64 nonce, const QString &err) {
                        if (!err.isEmpty()) {
                            return;
                        }
                        cb(nonce);
                    });
                return;
            }
            ctxPtrForNonce->lighterNonceInFlight = true;
            fetchNonce([ctxPtrForNonce, cb](qint64 nonce, const QString &err) {
                if (!ctxPtrForNonce) {
                    return;
                }
                ctxPtrForNonce->lighterNonceInFlight = false;
                const auto waiters = std::move(ctxPtrForNonce->lighterNonceWaiters);
                ctxPtrForNonce->lighterNonceWaiters.clear();
                if (err.isEmpty() && nonce > 0) {
                    ctxPtrForNonce->lighterNextNonce = nonce;
                } else {
                    ctxPtrForNonce->lighterNextNonce = 0;
                }
                for (const auto &w : waiters) {
                    if (!err.isEmpty() || nonce <= 0) {
                        w(0, err);
                        continue;
                    }
                    const qint64 reserved = ctxPtrForNonce->lighterNextNonce;
                    ctxPtrForNonce->lighterNextNonce = reserved + 1;
                    w(reserved, QString());
                }
                if (!err.isEmpty()) {
                    return;
                }
                if (nonce > 0) {
                    const qint64 reserved = ctxPtrForNonce->lighterNextNonce;
                    ctxPtrForNonce->lighterNextNonce = reserved + 1;
                    cb(reserved);
                }
            });
        };

        auto sendTx = [this, ctxPtrForNonce](int txType,
                                             const QString &txInfo,
                                             std::function<void(QString txHash, QString err)> cb) {
            if (!ctxPtrForNonce) {
                cb(QString(), QStringLiteral("Internal error"));
                return;
            }
            sendLighterTx(*ctxPtrForNonce, txType, txInfo, std::move(cb));
        };

        auto fetchTopBookPriceClose = [this, baseUrl, ctxPtr = &ctx](int marketId,
                                                      bool isAsk,
                                                      int priceDecimals,
                                                      std::function<void(double price, qint64 priceInt, QString err)> cb) {
            auto parseErr = [](const QByteArray &raw, int *outCode, QString *outMsg) {
                if (outCode) {
                    *outCode = 0;
                }
                if (outMsg) {
                    outMsg->clear();
                }
                if (raw.isEmpty()) {
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (!doc.isObject()) {
                    return;
                }
                const QJsonObject obj = doc.object();
                if (outCode) {
                    *outCode = obj.value(QStringLiteral("code")).toInt(0);
                }
                if (outMsg) {
                    *outMsg = obj.value(QStringLiteral("message")).toString();
                }
            };

            std::function<void(const QString &paramName, bool allowRetry)> doRequest;
            doRequest = [this, baseUrl, marketId, isAsk, priceDecimals, cb, parseErr, &doRequest, ctxPtr](const QString &paramName, bool allowRetry) {
                QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookOrders"));
                QUrlQuery q;
                q.addQueryItem(paramName, QString::number(marketId));
                q.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
                url.setQuery(q);
                QNetworkRequest req(url);
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
                connect(reply, &QNetworkReply::finished, this, [reply, isAsk, priceDecimals, cb, paramName, allowRetry, parseErr, &doRequest]() {
                    const QNetworkReply::NetworkError err = reply->error();
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QByteArray raw = reply->readAll();
                    reply->deleteLater();
                    int code = 0;
                    QString msg;
                    parseErr(raw, &code, &msg);
                    if (err != QNetworkReply::NoError || status >= 400) {
                        if (allowRetry && code == 20001
                            && msg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                            const QString otherParam =
                                (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                            : QStringLiteral("market_id");
                            doRequest(otherParam, false);
                            return;
                        }
                        const QString errMsg = msg.isEmpty() ? lighterHttpErrorMessage(reply, raw) : msg;
                        cb(0.0, 0, errMsg);
                        return;
                    }
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isNull() || !doc.isObject()) {
                        cb(0.0, 0, QStringLiteral("Invalid Lighter order book response"));
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    const int objCode = obj.value(QStringLiteral("code")).toInt(0);
                    if (objCode != 200) {
                        const QString objMsg = obj.value(QStringLiteral("message"))
                                                   .toString(QStringLiteral("Lighter order book error"));
                        if (allowRetry && objCode == 20001
                            && objMsg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                            const QString otherParam =
                                (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                            : QStringLiteral("market_id");
                            doRequest(otherParam, false);
                            return;
                        }
                        cb(0.0, 0, objMsg);
                        return;
                    }
                    QJsonObject bookObj = obj;
                    if (obj.value(QStringLiteral("order_book_orders")).isObject()) {
                        bookObj = obj.value(QStringLiteral("order_book_orders")).toObject();
                    }
                    const QJsonArray bids = bookObj.value(QStringLiteral("bids")).toArray();
                    const QJsonArray asks = bookObj.value(QStringLiteral("asks")).toArray();
                    const QJsonArray side = isAsk ? bids : asks;
                    if (side.isEmpty()) {
                        cb(0.0, 0, QStringLiteral("Empty Lighter order book"));
                        return;
                    }
                    const QJsonObject top = side.first().toObject();
                    const QJsonValue priceVal = top.value(QStringLiteral("price"));
                    bool ok = false;
                    double price = 0.0;
                    qint64 priceInt = 0;
                    if (priceVal.isString()) {
                        const QString priceStr = priceVal.toString();
                        price = priceStr.toDouble(&ok);
                        if (ok) {
                            QString digits = priceStr;
                            digits.remove(QLatin1Char('.'));
                            priceInt = digits.toLongLong();
                            if (priceDecimals > 0) {
                                const int expectedDigits = priceDecimals;
                                const int dotPos = priceStr.indexOf(QLatin1Char('.'));
                                if (dotPos >= 0) {
                                    const int fracLen = priceStr.length() - dotPos - 1;
                                    if (fracLen < expectedDigits) {
                                        priceInt *= static_cast<qint64>(pow10i(expectedDigits - fracLen));
                                    }
                                }
                            }
                        }
                    } else if (priceVal.isDouble()) {
                        price = priceVal.toDouble();
                        ok = price > 0.0;
                    }
                    if (!ok || !(price > 0.0)) {
                        cb(0.0, 0, QStringLiteral("Invalid Lighter order book price"));
                        return;
                    }
                    cb(price, priceInt, QString());
                });
            };

            doRequest(QStringLiteral("market_id"), true);
        };

        ensureMetaLoaded([this, sym, pos, priceHint, baseUrl, account, apiKeyIndex, accountIndex, ctxPtrForNonce, fail, sendTx, ensureNonce, fetchTopBookPriceClose, logLastInactiveOrder](const QString &err) {
            if (!err.isEmpty()) {
                fail(err);
                return;
            }
            auto &cache = lighterMetaCacheByUrl()[baseUrl];
            const LighterMarketMeta meta = cache.bySymbol.value(sym, LighterMarketMeta{});
            if (meta.marketId < 0) {
                fail(QStringLiteral("Unknown Lighter symbol: %1").arg(sym));
                return;
            }

            const double quantity = std::abs(pos.quantity);
            if (!(quantity > 0.0)) {
                fail(QStringLiteral("No active position"));
                return;
            }
                const int isAsk = (pos.side == OrderSide::Buy) ? 1 : 0;
            auto finalizeClose = [this, sym, pos, meta, quantity, apiKeyIndex, accountIndex, account, fail, sendTx, ensureNonce, ctxPtrForNonce, isAsk, logLastInactiveOrder](double price, qint64 priceIntOverride) {
                const qint64 baseScale = pow10i(meta.sizeDecimals);
                const qint64 priceScale = pow10i(meta.priceDecimals);
                const qint64 baseAmount = std::max<qint64>(1, qRound64(quantity * static_cast<double>(baseScale)));
                qint64 idealPriceInt64 = priceIntOverride;
                if (idealPriceInt64 <= 0 && price > 0.0) {
                    const double rawPrice = price * static_cast<double>(priceScale);
                    idealPriceInt64 = static_cast<qint64>(std::llround(rawPrice));
                }
                // Market order uses avg_execution_price (worst acceptable price). Apply a slippage buffer.
                qint64 acceptablePriceInt64 = idealPriceInt64;
                if (acceptablePriceInt64 > 0) {
                    if (isAsk == 1) {
                        acceptablePriceInt64 =
                            std::max<qint64>(1, static_cast<qint64>(std::floor(static_cast<double>(acceptablePriceInt64) * (1.0 - kLighterMarketMaxSlippage))));
                    } else {
                        acceptablePriceInt64 =
                            std::max<qint64>(1, static_cast<qint64>(std::ceil(static_cast<double>(acceptablePriceInt64) * (1.0 + kLighterMarketMaxSlippage))));
                    }
                }

                if (acceptablePriceInt64 <= 0 || acceptablePriceInt64 > std::numeric_limits<int>::max()) {
                    fail(QStringLiteral("Invalid scaled price for %1").arg(sym));
                    return;
                }
                 const double acceptablePrice = static_cast<double>(acceptablePriceInt64) / static_cast<double>(priceScale);
                 const double idealPrice = static_cast<double>(idealPriceInt64) / static_cast<double>(priceScale);

                emit logMessage(QStringLiteral("%1 Lighter close params (market/IOC): sym=%2 marketId=%3 isAsk=%4 qty=%5 baseAmount=%6 idealPrice=%7 idealPriceInt=%8 acceptablePrice=%9 acceptablePriceInt=%10 reduceOnly=1")
                                    .arg(contextTag(account))
                                    .arg(sym)
                                    .arg(meta.marketId)
                                    .arg(isAsk)
                                    .arg(quantity, 0, 'f', 8)
                                    .arg(baseAmount)
                                    .arg(idealPrice, 0, 'f', 8)
                                    .arg(idealPriceInt64)
                                    .arg(acceptablePrice, 0, 'f', 8)
                                    .arg(acceptablePriceInt64));

                ensureNonce([this, sym, meta, baseAmount, acceptablePriceInt64, pos, apiKeyIndex, accountIndex, account, fail, sendTx, ctxPtrForNonce, isAsk, logLastInactiveOrder](qint64 nonce) {
                    if (!ctxPtrForNonce || nonce <= 0) {
                        fail(QStringLiteral("Failed to get nonce"));
                        return;
                    }

                    const qint64 clientOrderIndex = 0;
                    const int orderType = 1; // market
                    const int timeInForce = 0; // IOC
                    const int reduceOnly = 1;
                    const int triggerPrice = 0;
                    const qint64 orderExpiry = 0; // IOC expiry

                    const LighterSignedTxResponse signedTx =
                        lighterSigner().signCreateOrder(meta.marketId,
                                                        clientOrderIndex,
                                                        static_cast<long long>(baseAmount),
                                                        static_cast<int>(acceptablePriceInt64),
                                                        isAsk,
                                                        orderType,
                                                        timeInForce,
                                                        reduceOnly,
                                                        triggerPrice,
                                                        static_cast<long long>(orderExpiry),
                                                        static_cast<long long>(nonce),
                                                        apiKeyIndex,
                                                        accountIndex);
                    if (signedTx.err) {
                        fail(QString::fromUtf8(signedTx.err));
                        return;
                    }
                    const QString txInfo = signedTx.txInfo ? QString::fromUtf8(signedTx.txInfo) : QString();
                    if (txInfo.isEmpty()) {
                        fail(QStringLiteral("Empty signed txInfo"));
                        return;
                    }

                    sendTx(static_cast<int>(signedTx.txType), txInfo, [this, sym, account, fail, ctxPtrForNonce, nonce, logLastInactiveOrder, meta](const QString &txHash, const QString &txErr) {
                        if (!txErr.isEmpty()) {
                            if (ctxPtrForNonce) {
                                ctxPtrForNonce->lighterNextNonce = 0;
                            }
                            if (txErr.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                                logLastInactiveOrder(meta.marketId);
                            }
                            fail(txErr);
                            return;
                        }
                        if (ctxPtrForNonce) {
                            ctxPtrForNonce->lighterNextNonce = nonce + 1;
                        }
                        emit logMessage(QStringLiteral("%1 Lighter close sent: %2 (tx=%3)")
                                            .arg(contextTag(account))
                                            .arg(sym)
                                            .arg(txHash.left(12)));
                    });
                });
            };

            fetchTopBookPriceClose(meta.marketId, isAsk == 1, meta.priceDecimals, [fail, finalizeClose, meta](double bookPrice, qint64 bookPriceInt, const QString &bookErr) {
                if (!bookErr.isEmpty()) {
                    if (meta.lastTradePrice > 0.0) {
                        finalizeClose(meta.lastTradePrice, 0);
                        return;
                    }
                    fail(bookErr);
                    return;
                }
                if (!(bookPrice > 0.0)) {
                    if (meta.lastTradePrice > 0.0) {
                        finalizeClose(meta.lastTradePrice, 0);
                        return;
                    }
                    fail(QStringLiteral("Missing price for market close"));
                    return;
                }
                finalizeClose(bookPrice, bookPriceInt);
            });
        });
        return;
    }

    if (profile == ConnectionStore::Profile::MexcFutures) {
        // Close position via a market order (MEXC futures requires a non-zero price even for market orders).
        double vol = pos.quantity;
        const auto metaIt = ctx.futuresContractMeta.constFind(sym);
        if (metaIt != ctx.futuresContractMeta.constEnd() && metaIt->valid()) {
            const int volScale = std::max(0, metaIt->volScale);
            const double factor = std::pow(10.0, static_cast<double>(volScale));
            vol = std::floor(vol * factor) / factor;
            if (vol + 1e-12 < static_cast<double>(metaIt->minVol)) {
                emit orderFailed(ctx.accountName,
                                 sym,
                                 tr("Close size too small for futures contract (minVol=%1)")
                                     .arg(metaIt->minVol));
                return;
            }
        } else {
            ensureFuturesContractMeta(ctx, sym);
        }
        double price = priceHint;
        if (!(price > 0.0)) {
            price = pos.averagePrice;
        }
        if (!(price > 0.0)) {
            emit orderFailed(ctx.accountName, sym, tr("Missing price for futures market close"));
            return;
        }
        QJsonObject payload;
        payload.insert(QStringLiteral("symbol"), sym);
        payload.insert(QStringLiteral("price"), price);
        payload.insert(QStringLiteral("vol"), vol);
        // MEXC futures: 4 = close long, 2 = close short
        payload.insert(QStringLiteral("side"), pos.side == OrderSide::Buy ? 4 : 2);
        payload.insert(QStringLiteral("type"), 5); // market
        const int openType = (pos.openType == 2) ? 2 : 1;
        payload.insert(QStringLiteral("openType"), openType);
        if (pos.positionId > 0) {
            payload.insert(QStringLiteral("positionId"), pos.positionId);
        }

        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        emit logMessage(QStringLiteral("%1 Closing futures position (market): %2 payload=%3")
                            .arg(contextTag(ctx.accountName), sym, QString::fromUtf8(body)));
        QNetworkRequest request =
            makeFuturesRequest(QStringLiteral("/private/order/submit"), ctx, body, true);
        auto *reply = ensureMexcNetwork(ctx)->post(request, body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, sym, ctxPtr = &ctx]() {
            const auto networkError = reply->error();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (networkError != QNetworkReply::NoError) {
                const QString message =
                    raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw);
                emit orderFailed(ctxPtr->accountName, sym, message);
                emit logMessage(QStringLiteral("%1 Futures close failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), message));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            const QJsonObject obj = doc.object();
            const bool success = obj.value(QStringLiteral("success")).toBool(true);
            const int code = obj.value(QStringLiteral("code")).toInt(0);
            if (!success || code != 0) {
                const QString errMsg =
                    obj.value(QStringLiteral("message"))
                        .toString(obj.value(QStringLiteral("msg")).toString(QStringLiteral("request rejected")));
                emit orderFailed(ctxPtr->accountName, sym, errMsg);
                emit logMessage(QStringLiteral("%1 Futures close rejected: %2 (code %3)")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(errMsg)
                                    .arg(code));
                return;
            }
            emit logMessage(QStringLiteral("%1 Futures close sent: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     QString::fromUtf8(raw)));
            fetchFuturesPositions(*ctxPtr);
        });
        return;
    }

    if (profile == ConnectionStore::Profile::UzxSwap) {
        if (!pos.hasPosition || !(pos.quantity > 0.0)) {
            emit orderFailed(ctx.accountName, sym, tr("No active position"));
            return;
        }
        const QString wireSym = uzxWireSymbol(sym, true);
        QJsonObject payload;
        payload.insert(QStringLiteral("product_name"), wireSym);
        payload.insert(QStringLiteral("pos_side"),
                       pos.side == OrderSide::Buy ? QStringLiteral("LG") : QStringLiteral("ST"));
        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        emit logMessage(QStringLiteral("%1 UZX close position request: %2")
                            .arg(contextTag(ctx.accountName), QString::fromUtf8(body)));
        QNetworkRequest req =
            makeUzxRequest(QStringLiteral("/v2/trade/swap/close-position"),
                           body,
                           QStringLiteral("PUT"),
                           QUrlQuery(),
                           ctx);
        auto *reply = m_network.sendCustomRequest(req, QByteArrayLiteral("PUT"), body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, sym, ctxPtr = &ctx]() {
            const auto err = reply->error();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                QString msg = reply->errorString();
                if (status >= 400) {
                    msg = QStringLiteral("HTTP %1: %2")
                              .arg(status)
                              .arg(QString::fromUtf8(raw));
                }
                emit orderFailed(ctxPtr->accountName, sym, msg);
                emit logMessage(QStringLiteral("%1 UZX close error: %2")
                                    .arg(contextTag(ctxPtr->accountName), msg));
                return;
            }
            const QString resp = QString::fromUtf8(raw);
            emit logMessage(QStringLiteral("%1 UZX close response: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     resp.isEmpty() ? QStringLiteral("<empty>") : resp));
            QJsonDocument uzxDoc = QJsonDocument::fromJson(raw);
            if (uzxDoc.isObject()) {
                const QJsonObject obj = uzxDoc.object();
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (code != 0 && code != 200) {
                    const QString rawMsg = obj.value(QStringLiteral("msg"))
                                               .toString(QStringLiteral("request error"));
                    const QString msg = uzxFormatError(code, rawMsg);
                    const QString msgWithCode =
                        QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                    emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                    emit logMessage(QStringLiteral("%1 UZX close rejected: %2")
                                        .arg(contextTag(ctxPtr->accountName))
                                        .arg(msgWithCode));
                    return;
                }
            }
        });
        return;
    }
    if (profile == ConnectionStore::Profile::UzxSpot) {
        if (!pos.hasPosition || !(pos.quantity > 0.0)) {
            emit orderFailed(ctx.accountName, sym, tr("No active position"));
            return;
        }
        const OrderSide closeSide = (pos.side == OrderSide::Buy) ? OrderSide::Sell : OrderSide::Buy;
        const double safePrice =
            (priceHint > 0.0) ? priceHint
                              : (pos.averagePrice > 0.0 ? pos.averagePrice : 0.00000001);
        double closeQty = pos.quantity;
        double lockedQty = 0.0;
        const QString baseAsset = uzxBaseAsset(sym);
        if (!baseAsset.isEmpty()) {
            const auto it = ctx.balances.constFind(baseAsset);
            if (it != ctx.balances.constEnd()) {
                lockedQty = it->locked;
                if (it->available > 0.0) {
                    closeQty = std::min(closeQty, it->available);
                }
            }
        }
        auto scheduleUzxSpotCloseRetry = [this, &ctx, sym, baseAsset](const QString &reason) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 lastMs = ctx.uzxSpotCloseRetryMs.value(sym, 0);
            if (lastMs > 0 && nowMs - lastMs < 1500) {
                return;
            }
            ctx.uzxSpotCloseRetryMs.insert(sym, nowMs);
            emit logMessage(QStringLiteral("%1 UZX market close retry: %2")
                                .arg(contextTag(ctx.accountName), reason));
            cancelAllOrders(sym, ctx.accountName);
            QTimer::singleShot(500, this, [this, sym, account = ctx.accountName]() {
                closePositionMarket(sym, account);
            });
        };
        if (!(closeQty > 0.0)) {
            if (lockedQty > 0.0) {
                scheduleUzxSpotCloseRetry(QStringLiteral("balance locked by open orders"));
                return;
            }
            emit orderFailed(ctx.accountName, sym, tr("No available balance to close position"));
            return;
        }
        QJsonObject payload;
        payload.insert(QStringLiteral("product_name"), uzxWireSymbol(sym, false));
        payload.insert(QStringLiteral("order_type"), 1);
        payload.insert(QStringLiteral("price"), QString::number(safePrice, 'f', 8));
        payload.insert(QStringLiteral("amount"), QString::number(closeQty, 'f', 8));
        payload.insert(QStringLiteral("order_buy_or_sell"), closeSide == OrderSide::Buy ? 1 : 2);
        payload.insert(QStringLiteral("trade_ccy"), 1);
        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        emit logMessage(QStringLiteral("%1 UZX market close body: %2")
                            .arg(contextTag(ctx.accountName), QString::fromUtf8(body)));
        QNetworkRequest req =
            makeUzxRequest(QStringLiteral("/v2/trade/spot/order"),
                           body,
                           QStringLiteral("POST"),
                           QUrlQuery(),
                           ctx);
        auto *reply = m_network.post(req, body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, sym, closeSide, ctxPtr = &ctx]() {
            const auto err = reply->error();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                QString msg = reply->errorString();
                if (status >= 400) {
                    msg = QStringLiteral("HTTP %1: %2")
                              .arg(status)
                              .arg(QString::fromUtf8(raw));
                }
                emit orderFailed(ctxPtr->accountName, sym, msg);
                emit logMessage(QStringLiteral("%1 UZX market close error: %2")
                                    .arg(contextTag(ctxPtr->accountName), msg));
                return;
            }
            const QString resp = QString::fromUtf8(raw);
            emit logMessage(QStringLiteral("%1 UZX market close response: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     resp.isEmpty() ? QStringLiteral("<empty>") : resp));
            QJsonDocument uzxDoc = QJsonDocument::fromJson(raw);
            if (uzxDoc.isObject()) {
                const QJsonObject obj = uzxDoc.object();
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (code != 0 && code != 200) {
                    const QString rawMsg = obj.value(QStringLiteral("msg"))
                                               .toString(QStringLiteral("request error"));
                    const QString msg = uzxFormatError(code, rawMsg);
                    const QString msgWithCode =
                        QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                    if (code == 1010004) {
                        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                        const qint64 lastMs = ctxPtr->uzxSpotCloseRetryMs.value(sym, 0);
                        if (lastMs == 0 || nowMs - lastMs >= 1500) {
                            ctxPtr->uzxSpotCloseRetryMs.insert(sym, nowMs);
                            emit logMessage(QStringLiteral("%1 UZX market close retry: %2")
                                                .arg(contextTag(ctxPtr->accountName), msgWithCode));
                            cancelAllOrders(sym, ctxPtr->accountName);
                            QTimer::singleShot(500, this, [this, sym, account = ctxPtr->accountName]() {
                                closePositionMarket(sym, account);
                            });
                            return;
                        }
                    }
                    emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                    emit logMessage(QStringLiteral("%1 UZX market close rejected: %2")
                                        .arg(contextTag(ctxPtr->accountName))
                                        .arg(msgWithCode));
                    return;
                }
            }
            TradePosition &pos = ctxPtr->positions[sym];
            if (pos.hasPosition) {
                pos.hasPosition = false;
                pos.quantity = 0.0;
                pos.averagePrice = 0.0;
                pos.side = closeSide;
                emitPositionChanged(*ctxPtr, sym);
            }
        });
        return;
    }

    // MEXC Spot: send a MARKET order in the opposite direction.
    if (!pos.hasPosition || !(pos.quantity > 0.0)) {
        emit orderFailed(ctx.accountName, sym, tr("No active position"));
        return;
    }
    const OrderSide closeSide = (pos.side == OrderSide::Buy) ? OrderSide::Sell : OrderSide::Buy;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), sym);
    query.addQueryItem(QStringLiteral("side"),
                       closeSide == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("MARKET"));
    query.addQueryItem(QStringLiteral("quantity"),
                       trimDecimalZeros(QString::number(pos.quantity, 'f', 8)));
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    query.addQueryItem(QStringLiteral("timestamp"),
                       QString::number(QDateTime::currentMSecsSinceEpoch()));

    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"),
                             QString::fromLatin1(signPayload(query, ctx)));
    QNetworkRequest request = makePrivateRequest(QStringLiteral("/api/v3/order"),
                                                 signedQuery,
                                                 QByteArray(),
                                                 ctx);
    auto *reply = ensureMexcNetwork(ctx)->post(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply, sym, closeSide, qty = pos.quantity, ctxPtr = &ctx]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            QString msg = reply->errorString();
            if (status >= 400) {
                msg = QStringLiteral("HTTP %1: %2")
                          .arg(status)
                          .arg(QString::fromUtf8(raw));
            }
            emit orderFailed(ctxPtr->accountName, sym, msg);
            emit logMessage(QStringLiteral("%1 Market close error: %2")
                                .arg(contextTag(ctxPtr->accountName), msg));
            return;
        }
        emit logMessage(QStringLiteral("%1 Market close sent for %2 qty=%3: %4")
                            .arg(contextTag(ctxPtr->accountName))
                            .arg(sym)
                            .arg(qty, 0, 'f', 8)
                            .arg(raw.isEmpty() ? QStringLiteral("<empty>")
                                               : QString::fromUtf8(raw)));
        Q_UNUSED(closeSide);
    });
}

void TradeManager::placeLighterStopOrder(const QString &symbol,
                                         const QString &accountName,
                                         double triggerPrice,
                                         bool isStopLoss)
{
    const QString sym = normalizedSymbol(symbol);
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    Context &ctx = ensureContext(profile);
    if (!ensureCredentials(ctx)) {
        emit orderFailed(ctx.accountName, sym, tr("Missing credentials"));
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        emit orderFailed(ctx.accountName, sym, tr("Connect to the exchange first"));
        return;
    }
    if (!(triggerPrice > 0.0) || !std::isfinite(triggerPrice)) {
        emit orderFailed(ctx.accountName, sym, tr("Invalid trigger price"));
        return;
    }

    if (profile == ConnectionStore::Profile::UzxSwap
        || profile == ConnectionStore::Profile::UzxSpot) {
        const TradePosition pos = ctx.positions.value(sym, TradePosition{});
        if (!pos.hasPosition || !(pos.quantity > 0.0)) {
            emit orderFailed(ctx.accountName, sym, tr("No active position for SL/TP"));
            return;
        }

        const bool isSwap = (profile == ConnectionStore::Profile::UzxSwap);
        const QString wireSym = uzxWireSymbol(sym, isSwap);
        const OrderSide closeSide = (pos.side == OrderSide::Buy) ? OrderSide::Sell : OrderSide::Buy;
        if (isStopLoss) {
            ctx.uzxSlTriggerBySymbol.insert(sym, triggerPrice);
        } else {
            ctx.uzxTpTriggerBySymbol.insert(sym, triggerPrice);
        }
        const double slTrigger = ctx.uzxSlTriggerBySymbol.value(sym, 0.0);
        const double tpTrigger = ctx.uzxTpTriggerBySymbol.value(sym, 0.0);
        const QString existingStopId = isStopLoss
                                           ? ctx.uzxSlOrderIdBySymbol.value(sym)
                                           : ctx.uzxTpOrderIdBySymbol.value(sym);
        if (!existingStopId.isEmpty()) {
            emit logMessage(QStringLiteral("%1 UZX SL/TP replacing existing %2 id=%3")
                                .arg(contextTag(ctx.accountName))
                                .arg(isStopLoss ? QStringLiteral("SL") : QStringLiteral("TP"))
                                .arg(existingStopId));
            cancelOrder(sym, ctx.accountName, existingStopId);
        }
        double posQty = pos.quantity;
        if (!isSwap) {
            const QString baseAsset = uzxBaseAsset(sym);
            if (!baseAsset.isEmpty()) {
                const auto it = ctx.balances.constFind(baseAsset);
                if (it != ctx.balances.constEnd() && it->available > 0.0) {
                    posQty = std::min(posQty, it->available);
                }
            }
        }
        if (!(posQty > 0.0)) {
            emit orderFailed(ctx.accountName, sym, tr("No available balance for SL/TP"));
            return;
        }
        const QString priceStr = QString::number(triggerPrice, 'f', 8);
        const QString amountStr = QString::number(posQty, 'f', 8);
        const QString slTriggerStr =
            slTrigger > 0.0 ? QString::number(slTrigger, 'f', 8) : QStringLiteral("0");
        const QString tpTriggerStr =
            tpTrigger > 0.0 ? QString::number(tpTrigger, 'f', 8) : QStringLiteral("0");
        if (slTrigger <= 0.0 || tpTrigger <= 0.0) {
            emit orderFailed(ctx.accountName, sym,
                             tr("UZX requires both SL and TP to be set together"));
            emit logMessage(QStringLiteral("%1 UZX SL/TP skipped: both triggers required (sl=%2 tp=%3)")
                                .arg(contextTag(ctx.accountName))
                                .arg(slTrigger, 0, 'f', 8)
                                .arg(tpTrigger, 0, 'f', 8));
            return;
        }

        QJsonObject algo;
        algo.insert(QStringLiteral("tp_trigger_price"), tpTriggerStr);
        algo.insert(QStringLiteral("tp_ord_price"), QStringLiteral("-1"));
        algo.insert(QStringLiteral("tp_ord_kind"), 1);
        algo.insert(QStringLiteral("sl_trigger_price"), slTriggerStr);
        algo.insert(QStringLiteral("sl_ord_price"), QStringLiteral("-1"));
        if (isSwap) {
            algo.insert(QStringLiteral("tp_compare"), 3);
            algo.insert(QStringLiteral("sl_compare"), 3);
        }

        QJsonArray attachAlgo;
        attachAlgo.append(algo);

        QJsonObject payload;
        payload.insert(QStringLiteral("product_name"), wireSym);
        payload.insert(QStringLiteral("order_type"), 2);
        payload.insert(QStringLiteral("price"), priceStr);
        payload.insert(QStringLiteral("amount"), amountStr);
        payload.insert(QStringLiteral("order_buy_or_sell"), closeSide == OrderSide::Buy ? 1 : 2);
        payload.insert(QStringLiteral("attach_algo"), attachAlgo);
        if (isSwap) {
            payload.insert(QStringLiteral("number"), amountStr);
            payload.insert(QStringLiteral("trade_ccy"), 1);
            payload.insert(QStringLiteral("pos_side"),
                           pos.side == OrderSide::Buy ? QStringLiteral("LG")
                                                      : QStringLiteral("ST"));
        } else {
            payload.insert(QStringLiteral("trade_ccy"), 1);
        }

        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        emit logMessage(QStringLiteral("%1 UZX SL/TP algo body: %2")
                            .arg(contextTag(ctx.accountName), QString::fromUtf8(body)));
        const QString path = isSwap ? QStringLiteral("/v2/trade/swap/order")
                                    : QStringLiteral("/v2/trade/spot/order");
        QNetworkRequest req = makeUzxRequest(path, body, QStringLiteral("POST"), QUrlQuery(), ctx);
        auto *reply = m_network.post(req, body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, sym, closeSide, triggerPrice, posQty, slTrigger, tpTrigger, ctxPtr = &ctx]() {
            const auto err = reply->error();
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                QString msg = reply->errorString();
                if (status >= 400) {
                    msg = QStringLiteral("HTTP %1: %2")
                              .arg(status)
                              .arg(QString::fromUtf8(raw));
                }
                emit orderFailed(ctxPtr->accountName, sym, msg);
                emit logMessage(QStringLiteral("%1 UZX SL/TP error: %2")
                                    .arg(contextTag(ctxPtr->accountName), msg));
                return;
            }
            const QString resp = QString::fromUtf8(raw);
            emit logMessage(QStringLiteral("%1 UZX SL/TP response: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     resp.isEmpty() ? QStringLiteral("<empty>") : resp));
            bool accepted = true;
            QJsonDocument uzxDoc = QJsonDocument::fromJson(raw);
            if (!uzxDoc.isNull() && uzxDoc.isObject()) {
                const QJsonObject obj = uzxDoc.object();
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (code != 0 && code != 200) {
                    const QString rawMsg = obj.value(QStringLiteral("msg"))
                                               .toString(QStringLiteral("request error"));
                    const QString msg = uzxFormatError(code, rawMsg);
                    const QString msgWithCode =
                        QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                    emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                    emit logMessage(QStringLiteral("%1 UZX SL/TP rejected: %2")
                                        .arg(contextTag(ctxPtr->accountName))
                                        .arg(msgWithCode));
                    accepted = false;
                }
            }
            if (!accepted) {
                return;
            }
            QString extractedOrderId;
            if (uzxDoc.isObject()) {
                const QJsonObject obj = uzxDoc.object();
                const QJsonObject dataObj = obj.value(QStringLiteral("data")).toObject();
                extractedOrderId = dataObj.value(QStringLiteral("order_id")).toString();
                if (extractedOrderId.isEmpty()) {
                    extractedOrderId = dataObj.value(QStringLiteral("orderId")).toString();
                }
                if (extractedOrderId.isEmpty()) {
                    extractedOrderId = obj.value(QStringLiteral("order_id")).toString();
                }
                if (extractedOrderId.isEmpty()) {
                    extractedOrderId = obj.value(QStringLiteral("orderId")).toString();
                }
                if (extractedOrderId.isEmpty()) {
                    extractedOrderId = dataObj.value(QStringLiteral("id")).toString();
                }
            }
            if (extractedOrderId.isEmpty()) {
                extractedOrderId = QStringLiteral("uzx_%1")
                                       .arg(QDateTime::currentMSecsSinceEpoch());
            }
            emit orderPlaced(ctxPtr->accountName,
                             sym,
                             closeSide,
                             triggerPrice,
                             posQty,
                             extractedOrderId);
            if (slTrigger > 0.0) {
                ctxPtr->uzxSlOrderIdBySymbol.insert(sym, extractedOrderId);
            }
            if (tpTrigger > 0.0) {
                ctxPtr->uzxTpOrderIdBySymbol.insert(sym, extractedOrderId);
            }
        });
        return;
    }

    if (profile != ConnectionStore::Profile::Lighter) {
        emit orderFailed(ctx.accountName, sym, tr("SL/TP is only supported for Lighter or UZX"));
        return;
    }

    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    const QString account = ctx.accountName;
    const int apiKeyIndex = ctx.credentials.apiKeyIndex;
    const long long accountIndex = static_cast<long long>(ctx.credentials.accountIndex);
    Context *ctxPtr = &ctx;

    auto fail = [this, account, sym](const QString &msg) {
        emit orderFailed(account, sym, msg);
        emit logMessage(QStringLiteral("%1 Lighter SL/TP error: %2")
                            .arg(contextTag(account), msg));
    };

    if (baseUrl.isEmpty()) {
        fail(tr("Missing Lighter base URL"));
        return;
    }

    auto fetchNonce = [this, baseUrl, apiKeyIndex, accountIndex, ctxPtr2 = &ctx](std::function<void(qint64 nonce, QString err)> cb) {
        QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/nextNonce"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("account_index"), QString::number(accountIndex));
        q.addQueryItem(QStringLiteral("api_key_index"), QString::number(apiKeyIndex));
        url.setQuery(q);
        QNetworkRequest req(url);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = ensureLighterNetwork(*ctxPtr2)->get(req);
        applyReplyTimeout(reply, 4500);
        connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
            const QNetworkReply::NetworkError err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                cb(0, lighterHttpErrorMessage(reply, raw));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isNull() || !doc.isObject()) {
                cb(0, QStringLiteral("Invalid Lighter nonce response"));
                return;
            }
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                cb(0, obj.value(QStringLiteral("message")).toString(QStringLiteral("Failed to get nonce")));
                return;
            }
            cb(static_cast<qint64>(obj.value(QStringLiteral("nonce")).toVariant().toLongLong()), QString());
        });
    };

    auto ensureNonce = [this, fetchNonce, ctxPtr, fail](std::function<void(qint64 nonce)> cb) {
        if (!ctxPtr) {
            fail(QStringLiteral("Internal error"));
            return;
        }
        if (ctxPtr->lighterNextNonce > 0 && !ctxPtr->lighterNonceInFlight) {
            const qint64 nonce = ctxPtr->lighterNextNonce;
            ctxPtr->lighterNextNonce = nonce + 1;
            cb(nonce);
            return;
        }
        ctxPtr->lighterNonceWaiters.push_back([cb](qint64 nonce, const QString &err) {
            if (!err.isEmpty() || nonce <= 0) {
                return;
            }
            cb(nonce);
        });
        if (ctxPtr->lighterNonceInFlight) {
            return;
        }
        ctxPtr->lighterNonceInFlight = true;
        fetchNonce([this, ctxPtr, fail](qint64 nonce, const QString &err) {
            if (!ctxPtr) {
                return;
            }
            ctxPtr->lighterNonceInFlight = false;
            if (!err.isEmpty() || nonce <= 0) {
                ctxPtr->lighterNextNonce = 0;
                const auto waiters = std::move(ctxPtr->lighterNonceWaiters);
                ctxPtr->lighterNonceWaiters.clear();
                for (const auto &w : waiters) {
                    w(0, err.isEmpty() ? QStringLiteral("Failed to get nonce") : err);
                }
                if (!err.isEmpty()) {
                    fail(err);
                }
                return;
            }
            const auto waiters = std::move(ctxPtr->lighterNonceWaiters);
            ctxPtr->lighterNonceWaiters.clear();
            ctxPtr->lighterNextNonce = nonce;
            for (const auto &w : waiters) {
                const qint64 reserved = ctxPtr->lighterNextNonce;
                ctxPtr->lighterNextNonce = reserved + 1;
                w(reserved, QString());
            }
        });
    };

    auto sendTx = [this, ctxPtr](int txType,
                                 const QString &txInfo,
                                 std::function<void(QString txHash, QString err)> cb) {
        if (!ctxPtr) {
            cb(QString(), QStringLiteral("Internal error"));
            return;
        }
        sendLighterTx(*ctxPtr, txType, txInfo, std::move(cb));
    };

    auto ensureMetaLoaded = [this, baseUrl, ctxPtr2 = &ctx](std::function<void(QString err)> cb) {
        auto &cache = lighterMetaCacheByUrl()[baseUrl];
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool freshEnough = (cache.updatedMs > 0 && (now - cache.updatedMs) < 5 * 60 * 1000);
        if (!cache.bySymbol.isEmpty() && freshEnough) {
            cb(QString());
            return;
        }
        cache.waiters.push_back(cb);
        if (cache.inFlight) {
            return;
        }
        cache.inFlight = true;

        QNetworkRequest req(lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookDetails")));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = ensureLighterNetwork(*ctxPtr2)->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, baseUrl]() {
            auto &cache2 = lighterMetaCacheByUrl()[baseUrl];
            cache2.inFlight = false;
            const QNetworkReply::NetworkError err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();

            QString outErr;
            if (err != QNetworkReply::NoError || status >= 400) {
                outErr = QStringLiteral("Failed to load Lighter markets: %1").arg(lighterHttpErrorMessage(reply, raw));
            } else {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    outErr = QStringLiteral("Failed to parse Lighter markets");
                } else {
                    const QJsonObject obj = doc.object();
                    const int code = obj.value(QStringLiteral("code")).toInt(0);
                    if (code != 200) {
                        outErr = obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter markets error"));
                    } else {
                        cache2.bySymbol.clear();
                        addLighterMarketMeta(cache2.bySymbol, obj.value(QStringLiteral("order_book_details")).toArray(), false);
                        addLighterMarketMeta(cache2.bySymbol, obj.value(QStringLiteral("spot_order_book_details")).toArray(), true);
                        cache2.updatedMs = QDateTime::currentMSecsSinceEpoch();
                    }
                }
            }

            const auto waiters = std::move(cache2.waiters);
            cache2.waiters.clear();
            for (const auto &w : waiters) {
                w(outErr);
            }
        });
    };

    auto sendStopOrder = [this,
                          sym,
                          account,
                          accountName,
                          baseUrl,
                          apiKeyIndex,
                          accountIndex,
                          ctxPtr,
                          triggerPrice,
                          fail,
                          ensureMetaLoaded,
                          ensureNonce,
                          sendTx](const TradePosition &pos, bool stopLoss) {
        if (!ctxPtr) {
            fail(tr("Internal error"));
            return;
        }
        const QString stopKey = sym + (stopLoss ? QStringLiteral("|SL") : QStringLiteral("|TP"));
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (ctxPtr->lighterStopPlaceInFlightKeys.contains(stopKey)) {
            ctxPtr->lighterStopQueuedTriggerByKey.insert(stopKey, triggerPrice);
            emit logMessage(QStringLiteral("%1 Lighter %2 placement coalesced for %3 @ %4 (in-flight)")
                                .arg(contextTag(account))
                                .arg(stopLoss ? QStringLiteral("SL") : QStringLiteral("TP"))
                                .arg(sym)
                                .arg(triggerPrice, 0, 'f', 6));
            return;
        }
        ctxPtr->lighterStopPlaceInFlightKeys.insert(stopKey);

        auto finish = [this, ctxPtr, stopKey, sym, accountName, stopLoss]() {
            if (!ctxPtr) {
                return;
            }
            ctxPtr->lighterStopPlaceInFlightKeys.remove(stopKey);
            if (!ctxPtr->lighterStopQueuedTriggerByKey.contains(stopKey)) {
                return;
            }
            const double queued = ctxPtr->lighterStopQueuedTriggerByKey.take(stopKey);
            if (!(queued > 0.0) || !std::isfinite(queued)) {
                return;
            }
            // Re-run placement for the latest queued click.
            QTimer::singleShot(0, this, [this, sym, accountName, queued, stopLoss]() {
                placeLighterStopOrder(sym, accountName, queued, stopLoss);
            });
        };

        ensureMetaLoaded([this,
                          sym,
                          account,
                          baseUrl,
                          apiKeyIndex,
                          accountIndex,
                          ctxPtr,
                          pos,
                          triggerPrice,
                           stopLoss,
                           fail,
                           ensureNonce,
                           sendTx,
                          finish](const QString &metaErr) {
            if (!metaErr.isEmpty()) {
                fail(metaErr);
                finish();
                return;
            }
            auto &cache = lighterMetaCacheByUrl()[baseUrl];
            const LighterMarketMeta meta = cache.bySymbol.value(sym, LighterMarketMeta{});
            if (meta.marketId < 0) {
                fail(QStringLiteral("Unknown Lighter symbol: %1").arg(sym));
                finish();
                return;
            }
            if (meta.isSpot) {
                fail(QStringLiteral("SL/TP is supported for perps only"));
                finish();
                return;
            }

            const qint64 baseScale = pow10i(meta.sizeDecimals);
            const qint64 priceScale = pow10i(meta.priceDecimals);
            const qint64 baseAmount = qRound64(pos.quantity * static_cast<double>(baseScale));
            if (baseAmount <= 0) {
                fail(QStringLiteral("Invalid position size"));
                finish();
                return;
            }
            const qint64 triggerInt64 = qRound64(triggerPrice * static_cast<double>(priceScale));
            if (triggerInt64 <= 0 || triggerInt64 > std::numeric_limits<int>::max()) {
                fail(QStringLiteral("Invalid scaled trigger price for %1").arg(sym));
                finish();
                return;
            }

            const int isAsk = (pos.side == OrderSide::Buy) ? 1 : 0; // close long=SELL, close short=BUY
            static constexpr double kSlippage = 0.01; // 1%
            const double acceptable =
                (isAsk == 1) ? (triggerPrice * (1.0 - kSlippage)) : (triggerPrice * (1.0 + kSlippage));
            const qint64 priceInt64 = qRound64(acceptable * static_cast<double>(priceScale));
            if (priceInt64 <= 0 || priceInt64 > std::numeric_limits<int>::max()) {
                fail(QStringLiteral("Invalid scaled price for %1").arg(sym));
                finish();
                return;
            }

            const int orderType = stopLoss ? 2 : 4; // StopLossOrder / TakeProfitOrder
            const int timeInForce = 0; // IOC
            const int reduceOnly = 1;
            const qint64 clientOrderIndex = nextLighterClientOrderIndex(*ctxPtr);
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 orderExpiry = nowMs + 28LL * 24LL * 60LL * 60LL * 1000LL;

            // Pre-seed stop kind by client_order_index so UI doesn't flap while waiting for WS/REST data.
            {
                auto &info = ctxPtr->lighterStopKindByOrderId[QString::number(clientOrderIndex)];
                info.kind = stopLoss ? Context::LighterStopKind::StopLoss : Context::LighterStopKind::TakeProfit;
                info.explicitType = true;
                info.updatedMs = nowMs;
            }

            // UX invariant: only one SL and one TP per symbol. Best-effort cancel of same-type stops before placing new.
            if (ctxPtr) {
                const QStringList existing =
                    stopLoss ? ctxPtr->lighterSlStopOrderIdsBySymbol.value(sym)
                             : ctxPtr->lighterTpStopOrderIdsBySymbol.value(sym);
                for (const auto &id : existing) {
                    const QString trimmed = id.trimmed();
                    if (!trimmed.isEmpty()) {
                        ctxPtr->lighterPendingCancelOrderIds.insert(trimmed);
                        cancelOrder(sym, account, trimmed);
                    }
                }
                if (stopLoss) {
                    ctxPtr->lighterSlStopOrderIdsBySymbol.insert(sym, {});
                } else {
                    ctxPtr->lighterTpStopOrderIdsBySymbol.insert(sym, {});
                }
            }

            emit logMessage(QStringLiteral("%1 Lighter %2 order: sym=%3 trigger=%4 avg_exec=%5 baseAmount=%6 priceDec=%7 sizeDec=%8 reduceOnly=1 tif=IOC exp=%9")
                                .arg(contextTag(account))
                                .arg(stopLoss ? QStringLiteral("SL") : QStringLiteral("TP"))
                                .arg(sym)
                                .arg(triggerPrice, 0, 'f', 6)
                                .arg(acceptable, 0, 'f', 6)
                                .arg(baseAmount)
                                .arg(meta.priceDecimals)
                                .arg(meta.sizeDecimals)
                                .arg(orderExpiry));

            ensureNonce([this,
                         sym,
                         account,
                         apiKeyIndex,
                          accountIndex,
                          ctxPtr,
                          meta,
                          stopLoss,
                          clientOrderIndex,
                          baseAmount,
                          priceInt64,
                          isAsk,
                          orderType,
                          timeInForce,
                          reduceOnly,
                          triggerInt64,
                          orderExpiry,
                          fail,
                          sendTx,
                          finish](qint64 nonce) {
                if (!ctxPtr || nonce <= 0) {
                    fail(QStringLiteral("Failed to get nonce"));
                    finish();
                    return;
                }
                const LighterSignedTxResponse signedTx =
                    lighterSigner().signCreateOrder(meta.marketId,
                                                    clientOrderIndex,
                                                    static_cast<long long>(baseAmount),
                                                    static_cast<int>(priceInt64),
                                                    isAsk,
                                                    orderType,
                                                    timeInForce,
                                                    reduceOnly,
                                                    static_cast<int>(triggerInt64),
                                                    static_cast<long long>(orderExpiry),
                                                    static_cast<long long>(nonce),
                                                    apiKeyIndex,
                                                    accountIndex);
                if (signedTx.err) {
                    fail(QString::fromUtf8(signedTx.err));
                    finish();
                    return;
                }
                const QString txInfo = signedTx.txInfo ? QString::fromUtf8(signedTx.txInfo) : QString();
                if (txInfo.isEmpty()) {
                    fail(QStringLiteral("Empty signed txInfo"));
                    finish();
                    return;
                }
                sendTx(static_cast<int>(signedTx.txType),
                       txInfo,
                       [this, sym, account, ctxPtr, fail, nonce, stopLoss, clientOrderIndex, finish](const QString &txHash, const QString &txErr) {
                     if (!txErr.isEmpty()) {
                         if (ctxPtr) {
                             ctxPtr->lighterNextNonce = 0;
                         }
                         fail(txErr);
                         finish();
                         return;
                     }
                     if (ctxPtr) {
                         ctxPtr->lighterNextNonce = nonce + 1;
                     }
                     emit logMessage(QStringLiteral("%1 Lighter SL/TP order sent for %2 (tx=%3)")
                                         .arg(contextTag(account), sym, txHash.left(12)));
                     if (ctxPtr) {
                         const QString id = QString::number(clientOrderIndex);
                         if (stopLoss) {
                             ctxPtr->lighterSlStopOrderIdsBySymbol.insert(sym, {id});
                         } else {
                             ctxPtr->lighterTpStopOrderIdsBySymbol.insert(sym, {id});
                         }
                         armLighterBurst(*ctxPtr, 10);
                     }
                     if (ctxPtr) {
                         // Post-submit checker: refresh active orders and let the poller auto-cancel duplicates.
                         QTimer::singleShot(650, this, [this, ctxPtr, sym]() {
                             if (ctxPtr) {
                                 fetchLighterActiveOrders(*ctxPtr, sym);
                             }
                         });
                         QTimer::singleShot(2000, this, [this, ctxPtr, sym]() {
                             if (ctxPtr) {
                                 fetchLighterActiveOrders(*ctxPtr, sym);
                             }
                         });
                     }
                     finish();
                 });
             });
         });
    };

    // Position side (long/short) can lag right after entry; retry briefly so we don't mis-classify SL/TP.
    Context *ctxPtrForAttempt = &ctx;
    auto attempt = std::make_shared<std::function<void(int)>>();
    *attempt = [this, attempt, ctxPtrForAttempt, sym, accountName, triggerPrice, isStopLoss, sendStopOrder, fail](int remaining) {
        if (!ctxPtrForAttempt) {
            fail(tr("Internal error"));
            return;
        }
        const TradePosition pos = positionForSymbol(sym, accountName);
        const bool hasPosition = pos.hasPosition && pos.quantity > 0.0 && pos.averagePrice > 0.0;
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 lastPosMs = ctxPtrForAttempt->positionUpdatedMsBySymbol.value(sym, 0);
        const bool fresh = lastPosMs > 0 && (nowMs - lastPosMs) <= 2500;
        if ((!hasPosition || !fresh) && remaining > 0) {
            fetchLighterAccount(*ctxPtrForAttempt);
            QTimer::singleShot(250, this, [attempt, remaining]() {
                (*attempt)(remaining - 1);
            });
            return;
        }
        if (!hasPosition) {
            fail(tr("No active position"));
            return;
        }
        Q_UNUSED(triggerPrice);
        sendStopOrder(pos, isStopLoss);
    };
    (*attempt)(10);
}

void TradeManager::cancelOrder(const QString &symbol, const QString &accountName, const QString &orderId)
{
    const QString sym = normalizedSymbol(symbol);
    const QString id = orderId.trimmed();
    if (sym.isEmpty() || id.isEmpty()) {
        return;
    }
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    Context &ctx = ensureContext(profile);
    if (!ensureCredentials(ctx)) {
        emit orderFailed(ctx.accountName, sym, tr("Missing credentials"));
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        emit orderFailed(ctx.accountName, sym, tr("Connect to the exchange first"));
        return;
    }

    if (profile == ConnectionStore::Profile::Lighter) {
        const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
        const QString account = ctx.accountName;
        const int apiKeyIndex = ctx.credentials.apiKeyIndex;
        const long long accountIndex = static_cast<long long>(ctx.credentials.accountIndex);
        Context *ctxPtr = &ctx;

        auto fail = [this, account, sym](const QString &msg) {
            emit orderFailed(account, sym, msg);
            emit logMessage(QStringLiteral("%1 Lighter cancel error: %2").arg(contextTag(account), msg));
        };

        if (baseUrl.isEmpty()) {
            fail(tr("Missing Lighter base URL"));
            return;
        }

        bool okId = false;
        const long long orderIndex = id.toLongLong(&okId);
        if (!okId || orderIndex <= 0) {
            fail(tr("Invalid order id: %1").arg(id));
            return;
        }

        ensureLighterMetaLoaded(ctx, baseUrl, [this, sym, baseUrl, apiKeyIndex, accountIndex, account, ctxPtr, orderIndex, fail](const QString &metaErr) {
            if (!metaErr.isEmpty()) {
                fail(metaErr);
                return;
            }
            auto &cache = lighterMetaCacheByUrl()[baseUrl];
            const LighterMarketMeta meta = cache.bySymbol.value(sym, LighterMarketMeta{});
            if (meta.marketId < 0) {
                fail(QStringLiteral("Unknown Lighter symbol: %1").arg(sym));
                return;
            }

            auto fetchNonce = [this, baseUrl, apiKeyIndex, accountIndex, ctxPtr2 = ctxPtr](std::function<void(qint64 nonce, QString err)> cb) {
                QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/nextNonce"));
                QUrlQuery q;
                q.addQueryItem(QStringLiteral("account_index"), QString::number(accountIndex));
                q.addQueryItem(QStringLiteral("api_key_index"), QString::number(apiKeyIndex));
                url.setQuery(q);
                QNetworkRequest req(url);
                req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
                QNetworkReply *reply = ensureLighterNetwork(*ctxPtr2)->get(req);
                applyReplyTimeout(reply, 4500);
                connect(reply, &QNetworkReply::finished, this, [reply, cb = std::move(cb), ctxPtr2]() mutable {
                    const QNetworkReply::NetworkError err = reply->error();
                    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    const QByteArray raw = reply->readAll();
                    reply->deleteLater();
                    if (err != QNetworkReply::NoError || status >= 400) {
                        cb(0, lighterHttpErrorMessage(reply, raw));
                        if (ctxPtr2) {
                            ctxPtr2->lighterNonceInFlight = false;
                        }
                        return;
                    }
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (!doc.isObject()) {
                        cb(0, QStringLiteral("Invalid nonce response"));
                        if (ctxPtr2) {
                            ctxPtr2->lighterNonceInFlight = false;
                        }
                        return;
                    }
                    const QJsonObject obj = doc.object();
                    const int code = obj.value(QStringLiteral("code")).toInt(0);
                    if (code != 200) {
                        cb(0, obj.value(QStringLiteral("message")).toString(QStringLiteral("Nonce fetch failed")));
                        if (ctxPtr2) {
                            ctxPtr2->lighterNonceInFlight = false;
                        }
                        return;
                    }
                    const qint64 nonce = obj.value(QStringLiteral("nonce")).toVariant().toLongLong();
                    cb(nonce, QString());
                    if (ctxPtr2) {
                        ctxPtr2->lighterNonceInFlight = false;
                        if (nonce > 0) {
                            ctxPtr2->lighterNextNonce = nonce;
                        }
                    }
                });
            };

            auto ensureNonce = [this, fetchNonce, ctxPtr, fail](std::function<void(qint64 nonce)> cb) {
                if (!ctxPtr) {
                    fail(tr("Internal error"));
                    return;
                }
                if (ctxPtr->lighterNextNonce > 0 && !ctxPtr->lighterNonceInFlight) {
                    const qint64 nonce = ctxPtr->lighterNextNonce;
                    ctxPtr->lighterNextNonce = nonce + 1;
                    cb(nonce);
                    return;
                }
                ctxPtr->lighterNonceWaiters.push_back([cb](qint64 nonce, const QString &err) {
                    if (!err.isEmpty() || nonce <= 0) {
                        return;
                    }
                    cb(nonce);
                });
                if (ctxPtr->lighterNonceInFlight) {
                    return;
                }
                ctxPtr->lighterNonceInFlight = true;
                fetchNonce([this, ctxPtr, fail](qint64 nonce, const QString &err) {
                    if (!ctxPtr) {
                        return;
                    }
                    ctxPtr->lighterNonceInFlight = false;
                    if (!err.isEmpty() || nonce <= 0) {
                        ctxPtr->lighterNextNonce = 0;
                        const auto waiters = std::move(ctxPtr->lighterNonceWaiters);
                        ctxPtr->lighterNonceWaiters.clear();
                        for (const auto &w : waiters) {
                            w(0, err.isEmpty() ? QStringLiteral("Failed to get nonce") : err);
                        }
                        if (!err.isEmpty()) {
                            fail(err);
                        }
                        return;
                    }
                    const auto waiters = std::move(ctxPtr->lighterNonceWaiters);
                    ctxPtr->lighterNonceWaiters.clear();
                    ctxPtr->lighterNextNonce = nonce;
                    for (const auto &w : waiters) {
                        const qint64 reserved = ctxPtr->lighterNextNonce;
                        ctxPtr->lighterNextNonce = reserved + 1;
                        w(reserved, QString());
                    }
                });
            };

            ensureNonce([this, sym, meta, apiKeyIndex, accountIndex, account, ctxPtr, fail, orderIndex](qint64 nonce) {
                if (!lighterSigner().lib.isLoaded() || !lighterSigner().signCancelOrder) {
                    // `ensureCredentials` already downloads/loads signer when needed, but keep a guard anyway.
                    fail(QStringLiteral("Lighter signer not available"));
                    return;
                }
                const LighterSignedTxResponse signedTx =
                    lighterSigner().signCancelOrder(meta.marketId,
                                                    orderIndex,
                                                    static_cast<long long>(nonce),
                                                    apiKeyIndex,
                                                    accountIndex);
                const QString txInfo = signedTx.txInfo ? QString::fromUtf8(signedTx.txInfo) : QString();
                if (txInfo.isEmpty()) {
                    fail(QStringLiteral("Empty signed txInfo"));
                    return;
                }
                sendLighterTx(*ctxPtr,
                              static_cast<int>(signedTx.txType),
                              txInfo,
                              [this, sym, account, ctxPtr, fail, nonce, orderIndex](const QString &txHash, const QString &txErr) {
                                  if (!txErr.isEmpty()) {
                                      if (ctxPtr) {
                                          ctxPtr->lighterNextNonce = 0;
                                      }
                                      fail(txErr);
                                      return;
                                  }
                                  if (ctxPtr) {
                                      ctxPtr->lighterNextNonce = nonce + 1;
                                      armLighterBurst(*ctxPtr, 6);
                                  }
                                  emit logMessage(QStringLiteral("%1 Lighter cancel sent for %2 id=%3 (tx=%4)")
                                                      .arg(contextTag(account),
                                                           sym,
                                                           QString::number(orderIndex),
                                                           txHash.left(12)));
                                  if (ctxPtr) {
                                      const QString orderId = QString::number(orderIndex);
                                      auto it = ctxPtr->activeOrders.constFind(orderId);
                                      if (it != ctxPtr->activeOrders.constEnd()) {
                                          const OrderRecord rec = it.value();
                                          ctxPtr->activeOrders.remove(orderId);
                                          emitLocalOrderSnapshot(*ctxPtr, rec.symbol);
                                          emit orderCanceled(ctxPtr->accountName,
                                                             rec.symbol,
                                                             rec.side,
                                                             rec.price,
                                                             orderId);
                                      } else {
                                          emit orderCanceled(ctxPtr->accountName,
                                                             sym,
                                                             OrderSide::Buy,
                                                             0.0,
                                                             orderId);
                                      }
                                  }
                              });
            });
        });
        return;
    }

    if (profile == ConnectionStore::Profile::UzxSwap
        || profile == ConnectionStore::Profile::UzxSpot) {
        const int instType = (profile == ConnectionStore::Profile::UzxSwap) ? 2 : 1;
        QJsonObject payload;
        payload.insert(QStringLiteral("inst_type"), instType);
        payload.insert(QStringLiteral("cancel_ord_type"), 1);
        payload.insert(QStringLiteral("order_id"), id);
        const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        emit logMessage(QStringLiteral("%1 UZX cancel requested: %2 id=%3")
                            .arg(contextTag(ctx.accountName), sym, id));
        QNetworkRequest req =
            makeUzxRequest(QStringLiteral("/v2/trade/cancel-order"),
                           body,
                           QStringLiteral("PUT"),
                           QUrlQuery(),
                           ctx);
        auto *reply = m_network.sendCustomRequest(req, QByteArrayLiteral("PUT"), body);
        connect(reply, &QNetworkReply::finished, this, [this, reply, sym, id, ctxPtr = &ctx]() {
            const QNetworkReply::NetworkError err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                QString msg = reply->errorString();
                if (status >= 400) {
                    msg = QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(raw));
                }
                emit orderFailed(ctxPtr->accountName, sym, msg);
                emit logMessage(QStringLiteral("%1 UZX cancel error: %2")
                                    .arg(contextTag(ctxPtr->accountName), msg));
                return;
            }
            emit logMessage(QStringLiteral("%1 UZX cancel response: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? QStringLiteral("<empty>")
                                                   : QString::fromUtf8(raw)));
            if (!raw.isEmpty()) {
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isObject()) {
                    const QJsonObject obj = doc.object();
                    const int code = obj.value(QStringLiteral("code")).toInt(0);
                    if (code != 0 && code != 200) {
                        const QString rawMsg = obj.value(QStringLiteral("msg"))
                                                   .toString(QStringLiteral("request error"));
                        const QString msg = uzxFormatError(code, rawMsg);
                        const QString msgWithCode =
                            QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                        emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                        emit logMessage(QStringLiteral("%1 UZX cancel rejected: %2")
                                            .arg(contextTag(ctxPtr->accountName))
                                            .arg(msgWithCode));
                        return;
                    }
                }
            }
            emit logMessage(QStringLiteral("%1 UZX cancel sent for %2 id=%3")
                                .arg(contextTag(ctxPtr->accountName), sym, id));
            if (ctxPtr) {
                auto it = ctxPtr->activeOrders.constFind(id);
                if (it != ctxPtr->activeOrders.constEnd()) {
                    const OrderRecord rec = it.value();
                    ctxPtr->activeOrders.remove(id);
                    emitLocalOrderSnapshot(*ctxPtr, rec.symbol);
                    emit orderCanceled(ctxPtr->accountName, rec.symbol, rec.side, rec.price, id);
                } else {
                    emit orderCanceled(ctxPtr->accountName, sym, OrderSide::Buy, 0.0, id);
                }
            }
        });
        return;
    }

    if (profile == ConnectionStore::Profile::MexcFutures) {
        // Not implemented yet; fall back to cancel-all for the symbol.
        cancelAllMexcFuturesOrders(ctx, sym);
        return;
    }

    // MEXC Spot / Binance-like: DELETE /api/v3/order
    emit logMessage(QStringLiteral("%1 Cancel requested for %2 id=%3")
                        .arg(contextTag(ctx.accountName), sym, id));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), sym);
    query.addQueryItem(QStringLiteral("orderId"), id);
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    query.addQueryItem(QStringLiteral("timestamp"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"), QString::fromLatin1(signPayload(query, ctx)));
    QNetworkRequest req = makePrivateRequest(QStringLiteral("/api/v3/order"),
                                             signedQuery,
                                             QByteArray(),
                                             ctx);
    auto *reply = ensureMexcNetwork(ctx)->sendCustomRequest(req, QByteArrayLiteral("DELETE"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, sym, id, ctxPtr2 = &ctx]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            QString msg = reply->errorString();
            if (status >= 400) {
                msg = QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(raw));
            }
            emit orderFailed(ctxPtr2->accountName, sym, msg);
            emit logMessage(QStringLiteral("%1 Cancel error: %2").arg(contextTag(ctxPtr2->accountName), msg));
            return;
        }
        emit logMessage(QStringLiteral("%1 Cancel sent for %2 id=%3: %4")
                            .arg(contextTag(ctxPtr2->accountName),
                                 sym,
                                 id,
                                 raw.isEmpty() ? QStringLiteral("<empty>") : QString::fromUtf8(raw)));
        if (ctxPtr2) {
            auto it = ctxPtr2->activeOrders.constFind(id);
            if (it != ctxPtr2->activeOrders.constEnd()) {
                const OrderRecord rec = it.value();
                ctxPtr2->activeOrders.remove(id);
                emitLocalOrderSnapshot(*ctxPtr2, rec.symbol);
                emit orderCanceled(ctxPtr2->accountName, rec.symbol, rec.side, rec.price, id);
            } else {
                emit orderCanceled(ctxPtr2->accountName, sym, OrderSide::Buy, 0.0, id);
            }
        }
    });
}

void TradeManager::cancelLighterStopOrders(const QString &symbol,
                                          const QString &accountName,
                                          bool isStopLoss)
{
    const QString sym = normalizedSymbol(symbol);
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    Context &ctx = ensureContext(profile);
    if (!ensureCredentials(ctx)) {
        emit orderFailed(ctx.accountName, sym, tr("Missing credentials"));
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        emit orderFailed(ctx.accountName, sym, tr("Connect to the exchange first"));
        return;
    }
    if (profile == ConnectionStore::Profile::UzxSwap
        || profile == ConnectionStore::Profile::UzxSpot) {
        const QString id = isStopLoss ? ctx.uzxSlOrderIdBySymbol.value(sym)
                                      : ctx.uzxTpOrderIdBySymbol.value(sym);
        if (id.isEmpty()) {
            return;
        }
        emit logMessage(QStringLiteral("%1 UZX cancel %2 stop: sym=%3 id=%4")
                            .arg(contextTag(ctx.accountName))
                            .arg(isStopLoss ? QStringLiteral("SL") : QStringLiteral("TP"))
                            .arg(sym)
                            .arg(id));
        cancelOrder(sym, ctx.accountName, id);
        if (isStopLoss) {
            ctx.uzxSlOrderIdBySymbol.remove(sym);
        } else {
            ctx.uzxTpOrderIdBySymbol.remove(sym);
        }
        return;
    }
    if (profile != ConnectionStore::Profile::Lighter) {
        return;
    }

    const QStringList existing =
        isStopLoss ? ctx.lighterSlStopOrderIdsBySymbol.value(sym)
                   : ctx.lighterTpStopOrderIdsBySymbol.value(sym);
    if (existing.isEmpty()) {
        return;
    }

    const QString account = ctx.accountName;
    emit logMessage(QStringLiteral("%1 Lighter cancel %2 stops: sym=%3 ids=%4")
                        .arg(contextTag(account))
                        .arg(isStopLoss ? QStringLiteral("SL") : QStringLiteral("TP"))
                        .arg(sym)
                        .arg(existing.join(QStringLiteral(","))));

    for (const auto &id0 : existing) {
        const QString id = id0.trimmed();
        if (id.isEmpty()) {
            continue;
        }
        ctx.lighterPendingCancelOrderIds.insert(id);
        cancelOrder(sym, account, id);
    }
    if (isStopLoss) {
        ctx.lighterSlStopOrderIdsBySymbol.insert(sym, {});
    } else {
        ctx.lighterTpStopOrderIdsBySymbol.insert(sym, {});
    }
}

void TradeManager::cancelAllOrders(const QString &symbol, const QString &accountName)
{
    const QString sym = normalizedSymbol(symbol);
    const ConnectionStore::Profile profile = profileFromAccountName(accountName);
    Context &ctx = ensureContext(profile);
    if (!ensureCredentials(ctx)) {
        emit orderFailed(ctx.accountName, sym, tr("Missing credentials"));
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        emit orderFailed(ctx.accountName, sym, tr("Connect to the exchange first"));
        return;
    }
    if (profile == ConnectionStore::Profile::Lighter) {
        const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
        const QString account = ctx.accountName;
        const int apiKeyIndex = ctx.credentials.apiKeyIndex;
        const long long accountIndex = static_cast<long long>(ctx.credentials.accountIndex);
        Context *ctxPtr = &ctx;

        auto fail = [this, account, sym](const QString &msg) {
            emit orderFailed(account, sym, msg);
            emit logMessage(QStringLiteral("%1 Lighter cancel-all error: %2")
                                .arg(contextTag(account), msg));
        };

        if (baseUrl.isEmpty()) {
            fail(tr("Missing Lighter base URL"));
            return;
        }

        auto fetchNonce = [this, baseUrl, apiKeyIndex, accountIndex, ctxPtr2 = &ctx](std::function<void(qint64 nonce, QString err)> cb) {
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/nextNonce"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("account_index"), QString::number(accountIndex));
            q.addQueryItem(QStringLiteral("api_key_index"), QString::number(apiKeyIndex));
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = ensureLighterNetwork(*ctxPtr2)->get(req);
            applyReplyTimeout(reply, 4500);
            connect(reply, &QNetworkReply::finished, this, [reply, cb]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    cb(0, lighterHttpErrorMessage(reply, raw));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    cb(0, QStringLiteral("Invalid Lighter nonce response"));
                    return;
                }
                const QJsonObject obj = doc.object();
                if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                    cb(0, obj.value(QStringLiteral("message")).toString(QStringLiteral("Failed to get nonce")));
                    return;
                }
                cb(static_cast<qint64>(obj.value(QStringLiteral("nonce")).toVariant().toLongLong()), QString());
            });
        };

        auto ensureNonce = [this, fetchNonce, ctxPtr, fail](std::function<void(qint64 nonce)> cb) {
            if (!ctxPtr) {
                fail(QStringLiteral("Internal error"));
                return;
            }
            if (ctxPtr->lighterNextNonce > 0 && !ctxPtr->lighterNonceInFlight) {
                const qint64 nonce = ctxPtr->lighterNextNonce;
                ctxPtr->lighterNextNonce = nonce + 1;
                cb(nonce);
                return;
            }
            ctxPtr->lighterNonceWaiters.push_back([cb](qint64 nonce, const QString &err) {
                if (!err.isEmpty() || nonce <= 0) {
                    return;
                }
                cb(nonce);
            });
            if (ctxPtr->lighterNonceInFlight) {
                return;
            }
            ctxPtr->lighterNonceInFlight = true;
            fetchNonce([this, ctxPtr, fail](qint64 nonce, const QString &err) {
                if (!ctxPtr) {
                    return;
                }
                ctxPtr->lighterNonceInFlight = false;
                if (!err.isEmpty() || nonce <= 0) {
                    ctxPtr->lighterNextNonce = 0;
                    const auto waiters = std::move(ctxPtr->lighterNonceWaiters);
                    ctxPtr->lighterNonceWaiters.clear();
                    for (const auto &w : waiters) {
                        w(0, err.isEmpty() ? QStringLiteral("Failed to get nonce") : err);
                    }
                    if (!err.isEmpty()) {
                        fail(err);
                    }
                    return;
                }
                const auto waiters = std::move(ctxPtr->lighterNonceWaiters);
                ctxPtr->lighterNonceWaiters.clear();
                ctxPtr->lighterNextNonce = nonce;
                for (const auto &w : waiters) {
                    const qint64 reserved = ctxPtr->lighterNextNonce;
                    ctxPtr->lighterNextNonce = reserved + 1;
                    w(reserved, QString());
                }
            });
        };

        auto sendTx = [this, ctxPtr](int txType,
                                     const QString &txInfo,
                                     std::function<void(QString txHash, QString err)> cb) {
            if (!ctxPtr) {
                cb(QString(), QStringLiteral("Internal error"));
                return;
            }
            sendLighterTx(*ctxPtr, txType, txInfo, std::move(cb));
        };

        emit logMessage(QStringLiteral("%1 Cancel-all requested for %2")
                            .arg(contextTag(account), sym));
        ctx.pendingCancelSymbols.insert(sym);

        ensureNonce([this, sym, apiKeyIndex, accountIndex, account, fail, sendTx, ctxPtr](qint64 nonce) {
            if (!ctxPtr || nonce <= 0) {
                fail(QStringLiteral("Failed to get nonce"));
                return;
            }
            const int timeInForce = 0; // immediate
            const qint64 cancelAllTimeMs = 0; // must be omitted in tx_info (server expects nil)
            const LighterSignedTxResponse signedTx =
                lighterSigner().signCancelAllOrders(timeInForce,
                                                    static_cast<long long>(cancelAllTimeMs),
                                                    static_cast<long long>(nonce),
                                                    apiKeyIndex,
                                                    accountIndex);
            if (signedTx.err) {
                fail(QString::fromUtf8(signedTx.err));
                return;
            }
            const QString txInfo = signedTx.txInfo ? QString::fromUtf8(signedTx.txInfo) : QString();
            if (txInfo.isEmpty()) {
                fail(QStringLiteral("Empty signed txInfo"));
                return;
            }
            sendTx(static_cast<int>(signedTx.txType), txInfo, [this, sym, account, ctxPtr, fail, nonce](const QString &txHash, const QString &txErr) {
                if (!txErr.isEmpty()) {
                    if (ctxPtr) {
                        ctxPtr->lighterNextNonce = 0;
                    }
                    fail(txErr);
                    return;
                }
                if (ctxPtr) {
                    ctxPtr->lighterNextNonce = nonce + 1;
                }
                emit logMessage(QStringLiteral("%1 Lighter cancel-all sent for %2 (tx=%3)")
                                    .arg(contextTag(account), sym, txHash.left(12)));
                if (ctxPtr) {
                    clearSymbolActiveOrders(*ctxPtr, sym);
                    emitLocalOrderSnapshot(*ctxPtr, sym);
                }
            });
        });
        return;
    }
    if (profile == ConnectionStore::Profile::MexcFutures) {
        cancelAllMexcFuturesOrders(ctx, sym);
        return;
    }
    if (profile == ConnectionStore::Profile::UzxSpot
        || profile == ConnectionStore::Profile::UzxSwap) {
        const int instType = (profile == ConnectionStore::Profile::UzxSwap) ? 2 : 1;
        const bool isSwap = (profile == ConnectionStore::Profile::UzxSwap);
        const QString insType = isSwap ? QStringLiteral("SWAP") : QStringLiteral("SPOT");
        const QString productName = uzxWireSymbol(sym, isSwap);
        auto sendUzxCancelBatch = [this, sym, ctxPtr = &ctx, instType](const QJsonArray &ids) {
            if (!ctxPtr) {
                return;
            }
            if (ids.isEmpty()) {
                emit logMessage(QStringLiteral("%1 UZX cancel-all: no active orders for %2")
                                    .arg(contextTag(ctxPtr->accountName), sym));
                return;
            }
            QJsonObject payload;
            payload.insert(QStringLiteral("inst_type"), instType);
            payload.insert(QStringLiteral("cancel_ord_type"), 1);
            payload.insert(QStringLiteral("order_ids"), ids);
            const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            emit logMessage(QStringLiteral("%1 UZX cancel-all requested for %2 (%3 orders)")
                                .arg(contextTag(ctxPtr->accountName), sym, QString::number(ids.size())));
            QNetworkRequest req =
                makeUzxRequest(QStringLiteral("/v2/trade/cancel-batch-order"),
                               body,
                               QStringLiteral("PUT"),
                               QUrlQuery(),
                               *ctxPtr);
            auto *reply = m_network.sendCustomRequest(req, QByteArrayLiteral("PUT"), body);
            connect(reply, &QNetworkReply::finished, this, [this, reply, sym, ctxPtr]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    QString msg = reply->errorString();
                    if (status >= 400) {
                        msg = QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(raw));
                    }
                    emit orderFailed(ctxPtr->accountName, sym, msg);
                    emit logMessage(QStringLiteral("%1 UZX cancel-all error: %2")
                                        .arg(contextTag(ctxPtr->accountName), msg));
                    return;
                }
                emit logMessage(QStringLiteral("%1 UZX cancel-all response: %2")
                                    .arg(contextTag(ctxPtr->accountName),
                                         raw.isEmpty() ? QStringLiteral("<empty>")
                                                       : QString::fromUtf8(raw)));
                if (!raw.isEmpty()) {
                    const QJsonDocument doc = QJsonDocument::fromJson(raw);
                    if (doc.isObject()) {
                        const QJsonObject obj = doc.object();
                        const int code = obj.value(QStringLiteral("code")).toInt(0);
                        if (code != 0 && code != 200) {
                            const QString rawMsg = obj.value(QStringLiteral("msg"))
                                                       .toString(QStringLiteral("request error"));
                            const QString msg = uzxFormatError(code, rawMsg);
                            const QString msgWithCode =
                                QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                            emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                            emit logMessage(QStringLiteral("%1 UZX cancel-all rejected: %2")
                                                .arg(contextTag(ctxPtr->accountName))
                                                .arg(msgWithCode));
                            return;
                        }
                    }
                }
                emit logMessage(QStringLiteral("%1 UZX cancel-all sent for %2")
                                    .arg(contextTag(ctxPtr->accountName), sym));
                if (ctxPtr) {
                    QList<OrderRecord> removed;
                    for (auto it = ctxPtr->activeOrders.constBegin();
                         it != ctxPtr->activeOrders.constEnd();
                         ++it) {
                        if (it.value().symbol == sym) {
                            OrderRecord record = it.value();
                            record.orderId = it.key();
                            removed.push_back(record);
                        }
                    }
                    clearSymbolActiveOrders(*ctxPtr, sym);
                    emitLocalOrderSnapshot(*ctxPtr, sym);
                    for (const auto &record : removed) {
                        emit orderCanceled(ctxPtr->accountName,
                                           record.symbol,
                                           record.side,
                                           record.price,
                                           record.orderId);
                    }
                }
            });
        };
        QJsonArray ids;
        for (auto it = ctx.activeOrders.constBegin(); it != ctx.activeOrders.constEnd(); ++it) {
            if (it.value().symbol == sym) {
                ids.append(it.key());
            }
        }
        if (ids.isEmpty()) {
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("ins_type"), insType);
            if (!productName.isEmpty()) {
                query.addQueryItem(QStringLiteral("product_name"), productName);
            }
            QNetworkRequest req =
                makeUzxRequest(QStringLiteral("/v2/trade/orders"),
                               QByteArray(),
                               QStringLiteral("GET"),
                               query,
                               ctx);
            auto *reply = m_network.get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, sym, ctxPtr = &ctx, sendUzxCancelBatch]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (err != QNetworkReply::NoError || status >= 400) {
                    QString msg = reply->errorString();
                    if (status >= 400) {
                        msg = QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(raw));
                    }
                    emit orderFailed(ctxPtr->accountName, sym, msg);
                    emit logMessage(QStringLiteral("%1 UZX open orders error: %2")
                                        .arg(contextTag(ctxPtr->accountName), msg));
                    return;
                }
                QJsonArray orderIds;
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isObject()) {
                    const QJsonObject obj = doc.object();
                    const int code = obj.value(QStringLiteral("code")).toInt(0);
                    if (code != 0 && code != 200) {
                        const QString rawMsg = obj.value(QStringLiteral("msg"))
                                                   .toString(QStringLiteral("request error"));
                        const QString msg = uzxFormatError(code, rawMsg);
                        const QString msgWithCode =
                            QStringLiteral("%1 (code %2)").arg(msg).arg(code);
                        emit orderFailed(ctxPtr->accountName, sym, msgWithCode);
                        emit logMessage(QStringLiteral("%1 UZX open orders rejected: %2")
                                            .arg(contextTag(ctxPtr->accountName))
                                            .arg(msgWithCode));
                        return;
                    }
                    const QJsonArray dataArr = obj.value(QStringLiteral("data")).toArray();
                    for (const auto &val : dataArr) {
                        if (!val.isObject()) {
                            continue;
                        }
                        const QJsonObject o = val.toObject();
                        const QString orderId =
                            o.value(QStringLiteral("order_id")).toVariant().toString().trimmed();
                        if (!orderId.isEmpty()) {
                            orderIds.append(orderId);
                        }
                    }
                }
                sendUzxCancelBatch(orderIds);
            });
            return;
        }
        sendUzxCancelBatch(ids);
        return;
    }
    emit logMessage(QStringLiteral("%1 Cancel-all requested for %2")
                        .arg(contextTag(ctx.accountName), sym));
    ctx.pendingCancelSymbols.insert(sym);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), sym);
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    query.addQueryItem(QStringLiteral("timestamp"),
                       QString::number(QDateTime::currentMSecsSinceEpoch()));
    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"),
                             QString::fromLatin1(signPayload(query, ctx)));

    QNetworkRequest req = makePrivateRequest(QStringLiteral("/api/v3/openOrders"),
                                             signedQuery,
                                             QByteArray(),
                                             ctx);
    auto *reply = ensureMexcNetwork(ctx)->sendCustomRequest(req, QByteArrayLiteral("DELETE"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, sym, ctxPtr = &ctx]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            QString msg = reply->errorString();
            if (status >= 400) {
                msg = QStringLiteral("HTTP %1: %2").arg(status).arg(QString::fromUtf8(raw));
            }
            emit orderFailed(ctxPtr->accountName, sym, msg);
            emit logMessage(QStringLiteral("%1 Cancel all error: %2")
                                .arg(contextTag(ctxPtr->accountName), msg));
            return;
        }
        emit logMessage(QStringLiteral("%1 Cancel all sent for %2 (response: %3)")
                            .arg(contextTag(ctxPtr->accountName),
                                 sym,
                                 raw.isEmpty() ? QStringLiteral("<empty>")
                                               : QString::fromUtf8(raw)));
        clearSymbolActiveOrders(*ctxPtr, sym);
    });
}
double TradeManager::handleOrderFill(Context &ctx,
                                     const QString &symbol,
                                     OrderSide side,
                                     double price,
                                     double quantity,
                                     double *outClosedNotional)
{
    const QString sym = normalizedSymbol(symbol);
    TradePosition &pos = ctx.positions[sym];
    double realizedDelta = 0.0;
    if (outClosedNotional) {
        *outClosedNotional = 0.0;
    }
    if (!pos.hasPosition) {
        pos.hasPosition = true;
        pos.side = side;
        pos.averagePrice = price;
        pos.quantity = quantity;
        pos.qtyMultiplier = 1.0;
    } else if (pos.side == side) {
        const double totalNotional = pos.averagePrice * pos.quantity + price * quantity;
        pos.quantity += quantity;
        if (pos.quantity > 1e-9) {
            pos.averagePrice = totalNotional / pos.quantity;
        } else {
            pos.averagePrice = price;
        }
    } else {
        const double closingQty = std::min(pos.quantity, quantity);
        double pnl = 0.0;
        if (pos.side == OrderSide::Buy) {
            pnl = (price - pos.averagePrice) * closingQty;
        } else {
            pnl = (pos.averagePrice - price) * closingQty;
        }
        pos.realizedPnl += pnl;
        realizedDelta = pnl;
        if (outClosedNotional) {
            *outClosedNotional = std::abs(pos.averagePrice * closingQty);
        }
        pos.quantity -= closingQty;
        if (pos.quantity <= 1e-8) {
            pos.hasPosition = false;
            pos.quantity = 0.0;
            pos.averagePrice = 0.0;
            pos.side = side;
        }
        const double remainder = quantity - closingQty;
        if (remainder > 1e-8) {
            TradePosition &newPos = ctx.positions[sym];
            if (!newPos.hasPosition) {
                newPos.hasPosition = true;
                newPos.side = side;
                newPos.quantity = remainder;
                newPos.averagePrice = price;
            } else if (newPos.side == side) {
                const double total = newPos.averagePrice * newPos.quantity + price * remainder;
                newPos.quantity += remainder;
                newPos.averagePrice = total / newPos.quantity;
            }
        }
    }
    emitPositionChanged(ctx, sym);
    return realizedDelta;
}

void TradeManager::emitPositionChanged(Context &ctx, const QString &symbol)
{
    const QString sym = normalizedSymbol(symbol);
    if (!sym.isEmpty()) {
        ctx.positionUpdatedMsBySymbol.insert(sym, QDateTime::currentMSecsSinceEpoch());
    }
    emit positionChanged(ctx.accountName, symbol, ctx.positions.value(symbol));
}

QByteArray TradeManager::signPayload(const QUrlQuery &query, const Context &ctx) const
{
    const QByteArray payload = query.query(QUrl::FullyEncoded).toUtf8();
    return QMessageAuthenticationCode::hash(payload,
                                            ctx.credentials.secretKey.toUtf8(),
                                            QCryptographicHash::Sha256)
        .toHex();
}

QByteArray TradeManager::signUzxPayload(const QByteArray &body,
                                        const QString &method,
                                        const QString &path,
                                        const QUrlQuery &query,
                                        const Context &ctx) const
{
    const QString ts = QString::number(QDateTime::currentSecsSinceEpoch());
    QString queryPart;
    if (!query.isEmpty()) {
        const QString enc = query.query(QUrl::FullyEncoded);
        if (!enc.isEmpty()) {
            queryPart = QStringLiteral("?") + enc;
        }
    }
    const QString base = ts + method.toUpper() + path + queryPart + QString::fromUtf8(body);
    const QByteArray sig = QMessageAuthenticationCode::hash(base.toUtf8(),
                                                            ctx.credentials.secretKey.toUtf8(),
                                                            QCryptographicHash::Sha256)
                               .toBase64();
    QByteArray out;
    out.append(ts.toUtf8());
    out.append('\n');
    out.append(sig);
    return out;
}

QNetworkRequest TradeManager::makePrivateRequest(const QString &path,
                                                 const QUrlQuery &query,
                                                 const QByteArray &contentType,
                                                 const Context &ctx) const
{
    QUrl url(m_baseUrl + path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    QNetworkRequest req(url);
    if (!contentType.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    req.setRawHeader("X-MEXC-APIKEY", ctx.credentials.apiKey.toUtf8());
    return req;
}

QNetworkRequest TradeManager::makeUzxRequest(const QString &path,
                                             const QByteArray &body,
                                             const QString &method,
                                             const QUrlQuery &query,
                                             const Context &ctx) const
{
    QUrl url(m_uzxBaseUrl + path);
    if (!query.isEmpty()) {
        url.setQuery(query);
    }
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray tsSig = signUzxPayload(body, method, path, query, ctx);
    const QList<QByteArray> parts = tsSig.split('\n');
    const QByteArray ts = parts.value(0);
    const QByteArray sig = parts.value(1);
    req.setRawHeader("UZX-ACCESS-KEY", ctx.credentials.apiKey.toUtf8());
    req.setRawHeader("UZX-ACCESS-SIGN", sig);
    req.setRawHeader("UZX-ACCESS-TIMESTAMP", ts);
    req.setRawHeader("UZX-ACCESS-PASSPHRASE", ctx.credentials.passphrase.toUtf8());
    return req;
}

void TradeManager::requestListenKey(Context &ctx)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("timestamp"),
                       QString::number(QDateTime::currentMSecsSinceEpoch()));
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"),
                             QString::fromLatin1(signPayload(query, ctx)));
    QNetworkRequest request = makePrivateRequest(QStringLiteral("/api/v3/userDataStream"),
                                                 signedQuery,
                                                 QByteArrayLiteral("application/json"),
                                                 ctx);
    auto *reply = ensureMexcNetwork(ctx)->post(request, QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        const QNetworkReply::NetworkError err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            const QString message = raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw);
            resetConnection(*ctxPtr, message);
            emit logMessage(QStringLiteral("%1 Listen key request failed: %2")
                                .arg(contextTag(ctxPtr->accountName), message));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        const QJsonObject obj = doc.object();
        const QString listenKey = obj.value(QStringLiteral("listenKey")).toString();
        if (listenKey.isEmpty()) {
            resetConnection(*ctxPtr, QStringLiteral("Listen key missing"));
            emit logMessage(QStringLiteral("%1 Unexpected listen key payload: %2")
                                .arg(contextTag(ctxPtr->accountName), QString::fromUtf8(raw)));
            return;
        }
        emit logMessage(QStringLiteral("%1 Received listen key %2, opening private WS...")
                            .arg(contextTag(ctxPtr->accountName), listenKey));
        initializeWebSocket(*ctxPtr, listenKey);
    });
}

void TradeManager::initializeWebSocket(Context &ctx, const QString &listenKey)
{
    ctx.listenKey = listenKey;
    if (ctx.privateSocket.state() != QAbstractSocket::UnconnectedState) {
        ctx.closingSocket = true;
        ctx.privateSocket.close();
    }
    if (ctx.openOrdersTimer.isActive()) {
        ctx.openOrdersTimer.stop();
    }
    ctx.openOrdersPending = false;
    ctx.trackedSymbols.clear();
    QUrl url(QStringLiteral("wss://wbs-api.mexc.com/ws"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("listenKey"), listenKey);
    url.setQuery(query);
    emit logMessage(QStringLiteral("%1 Connecting to %2")
                        .arg(contextTag(ctx.accountName), url.toString(QUrl::RemoveUserInfo)));
    ctx.privateSocket.open(url);
}

void TradeManager::initializeFuturesWebSocket(Context &ctx)
{
    if (ctx.privateSocket.state() != QAbstractSocket::UnconnectedState) {
        ctx.closingSocket = true;
        ctx.privateSocket.close();
    }
    if (ctx.openOrdersTimer.isActive()) {
        ctx.openOrdersTimer.stop();
    }
    ctx.openOrdersPending = false;
    ctx.trackedSymbols.clear();
    ctx.futuresLoggedIn = false;
    ctx.hasSubscribed = false;
    QUrl url(QStringLiteral("wss://contract.mexc.com/edge"));
    emit logMessage(QStringLiteral("%1 Connecting to %2")
                        .arg(contextTag(ctx.accountName), url.toString(QUrl::RemoveUserInfo)));
    ctx.privateSocket.open(url);
}

void TradeManager::initializeUzxWebSocket(Context &ctx)
{
    if (ctx.privateSocket.state() != QAbstractSocket::UnconnectedState) {
        ctx.closingSocket = true;
        ctx.privateSocket.close();
    }
    QUrl url(QStringLiteral("wss://stream.uzx.com/notification/pri/ws"));
    emit logMessage(QStringLiteral("%1 Connecting to %2")
                        .arg(contextTag(ctx.accountName), url.toString(QUrl::RemoveUserInfo)));
    ctx.privateSocket.open(url);
}
void TradeManager::subscribePrivateChannels(Context &ctx)
{
    if (ctx.profile == ConnectionStore::Profile::UzxSwap
        || ctx.profile == ConnectionStore::Profile::UzxSpot) {
        subscribeUzxPrivate(ctx);
        return;
    }
    if (ctx.privateSocket.state() != QAbstractSocket::ConnectedState || ctx.hasSubscribed) {
        return;
    }
    const QStringList channels{QStringLiteral("spot@private.orders.v3.api.pb"),
                               QStringLiteral("spot@private.deals.v3.api.pb"),
                               QStringLiteral("spot@private.account.v3.api.pb")};
    QJsonObject payload;
    payload.insert(QStringLiteral("method"), QStringLiteral("SUBSCRIPTION"));
    payload.insert(QStringLiteral("params"), QJsonArray::fromStringList(channels));
    payload.insert(QStringLiteral("id"), 1);
    const QString message =
        QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    ctx.privateSocket.sendTextMessage(message);
    emit logMessage(QStringLiteral("%1 Subscribed to private channels.").arg(contextTag(ctx.accountName)));
    ctx.hasSubscribed = true;
    if (!ctx.openOrdersTimer.isActive()) {
        fetchOpenOrders(ctx);
        ctx.openOrdersTimer.start();
    }
}

void TradeManager::subscribeUzxPrivate(Context &ctx)
{
    if (ctx.privateSocket.state() != QAbstractSocket::ConnectedState) {
        return;
    }
    const QString path = QStringLiteral("/notification/pri/ws");
    const QString method = QStringLiteral("GET");
    const QString ts = QString::number(QDateTime::currentSecsSinceEpoch());
    const QString sign = QMessageAuthenticationCode::hash((ts + method + path).toUtf8(),
                                                          ctx.credentials.secretKey.toUtf8(),
                                                          QCryptographicHash::Sha256)
                             .toBase64();

    QJsonObject loginParams;
    loginParams.insert(QStringLiteral("type"), QStringLiteral("api"));
    loginParams.insert(QStringLiteral("api_key"), ctx.credentials.apiKey);
    loginParams.insert(QStringLiteral("api_timestamp"), ts);
    loginParams.insert(QStringLiteral("api_sign"), sign);
    loginParams.insert(QStringLiteral("api_passphrase"), ctx.credentials.passphrase);
    QJsonObject loginPayload;
    loginPayload.insert(QStringLiteral("event"), QStringLiteral("login"));
    loginPayload.insert(QStringLiteral("params"), loginParams);
    const QString loginMsg =
        QString::fromUtf8(QJsonDocument(loginPayload).toJson(QJsonDocument::Compact));
    ctx.privateSocket.sendTextMessage(loginMsg);
    emit logMessage(QStringLiteral("%1 Sent UZX login.").arg(contextTag(ctx.accountName)));
    ctx.hasSubscribed = false;
}

void TradeManager::sendListenKeyKeepAlive(Context &ctx)
{
    if (ctx.profile == ConnectionStore::Profile::UzxSwap
        || ctx.profile == ConnectionStore::Profile::UzxSpot
        || ctx.profile == ConnectionStore::Profile::MexcFutures) {
        return;
    }
    if (ctx.listenKey.isEmpty()) {
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("listenKey"), ctx.listenKey);
    QNetworkRequest request = makePrivateRequest(QStringLiteral("/api/v3/userDataStream"),
                                                 query,
                                                 QByteArray(),
                                                 ctx);
    auto *reply = ensureMexcNetwork(ctx)->put(request, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        const QNetworkReply::NetworkError err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            const QString message = raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw);
            emit logMessage(QStringLiteral("%1 Keepalive failed: %2")
                                .arg(contextTag(ctxPtr->accountName), message));
            scheduleReconnect(*ctxPtr);
        } else {
            emit logMessage(QStringLiteral("%1 Listen key refreshed.")
                                .arg(contextTag(ctxPtr->accountName)));
        }
    });
}

void TradeManager::closeWebSocket(Context &ctx)
{
    if (ctx.reconnectTimer.isActive()) {
        ctx.reconnectTimer.stop();
    }
    ctx.keepAliveTimer.stop();
    ctx.wsPingTimer.stop();
    ctx.hasSubscribed = false;
    ctx.futuresLoggedIn = false;
    if (ctx.privateSocket.state() != QAbstractSocket::UnconnectedState) {
        ctx.closingSocket = true;
        ctx.privateSocket.close();
    }
    if (ctx.lighterStreamSocket.state() != QAbstractSocket::UnconnectedState) {
        ctx.lighterStreamSocket.close();
    }
    ctx.lighterStreamConnecting = false;
    ctx.lighterStreamConnected = false;
    ctx.lighterStreamReady = false;
    ctx.lighterPrivateSubscribed = false;
    ctx.lighterWsTxWaiters.clear();
}

void TradeManager::fetchOpenOrders(Context &ctx)
{
    if (ctx.profile == ConnectionStore::Profile::MexcFutures) {
        fetchFuturesOpenOrders(ctx);
        return;
    }
    if (ctx.profile == ConnectionStore::Profile::UzxSwap
        || ctx.profile == ConnectionStore::Profile::UzxSpot) {
        fetchUzxOpenOrders(ctx);
        return;
    }
    if (ctx.openOrdersPending) {
        return;
    }
    ctx.openOrdersPending = true;
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    query.addQueryItem(QStringLiteral("timestamp"),
                       QString::number(QDateTime::currentMSecsSinceEpoch()));
    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"),
                             QString::fromLatin1(signPayload(query, ctx)));
    QNetworkRequest req = makePrivateRequest(QStringLiteral("/api/v3/openOrders"),
                                             signedQuery,
                                             QByteArray(),
                                             ctx);
    auto *reply = ensureLighterNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        ctxPtr->openOrdersPending = false;
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        logTradeManagerEvent(QStringLiteral("[tm] fetchOpenOrders profile=%1 account=%2 status=%3 payload=%4")
                                 .arg(static_cast<int>(ctxPtr->profile))
                                 .arg(ctxPtr->accountName)
                                 .arg(err)
                                 .arg(QString::fromLatin1(raw.left(512))));
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            emit logMessage(QStringLiteral("%1 openOrders fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isArray()) {
            return;
        }
        QHash<QString, QVector<DomWidget::LocalOrderMarker>> symbolMap;
        QSet<QString> newSymbols;
        QHash<QString, OrderRecord> fetchedOrders;
        QSet<QString> fetchedSymbols;
        const QJsonArray arr = doc.array();
        for (const auto &value : arr) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject order = value.toObject();
            const QString symbol = normalizedSymbol(order.value(QStringLiteral("symbol")).toString());
            if (symbol.isEmpty()) {
                continue;
            }
            const QString orderId =
                order.value(QStringLiteral("orderId")).toString().trimmed();
            if (orderId.isEmpty()) {
                continue;
            }
            const double price = order.value(QStringLiteral("price")).toString().toDouble();
            const double origQty = order.value(QStringLiteral("origQty")).toString().toDouble();
            const double execQty = order.value(QStringLiteral("executedQty")).toString().toDouble();
            const double remainQty = origQty - execQty;
            if (price <= 0.0 || remainQty <= 0.0) {
                continue;
            }
            DomWidget::LocalOrderMarker marker;
            marker.price = price;
            marker.quantity = std::abs(price * remainQty);
            const QString side = order.value(QStringLiteral("side")).toString();
            marker.side = side.compare(QStringLiteral("SELL"), Qt::CaseInsensitive) == 0 ? OrderSide::Sell
                                                                                         : OrderSide::Buy;
            marker.createdMs = order.value(QStringLiteral("time")).toVariant().toLongLong();
            marker.orderId = orderId;
            symbolMap[symbol].push_back(marker);
            newSymbols.insert(symbol);
            fetchedSymbols.insert(symbol);

            OrderRecord record;
            record.symbol = symbol;
            record.price = price;
            record.quantityNotional = std::abs(price * remainQty);
            record.side = marker.side;
            record.createdMs = marker.createdMs;
            record.orderId = orderId;
            fetchedOrders.insert(orderId, record);
        }
        QList<OrderRecord> removedOrders;
        for (auto it = ctxPtr->activeOrders.constBegin(); it != ctxPtr->activeOrders.constEnd();
             ++it) {
            if (!fetchedOrders.contains(it.key())) {
                OrderRecord record = it.value();
                record.orderId = it.key();
                removedOrders.push_back(record);
            }
        }
        ctxPtr->activeOrders = fetchedOrders;
        for (const auto &record : removedOrders) {
            emit orderCanceled(ctxPtr->accountName, record.symbol, record.side, record.price, record.orderId);
        }
        for (auto it = ctxPtr->pendingCancelSymbols.begin(); it != ctxPtr->pendingCancelSymbols.end();) {
            if (!fetchedSymbols.contains(*it)) {
                it = ctxPtr->pendingCancelSymbols.erase(it);
            } else {
                ++it;
            }
        }
        QSet<QString> allSymbols = ctxPtr->trackedSymbols;
        allSymbols.unite(newSymbols);
        for (const QString &symbol : allSymbols) {
            QVector<DomWidget::LocalOrderMarker> output;
            if (!ctxPtr->pendingCancelSymbols.contains(symbol)) {
                output = symbolMap.value(symbol);
            }
            emit localOrdersUpdated(ctxPtr->accountName, symbol, output);
        }
        ctxPtr->trackedSymbols = newSymbols;
    });
}

void TradeManager::fetchUzxOpenOrders(Context &ctx)
{
    if (ctx.openOrdersPending) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    ctx.openOrdersPending = true;

    const bool isSwap = (ctx.profile == ConnectionStore::Profile::UzxSwap);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ins_type"), isSwap ? QStringLiteral("2") : QStringLiteral("1"));
    QNetworkRequest req =
        makeUzxRequest(QStringLiteral("/v2/trade/orders"), QByteArray(), QStringLiteral("GET"), query, ctx);
    auto *reply = m_network.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        ctxPtr->openOrdersPending = false;
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            emit logMessage(QStringLiteral("%1 UZX open orders fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            return;
        }

        const auto toDouble = [](const QJsonValue &v) -> double {
            if (v.isString()) return v.toString().toDouble();
            if (v.isDouble()) return v.toDouble();
            return v.toVariant().toDouble();
        };
        const auto toLongLong = [](const QJsonValue &v) -> qint64 {
            if (v.isString()) return v.toString().toLongLong();
            if (v.isDouble()) return static_cast<qint64>(v.toDouble());
            return v.toVariant().toLongLong();
        };
        auto orderIdFromValue = [](const QJsonValue &v) -> QString {
            if (v.isString()) return v.toString().trimmed();
            if (v.isDouble()) return QString::number(static_cast<qlonglong>(v.toDouble()));
            return v.toVariant().toString().trimmed();
        };

        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (code != 0 && code != 200) {
            const QString rawMsg =
                obj.value(QStringLiteral("msg")).toString(QStringLiteral("request error"));
            emit logMessage(QStringLiteral("%1 UZX open orders rejected: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     uzxFormatError(code, rawMsg)));
            return;
        }

        const QJsonArray arr = obj.value(QStringLiteral("data")).toArray();
        QHash<QString, QVector<DomWidget::LocalOrderMarker>> symbolMap;
        QSet<QString> newSymbols;
        QHash<QString, OrderRecord> fetchedOrders;
        QSet<QString> fetchedSymbols;

        for (const auto &val : arr) {
            if (!val.isObject()) {
                continue;
            }
            const QJsonObject o = val.toObject();
            QString wire = o.value(QStringLiteral("product_name")).toString().trimmed().toUpper();
            if (wire.isEmpty()) {
                wire = o.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
            }
            const QString symbol = normalizedSymbol(uzxUserSymbol(wire));
            if (symbol.isEmpty()) {
                continue;
            }
            const QString orderId =
                orderIdFromValue(o.value(QStringLiteral("order_id")).isUndefined()
                                     ? o.value(QStringLiteral("orderId"))
                                     : o.value(QStringLiteral("order_id")));
            if (orderId.isEmpty()) {
                continue;
            }
            const double price = toDouble(o.value(QStringLiteral("price")));
            if (!(price > 0.0)) {
                continue;
            }
            double remain = toDouble(o.value(QStringLiteral("un_filled_amount")));
            if (!(remain > 0.0)) {
                remain = toDouble(o.value(QStringLiteral("un_filled_number")));
            }
            if (!(remain > 0.0)) {
                remain = toDouble(o.value(QStringLiteral("amount")));
                if (!(remain > 0.0)) {
                    remain = toDouble(o.value(QStringLiteral("number")));
                }
            }
            if (!(remain > 0.0)) {
                continue;
            }
            const int sideFlag = o.value(QStringLiteral("order_buy_or_sell")).toInt(1);
            const OrderSide side = sideFlag == 2 ? OrderSide::Sell : OrderSide::Buy;
            const qint64 createdMs =
                std::max(toLongLong(o.value(QStringLiteral("updated_at"))),
                         toLongLong(o.value(QStringLiteral("created_at"))));

            DomWidget::LocalOrderMarker marker;
            marker.price = price;
            marker.quantity = std::abs(price * remain);
            marker.side = side;
            marker.createdMs = createdMs > 0 ? createdMs : QDateTime::currentMSecsSinceEpoch();
            marker.orderId = orderId;
            symbolMap[symbol].push_back(marker);
            newSymbols.insert(symbol);
            fetchedSymbols.insert(symbol);

            OrderRecord record;
            record.symbol = symbol;
            record.price = price;
            record.quantityNotional = marker.quantity;
            record.side = side;
            record.createdMs = marker.createdMs;
            record.orderId = orderId;
            fetchedOrders.insert(orderId, record);
        }

        QList<OrderRecord> removedOrders;
        for (auto it = ctxPtr->activeOrders.constBegin(); it != ctxPtr->activeOrders.constEnd(); ++it) {
            if (!fetchedOrders.contains(it.key())) {
                OrderRecord record = it.value();
                record.orderId = it.key();
                removedOrders.push_back(record);
            }
        }
        ctxPtr->activeOrders = fetchedOrders;
        for (const auto &record : removedOrders) {
            emit orderCanceled(ctxPtr->accountName, record.symbol, record.side, record.price, record.orderId);
        }
        for (auto it = ctxPtr->pendingCancelSymbols.begin(); it != ctxPtr->pendingCancelSymbols.end();) {
            if (!fetchedSymbols.contains(*it)) {
                it = ctxPtr->pendingCancelSymbols.erase(it);
            } else {
                ++it;
            }
        }

        QSet<QString> allSymbols = ctxPtr->trackedSymbols;
        allSymbols.unite(newSymbols);
        for (const QString &symbol : allSymbols) {
            QVector<DomWidget::LocalOrderMarker> output;
            if (!ctxPtr->pendingCancelSymbols.contains(symbol)) {
                output = symbolMap.value(symbol);
            }
            emit localOrdersUpdated(ctxPtr->accountName, symbol, output);
        }
        ctxPtr->trackedSymbols = newSymbols;
    });
}

void TradeManager::fetchUzxSwapPositions(Context &ctx)
{
    if (ctx.futuresPositionsPending) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    ctx.futuresPositionsPending = true;
    fetchUzxSwapPositionsPath(ctx, QStringLiteral("/v2/trade/swap/position"), true);
}

void TradeManager::fetchUzxSwapPositionsPath(Context &ctx, const QString &path, bool allowFallback)
{
    QNetworkRequest req = makeUzxRequest(path, QByteArray(), QStringLiteral("GET"), QUrlQuery(), ctx);
    auto *reply = m_network.get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, path, allowFallback]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            if (allowFallback && path == QStringLiteral("/v2/trade/swap/position")) {
                fetchUzxSwapPositionsPath(*ctxPtr, QStringLiteral("/v2/trade/swap/positions"), false);
                return;
            }
            emit logMessage(QStringLiteral("%1 UZX positions fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            ctxPtr->futuresPositionsPending = false;
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            ctxPtr->futuresPositionsPending = false;
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (code != 0 && code != 200) {
            const QString rawMsg =
                obj.value(QStringLiteral("msg")).toString(QStringLiteral("request error"));
            if (allowFallback && path == QStringLiteral("/v2/trade/swap/position")) {
                fetchUzxSwapPositionsPath(*ctxPtr, QStringLiteral("/v2/trade/swap/positions"), false);
                return;
            }
            emit logMessage(QStringLiteral("%1 UZX positions rejected: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     uzxFormatError(code, rawMsg)));
            ctxPtr->futuresPositionsPending = false;
            return;
        }

        QJsonArray arr;
        const QJsonValue dataVal = obj.value(QStringLiteral("data"));
        if (dataVal.isArray()) {
            arr = dataVal.toArray();
        } else if (dataVal.isObject()) {
            const QJsonObject dataObj = dataVal.toObject();
            arr = dataObj.value(QStringLiteral("positions")).toArray();
            if (arr.isEmpty()) {
                arr = dataObj.value(QStringLiteral("data")).toArray();
            }
        }
        const auto toDouble = [](const QJsonValue &v) -> double {
            if (v.isString()) return v.toString().toDouble();
            if (v.isDouble()) return v.toDouble();
            return v.toVariant().toDouble();
        };
        const auto toString = [](const QJsonValue &v) -> QString {
            if (v.isString()) return v.toString();
            return v.toVariant().toString();
        };

        QSet<QString> updatedSymbols;
        for (const auto &val : arr) {
            if (!val.isObject()) {
                continue;
            }
            const QJsonObject p = val.toObject();
            QString wire = p.value(QStringLiteral("product_name")).toString().trimmed().toUpper();
            if (wire.isEmpty()) {
                wire = p.value(QStringLiteral("symbol")).toString().trimmed().toUpper();
            }
            const QString sym = normalizedSymbol(uzxUserSymbol(wire));
            if (sym.isEmpty()) {
                continue;
            }
            const QString sideStr = toString(p.value(QStringLiteral("pos_side"))).trimmed().toUpper();
            const OrderSide side =
                (sideStr == QStringLiteral("ST") || sideStr == QStringLiteral("SHORT"))
                    ? OrderSide::Sell
                    : OrderSide::Buy;
            double qty = toDouble(p.value(QStringLiteral("holdVol")));
            if (!(qty > 0.0)) qty = toDouble(p.value(QStringLiteral("quantity")));
            if (!(qty > 0.0)) qty = toDouble(p.value(QStringLiteral("pos")));
            if (!(qty > 0.0)) qty = toDouble(p.value(QStringLiteral("amount")));
            if (!(qty > 0.0)) {
                continue;
            }
            double avg = toDouble(p.value(QStringLiteral("avg_entry_price")));
            if (!(avg > 0.0)) avg = toDouble(p.value(QStringLiteral("holdAvgPrice")));
            if (!(avg > 0.0)) avg = toDouble(p.value(QStringLiteral("avg_price")));
            if (!(avg > 0.0)) avg = toDouble(p.value(QStringLiteral("open_avg_price")));

            TradePosition pos;
            pos.hasPosition = true;
            pos.side = side;
            pos.quantity = qty;
            pos.averagePrice = avg;
            pos.qtyMultiplier = 1.0;
            pos.realizedPnl = toDouble(p.value(QStringLiteral("realized_pnl")));
            ctxPtr->positions.insert(sym, pos);
            updatedSymbols.insert(sym);
            emitPositionChanged(*ctxPtr, sym);
        }

        for (auto it = ctxPtr->positions.begin(); it != ctxPtr->positions.end();) {
            const QString key = it.key();
            if (!updatedSymbols.contains(key) && it.value().hasPosition && it.value().quantity > 0.0) {
                it = ctxPtr->positions.erase(it);
                emitPositionChanged(*ctxPtr, key);
            } else {
                ++it;
            }
        }

        ctxPtr->futuresPositionsPending = false;
    });
}

void TradeManager::fetchFuturesOpenOrders(Context &ctx)
{
    if (ctx.openOrdersPending) {
        return;
    }
    ctx.openOrdersPending = true;
    QNetworkRequest req = makeFuturesRequest(QStringLiteral("/private/order/list/open_orders"),
                                             ctx);
    auto *reply = ensureMexcNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        ctxPtr->openOrdersPending = false;
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            emit logMessage(QStringLiteral("%1 Futures open orders fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isNull() && doc.isObject()) {
            const QJsonObject obj = doc.object();
            const bool success = obj.value(QStringLiteral("success")).toBool(true);
            const int code = obj.value(QStringLiteral("code")).toInt(0);
            if (!success || code != 0) {
                const QString errMsg =
                    obj.value(QStringLiteral("message"))
                        .toString(obj.value(QStringLiteral("msg")).toString(reply->errorString()));
                emit logMessage(QStringLiteral("%1 Futures open orders rejected: %2 (code %3)")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(errMsg)
                                    .arg(code));
                return;
            }
        }
        QJsonArray arr;
        if (doc.isObject()) {
            const QJsonValue dataVal = doc.object().value(QStringLiteral("data"));
            if (dataVal.isArray()) {
                arr = dataVal.toArray();
            } else if (dataVal.isObject()) {
                arr = dataVal.toObject().value(QStringLiteral("orders")).toArray();
            }
        } else if (doc.isArray()) {
            arr = doc.array();
        }
        auto toDouble = [](const QJsonValue &value) -> double {
            if (value.isDouble()) {
                return value.toDouble();
            }
            if (value.isString()) {
                return value.toString().toDouble();
            }
            return value.toVariant().toDouble();
        };
        QHash<QString, QVector<DomWidget::LocalOrderMarker>> symbolMap;
        QSet<QString> newSymbols;
        QHash<QString, OrderRecord> fetchedOrders;
        QSet<QString> fetchedSymbols;
        for (const auto &value : arr) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject order = value.toObject();
            const QString symbol = normalizedSymbol(order.value(QStringLiteral("symbol")).toString());
            if (symbol.isEmpty()) {
                continue;
            }
            const auto metaIt = ctxPtr->futuresContractMeta.constFind(symbol);
            const double contractSize =
                (metaIt != ctxPtr->futuresContractMeta.constEnd() && metaIt->valid())
                    ? metaIt->contractSize
                    : 1.0;
            if (contractSize <= 0.0) {
                // Best-effort fallback; a proper contractSize will be fetched when placing orders.
                // If we don't have it yet, markers may temporarily look smaller until next refresh.
            }
            QString orderId = order.value(QStringLiteral("orderId")).toVariant().toString();
            if (orderId.isEmpty()) {
                orderId = order.value(QStringLiteral("id")).toVariant().toString();
            }
            if (orderId.isEmpty()) {
                continue;
            }
            const double price = toDouble(order.value(QStringLiteral("price")));
            const double vol = toDouble(order.value(QStringLiteral("vol")));
            const double dealVol = toDouble(order.value(QStringLiteral("dealVol")));
            const double remainContracts = std::max(0.0, vol - dealVol);
            const double remainBase = remainContracts * contractSize;
            if (price <= 0.0 || remainContracts <= 0.0) {
                continue;
            }
            DomWidget::LocalOrderMarker marker;
            marker.price = price;
            marker.quantity = std::abs(price * remainBase);
            const int sideCode = order.value(QStringLiteral("side")).toInt(1);
            marker.side =
                (sideCode == 3 || sideCode == 4) ? OrderSide::Sell : OrderSide::Buy;
            marker.createdMs =
                order.value(QStringLiteral("createTime")).toVariant().toLongLong();
            if (marker.createdMs == 0) {
                marker.createdMs = QDateTime::currentMSecsSinceEpoch();
            }
            marker.orderId = orderId;
            symbolMap[symbol].push_back(marker);
            newSymbols.insert(symbol);
            fetchedSymbols.insert(symbol);

            OrderRecord record;
            record.symbol = symbol;
            record.price = price;
            record.quantityNotional = marker.quantity;
            record.side = marker.side;
            record.createdMs = marker.createdMs;
            record.orderId = orderId;
            fetchedOrders.insert(orderId, record);
        }
        QList<OrderRecord> removedOrders;
        for (auto it = ctxPtr->activeOrders.constBegin(); it != ctxPtr->activeOrders.constEnd();
             ++it) {
            if (!fetchedOrders.contains(it.key())) {
                OrderRecord record = it.value();
                record.orderId = it.key();
                removedOrders.push_back(record);
            }
        }
        ctxPtr->activeOrders = fetchedOrders;
        for (const auto &record : removedOrders) {
            emit orderCanceled(ctxPtr->accountName, record.symbol, record.side, record.price, record.orderId);
        }
        for (auto it = ctxPtr->pendingCancelSymbols.begin(); it != ctxPtr->pendingCancelSymbols.end();) {
            if (!fetchedSymbols.contains(*it)) {
                it = ctxPtr->pendingCancelSymbols.erase(it);
            } else {
                ++it;
            }
        }
        QSet<QString> allSymbols = ctxPtr->trackedSymbols;
        allSymbols.unite(newSymbols);
        for (const QString &symbol : allSymbols) {
            QVector<DomWidget::LocalOrderMarker> output;
            if (!ctxPtr->pendingCancelSymbols.contains(symbol)) {
                output = symbolMap.value(symbol);
            }
            emit localOrdersUpdated(ctxPtr->accountName, symbol, output);
        }
        ctxPtr->trackedSymbols = newSymbols;
        fetchFuturesPositions(*ctxPtr);
    });
}

void TradeManager::fetchFuturesPositions(Context &ctx)
{
    if (ctx.futuresPositionsPending) {
        return;
    }
    ctx.futuresPositionsPending = true;
    QNetworkRequest req = makeFuturesRequest(QStringLiteral("/private/position/open_positions"),
                                             ctx);
    auto *reply = ensureMexcNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        ctxPtr->futuresPositionsPending = false;
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError) {
            emit logMessage(QStringLiteral("%1 Futures positions fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            return;
        }
        QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isNull() && doc.isObject()) {
            const QJsonObject obj = doc.object();
            const bool success = obj.value(QStringLiteral("success")).toBool(true);
            const int code = obj.value(QStringLiteral("code")).toInt(0);
            if (!success || code != 0) {
                const QString errMsg =
                    obj.value(QStringLiteral("message"))
                        .toString(obj.value(QStringLiteral("msg")).toString(reply->errorString()));
                emit logMessage(QStringLiteral("%1 Futures positions rejected: %2 (code %3)")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(errMsg)
                                    .arg(code));
                return;
            }
        }
        QJsonArray arr;
        if (doc.isObject()) {
            arr = doc.object().value(QStringLiteral("data")).toArray();
        } else if (doc.isArray()) {
            arr = doc.array();
        }
        auto toDouble = [](const QJsonValue &value) -> double {
            if (value.isDouble()) {
                return value.toDouble();
            }
            if (value.isString()) {
                return value.toString().toDouble();
            }
            return value.toVariant().toDouble();
        };
        auto toLongLong = [](const QJsonValue &value) -> qint64 {
            if (value.isDouble()) {
                return static_cast<qint64>(value.toDouble());
            }
            if (value.isString()) {
                return value.toString().toLongLong();
            }
            return value.toVariant().toLongLong();
        };
        QSet<QString> updatedSymbols;
        for (const auto &value : arr) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject obj = value.toObject();
            const QString symbol = normalizedSymbol(obj.value(QStringLiteral("symbol")).toString());
            if (symbol.isEmpty()) {
                continue;
            }
            const double holdVol = toDouble(obj.value(QStringLiteral("holdVol")));
            if (holdVol <= 0.0) {
                continue;
            }
            TradePosition pos;
            pos.hasPosition = true;
            pos.quantity = holdVol;
            pos.qtyMultiplier = 1.0;
            pos.averagePrice = toDouble(obj.value(QStringLiteral("holdAvgPrice")));
            pos.realizedPnl = toDouble(obj.value(QStringLiteral("realised")));
            pos.positionId = toLongLong(obj.value(QStringLiteral("positionId")));
            pos.openType = obj.value(QStringLiteral("openType")).toInt(1);
            pos.leverage = obj.value(QStringLiteral("leverage")).toInt(0);
            const int positionType = obj.value(QStringLiteral("positionType")).toInt(1);
            pos.side = positionType == 2 ? OrderSide::Sell : OrderSide::Buy;
            // Meta is needed for correct PnL/value display (contractSize).
            auto metaIt = ctxPtr->futuresContractMeta.constFind(symbol);
            if (metaIt != ctxPtr->futuresContractMeta.constEnd() && metaIt->valid()) {
                pos.qtyMultiplier = metaIt->contractSize;
            } else {
                ensureFuturesContractMeta(*ctxPtr, symbol);
            }
            ctxPtr->positions.insert(symbol, pos);
            emitPositionChanged(*ctxPtr, symbol);
            updatedSymbols.insert(symbol);
        }
        for (auto it = ctxPtr->positions.begin(); it != ctxPtr->positions.end();) {
            if (!updatedSymbols.contains(it.key())) {
                if (it.value().hasPosition) {
                    TradePosition empty;
                    it.value() = empty;
                    emitPositionChanged(*ctxPtr, it.key());
                }
                it = ctxPtr->positions.erase(it);
            } else {
                ++it;
            }
        }
    });
}

void TradeManager::fetchLighterAccount(Context &ctx)
{
    if (ctx.lighterAccountPending) {
        return;
    }
    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    if (baseUrl.isEmpty()) {
        return;
    }
    ctx.lighterAccountPending = true;
    QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/account"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("by"), QStringLiteral("index"));
    q.addQueryItem(QStringLiteral("value"), QString::number(ctx.credentials.accountIndex));
    if (!ctx.lighterAuthToken.isEmpty()) {
        q.addQueryItem(QStringLiteral("auth"), ctx.lighterAuthToken);
    }
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!ctx.lighterAuthToken.isEmpty()) {
        req.setRawHeader("authorization", ctx.lighterAuthToken.toUtf8());
    }
    auto *reply = ensureLighterNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx]() {
        ctxPtr->lighterAccountPending = false;
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        auto parseErr = [](const QByteArray &body, int *outCode, QString *outMsg) {
            if (outCode) {
                *outCode = 0;
            }
            if (outMsg) {
                outMsg->clear();
            }
            if (body.isEmpty()) {
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (!doc.isObject()) {
                return;
            }
            const QJsonObject obj = doc.object();
            if (outCode) {
                *outCode = obj.value(QStringLiteral("code")).toInt(0);
            }
            if (outMsg) {
                *outMsg = obj.value(QStringLiteral("message")).toString();
            }
        };
        auto applyBackoff = [this, ctxPtr]() {
            if (ctxPtr->lighterBurstTimer.isActive()) {
                ctxPtr->lighterBurstRemaining = 0;
                ctxPtr->lighterBurstTimer.stop();
            }
            const int next = ctxPtr->lighterAccountBackoffMs > 0
                                 ? std::min(ctxPtr->lighterAccountBackoffMs * 2, kLighterPollMaxBackoffMs)
                                 : std::min(kLighterAccountPollMs * 2, kLighterPollMaxBackoffMs);
            if (ctxPtr->lighterAccountBackoffMs != next) {
                ctxPtr->lighterAccountBackoffMs = next;
                ctxPtr->lighterAccountTimer.setInterval(next);
                emit logMessage(QStringLiteral("%1 Lighter account rate limited; backing off to %2ms")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(next));
            }
        };
        if (err != QNetworkReply::NoError || status >= 400) {
            int code = 0;
            QString msg;
            parseErr(raw, &code, &msg);
            if (code == 23000 || msg.contains(QStringLiteral("Too Many Requests"), Qt::CaseInsensitive)) {
                applyBackoff();
            }
            emit logMessage(QStringLiteral("%1 Lighter account fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isNull() || !doc.isObject()) {
            emit logMessage(QStringLiteral("%1 Lighter account parse failed").arg(contextTag(ctxPtr->accountName)));
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (code != 200) {
            const QString msg = obj.value(QStringLiteral("message")).toString(QStringLiteral("Account error"));
            if (code == 23000 || msg.contains(QStringLiteral("Too Many Requests"), Qt::CaseInsensitive)) {
                applyBackoff();
            }
            emit logMessage(QStringLiteral("%1 Lighter account error: %2").arg(contextTag(ctxPtr->accountName), msg));
            return;
        }
        if (ctxPtr->lighterAccountBackoffMs > 0) {
            ctxPtr->lighterAccountBackoffMs = 0;
            ctxPtr->lighterAccountTimer.setInterval(kLighterAccountPollMs);
        }

        auto toDouble = [](const QJsonValue &value) -> double {
            if (value.isDouble()) {
                return value.toDouble();
            }
            if (value.isString()) {
                return value.toString().toDouble();
            }
            return value.toVariant().toDouble();
        };
        auto toLongLong = [](const QJsonValue &value) -> qint64 {
            if (value.isDouble()) {
                return static_cast<qint64>(value.toDouble());
            }
            if (value.isString()) {
                return value.toString().toLongLong();
            }
            return value.toVariant().toLongLong();
        };

        const QJsonArray accounts = obj.value(QStringLiteral("accounts")).toArray();
        if (accounts.isEmpty()) {
            return;
        }
        const qint64 targetIndex = static_cast<qint64>(ctxPtr->credentials.accountIndex);
        QJsonObject accountObj;
        for (const auto &value : accounts) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject cand = value.toObject();
            const qint64 idx = toLongLong(cand.value(QStringLiteral("index")));
            const qint64 idx2 = toLongLong(cand.value(QStringLiteral("account_index")));
            if (idx == targetIndex || idx2 == targetIndex) {
                accountObj = cand;
                break;
            }
        }
        if (accountObj.isEmpty()) {
            accountObj = accounts.first().toObject();
        }

        const QJsonArray positions = accountObj.value(QStringLiteral("positions")).toArray();
        QSet<QString> updatedSymbols;
        for (const auto &value : positions) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject p = value.toObject();
            const QString symbol = normalizedSymbol(p.value(QStringLiteral("symbol")).toString());
            if (symbol.isEmpty()) {
                continue;
            }
            const double positionRaw = toDouble(p.value(QStringLiteral("position")));
            if (std::abs(positionRaw) <= 0.0) {
                continue;
            }
            const int sign = p.value(QStringLiteral("sign")).toInt(positionRaw >= 0.0 ? 1 : -1);
            TradePosition pos;
            pos.hasPosition = true;
            pos.side = sign < 0 ? OrderSide::Sell : OrderSide::Buy;
            pos.quantity = std::abs(positionRaw);
            pos.qtyMultiplier = 1.0;
            pos.averagePrice = toDouble(p.value(QStringLiteral("avg_entry_price")));
            pos.realizedPnl = toDouble(p.value(QStringLiteral("realized_pnl")));
            const double imf = toDouble(p.value(QStringLiteral("initial_margin_fraction")));
            if (imf > 0.0) {
                const double levRaw = (imf > 100.0) ? (10000.0 / imf) : (100.0 / imf);
                pos.leverage = std::max(1, static_cast<int>(std::round(levRaw)));
            }
            ctxPtr->positions.insert(symbol, pos);
            emitPositionChanged(*ctxPtr, symbol);
            updatedSymbols.insert(symbol);
        }

        for (auto it = ctxPtr->positions.begin(); it != ctxPtr->positions.end();) {
            if (!updatedSymbols.contains(it.key())) {
                if (it.value().hasPosition) {
                    TradePosition empty;
                    it.value() = empty;
                    emitPositionChanged(*ctxPtr, it.key());
                }
                it = ctxPtr->positions.erase(it);
            } else {
                ++it;
            }
        }
    });
}

void TradeManager::fetchLighterTrades(Context &ctx)
{
    if (ctx.lighterTradesDisabled) {
        return;
    }
    if (ctx.lighterTradesPending) {
        return;
    }
    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    if (baseUrl.isEmpty()) {
        return;
    }
    if (ctx.lighterAuthToken.isEmpty()) {
        return;
    }

        auto &cache = lighterMetaCacheByUrl()[baseUrl];
        if (cache.bySymbol.isEmpty()) {
            if (cache.inFlight) {
                return;
            }
            cache.inFlight = true;
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookDetails"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("filter"), QStringLiteral("perp"));
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = ensureLighterNetwork(ctx)->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, baseUrl]() {
                auto &cache2 = lighterMetaCacheByUrl()[baseUrl];
                cache2.inFlight = false;
            const QNetworkReply::NetworkError err = reply->error();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray raw = reply->readAll();
            reply->deleteLater();
            if (err != QNetworkReply::NoError || status >= 400) {
                emit logMessage(QStringLiteral("%1 Lighter markets fetch failed: %2")
                                    .arg(contextTag(ctxPtr->accountName),
                                         raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (doc.isNull() || !doc.isObject()) {
                emit logMessage(QStringLiteral("%1 Lighter markets parse failed").arg(contextTag(ctxPtr->accountName)));
                return;
            }
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("code")).toInt(0) != 200) {
                const QString msg = obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter markets error"));
                emit logMessage(QStringLiteral("%1 Lighter markets error: %2").arg(contextTag(ctxPtr->accountName), msg));
                return;
            }
            cache2.bySymbol.clear();
            addLighterMarketMeta(cache2.bySymbol, obj.value(QStringLiteral("order_book_details")).toArray(), false);
            addLighterMarketMeta(cache2.bySymbol, obj.value(QStringLiteral("spot_order_book_details")).toArray(), true);
            cache2.symbolByMarketId.clear();
            cache2.symbolByMarketId.reserve(cache2.bySymbol.size());
            for (auto it = cache2.bySymbol.constBegin(); it != cache2.bySymbol.constEnd(); ++it) {
                if (it.value().marketId >= 0) {
                    cache2.symbolByMarketId.insert(it.value().marketId, it.key());
                }
            }
            cache2.updatedMs = QDateTime::currentMSecsSinceEpoch();
            if (ctxPtr) {
                fetchLighterTrades(*ctxPtr);
            }
        });
        return;
    }

    ctx.lighterTradesPending = true;
    QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/trades"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("account_index"), QString::number(ctx.credentials.accountIndex));
    // Lighter API expects market_id; 255 means "all markets" (default per docs) but some deployments reject when omitted.
    q.addQueryItem(QStringLiteral("market_id"), QStringLiteral("255"));
    q.addQueryItem(QStringLiteral("sort_by"), QStringLiteral("trade_id"));
    // On first run we only want to "sync" the cursor without replaying a large batch on the UI thread.
    // Subsequent polls will fetch incrementally via `from=lastTradeId+1`.
    q.addQueryItem(QStringLiteral("limit"), ctx.lighterLastTradeId > 0 ? QStringLiteral("100")
                                                                      : QStringLiteral("1"));
    if (ctx.lighterLastTradeId > 0) {
        q.addQueryItem(QStringLiteral("sort_dir"), QStringLiteral("asc"));
        q.addQueryItem(QStringLiteral("from"), QString::number(ctx.lighterLastTradeId + 1));
    } else {
        q.addQueryItem(QStringLiteral("sort_dir"), QStringLiteral("desc"));
    }
    if (!ctx.lighterAuthToken.isEmpty()) {
        q.addQueryItem(QStringLiteral("auth"), ctx.lighterAuthToken);
    }
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    if (!ctx.lighterAuthToken.isEmpty()) {
        req.setRawHeader("authorization", ctx.lighterAuthToken.toUtf8());
    }
    auto *reply = ensureLighterNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, baseUrl]() {
        ctxPtr->lighterTradesPending = false;
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        auto parseErr = [](const QByteArray &body, int *outCode, QString *outMsg) {
            if (outCode) {
                *outCode = 0;
            }
            if (outMsg) {
                outMsg->clear();
            }
            if (body.isEmpty()) {
                return;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (!doc.isObject()) {
                return;
            }
            const QJsonObject obj = doc.object();
            if (outCode) {
                *outCode = obj.value(QStringLiteral("code")).toInt(0);
            }
            if (outMsg) {
                *outMsg = obj.value(QStringLiteral("message")).toString();
            }
        };
        auto applyBackoff = [this, ctxPtr]() {
            if (ctxPtr->lighterBurstTimer.isActive()) {
                ctxPtr->lighterBurstRemaining = 0;
                ctxPtr->lighterBurstTimer.stop();
            }
            const int next = ctxPtr->lighterTradesBackoffMs > 0
                                 ? std::min(ctxPtr->lighterTradesBackoffMs * 2, kLighterPollMaxBackoffMs)
                                 : std::min(kLighterTradesPollMs * 2, kLighterPollMaxBackoffMs);
            if (ctxPtr->lighterTradesBackoffMs != next) {
                ctxPtr->lighterTradesBackoffMs = next;
                ctxPtr->lighterTradesTimer.setInterval(next);
                emit logMessage(QStringLiteral("%1 Lighter trades rate limited; backing off to %2ms")
                                    .arg(contextTag(ctxPtr->accountName))
                                    .arg(next));
            }
        };
        auto maybeRefreshAuth = [this, ctxPtr]() {
            if (!ctxPtr) {
                return;
            }
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (ctxPtr->lighterLastAuthRefreshMs > 0 && (nowMs - ctxPtr->lighterLastAuthRefreshMs) < 30000) {
                return;
            }
            ctxPtr->lighterLastAuthRefreshMs = nowMs;
            const int apiKeyIndex = ctxPtr->credentials.apiKeyIndex;
            const long long accountIndex = static_cast<long long>(ctxPtr->credentials.accountIndex);
            const LighterStrOrErr tok = lighterSigner().createAuthToken(0, apiKeyIndex, accountIndex);
            if (tok.err) {
                emit logMessage(QStringLiteral("%1 Lighter auth token refresh failed: %2")
                                    .arg(contextTag(ctxPtr->accountName), QString::fromUtf8(tok.err)));
                return;
            }
            if (tok.str) {
                ctxPtr->lighterAuthToken = QString::fromUtf8(tok.str);
                emit logMessage(QStringLiteral("%1 Lighter auth token refreshed").arg(contextTag(ctxPtr->accountName)));
            }
        };
        if (err != QNetworkReply::NoError || status >= 400) {
            int code = 0;
            QString msg;
            parseErr(raw, &code, &msg);
            if (code == 23000 || msg.contains(QStringLiteral("Too Many Requests"), Qt::CaseInsensitive)) {
                applyBackoff();
            }
            if (code == 20001 || msg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                maybeRefreshAuth();
                ctxPtr->lighterTradesBackoffMs = std::max(ctxPtr->lighterTradesBackoffMs, 15000);
                ctxPtr->lighterTradesTimer.setInterval(ctxPtr->lighterTradesBackoffMs);
                emit logMessage(QStringLiteral("%1 Lighter trades rejected (invalid param). Will retry.")
                                    .arg(contextTag(ctxPtr->accountName)));
            }
            emit logMessage(QStringLiteral("%1 Lighter trades fetch failed: %2")
                                .arg(contextTag(ctxPtr->accountName),
                                     raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw)));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isNull() || !doc.isObject()) {
            emit logMessage(QStringLiteral("%1 Lighter trades parse failed").arg(contextTag(ctxPtr->accountName)));
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (code != 200) {
            const QString msg = obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter trades error"));
            if (code == 23000 || msg.contains(QStringLiteral("Too Many Requests"), Qt::CaseInsensitive)) {
                applyBackoff();
            }
            if (code == 20001 || msg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                maybeRefreshAuth();
                ctxPtr->lighterTradesBackoffMs = std::max(ctxPtr->lighterTradesBackoffMs, 15000);
                ctxPtr->lighterTradesTimer.setInterval(ctxPtr->lighterTradesBackoffMs);
                emit logMessage(QStringLiteral("%1 Lighter trades rejected (invalid param). Will retry.")
                                    .arg(contextTag(ctxPtr->accountName)));
            }
            emit logMessage(QStringLiteral("%1 Lighter trades error: %2").arg(contextTag(ctxPtr->accountName), msg));
            return;
        }
        if (ctxPtr->lighterTradesBackoffMs > 0) {
            ctxPtr->lighterTradesBackoffMs = 0;
            ctxPtr->lighterTradesTimer.setInterval(kLighterTradesPollMs);
        }
        const QJsonArray trades = obj.value(QStringLiteral("trades")).toArray();
        if (trades.isEmpty()) {
            return;
        }
        auto toDouble = [](const QJsonValue &value) -> double {
            if (value.isDouble()) {
                return value.toDouble();
            }
            if (value.isString()) {
                return value.toString().toDouble();
            }
            return value.toVariant().toDouble();
        };
        auto toLongLong = [](const QJsonValue &value) -> qint64 {
            if (value.isDouble()) {
                return static_cast<qint64>(value.toDouble());
            }
            if (value.isString()) {
                return value.toString().toLongLong();
            }
            return value.toVariant().toLongLong();
        };

        auto &cache = lighterMetaCacheByUrl()[baseUrl];
        auto symbolForMarket = [&cache](int marketId) -> QString {
            return cache.symbolByMarketId.value(marketId);
        };

        qint64 maxTradeId = ctxPtr->lighterLastTradeId;
        const qint64 accountIndex = static_cast<qint64>(ctxPtr->credentials.accountIndex);
        if (ctxPtr->lighterLastTradeId <= 0) {
            // Cursor sync: record the newest trade_id but do not emit trades yet.
            for (const auto &value : trades) {
                if (!value.isObject()) {
                    continue;
                }
                const QJsonObject t = value.toObject();
                const qint64 tradeId = toLongLong(t.value(QStringLiteral("trade_id")));
                maxTradeId = std::max(maxTradeId, tradeId);
            }
            ctxPtr->lighterLastTradeId = maxTradeId;
            return;
        }
        for (const auto &value : trades) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject t = value.toObject();
            const qint64 tradeId = toLongLong(t.value(QStringLiteral("trade_id")));
            if (tradeId <= ctxPtr->lighterLastTradeId) {
                continue;
            }
            maxTradeId = std::max(maxTradeId, tradeId);
            const int marketId = t.value(QStringLiteral("market_id")).toInt(-1);
            const QString symbol = normalizedSymbol(symbolForMarket(marketId));
            if (symbol.isEmpty()) {
                continue;
            }
            const qint64 askAccount = toLongLong(t.value(QStringLiteral("ask_account_id")));
            const qint64 bidAccount = toLongLong(t.value(QStringLiteral("bid_account_id")));
            OrderSide side;
            bool isMaker = false;
            const bool isMakerAsk = t.value(QStringLiteral("is_maker_ask")).toBool(false);
            if (accountIndex == bidAccount) {
                side = OrderSide::Buy;
                isMaker = !isMakerAsk;
            } else if (accountIndex == askAccount) {
                side = OrderSide::Sell;
                isMaker = isMakerAsk;
            } else {
                continue;
            }

            const double price = toDouble(t.value(QStringLiteral("price")));
            const double qty = toDouble(t.value(QStringLiteral("size")));
            if (!(price > 0.0) || !(qty > 0.0)) {
                continue;
            }
            qint64 timeMs = toLongLong(t.value(QStringLiteral("timestamp")));
            if (timeMs > 0 && timeMs < 1000000000000LL) {
                timeMs *= 1000;
            }
            if (timeMs <= 0) {
                timeMs = QDateTime::currentMSecsSinceEpoch();
            }

            ExecutedTrade trade;
            trade.accountName = ctxPtr->accountName;
            trade.symbol = symbol;
            trade.side = side;
            trade.price = price;
            trade.quantity = qty;
            const double makerFee = toDouble(t.value(QStringLiteral("maker_fee")));
            const double takerFee = toDouble(t.value(QStringLiteral("taker_fee")));
            trade.feeAmount = isMaker ? makerFee : takerFee;
            trade.feeCurrency = QStringLiteral("USDC");
            trade.realizedPnl = 0.0;
            trade.realizedPct = 0.0;
            trade.timeMs = timeMs;
            appendTradeHistory(trade);
            recordFinrezTrade(*ctxPtr, trade);
            emit tradeExecuted(trade);
        }
        ctxPtr->lighterLastTradeId = maxTradeId;
    });
}

void TradeManager::resetConnection(Context &ctx, const QString &reason)
{
    closeWebSocket(ctx);
    clearLocalOrderSnapshots(ctx);
    ctx.hasSubscribed = false;
    ctx.listenKey.clear();
    setState(ctx, ConnectionState::Error, reason);
    scheduleReconnect(ctx);
}

void TradeManager::scheduleReconnect(Context &ctx)
{
    if (ctx.reconnectTimer.isActive()) {
        return;
    }
    ctx.reconnectTimer.start();
}
void TradeManager::processPrivateDeal(Context &ctx, const QByteArray &body, const QString &symbol)
{
    if (symbol.isEmpty()) {
        emit logMessage(QStringLiteral("%1 Private deal missing symbol.").arg(contextTag(ctx.accountName)));
        return;
    }
    PrivateDealEvent event;
    if (!parsePrivateDealBody(body, event)) {
        emit logMessage(QStringLiteral("%1 Failed to parse private deal.").arg(contextTag(ctx.accountName)));
        return;
    }
    if (event.quantity <= 0.0 || event.price <= 0.0) {
        return;
    }
    const QString sym = normalizedSymbol(symbol);
    const QString orderId = !event.orderId.isEmpty() ? event.orderId : event.clientOrderId;
    OrderSide side = OrderSide::Buy;
    auto existing = ctx.activeOrders.constFind(orderId);
    if (existing != ctx.activeOrders.constEnd()) {
        side = existing.value().side;
    } else if (event.tradeType == 1) {
        side = OrderSide::Buy;
    } else if (event.tradeType == 2) {
        side = OrderSide::Sell;
    }
    double closedNotional = 0.0;
    const double realizedDelta = handleOrderFill(ctx, sym, side, event.price, event.quantity, &closedNotional);
    qint64 timeMs = event.time;
    if (timeMs > 0 && timeMs < 1000000000000LL) {
        timeMs *= 1000;
    }
    if (timeMs <= 0) {
        timeMs = QDateTime::currentMSecsSinceEpoch();
    }
    ExecutedTrade trade;
    trade.accountName = ctx.accountName;
    trade.symbol = sym;
    trade.side = side;
    trade.price = event.price;
    trade.quantity = event.quantity;
    trade.feeCurrency = event.feeCurrency;
    trade.feeAmount = event.feeAmount;
    trade.realizedPnl = realizedDelta;
    trade.realizedPct = (closedNotional > 0.0) ? (realizedDelta / closedNotional) * 100.0 : 0.0;
    trade.timeMs = timeMs;
    appendTradeHistory(trade);
    recordFinrezTrade(ctx, trade);
    emit tradeExecuted(trade);
    emit logMessage(QStringLiteral("%1 Deal %2 %3 %4 @ %5 (order %6)")
                        .arg(contextTag(ctx.accountName))
                        .arg(sym)
                        .arg(side == OrderSide::Buy ? QStringLiteral("BUY")
                                                    : QStringLiteral("SELL"))
                        .arg(event.quantity, 0, 'f', 8)
                        .arg(event.price, 0, 'f', 8)
                        .arg(event.orderId));
}

void TradeManager::processPrivateOrder(Context &ctx,
                                       const QByteArray &body,
                                       const QString &symbol)
{
    PrivateOrderEvent event;
    if (!parsePrivateOrderBody(body, event)) {
        emit logMessage(QStringLiteral("%1 Failed to parse private order payload.")
                            .arg(contextTag(ctx.accountName)));
        return;
    }
    const QString identifier = !event.id.isEmpty() ? event.id : event.clientId;
    const double reportedRemain = event.remainQuantity;
    const double rawRemain = event.quantity;
    logTradeManagerEvent(QStringLiteral("[tm] privateOrder account=%1 symbol=%2 id=%3 status=%4 tradeType=%5 remain=%6 price=%7 rawRemain=%8")
                             .arg(ctx.accountName)
                             .arg(symbol)
                             .arg(identifier)
                             .arg(event.status)
                             .arg(event.tradeType)
                             .arg(reportedRemain, 0, 'f', 8)
                             .arg(event.price, 0, 'f', 8)
                             .arg(rawRemain, 0, 'f', 8));
    const QString normalizedSym = normalizedSymbol(symbol);
    emit logMessage(QStringLiteral("%1 Order %2 (%3): status=%4 remain=%5 cumQty=%6 @avg %7")
                        .arg(contextTag(ctx.accountName))
                        .arg(identifier)
                        .arg(symbol)
                        .arg(statusText(event.status))
                        .arg(event.remainQuantity, 0, 'f', 8)
                        .arg(event.cumulativeQuantity, 0, 'f', 8)
                        .arg(event.avgPrice, 0, 'f', 8));
    const QString orderId = !event.id.isEmpty() ? event.id : event.clientId;
    OrderSide side = OrderSide::Buy;
    auto existing = ctx.activeOrders.constFind(orderId);
    if (existing != ctx.activeOrders.constEnd()) {
        side = existing.value().side;
    } else if (event.tradeType == 1) {
        side = OrderSide::Buy;
    } else if (event.tradeType == 2) {
        side = OrderSide::Sell;
    }
    if (!orderId.isEmpty() && !normalizedSym.isEmpty()) {
        const double price = event.price;
        double remain = event.remainQuantity;
        if (remain <= 0.0 && event.quantity > 0.0) {
            remain = event.quantity;
        }
        const double notional = price > 0.0 && remain > 0.0 ? price * remain : 0.0;
        if (notional > 0.0 || event.status == 0 || event.status == 1) {
            OrderRecord record;
            record.symbol = normalizedSym;
            record.side = side;
            record.price = price;
            record.quantityNotional =
                notional > 0.0 ? notional : std::abs(price * std::max(remain, 0.0));
            record.createdMs =
                event.createTime > 0 ? event.createTime : QDateTime::currentMSecsSinceEpoch();
            record.orderId = orderId;
            ctx.activeOrders.insert(orderId, record);
        } else {
            ctx.activeOrders.remove(orderId);
        }
        emitLocalOrderSnapshot(ctx, normalizedSym);
    }
    const bool isTerminalStatus = event.status == 2
                                  || event.status == 3
                                  || event.status == 4
                                  || event.status == 5;
    if (isTerminalStatus) {
        ctx.pendingCancelSymbols.remove(normalizedSym);
        emit orderCanceled(ctx.accountName, normalizedSym, side, event.price, orderId);
    }
}

void TradeManager::processPrivateAccount(Context &ctx, const QByteArray &body)
{
    PrivateAccountEvent event;
    if (!parsePrivateAccountBody(body, event)) {
        emit logMessage(QStringLiteral("%1 Failed to parse private account payload.")
                            .arg(contextTag(ctx.accountName)));
        return;
    }
    const QString asset = event.asset.trimmed().toUpper();
    if (!asset.isEmpty()) {
        BalanceState &st = ctx.balances[asset];
        st.available = event.balance;
        st.locked = event.frozen;
        st.updatedMs = (event.time > 0 && event.time < 1000000000000LL) ? (event.time * 1000) : event.time;
        if (st.updatedMs <= 0) {
            st.updatedMs = QDateTime::currentMSecsSinceEpoch();
        }
        emit finrezChanged(ctx.profile);
    }
    emit logMessage(QStringLiteral("%1 Balance %2: available=%3 frozen=%4 (%5)")
                        .arg(contextTag(ctx.accountName))
                        .arg(event.asset)
                        .arg(event.balance, 0, 'f', 8)
                        .arg(event.frozen, 0, 'f', 8)
                        .arg(event.changeType));
}

void TradeManager::recordFinrezTrade(Context &ctx, const ExecutedTrade &trade)
{
    const QString pnlAsset = QStringLiteral("USDT");
    if (std::abs(trade.realizedPnl) > 1e-12) {
        ctx.realizedPnl[pnlAsset] += trade.realizedPnl;
    }
    QString feeAsset = trade.feeCurrency.trimmed().toUpper();
    if (feeAsset.isEmpty()) {
        feeAsset = pnlAsset;
    }
    if (std::abs(trade.feeAmount) > 1e-12) {
        ctx.commissions[feeAsset] += trade.feeAmount;
    }
    emit finrezChanged(ctx.profile);
}

void TradeManager::emitLocalOrderSnapshot(Context &ctx, const QString &symbol)
{
    const QString normalized = normalizedSymbol(symbol);
    QVector<DomWidget::LocalOrderMarker> markers;
    markers.reserve(ctx.activeOrders.size());
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto it = ctx.activeOrders.constBegin(); it != ctx.activeOrders.constEnd(); ++it) {
        const auto &record = it.value();
        if (record.symbol != normalized || record.price <= 0.0 || record.quantityNotional <= 0.0) {
            continue;
        }
        DomWidget::LocalOrderMarker marker;
        marker.price = record.price;
        marker.quantity = record.quantityNotional;
        marker.side = record.side;
        marker.createdMs = record.createdMs > 0 ? record.createdMs : nowMs;
        marker.orderId = it.key();
        markers.push_back(marker);
    }
    emit localOrdersUpdated(ctx.accountName, normalized, markers);
}

void TradeManager::clearSymbolActiveOrders(Context &ctx, const QString &symbol)
{
    const QString normalized = normalizedSymbol(symbol);
    if (normalized.isEmpty()) {
        return;
    }
    QVector<OrderRecord> removed;
    for (auto it = ctx.activeOrders.begin(); it != ctx.activeOrders.end();) {
        if (it.value().symbol == normalized) {
            OrderRecord record = it.value();
            record.orderId = it.key();
            removed.push_back(record);
            it = ctx.activeOrders.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &record : removed) {
        emit orderCanceled(ctx.accountName, normalized, record.side, record.price, record.orderId);
    }
    emit localOrdersUpdated(ctx.accountName, normalized, {});
}

void TradeManager::clearLocalOrderSnapshots(Context &ctx)
{
    if (ctx.activeOrders.isEmpty()) {
        return;
    }
    QSet<QString> symbols;
    symbols.reserve(ctx.activeOrders.size());
    for (const auto &record : ctx.activeOrders) {
        if (!record.symbol.isEmpty()) {
            symbols.insert(record.symbol);
        }
    }
    ctx.activeOrders.clear();
    for (const QString &symbol : symbols) {
        emit localOrdersUpdated(ctx.accountName, symbol, {});
    }
}

QNetworkRequest TradeManager::makeFuturesRequest(const QString &path,
                                                 const Context &ctx,
                                                 const QByteArray &body,
                                                 bool signBody) const
{
    QUrl url(m_futuresBaseUrl + path);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("accept", "*/*");
    request.setRawHeader("accept-language", "en-US,en;q=0.9,ru;q=0.8");
    request.setRawHeader("cache-control", "no-cache");
    request.setRawHeader("pragma", "no-cache");
    request.setRawHeader("origin", "https://www.mexc.com");
    request.setRawHeader("referer", "https://www.mexc.com/");
    request.setRawHeader("user-agent",
                         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                         "Chrome/118.0.0.0 Safari/537.36");
    request.setRawHeader("x-language", "en-US");
    if (!ctx.credentials.uid.trimmed().isEmpty()) {
        request.setRawHeader("authorization", ctx.credentials.uid.trimmed().toUtf8());
    }
    if (signBody && !body.isEmpty()) {
        QByteArray nonce;
        QByteArray signature;
        if (buildFuturesSignature(body, ctx, nonce, signature)) {
            request.setRawHeader("x-mxc-nonce", nonce);
            request.setRawHeader("x-mxc-sign", signature);
        }
    }
    return request;
}

bool TradeManager::buildFuturesSignature(const QByteArray &body,
                                         const Context &ctx,
                                         QByteArray &nonceOut,
                                         QByteArray &signOut) const
{
    if (ctx.credentials.uid.trimmed().isEmpty()) {
        return false;
    }
    const QByteArray nonce = QByteArray::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray key = ctx.credentials.uid.trimmed().toUtf8();
    const QByteArray gHash =
        QCryptographicHash::hash(key + nonce, QCryptographicHash::Md5).toHex();
    const QByteArray g = gHash.mid(7);
    const QByteArray signHash =
        QCryptographicHash::hash(nonce + body + g, QCryptographicHash::Md5).toHex();
    nonceOut = nonce;
    signOut = signHash;
    return true;
}

void TradeManager::placeMexcFuturesOrder(Context &ctx,
                                         const QString &symbol,
                                         double price,
                                         double quantity,
                                         OrderSide side,
                                         int leverage)
{
    const QString symUpper = normalizedSymbol(symbol);
    PendingFuturesOrder order;
    order.symbol = symUpper;
    order.price = price;
    order.quantityBase = quantity;
    order.side = side;
    order.leverage = leverage;

    const auto metaIt = ctx.futuresContractMeta.constFind(symUpper);
    if (metaIt != ctx.futuresContractMeta.constEnd() && metaIt->valid()) {
        submitMexcFuturesOrder(ctx, order, *metaIt);
        return;
    }

    ctx.pendingFuturesOrders[symUpper].push_back(order);
    ensureFuturesContractMeta(ctx, symUpper);
}

void TradeManager::ensureFuturesContractMeta(Context &ctx, const QString &symbolUpper)
{
    const QString sym = normalizedSymbol(symbolUpper);
    if (sym.isEmpty()) {
        return;
    }
    if (ctx.futuresContractMeta.contains(sym)) {
        return;
    }
    if (ctx.futuresContractMetaInFlight.contains(sym)) {
        return;
    }
    ctx.futuresContractMetaInFlight.insert(sym);

    QUrl url(QStringLiteral("https://contract.mexc.com/api/v1/contract/detail"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("symbol"), sym);
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    auto *reply = ensureMexcNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, sym]() {
        ctxPtr->futuresContractMetaInFlight.remove(sym);
        const auto err = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();

        FuturesContractMeta meta;
        bool ok = false;
        if (err == QNetworkReply::NoError) {
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            const QJsonObject obj = doc.object();
            QJsonValue dataVal = obj.value(QStringLiteral("data"));
            QJsonObject dataObj;
            if (dataVal.isObject()) {
                dataObj = dataVal.toObject();
            } else if (dataVal.isArray()) {
                const QJsonArray arr = dataVal.toArray();
                if (!arr.isEmpty() && arr.first().isObject()) {
                    dataObj = arr.first().toObject();
                }
            }
            if (!dataObj.isEmpty()) {
                meta.contractSize = dataObj.value(QStringLiteral("contractSize")).toDouble(1.0);
                meta.volScale = dataObj.value(QStringLiteral("volScale")).toInt(0);
                meta.minVol = dataObj.value(QStringLiteral("minVol")).toInt(1);
                meta.minLeverage = dataObj.value(QStringLiteral("minLeverage")).toInt(1);
                meta.maxLeverage = dataObj.value(QStringLiteral("maxLeverage")).toInt(200);
                meta.minLeverage = std::max(1, meta.minLeverage);
                meta.maxLeverage = std::max(meta.minLeverage, meta.maxLeverage);
                ok = meta.valid();
            }
        }
        if (!ok) {
            emit logMessage(QStringLiteral("%1 Failed to load futures contract meta for %2: %3")
                                .arg(contextTag(ctxPtr->accountName))
                                .arg(sym)
                                .arg(err == QNetworkReply::NoError ? QStringLiteral("bad payload") : reply->errorString()));
            // Fail any queued orders for this symbol (better than sending wrong size).
            const auto pending = ctxPtr->pendingFuturesOrders.take(sym);
            for (const auto &ord : pending) {
                emit orderFailed(ctxPtr->accountName,
                                 sym,
                                 tr("Failed to load futures contract info (contractSize/minVol)"));
            }
            return;
        }

        ctxPtr->futuresContractMeta.insert(sym, meta);
        emit logMessage(QStringLiteral("%1 Futures meta %2: contractSize=%3 minVol=%4 volScale=%5 maxLev=%6")
                            .arg(contextTag(ctxPtr->accountName))
                            .arg(sym)
                            .arg(meta.contractSize, 0, 'g', 10)
                            .arg(meta.minVol)
                            .arg(meta.volScale)
                            .arg(meta.maxLeverage));
        // Update multiplier for any cached position of this symbol.
        auto posIt = ctxPtr->positions.find(sym);
        if (posIt != ctxPtr->positions.end() && posIt->hasPosition) {
            posIt->qtyMultiplier = meta.contractSize;
            emitPositionChanged(*ctxPtr, sym);
        }

        const auto pending = ctxPtr->pendingFuturesOrders.take(sym);
        for (const auto &ord : pending) {
            submitMexcFuturesOrder(*ctxPtr, ord, meta);
        }
    });
}

void TradeManager::submitMexcFuturesOrder(Context &ctx,
                                         const PendingFuturesOrder &order,
                                         const FuturesContractMeta &meta)
{
    const QString sym = normalizedSymbol(order.symbol);
    const double contractSize = meta.contractSize > 0.0 ? meta.contractSize : 1.0;
    const double contractsRaw = order.quantityBase / contractSize;
    const int volScale = std::max(0, meta.volScale);
    const double factor = std::pow(10.0, static_cast<double>(volScale));
    // FLOOR: never exceed requested notional; keeps order size <= requested.
    const double vol = std::floor(contractsRaw * factor) / factor;
    if (!(vol > 0.0) || vol + 1e-12 < static_cast<double>(meta.minVol)) {
        emit orderFailed(ctx.accountName,
                         sym,
                         tr("Order size too small for futures contract (minVol=%1)")
                             .arg(meta.minVol));
        emit logMessage(QStringLiteral("%1 Futures order blocked (too small): %2 baseQty=%3 contracts=%4 minVol=%5")
                            .arg(contextTag(ctx.accountName))
                            .arg(sym)
                            .arg(order.quantityBase, 0, 'g', 10)
                            .arg(vol, 0, 'g', 10)
                            .arg(meta.minVol));
        return;
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("symbol"), sym);
    payload.insert(QStringLiteral("price"), order.price);
    payload.insert(QStringLiteral("vol"), vol);
    payload.insert(QStringLiteral("side"), order.side == OrderSide::Buy ? 1 : 3);
    payload.insert(QStringLiteral("type"), 1); // limit order
    payload.insert(QStringLiteral("openType"), 1); // isolated margin (matches mexc-futures-sdk defaults)
    payload.insert(QStringLiteral("leverage"),
                   std::clamp(order.leverage > 0 ? order.leverage : 20,
                              meta.minLeverage,
                              std::max(meta.minLeverage, meta.maxLeverage)));

    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    emit logMessage(QStringLiteral("%1 Sending futures order payload: %2")
                        .arg(contextTag(ctx.accountName), QString::fromUtf8(body)));
    QNetworkRequest request =
        makeFuturesRequest(QStringLiteral("/private/order/submit"), ctx, body, true);
    auto *reply = ensureMexcNetwork(ctx)->post(request, body);
    connect(reply,
            &QNetworkReply::finished,
            this,
            [this, reply, ctxPtr = &ctx, sym, side = order.side, price = order.price, quantityBase = order.quantityBase]() {
                const auto networkError = reply->error();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();
                if (networkError != QNetworkReply::NoError) {
                    const QString message =
                        raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw);
                    emit orderFailed(ctxPtr->accountName, sym, message);
                    emit logMessage(QStringLiteral("%1 Futures order failed: %2")
                                        .arg(contextTag(ctxPtr->accountName), message));
                    return;
                }
                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                const QJsonObject obj = doc.object();
                const bool success = obj.value(QStringLiteral("success")).toBool(true);
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                if (!success || code != 0) {
                    const QString errMsg =
                        obj.value(QStringLiteral("message"))
                            .toString(obj.value(QStringLiteral("msg")).toString(QStringLiteral("request rejected")));
                    emit orderFailed(ctxPtr->accountName, sym, errMsg);
                    emit logMessage(QStringLiteral("%1 Futures order rejected: %2 (code %3)")
                                        .arg(contextTag(ctxPtr->accountName))
                                        .arg(errMsg)
                                        .arg(code));
                    return;
                }
                QString orderId = obj.value(QStringLiteral("data")).toVariant().toString();
                if (orderId.isEmpty()) {
                    orderId = QStringLiteral("futures_%1")
                                  .arg(QDateTime::currentMSecsSinceEpoch());
                }
                emit logMessage(QStringLiteral("%1 Futures order accepted: %2")
                                    .arg(contextTag(ctxPtr->accountName),
                                         QString::fromUtf8(raw)));
                emit orderPlaced(ctxPtr->accountName, sym, side, price, quantityBase, orderId);
            });
}


void TradeManager::cancelAllMexcFuturesOrders(Context &ctx, const QString &symbol)
{
    QJsonObject payload;
    if (!symbol.isEmpty()) {
        payload.insert(QStringLiteral("symbol"), symbol);
    }
    const QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    QNetworkRequest request =
        makeFuturesRequest(QStringLiteral("/private/order/cancel_all"), ctx, body, true);
    auto *reply = ensureMexcNetwork(ctx)->post(request, body);
    emit logMessage(QStringLiteral("%1 Sending futures cancel-all for %2")
                        .arg(contextTag(ctx.accountName), symbol));
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, symbol]() {
        const auto networkError = reply->error();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError) {
            const QString message =
                raw.isEmpty() ? reply->errorString() : QString::fromUtf8(raw);
            emit orderFailed(ctxPtr->accountName, symbol, message);
            emit logMessage(QStringLiteral("%1 Futures cancel failed: %2")
                                .arg(contextTag(ctxPtr->accountName), message));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        const QJsonObject obj = doc.object();
        const bool success = obj.value(QStringLiteral("success")).toBool(true);
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (!success || code != 0) {
            const QString errMsg =
                obj.value(QStringLiteral("message"))
                    .toString(obj.value(QStringLiteral("msg")).toString(QStringLiteral("request rejected")));
            emit orderFailed(ctxPtr->accountName, symbol, errMsg);
            emit logMessage(QStringLiteral("%1 Futures cancel rejected: %2 (code %3)")
                                .arg(contextTag(ctxPtr->accountName))
                                .arg(errMsg)
                                .arg(code));
            return;
        }
        emit logMessage(QStringLiteral("%1 Futures cancel-all response: %2")
                            .arg(contextTag(ctxPtr->accountName),
                                 QString::fromUtf8(raw)));
        if (!symbol.isEmpty()) {
            clearSymbolActiveOrders(*ctxPtr, symbol);
            ctxPtr->pendingCancelSymbols.remove(symbol);
        } else {
            ctxPtr->pendingCancelSymbols.clear();
        }
        fetchFuturesOpenOrders(*ctxPtr);
    });
}

QString TradeManager::tradeHistoryPath() const
{
    return tradeConfigDir() + QDir::separator() + QStringLiteral("trades_history.jsonl");
}

void TradeManager::loadTradeHistory()
{
    const QString primaryDir = tradeConfigDir();
    QFile f(resolveTradeHistoryPath(primaryDir));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QVector<ExecutedTrade> out;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        ExecutedTrade t;
        t.timeMs = static_cast<qint64>(obj.value(QStringLiteral("t")).toVariant().toLongLong());
        t.accountName = obj.value(QStringLiteral("acct")).toString();
        t.symbol = obj.value(QStringLiteral("sym")).toString();
        const QString sideStr = obj.value(QStringLiteral("side")).toString();
        t.side = sideStr.compare(QStringLiteral("SELL"), Qt::CaseInsensitive) == 0 ? OrderSide::Sell
                                                                                   : OrderSide::Buy;
        t.price = obj.value(QStringLiteral("price")).toDouble();
        t.quantity = obj.value(QStringLiteral("qty")).toDouble();
        t.feeCurrency = obj.value(QStringLiteral("feeCcy")).toString();
        t.feeAmount = obj.value(QStringLiteral("fee")).toDouble();
        t.realizedPnl = obj.value(QStringLiteral("pnl")).toDouble(0.0);
        t.realizedPct = obj.value(QStringLiteral("pnlPct")).toDouble(0.0);
        if (!t.symbol.isEmpty() && t.timeMs > 0) {
            out.push_back(t);
        }
    }
    m_executedTrades = std::move(out);
}

void TradeManager::appendTradeHistory(const ExecutedTrade &trade)
{
    m_executedTrades.push_back(trade);

    QFile f(tradeHistoryPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QJsonObject obj;
        obj.insert(QStringLiteral("t"), static_cast<double>(trade.timeMs));
        obj.insert(QStringLiteral("acct"), trade.accountName);
        obj.insert(QStringLiteral("sym"), trade.symbol);
        obj.insert(QStringLiteral("side"),
                   trade.side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
        obj.insert(QStringLiteral("price"), trade.price);
        obj.insert(QStringLiteral("qty"), trade.quantity);
        obj.insert(QStringLiteral("feeCcy"), trade.feeCurrency);
        obj.insert(QStringLiteral("fee"), trade.feeAmount);
        obj.insert(QStringLiteral("pnl"), trade.realizedPnl);
        obj.insert(QStringLiteral("pnlPct"), trade.realizedPct);
        f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        f.write("\n");
    }

    constexpr int kMaxTrades = 10000;
    if (m_executedTrades.size() > kMaxTrades) {
        const int start =
            std::max(0, static_cast<int>(m_executedTrades.size()) - kMaxTrades);
        m_executedTrades = m_executedTrades.mid(start);
        QSaveFile sf(tradeHistoryPath());
        if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            for (const auto &t : m_executedTrades) {
                QJsonObject obj;
                obj.insert(QStringLiteral("t"), static_cast<double>(t.timeMs));
                obj.insert(QStringLiteral("acct"), t.accountName);
                obj.insert(QStringLiteral("sym"), t.symbol);
                obj.insert(QStringLiteral("side"),
                           t.side == OrderSide::Buy ? QStringLiteral("BUY") : QStringLiteral("SELL"));
                obj.insert(QStringLiteral("price"), t.price);
                obj.insert(QStringLiteral("qty"), t.quantity);
                obj.insert(QStringLiteral("feeCcy"), t.feeCurrency);
                obj.insert(QStringLiteral("fee"), t.feeAmount);
                obj.insert(QStringLiteral("pnl"), t.realizedPnl);
                obj.insert(QStringLiteral("pnlPct"), t.realizedPct);
                sf.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
                sf.write("\n");
            }
            sf.commit();
        }
    }
}

void TradeManager::pollMyTrades(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::MexcSpot) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    if (ctx.watchedSymbols.isEmpty()) {
        return;
    }
    for (const QString &sym : ctx.watchedSymbols) {
        if (ctx.myTradesInFlight.contains(sym)) {
            continue;
        }
        fetchMyTrades(ctx, sym);
        // one per tick to avoid spamming
        break;
    }
}

void TradeManager::pollFuturesDeals(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::MexcFutures) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    if (ctx.watchedSymbols.isEmpty()) {
        return;
    }
    for (const QString &sym : ctx.watchedSymbols) {
        if (ctx.futuresDealsInFlight.contains(sym)) {
            continue;
        }
        fetchFuturesDeals(ctx, sym);
        break;
    }
}

void TradeManager::pollLighterAccount(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    if (ctx.watchedSymbols.isEmpty()) {
        return;
    }
    fetchLighterAccount(ctx);
    pollLighterActiveOrders(ctx);
}

void TradeManager::pollLighterTrades(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    if (ctx.watchedSymbols.isEmpty()) {
        return;
    }
    // We prefer private WS subscriptions for trades; REST trades endpoint is flaky (invalid param) on some deployments.
    if (ctx.lighterPrivateSubscribed || (ctx.lighterStreamConnected && ctx.lighterStreamReady)) {
        return;
    }
    fetchLighterTrades(ctx);
}

void TradeManager::armLighterBurst(Context &ctx, int ticks)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (ticks <= 0) {
        return;
    }
    ctx.lighterBurstRemaining = std::max(ctx.lighterBurstRemaining, ticks);
    if (!ctx.lighterBurstTimer.isActive()) {
        ctx.lighterBurstTimer.start();
    }
    // Kick an immediate refresh (best-effort).
    pollLighterActiveOrders(ctx);
}

void TradeManager::pollLighterActiveOrders(Context &ctx)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (ctx.state != ConnectionState::Connected) {
        return;
    }
    if (!ensureCredentials(ctx)) {
        return;
    }
    if (ctx.watchedSymbols.isEmpty() || ctx.lighterActiveOrdersPending) {
        return;
    }
    QStringList symbols = ctx.watchedSymbols.values();
    if (symbols.isEmpty()) {
        return;
    }
    std::sort(symbols.begin(), symbols.end());
    const int idx = ctx.lighterActiveOrdersPollIndex % symbols.size();
    ctx.lighterActiveOrdersPollIndex = (ctx.lighterActiveOrdersPollIndex + 1) % symbols.size();
    const QString sym = normalizedSymbol(symbols[idx]);
    if (sym.isEmpty()) {
        return;
    }
    fetchLighterActiveOrders(ctx, sym);
}

void TradeManager::fetchFuturesDeals(Context &ctx, const QString &symbolUpper)
{
    const QString sym = normalizedSymbol(symbolUpper);
    if (sym.isEmpty()) {
        return;
    }
    ctx.futuresDealsInFlight.insert(sym);

    // Ensure contract meta so we can compute PnL % correctly when possible.
    ensureFuturesContractMeta(ctx, sym);

    QUrl url(m_futuresBaseUrl + QStringLiteral("/private/order/list/order_deals"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("symbol"), sym);
    q.addQueryItem(QStringLiteral("page_num"), QStringLiteral("1"));
    q.addQueryItem(QStringLiteral("page_size"), QStringLiteral("100"));
    const qint64 lastTs = ctx.lastFuturesDealTsBySymbol.value(sym, 0);
    if (lastTs > 0) {
        q.addQueryItem(QStringLiteral("start_time"), QString::number(lastTs + 1));
    }
    url.setQuery(q);

    QNetworkRequest req = makeFuturesRequest(QStringLiteral("/private/order/list/order_deals"), ctx);
    req.setUrl(url);
    auto *reply = ensureMexcNetwork(ctx)->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, sym]() {
        ctxPtr->futuresDealsInFlight.remove(sym);
        const auto err = reply->error();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            const QString msg = status >= 400 ? QStringLiteral("HTTP %1: %2")
                                                    .arg(status)
                                                    .arg(QString::fromUtf8(raw))
                                              : reply->errorString();
            emit logMessage(QStringLiteral("%1 futures order_deals failed for %2: %3")
                                .arg(contextTag(ctxPtr->accountName), sym, msg));
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isObject()) {
            return;
        }
        const QJsonObject obj = doc.object();
        const bool success = obj.value(QStringLiteral("success")).toBool(true);
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (!success || code != 0) {
            const QString msg = obj.value(QStringLiteral("message"))
                                    .toString(obj.value(QStringLiteral("msg")).toString());
            emit logMessage(QStringLiteral("%1 futures order_deals rejected for %2: %3 (code %4)")
                                .arg(contextTag(ctxPtr->accountName), sym, msg, QString::number(code)));
            return;
        }
        const QJsonArray arr = obj.value(QStringLiteral("data")).toArray();
        if (arr.isEmpty()) {
            return;
        }
        auto toDouble = [](const QJsonValue &v) -> double {
            if (v.isDouble()) return v.toDouble();
            if (v.isString()) return v.toString().toDouble();
            return v.toVariant().toDouble();
        };
        auto toInt64 = [](const QJsonValue &v) -> qint64 {
            if (v.isDouble()) return static_cast<qint64>(v.toDouble());
            if (v.isString()) return v.toString().toLongLong();
            return v.toVariant().toLongLong();
        };

        qint64 maxTs = ctxPtr->lastFuturesDealTsBySymbol.value(sym, 0);
        const auto metaIt = ctxPtr->futuresContractMeta.constFind(sym);
        const double contractSize =
            (metaIt != ctxPtr->futuresContractMeta.constEnd() && metaIt->valid())
                ? metaIt->contractSize
                : 1.0;

        for (const auto &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            const qint64 ts = toInt64(o.value(QStringLiteral("timestamp")));
            if (ts > 0) {
                maxTs = std::max(maxTs, ts);
            }
            const double price = toDouble(o.value(QStringLiteral("price")));
            const double vol = toDouble(o.value(QStringLiteral("vol")));
            if (!(price > 0.0) || !(vol > 0.0)) continue;

            const int sideFlag = o.value(QStringLiteral("side")).toInt(1);
            const OrderSide side = (sideFlag == 1 || sideFlag == 2) ? OrderSide::Buy : OrderSide::Sell;
            const double profit = toDouble(o.value(QStringLiteral("profit")));
            const double notional = std::abs(price * vol * std::max(1e-12, contractSize));

            ExecutedTrade trade;
            trade.accountName = ctxPtr->accountName;
            trade.symbol = sym;
            trade.side = side;
            trade.price = price;
            trade.quantity = vol;
            trade.feeAmount = toDouble(o.value(QStringLiteral("fee")));
            trade.feeCurrency = o.value(QStringLiteral("feeCurrency")).toString();
            trade.realizedPnl = profit;
            trade.realizedPct = notional > 0.0 ? (profit / notional) * 100.0 : 0.0;
            trade.timeMs = ts > 0 ? ts : QDateTime::currentMSecsSinceEpoch();
            appendTradeHistory(trade);
            recordFinrezTrade(*ctxPtr, trade);
            emit tradeExecuted(trade);
        }
        if (maxTs > 0) {
            ctxPtr->lastFuturesDealTsBySymbol.insert(sym, maxTs);
        }
        // Refresh positions periodically when deals arrive.
        fetchFuturesPositions(*ctxPtr);
    });
}

void TradeManager::ensureLighterMetaLoaded(Context &ctx, const QString &baseUrl, std::function<void(QString err)> cb)
{
    auto &cache = lighterMetaCacheByUrl()[baseUrl];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool freshEnough = (cache.updatedMs > 0 && (now - cache.updatedMs) < 5 * 60 * 1000);
    if (!cache.bySymbol.isEmpty() && freshEnough) {
        cb(QString());
        return;
    }
    cache.waiters.push_back(std::move(cb));
    if (cache.inFlight) {
        return;
    }
    cache.inFlight = true;
    QNetworkRequest req(lighterUrl(baseUrl, QStringLiteral("/api/v1/orderBookDetails")));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = ensureLighterNetwork(ctx)->get(req);
    applyReplyTimeout(reply, 6000);
    connect(reply, &QNetworkReply::finished, this, [this, reply, baseUrl]() {
        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        auto &cache2 = lighterMetaCacheByUrl()[baseUrl];
        cache2.inFlight = false;
        QVector<std::function<void(QString err)>> waiters = std::move(cache2.waiters);
        cache2.waiters.clear();
        auto failAll = [&](const QString &msg) {
            for (auto &w : waiters) {
                w(msg);
            }
        };
        if (err != QNetworkReply::NoError || status >= 400) {
            failAll(lighterHttpErrorMessage(reply, raw));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (doc.isNull() || !doc.isObject()) {
            failAll(QStringLiteral("Invalid Lighter markets response"));
            return;
        }
        const QJsonObject obj = doc.object();
        const int code = obj.value(QStringLiteral("code")).toInt(0);
        if (code != 200) {
            failAll(obj.value(QStringLiteral("message")).toString(QStringLiteral("Lighter markets error")));
            return;
        }
        cache2.bySymbol.clear();
        // Lighter mainnet currently returns perp markets in `order_book_details` (market_type="perp").
        // Spot markets come in `spot_order_book_details`.
        addLighterMarketMeta(cache2.bySymbol, obj.value(QStringLiteral("order_book_details")).toArray(), false);
        addLighterMarketMeta(cache2.bySymbol, obj.value(QStringLiteral("spot_order_book_details")).toArray(), true);
        cache2.symbolByMarketId.clear();
        cache2.symbolByMarketId.reserve(cache2.bySymbol.size());
        for (auto it = cache2.bySymbol.constBegin(); it != cache2.bySymbol.constEnd(); ++it) {
            if (it.value().marketId >= 0) {
                cache2.symbolByMarketId.insert(it.value().marketId, it.key());
            }
        }
        cache2.updatedMs = QDateTime::currentMSecsSinceEpoch();
        for (auto &w : waiters) {
            w(QString());
        }
    });
}

void TradeManager::fetchLighterActiveOrders(Context &ctx, const QString &symbolUpper)
{
    if (ctx.profile != ConnectionStore::Profile::Lighter) {
        return;
    }
    if (ctx.lighterActiveOrdersPending) {
        return;
    }
    const QString sym = normalizedSymbol(symbolUpper);
    if (sym.isEmpty()) {
        return;
    }
    const QString baseUrl = normalizeLighterUrl(ctx.credentials.baseUrl);
    if (baseUrl.isEmpty() || ctx.lighterAuthToken.isEmpty()) {
        return;
    }

    ctx.lighterActiveOrdersPending = true;

    ensureLighterMetaLoaded(ctx, baseUrl, [this, ctxPtr = &ctx, baseUrl, sym](const QString &metaErr) {
        if (!ctxPtr) {
            return;
        }
        if (!metaErr.isEmpty()) {
            ctxPtr->lighterActiveOrdersPending = false;
            emit logMessage(QStringLiteral("%1 Lighter active orders fetch skipped: %2")
                                .arg(contextTag(ctxPtr->accountName), metaErr));
            return;
        }
        const auto &cache = lighterMetaCacheByUrl().value(baseUrl);
        const auto metaIt = cache.bySymbol.constFind(sym);
        if (metaIt == cache.bySymbol.constEnd() || metaIt->marketId < 0) {
            ctxPtr->lighterActiveOrdersPending = false;
            emit logMessage(QStringLiteral("%1 Lighter active orders fetch skipped: missing market id for %2")
                                .arg(contextTag(ctxPtr->accountName), sym));
            return;
        }
        const int marketId = metaIt->marketId;
        const int priceDecimals = metaIt->priceDecimals;

        auto parseBool = [](const QJsonValue &v) -> bool {
            if (v.isBool()) return v.toBool();
            if (v.isDouble()) return v.toInt() != 0;
            if (v.isString()) {
                const QString s = v.toString().trimmed();
                if (s.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0) return true;
                if (s.compare(QStringLiteral("false"), Qt::CaseInsensitive) == 0) return false;
                return s.toInt() != 0;
            }
            return v.toVariant().toBool();
        };
        auto toDouble = [](const QJsonValue &v) -> double {
            if (v.isDouble()) return v.toDouble();
            if (v.isString()) return v.toString().toDouble();
            return v.toVariant().toDouble();
        };

        enum class TriggerCond { Unknown, Above, Below };
        auto parseTriggerCond = [](const QJsonObject &o) -> TriggerCond {
            const QStringList stringKeys = {
                QStringLiteral("trigger_condition"),
                QStringLiteral("triggerCondition"),
                QStringLiteral("trigger_conditions"),
                QStringLiteral("triggerConditions")
            };
            for (const auto &k : stringKeys) {
                if (!o.contains(k)) continue;
                const QString s = o.value(k).toVariant().toString().trimmed().toLower();
                if (s.contains(QStringLiteral("below")) || s.contains(QStringLiteral("under"))) return TriggerCond::Below;
                if (s.contains(QStringLiteral("above")) || s.contains(QStringLiteral("over"))) return TriggerCond::Above;
                if (s == QStringLiteral("0")) return TriggerCond::Below;
                if (s == QStringLiteral("1")) return TriggerCond::Above;
            }
            // Some APIs use numeric enums.
            if (o.contains(QStringLiteral("trigger_condition")) && o.value(QStringLiteral("trigger_condition")).isDouble()) {
                const int v = o.value(QStringLiteral("trigger_condition")).toInt(-1);
                if (v == 0) return TriggerCond::Below;
                if (v == 1) return TriggerCond::Above;
            }
            const QStringList boolAboveKeys = {
                QStringLiteral("trigger_is_above"),
                QStringLiteral("triggerIsAbove"),
                QStringLiteral("is_trigger_above"),
                QStringLiteral("isTriggerAbove"),
                QStringLiteral("is_above"),
                QStringLiteral("isAbove")
            };
            for (const auto &k : boolAboveKeys) {
                if (!o.contains(k)) continue;
                const QJsonValue v = o.value(k);
                if (v.isBool()) return v.toBool() ? TriggerCond::Above : TriggerCond::Below;
                if (v.isDouble()) return v.toInt() != 0 ? TriggerCond::Above : TriggerCond::Below;
                if (v.isString()) return v.toString().trimmed().toInt() != 0 ? TriggerCond::Above : TriggerCond::Below;
            }
            return TriggerCond::Unknown;
        };

        auto doRequest = std::make_shared<std::function<void(const QString &paramName, bool allowRetry)>>();
        *doRequest = [this, ctxPtr, baseUrl, sym, marketId, priceDecimals, parseBool, toDouble, parseTriggerCond, doRequest](const QString &paramName, bool allowRetry) {
            QUrl url = lighterUrl(baseUrl, QStringLiteral("/api/v1/accountActiveOrders"));
            QUrlQuery q;
            q.addQueryItem(QStringLiteral("account_index"), QString::number(ctxPtr->credentials.accountIndex));
            q.addQueryItem(paramName, QString::number(marketId));
            q.addQueryItem(QStringLiteral("auth"), ctxPtr->lighterAuthToken);
            url.setQuery(q);
            QNetworkRequest req(url);
            req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
            req.setRawHeader("authorization", ctxPtr->lighterAuthToken.toUtf8());
            QNetworkReply *reply = ensureLighterNetwork(*ctxPtr)->get(req);
            connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr, sym, paramName, allowRetry, priceDecimals, parseBool, toDouble, parseTriggerCond, doRequest]() {
                const QNetworkReply::NetworkError err = reply->error();
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray raw = reply->readAll();
                reply->deleteLater();

                auto done = [&]() {
                    ctxPtr->lighterActiveOrdersPending = false;
                };

                if (err != QNetworkReply::NoError || status >= 400) {
                    done();
                    emit logMessage(QStringLiteral("%1 Lighter active orders fetch failed for %2: %3")
                                        .arg(contextTag(ctxPtr->accountName), sym, lighterHttpErrorMessage(reply, raw)));
                    return;
                }

                const QJsonDocument doc = QJsonDocument::fromJson(raw);
                if (doc.isNull() || !doc.isObject()) {
                    done();
                    emit logMessage(QStringLiteral("%1 Lighter active orders fetch invalid response for %2")
                                        .arg(contextTag(ctxPtr->accountName), sym));
                    return;
                }
                const QJsonObject obj = doc.object();
                const int code = obj.value(QStringLiteral("code")).toInt(0);
                const QString msg = obj.value(QStringLiteral("message")).toString();
                if (code != 200) {
                    if (allowRetry && code == 20001 && msg.contains(QStringLiteral("invalid param"), Qt::CaseInsensitive)) {
                        const QString otherParam =
                            (paramName == QStringLiteral("market_id")) ? QStringLiteral("market_index")
                                                                       : QStringLiteral("market_id");
                        // Retry with the other market parameter.
                        (*doRequest)(otherParam, false);
                        return;
                    }
                    done();
                    emit logMessage(QStringLiteral("%1 Lighter active orders fetch rejected for %2: %3 (code %4)")
                                        .arg(contextTag(ctxPtr->accountName), sym, msg, QString::number(code)));
                    return;
                }

                const QJsonArray orders = obj.value(QStringLiteral("orders")).toArray();
                QHash<QString, OrderRecord> fetched;
                fetched.reserve(orders.size());

                bool hasSl = false;
                bool hasTp = false;
                double slPrice = 0.0;
                double tpPrice = 0.0;
                QStringList slStopIds;
                QStringList tpStopIds;
                qint64 bestSlId = 0;
                qint64 bestTpId = 0;
                const TradePosition pos = ctxPtr->positions.value(sym, TradePosition{});
                const bool hasPosition = pos.hasPosition && pos.quantity > 0.0 && pos.averagePrice > 0.0;

                auto parseIsAskFlag = [&](const QJsonObject &o) -> int {
                    const QStringList boolKeys = {QStringLiteral("is_ask"), QStringLiteral("isAsk")};
                    for (const auto &k : boolKeys) {
                        if (!o.contains(k)) continue;
                        const QJsonValue v = o.value(k);
                        if (v.isBool()) return v.toBool() ? 1 : 0;
                        if (v.isDouble()) return v.toInt() != 0 ? 1 : 0;
                        if (v.isString()) {
                            const QString s = v.toString().trimmed().toLower();
                            if (s == QStringLiteral("ask") || s == QStringLiteral("sell") || s == QStringLiteral("true") || s == QStringLiteral("1")) return 1;
                            if (s == QStringLiteral("bid") || s == QStringLiteral("buy") || s == QStringLiteral("false") || s == QStringLiteral("0")) return 0;
                        }
                    }
                    const QStringList sideKeys = {QStringLiteral("side"), QStringLiteral("order_side"), QStringLiteral("orderSide"), QStringLiteral("direction")};
                    for (const auto &k : sideKeys) {
                        if (!o.contains(k)) continue;
                        const QString s = o.value(k).toVariant().toString().trimmed().toLower();
                        if (s.contains(QStringLiteral("sell")) || s.contains(QStringLiteral("ask")) || s == QStringLiteral("short")) return 1;
                        if (s.contains(QStringLiteral("buy")) || s.contains(QStringLiteral("bid")) || s == QStringLiteral("long")) return 0;
                    }
                    return -1;
                };

                auto resolveStopKind = [&](Context &ctx,
                                           const QString &orderId,
                                           Context::LighterStopKind explicitKind,
                                           Context::LighterStopKind inferredKind,
                                           qint64 nowMs) -> Context::LighterStopKind {
                    const QString id = orderId.trimmed();
                    if (id.isEmpty()) {
                        return (explicitKind != Context::LighterStopKind::Unknown) ? explicitKind : inferredKind;
                    }
                    static constexpr int kMaxCacheSize = 8000;
                    if (ctx.lighterStopKindByOrderId.size() > kMaxCacheSize) {
                        ctx.lighterStopKindByOrderId.clear();
                    }
                    auto &info = ctx.lighterStopKindByOrderId[id];
                    if (explicitKind != Context::LighterStopKind::Unknown) {
                        info.kind = explicitKind;
                        info.explicitType = true;
                        info.updatedMs = nowMs;
                        return explicitKind;
                    }
                    if (info.explicitType && info.kind != Context::LighterStopKind::Unknown) {
                        info.updatedMs = nowMs;
                        return info.kind;
                    }
                    if (info.kind != Context::LighterStopKind::Unknown) {
                        info.updatedMs = nowMs;
                        return info.kind;
                    }
                    info.kind = inferredKind;
                    info.explicitType = false;
                    info.updatedMs = nowMs;
                    return inferredKind;
                };

                for (const auto &v : orders) {
                    if (!v.isObject()) continue;
                    const QJsonObject o = v.toObject();
                    const QJsonValue typeVal = o.value(QStringLiteral("type"));
                    const QString typeStr = typeVal.isString() ? typeVal.toString().trimmed() : QString();
                    const QString tifStr = o.value(QStringLiteral("time_in_force")).toString().trimmed();
                    Q_UNUSED(tifStr);

                    const int askFlag = parseIsAskFlag(o);
                    const bool isAsk = (askFlag == 1);
                    const OrderSide side = isAsk ? OrderSide::Sell : OrderSide::Buy;
                    const double price = toDouble(o.value(QStringLiteral("price")));

                    double baseAmt = 0.0;
                    if (o.contains(QStringLiteral("base_amount"))) {
                        baseAmt = toDouble(o.value(QStringLiteral("base_amount")));
                    } else if (o.contains(QStringLiteral("initial_base_amount"))) {
                        baseAmt = toDouble(o.value(QStringLiteral("initial_base_amount")));
                    } else if (o.contains(QStringLiteral("remaining_base_amount"))) {
                        baseAmt = toDouble(o.value(QStringLiteral("remaining_base_amount")));
                    }

                    QString orderId;
                    const QJsonValue clientIndexVal =
                        o.contains(QStringLiteral("client_order_index"))
                            ? o.value(QStringLiteral("client_order_index"))
                            : o.value(QStringLiteral("clientOrderIndex"));
                    const qint64 clientIndex = clientIndexVal.toVariant().toLongLong();
                    if (clientIndex > 0) {
                        orderId = QString::number(clientIndex);
                    }
                    if (o.contains(QStringLiteral("order_index"))) {
                        if (orderId.isEmpty()) {
                            orderId = o.value(QStringLiteral("order_index")).toVariant().toString();
                        }
                    }
                    if (orderId.isEmpty()) {
                        orderId = o.value(QStringLiteral("orderId")).toVariant().toString();
                    }
                    if (orderId.isEmpty()) {
                        orderId = o.value(QStringLiteral("order_id")).toVariant().toString();
                    }
                    if (orderId.isEmpty()) {
                        orderId = QStringLiteral("lighter_%1_%2")
                                      .arg(sym)
                                      .arg(QDateTime::currentMSecsSinceEpoch());
                    }

                    qint64 createdMs = o.value(QStringLiteral("timestamp")).toVariant().toLongLong();
                    if (createdMs <= 0) {
                        createdMs = o.value(QStringLiteral("time")).toVariant().toLongLong();
                    }
                    if (createdMs > 0 && createdMs < 1000000000000LL) {
                        createdMs *= 1000;
                    }
                    if (createdMs <= 0) {
                        createdMs = QDateTime::currentMSecsSinceEpoch();
                    }

                    const double notional = (price > 0.0 && baseAmt > 0.0) ? std::abs(price * baseAmt) : 0.0;

                    const bool reduceOnly = parseBool(o.value(QStringLiteral("reduce_only")));
                    int orderTypeCode = -1;
                    const QJsonValue orderTypeVal =
                        o.contains(QStringLiteral("order_type")) ? o.value(QStringLiteral("order_type"))
                        : (o.contains(QStringLiteral("orderType")) ? o.value(QStringLiteral("orderType"))
                        : (typeVal.isDouble() ? typeVal : QJsonValue()));
                    if (!orderTypeVal.isUndefined() && !orderTypeVal.isNull()) {
                        orderTypeCode = orderTypeVal.toVariant().toInt();
                    }
                    const QString typeLower = typeStr.toLower();
                    const bool isStopLossOrder =
                        reduceOnly && (orderTypeCode == 2
                                       || typeLower.contains(QStringLiteral("stoploss"))
                                       || typeLower.contains(QStringLiteral("s/l"))
                                       || typeLower == QStringLiteral("sl")
                                       || typeLower.contains(QStringLiteral("sl ")));
                    const bool isTakeProfitOrder =
                        reduceOnly && (orderTypeCode == 4
                                       || typeLower.contains(QStringLiteral("take"))
                                       || typeLower.contains(QStringLiteral("t/p"))
                                       || typeLower == QStringLiteral("tp")
                                       || typeLower.contains(QStringLiteral("tp "))
                                       || typeLower.contains(QStringLiteral("profit")));

                    auto parseTriggerPrice = [&](const QJsonObject &obj) -> double {
                        const QJsonValue trig =
                            obj.contains(QStringLiteral("trigger_price")) ? obj.value(QStringLiteral("trigger_price"))
                            : (obj.contains(QStringLiteral("triggerPrice")) ? obj.value(QStringLiteral("triggerPrice"))
                            : (obj.contains(QStringLiteral("stop_price")) ? obj.value(QStringLiteral("stop_price"))
                            : (obj.contains(QStringLiteral("stopPrice")) ? obj.value(QStringLiteral("stopPrice"))
                            : QJsonValue())));
                        double out = toDouble(trig);
                        if (out > 0.0) {
                            return out;
                        }
                        const QJsonValue trigInt =
                            obj.contains(QStringLiteral("trigger_price_int")) ? obj.value(QStringLiteral("trigger_price_int"))
                            : (obj.contains(QStringLiteral("triggerPriceInt")) ? obj.value(QStringLiteral("triggerPriceInt"))
                            : QJsonValue());
                        const qint64 trigScaled = trigInt.toVariant().toLongLong();
                        if (trigScaled > 0) {
                            const double denom = static_cast<double>(pow10i(priceDecimals));
                            if (denom > 0.0) {
                                return static_cast<double>(trigScaled) / denom;
                            }
                        }
                        return 0.0;
                    };

                    const double triggerPx = reduceOnly ? parseTriggerPrice(o) : 0.0;
                    const bool hasTrigger = triggerPx > 0.0;

                    // Prefer stable classification: explicit order_type or trigger_condition.
                    bool inferredSl = false;
                    bool inferredTp = false;
                    if (reduceOnly && hasTrigger) {
                        const TriggerCond cond = parseTriggerCond(o);
                        if (cond != TriggerCond::Unknown) {
                            if (askFlag != -1) {
                                const bool closesLong = (askFlag == 1);
                                inferredSl = closesLong ? (cond == TriggerCond::Below) : (cond == TriggerCond::Above);
                                inferredTp = closesLong ? (cond == TriggerCond::Above) : (cond == TriggerCond::Below);
                            } else if (hasPosition) {
                                if (pos.side == OrderSide::Buy) {
                                    inferredSl = (cond == TriggerCond::Below);
                                    inferredTp = (cond == TriggerCond::Above);
                                } else {
                                    inferredSl = (cond == TriggerCond::Above);
                                    inferredTp = (cond == TriggerCond::Below);
                                }
                            }
                        } else if (hasPosition) {
                            // Fallback: compare with entry avg. (Cannot represent "profit-lock" SL above entry.)
                            if (pos.side == OrderSide::Buy) {
                                inferredSl = triggerPx < pos.averagePrice;
                                inferredTp = triggerPx > pos.averagePrice;
                            } else {
                                inferredSl = triggerPx > pos.averagePrice;
                                inferredTp = triggerPx < pos.averagePrice;
                            }
                        }
                    }

                    Context::LighterStopKind stopKind = Context::LighterStopKind::Unknown;
                    if (reduceOnly && hasTrigger && (isStopLossOrder || isTakeProfitOrder || inferredSl || inferredTp)) {
                        const Context::LighterStopKind explicitKind =
                            isStopLossOrder ? Context::LighterStopKind::StopLoss
                                            : (isTakeProfitOrder ? Context::LighterStopKind::TakeProfit
                                                                 : Context::LighterStopKind::Unknown);
                        const Context::LighterStopKind inferredKind =
                            inferredSl ? Context::LighterStopKind::StopLoss
                                       : (inferredTp ? Context::LighterStopKind::TakeProfit
                                                     : Context::LighterStopKind::Unknown);
                        stopKind = resolveStopKind(*ctxPtr, orderId, explicitKind, inferredKind, createdMs);
                    }

                    const bool treatAsStop =
                        reduceOnly && hasTrigger && (stopKind == Context::LighterStopKind::StopLoss
                                                     || stopKind == Context::LighterStopKind::TakeProfit);
                    if (treatAsStop) {
                        bool ok = false;
                        const qint64 idNum = orderId.toLongLong(&ok);
                        if (ok && idNum > 0) {
                            const double p = triggerPx > 0.0 ? triggerPx : (price > 0.0 ? price : 0.0);
                            if (stopKind == Context::LighterStopKind::StopLoss) {
                                slStopIds.push_back(QString::number(idNum));
                                if (idNum > bestSlId) {
                                    bestSlId = idNum;
                                    if (p > 0.0) slPrice = p;
                                }
                            } else if (stopKind == Context::LighterStopKind::TakeProfit) {
                                tpStopIds.push_back(QString::number(idNum));
                                if (idNum > bestTpId) {
                                    bestTpId = idNum;
                                    if (p > 0.0) tpPrice = p;
                                }
                            }
                        }
                    }
                    // Keep regular order markers, but skip reduce-only stop orders (SL/TP) to avoid duplicates.
                    if (price > 0.0 && notional > 0.0 && !treatAsStop) {
                        OrderRecord rec;
                        rec.symbol = sym;
                        rec.side = side;
                        rec.price = price;
                        rec.quantityNotional = notional;
                        rec.createdMs = createdMs;
                        rec.orderId = orderId;
                        fetched.insert(orderId, rec);
                    }

                    if (treatAsStop) {
                        const double p = triggerPx > 0.0 ? triggerPx : (price > 0.0 ? price : 0.0);
                        if (stopKind == Context::LighterStopKind::StopLoss) {
                            hasSl = true;
                            if (slPrice <= 0.0 && p > 0.0) slPrice = p;
                        } else if (stopKind == Context::LighterStopKind::TakeProfit) {
                            hasTp = true;
                            if (tpPrice <= 0.0 && p > 0.0) tpPrice = p;
                        }
                    }
                }
                if (bestSlId > 0) {
                    hasSl = true;
                }
                if (bestTpId > 0) {
                    hasTp = true;
                }
                slStopIds.removeDuplicates();
                tpStopIds.removeDuplicates();

                // Cleanup: if multiple reduce-only stop orders exist (same type), cancel extras and keep only the newest.
                QSet<QString> liveStopIds;
                for (const auto &id : slStopIds) {
                    if (!id.trimmed().isEmpty()) liveStopIds.insert(id.trimmed());
                }
                for (const auto &id : tpStopIds) {
                    if (!id.trimmed().isEmpty()) liveStopIds.insert(id.trimmed());
                }
                // Drop pending-cancel ids that are no longer present.
                if (!ctxPtr->lighterPendingCancelOrderIds.isEmpty()) {
                    QSet<QString> filtered;
                    filtered.reserve(ctxPtr->lighterPendingCancelOrderIds.size());
                    for (const auto &id : ctxPtr->lighterPendingCancelOrderIds) {
                        if (liveStopIds.contains(id)) {
                            filtered.insert(id);
                        }
                    }
                    ctxPtr->lighterPendingCancelOrderIds = std::move(filtered);
                }

                auto cancelExtras = [this, ctxPtr, sym](const QStringList &ids, qint64 keepId) {
                    if (!ctxPtr) {
                        return;
                    }
                    QString keepStr;
                    if (keepId > 0) {
                        keepStr = QString::number(keepId);
                    }
                    for (const auto &idRaw : ids) {
                        const QString id = idRaw.trimmed();
                        if (id.isEmpty() || id == keepStr) {
                            continue;
                        }
                        if (ctxPtr->lighterPendingCancelOrderIds.contains(id)) {
                            continue;
                        }
                        ctxPtr->lighterPendingCancelOrderIds.insert(id);
                        cancelOrder(sym, ctxPtr->accountName, id);
                    }
                };
                if (slStopIds.size() > 1 && bestSlId > 0) {
                    cancelExtras(slStopIds, bestSlId);
                    slStopIds = {QString::number(bestSlId)};
                }
                if (tpStopIds.size() > 1 && bestTpId > 0) {
                    cancelExtras(tpStopIds, bestTpId);
                    tpStopIds = {QString::number(bestTpId)};
                }

                ctxPtr->lighterSlStopOrderIdsBySymbol.insert(sym, slStopIds);
                ctxPtr->lighterTpStopOrderIdsBySymbol.insert(sym, tpStopIds);

                QList<OrderRecord> removed;
                for (auto it = ctxPtr->activeOrders.constBegin(); it != ctxPtr->activeOrders.constEnd(); ++it) {
                    if (it.value().symbol == sym && !fetched.contains(it.key())) {
                        OrderRecord rec = it.value();
                        rec.orderId = it.key();
                        removed.push_back(rec);
                    }
                }
                for (const auto &rec : removed) {
                    ctxPtr->activeOrders.remove(rec.orderId);
                    emit orderCanceled(ctxPtr->accountName, sym, rec.side, rec.price, rec.orderId);
                }
                for (auto it = fetched.constBegin(); it != fetched.constEnd(); ++it) {
                    ctxPtr->activeOrders.insert(it.key(), it.value());
                }

                QVector<DomWidget::LocalOrderMarker> markers;
                markers.reserve(fetched.size());
                for (auto it = fetched.constBegin(); it != fetched.constEnd(); ++it) {
                    const auto &rec = it.value();
                    DomWidget::LocalOrderMarker marker;
                    marker.price = rec.price;
                    marker.quantity = rec.quantityNotional;
                    marker.side = rec.side;
                    marker.createdMs = rec.createdMs;
                    marker.orderId = it.key();
                    markers.push_back(marker);
                }
                emit localOrdersUpdated(ctxPtr->accountName, sym, markers);
                emit lighterStopOrdersUpdated(ctxPtr->accountName, sym, hasSl, slPrice, hasTp, tpPrice);
                done();
            });
        };

        (*doRequest)(QStringLiteral("market_id"), true);
    });
}

void TradeManager::fetchMyTrades(Context &ctx, const QString &symbolUpper)
{
    const QString sym = normalizedSymbol(symbolUpper);
    if (sym.isEmpty()) {
        return;
    }
    ctx.myTradesInFlight.insert(sym);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("symbol"), sym);
    query.addQueryItem(QStringLiteral("limit"), QStringLiteral("50"));
    const qint64 lastId = ctx.lastTradeIdBySymbol.value(sym, 0);
    if (lastId > 0) {
        query.addQueryItem(QStringLiteral("fromId"), QString::number(lastId + 1));
    }
    query.addQueryItem(QStringLiteral("recvWindow"), QStringLiteral("5000"));
    query.addQueryItem(QStringLiteral("timestamp"),
                       QString::number(QDateTime::currentMSecsSinceEpoch()));

    QUrlQuery signedQuery = query;
    signedQuery.addQueryItem(QStringLiteral("signature"),
                             QString::fromLatin1(signPayload(query, ctx)));
    QNetworkRequest request = makePrivateRequest(QStringLiteral("/api/v3/myTrades"),
                                                 signedQuery,
                                                 QByteArray(),
                                                 ctx);

    auto *reply = ensureMexcNetwork(ctx)->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, ctxPtr = &ctx, sym]() {
        ctxPtr->myTradesInFlight.remove(sym);
        const QNetworkReply::NetworkError err = reply->error();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        reply->deleteLater();
        if (err != QNetworkReply::NoError || status >= 400) {
            const QString msg = status >= 400 ? QStringLiteral("HTTP %1: %2")
                                                    .arg(status)
                                                    .arg(QString::fromUtf8(raw))
                                              : reply->errorString();
            emit logMessage(QStringLiteral("%1 myTrades failed for %2: %3")
                                .arg(contextTag(ctxPtr->accountName), sym, msg));
            return;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(raw);
        if (!doc.isArray()) {
            return;
        }
        const QJsonArray arr = doc.array();
        if (arr.isEmpty()) {
            return;
        }
        auto toDouble = [](const QJsonValue &v) -> double {
            if (v.isDouble()) return v.toDouble();
            if (v.isString()) return v.toString().toDouble();
            return v.toVariant().toDouble();
        };
        auto toInt64 = [](const QJsonValue &v) -> qint64 {
            if (v.isDouble()) return static_cast<qint64>(v.toDouble());
            if (v.isString()) return v.toString().toLongLong();
            return v.toVariant().toLongLong();
        };
        qint64 maxId = ctxPtr->lastTradeIdBySymbol.value(sym, 0);
        for (const auto &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            const qint64 id = toInt64(o.value(QStringLiteral("id")));
            if (id <= 0) continue;
            maxId = std::max(maxId, id);
            const double price = toDouble(o.value(QStringLiteral("price")));
            const double qty = toDouble(o.value(QStringLiteral("qty")));
            if (!(price > 0.0) || !(qty > 0.0)) continue;
            const bool isBuyer = o.value(QStringLiteral("isBuyer")).toBool(true);
            const OrderSide side = isBuyer ? OrderSide::Buy : OrderSide::Sell;
            double closedNotional = 0.0;
            const double realizedDelta = handleOrderFill(*ctxPtr, sym, side, price, qty, &closedNotional);

            ExecutedTrade trade;
            trade.accountName = ctxPtr->accountName;
            trade.symbol = sym;
            trade.side = side;
            trade.price = price;
            trade.quantity = qty;
            trade.feeAmount = toDouble(o.value(QStringLiteral("commission")));
            trade.feeCurrency = o.value(QStringLiteral("commissionAsset")).toString();
            trade.realizedPnl = realizedDelta;
            trade.realizedPct = (closedNotional > 0.0) ? (realizedDelta / closedNotional) * 100.0 : 0.0;
            trade.timeMs = toInt64(o.value(QStringLiteral("time")));
            if (trade.timeMs <= 0) {
                trade.timeMs = QDateTime::currentMSecsSinceEpoch();
            }
            appendTradeHistory(trade);
            recordFinrezTrade(*ctxPtr, trade);
            emit tradeExecuted(trade);
        }
        if (maxId > 0) {
            ctxPtr->lastTradeIdBySymbol.insert(sym, maxId);
        }
    });
}

TradeManager::Context *TradeManager::contextForProfile(ConnectionStore::Profile profile) const
{
    return m_contexts.value(profile, nullptr);
}

TradeManager::Context &TradeManager::ensureContext(ConnectionStore::Profile profile) const
{
    if (Context *ctx = contextForProfile(profile)) {
        return *ctx;
    }
    auto *ctx = new Context;
    ctx->profile = profile;
    ctx->accountName = defaultAccountName(profile);

    auto *self = const_cast<TradeManager *>(this);
    ctx->keepAliveTimer.setParent(self);
    ctx->wsPingTimer.setParent(self);
    ctx->reconnectTimer.setParent(self);
    ctx->myTradesTimer.setParent(self);
    ctx->futuresDealsTimer.setParent(self);
    ctx->lighterAccountTimer.setParent(self);
    ctx->lighterTradesTimer.setParent(self);
    ctx->lighterBurstTimer.setParent(self);
    ctx->privateSocket.setParent(self);

    ctx->keepAliveTimer.setInterval(25 * 60 * 1000);
    self->connect(&ctx->keepAliveTimer, &QTimer::timeout, self, [self, ctx]() {
        self->sendListenKeyKeepAlive(*ctx);
    });

    ctx->wsPingTimer.setInterval(45 * 1000);
    self->connect(&ctx->wsPingTimer, &QTimer::timeout, self, [ctx]() {
        if (!ctx->privateSocket.isValid()) {
            return;
        }
        ctx->lastPrivatePingSentMs = QDateTime::currentMSecsSinceEpoch();
        if (ctx->profile == ConnectionStore::Profile::MexcFutures) {
            ctx->privateSocket.sendTextMessage(QStringLiteral("{\"method\":\"ping\"}"));
        } else {
            ctx->privateSocket.ping();
        }
    });

    ctx->reconnectTimer.setSingleShot(true);
    ctx->reconnectTimer.setInterval(3000);
    self->connect(&ctx->reconnectTimer, &QTimer::timeout, self, [self, ctx]() {
        if (ctx->state == ConnectionState::Disconnected || ctx->state == ConnectionState::Error) {
            emit self->logMessage(QStringLiteral("%1 Reconnecting private WebSocket...")
                                      .arg(contextTag(ctx->accountName)));
            self->connectToExchange(ctx->profile);
        }
    });

    ctx->openOrdersTimer.setSingleShot(false);
    ctx->openOrdersTimer.setInterval(4000);
    self->connect(&ctx->openOrdersTimer, &QTimer::timeout, self, [self, ctx]() {
        self->fetchOpenOrders(*ctx);
        if (ctx->profile == ConnectionStore::Profile::UzxSwap) {
            self->fetchUzxSwapPositions(*ctx);
        }
    });

    ctx->myTradesTimer.setSingleShot(false);
    ctx->myTradesTimer.setInterval(1500);
    self->connect(&ctx->myTradesTimer, &QTimer::timeout, self, [self, ctx]() {
        self->pollMyTrades(*ctx);
    });

    ctx->futuresDealsTimer.setSingleShot(false);
    ctx->futuresDealsTimer.setInterval(1500);
    self->connect(&ctx->futuresDealsTimer, &QTimer::timeout, self, [self, ctx]() {
        self->pollFuturesDeals(*ctx);
    });

    ctx->lighterAccountTimer.setSingleShot(false);
    ctx->lighterAccountTimer.setInterval(kLighterAccountPollMs);
    self->connect(&ctx->lighterAccountTimer, &QTimer::timeout, self, [self, ctx]() {
        self->pollLighterAccount(*ctx);
    });

    ctx->lighterTradesTimer.setSingleShot(false);
    ctx->lighterTradesTimer.setInterval(kLighterTradesPollMs);
    self->connect(&ctx->lighterTradesTimer, &QTimer::timeout, self, [self, ctx]() {
        self->pollLighterTrades(*ctx);
    });

    ctx->lighterBurstTimer.setSingleShot(false);
    ctx->lighterBurstTimer.setInterval(500);
    self->connect(&ctx->lighterBurstTimer, &QTimer::timeout, self, [self, ctx]() {
        if (ctx->lighterBurstRemaining <= 0) {
            ctx->lighterBurstTimer.stop();
            return;
        }
        --ctx->lighterBurstRemaining;
        // Fast UI sync after order actions. Keep it light to avoid rate limits:
        // poll active orders every tick, and account less frequently.
        self->pollLighterActiveOrders(*ctx);
        if ((ctx->lighterBurstRemaining % 4) == 0) {
            self->pollLighterAccount(*ctx);
        }
        if (ctx->lighterBurstRemaining <= 0) {
            ctx->lighterBurstTimer.stop();
        }
    });

    self->connect(&ctx->privateSocket, &QWebSocket::connected, self, [self, ctx]() {
        if (ctx->profile == ConnectionStore::Profile::UzxSwap
            || ctx->profile == ConnectionStore::Profile::UzxSpot) {
            emit self->logMessage(QStringLiteral("%1 UZX private WebSocket connected.")
                                      .arg(contextTag(ctx->accountName)));
            self->subscribeUzxPrivate(*ctx);
            if (ctx->reconnectTimer.isActive()) {
                ctx->reconnectTimer.stop();
            }
            self->setState(*ctx, ConnectionState::Connecting, tr("Authenticating..."));
            return;
        }
        if (ctx->profile == ConnectionStore::Profile::MexcFutures) {
            emit self->logMessage(QStringLiteral("%1 Futures WebSocket connected.")
                                      .arg(contextTag(ctx->accountName)));
            if (ctx->reconnectTimer.isActive()) {
                ctx->reconnectTimer.stop();
            }
            if (!ctx->wsPingTimer.isActive()) {
                ctx->wsPingTimer.start();
            }
            const qint64 reqTime = QDateTime::currentMSecsSinceEpoch();
            const QByteArray payload =
                ctx->credentials.apiKey.toUtf8() + QByteArray::number(reqTime);
            const QByteArray signature =
                QMessageAuthenticationCode::hash(payload,
                                                  ctx->credentials.secretKey.toUtf8(),
                                                  QCryptographicHash::Sha256)
                    .toHex();
            QJsonObject params;
            params.insert(QStringLiteral("apiKey"), ctx->credentials.apiKey);
            params.insert(QStringLiteral("reqTime"), static_cast<qint64>(reqTime));
            params.insert(QStringLiteral("signature"), QString::fromUtf8(signature));
            QJsonObject loginPayload;
            loginPayload.insert(QStringLiteral("method"), QStringLiteral("login"));
            loginPayload.insert(QStringLiteral("param"), params);
            loginPayload.insert(QStringLiteral("subscribe"), false);
            ctx->privateSocket.sendTextMessage(QString::fromUtf8(
                QJsonDocument(loginPayload).toJson(QJsonDocument::Compact)));
            self->setState(*ctx, ConnectionState::Connecting, tr("Authenticating..."));
            return;
        }
        emit self->logMessage(QStringLiteral("%1 Private WebSocket connected.").arg(contextTag(ctx->accountName)));
        self->subscribePrivateChannels(*ctx);
        self->sendListenKeyKeepAlive(*ctx);
        if (!ctx->keepAliveTimer.isActive()) {
            ctx->keepAliveTimer.start();
        }
        if (!ctx->wsPingTimer.isActive()) {
            ctx->wsPingTimer.start();
        }
        if (!ctx->myTradesTimer.isActive() && !ctx->watchedSymbols.isEmpty()) {
            ctx->myTradesTimer.start();
        }
        if (!ctx->futuresDealsTimer.isActive()
            && ctx->profile == ConnectionStore::Profile::MexcFutures
            && !ctx->watchedSymbols.isEmpty()) {
            ctx->futuresDealsTimer.start();
        }
        if (ctx->reconnectTimer.isActive()) {
            ctx->reconnectTimer.stop();
        }
        self->setState(*ctx, ConnectionState::Connected, tr("Connected to private WebSocket"));
    });

    self->connect(&ctx->privateSocket,
                  &QWebSocket::disconnected,
                  self,
                  [self, ctx]() {
                      ctx->keepAliveTimer.stop();
                      ctx->wsPingTimer.stop();
                      ctx->myTradesTimer.stop();
                      ctx->futuresDealsTimer.stop();
                      if (ctx->closingSocket) {
                          ctx->closingSocket = false;
                          emit self->logMessage(QStringLiteral("%1 Private WebSocket closed.")
                                                    .arg(contextTag(ctx->accountName)));
                          return;
                      }
                      emit self->logMessage(QStringLiteral(
                                                "%1 Private WebSocket disconnected unexpectedly. code=%2 reason=%3")
                                                .arg(contextTag(ctx->accountName))
                                                .arg(static_cast<int>(ctx->privateSocket.closeCode()))
                                                .arg(ctx->privateSocket.closeReason()));
                      self->setState(*ctx, ConnectionState::Error, tr("WebSocket disconnected"));
                      self->scheduleReconnect(*ctx);
                  });

    self->connect(&ctx->privateSocket,
                  QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::errorOccurred),
                  self,
                  [self, ctx](QAbstractSocket::SocketError) {
                      if (ctx->closingSocket) {
                          return;
                      }
                      emit self->logMessage(QStringLiteral("%1 WebSocket error: %2")
                                                .arg(contextTag(ctx->accountName))
                                                .arg(ctx->privateSocket.errorString()));
                      if (ctx->state != ConnectionState::Error) {
                           self->setState(*ctx, ConnectionState::Error, ctx->privateSocket.errorString());
                      }
                      self->scheduleReconnect(*ctx);
                  });
    self->connect(&ctx->privateSocket,
                  &QWebSocket::pong,
                  self,
                  [self, ctx](quint64 elapsedMs, const QByteArray &) {
                      ctx->lastPrivatePingMs = static_cast<int>(elapsedMs);
                      emit self->privatePingUpdated(ctx->accountName, ctx->lastPrivatePingMs);
                  });
    self->connect(&ctx->privateSocket,
                  &QWebSocket::textMessageReceived,
                  self,
                  [self, ctx](const QString &message) {
                      const auto profile = ctx->profile;
                      const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
                      if (!doc.isObject()) {
                          emit self->logMessage(QStringLiteral("%1 WS text: %2")
                                              .arg(contextTag(ctx->accountName), message));
                          return;
                      }
                      const QJsonObject obj = doc.object();
                      if (profile == ConnectionStore::Profile::MexcFutures) {
                          const QString channel = obj.value(QStringLiteral("channel")).toString();
                          if (channel.compare(QStringLiteral("pong"), Qt::CaseInsensitive) == 0) {
                              if (ctx->lastPrivatePingSentMs > 0) {
                                  const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                                  const qint64 delta = std::max<qint64>(0, nowMs - ctx->lastPrivatePingSentMs);
                                  ctx->lastPrivatePingMs = static_cast<int>(delta);
                                  emit self->privatePingUpdated(ctx->accountName, ctx->lastPrivatePingMs);
                              }
                              return;
                          }
                          if (channel == QStringLiteral("rs.login")) {
                              bool ok = false;
                              const QJsonValue dataVal = obj.value(QStringLiteral("data"));
                              if (dataVal.isString()) {
                                  ok = dataVal.toString().compare(QStringLiteral("success"),
                                                                  Qt::CaseInsensitive)
                                       == 0;
                              } else if (dataVal.isObject()) {
                                  ok = dataVal.toObject().value(QStringLiteral("code")).toInt(0) == 0;
                              }
                              if (!ok) {
                                  const QString msg =
                                      dataVal.isString()
                                          ? dataVal.toString()
                                          : dataVal.toObject()
                                                .value(QStringLiteral("msg"))
                                                .toString(obj.value(QStringLiteral("msg"))
                                                              .toString(tr("Login failed")));
                                  emit self->logMessage(QStringLiteral("%1 Futures login failed: %2")
                                                            .arg(contextTag(ctx->accountName), msg));
                                  self->setState(*ctx, ConnectionState::Error, msg);
                                  self->scheduleReconnect(*ctx);
                              } else {
                                  ctx->futuresLoggedIn = true;
                                  emit self->logMessage(QStringLiteral(
                                                            "%1 Futures login ok. Requesting filter...")
                                                            .arg(contextTag(ctx->accountName)));
                                  QJsonObject filterMsg;
                                  filterMsg.insert(QStringLiteral("method"),
                                                   QStringLiteral("personal.filter"));
                                  QJsonObject param;
                                  param.insert(QStringLiteral("filters"), QJsonArray());
                                  filterMsg.insert(QStringLiteral("param"), param);
                                  ctx->privateSocket.sendTextMessage(QString::fromUtf8(
                                      QJsonDocument(filterMsg).toJson(QJsonDocument::Compact)));
                              }
                              return;
                          }
                          if (channel == QStringLiteral("rs.personal.filter")) {
                              bool ok = false;
                              const QJsonValue dataVal = obj.value(QStringLiteral("data"));
                              if (dataVal.isString()) {
                                  ok = dataVal.toString().compare(QStringLiteral("success"),
                                                                  Qt::CaseInsensitive)
                                       == 0;
                              } else if (dataVal.isObject()) {
                                  ok = dataVal.toObject().value(QStringLiteral("code")).toInt(0) == 0;
                              }
                              if (!ok) {
                                  const QString msg =
                                      dataVal.isString()
                                          ? dataVal.toString()
                                          : dataVal.toObject()
                                                .value(QStringLiteral("msg"))
                                                .toString(obj.value(QStringLiteral("msg"))
                                                              .toString(tr("Subscription failed")));
                                  emit self->logMessage(QStringLiteral("%1 Futures filter failed: %2")
                                                            .arg(contextTag(ctx->accountName), msg));
                                  self->setState(*ctx, ConnectionState::Error, msg);
                                  self->scheduleReconnect(*ctx);
                              } else {
                                  ctx->hasSubscribed = true;
                                  emit self->logMessage(QStringLiteral(
                                                            "%1 Subscribed to futures private channels.")
                                                            .arg(contextTag(ctx->accountName)));
                                  self->setState(*ctx,
                                                 ConnectionState::Connected,
                                                 tr("Connected to MEXC futures"));
                                  self->fetchOpenOrders(*ctx);
                                  self->fetchFuturesPositions(*ctx);
                                  if (!ctx->openOrdersTimer.isActive()) {
                                      ctx->openOrdersTimer.start();
                                  }
                                  if (!ctx->futuresDealsTimer.isActive() && !ctx->watchedSymbols.isEmpty()) {
                                      ctx->futuresDealsTimer.start();
                                  }
                              }
                              return;
                          }
                          if (channel == QStringLiteral("rs.error")) {
                              emit self->logMessage(QStringLiteral("%1 Futures WS error: %2")
                                                        .arg(contextTag(ctx->accountName), message));
                              return;
                          }
                          if (channel.startsWith(QStringLiteral("push.personal"))) {
                              if (channel == QStringLiteral("push.personal.order")
                                  || channel == QStringLiteral("push.personal.order.deal")
                                  || channel == QStringLiteral("push.personal.stop.order")
                                  || channel == QStringLiteral("push.personal.stop.planorder")) {
                                  self->fetchOpenOrders(*ctx);
                              }
                              if (channel == QStringLiteral("push.personal.position")
                                  || channel == QStringLiteral("push.personal.asset")
                                  || channel == QStringLiteral("push.personal.liquidate.risk")) {
                                  self->fetchFuturesPositions(*ctx);
                              }
                              return;
                          }
                          emit self->logMessage(QStringLiteral("%1 Futures WS event: %2")
                                                    .arg(contextTag(ctx->accountName), message));
                          return;
                      }
                      if (profile == ConnectionStore::Profile::UzxSwap
                          || profile == ConnectionStore::Profile::UzxSpot) {
                          if (obj.contains(QStringLiteral("ping"))) {
                              QJsonObject pong;
                              pong.insert(QStringLiteral("pong"), obj.value(QStringLiteral("ping")));
                              ctx->privateSocket.sendTextMessage(
                                  QString::fromUtf8(
                                      QJsonDocument(pong).toJson(QJsonDocument::Compact)));
                              return;
                          }
                          const QString event = obj.value(QStringLiteral("event")).toString();
                          if (event.compare(QStringLiteral("login"), Qt::CaseInsensitive) == 0) {
                              const QString status = obj.value(QStringLiteral("status")).toString();
                              if (status.compare(QStringLiteral("success"), Qt::CaseInsensitive) != 0) {
                                  const QString msg = obj.value(QStringLiteral("msg"))
                                                            .toString(
                                                                obj.value(QStringLiteral("message"))
                                                                    .toString(status));
                                  emit self->logMessage(QStringLiteral("%1 UZX login failed: %2")
                                                      .arg(contextTag(ctx->accountName), msg));
                                  self->setState(*ctx, ConnectionState::Error, msg);
                                  ctx->hasSubscribed = false;
                                  self->closeWebSocket(*ctx);
                              } else {
                                  emit self->logMessage(QStringLiteral("%1 UZX login response: %2")
                                                      .arg(contextTag(ctx->accountName), message));
                                  QJsonObject subParams;
                                  subParams.insert(QStringLiteral("biz"), QStringLiteral("private"));
                                  subParams.insert(QStringLiteral("type"),
                                                   profile == ConnectionStore::Profile::UzxSpot
                                                       ? QStringLiteral("order.spot")
                                                       : QStringLiteral("order.swap"));
                                  QJsonObject subPayload;
                                  subPayload.insert(QStringLiteral("event"), QStringLiteral("sub"));
                                  subPayload.insert(QStringLiteral("params"), subParams);
                                  subPayload.insert(QStringLiteral("zip"), false);
                                  ctx->privateSocket.sendTextMessage(
                                      QString::fromUtf8(QJsonDocument(subPayload)
                                                            .toJson(QJsonDocument::Compact)));
                                  emit self->logMessage(QStringLiteral(
                                                      "%1 Subscribed to UZX private order updates.")
                                                      .arg(contextTag(ctx->accountName)));
                                  ctx->hasSubscribed = true;
                                  self->setState(*ctx,
                                           ConnectionState::Connected,
                                           QStringLiteral("UZX authenticated"));
                                  self->fetchOpenOrders(*ctx);
                                  if (profile == ConnectionStore::Profile::UzxSwap) {
                                      self->fetchUzxSwapPositions(*ctx);
                                  }
                                  if (!ctx->openOrdersTimer.isActive()) {
                                      ctx->openOrdersTimer.start();
                                  }
                              }
                              return;
                          }
                      const QString type = obj.value(QStringLiteral("type")).toString();
                      if (type.endsWith(QStringLiteral(".assets"), Qt::CaseInsensitive)) {
                          const QString asset = obj.value(QStringLiteral("name")).toString().trimmed().toUpper();
                          const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
                          const auto toDouble = [&](const QJsonValue &v) -> double {
                              if (v.isString()) return v.toString().toDouble();
                              if (v.isDouble()) return v.toDouble();
                              if (v.isObject() || v.isArray()) return 0.0;
                              return v.toVariant().toDouble();
                          };
                          if (!asset.isEmpty()) {
                              BalanceState &st = ctx->balances[asset];
                              st.available = toDouble(data.value(QStringLiteral("balance")));
                              st.locked = toDouble(data.value(QStringLiteral("entrust_margin")));
                              if (st.locked <= 0.0) {
                                  st.locked = toDouble(data.value(QStringLiteral("frozen")));
                              }
                              if (st.locked <= 0.0) {
                                  st.locked = toDouble(data.value(QStringLiteral("freeze_amount")));
                              }
                              st.updatedMs = QDateTime::currentMSecsSinceEpoch();
                              emit self->finrezChanged(ctx->profile);
                          }
                          return;
                      }
                      const bool isOrder = type == QStringLiteral("order.swap")
                                           || type == QStringLiteral("order.spot")
                                           || type == QStringLiteral("orderV2.spot")
                                             || type == QStringLiteral("orderV2.swap")
                                             || type == QStringLiteral("orderV2.base");
                        if (isOrder) {
                            const QString name = obj.value(QStringLiteral("name")).toString();
                            const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
                            const QString insType = data.value(QStringLiteral("ins_type")).toString().trimmed().toUpper();
                            bool isSpotMsg = false;
                            bool isSwapMsg = false;
                            if (!insType.isEmpty()) {
                                if (insType == QStringLiteral("SPOT")) {
                                    isSpotMsg = true;
                                } else if (insType == QStringLiteral("SWAP")) {
                                    isSwapMsg = true;
                                }
                                if (profile == ConnectionStore::Profile::UzxSpot
                                    && insType == QStringLiteral("SWAP")) {
                                    return;
                                }
                                if (profile == ConnectionStore::Profile::UzxSwap
                                    && insType == QStringLiteral("SPOT")) {
                                    return;
                                }
                            } else {
                                const bool swapType = type.contains(QStringLiteral("swap"), Qt::CaseInsensitive);
                                const bool spotType = type.contains(QStringLiteral("spot"), Qt::CaseInsensitive);
                                if (swapType) {
                                    isSwapMsg = true;
                                }
                                if (spotType) {
                                    isSpotMsg = true;
                                }
                                if (profile == ConnectionStore::Profile::UzxSpot && swapType) {
                                    return;
                                }
                                if (profile == ConnectionStore::Profile::UzxSwap && spotType) {
                                    return;
                                }
                            }
                            const auto toDouble = [&](const QJsonValue &v) -> double {
                                if (v.isString()) return v.toString().toDouble();
                                if (v.isDouble()) return v.toDouble();
                                if (v.isObject() || v.isArray()) return 0.0;
                                return v.toVariant().toDouble();
                            };
                            const double price = toDouble(data.value(QStringLiteral("price")));
                            const double cumulativeFilledRaw =
                                toDouble(data.value(QStringLiteral("deal_number")));
                            emit self->logMessage(QStringLiteral("%1 UZX order update %2: %3")
                                                .arg(contextTag(ctx->accountName))
                                                .arg(name,
                                                     QString::fromUtf8(
                                                         QJsonDocument(data)
                                                             .toJson(QJsonDocument::Compact))));
                            auto orderIdFromValue = [](const QJsonValue &v) -> QString {
                                if (v.isString()) return v.toString().trimmed();
                                if (v.isDouble()) return QString::number(static_cast<qlonglong>(v.toDouble()));
                                return v.toVariant().toString().trimmed();
                            };
                            QString orderId = orderIdFromValue(data.value(QStringLiteral("order_id")));
                            if (orderId.isEmpty()) {
                                orderId = orderIdFromValue(data.value(QStringLiteral("orderId")));
                            }
                            const int sideFlag = data.value(QStringLiteral("order_buy_or_sell")).toInt(1);
                            const OrderSide side = sideFlag == 2 ? OrderSide::Sell : OrderSide::Buy;
                            double remain = 0.0;
                            if (isSpotMsg) {
                                remain = toDouble(data.value(QStringLiteral("un_filled_amount")));
                            } else if (isSwapMsg) {
                                remain = toDouble(data.value(QStringLiteral("un_filled_number")));
                            } else {
                                remain = toDouble(data.value(QStringLiteral("un_filled_amount")));
                                if (remain <= 0.0) {
                                    remain = toDouble(data.value(QStringLiteral("un_filled_number")));
                                }
                            }
                          const int statusVal = data.value(QStringLiteral("status")).toInt(
                              data.value(QStringLiteral("state")).toInt(-1));
                          const bool terminal =
                              statusVal == 2 || statusVal == 3 || statusVal == 4 || statusVal == 5;
                          const bool isUzxProfile =
                              ctx->profile == ConnectionStore::Profile::UzxSpot
                              || ctx->profile == ConnectionStore::Profile::UzxSwap;
                          const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                          if (terminal && isUzxProfile && !orderId.isEmpty()) {
                              const qint64 seenMs = ctx->uzxTerminalOrderSeenMs.value(orderId, 0);
                              if (seenMs > 0 && nowMs - seenMs < 5000) {
                                  return;
                              }
                              ctx->uzxTerminalOrderSeenMs.insert(orderId, nowMs);
                          }
                          double cumulativeFilled = 0.0;
                          if (isSpotMsg) {
                              const double filledAmountRaw =
                                  toDouble(data.value(QStringLiteral("filled_amount")));
                              const double filledQuote =
                                  toDouble(data.value(QStringLiteral("filled_quote_amount")));
                              if (filledAmountRaw > 0.0) {
                                  cumulativeFilled = filledAmountRaw;
                              } else if (filledQuote > 0.0 && price > 0.0) {
                                  cumulativeFilled = filledQuote / price;
                              }
                          } else {
                              cumulativeFilled = cumulativeFilledRaw;
                              if (cumulativeFilled <= 0.0) {
                                  cumulativeFilled = toDouble(data.value(QStringLiteral("filled_number")));
                              }
                              if (cumulativeFilled <= 0.0) {
                                  cumulativeFilled = toDouble(data.value(QStringLiteral("filled_amount")));
                              }
                          }
                          if (profile == ConnectionStore::Profile::UzxSpot) {
                              const auto toLongLong = [&](const QJsonValue &v) -> qint64 {
                                  if (v.isString()) {
                                      return v.toString().toLongLong();
                                  }
                                  if (v.isDouble()) {
                                      return static_cast<qint64>(v.toDouble());
                                  }
                                  return v.toVariant().toLongLong();
                              };
                              const qint64 createdAt = toLongLong(data.value(QStringLiteral("created_at")));
                              const qint64 updatedAt = toLongLong(data.value(QStringLiteral("updated_at")));
                              double totalAmount = toDouble(data.value(QStringLiteral("amount")));
                              if (totalAmount <= 0.0) {
                                  totalAmount = toDouble(data.value(QStringLiteral("number")));
                              }
                              const QString fp = QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                                                     .arg(type)
                                                     .arg(name)
                                                     .arg(sideFlag)
                                                     .arg(price, 0, 'f', 8)
                                                     .arg(totalAmount, 0, 'f', 8)
                                                     .arg(cumulativeFilled, 0, 'f', 8)
                                                     .arg(remain, 0, 'f', 8)
                                                     .arg(statusVal)
                                                     .arg(updatedAt > 0 ? updatedAt : createdAt);
                              const qint64 lastMs = ctx->uzxRecentOrderFingerprintsMs.value(fp, 0);
                              if (lastMs > 0 && nowMs - lastMs < 1500) {
                                  return;
                              }
                              ctx->uzxRecentOrderFingerprintsMs.insert(fp, nowMs);
                              if (ctx->uzxRecentOrderFingerprintsMs.size() > 800) {
                                  const qint64 cutoff = nowMs - 20000;
                                  for (auto it = ctx->uzxRecentOrderFingerprintsMs.begin();
                                       it != ctx->uzxRecentOrderFingerprintsMs.end();) {
                                      if (it.value() < cutoff) {
                                          it = ctx->uzxRecentOrderFingerprintsMs.erase(it);
                                      } else {
                                          ++it;
                                      }
                                  }
                              }
                          }
                          double deltaFilled = 0.0;
                          if (!orderId.isEmpty() && cumulativeFilled > 0.0) {
                              const double prev =
                                  ctx->uzxLastFilledByOrderId.value(orderId, 0.0);
                              if (cumulativeFilled > prev) {
                                  deltaFilled = cumulativeFilled - prev;
                                  ctx->uzxLastFilledByOrderId.insert(orderId, cumulativeFilled);
                              }
                          }
                            const QString normalized = uzxUserSymbol(name);
                            if (remain <= 0.0) {
                                double total = 0.0;
                                if (isSpotMsg) {
                                    total = toDouble(data.value(QStringLiteral("amount")));
                                } else if (isSwapMsg) {
                                    total = toDouble(data.value(QStringLiteral("number")));
                                } else {
                                    total = toDouble(data.value(QStringLiteral("amount")));
                                    if (total <= 0.0) {
                                        total = toDouble(data.value(QStringLiteral("number")));
                                    }
                                }
                                if (total > 0.0) {
                                    if (cumulativeFilled > 0.0 && cumulativeFilled < total) {
                                        remain = total - cumulativeFilled;
                                    } else {
                                        remain = total;
                                    }
                                }
                            }
                            if (!orderId.isEmpty() && !normalized.isEmpty()) {
                                if (price > 0.0 && remain > 0.0 && !terminal) {
                                    OrderRecord record;
                                    record.symbol = normalized;
                                    record.side = side;
                                    record.price = price;
                                    record.quantityNotional = price * remain;
                                    record.createdMs = QDateTime::currentMSecsSinceEpoch();
                                    record.orderId = orderId;
                                    ctx->activeOrders.insert(orderId, record);
                              } else {
                                  ctx->activeOrders.remove(orderId);
                              }
                              self->emitLocalOrderSnapshot(*ctx, normalized);
                          }
                          if (deltaFilled > 0.0 && price > 0.0) {
                              double closedNotional = 0.0;
                              const double realizedDelta =
                                  self->handleOrderFill(*ctx, normalized, side, price, deltaFilled, &closedNotional);
                                  ExecutedTrade trade;
                                  trade.accountName = ctx->accountName;
                                  trade.symbol = normalized;
                                  trade.side = side;
                                  trade.price = price;
                                  trade.quantity = deltaFilled;
                                  trade.realizedPnl = realizedDelta;
                                  trade.realizedPct =
                                      (closedNotional > 0.0) ? (realizedDelta / closedNotional) * 100.0 : 0.0;
                                  trade.timeMs = QDateTime::currentMSecsSinceEpoch();
                                  self->appendTradeHistory(trade);
                                  emit self->tradeExecuted(trade);
                              }
                          if (terminal || remain <= 0.0) {
                              if (!orderId.isEmpty() && !isUzxProfile) {
                                  ctx->uzxLastFilledByOrderId.remove(orderId);
                              }
                              QString wsOrderId = orderId;
                              if (wsOrderId.isEmpty()) {
                                  wsOrderId = data.value(QStringLiteral("orderId")).toString();
                              }
                              if (wsOrderId.isEmpty()) {
                                  wsOrderId = data.value(QStringLiteral("clientOrderId")).toString();
                              }
                              if (wsOrderId.isEmpty()) {
                                  wsOrderId = QStringLiteral("uzx_ws_%1")
                                                  .arg(QDateTime::currentMSecsSinceEpoch());
                              }
                              emit self->orderCanceled(ctx->accountName,
                                                       normalized,
                                                       side,
                                                       price,
                                                       wsOrderId);
                              if (isUzxProfile && !ctx->uzxTerminalOrderSeenMs.isEmpty()) {
                                  const qint64 cutoff = nowMs - 60000;
                                  for (auto it = ctx->uzxTerminalOrderSeenMs.begin();
                                       it != ctx->uzxTerminalOrderSeenMs.end();) {
                                      if (it.value() < cutoff) {
                                          ctx->uzxLastFilledByOrderId.remove(it.key());
                                          it = ctx->uzxTerminalOrderSeenMs.erase(it);
                                      } else {
                                          ++it;
                                      }
                                  }
                              }
                          }
                          return;
                      }
                          emit self->logMessage(QStringLiteral("%1 UZX WS: %2")
                                              .arg(contextTag(ctx->accountName), message));
                          return;
                      }
                      const QString method = obj.value(QStringLiteral("method")).toString().toUpper();
                      if (method == QStringLiteral("PING")) {
                          ctx->privateSocket.sendTextMessage(QStringLiteral("{\"method\":\"PONG\"}"));
                          return;
                      }
                      if (obj.contains(QStringLiteral("code"))) {
                          emit self->logMessage(QStringLiteral("%1 WS event: %2")
                                              .arg(contextTag(ctx->accountName), message));
                      }
                  });

    self->connect(&ctx->privateSocket,
                  &QWebSocket::binaryMessageReceived,
                  self,
                  [self, ctx](const QByteArray &payload) {
                      if (ctx->profile == ConnectionStore::Profile::UzxSwap
                          || ctx->profile == ConnectionStore::Profile::UzxSpot
                          || ctx->profile == ConnectionStore::Profile::MexcFutures) {
                          return;
                      }
                      PushMessage message;
                      if (!parsePushMessage(payload, message)) {
                          emit self->logMessage(QStringLiteral("%1 Failed to decode private WS payload.")
                                                    .arg(contextTag(ctx->accountName)));
                          return;
                      }
                      switch (message.type) {
                      case PushBodyType::PrivateDeals:
                          self->processPrivateDeal(*ctx, message.body, message.symbol);
                          break;
                      case PushBodyType::PrivateOrders:
                          self->processPrivateOrder(*ctx, message.body, message.symbol);
                          break;
                      case PushBodyType::PrivateAccount:
                          self->processPrivateAccount(*ctx, message.body);
                          break;
                      default:
                          break;
                      }
                  });

    m_contexts.insert(profile, ctx);
    return *ctx;
}

QString TradeManager::defaultAccountName(ConnectionStore::Profile profile) const
{
    switch (profile) {
    case ConnectionStore::Profile::BinanceSpot:
        return QStringLiteral("Binance Spot");
    case ConnectionStore::Profile::BinanceFutures:
        return QStringLiteral("Binance Futures");
    case ConnectionStore::Profile::Lighter:
        return QStringLiteral("Lighter");
    case ConnectionStore::Profile::MexcFutures:
        return QStringLiteral("MEXC Futures");
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("UZX Swap");
    case ConnectionStore::Profile::UzxSpot:
        return QStringLiteral("UZX Spot");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QStringLiteral("MEXC Spot");
    }
}

QString TradeManager::profileKey(ConnectionStore::Profile profile) const
{
    switch (profile) {
    case ConnectionStore::Profile::BinanceSpot:
        return QStringLiteral("binanceSpot");
    case ConnectionStore::Profile::BinanceFutures:
        return QStringLiteral("binanceFutures");
    case ConnectionStore::Profile::Lighter:
        return QStringLiteral("lighter");
    case ConnectionStore::Profile::MexcFutures:
        return QStringLiteral("mexcFutures");
    case ConnectionStore::Profile::UzxSwap:
        return QStringLiteral("uzxSwap");
    case ConnectionStore::Profile::UzxSpot:
        return QStringLiteral("uzxSpot");
    case ConnectionStore::Profile::MexcSpot:
    default:
        return QStringLiteral("mexcSpot");
    }
}

void TradeManager::setState(Context &ctx, ConnectionState state, const QString &message)
{
    if (ctx.state == state && message.isEmpty()) {
        return;
    }
    ctx.state = state;
    emit connectionStateChanged(ctx.profile, state, message);
}
