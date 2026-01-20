#pragma once

#include "TradeTypes.h"
#include "ConnectionStore.h"
#include "DomWidget.h"

#include <QObject>
#include <QAbstractSocket>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QStringList>
#include <QUrlQuery>
#include <QTimer>
#include <QWebSocket>
#include <functional>

class QProcess;

class TradeManager : public QObject {
    Q_OBJECT

public:
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        Error
    };
    Q_ENUM(ConnectionState)

    explicit TradeManager(QObject *parent = nullptr);
    ~TradeManager();

    struct FinrezRow {
        QString asset;
        double pnl = 0.0;       // realized P&L in asset currency
        double commission = 0.0; // fees in asset currency
        double funds = 0.0;     // total funds (available + locked)
        double available = 0.0;
        double locked = 0.0;
        qint64 updatedMs = 0;
    };

    void setCredentials(ConnectionStore::Profile profile, const MexcCredentials &creds);
    MexcCredentials credentials(ConnectionStore::Profile profile) const;
    ConnectionState state(ConnectionStore::Profile profile) const;
    ConnectionState overallState() const;
    QString accountName(ConnectionStore::Profile profile) const;

    QVector<FinrezRow> finrezRows(ConnectionStore::Profile profile) const;
    void resetFinrez(ConnectionStore::Profile profile);

    void connectToExchange(ConnectionStore::Profile profile);
    void disconnect(ConnectionStore::Profile profile);
    bool isConnected(ConnectionStore::Profile profile) const;

    void placeLimitOrder(const QString &symbol,
                         const QString &accountName,
                         double price,
                         double quantity,
                         OrderSide side,
                         int leverage = 0);
    void closePositionMarket(const QString &symbol,
                              const QString &accountName,
                              double priceHint = 0.0);
    void placeLighterStopOrder(const QString &symbol,
                               const QString &accountName,
                               double triggerPrice,
                               bool isStopLoss);
    void cancelLighterStopOrders(const QString &symbol,
                                 const QString &accountName,
                                 bool isStopLoss);
    void cancelAllOrders(const QString &symbol, const QString &accountName);
    void cancelOrder(const QString &symbol, const QString &accountName, const QString &orderId);

    TradePosition positionForSymbol(const QString &symbol, const QString &accountName) const;
    QVector<ExecutedTrade> executedTrades() const;
    void clearExecutedTrades();
    void setWatchedSymbols(const QString &accountName, const QSet<QString> &symbols);

signals:
    void connectionStateChanged(ConnectionStore::Profile profile,
                                TradeManager::ConnectionState state,
                                const QString &message);
    void orderPlaced(const QString &accountName,
                     const QString &symbol,
                     OrderSide side,
                     double price,
                     double quantity,
                     const QString &orderId);
    void orderCanceled(const QString &accountName,
                       const QString &symbol,
                       OrderSide side,
                       double price,
                       const QString &orderId);
    void orderFailed(const QString &accountName, const QString &symbol, const QString &message);
    void positionChanged(const QString &accountName,
                         const QString &symbol,
                         const TradePosition &position);
    void tradeExecuted(const ExecutedTrade &trade);
    void tradeHistoryCleared();
    void finrezChanged(ConnectionStore::Profile profile);
    void logMessage(const QString &message);
    void localOrdersUpdated(const QString &accountName,
                            const QString &symbol,
                            const QVector<DomWidget::LocalOrderMarker> &markers);
    void lighterStopOrdersUpdated(const QString &accountName,
                                  const QString &symbol,
                                  bool hasSl,
                                  double slTriggerPrice,
                                  bool hasTp,
                                  double tpTriggerPrice);
    void privatePingUpdated(const QString &accountName, int ms);

private:
    struct BalanceState {
        double available = 0.0;
        double locked = 0.0;
        qint64 updatedMs = 0;
    };

    struct OrderRecord {
        QString symbol;
        OrderSide side = OrderSide::Buy;
        double price = 0.0;
        double quantityNotional = 0.0;
        qint64 createdMs = 0;
        QString orderId;
    };

    struct FuturesContractMeta {
        double contractSize = 1.0; // base units per contract
        int volScale = 0;          // decimals for vol
        int minVol = 1;            // minimum vol in contracts
        int minLeverage = 1;
        int maxLeverage = 200;
        double tickSize = 0.0; // price tick size (best-effort; some endpoints may omit)
        bool valid() const { return contractSize > 0.0 && minVol > 0 && volScale >= 0; }
    };

    struct PendingFuturesOrder {
        QString symbol;
        double price = 0.0;
        double quantityBase = 0.0; // base asset qty from UI
        OrderSide side = OrderSide::Buy;
        int leverage = 0;
    };

    struct SpotSymbolMeta {
        double minQty = 0.0;
        double stepSize = 0.0;
        double tickSize = 0.0;
        double minNotional = 0.0; // quote notional constraint (if provided by exchange)
        bool valid() const { return stepSize > 0.0 && tickSize > 0.0; }
    };

    struct PendingSpotOrder {
        QString symbol;
        double price = 0.0;
        double quantityBase = 0.0; // base asset qty from UI
        OrderSide side = OrderSide::Buy;
    };

    struct ParadexMarketMeta {
        double orderSizeIncrement = 0.0;
        double priceTickSize = 0.0;
        int sizeDecimals = 0;
        int priceDecimals = 0;
        bool valid() const { return orderSizeIncrement > 0.0 && priceTickSize > 0.0; }
    };

    struct PendingParadexOrder {
        QString symbol;
        double price = 0.0;
        double quantityBase = 0.0; // base asset qty from UI
        OrderSide side = OrderSide::Buy;
        int leverage = 0;
    };
    struct Context {
        enum class LighterStopKind : quint8 {
            Unknown = 0,
            StopLoss = 1,
            TakeProfit = 2,
        };
        struct LighterStopKindInfo {
            LighterStopKind kind{LighterStopKind::Unknown};
            bool explicitType{false};
            qint64 updatedMs{0};
        };

        ConnectionStore::Profile profile{ConnectionStore::Profile::MexcSpot};
        MexcCredentials credentials;
        QString accountName;
        ConnectionState state{ConnectionState::Disconnected};
        QNetworkAccessManager *mexcNetwork{nullptr};
        QNetworkAccessManager *lighterNetwork{nullptr};
        QNetworkAccessManager *uzxNetwork{nullptr};
        QNetworkAccessManager *paradexNetwork{nullptr};
        QWebSocket privateSocket;
        QWebSocket lighterStreamSocket;
        QString proxyStatusLog;
        QString proxyPreflightKey;
        bool proxyPreflightPassed{false};
        bool proxyPreflightInFlight{false};
        quint64 proxyPreflightToken{0};
        bool lighterStreamConnecting{false};
        bool lighterStreamConnected{false};
        bool lighterStreamReady{false};
        bool lighterPrivateSubscribed{false};
        qint64 lighterStreamLastErrorMs{0};
        QString lighterStreamLastError;
        QTimer keepAliveTimer;
        QTimer reconnectTimer;
        QTimer wsPingTimer;
        QTimer openOrdersTimer;
        QTimer uzxPositionsTimer;
        QTimer myTradesTimer;
        QTimer futuresDealsTimer;
        QTimer lighterAccountTimer;
        QTimer lighterTradesTimer;
        QTimer lighterBurstTimer;
        qint64 lastPrivatePingSentMs{0};
        int lastPrivatePingMs{0};
        int lighterBurstRemaining{0};
        QString listenKey;
        QString lighterAuthToken;
        QString paradexJwt;
        QString paradexAccount;
        qint64 paradexJwtExpiresAt{0};
        bool paradexWsAuthed{false};
        bool paradexAuthInFlight{false};
        quint64 paradexAuthToken{0};
        int paradexAuthMsgId{0};
        int paradexNextRpcId{1};
        bool closingSocket{false};
        bool hasSubscribed{false};
        bool openOrdersPending{false};
        bool lighterAccountPending{false};
        bool lighterTradesPending{false};
        bool lighterActiveOrdersPending{false};
        bool lighterTradesDisabled{false};
        int lighterAccountBackoffMs{0};
        int lighterTradesBackoffMs{0};
        int lighterActiveOrdersPollIndex{0};
        QSet<QString> trackedSymbols;
        QSet<QString> watchedSymbols;
        QHash<QString, TradePosition> positions;
        QHash<QString, qint64> positionUpdatedMsBySymbol;
        QHash<QString, qint64> paradexMissingEntryLogMsBySymbol;
        QHash<QString, OrderRecord> activeOrders;
        // Lighter: cached reduce-only trigger orders (SL/TP) per symbol, best-effort.
        QHash<QString, QStringList> lighterSlStopOrderIdsBySymbol;
        QHash<QString, QStringList> lighterTpStopOrderIdsBySymbol;
        // Lighter: stable stop kind per order id to avoid SL/TP label flapping when some fields are missing in WS/REST.
        QHash<QString, LighterStopKindInfo> lighterStopKindByOrderId;
        QSet<QString> pendingCancelSymbols;
        QSet<QString> paradexPendingCancelOrderIds;
        QSet<QString> myTradesInFlight;
        QHash<QString, qint64> lastTradeIdBySymbol;
        QHash<QString, qint64> paradexLastTradeTsBySymbol;
        QHash<QString, QString> paradexLastTradeIdBySymbol;
        QSet<QString> pendingParadexCloseSymbols;
        QHash<QString, qint64> paradexCloseInFlightMsBySymbol;
        QSet<QString> paradexSubscribedChannels;
        bool paradexTradesDisabled{false};
        QSet<QString> futuresDealsInFlight;
        QHash<QString, qint64> lastFuturesDealTsBySymbol;
        bool futuresPositionsPending{false};
        bool futuresLoggedIn{false};
        bool futuresAuthPending{false};
        bool futuresRestAuthed{false};
        quint64 futuresAuthToken{0};
        QHash<QString, FuturesContractMeta> futuresContractMeta; // symbol -> meta
        QSet<QString> futuresContractMetaInFlight;
        QHash<QString, QVector<PendingFuturesOrder>> pendingFuturesOrders; // symbol -> queued orders until meta fetched

        QHash<QString, SpotSymbolMeta> spotSymbolMeta; // symbol -> meta
        QSet<QString> spotSymbolMetaInFlight;
        QHash<QString, QVector<PendingSpotOrder>> pendingSpotOrders; // symbol -> queued orders until meta fetched

        QHash<QString, ParadexMarketMeta> paradexMarketMeta; // symbol -> meta
        QSet<QString> paradexMarketMetaInFlight;
        QHash<QString, QVector<PendingParadexOrder>> pendingParadexOrders; // symbol -> queued orders until meta fetched

        QHash<QString, BalanceState> balances; // asset -> available/locked
        QHash<QString, double> realizedPnl; // asset -> pnl sum (realized)
        QHash<QString, double> commissions; // asset -> fee sum
        QHash<QString, double> uzxLastFilledByOrderId;
        QHash<QString, int> uzxLeverageBySymbol;
        QHash<QString, QString> uzxSlOrderIdBySymbol;
        QHash<QString, QString> uzxTpOrderIdBySymbol;
        QHash<QString, double> uzxSlTriggerBySymbol;
        QHash<QString, double> uzxTpTriggerBySymbol;
        QHash<QString, qint64> uzxRecentOrderFingerprintsMs;
        QHash<QString, qint64> uzxTerminalOrderSeenMs;
        QHash<QString, qint64> uzxSpotCloseRetryMs;
        qint64 uzxSwapPositionsDisabledUntilMs{0};
        // UZX swap positions rehydration is REST-based and sometimes returns transient/partial data.
        // Track when a symbol was last seen as non-zero (and explicitly flat) to avoid UI flicker.
        QHash<QString, qint64> uzxSwapRestLastSeenMsBySymbol;
        QHash<QString, qint64> uzxSwapRestFlatSeenMsBySymbol;
        QHash<QString, qint64> uzxSwapRestMissingSeenMsBySymbol;

        // Lighter: best-effort local cache of per-market leverage we last set (market_id -> leverage x).
        QHash<int, int> lighterLeverageByMarketId;

        // MEXC futures: cached stop plan orders (SL/TP) per symbol (best-effort).
        QHash<QString, QString> mexcFuturesSlPlanOrderIdBySymbol;
        QHash<QString, QString> mexcFuturesTpPlanOrderIdBySymbol;
        QHash<QString, double> mexcFuturesSlTriggerBySymbol;
        QHash<QString, double> mexcFuturesTpTriggerBySymbol;

        // MEXC spot: cached trigger orders (SL/TP) per symbol (best-effort).
        QHash<QString, QString> mexcSpotSlOrderIdBySymbol;
        QHash<QString, QString> mexcSpotTpOrderIdBySymbol;
        QHash<QString, double> mexcSpotSlTriggerBySymbol;
        QHash<QString, double> mexcSpotTpTriggerBySymbol;

        // Lighter: cache next expected nonce to avoid an extra round-trip per order.
        // Only valid if this app is the only tx sender for the account.
        qint64 lighterNextNonce{0};
        bool lighterNonceInFlight{false};
        QVector<std::function<void(qint64 nonce, QString err)>> lighterNonceWaiters;
        // Lighter: prevent double-submit of SL/TP from UI and allow coalescing to the latest click.
        QSet<QString> lighterStopPlaceInFlightKeys;   // key = "SYM|SL" or "SYM|TP"
        QHash<QString, double> lighterStopQueuedTriggerByKey;
        // Lighter: avoid repeated cancel spam while cleanup is running.
        QSet<QString> lighterPendingCancelOrderIds;
        qint64 lighterLastTradeId{0};
        qint64 lighterLastClientOrderIndex{0};
        qint64 lighterLastAuthRefreshMs{0};
        struct LighterWsTxWaiter {
            qint64 startMs{0};
            bool sent{false};
            int txType{0};
            QString txInfo;
            std::function<void(QString txHash, QString err)> cb;
        };
        QHash<QString, LighterWsTxWaiter> lighterWsTxWaiters;

        // Paradex: keep a signer helper process running to avoid per-order spawn latency.
        struct ParadexSignWaiter {
            QString symbol;
            std::function<void(QString signature, qint64 signatureTsMs, QString err)> cb;
        };
        QProcess *paradexSignerProc{nullptr};
        QByteArray paradexSignerStdoutBuf;
        qint64 paradexSignerNextId{1};
        QHash<qint64, ParadexSignWaiter> paradexSignWaiters;
    };

    static qint64 nextLighterClientOrderIndex(Context &ctx);

    void verifyMexcFuturesRestAuth(Context &ctx);
    void finishMexcFuturesConnect(Context &ctx);
    void fetchMexcFuturesPlanOrders(Context &ctx, const QString &symbol = QString());
    void ensureParadexMarketMeta(Context &ctx, const QString &symbolUpper);
    void subscribeParadexPrivate(Context &ctx);
    void fetchParadexMyTrades(Context &ctx, const QString &symbolUpper);
    void placeMexcFuturesStopPlanOrder(Context &ctx,
                                       const QString &symbol,
                                       double triggerPrice,
                                       bool isStopLoss);
    void cancelMexcFuturesStopPlanOrder(Context &ctx,
                                        const QString &symbol,
                                        bool isStopLoss,
                                        std::function<void(bool ok)> after = {});
    void sendLighterTx(Context &ctx,
                       int txType,
                       const QString &txInfo,
                       std::function<void(QString txHash, QString err)> cb);
    QNetworkAccessManager *ensureMexcNetwork(Context &ctx);
    QNetworkAccessManager *ensureUzxNetwork(Context &ctx);
    QNetworkAccessManager *ensureParadexNetwork(Context &ctx);
    void ensureLighterStreamWired(Context &ctx);
    void ensureLighterStreamOpen(Context &ctx);
    void subscribeLighterPrivateWs(Context &ctx);
    void fetchUzxOpenOrders(Context &ctx);
    void fetchUzxSwapPositions(Context &ctx);

    Context &ensureContext(ConnectionStore::Profile profile) const;
    QString defaultAccountName(ConnectionStore::Profile profile) const;
    QString profileKey(ConnectionStore::Profile profile) const;
    QString accountNameFor(ConnectionStore::Profile profile) const;
    ConnectionStore::Profile profileFromAccountName(const QString &accountName) const;

    void setState(Context &ctx, ConnectionState state, const QString &message = QString());
    QByteArray signPayload(const QUrlQuery &query, const Context &ctx) const;
    QByteArray signUzxPayload(const QByteArray &body,
                              const QString &method,
                              const QString &path,
                              const QUrlQuery &query,
                              const Context &ctx) const;
    QNetworkRequest makePrivateRequest(const QString &path,
                                       const QUrlQuery &query,
                                       const QByteArray &contentType,
                                       const Context &ctx) const;
    QNetworkRequest makeUzxRequest(const QString &path,
                                   const QByteArray &body,
                                   const QString &method,
                                   const QUrlQuery &query,
                                   const Context &ctx) const;
    double handleOrderFill(Context &ctx,
                           const QString &symbol,
                           OrderSide side,
                           double price,
                           double quantity,
                           double *outClosedNotional = nullptr);
    void emitPositionChanged(Context &ctx, const QString &symbol);
    bool ensureCredentials(const Context &ctx) const;
    void requestListenKey(Context &ctx);
    void initializeWebSocket(Context &ctx, const QString &listenKey);
    void initializeFuturesWebSocket(Context &ctx);
    void initializeUzxWebSocket(Context &ctx);
    void initializeParadexWebSocket(Context &ctx);
    void closeWebSocket(Context &ctx);
    void subscribePrivateChannels(Context &ctx);
    void subscribeUzxPrivate(Context &ctx);
    void sendParadexWsAuth(Context &ctx);
    void sendListenKeyKeepAlive(Context &ctx);
    void resetConnection(Context &ctx, const QString &reason);
    void scheduleReconnect(Context &ctx);
    void fetchOpenOrders(Context &ctx);
    void fetchParadexOpenOrders(Context &ctx);
    void fetchParadexPositions(Context &ctx);
    bool ensureParadexSigner(Context &ctx, QString &errOut);
    void requestParadexOrderSignature(Context &ctx,
                                      const QString &symbol,
                                      const QString &market,
                                      const QString &side,
                                      const QString &orderType,
                                      const QString &size,
                                      const QString &price,
                                      std::function<void(QString signature, qint64 signatureTsMs, QString err)> cb);
    void fetchFuturesOpenOrders(Context &ctx);
    void fetchFuturesPositions(Context &ctx);
    void fetchLighterAccount(Context &ctx);
    void fetchLighterTrades(Context &ctx);
    void fetchLighterActiveOrders(Context &ctx, const QString &symbolUpper);
    void processPrivateDeal(Context &ctx, const QByteArray &body, const QString &symbol);
    void processPrivateOrder(Context &ctx, const QByteArray &body, const QString &symbol);
    void processPrivateAccount(Context &ctx, const QByteArray &body);
    void recordFinrezTrade(Context &ctx, const ExecutedTrade &trade);
    void emitLocalOrderSnapshot(Context &ctx, const QString &symbol);
    void clearLocalOrderSnapshots(Context &ctx);
    void clearSymbolActiveOrders(Context &ctx, const QString &symbol);
    QNetworkRequest makeFuturesRequest(const QString &path,
                                       const Context &ctx,
                                       const QByteArray &body = QByteArray(),
                                       bool signBody = false) const;
    bool buildFuturesSignature(const QByteArray &body,
                               const Context &ctx,
                               QByteArray &nonceOut,
                               QByteArray &signOut) const;
    void ensureFuturesContractMeta(Context &ctx, const QString &symbolUpper);
    void submitMexcFuturesOrder(Context &ctx,
                                const PendingFuturesOrder &order,
                                const FuturesContractMeta &meta);
    void ensureMexcSpotSymbolMeta(Context &ctx, const QString &symbolUpper);
    void submitMexcSpotOrder(Context &ctx,
                             const PendingSpotOrder &order,
                             const SpotSymbolMeta &meta);
    void placeMexcFuturesOrder(Context &ctx,
                               const QString &symbol,
                               double price,
                               double quantity,
                               OrderSide side,
                               int leverage);
    void cancelAllMexcFuturesOrders(Context &ctx, const QString &symbol);
    QString tradeHistoryPath() const;
    void loadTradeHistory();
    void appendTradeHistory(const ExecutedTrade &trade);
    void pollMyTrades(Context &ctx);
    void fetchMyTrades(Context &ctx, const QString &symbolUpper);
    void pollFuturesDeals(Context &ctx);
    void fetchFuturesDeals(Context &ctx, const QString &symbolUpper);
    void pollLighterAccount(Context &ctx);
    void pollLighterTrades(Context &ctx);
    void pollLighterActiveOrders(Context &ctx);
    QNetworkAccessManager *ensureLighterNetwork(Context &ctx);
    void applyExecutedTradeMeta(Context &ctx, const QString &symbol, ExecutedTrade &trade);
    void ensureLighterMetaLoaded(Context &ctx, const QString &baseUrl, std::function<void(QString err)> cb);
    void armLighterBurst(Context &ctx, int ticks);

    Context *contextForProfile(ConnectionStore::Profile profile) const;

    QNetworkAccessManager m_network;
    mutable QHash<ConnectionStore::Profile, Context *> m_contexts;
    QVector<ExecutedTrade> m_executedTrades;
    const QString m_baseUrl = QStringLiteral("https://api.mexc.com");
    const QString m_uzxBaseUrl = QStringLiteral("https://api-v2.uzx.com");
    const QString m_futuresBaseUrl = QStringLiteral("https://futures.mexc.com/api/v1");
};
