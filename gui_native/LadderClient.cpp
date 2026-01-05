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

#include <json.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>

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

static bool quantizePriceToTick(const json &priceValue,
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

QString LadderClient::backendLogPath() const
{
    return fusionLogFilePath();
}

void LadderClient::logBackendEvent(const QString &line)
{
    const QString path = backendLogPath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODate)
       << " [backend] " << formatBackendPrefix() << " [event] " << line << "\n";
}

void LadderClient::logBackendLine(const QString &line)
{
    const QString path = backendLogPath();
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }
    QTextStream ts(&f);
    ts << QDateTime::currentDateTime().toString(Qt::ISODate)
       << " [backend] " << formatBackendPrefix() << " [stderr] " << line << "\n";
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
    m_tickCompression = std::max(1, factor);
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
            processLine(line);
        }
    }
}

void LadderClient::handleReadyReadStderr()
{
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
        qWarning() << "[LadderClient stderr]" << text;
        if (text.contains(QStringLiteral("proxy enabled:"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("lighter:"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("lighter ws"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("lighter orderBookDetails"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("httpGetQt failed"), Qt::CaseInsensitive)) {
            emitStatus(QStringLiteral("%1 %2").arg(formatBackendPrefix(), text));
        }
        appendRecent(m_recentStderr, text, 80);
        logBackendLine(text);
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

void LadderClient::processLine(const QByteArray &line)
{
    json j;
    try {
        j = json::parse(line.constData(), line.constData() + line.size());
    } catch (const std::exception &ex) {
        qWarning() << "[LadderClient] parse error:" << ex.what();
        emitStatus("Parse error: " + QString::fromUtf8(ex.what()));
        return;
    }

    const std::string type = j.value("type", std::string());
    armWatchdog();
    if (type == "trade") {
        if (!m_prints) {
            return;
        }
        double price = j.value("price", 0.0);
        const double qtyBase = j.value("qty", 0.0);
        const std::string side = j.value("side", std::string("buy"));
        if (price <= 0.0 || qtyBase <= 0.0) {
            return;
        }
        qint64 tick = 0;
        if (j.contains("tick") && parseTickValue(j["tick"], tick)) {
            if (m_lastTickSize > 0.0) {
                price = static_cast<double>(tick) * m_lastTickSize;
            }
        }
        if (tick == 0 && m_lastTickSize > 0.0) {
            auto priceIt = j.find("price");
            double snappedPrice = 0.0;
            if (priceIt != j.end() && quantizePriceToTick(*priceIt, m_lastTickSize, tick, snappedPrice)) {
                price = snappedPrice;
            } else {
                tick = static_cast<qint64>(std::llround(price / m_lastTickSize));
                price = static_cast<double>(tick) * m_lastTickSize;
            }
        }

        const double qtyQuote = price * qtyBase;
        if (qtyQuote <= 0.0) {
            return;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        PrintItem it;
        it.price = price;
        it.qty = qtyQuote;
        it.buy = (side != "sell");
        it.rowHint = -1;
        it.tick = tick;
        it.timeMs = nowMs;
        it.seq = ++m_printSeq;
        m_printBuffer.push_back(it);
        // IMPORTANT: prints UI only renders a small tail (<= ~64 slots). Keeping thousands of prints
        // and shifting the vector on every trade can freeze the whole UI on high-throughput symbols
        // like BTC. Keep a small rolling buffer instead.
        const int maxPrints = 128;
        if (m_printBuffer.size() > maxPrints) {
            m_printBuffer.erase(m_printBuffer.begin(),
                                m_printBuffer.begin() + (m_printBuffer.size() - maxPrints));
        }
        m_prints->setPrints(m_printBuffer);
        return;
    }

    bool handledLadder = false;
    if (type == "ladder_delta") {
        applyDeltaLadderMessage(j);
        handledLadder = true;
    } else if (type == "ladder") {
        applyFullLadderMessage(j);
        handledLadder = true;
    } else {
        return;
    }

    if (!handledLadder) {
        return;
    }

    const auto tsIt = j.find("timestamp");
    if (tsIt != j.end() && tsIt->is_number_integer()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 tsMs = static_cast<qint64>(tsIt->get<std::int64_t>());
        const int pingMs = static_cast<int>(std::max<qint64>(0, nowMs - tsMs));
        emit pingUpdated(pingMs);
    } else {
        // no-op: avoid spamming status (it also makes column width jitter)
    }
}

void LadderClient::applyFullLadderMessage(const json &j)
{
    m_bestBid = j.value("bestBid", 0.0);
    m_bestAsk = j.value("bestAsk", 0.0);
    const double tickSize = j.value("tickSize", 0.0);
    if (tickSize > 0.0) {
        m_lastTickSize = tickSize;
    }

    auto rowsIt = j.find("rows");
    m_book.clear();
    if (rowsIt != j.end() && rowsIt->is_array() && m_lastTickSize > 0.0) {
        for (const auto &row : *rowsIt) {
            const double bidQty = row.value("bid", 0.0);
            const double askQty = row.value("ask", 0.0);
            if (bidQty <= 0.0 && askQty <= 0.0) {
                continue;
            }
            qint64 tick = 0;
            if (row.contains("tick") && parseTickValue(row["tick"], tick)) {
            } else {
                const double price = row.value("price", 0.0);
                tick = static_cast<qint64>(std::llround(price / m_lastTickSize));
            }
            BookEntry &entry = m_book[tick];
            entry.bidQty = bidQty;
            entry.askQty = askQty;
        }
    }

    const qint64 minTick =
        j.value("windowMinTick", m_book.isEmpty() ? qint64(0) : m_book.firstKey());
    const qint64 maxTick =
        j.value("windowMaxTick", m_book.isEmpty() ? qint64(0) : m_book.lastKey());
    const bool hasWindow = (m_lastTickSize > 0.0 && minTick <= maxTick);

    m_hasBook = hasWindow;
    if (m_hasBook) {
        m_bufferMinTick = minTick;
        m_bufferMaxTick = maxTick;
        m_centerTick = j.value("centerTick", (m_bufferMinTick + m_bufferMaxTick) / 2);
        trimBookToWindow(m_bufferMinTick, m_bufferMaxTick);
        emit bookRangeUpdated(m_bufferMinTick, m_bufferMaxTick, m_centerTick, m_lastTickSize);
    } else {
        m_bufferMinTick = 0;
        m_bufferMaxTick = 0;
        m_centerTick = 0;
        m_book.clear();
    }
}

void LadderClient::applyDeltaLadderMessage(const json &j)
{
    if (!m_hasBook) {
        applyFullLadderMessage(j);
        return;
    }

    m_bestBid = j.value("bestBid", m_bestBid);
    m_bestAsk = j.value("bestAsk", m_bestAsk);
    const double tickSize = j.value("tickSize", 0.0);
    if (tickSize > 0.0) {
        m_lastTickSize = tickSize;
    }

    auto updatesIt = j.find("updates");
    if (updatesIt != j.end() && updatesIt->is_array() && m_lastTickSize > 0.0) {
        for (const auto &row : *updatesIt) {
            qint64 tick = 0;
            if (row.contains("tick") && parseTickValue(row["tick"], tick)) {
            } else {
                const double price = row.value("price", 0.0);
                tick = static_cast<qint64>(std::llround(price / m_lastTickSize));
            }
            BookEntry &entry = m_book[tick];
            entry.bidQty = row.value("bid", entry.bidQty);
            entry.askQty = row.value("ask", entry.askQty);
            if (entry.bidQty <= 0.0 && entry.askQty <= 0.0) {
                m_book.remove(tick);
            }
        }
    }

    auto removalsIt = j.find("removals");
    if (removalsIt != j.end() && removalsIt->is_array()) {
        for (const auto &tickValue : *removalsIt) {
            if (tickValue.is_number_integer()) {
                const qint64 tick = static_cast<qint64>(tickValue.get<std::int64_t>());
                m_book.remove(tick);
            }
        }
    }

    const qint64 minTick = j.value("windowMinTick", m_bufferMinTick);
    const qint64 maxTick = j.value("windowMaxTick", m_bufferMaxTick);
    if (minTick <= maxTick) {
        m_bufferMinTick = minTick;
        m_bufferMaxTick = maxTick;
    }
    m_centerTick = j.value("centerTick", m_centerTick);
    trimBookToWindow(m_bufferMinTick, m_bufferMaxTick);

    m_hasBook = (m_lastTickSize > 0.0 && m_bufferMinTick <= m_bufferMaxTick);
    if (m_hasBook) {
        emit bookRangeUpdated(m_bufferMinTick, m_bufferMaxTick, m_centerTick, m_lastTickSize);
    } else {
        m_bufferMinTick = 0;
        m_bufferMaxTick = 0;
        m_centerTick = 0;
        m_book.clear();
    }
}

void LadderClient::trimBookToWindow(qint64 minTick, qint64 maxTick)
{
    if (minTick > maxTick) {
        return;
    }
    auto it = m_book.begin();
    while (it != m_book.end()) {
        if (it.key() < minTick || it.key() > maxTick) {
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

    auto it = m_book.lowerBound(minTick);
    for (; it != m_book.constEnd() && it.key() <= maxTick; ++it) {
        if (it->bidQty > 0.0) {
            const qint64 bucketTick = floorBucket(it.key());
            const qint64 idx = (bucketTick - bucketMinTick) / compression;
            if (idx >= 0 && idx < bucketCount) {
                buckets[static_cast<int>(idx)].bidQty += it->bidQty;
            }
        }
        if (it->askQty > 0.0) {
            const qint64 bucketTick = ceilBucket(it.key());
            const qint64 idx = (bucketTick - bucketMinTick) / compression;
            if (idx >= 0 && idx < bucketCount) {
                buckets[static_cast<int>(idx)].askQty += it->askQty;
            }
        }
    }

    snap.levels.reserve(buckets.size());
    for (int i = buckets.size() - 1; i >= 0; --i) {
        snap.levels.push_back(buckets[i]);
    }
    return snap;
}
