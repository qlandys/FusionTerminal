// Lightweight client that runs orderbook_backend.exe and feeds DomWidget.

#pragma once

#include "DomWidget.h"
#include "PrintsWidget.h"
#include <json.hpp>

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QMap>

class LadderClient : public QObject {
    Q_OBJECT

public:
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

private slots:
    void handleReadyRead();
    void handleReadyReadStderr();
    void handleErrorOccurred(QProcess::ProcessError error);
    void handleFinished(int exitCode, QProcess::ExitStatus status);
    void handleWatchdogTimeout();

signals:
    void statusMessage(const QString &message);
    void pingUpdated(int milliseconds);
    void bookRangeUpdated(qint64 minTick, qint64 maxTick, qint64 centerTick, double tickSize);

private:
    void emitStatus(const QString &msg);
    void processLine(const QByteArray &line);
    void armWatchdog();
    void logBackendLine(const QString &line);
    void logBackendEvent(const QString &line);
    QString backendLogPath() const;
    QString formatBackendPrefix() const;
    QString formatCrashSummary(int exitCode, QProcess::ExitStatus status) const;
    void applyFullLadderMessage(const nlohmann::json &j);
    void applyDeltaLadderMessage(const nlohmann::json &j);
    void trimBookToWindow(qint64 minTick, qint64 maxTick);

    DomSnapshot buildSnapshot(qint64 minTick, qint64 maxTick) const;

    struct BookEntry {
        double bidQty = 0.0;
        double askQty = 0.0;
    };

    QString m_backendPath;
    QString m_symbol;
    int m_levels;
    QString m_exchange;
    QString m_proxyType;
    QString m_proxy;
    QProcess m_process;
    QByteArray m_buffer;
    class PrintsWidget *m_prints;
    QVector<PrintItem> m_printBuffer;
    quint64 m_printSeq = 0;
    QTimer m_watchdogTimer;
    qint64 m_lastUpdateMs = 0;
    const int m_watchdogIntervalMs = 15000;
    int m_tickCompression = 1;
    QMap<qint64, BookEntry> m_book; // ascending ticks
    qint64 m_bufferMinTick = 0;
    qint64 m_bufferMaxTick = 0;
    qint64 m_centerTick = 0;
    double m_lastTickSize = 0.0;
    bool m_hasBook = false;
    double m_bestBid = 0.0;
    double m_bestAsk = 0.0;
    bool m_stopRequested = false;

    QStringList m_recentStderr;
    int m_lastExitCode = 0;
    QProcess::ExitStatus m_lastExitStatus = QProcess::NormalExit;
    QProcess::ProcessError m_lastProcessError = QProcess::UnknownError;
    QString m_lastProcessErrorString;
    bool m_restartInProgress = false;
};
