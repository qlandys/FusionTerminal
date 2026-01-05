#pragma once

#include <QString>
#include <QMetaType>

enum class OrderSide {
    Buy,
    Sell
};

struct MexcCredentials {
    QString apiKey;
    QString secretKey;
    QString passphrase;
    QString uid;
    QString proxy;
    QString proxyType; // "http" | "https" | "socks5"
    QString baseUrl;
    int accountIndex = 0;
    int apiKeyIndex = -1;
    QString seedPhrase; // Lighter: used only by the setup dialog; not persisted by default
    int seedAddressIndex = 0; // Lighter: m/44'/60'/0'/0/{index}
    bool preferSeedPhrase = false; // Lighter: UI preference
    QString colorHex;
    QString label;
    bool saveSecret = false;
    bool viewOnly = false;
    bool autoConnect = true;
};

struct TradePosition {
    bool hasPosition = false;
    OrderSide side = OrderSide::Buy;
    double averagePrice = 0.0;
    double quantity = 0.0; // spot: base units; futures: contracts (vol)
    double qtyMultiplier = 1.0; // futures: contractSize (base per contract), spot: 1.0
    qint64 positionId = 0; // futures positionId (recommended for close); spot: 0
    int openType = 1; // futures: 1 isolated, 2 cross
    int leverage = 0; // futures leverage (isolated may require), 0 = unknown
    double realizedPnl = 0.0;
};

struct ExecutedTrade {
    QString accountName;
    QString symbol;
    OrderSide side = OrderSide::Buy;
    double price = 0.0;
    double quantity = 0.0;
    QString feeCurrency;
    double feeAmount = 0.0;
    double realizedPnl = 0.0; // per-fill realized PnL delta in quote currency (USDT)
    double realizedPct = 0.0; // per-fill realized PnL % vs entry notional for closed part
    qint64 timeMs = 0;
};

Q_DECLARE_METATYPE(MexcCredentials)
Q_DECLARE_METATYPE(TradePosition)
Q_DECLARE_METATYPE(ExecutedTrade)
