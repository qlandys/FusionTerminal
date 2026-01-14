// Lightweight client that runs orderbook_backend.exe and feeds DomWidget.

#pragma once

#include "DomWidget.h"
#include "PrintsWidget.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QMap>
#include <QHash>

struct ParsedLadderRow {
    qint64 tick = 0;
    bool hasBid = false;
    double bid = 0.0;
    bool hasAsk = false;
    double ask = 0.0;
};
Q_DECLARE_METATYPE(ParsedLadderRow)

struct ParsedTradeEvent {
    double price = 0.0;
    double qtyBase = 0.0;
    bool buy = true;
    qint64 tick = 0; // 0 means "unknown"
};
Q_DECLARE_METATYPE(ParsedTradeEvent)

struct ParsedLadderFull {
    double bestBid = 0.0;
    double bestAsk = 0.0;
    double tickSize = 0.0;
    qint64 windowMinTick = 0;
    qint64 windowMaxTick = 0;
    qint64 centerTick = 0;
    qint64 timestampMs = 0; // 0 means "missing"
    QVector<ParsedLadderRow> rows;
};
Q_DECLARE_METATYPE(ParsedLadderFull)

struct ParsedLadderDelta {
    double bestBid = 0.0;
    double bestAsk = 0.0;
    double tickSize = 0.0;
    qint64 windowMinTick = 0;
    qint64 windowMaxTick = 0;
    qint64 centerTick = 0;
    qint64 timestampMs = 0; // 0 means "missing"
    QVector<ParsedLadderRow> updates;
    QVector<qint64> removals;
};
Q_DECLARE_METATYPE(ParsedLadderDelta)

class LadderClient : public QObject {
    Q_OBJECT

public:
    struct BookEntry {
        double bidQty = 0.0;
        double askQty = 0.0;
    };

    explicit LadderClient(const QString &backendPath,
                          const QString &symbol,
                          int levels,
                          const QString &exchange,
                          QObject *parent = nullptr,
                          class PrintsWidget *prints = nullptr,
                          const QString &proxyType = QString(),
                          const QString &proxy = QString());
    ~LadderClient() override;

    void restart(const QString &symbol, int levels, const QString &exchange = QString());
    void stop();
    bool isRunning() const;
    void setProxy(const QString &proxyType, const QString &proxy);
    void setCompression(int factor);
    int compression() const { return m_tickCompression; }
    void shiftWindowTicks(qint64 ticks);
    void resetManualCenter();
    DomSnapshot snapshotForRange(qint64 minTick, qint64 maxTick) const;
    qint64 bufferMinTick() const { return m_bufferMinTick; }
    qint64 bufferMaxTick() const { return m_bufferMaxTick; }
    qint64 centerTick() const { return m_centerTick; }
    double tickSize() const { return m_lastTickSize; }
    bool hasBook() const { return m_hasBook; }
    quint64 bookRevision() const { return m_bookRevision; }

private slots:
    void handleReadyRead();
    void handleReadyReadStderr();
    void handleErrorOccurred(QProcess::ProcessError error);
    void handleFinished(int exitCode, QProcess::ExitStatus status);
    void handleWatchdogTimeout();

public slots:
    void handleParsedTrade(const ParsedTradeEvent &ev);
    void handleParsedTrades(const QVector<ParsedTradeEvent> &events);
    void handleParsedLadderFull(const ParsedLadderFull &msg);
    void handleParsedLadderDelta(const ParsedLadderDelta &msg);
    void handleParsedLadderDeltas(const QVector<ParsedLadderDelta> &msgs);

signals:
    void statusMessage(const QString &message);
    void pingUpdated(int milliseconds);
    void bookRangeUpdated(qint64 minTick, qint64 maxTick, qint64 centerTick, double tickSize);
    void bookUpdated(quint64 revision);
    void bucketTicksUpdated(const QVector<qint64> &bucketTicks);
    void parseLinesRequested(const QVector<QByteArray> &lines);

private:
    void requestForceFull();
    bool crossedBookLikely(const QSet<qint64> &dirtyBuckets) const;
    void emitStatus(const QString &msg);
    void armWatchdog();
    void logBackendLine(const QString &line);
    void logBackendEvent(const QString &line);
    QString backendLogPath() const;
    QString formatBackendPrefix() const;
    QString formatCrashSummary(int exitCode, QProcess::ExitStatus status) const;
    void applyFullLadderMessage(const ParsedLadderFull &msg);
    void applyDeltaLadderMessage(const ParsedLadderDelta &msg);
    void trimBookToWindow(qint64 minTick, qint64 maxTick, QSet<qint64> *dirtyBuckets = nullptr);

    DomSnapshot buildSnapshot(qint64 minTick, qint64 maxTick) const;

    QString m_backendPath;
    QString m_symbol;
    int m_levels;
    QString m_exchange;
    QString m_proxyType;
    QString m_proxy;
    QProcess m_process;
    QByteArray m_buffer;
    QVector<QByteArray> m_pendingParseLines;
    bool m_parseEmitScheduled = false;
    class PrintsWidget *m_prints;
    QVector<PrintItem> m_printBuffer;
    QVector<PrintItem> m_pendingPrintItems;
    bool m_printFlushScheduled = false;
    quint64 m_printSeq = 0;
    QTimer m_watchdogTimer;
    qint64 m_lastUpdateMs = 0;
    const int m_watchdogIntervalMs = 15000;
    int m_tickCompression = 1;
    QMap<qint64, BookEntry> m_book; // ascending ticks
    QHash<qint64, BookEntry> m_bucketBook; // aggregated per bucket tick
    qint64 m_bufferMinTick = 0;
    qint64 m_bufferMaxTick = 0;
    qint64 m_centerTick = 0;
    double m_lastTickSize = 0.0;
    bool m_hasBook = false;
    double m_bestBid = 0.0;
    double m_bestAsk = 0.0;
    bool m_stopRequested = false;
    quint64 m_bookRevision = 0;

    QObject *m_parseWorker = nullptr;

    // Last stable top-of-book buckets (computed from aggregated bucket book).
    // Used to keep DOM sanitization/highlighting consistent during transient out-of-order frames.
    qint64 m_lastStableBestBidBucketTick = 0;
    qint64 m_lastStableBestAskBucketTick = 0;

    QStringList m_recentStderr;
    int m_lastExitCode = 0;
    QProcess::ExitStatus m_lastExitStatus = QProcess::NormalExit;
    QProcess::ProcessError m_lastProcessError = QProcess::UnknownError;
    QString m_lastProcessErrorString;
    bool m_restartInProgress = false;
    qint64 m_lastForceFullMs = 0;
};
