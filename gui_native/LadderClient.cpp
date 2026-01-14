#include "LadderClient.h"
#include "PrintsWidget.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QUrl>
#include <QMetaType>
#include <QThread>
#include <QSet>
#include <QTimer>

#include <json.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>

using json = nlohmann::json;

namespace {
static QString safeFileComponent(QString s)
{
    s = s.trimmed();
    s.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_\\-\\.]+")), QStringLiteral("_"));
    if (s.isEmpty()) {
        return QStringLiteral("unknown");
    }
    return s.left(80);
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

static void appendRecent(QStringList &buf, const QString &line, int maxLines)
{
    if (line.isEmpty()) {
        return;
    }
    buf.push_back(line);
    while (buf.size() > maxLines) {
        buf.pop_front();
    }
}

static bool resolveSystemProxyForUrl(const QUrl &url, QString &outType, QString &outProxy)
{
    outType.clear();
    outProxy.clear();
    const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(QNetworkProxyQuery(url));
    for (const auto &p : proxies) {
        if (p.type() != QNetworkProxy::HttpProxy && p.type() != QNetworkProxy::Socks5Proxy) {
            continue;
        }
        if (p.hostName().trimmed().isEmpty() || p.port() <= 0) {
            continue;
        }
        outType = (p.type() == QNetworkProxy::Socks5Proxy) ? QStringLiteral("socks5") : QStringLiteral("http");
        if (!p.user().isEmpty()) {
            outProxy = QStringLiteral("%1:%2@%3:%4").arg(p.user(), p.password(), p.hostName(), QString::number(p.port()));
        } else {
            outProxy = QStringLiteral("%1:%2").arg(p.hostName(), QString::number(p.port()));
        }
        return true;
    }
    return false;
}

static bool isSpamBackendStderrLine(const QString &line)
{
    // Older backend builds can spam very large JSON blobs on stderr (e.g. UZX fills raw),
    // which can cause UI freezes if we echo/log each line.
    return line.contains(QStringLiteral("UZX fills raw:"), Qt::CaseInsensitive)
        || line.contains(QStringLiteral("UZX ws subscribed:"), Qt::CaseInsensitive);
}

class BackendLogWorker final : public QObject {
    Q_OBJECT
public:
    explicit BackendLogWorker(QObject *parent = nullptr)
        : QObject(parent)
        , m_flushTimer(this)
    {
        m_enabled = !qEnvironmentVariableIsSet("BACKEND_LOG")
            || qEnvironmentVariableIntValue("BACKEND_LOG") != 0;
        m_logStderr = !qEnvironmentVariableIsSet("BACKEND_LOG_STDERR")
            || qEnvironmentVariableIntValue("BACKEND_LOG_STDERR") != 0;
        m_logEvents = !qEnvironmentVariableIsSet("BACKEND_LOG_EVENTS")
            || qEnvironmentVariableIntValue("BACKEND_LOG_EVENTS") != 0;
        m_filterSpam = !qEnvironmentVariableIsSet("BACKEND_LOG_FILTER_SPAM")
            || qEnvironmentVariableIntValue("BACKEND_LOG_FILTER_SPAM") != 0;

        int flushMs = qEnvironmentVariableIntValue("BACKEND_LOG_FLUSH_MS");
        if (flushMs <= 0) {
            flushMs = 50;
        }
        flushMs = qBound(5, flushMs, 500);

        // Must be parented to this object so it moves with us to BackendLogThread.
        m_flushTimer.setSingleShot(true);
        m_flushTimer.setInterval(flushMs);
        connect(&m_flushTimer, &QTimer::timeout, this, &BackendLogWorker::flush);
    }

public slots:
    void appendBackendStderr(const QString &prefix, const QString &line)
    {
        if (!m_enabled || !m_logStderr) {
            return;
        }
        if (m_filterSpam && isSpamBackendStderrLine(line)) {
            return;
        }
        m_pending.push_back(QStringLiteral("%1 [backend] %2 [stderr] %3")
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODate), prefix, line));
        scheduleFlush();
    }

    void appendBackendEvent(const QString &prefix, const QString &line)
    {
        if (!m_enabled || !m_logEvents) {
            return;
        }
        m_pending.push_back(QStringLiteral("%1 [backend] %2 [event] %3")
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODate), prefix, line));
        scheduleFlush();
    }

private:
    void scheduleFlush()
    {
        if (m_pending.size() >= 2000) {
            flush();
            return;
        }
        if (!m_flushTimer.isActive()) {
            m_flushTimer.start();
        }
    }

    void flush()
    {
        if (m_pending.isEmpty()) {
            return;
        }
        const QString path = fusionLogFilePath();
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            m_pending.clear();
            return;
        }
        QTextStream ts(&f);
        for (const QString &line : m_pending) {
            ts << line << "\n";
        }
        m_pending.clear();
    }

    bool m_enabled = true;
    bool m_logStderr = true;
    bool m_logEvents = true;
    bool m_filterSpam = true;
    QTimer m_flushTimer;
    QStringList m_pending;
};

static bool parseTickValue(const json &value, qint64 &outTick)
{
    try {
        if (value.is_number_integer()) {
            outTick = static_cast<qint64>(value.get<std::int64_t>());
            return true;
        }
        if (value.is_number_float()) {
            const double d = value.get<double>();
            if (!std::isfinite(d)) {
                return false;
            }
            outTick = static_cast<qint64>(std::llround(d));
            return true;
        }
        if (value.is_string()) {
            const std::string s = value.get<std::string>();
            if (s.empty()) {
                return false;
            }
            std::size_t idx = 0;
            const long long v = std::stoll(s, &idx, 10);
            if (idx == 0) {
                return false;
            }
            outTick = static_cast<qint64>(v);
            return true;
        }
    } catch (...) {
        return false;
    }
    return false;
}

class BackendParseWorker final : public QObject {
public:
    explicit BackendParseWorker(LadderClient *owner, QObject *parent = nullptr)
        : QObject(parent), m_owner(owner)
    {
    }

    void parseLines(const QVector<QByteArray> &lines)
    {
        QVector<ParsedTradeEvent> tradeBatch;
        QVector<ParsedLadderDelta> deltaBatch;
        ParsedLadderFull lastFull;
        bool haveFull = false;
        for (const QByteArray &line : lines) {
            if (line.trimmed().isEmpty()) {
                continue;
            }

            json j;
            try {
                j = json::parse(line.constData(), line.constData() + line.size());
            } catch (...) {
                continue;
            }

            const std::string type = j.value("type", std::string());
            if (type == "trade") {
                ParsedTradeEvent ev;
                ev.price = j.value("price", 0.0);
                ev.qtyBase = j.value("qty", 0.0);
                const std::string side = j.value("side", std::string("buy"));
                ev.buy = (side != "sell");
                qint64 tick = 0;
                if (j.contains("tick") && parseTickValue(j["tick"], tick)) {
                    ev.tick = tick;
                }
                tradeBatch.push_back(ev);
                continue;
            }

            if (type == "trades") {
                auto eventsIt = j.find("events");
                if (eventsIt != j.end() && eventsIt->is_array()) {
                    tradeBatch.reserve(tradeBatch.size() + static_cast<int>(eventsIt->size()));
                    for (const auto &e : *eventsIt) {
                        if (!e.is_object()) {
                            continue;
                        }
                        ParsedTradeEvent ev;
                        ev.price = e.value("price", 0.0);
                        ev.qtyBase = e.value("qty", 0.0);
                        const std::string side = e.value("side", std::string("buy"));
                        ev.buy = (side != "sell");
                        qint64 tick = 0;
                        if (e.contains("tick") && parseTickValue(e["tick"], tick)) {
                            ev.tick = tick;
                        }
                        tradeBatch.push_back(ev);
                    }
                }
                continue;
            }

            if (type == "ladder") {
                ParsedLadderFull out;
                out.bestBid = j.value("bestBid", 0.0);
                out.bestAsk = j.value("bestAsk", 0.0);
                out.tickSize = j.value("tickSize", 0.0);
                out.windowMinTick = j.value("windowMinTick", qint64(0));
                out.windowMaxTick = j.value("windowMaxTick", qint64(0));
                out.centerTick = j.value("centerTick", qint64(0));
                const auto tsIt = j.find("timestamp");
                if (tsIt != j.end() && tsIt->is_number_integer()) {
                    out.timestampMs = static_cast<qint64>(tsIt->get<std::int64_t>());
                }

                auto rowsIt = j.find("rows");
                if (rowsIt != j.end() && rowsIt->is_array()) {
                    out.rows.reserve(static_cast<int>(rowsIt->size()));
                    for (const auto &row : *rowsIt) {
                        ParsedLadderRow r;
                        qint64 tick = 0;
                        if (row.contains("tick") && parseTickValue(row["tick"], tick)) {
                            r.tick = tick;
                        }
                        r.hasBid = row.contains("bid");
                        r.bid = row.value("bid", 0.0);
                        r.hasAsk = row.contains("ask");
                        r.ask = row.value("ask", 0.0);
                        out.rows.push_back(r);
                    }
                }
                lastFull = std::move(out);
                haveFull = true;
                continue;
            }

            if (type == "ladder_delta") {
                ParsedLadderDelta out;
                out.bestBid = j.value("bestBid", 0.0);
                out.bestAsk = j.value("bestAsk", 0.0);
                out.tickSize = j.value("tickSize", 0.0);
                out.windowMinTick = j.value("windowMinTick", qint64(0));
                out.windowMaxTick = j.value("windowMaxTick", qint64(0));
                out.centerTick = j.value("centerTick", qint64(0));
                const auto tsIt = j.find("timestamp");
                if (tsIt != j.end() && tsIt->is_number_integer()) {
                    out.timestampMs = static_cast<qint64>(tsIt->get<std::int64_t>());
                }

                auto updatesIt = j.find("updates");
                if (updatesIt != j.end() && updatesIt->is_array()) {
                    out.updates.reserve(static_cast<int>(updatesIt->size()));
                    for (const auto &row : *updatesIt) {
                        ParsedLadderRow r;
                        qint64 tick = 0;
                        if (row.contains("tick") && parseTickValue(row["tick"], tick)) {
                            r.tick = tick;
                        }
                        r.hasBid = row.contains("bid");
                        r.bid = row.value("bid", 0.0);
                        r.hasAsk = row.contains("ask");
                        r.ask = row.value("ask", 0.0);
                        out.updates.push_back(r);
                    }
                }

                auto removalsIt = j.find("removals");
                if (removalsIt != j.end() && removalsIt->is_array()) {
                    out.removals.reserve(static_cast<int>(removalsIt->size()));
                    for (const auto &tickValue : *removalsIt) {
                        if (tickValue.is_number_integer()) {
                            out.removals.push_back(static_cast<qint64>(tickValue.get<std::int64_t>()));
                        }
                    }
                }
                deltaBatch.push_back(std::move(out));
                continue;
            }
        }
        if (!tradeBatch.isEmpty() && m_owner) {
            QMetaObject::invokeMethod(
                m_owner,
                [owner = m_owner, tradeBatch = std::move(tradeBatch)]() { owner->handleParsedTrades(tradeBatch); },
                Qt::QueuedConnection);
        }
        if (haveFull && m_owner) {
            QMetaObject::invokeMethod(
                m_owner,
                [owner = m_owner, lastFull]() { owner->handleParsedLadderFull(lastFull); },
                Qt::QueuedConnection);
        }
        if (!deltaBatch.isEmpty() && m_owner) {
            QMetaObject::invokeMethod(
                m_owner,
                [owner = m_owner, deltaBatch = std::move(deltaBatch)]() { owner->handleParsedLadderDeltas(deltaBatch); },
                Qt::QueuedConnection);
        }
    }

private:
    LadderClient *m_owner = nullptr;
};

static qint64 pow10i(int exp)
{
    qint64 v = 1;
    for (int i = 0; i < exp; ++i) {
        v *= 10;
    }
    return v;
}

static bool chooseScaleForTickSize(double tickSize, qint64 &outScale, qint64 &outTickSizeScaled)
{
    if (!(tickSize > 0.0) || !std::isfinite(tickSize)) {
        return false;
    }
    for (int decimals = 0; decimals <= 12; ++decimals) {
        const qint64 scale = pow10i(decimals);
        const double scaled = tickSize * static_cast<double>(scale);
        if (!std::isfinite(scaled)) {
            continue;
        }
        const qint64 rounded = static_cast<qint64>(std::llround(scaled));
        if (rounded <= 0) {
            continue;
        }
        if (std::abs(scaled - static_cast<double>(rounded)) <= 1e-9) {
            outScale = scale;
            outTickSizeScaled = rounded;
            return true;
        }
    }
    return false;
}

static bool parseDecimalToScaledInt(const json &value, int decimals, qint64 &out)
{
    if (decimals < 0 || decimals > 12) {
        return false;
    }

    std::string s;
    if (value.is_string()) {
        s = value.get<std::string>();
    } else if (value.is_number()) {
        s = value.dump();
    } else {
        return false;
    }

    if (s.empty()) {
        return false;
    }

    bool neg = false;
    size_t pos = 0;
    if (s[pos] == '-') {
        neg = true;
        ++pos;
    } else if (s[pos] == '+') {
        ++pos;
    }

    qint64 intPart = 0;
    bool anyDigit = false;
    while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
        anyDigit = true;
        intPart = intPart * 10 + (s[pos] - '0');
        ++pos;
    }

    qint64 fracPart = 0;
    int fracDigits = 0;
    int nextDigit = -1;
    if (pos < s.size() && s[pos] == '.') {
        ++pos;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            anyDigit = true;
            if (fracDigits < decimals) {
                fracPart = fracPart * 10 + (s[pos] - '0');
                ++fracDigits;
            } else if (nextDigit < 0) {
                nextDigit = (s[pos] - '0');
            }
            ++pos;
        }
    }

    if (!anyDigit) {
        return false;
    }

    while (fracDigits < decimals) {
        fracPart *= 10;
        ++fracDigits;
    }

    if (nextDigit >= 5) {
        fracPart += 1;
        const qint64 scale = pow10i(decimals);
        if (fracPart >= scale) {
            fracPart -= scale;
            intPart += 1;
        }
    }

    const qint64 scale = pow10i(decimals);
    qint64 result = intPart * scale + fracPart;
    if (neg) {
        result = -result;
    }
    out = result;
    return true;
}

static qint64 floorBucketTickSigned(qint64 tick, qint64 compression)
{
    compression = std::max<qint64>(1, compression);
    if (compression == 1) {
        return tick;
    }
    if (tick >= 0) {
        return (tick / compression) * compression;
    }
    const qint64 absTick = -tick;
    const qint64 buckets = (absTick + compression - 1) / compression;
    return -buckets * compression;
}

static qint64 ceilBucketTickSigned(qint64 tick, qint64 compression)
{
    compression = std::max<qint64>(1, compression);
    if (compression == 1) {
        return tick;
    }
    if (tick >= 0) {
        return ((tick + compression - 1) / compression) * compression;
    }
    const qint64 absTick = -tick;
    const qint64 buckets = absTick / compression;
    return -buckets * compression;
}

[[maybe_unused]] static bool quantizePriceToTick(const json &priceValue,
                                double tickSize,
                                qint64 &outTick,
                                double &outSnappedPrice)
{
    qint64 scale = 0;
    qint64 tickSizeScaled = 0;
    if (!chooseScaleForTickSize(tickSize, scale, tickSizeScaled)) {
        return false;
    }
    int decimals = 0;
    qint64 tmp = scale;
    while (tmp > 1) {
        tmp /= 10;
        ++decimals;
    }

    qint64 priceScaled = 0;
    if (!parseDecimalToScaledInt(priceValue, decimals, priceScaled)) {
        return false;
    }

    qint64 tick = 0;
    if (priceScaled >= 0) {
        tick = (priceScaled + tickSizeScaled / 2) / tickSizeScaled;
    } else {
        tick = -((-priceScaled + tickSizeScaled / 2) / tickSizeScaled);
    }

    const qint64 snappedScaled = tick * tickSizeScaled;
    outTick = tick;
    outSnappedPrice = static_cast<double>(snappedScaled) / static_cast<double>(scale);
    return std::isfinite(outSnappedPrice);
}

static QThread *sharedBackendParseThread()
{
    static QThread *t = []() -> QThread * {
        auto *thread = new QThread(QCoreApplication::instance());
        thread->setObjectName(QStringLiteral("BackendParseThread"));
        thread->start();
        return thread;
    }();
    return t;
}

static QThread *sharedBackendLogThread()
{
    static QThread *t = []() -> QThread * {
        auto *thread = new QThread(QCoreApplication::instance());
        thread->setObjectName(QStringLiteral("BackendLogThread"));
        thread->start();
        return thread;
    }();
    return t;
}

static BackendLogWorker *sharedBackendLogWorker()
{
    static BackendLogWorker *w = []() -> BackendLogWorker * {
        auto *worker = new BackendLogWorker();
        worker->moveToThread(sharedBackendLogThread());
        return worker;
    }();
    return w;
}
} // namespace

LadderClient::LadderClient(const QString &backendPath,
                           const QString &symbol,
                           int levels,
                           const QString &exchange,
                           QObject *parent,
                           PrintsWidget *prints,
                           const QString &proxyType,
                           const QString &proxy)
    : QObject(parent)
    , m_backendPath(backendPath)
    , m_symbol(symbol)
    , m_levels(levels)
    , m_exchange(exchange)
    , m_proxyType(proxyType)
    , m_proxy(proxy)
    , m_prints(prints)
{
    qRegisterMetaType<QVector<QByteArray>>("QVector<QByteArray>");
    qRegisterMetaType<ParsedTradeEvent>("ParsedTradeEvent");
    qRegisterMetaType<ParsedLadderRow>("ParsedLadderRow");
    qRegisterMetaType<ParsedLadderFull>("ParsedLadderFull");
    qRegisterMetaType<ParsedLadderDelta>("ParsedLadderDelta");

    auto *worker = new BackendParseWorker(this);
    worker->moveToThread(sharedBackendParseThread());
    connect(this, &QObject::destroyed, worker, &QObject::deleteLater, Qt::QueuedConnection);
    connect(this, &LadderClient::parseLinesRequested, worker, &BackendParseWorker::parseLines, Qt::QueuedConnection);
    m_parseWorker = worker;

    m_process.setProgram(m_backendPath);
    if (!QFileInfo::exists(m_backendPath)) {
        const QString fallback = QDir(QCoreApplication::applicationDirPath())
                                     .filePath(QFileInfo(m_backendPath).fileName());
        if (QFileInfo::exists(fallback)) {
            m_backendPath = fallback;
            m_process.setProgram(m_backendPath);
        }
    }
    m_process.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_process.setProcessChannelMode(QProcess::SeparateChannels);

    connect(&m_process, &QProcess::readyReadStandardOutput, this, &LadderClient::handleReadyRead);
    connect(&m_process,
            &QProcess::errorOccurred,
            this,
            &LadderClient::handleErrorOccurred);
    connect(&m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &LadderClient::handleFinished);
    connect(&m_process,
            &QProcess::readyReadStandardError,
            this,
            &LadderClient::handleReadyReadStderr);

    m_watchdogTimer.setSingleShot(true);
    connect(&m_watchdogTimer, &QTimer::timeout, this, &LadderClient::handleWatchdogTimeout);

    restart(m_symbol, m_levels, m_exchange);
}

LadderClient::~LadderClient()
{
    stop();
}

QString LadderClient::formatBackendPrefix() const
{
    const QString ex = m_exchange.isEmpty() ? QStringLiteral("auto") : m_exchange;
    return QStringLiteral("[%1 %2]").arg(ex, m_symbol);
}

static void bucketAggAdd(QHash<qint64, LadderClient::BookEntry> &buckets, qint64 bucketTick, double bidDelta, double askDelta)
{
    if (!(bidDelta != 0.0 || askDelta != 0.0)) {
        return;
    }
    static constexpr double kEps = 1e-9;
    LadderClient::BookEntry &e = buckets[bucketTick];
    e.bidQty += bidDelta;
    e.askQty += askDelta;
    if (!std::isfinite(e.bidQty)) {
        e.bidQty = 0.0;
    }
    if (!std::isfinite(e.askQty)) {
        e.askQty = 0.0;
    }
    if (e.bidQty < kEps) {
        e.bidQty = 0.0;
    }
    if (e.askQty < kEps) {
        e.askQty = 0.0;
    }
    if (e.bidQty <= 0.0 && e.askQty <= 0.0) {
        buckets.remove(bucketTick);
    }
}

static void rebuildBucketBook(QHash<qint64, LadderClient::BookEntry> &out,
                              const QMap<qint64, LadderClient::BookEntry> &book,
                              qint64 compression)
{
    out.clear();
    const qint64 c = std::max<qint64>(1, compression);
    for (auto it = book.constBegin(); it != book.constEnd(); ++it) {
        const qint64 tick = it.key();
        const double bid = it->bidQty;
        const double ask = it->askQty;
        if (bid > 0.0) {
            bucketAggAdd(out, floorBucketTickSigned(tick, c), bid, 0.0);
        }
        if (ask > 0.0) {
            bucketAggAdd(out, ceilBucketTickSigned(tick, c), 0.0, ask);
        }
    }
}

static bool computeBestBuckets(const QHash<qint64, LadderClient::BookEntry> &buckets,
                               qint64 &outBestBidBucket,
                               qint64 &outBestAskBucket)
{
    outBestBidBucket = 0;
    outBestAskBucket = 0;
    bool hasBid = false;
    bool hasAsk = false;
    for (auto it = buckets.constBegin(); it != buckets.constEnd(); ++it) {
        const qint64 tick = it.key();
        if (it->bidQty > 0.0) {
            if (!hasBid || tick > outBestBidBucket) {
                outBestBidBucket = tick;
                hasBid = true;
            }
        }
        if (it->askQty > 0.0) {
            if (!hasAsk || tick < outBestAskBucket) {
                outBestAskBucket = tick;
                hasAsk = true;
            }
        }
    }
    return hasBid && hasAsk;
}

static void sanitizeBucketBook(QHash<qint64, LadderClient::BookEntry> &buckets,
                               qint64 bestBidBucketTick,
                               qint64 bestAskBucketTick,
                               QSet<qint64> *dirtyBuckets = nullptr)
{
    if (buckets.isEmpty()) {
        return;
    }
    if (bestBidBucketTick == 0 && bestAskBucketTick == 0) {
        return;
    }

    // Remove any liquidity that would be "impossible" relative to the spread buckets.
    // This prevents stale aggregated buckets from sticking in GPU incremental mode.
    auto it = buckets.begin();
    while (it != buckets.end()) {
        const qint64 tick = it.key();
        auto &e = it.value();
        bool changed = false;

        if (bestBidBucketTick != 0 && tick <= bestBidBucketTick && e.askQty > 0.0) {
            e.askQty = 0.0;
            changed = true;
        }
        if (bestAskBucketTick != 0 && tick >= bestAskBucketTick && e.bidQty > 0.0) {
            e.bidQty = 0.0;
            changed = true;
        }
        if (bestBidBucketTick != 0
            && bestAskBucketTick != 0
            && bestBidBucketTick < bestAskBucketTick
            && tick > bestBidBucketTick
            && tick < bestAskBucketTick
            && (e.bidQty > 0.0 || e.askQty > 0.0)) {
            e.bidQty = 0.0;
            e.askQty = 0.0;
            changed = true;
        }

        if (changed && dirtyBuckets) {
            dirtyBuckets->insert(tick);
        }

        static constexpr double kEps = 1e-9;
        if (e.bidQty < kEps) e.bidQty = 0.0;
        if (e.askQty < kEps) e.askQty = 0.0;
        if (e.bidQty <= 0.0 && e.askQty <= 0.0) {
            it = buckets.erase(it);
        } else {
            ++it;
        }
    }
}

QString LadderClient::backendLogPath() const
{
    return fusionLogFilePath();
}

void LadderClient::logBackendEvent(const QString &line)
{
    BackendLogWorker *w = sharedBackendLogWorker();
    const QString prefix = formatBackendPrefix();
    QMetaObject::invokeMethod(w,
                              "appendBackendEvent",
                              Qt::QueuedConnection,
                              Q_ARG(QString, prefix),
                              Q_ARG(QString, line));
}

void LadderClient::logBackendLine(const QString &line)
{
    BackendLogWorker *w = sharedBackendLogWorker();
    const QString prefix = formatBackendPrefix();
    QMetaObject::invokeMethod(w,
                              "appendBackendStderr",
                              Qt::QueuedConnection,
                              Q_ARG(QString, prefix),
                              Q_ARG(QString, line));
}

QString LadderClient::formatCrashSummary(int exitCode, QProcess::ExitStatus status) const
{
    const QString statusText = (status == QProcess::CrashExit) ? QStringLiteral("CrashExit") : QStringLiteral("NormalExit");
    QString msg = QStringLiteral("%1 Backend crashed (exitCode=%2, %3)")
                      .arg(formatBackendPrefix())
                      .arg(exitCode)
                      .arg(statusText);
    if (!m_lastProcessErrorString.isEmpty()) {
        msg += QStringLiteral(". errorString=\"%1\"").arg(m_lastProcessErrorString);
    }
    if (!m_recentStderr.isEmpty()) {
        const int count = static_cast<int>(m_recentStderr.size());
        const int take = std::min(6, count);
        const QString tail = m_recentStderr.mid(m_recentStderr.size() - take).join(QStringLiteral(" | "));
        msg += QStringLiteral(". stderr: %1").arg(tail);
    }
    msg += QStringLiteral(". log: %1").arg(backendLogPath());
    return msg;
}

void LadderClient::restart(const QString &symbol, int levels, const QString &exchange)
{
    m_restartInProgress = true;
    // Treat any termination that happens during restart() as expected; otherwise
    // we end up with "Process crashed" spam during startup (we restart once more
    // after applying compression/account settings).
    m_stopRequested = true;
    m_symbol = symbol;
    m_levels = levels;
    if (!exchange.isEmpty()) {
        m_exchange = exchange;
    }
    m_lastTickSize = 0.0;
    m_bestBid = 0.0;
    m_bestAsk = 0.0;
    m_book.clear();
    m_bucketBook.clear();
    m_bufferMinTick = 0;
    m_bufferMaxTick = 0;
    m_centerTick = 0;
    m_hasBook = false;
    m_printBuffer.clear();
    if (m_prints) {
        QVector<PrintItem> emptyPrints;
        m_prints->setPrints(emptyPrints);
        QVector<double> emptyPrices;
        QVector<qint64> emptyTicks;
        m_prints->setLadderPrices(emptyPrices, emptyTicks, 20, 0.0, 0, 0, 1, 0.0);
        QVector<LocalOrderMarker> emptyOrders;
        m_prints->setLocalOrders(emptyOrders);
    }

    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
    m_recentStderr.clear();
    m_lastExitCode = 0;
    m_lastExitStatus = QProcess::NormalExit;
    m_lastProcessError = QProcess::UnknownError;
    m_lastProcessErrorString.clear();
    m_stopRequested = false;

    // Map UI symbol to exchange-specific wire format.
    QString wireSymbol = m_symbol;
    if (m_exchange == QStringLiteral("uzxspot"))
    {
        if (!wireSymbol.contains(QLatin1Char('-')))
        {
            static const QStringList quotes = {QStringLiteral("USDT"),
                                               QStringLiteral("USDC"),
                                               QStringLiteral("USDR"),
                                               QStringLiteral("USDQ"),
                                               QStringLiteral("EURQ"),
                                               QStringLiteral("EURR"),
                                               QStringLiteral("BTC"),
                                               QStringLiteral("ETH")};
            for (const auto &q : quotes)
            {
                if (wireSymbol.endsWith(q, Qt::CaseInsensitive))
                {
                    const QString base = wireSymbol.left(wireSymbol.size() - q.size());
                    if (!base.isEmpty())
                    {
                        wireSymbol = base + QLatin1Char('-') + q;
                    }
                    break;
                }
            }
        }
    }
    else if (m_exchange == QStringLiteral("uzxswap"))
    {
        wireSymbol = wireSymbol.replace(QStringLiteral("-"), QString());
    }
    else if (m_exchange == QStringLiteral("binance") || m_exchange == QStringLiteral("binance_futures"))
    {
        wireSymbol = wireSymbol.replace(QStringLiteral("_"), QString());
        wireSymbol = wireSymbol.replace(QStringLiteral("-"), QString());
    }

    QStringList args;
    args << "--symbol" << wireSymbol
         << "--ladder-levels" << QString::number(m_levels)
         << "--cache-levels" << QString::number(m_levels);
    if (!m_exchange.isEmpty()) {
        args << "--exchange" << m_exchange;
    }
    QString proxyRaw = m_proxy.trimmed();
    QString type = m_proxyType.trimmed().toLower();
    bool systemProxyResolved = false;
    if (proxyRaw.isEmpty()) {
        // Match GUI behavior: empty field => use Windows system proxy settings.
        // Backend can't consume "system", so we resolve it into host:port here.
        const QUrl queryUrl =
            (m_exchange.trimmed().toLower().contains(QStringLiteral("lighter")))
                ? QUrl(QStringLiteral("https://mainnet.zklighter.elliot.ai/"))
                : QUrl(QStringLiteral("https://www.google.com/generate_204"));
        systemProxyResolved = resolveSystemProxyForUrl(queryUrl, type, proxyRaw);
    }

    if (!proxyRaw.isEmpty()) {
        if (!type.isEmpty()) {
            args << "--proxy-type" << type;
        }
        args << "--proxy" << proxyRaw;

        auto summarize = [](const QString &typeRaw, const QString &raw) -> QString {
            const QString proto =
                (typeRaw.trimmed().toLower() == QStringLiteral("socks5")) ? QStringLiteral("SOCKS5") : QStringLiteral("HTTP");
            QString host;
            QString port;
            bool auth = false;

            const QString trimmed = raw.trimmed();
            const QStringList atSplit = trimmed.split('@');
            if (atSplit.size() == 2) {
                const QStringList cp = atSplit.at(0).split(':');
                const QStringList hp = atSplit.at(1).split(':');
                if (cp.size() == 2 && hp.size() == 2) {
                    auth = true;
                    host = hp.at(0);
                    port = hp.at(1);
                }
            } else {
                const QStringList parts = trimmed.split(':');
                if (parts.size() == 2) {
                    host = parts.at(0);
                    port = parts.at(1);
                } else if (parts.size() == 4) {
                    // host:port:user:pass OR host:user:pass:port
                    host = parts.at(0);
                    auth = true;
                    port = parts.at(1);
                    if (port.toInt() <= 0) {
                        port = parts.at(3);
                    }
                }
            }

            if (host.isEmpty() || port.isEmpty()) {
                return QStringLiteral("%1 <unparsed>").arg(proto);
            }
            return QStringLiteral("%1 %2:%3%4").arg(proto, host, port, auth ? QStringLiteral(" auth") : QString());
        };

        const QString label = systemProxyResolved ? QStringLiteral("system") : summarize(type, proxyRaw);
        emitStatus(QStringLiteral("%1 Backend proxy: %2").arg(formatBackendPrefix(), label));
    }
    m_process.setArguments(args);

    emitStatus(QStringLiteral("Starting backend (%1, %2 levels, %3)...")
                   .arg(m_symbol)
                   .arg(m_levels)
                   .arg(m_exchange.isEmpty() ? QStringLiteral("auto") : m_exchange));
    QStringList argsForLog = args;
    for (int i = 0; i < argsForLog.size(); ++i) {
        if (argsForLog.at(i) == QStringLiteral("--proxy") && i + 1 < argsForLog.size()) {
            argsForLog[i + 1] = QStringLiteral("<redacted>");
        }
    }
    qWarning() << "[LadderClient] starting backend with args" << argsForLog;
    logBackendEvent(QStringLiteral("start args=%1").arg(argsForLog.join(QLatin1Char(' '))));
    m_process.start();
    armWatchdog();
    m_restartInProgress = false;
}

void LadderClient::setProxy(const QString &proxyType, const QString &proxy)
{
    m_proxyType = proxyType;
    m_proxy = proxy;
}

void LadderClient::stop()
{
    m_stopRequested = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
        emitStatus(QStringLiteral("Backend stopped"));
    }
    m_watchdogTimer.stop();
}

bool LadderClient::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

void LadderClient::setCompression(int factor)
{
    const int v = std::max(1, factor);
    if (v == m_tickCompression) {
        return;
    }
    m_tickCompression = v;
    if (!m_book.isEmpty()) {
        rebuildBucketBook(m_bucketBook, m_book, m_tickCompression);
        ++m_bookRevision;
        emit bookUpdated(m_bookRevision);
        if (m_hasBook) {
            emit bookRangeUpdated(m_bufferMinTick, m_bufferMaxTick, m_centerTick, m_lastTickSize);
        }
    }
}

void LadderClient::shiftWindowTicks(qint64 ticks)
{
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    json cmd;
    cmd["cmd"] = "shift";
    cmd["ticks"] = ticks;
    const std::string payload = cmd.dump();
    m_process.write(payload.c_str(), static_cast<int>(payload.size()));
    m_process.write("\n", 1);
}

void LadderClient::resetManualCenter()
{
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    json cmd;
    cmd["cmd"] = "center_auto";
    const std::string payload = cmd.dump();
    m_process.write(payload.c_str(), static_cast<int>(payload.size()));
    m_process.write("\n", 1);
}

void LadderClient::requestForceFull()
{
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    json cmd;
    cmd["cmd"] = "force_full";
    const std::string payload = cmd.dump();
    m_process.write(payload.c_str(), static_cast<int>(payload.size()));
    m_process.write("\n", 1);
}

bool LadderClient::crossedBookLikely(const QSet<qint64> &dirtyBuckets) const
{
    if (!(m_lastTickSize > 0.0) || !std::isfinite(m_lastTickSize)) {
        return false;
    }
    if (!(m_bestBid > 0.0) || !(m_bestAsk > 0.0)) {
        return false;
    }
    const qint64 bidTick = static_cast<qint64>(std::llround(m_bestBid / m_lastTickSize));
    const qint64 askTick = static_cast<qint64>(std::llround(m_bestAsk / m_lastTickSize));
    const qint64 bidBucket = floorBucketTickSigned(bidTick, m_tickCompression);
    const qint64 askBucket = ceilBucketTickSigned(askTick, m_tickCompression);
    if (bidBucket > askBucket) {
        return true;
    }
    const bool hasTop = (bidBucket != 0 && askBucket != 0);
    for (qint64 t : dirtyBuckets) {
        auto it = m_bucketBook.constFind(t);
        if (it == m_bucketBook.constEnd()) {
            continue;
        }
        if (it->bidQty > 0.0 && it->askQty > 0.0) {
            if (!hasTop) {
                return true;
            }
            if (bidBucket < askBucket) {
                return true;
            }
        }
    }
    return false;
}

DomSnapshot LadderClient::snapshotForRange(qint64 minTick, qint64 maxTick) const
{
    if (!m_hasBook || m_lastTickSize <= 0.0) {
        return DomSnapshot{};
    }
    return buildSnapshot(minTick, maxTick);
}

void LadderClient::handleReadyRead()
{
    m_buffer += m_process.readAllStandardOutput();
    int idx = -1;
    while ((idx = m_buffer.indexOf('\n')) != -1) {
        QByteArray line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 1);
        if (!line.trimmed().isEmpty()) {
            m_pendingParseLines.push_back(line);
        }
    }
    if (m_parseEmitScheduled || m_pendingParseLines.isEmpty()) {
        return;
    }
    m_parseEmitScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_parseEmitScheduled = false;
        if (m_pendingParseLines.isEmpty()) {
            return;
        }
        QVector<QByteArray> lines;
        lines.swap(m_pendingParseLines);
        emit parseLinesRequested(lines);
    });
}

void LadderClient::handleReadyReadStderr()
{
    static const bool echoSpam = qEnvironmentVariableIntValue("BACKEND_STDERR_ECHO_SPAM") > 0;
    static const bool dropSpam =
        !qEnvironmentVariableIsSet("BACKEND_STDERR_DROP_SPAM")
        || qEnvironmentVariableIntValue("BACKEND_STDERR_DROP_SPAM") != 0;

    const QByteArray raw = m_process.readAllStandardError();
    if (raw.isEmpty()) {
        return;
    }
    const QList<QByteArray> lines = raw.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        const QString text = QString::fromLocal8Bit(trimmed);
        const bool spam = isSpamBackendStderrLine(text);
        if (!spam || echoSpam) {
            qWarning() << "[LadderClient stderr]" << text;
        }
        if (text.contains(QStringLiteral("proxy enabled:"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("lighter:"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("lighter ws"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("lighter orderBookDetails"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("httpGetQt failed"), Qt::CaseInsensitive)) {
            emitStatus(QStringLiteral("%1 %2").arg(formatBackendPrefix(), text));
        }
        if (!spam || !dropSpam) {
            appendRecent(m_recentStderr, text, 80);
            logBackendLine(text);
        }
    }
}

void LadderClient::handleErrorOccurred(QProcess::ProcessError error)
{
    m_lastProcessError = error;
    m_lastProcessErrorString = m_process.errorString();
    qWarning() << "[LadderClient] backend error" << error << m_lastProcessErrorString;
    logBackendEvent(QStringLiteral("errorOccurred code=%1 msg=%2")
                        .arg(static_cast<int>(error))
                        .arg(m_lastProcessErrorString));

    // Don't spam generic "process crashed" here; finished() will provide exit code + stderr tail.
    if (error == QProcess::FailedToStart) {
        emitStatus(QStringLiteral("%1 Backend failed to start: %2. log: %3")
                       .arg(formatBackendPrefix(), m_lastProcessErrorString, backendLogPath()));
    } else if (error == QProcess::Timedout) {
        emitStatus(QStringLiteral("%1 Backend timeout: %2. log: %3")
                       .arg(formatBackendPrefix(), m_lastProcessErrorString, backendLogPath()));
    }
}

void LadderClient::handleFinished(int exitCode, QProcess::ExitStatus status)
{
    m_lastExitCode = exitCode;
    m_lastExitStatus = status;
    qWarning() << "[LadderClient] backend finished" << exitCode << status;
    logBackendEvent(QStringLiteral("finished exitCode=%1 exitStatus=%2")
                        .arg(exitCode)
                        .arg(status == QProcess::CrashExit ? QStringLiteral("CrashExit") : QStringLiteral("NormalExit")));
    if (m_restartInProgress) {
        // Restart already started another process; don't notify and don't chain-restart.
        m_watchdogTimer.stop();
        return;
    }
    if (status == QProcess::CrashExit && !m_stopRequested) {
        emitStatus(formatCrashSummary(exitCode, status));
    } else {
        emitStatus(QStringLiteral("%1 Backend finished (%2). log: %3")
                       .arg(formatBackendPrefix())
                       .arg(exitCode)
                       .arg(backendLogPath()));
    }
    m_watchdogTimer.stop();
    if (!m_stopRequested) {
        // If the backend exits because the symbol can't be resolved, don't spam restart loops.
        const bool fatalSymbol =
            (status == QProcess::NormalExit && exitCode != 0
             && m_recentStderr.join(QLatin1Char('\n'))
                    .contains(QStringLiteral("failed to resolve market_id/tickSize"),
                              Qt::CaseInsensitive));
        if (fatalSymbol) {
            emitStatus(QStringLiteral("%1 Backend stopped: invalid Lighter symbol (can't resolve market_id).")
                           .arg(formatBackendPrefix()));
            return;
        }
        QTimer::singleShot(700, this, [this]() {
            if (m_stopRequested) {
                return;
            }
            if (m_process.state() != QProcess::NotRunning) {
                return;
            }
            restart(m_symbol, m_levels, m_exchange);
        });
    }
}

void LadderClient::handleParsedTrade(const ParsedTradeEvent &ev)
{
    armWatchdog();
    if (!m_prints) {
        return;
    }
    // Batched path handles sampling/caps; keep single-trade path for compatibility.
    double price = ev.price;
    const double qtyBase = ev.qtyBase;
    if (price <= 0.0 || qtyBase <= 0.0) {
        return;
    }
    qint64 tick = ev.tick;
    if (tick != 0 && m_lastTickSize > 0.0) {
        price = static_cast<double>(tick) * m_lastTickSize;
    }
    if (tick == 0 && m_lastTickSize > 0.0) {
        tick = static_cast<qint64>(std::llround(price / m_lastTickSize));
        price = static_cast<double>(tick) * m_lastTickSize;
    }

    const double qtyQuote = price * qtyBase;
    if (qtyQuote <= 0.0) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    PrintItem it;
    it.price = price;
    it.qty = qtyQuote;
    it.buy = ev.buy;
    it.rowHint = -1;
    it.tick = tick;
    it.timeMs = nowMs;
    it.seq = ++m_printSeq;
    m_pendingPrintItems.push_back(it);
    if (m_printFlushScheduled) {
        return;
    }
    m_printFlushScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_printFlushScheduled = false;
        if (!m_prints || m_pendingPrintItems.isEmpty()) {
            m_pendingPrintItems.clear();
            return;
        }
        for (const auto &p : m_pendingPrintItems) {
            m_printBuffer.push_back(p);
        }
        m_pendingPrintItems.clear();

        const int maxPrints = 128;
        if (m_printBuffer.size() > maxPrints) {
            m_printBuffer.erase(m_printBuffer.begin(),
                                m_printBuffer.begin() + (m_printBuffer.size() - maxPrints));
        }
        m_prints->setPrints(m_printBuffer);
    });
}

void LadderClient::handleParsedTrades(const QVector<ParsedTradeEvent> &events)
{
    armWatchdog();
    if (!m_prints || events.isEmpty()) {
        return;
    }

    static const int maxPerBatch = []() {
        const int v = qEnvironmentVariableIntValue("PRINTS_MAX_EVENTS_PER_BATCH");
        return std::clamp(v > 0 ? v : 96, 8, 1024);
    }();
    static const int maxPending = []() {
        const int v = qEnvironmentVariableIntValue("PRINTS_MAX_PENDING");
        return std::clamp(v > 0 ? v : 256, 32, 4096);
    }();

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int start = (events.size() > maxPerBatch) ? (events.size() - maxPerBatch) : 0;
    for (int i = start; i < events.size(); ++i) {
        const auto &ev = events[i];
        double price = ev.price;
        const double qtyBase = ev.qtyBase;
        if (price <= 0.0 || qtyBase <= 0.0) {
            continue;
        }
        qint64 tick = ev.tick;
        if (tick != 0 && m_lastTickSize > 0.0) {
            price = static_cast<double>(tick) * m_lastTickSize;
        }
        if (tick == 0 && m_lastTickSize > 0.0) {
            tick = static_cast<qint64>(std::llround(price / m_lastTickSize));
            price = static_cast<double>(tick) * m_lastTickSize;
        }
        const double qtyQuote = price * qtyBase;
        if (qtyQuote <= 0.0) {
            continue;
        }
        PrintItem it;
        it.price = price;
        it.qty = qtyQuote;
        it.buy = ev.buy;
        it.rowHint = -1;
        it.tick = tick;
        it.timeMs = nowMs;
        it.seq = ++m_printSeq;
        m_pendingPrintItems.push_back(it);
    }

    if (m_pendingPrintItems.size() > maxPending) {
        m_pendingPrintItems.erase(m_pendingPrintItems.begin(),
                                  m_pendingPrintItems.begin() + (m_pendingPrintItems.size() - maxPending));
    }

    if (m_printFlushScheduled) {
        return;
    }
    m_printFlushScheduled = true;
    QTimer::singleShot(0, this, [this]() {
        m_printFlushScheduled = false;
        if (!m_prints || m_pendingPrintItems.isEmpty()) {
            m_pendingPrintItems.clear();
            return;
        }
        for (const auto &p : m_pendingPrintItems) {
            m_printBuffer.push_back(p);
        }
        m_pendingPrintItems.clear();

        const int maxPrints = 128;
        if (m_printBuffer.size() > maxPrints) {
            m_printBuffer.erase(m_printBuffer.begin(),
                                m_printBuffer.begin() + (m_printBuffer.size() - maxPrints));
        }
        m_prints->setPrints(m_printBuffer);
    });
}

void LadderClient::handleParsedLadderFull(const ParsedLadderFull &msg)
{
    armWatchdog();
    applyFullLadderMessage(msg);
    if (msg.timestampMs > 0) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const int pingMs = static_cast<int>(std::max<qint64>(0, nowMs - msg.timestampMs));
        emit pingUpdated(pingMs);
    }
}

void LadderClient::handleParsedLadderDelta(const ParsedLadderDelta &msg)
{
    armWatchdog();
    applyDeltaLadderMessage(msg);
    if (msg.timestampMs > 0) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const int pingMs = static_cast<int>(std::max<qint64>(0, nowMs - msg.timestampMs));
        emit pingUpdated(pingMs);
    }
}

void LadderClient::handleParsedLadderDeltas(const QVector<ParsedLadderDelta> &msgs)
{
    if (msgs.isEmpty()) {
        return;
    }
    armWatchdog();
    for (const auto &msg : msgs) {
        applyDeltaLadderMessage(msg);
    }
    const ParsedLadderDelta &last = msgs.back();
    if (last.timestampMs > 0) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const int pingMs = static_cast<int>(std::max<qint64>(0, nowMs - last.timestampMs));
        emit pingUpdated(pingMs);
    }
}

void LadderClient::applyFullLadderMessage(const ParsedLadderFull &msg)
{
    const bool wasReady = m_hasBook;
    const qint64 prevMin = m_bufferMinTick;
    const qint64 prevMax = m_bufferMaxTick;
    const qint64 prevCenter = m_centerTick;
    const double prevTick = m_lastTickSize;

    m_bestBid = msg.bestBid;
    m_bestAsk = msg.bestAsk;
    const double tickSize = msg.tickSize;
    if (tickSize > 0.0) {
        m_lastTickSize = tickSize;
    }

    m_book.clear();
    m_bucketBook.clear();
    if (!msg.rows.isEmpty() && m_lastTickSize > 0.0) {
        static constexpr double kEps = 1e-9;
        for (const auto &row : msg.rows) {
            const double bidQty = row.hasBid ? std::max(0.0, row.bid) : 0.0;
            const double askQty = row.hasAsk ? std::max(0.0, row.ask) : 0.0;
            if (bidQty <= 0.0 && askQty <= 0.0) {
                continue;
            }
            BookEntry &entry = m_book[row.tick];
            entry.bidQty = (bidQty < kEps) ? 0.0 : bidQty;
            entry.askQty = (askQty < kEps) ? 0.0 : askQty;
        }
    }

    qint64 minTick = msg.windowMinTick;
    qint64 maxTick = msg.windowMaxTick;
    if (minTick == 0 && maxTick == 0) {
        minTick = m_book.isEmpty() ? qint64(0) : m_book.firstKey();
        maxTick = m_book.isEmpty() ? qint64(0) : m_book.lastKey();
    }
    const bool hasWindow = (m_lastTickSize > 0.0 && minTick <= maxTick);

    m_hasBook = hasWindow;
    if (m_hasBook) {
        m_bufferMinTick = minTick;
        m_bufferMaxTick = maxTick;
        m_centerTick = (msg.centerTick != 0) ? msg.centerTick : (m_bufferMinTick + m_bufferMaxTick) / 2;
        trimBookToWindow(m_bufferMinTick, m_bufferMaxTick);
        rebuildBucketBook(m_bucketBook, m_book, m_tickCompression);

        {
            qint64 bidBucket = 0;
            qint64 askBucket = 0;
            if (computeBestBuckets(m_bucketBook, bidBucket, askBucket)) {
                if (bidBucket < askBucket) {
                    sanitizeBucketBook(m_bucketBook, bidBucket, askBucket);
                    qint64 bid2 = 0;
                    qint64 ask2 = 0;
                    if (computeBestBuckets(m_bucketBook, bid2, ask2) && bid2 < ask2) {
                        m_lastStableBestBidBucketTick = bid2;
                        m_lastStableBestAskBucketTick = ask2;
                    } else {
                        m_lastStableBestBidBucketTick = bidBucket;
                        m_lastStableBestAskBucketTick = askBucket;
                    }
                } else if (bidBucket == askBucket) {
                    // Locked spread: keep the bucket as-is to avoid blanking the DOM.
                    m_lastStableBestBidBucketTick = bidBucket;
                    m_lastStableBestAskBucketTick = askBucket;
                } else {
                    // Crossed: treat as inconsistent; clear and resync.
                    m_lastStableBestBidBucketTick = 0;
                    m_lastStableBestAskBucketTick = 0;
                    m_book.clear();
                    m_bucketBook.clear();
                    m_bestBid = 0.0;
                    m_bestAsk = 0.0;
                    requestForceFull();
                }
            }
        }

        ++m_bookRevision;
        emit bookUpdated(m_bookRevision);
        // IMPORTANT: A full resync can "drop" levels by omission (not included in `rows`).
        // If we only mark buckets touched by `rows`, UI delegates can keep stale bid/ask values
        // until the user scrolls (layout rebuild). Mark the whole window (and previous window)
        // as dirty so empty buckets get cleared immediately.
        {
            static constexpr int kDirtyCap = 4096; // DomWidget forces full recalc above this
            const qint64 c = std::max<qint64>(1, m_tickCompression);
            auto floorBucket = [c](qint64 tick) -> qint64 {
                if (c == 1) return tick;
                if (tick >= 0) return (tick / c) * c;
                const qint64 absTick = -tick;
                const qint64 buckets = (absTick + c - 1) / c;
                return -buckets * c;
            };
            auto ceilBucket = [c](qint64 tick) -> qint64 {
                if (c == 1) return tick;
                if (tick >= 0) return ((tick + c - 1) / c) * c;
                const qint64 absTick = -tick;
                const qint64 buckets = absTick / c;
                return -buckets * c;
            };

            auto bucketCountForRange = [&](qint64 a, qint64 b) -> qint64 {
                if (a == 0 && b == 0) return 0;
                if (a > b) std::swap(a, b);
                const qint64 bmin = floorBucket(a);
                const qint64 bmax = ceilBucket(b);
                if (bmax < bmin) return 0;
                return (bmax - bmin) / c + 1;
            };

            qint64 estimated = 0;
            if (wasReady && prevMin <= prevMax) {
                estimated += bucketCountForRange(prevMin, prevMax);
            }
            if (m_bufferMinTick <= m_bufferMaxTick) {
                estimated += bucketCountForRange(m_bufferMinTick, m_bufferMaxTick);
            }

            if (estimated > kDirtyCap) {
                QVector<qint64> ticks;
                ticks.resize(kDirtyCap + 1);
                for (int i = 0; i < ticks.size(); ++i) {
                    ticks[i] = static_cast<qint64>(i + 1);
                }
                emit bucketTicksUpdated(ticks);
            } else {
                QSet<qint64> dirtyBuckets;
                dirtyBuckets.reserve(static_cast<int>(std::max<qint64>(0, estimated)));
                auto addRange = [&](qint64 a, qint64 b) {
                    if (a == 0 && b == 0) return;
                    if (a > b) std::swap(a, b);
                    const qint64 bmin = floorBucket(a);
                    const qint64 bmax = ceilBucket(b);
                    if (bmax < bmin) return;
                    for (qint64 t = bmin; t <= bmax; t += c) {
                        dirtyBuckets.insert(t);
                    }
                };
                if (wasReady && prevMin <= prevMax) {
                    addRange(prevMin, prevMax);
                }
                addRange(m_bufferMinTick, m_bufferMaxTick);

                if (!dirtyBuckets.isEmpty()) {
                    QVector<qint64> ticks;
                    ticks.reserve(dirtyBuckets.size());
                    for (qint64 t : std::as_const(dirtyBuckets)) {
                        ticks.push_back(t);
                    }
                    emit bucketTicksUpdated(ticks);
                }
            }
        }
        const bool windowChanged =
            (!wasReady)
            || (prevMin != m_bufferMinTick)
            || (prevMax != m_bufferMaxTick)
            || (prevCenter != m_centerTick)
            || (std::abs(prevTick - m_lastTickSize) > 1e-12);
        if (windowChanged) {
            emit bookRangeUpdated(m_bufferMinTick, m_bufferMaxTick, m_centerTick, m_lastTickSize);
        }
    } else {
        m_bufferMinTick = 0;
        m_bufferMaxTick = 0;
        m_centerTick = 0;
        m_book.clear();
        m_bucketBook.clear();
        m_lastStableBestBidBucketTick = 0;
        m_lastStableBestAskBucketTick = 0;
    }
}

void LadderClient::applyDeltaLadderMessage(const ParsedLadderDelta &msg)
{
    if (!m_hasBook) {
        ParsedLadderFull full;
        full.bestBid = msg.bestBid;
        full.bestAsk = msg.bestAsk;
        full.tickSize = msg.tickSize;
        full.windowMinTick = msg.windowMinTick;
        full.windowMaxTick = msg.windowMaxTick;
        full.centerTick = msg.centerTick;
        full.timestampMs = msg.timestampMs;
        applyFullLadderMessage(full);
        return;
    }

    const qint64 prevMin = m_bufferMinTick;
    const qint64 prevMax = m_bufferMaxTick;
    const qint64 prevCenter = m_centerTick;
    const double prevTick = m_lastTickSize;
    const double prevBestBid = m_bestBid;
    const double prevBestAsk = m_bestAsk;

    if (msg.bestBid != 0.0) {
        m_bestBid = msg.bestBid;
    }
    if (msg.bestAsk != 0.0) {
        m_bestAsk = msg.bestAsk;
    }
    const double tickSize = msg.tickSize;
    if (tickSize > 0.0) {
        m_lastTickSize = tickSize;
    }

    QSet<qint64> dirtyBuckets;
    // IMPORTANT: Some backends can update top-of-book (bestBid/bestAsk) without sending
    // a depth delta for the corresponding tick in the same frame. In GPU mode we do
    // incremental updates by dirty buckets, so if best prices change we must mark the
    // old+new best buckets dirty, otherwise stale "best" highlights can remain visible
    // (looks like 2-3 asks/bids) until the user scrolls and forces a full rebuild.
    if (m_lastTickSize > 0.0) {
        auto priceToRawTick = [&](double price) -> qint64 {
            if (!(price > 0.0) || !(m_lastTickSize > 0.0) || !std::isfinite(price) || !std::isfinite(m_lastTickSize)) {
                return 0;
            }
            const double scaled = price / m_lastTickSize;
            if (!std::isfinite(scaled)) {
                return 0;
            }
            const double nudged = scaled + (scaled >= 0.0 ? 1e-9 : -1e-9);
            return static_cast<qint64>(std::llround(nudged));
        };
        const qint64 c = std::max<qint64>(1, m_tickCompression);
        auto floorBucketSigned = [c](qint64 tick) -> qint64 {
            if (c == 1) return tick;
            if (tick >= 0) return (tick / c) * c;
            const qint64 absTick = -tick;
            const qint64 buckets = (absTick + c - 1) / c;
            return -buckets * c;
        };
        auto ceilBucketSigned = [c](qint64 tick) -> qint64 {
            if (c == 1) return tick;
            if (tick >= 0) return ((tick + c - 1) / c) * c;
            const qint64 absTick = -tick;
            const qint64 buckets = absTick / c;
            return -buckets * c;
        };

        const qint64 prevBidTick = priceToRawTick(prevBestBid);
        const qint64 prevAskTick = priceToRawTick(prevBestAsk);
        const qint64 nextBidTick = priceToRawTick(m_bestBid);
        const qint64 nextAskTick = priceToRawTick(m_bestAsk);
        if (prevBidTick != 0) dirtyBuckets.insert(floorBucketSigned(prevBidTick));
        if (nextBidTick != 0) dirtyBuckets.insert(floorBucketSigned(nextBidTick));
        if (prevAskTick != 0) dirtyBuckets.insert(ceilBucketSigned(prevAskTick));
        if (nextAskTick != 0) dirtyBuckets.insert(ceilBucketSigned(nextAskTick));
    }
    if (!msg.updates.isEmpty() && m_lastTickSize > 0.0) {
        static constexpr double kEps = 1e-9;
        dirtyBuckets.reserve(msg.updates.size() * 2);
        for (const auto &row : msg.updates) {
            const qint64 tick = row.tick;
            if (tick == 0) {
                continue;
            }
            double oldBid = 0.0;
            double oldAsk = 0.0;
            auto it = m_book.find(tick);
            BookEntry entry;
            if (it != m_book.end()) {
                entry = it.value();
                oldBid = entry.bidQty;
                oldAsk = entry.askQty;
            }

            if (row.hasBid) {
                const double v = std::max(0.0, row.bid);
                entry.bidQty = (v < kEps) ? 0.0 : v;
            }
            if (row.hasAsk) {
                const double v = std::max(0.0, row.ask);
                entry.askQty = (v < kEps) ? 0.0 : v;
            }

            if (entry.bidQty <= 0.0 && entry.askQty <= 0.0) {
                if (it != m_book.end()) {
                    m_book.erase(it);
                }
            } else {
                if (it != m_book.end()) {
                    it.value() = entry;
                } else {
                    m_book.insert(tick, entry);
                }
            }

            if (row.hasBid) {
                bucketAggAdd(m_bucketBook,
                             floorBucketTickSigned(tick, m_tickCompression),
                             entry.bidQty - oldBid,
                             0.0);
            }
            if (row.hasAsk) {
                bucketAggAdd(m_bucketBook,
                             ceilBucketTickSigned(tick, m_tickCompression),
                             0.0,
                             entry.askQty - oldAsk);
            }

            dirtyBuckets.insert(floorBucketTickSigned(tick, m_tickCompression));
            dirtyBuckets.insert(ceilBucketTickSigned(tick, m_tickCompression));
        }
    }

    if (!msg.removals.isEmpty()) {
        dirtyBuckets.reserve(dirtyBuckets.size() + msg.removals.size() * 2);
        for (qint64 t : msg.removals) {
            if (t == 0) {
                continue;
            }
            auto it = m_book.find(t);
            if (it != m_book.end()) {
                const double bidOld = it->bidQty;
                const double askOld = it->askQty;
                if (bidOld > 0.0) {
                    bucketAggAdd(m_bucketBook, floorBucketTickSigned(t, m_tickCompression), -bidOld, 0.0);
                }
                if (askOld > 0.0) {
                    bucketAggAdd(m_bucketBook, ceilBucketTickSigned(t, m_tickCompression), 0.0, -askOld);
                }
                m_book.erase(it);
            }
            dirtyBuckets.insert(floorBucketTickSigned(t, m_tickCompression));
            dirtyBuckets.insert(ceilBucketTickSigned(t, m_tickCompression));
        }
    }

    // If the book becomes crossed (bid >= ask) due to out-of-order bursts on volatile symbols,
    // request a full ladder resync from the backend (without changing manual center).
    //
    // IMPORTANT: In GPU mode the DOM updates incrementally from `bucketTicksUpdated` + `bookUpdated`.
    // If we early-return here (old behavior), the UI can "freeze" on stale rows until the user scrolls,
    // because no new snapshot pull is triggered. Instead, keep going and force a full DOM recalc for
    // this frame (while still requesting a backend resync).
    bool inconsistentFrame = false;
    const qint64 prevStableBid = m_lastStableBestBidBucketTick;
    const qint64 prevStableAsk = m_lastStableBestAskBucketTick;

    if (m_hasBook) {
        // Robust crossed detection: dirty-bucket heuristics can miss cases where the crossing is
        // caused by stale buckets outside the delta set. Always verify the aggregated top-of-book.
        qint64 bidBucket = 0;
        qint64 askBucket = 0;
        const bool haveTop = computeBestBuckets(m_bucketBook, bidBucket, askBucket);
        const bool crossed = haveTop && bidBucket > askBucket;
        const bool likelyCrossed = (!crossed && crossedBookLikely(dirtyBuckets));
        if (crossed || likelyCrossed) {
            inconsistentFrame = true;
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (m_lastForceFullMs <= 0 || nowMs - m_lastForceFullMs > 500) {
                m_lastForceFullMs = nowMs;
                emitStatus(QStringLiteral("%1 Crossed book detected; forcing full resync...")
                               .arg(formatBackendPrefix()));
                requestForceFull();
            }
            if (crossed) {
                // Fail-safe: clear local state so ghosts cannot persist; MainWindow will now
                // push an empty snapshot to DomWidget to clear rendering immediately.
                m_book.clear();
                m_bucketBook.clear();
                m_bestBid = 0.0;
                m_bestAsk = 0.0;
                m_lastStableBestBidBucketTick = 0;
                m_lastStableBestAskBucketTick = 0;
            }
        }
    }

    const bool hasWindow = (msg.windowMinTick != 0 || msg.windowMaxTick != 0);
    const qint64 minTick = hasWindow ? msg.windowMinTick : m_bufferMinTick;
    const qint64 maxTick = hasWindow ? msg.windowMaxTick : m_bufferMaxTick;
    if (minTick <= maxTick) {
        m_bufferMinTick = minTick;
        m_bufferMaxTick = maxTick;
    }
    if (msg.centerTick != 0) {
        m_centerTick = msg.centerTick;
    }
    trimBookToWindow(m_bufferMinTick, m_bufferMaxTick, &dirtyBuckets);

    m_hasBook = (m_lastTickSize > 0.0 && m_bufferMinTick <= m_bufferMaxTick);
    if (m_hasBook) {
        {
            qint64 bidBucket = 0;
            qint64 askBucket = 0;
            if (computeBestBuckets(m_bucketBook, bidBucket, askBucket)) {
                if (bidBucket < askBucket) {
                    sanitizeBucketBook(m_bucketBook, bidBucket, askBucket, &dirtyBuckets);
                    qint64 bid2 = 0;
                    qint64 ask2 = 0;
                    if (computeBestBuckets(m_bucketBook, bid2, ask2) && bid2 < ask2) {
                        m_lastStableBestBidBucketTick = bid2;
                        m_lastStableBestAskBucketTick = ask2;
                    } else {
                        m_lastStableBestBidBucketTick = bidBucket;
                        m_lastStableBestAskBucketTick = askBucket;
                    }
                } else if (bidBucket == askBucket) {
                    // Locked spread: keep the bucket as-is to avoid blanking the DOM.
                    m_lastStableBestBidBucketTick = bidBucket;
                    m_lastStableBestAskBucketTick = askBucket;
                }
            }
        }
        // If top-of-book buckets moved, mark the affected range dirty so GPU rows re-evaluate
        // the spread guard logic (prevents stale "wrong side" levels from sticking).
        const qint64 compression = std::max<qint64>(1, m_tickCompression);
        auto markRange = [&](qint64 a, qint64 b) {
            if (a == 0 || b == 0) {
                return;
            }
            if (a > b) {
                std::swap(a, b);
            }
            const qint64 count = (b - a) / compression + 1;
            if (count > 4096) {
                // Force full recalc.
                dirtyBuckets.clear();
                dirtyBuckets.insert(1);
                dirtyBuckets.insert(2);
                dirtyBuckets.insert(3);
                dirtyBuckets.insert(4);
                dirtyBuckets.insert(5);
                return;
            }
            for (qint64 t = a; t <= b; t += compression) {
                dirtyBuckets.insert(t);
            }
        };
        if (prevStableBid != m_lastStableBestBidBucketTick) {
            if (prevStableBid != 0 && m_lastStableBestBidBucketTick != 0) {
                markRange(prevStableBid, m_lastStableBestBidBucketTick);
            } else if (m_lastStableBestBidBucketTick != 0) {
                dirtyBuckets.insert(m_lastStableBestBidBucketTick);
            } else if (prevStableBid != 0) {
                dirtyBuckets.insert(prevStableBid);
            }
        }
        if (prevStableAsk != m_lastStableBestAskBucketTick) {
            if (prevStableAsk != 0 && m_lastStableBestAskBucketTick != 0) {
                markRange(prevStableAsk, m_lastStableBestAskBucketTick);
            } else if (m_lastStableBestAskBucketTick != 0) {
                dirtyBuckets.insert(m_lastStableBestAskBucketTick);
            } else if (prevStableAsk != 0) {
                dirtyBuckets.insert(prevStableAsk);
            }
        }

        ++m_bookRevision;
        emit bookUpdated(m_bookRevision);
        if (inconsistentFrame) {
            // Force a full recalc in DomWidget for immediate visual recovery.
            QVector<qint64> ticks;
            ticks.resize(4097);
            for (int i = 0; i < ticks.size(); ++i) {
                ticks[i] = static_cast<qint64>(i + 1);
            }
            emit bucketTicksUpdated(ticks);
        } else if (!dirtyBuckets.isEmpty()) {
            // Too many dirty buckets => force a full recalc on the DOM side.
            if (dirtyBuckets.size() > 4096) {
                QVector<qint64> ticks;
                ticks.resize(4097);
                for (int i = 0; i < ticks.size(); ++i) {
                    ticks[i] = static_cast<qint64>(i + 1);
                }
                emit bucketTicksUpdated(ticks);
            } else {
                QVector<qint64> ticks;
                ticks.reserve(dirtyBuckets.size());
                for (qint64 t : std::as_const(dirtyBuckets)) {
                    ticks.push_back(t);
                }
                emit bucketTicksUpdated(ticks);
            }
        }
        const bool windowChanged =
            (prevMin != m_bufferMinTick)
            || (prevMax != m_bufferMaxTick)
            || (prevCenter != m_centerTick)
            || (std::abs(prevTick - m_lastTickSize) > 1e-12);
        if (windowChanged) {
            emit bookRangeUpdated(m_bufferMinTick, m_bufferMaxTick, m_centerTick, m_lastTickSize);
        }
    } else {
        m_bufferMinTick = 0;
        m_bufferMaxTick = 0;
        m_centerTick = 0;
        m_book.clear();
        m_lastStableBestBidBucketTick = 0;
        m_lastStableBestAskBucketTick = 0;
    }
}

void LadderClient::trimBookToWindow(qint64 minTick, qint64 maxTick, QSet<qint64> *dirtyBuckets)
{
    if (minTick > maxTick) {
        return;
    }
    auto it = m_book.begin();
    while (it != m_book.end()) {
        if (it.key() < minTick || it.key() > maxTick) {
            const qint64 tick = it.key();
            const double bidOld = it->bidQty;
            const double askOld = it->askQty;
            if (bidOld > 0.0) {
                bucketAggAdd(m_bucketBook, floorBucketTickSigned(tick, m_tickCompression), -bidOld, 0.0);
                if (dirtyBuckets) {
                    dirtyBuckets->insert(floorBucketTickSigned(tick, m_tickCompression));
                }
            }
            if (askOld > 0.0) {
                bucketAggAdd(m_bucketBook, ceilBucketTickSigned(tick, m_tickCompression), 0.0, -askOld);
                if (dirtyBuckets) {
                    dirtyBuckets->insert(ceilBucketTickSigned(tick, m_tickCompression));
                }
            }
            it = m_book.erase(it);
        } else {
            ++it;
        }
    }
}
void LadderClient::emitStatus(const QString &msg)
{
    const QString symbol = m_symbol.toUpper();
    QString exchangeLabel = m_exchange.toUpper();
    if (exchangeLabel.isEmpty()) {
        exchangeLabel = QStringLiteral("auto");
    }
    const QString decorated = QStringLiteral("[%1@%2] %3").arg(symbol, exchangeLabel, msg);
    emit statusMessage(decorated);
}

void LadderClient::armWatchdog()
{
    m_lastUpdateMs = QDateTime::currentMSecsSinceEpoch();
    if (m_watchdogIntervalMs > 0) {
        m_watchdogTimer.start(m_watchdogIntervalMs);
    }
}

void LadderClient::handleWatchdogTimeout()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastUpdateMs < m_watchdogIntervalMs - 50) {
        // Data arrived while timer was firing.
        return;
    }
    emitStatus(QStringLiteral("No data received for %1s, restarting backend...")
                   .arg(m_watchdogIntervalMs / 1000));
    restart(m_symbol, m_levels, m_exchange);
}
DomSnapshot LadderClient::buildSnapshot(qint64 minTick, qint64 maxTick) const
{
    DomSnapshot snap;
    if (minTick > maxTick) {
        std::swap(minTick, maxTick);
    }
    snap.tickSize = m_lastTickSize;
    snap.bestBid = m_bestBid;
    snap.bestAsk = m_bestAsk;
    if (m_lastTickSize <= 0.0) {
        return snap;
    }

    const qint64 compression = std::max<qint64>(1, m_tickCompression);
    snap.compression = compression;
    auto floorBucket = [compression](qint64 tick) -> qint64 {
        if (compression == 1) {
            return tick;
        }
        if (tick >= 0) {
            return (tick / compression) * compression;
        }
        const qint64 absTick = -tick;
        const qint64 buckets = (absTick + compression - 1) / compression;
        return -buckets * compression;
    };
    auto ceilBucket = [compression](qint64 tick) -> qint64 {
        if (compression == 1) {
            return tick;
        }
        if (tick >= 0) {
            return ((tick + compression - 1) / compression) * compression;
        }
        const qint64 absTick = -tick;
        const qint64 buckets = absTick / compression;
        return -buckets * compression;
    };

    // Align compressed buckets so bids are floored and asks are ceiled. This prevents the
    // ask side from "shifting down" by up to (compression-1) ticks compared to other ladders.
    const qint64 bucketMinTick = floorBucket(minTick);
    const qint64 bucketMaxTick = ceilBucket(maxTick);
    snap.minTick = bucketMinTick;
    snap.maxTick = bucketMaxTick;
    if (bucketMaxTick < bucketMinTick) {
        return snap;
    }
    const qint64 bucketCount = (bucketMaxTick - bucketMinTick) / compression + 1;
    if (bucketCount <= 0 || bucketCount > 2000000) {
        return snap;
    }

    QVector<DomLevel> buckets;
    buckets.resize(static_cast<int>(bucketCount));
    for (qint64 i = 0; i < bucketCount; ++i) {
        const qint64 bucketTick = bucketMinTick + i * compression;
        buckets[static_cast<int>(i)].tick = bucketTick;
        buckets[static_cast<int>(i)].price = static_cast<double>(bucketTick) * snap.tickSize;
    }

    for (qint64 i = 0; i < bucketCount; ++i) {
        const qint64 bucketTick = bucketMinTick + i * compression;
        auto it = m_bucketBook.constFind(bucketTick);
        if (it == m_bucketBook.constEnd()) {
            continue;
        }
        buckets[static_cast<int>(i)].bidQty = it->bidQty;
        buckets[static_cast<int>(i)].askQty = it->askQty;
    }

    auto priceToRawTick = [&](double price) -> qint64 {
        if (!(price > 0.0) || !(snap.tickSize > 0.0) || !std::isfinite(price) || !std::isfinite(snap.tickSize)) {
            return 0;
        }
        const double scaled = price / snap.tickSize;
        if (!std::isfinite(scaled)) {
            return 0;
        }
        const double nudged = scaled + (scaled >= 0.0 ? 1e-9 : -1e-9);
        return static_cast<qint64>(std::llround(nudged));
    };
    qint64 backendBidBucket = 0;
    qint64 backendAskBucket = 0;
    bool backendHasBid = false;
    bool backendHasAsk = false;
    if (snap.bestBid > 0.0) {
        const qint64 bestBidTick = priceToRawTick(snap.bestBid);
        if (bestBidTick != 0) {
            backendBidBucket = floorBucket(bestBidTick);
            backendHasBid = true;
        }
    }
    if (snap.bestAsk > 0.0) {
        const qint64 bestAskTick = priceToRawTick(snap.bestAsk);
        if (bestAskTick != 0) {
            backendAskBucket = ceilBucket(bestAskTick);
            backendHasAsk = true;
        }
    }
    const bool backendOk =
        backendHasBid && backendHasAsk && backendBidBucket < backendAskBucket;

    // Sanitize: never show asks at/below best bid bucket, and never show bids at/above best ask bucket.
    // Under volatile / out-of-order frames, aggregated buckets can momentarily contain stale side data
    // that would look "impossible" in the UI. This keeps rendering sane until the next resync.
    qint64 bestBidBucket = m_lastStableBestBidBucketTick;
    qint64 bestAskBucket = m_lastStableBestAskBucketTick;
    bool haveBid = (bestBidBucket != 0);
    bool haveAsk = (bestAskBucket != 0);

    if (!haveBid || !haveAsk) {
        // Fallback: derive from the current snapshot rows.
        bestBidBucket = 0;
        bestAskBucket = 0;
        haveBid = false;
        haveAsk = false;
        for (const auto &lvl : buckets) {
            if (lvl.tick == 0) continue;
            if (lvl.bidQty > 0.0) {
                if (!haveBid || lvl.tick > bestBidBucket) {
                    bestBidBucket = lvl.tick;
                    haveBid = true;
                }
            }
            if (lvl.askQty > 0.0) {
                if (!haveAsk || lvl.tick < bestAskBucket) {
                    bestAskBucket = lvl.tick;
                    haveAsk = true;
                }
            }
        }
    }

    if (!haveBid || !haveAsk) {
        if (!haveBid && backendHasBid) {
            bestBidBucket = backendBidBucket;
            haveBid = true;
        }
        if (!haveAsk && backendHasAsk) {
            bestAskBucket = backendAskBucket;
            haveAsk = true;
        }
    }

    // If buckets are inconsistent, fall back to backend best-bid/ask buckets when available.
    if (haveBid && haveAsk && bestBidBucket >= bestAskBucket && backendOk) {
        bestBidBucket = backendBidBucket;
        bestAskBucket = backendAskBucket;
    }

    const bool lockedSpread = haveBid && haveAsk && bestBidBucket == bestAskBucket;
    if (haveBid || haveAsk) {
        for (auto &lvl : buckets) {
            if (haveBid) {
                if (lockedSpread) {
                    if (lvl.tick < bestBidBucket) {
                        lvl.askQty = 0.0;
                    }
                } else if (lvl.tick <= bestBidBucket) {
                    lvl.askQty = 0.0;
                }
            }
            if (haveAsk) {
                if (lockedSpread) {
                    if (lvl.tick > bestAskBucket) {
                        lvl.bidQty = 0.0;
                    }
                } else if (lvl.tick >= bestAskBucket) {
                    lvl.bidQty = 0.0;
                }
            }
        }
    }

    // Strong invariant: there must be no liquidity strictly inside the spread range.
    // If a transient out-of-order delta makes us aggregate stale bid+ask into interior buckets,
    // clear them so "impossible" levels can't remain visible until the next resync.
    if (haveBid && haveAsk && bestBidBucket < bestAskBucket) {
        for (auto &lvl : buckets) {
            if (lvl.tick > bestBidBucket && lvl.tick < bestAskBucket) {
                lvl.bidQty = 0.0;
                lvl.askQty = 0.0;
            }
        }
    }

    // Keep displayed best prices consistent with sanitized buckets when possible.
    if (haveBid) {
        snap.bestBid = static_cast<double>(bestBidBucket) * snap.tickSize;
    }
    if (haveAsk) {
        snap.bestAsk = static_cast<double>(bestAskBucket) * snap.tickSize;
    }

    snap.levels.reserve(buckets.size());
    for (int i = buckets.size() - 1; i >= 0; --i) {
        snap.levels.push_back(buckets[i]);
    }
    return snap;
}

#include "LadderClient.moc"
