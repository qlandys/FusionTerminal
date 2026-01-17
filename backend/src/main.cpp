#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <winhttp.h>
#else
#    error "This backend is implemented for Windows (WinHTTP) only."
#endif

#if defined(ORDERBOOK_BACKEND_QT)
#    include <QCoreApplication>
#    include <QEventLoop>
#    include <QNetworkAccessManager>
#    include <QNetworkProxy>
#    include <QNetworkReply>
#    include <QTimer>
#    include <QUrl>
#    include <QWebSocket>
#endif

#include "OrderBook.hpp"

#include <chrono>
#include <cmath>
#include <charconv>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cctype>

#include <json.hpp>

namespace
{
    using namespace std::chrono_literals;
    using json = nlohmann::json;

    class StdoutBatchWriter
    {
    public:
        StdoutBatchWriter()
        {
            const char* ms = std::getenv("BACKEND_STDOUT_FLUSH_MS");
            if (ms && *ms)
            {
                try
                {
                    const int v = std::max(0, std::stoi(ms));
                    flushInterval = std::chrono::milliseconds(std::clamp(v, 0, 250));
                }
                catch (...)
                {
                }
            }
            const char* bytes = std::getenv("BACKEND_STDOUT_FLUSH_BYTES");
            if (bytes && *bytes)
            {
                try
                {
                    const int v = std::max(0, std::stoi(bytes));
                    flushBytes = static_cast<std::size_t>(std::clamp(v, 1024, 512 * 1024));
                }
                catch (...)
                {
                }
            }
        }

        void writeLine(const std::string& line)
        {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mu);
            buf.append(line);
            buf.push_back('\n');
            if (flushInterval.count() == 0 || buf.size() >= flushBytes || now - lastFlush >= flushInterval)
            {
                flushLocked();
                lastFlush = now;
            }
        }

        void flush()
        {
            std::lock_guard<std::mutex> lock(mu);
            flushLocked();
            lastFlush = std::chrono::steady_clock::now();
        }

    private:
        void flushLocked()
        {
            if (buf.empty())
            {
                return;
            }
            std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::cout.flush();
            buf.clear();
        }

        std::mutex mu;
        std::string buf;
        std::chrono::milliseconds flushInterval{8};
        std::size_t flushBytes{16 * 1024};
        std::chrono::steady_clock::time_point lastFlush{std::chrono::steady_clock::now()};
    };

    static StdoutBatchWriter& stdoutWriter()
    {
        static StdoutBatchWriter w;
        return w;
    }

    class TradeBatcher
    {
    public:
        TradeBatcher()
        {
            enabled = !std::getenv("BACKEND_TRADE_BATCH_DISABLE");
            const char* ms = std::getenv("BACKEND_TRADE_BATCH_MS");
            if (ms && *ms)
            {
                try
                {
                    const int v = std::max(0, std::stoi(ms));
                    flushInterval = std::chrono::milliseconds(std::clamp(v, 0, 250));
                }
                catch (...)
                {
                }
            }
            const char* max = std::getenv("BACKEND_TRADE_BATCH_MAX");
            if (max && *max)
            {
                try
                {
                    const int v = std::max(1, std::stoi(max));
                    flushMax = static_cast<std::size_t>(std::clamp(v, 1, 2048));
                }
                catch (...)
                {
                }
            }
        }

        void add(const std::string& symbol, json&& trade)
        {
            if (!enabled)
            {
                // Fallback: emit one-per-line.
                stdoutWriter().writeLine(trade.dump());
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mu);
            if (this->symbol.empty())
            {
                this->symbol = symbol;
            }
            // Keep payload compact: symbol on top-level.
            trade.erase("symbol");
            batch.push_back(std::move(trade));
            if (flushInterval.count() == 0 || batch.size() >= flushMax || now - lastFlush >= flushInterval)
            {
                flushLocked();
                lastFlush = now;
            }
        }

        void flush()
        {
            std::lock_guard<std::mutex> lock(mu);
            flushLocked();
            lastFlush = std::chrono::steady_clock::now();
        }

    private:
        void flushLocked()
        {
            if (batch.empty())
            {
                return;
            }
            json out;
            out["type"] = "trades";
            out["symbol"] = symbol;
            out["events"] = std::move(batch);
            batch = json::array();
            stdoutWriter().writeLine(out.dump());
        }

        std::mutex mu;
        bool enabled{true};
        std::string symbol;
        json batch = json::array();
        std::chrono::milliseconds flushInterval{16};
        std::size_t flushMax{64};
        std::chrono::steady_clock::time_point lastFlush{std::chrono::steady_clock::now()};
    };

    static TradeBatcher& tradeBatcher()
    {
        static TradeBatcher b;
        return b;
    }

    struct Config
    {
        std::string symbol{"BIOUSDT"};
        std::string endpoint{"wss://wbs-api.mexc.com/ws"};
        std::string exchange{"mexc"};
        std::string proxyType{"http"}; // http | socks5
        std::string proxy;             // host:port[:user:pass] / user:pass@host:port / etc
        bool forceNoProxy{false};      // ignore system proxy and use direct connections
        std::size_t ladderLevelsPerSide{120};
        std::chrono::milliseconds throttle{50};
        std::size_t snapshotDepth{500};
        std::size_t cacheLevelsPerSide{5000};
        double futuresContractSize{1.0}; // MEXC futures qty is in contracts; multiply by this to get base qty
        int mexcStreamIntervalMs{100};  // MEXC spot protobuf WS interval (ms)
        int mexcSpotPollMs{250};        // MEXC spot REST polling interval (fallback)
        std::string mexcSpotMode{"pbws"}; // pbws | rest

        std::wstring winProxy; // WinHTTP proxy string; empty means no proxy
        std::wstring proxyUser;
        std::wstring proxyPass;
    };

    std::wstring toWide(const std::string& s);

    std::string trimAscii(std::string s)
    {
        auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
        {
            s.erase(s.begin());
        }
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
        {
            s.pop_back();
        }
        return s;
    }

    std::string toLowerAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }

    bool parseProxyString(const std::string &rawInput,
                          std::string typeHint,
                          std::string &outType,
                          std::string &outHost,
                          int &outPort,
                          std::string &outUser,
                          std::string &outPass,
                          std::string &outError)
    {
        outType.clear();
        outHost.clear();
        outPort = 0;
        outUser.clear();
        outPass.clear();
        outError.clear();

        std::string raw = trimAscii(rawInput);
        if (raw.empty())
        {
            outError = "empty proxy";
            return false;
        }

        std::string lower = toLowerAscii(raw);
        auto stripScheme = [&](const std::string &scheme) {
            if (lower.rfind(scheme, 0) == 0)
            {
                raw = raw.substr(scheme.size());
                lower = lower.substr(scheme.size());
                return true;
            }
            return false;
        };

        if (stripScheme("socks5://") || stripScheme("socks://"))
        {
            typeHint = "socks5";
        }
        else if (stripScheme("http://") || stripScheme("https://"))
        {
            typeHint = "http";
        }

        outType = toLowerAscii(trimAscii(typeHint));
        if (outType.empty())
        {
            outType = "http";
        }
        if (outType == "https")
        {
            outType = "http";
        }
        if (outType != "http" && outType != "socks5")
        {
            outError = "unsupported proxy type: " + outType;
            return false;
        }

        // Accept:
        // - host:port
        // - user:pass@host:port
        // - host:port@user:pass
        // - host:port:user:pass
        // - user:pass:host:port
        const auto isAllDigits = [](const std::string &s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            });
        };

        auto split = [](const std::string &s, char sep) {
            std::vector<std::string> parts;
            std::string cur;
            for (char c : s)
            {
                if (c == sep)
                {
                    parts.push_back(cur);
                    cur.clear();
                }
                else
                {
                    cur.push_back(c);
                }
            }
            parts.push_back(cur);
            return parts;
        };

        auto parseHostPort = [&](const std::string &hp, std::string &host, int &port) -> bool {
            const auto parts = split(hp, ':');
            if (parts.size() != 2 || !isAllDigits(parts[1]))
            {
                return false;
            }
            host = parts[0];
            port = std::stoi(parts[1]);
            return !host.empty() && port > 0 && port <= 65535;
        };

        std::string core = raw;
        std::string left;
        std::string right;
        const std::size_t at = core.find('@');
        if (at != std::string::npos)
        {
            left = core.substr(0, at);
            right = core.substr(at + 1);

            std::string host;
            int port = 0;
            if (parseHostPort(right, host, port))
            {
                outHost = host;
                outPort = port;
                const auto cred = split(left, ':');
                if (cred.size() >= 2)
                {
                    outUser = cred[0];
                    outPass = cred[1];
                }
                return true;
            }
            if (parseHostPort(left, host, port))
            {
                outHost = host;
                outPort = port;
                const auto cred = split(right, ':');
                if (cred.size() >= 2)
                {
                    outUser = cred[0];
                    outPass = cred[1];
                }
                return true;
            }
        }

        const auto parts = split(core, ':');
        if (parts.size() == 2 && isAllDigits(parts[1]))
        {
            outHost = parts[0];
            outPort = std::stoi(parts[1]);
            return !outHost.empty() && outPort > 0 && outPort <= 65535;
        }
        if (parts.size() == 4)
        {
            // host:port:user:pass
            if (isAllDigits(parts[1]))
            {
                outHost = parts[0];
                outPort = std::stoi(parts[1]);
                outUser = parts[2];
                outPass = parts[3];
                return !outHost.empty() && outPort > 0 && outPort <= 65535;
            }
            // user:pass:host:port
            if (isAllDigits(parts[3]))
            {
                outUser = parts[0];
                outPass = parts[1];
                outHost = parts[2];
                outPort = std::stoi(parts[3]);
                return !outHost.empty() && outPort > 0 && outPort <= 65535;
            }
        }

        outError = "invalid proxy format";
        return false;
    }

    void finalizeProxy(Config &cfg)
    {
        cfg.winProxy.clear();
        cfg.proxyUser.clear();
        cfg.proxyPass.clear();

        const std::string raw = trimAscii(cfg.proxy);
        if (raw.empty())
        {
            return;
        }

        std::string type;
        std::string host;
        int port = 0;
        std::string user;
        std::string pass;
        std::string err;
        if (!parseProxyString(raw, cfg.proxyType, type, host, port, user, pass, err))
        {
            throw std::runtime_error("Invalid proxy: " + err);
        }

        cfg.proxyType = type;
        std::ostringstream hp;
        hp << host << ":" << port;

        std::string winProxy;
        // WinHTTP does not reliably support SOCKS proxies for WebSockets.
        // Most "SOCKS5" proxy providers also expose an HTTP CONNECT proxy on the same host:port.
        // Use HTTP/HTTPS named proxy for WinHTTP even when the UI proxy type is SOCKS5.
        winProxy = "http=" + hp.str() + ";https=" + hp.str();

        cfg.winProxy = toWide(winProxy);
        if (!user.empty())
        {
            cfg.proxyUser = toWide(user);
            cfg.proxyPass = toWide(pass);
        }
    }

    Config parseArgs(int argc, char** argv)
    {
        Config cfg;
        bool snapshotDepthSet = false;

        for (int i = 1; i < argc; ++i)
        {
            std::string arg = argv[i];
            auto value = [&](std::string_view name) {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("Missing value for " + std::string(name));
                }
                return std::string(argv[++i]);
            };

            if (arg == "--symbol")
            {
                cfg.symbol = value("--symbol");
            }
            else if (arg == "--endpoint")
            {
                cfg.endpoint = value("--endpoint");
            }
            else if (arg == "--exchange")
            {
                cfg.exchange = value("--exchange");
            }
            else if (arg == "--proxy")
            {
                cfg.proxy = value("--proxy");
            }
            else if (arg == "--proxy-type")
            {
                cfg.proxyType = value("--proxy-type");
                const std::string t = toLowerAscii(cfg.proxyType);
                if (t == "none" || t == "direct" || t == "noproxy" || t == "no_proxy")
                {
                    cfg.forceNoProxy = true;
                    cfg.proxyType = "http";
                }
            }
            else if (arg == "--ladder-levels")
            {
                cfg.ladderLevelsPerSide = std::stoul(value("--ladder-levels"));
            }
            else if (arg == "--throttle-ms")
            {
                cfg.throttle = std::chrono::milliseconds(std::stoul(value("--throttle-ms")));
            }
            else if (arg == "--snapshot-depth")
            {
                cfg.snapshotDepth = std::stoul(value("--snapshot-depth"));
                snapshotDepthSet = true;
            }
            else if (arg == "--mexc-interval-ms")
            {
                cfg.mexcStreamIntervalMs = std::stoi(value("--mexc-interval-ms"));
            }
            else if (arg == "--mexc-spot-poll-ms")
            {
                cfg.mexcSpotPollMs = std::stoi(value("--mexc-spot-poll-ms"));
            }
            else if (arg == "--mexc-spot-mode")
            {
                cfg.mexcSpotMode = toLowerAscii(value("--mexc-spot-mode"));
            }
            else if (arg == "--no-proxy")
            {
                cfg.forceNoProxy = true;
            }
            else if (arg == "--cache-levels")
            {
                cfg.cacheLevelsPerSide = std::stoul(value("--cache-levels"));
            }
        }

        constexpr std::size_t kMinCacheLevels = 5000;
        const std::size_t kMaxSnapshotDepth =
            (cfg.exchange == "binance" || cfg.exchange == "binance_futures") ? 1000 : 5000;

        cfg.cacheLevelsPerSide = std::max(cfg.cacheLevelsPerSide, kMinCacheLevels);

        if (!snapshotDepthSet || cfg.snapshotDepth == 0)
        {
            if (cfg.exchange == "binance" || cfg.exchange == "binance_futures")
            {
                cfg.snapshotDepth = 1000;
            }
            else if (cfg.exchange == "mexc" || cfg.exchange == "mexc_futures")
            {
                cfg.snapshotDepth = 200;
            }
            else
            {
                cfg.snapshotDepth = 500;
            }
        }
        // Ensure the initial REST snapshot is "good enough" to show a usable ladder quickly.
        // Note: ladderLevelsPerSide can be very large (user can zoom out), but fetching an equally
        // huge REST snapshot is expensive and often unnecessary — the WS stream will fill depth
        // progressively. Cap the minimum snapshot to keep startup responsive.
        const std::size_t minSnapshot =
            std::min(std::max<std::size_t>(std::min<std::size_t>(cfg.ladderLevelsPerSide * 2, 1000), 50),
                     kMaxSnapshotDepth);
        if (cfg.snapshotDepth < minSnapshot)
        {
            cfg.snapshotDepth = minSnapshot;
        }
        if (cfg.snapshotDepth > kMaxSnapshotDepth)
        {
            cfg.snapshotDepth = kMaxSnapshotDepth;
        }

        if (cfg.forceNoProxy)
        {
            cfg.proxy.clear();
            cfg.winProxy.clear();
            cfg.proxyUser.clear();
            cfg.proxyPass.clear();
        }
        else
        {
            finalizeProxy(cfg);
        }

        // Normalize MEXC spot stream interval to supported values.
        // Note: `spot@public.aggre.*.v3.api.pb` is allowed only at 100ms; other intervals are blocked.
        if (cfg.exchange == "mexc")
        {
            // MEXC spot JSON WS supports a small set of fixed intervals; keep 100ms by default.
            cfg.mexcStreamIntervalMs = 100;
            if (cfg.mexcSpotPollMs < 50) cfg.mexcSpotPollMs = 50;
            if (cfg.mexcSpotPollMs > 2000) cfg.mexcSpotPollMs = 2000;
            if (cfg.mexcSpotMode != "pbws" && cfg.mexcSpotMode != "rest")
            {
                cfg.mexcSpotMode = "pbws";
            }
        }
        return cfg;
    }

    // Forward declarations for MEXC spot polling helpers (defined later in this file).
    std::optional<std::string> httpGet(const Config& cfg,
                                       const std::string& host,
                                       const std::string& path,
                                       bool secure);
    double jsonToDouble(const json& value);
    dom::OrderBook::Tick tickFromPrice(double price, double tickSize);
    void emitLadder(const Config& config,
                    const dom::OrderBook& book,
                    double bestBid,
                    double bestAsk,
                    std::int64_t ts);
    extern std::mutex g_bookMutex;

    bool fetchMexcSpotDepthSnapshot(const Config& cfg,
                                    double tickSize,
                                    std::vector<std::pair<dom::OrderBook::Tick, double>>& bids,
                                    std::vector<std::pair<dom::OrderBook::Tick, double>>& asks)
    {
        if (tickSize <= 0.0)
        {
            return false;
        }
        std::ostringstream path;
        path << "/api/v3/depth?symbol=" << cfg.symbol << "&limit=" << cfg.snapshotDepth;
        auto body = httpGet(cfg, "api.mexc.com", path.str(), true);
        if (!body)
        {
            return false;
        }
        json j;
        try
        {
            j = json::parse(*body);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[backend] depth JSON parse error: " << ex.what() << std::endl;
            return false;
        }
        if (!j.contains("bids") || !j.contains("asks"))
        {
            return false;
        }

        auto parseSide = [tickSize](const json& arr,
                                    std::vector<std::pair<dom::OrderBook::Tick, double>>& out) {
            out.clear();
            if (!arr.is_array()) return;
            for (const auto& e : arr)
            {
                if (!e.is_array() || e.size() < 2) continue;
                double price = 0.0;
                double qty = 0.0;
                try
                {
                    if (e[0].is_string()) price = std::stod(e[0].get<std::string>());
                    else price = jsonToDouble(e[0]);
                    if (e[1].is_string()) qty = std::stod(e[1].get<std::string>());
                    else qty = jsonToDouble(e[1]);
                }
                catch (...)
                {
                    continue;
                }
                if (!(price > 0.0) || !(qty >= 0.0)) continue;
                const auto tick = tickFromPrice(price, tickSize);
                out.emplace_back(tick, qty);
            }
        };

        parseSide(j["bids"], bids);
        parseSide(j["asks"], asks);
        return true;
    }

    [[noreturn]] void runMexcSpotPolling(const Config& config, dom::OrderBook& book)
    {
        const double tickSize = book.tickSize();
        if (!(tickSize > 0.0))
        {
            throw std::runtime_error("mexc poll: tickSize missing");
        }

        std::cerr << "[backend] starting MEXC spot REST polling for " << config.symbol
                  << " every " << config.mexcSpotPollMs << "ms" << std::endl;

        auto lastEmit = std::chrono::steady_clock::now();
        std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
        std::vector<std::pair<dom::OrderBook::Tick, double>> asks;

        for (;;)
        {
            if (fetchMexcSpotDepthSnapshot(config, tickSize, bids, asks))
            {
                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(g_bookMutex);
                book.loadSnapshot(bids, asks);
                if (now - lastEmit >= config.throttle)
                {
                    lastEmit = now;
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                    emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(config.mexcSpotPollMs));
        }
    }

    bool isMexcFutures(const Config &cfg)
    {
        return cfg.exchange == "mexc_futures";
    }

    bool isBinanceSpot(const Config &cfg)
    {
        return cfg.exchange == "binance";
    }

    bool isBinanceFutures(const Config &cfg)
    {
        return cfg.exchange == "binance_futures";
    }

    std::string normalizeBinanceSymbol(std::string sym)
    {
        sym.erase(std::remove(sym.begin(), sym.end(), '_'), sym.end());
        sym.erase(std::remove(sym.begin(), sym.end(), '-'), sym.end());
        std::transform(sym.begin(), sym.end(), sym.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return sym;
    }

    struct BinanceDepthSnapshot
    {
        std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
        std::vector<std::pair<dom::OrderBook::Tick, double>> asks;
        long long lastUpdateId = 0;
    };

    double jsonToDouble(const json &value)
    {
        if (value.is_number_float())
        {
            return value.get<double>();
        }
        if (value.is_number_integer())
        {
            return static_cast<double>(value.get<std::int64_t>());
        }
        if (value.is_string())
        {
            try
            {
                return std::stod(value.get<std::string>());
            }
            catch (...)
            {
                return 0.0;
            }
        }
        return 0.0;
    }

    bool quantizeTickFromPrice(double price,
                               double tickSize,
                               dom::OrderBook::Tick &outTick,
                               double &outSnappedPrice)
    {
        if (!(tickSize > 0.0) || !std::isfinite(tickSize) || !(price > 0.0) || !std::isfinite(price))
        {
            return false;
        }

        auto pow10i = [](int exp) -> std::int64_t {
            std::int64_t v = 1;
            for (int i = 0; i < exp; ++i) v *= 10;
            return v;
        };

        for (int decimals = 0; decimals <= 12; ++decimals)
        {
            const std::int64_t scale = pow10i(decimals);
            const double scaledTickSizeD = tickSize * static_cast<double>(scale);
            if (!std::isfinite(scaledTickSizeD))
            {
                continue;
            }
            const std::int64_t tickSizeScaled = static_cast<std::int64_t>(std::llround(scaledTickSizeD));
            if (tickSizeScaled <= 0)
            {
                continue;
            }
            if (std::abs(scaledTickSizeD - static_cast<double>(tickSizeScaled)) > 1e-9)
            {
                continue;
            }

            const std::int64_t priceScaled = static_cast<std::int64_t>(std::llround(price * static_cast<double>(scale)));
            std::int64_t tick = 0;
            if (priceScaled >= 0)
            {
                tick = (priceScaled + tickSizeScaled / 2) / tickSizeScaled;
            }
            else
            {
                tick = -((-priceScaled + tickSizeScaled / 2) / tickSizeScaled);
            }
            outTick = static_cast<dom::OrderBook::Tick>(tick);
            const std::int64_t snappedScaled = tick * tickSizeScaled;
            outSnappedPrice = static_cast<double>(snappedScaled) / static_cast<double>(scale);
            return std::isfinite(outSnappedPrice);
        }

        return false;
    }

    dom::OrderBook::Tick tickFromPrice(double price, double tickSize)
    {
        dom::OrderBook::Tick tick = 0;
        double snappedPrice = price;
        if (quantizeTickFromPrice(price, tickSize, tick, snappedPrice))
        {
            return tick;
        }
        return static_cast<dom::OrderBook::Tick>(std::llround(price / tickSize));
    }

    std::string winhttpError(const char* where)
    {
        DWORD error = GetLastError();
        LPWSTR buffer = nullptr;
        DWORD len =
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr,
                           error,
                           0,
                           reinterpret_cast<LPWSTR>(&buffer),
                           0,
                           nullptr);
        std::string msg = where;
        msg += ": ";
        if (len && buffer)
        {
            int outLen = WideCharToMultiByte(CP_UTF8, 0, buffer, len, nullptr, 0, nullptr, nullptr);
            std::string utf8(outLen, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, len, utf8.data(), outLen, nullptr, nullptr);
            msg += utf8;
            LocalFree(buffer);
        }
        else
        {
            msg += "unknown error";
        }
        return msg;
    }

    struct WinHttpHandle
    {
        WinHttpHandle() = default;
        explicit WinHttpHandle(HINTERNET h) : handle(h) {}
        ~WinHttpHandle() { reset(); }

        WinHttpHandle(const WinHttpHandle&) = delete;
        WinHttpHandle& operator=(const WinHttpHandle&) = delete;

        WinHttpHandle(WinHttpHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
        WinHttpHandle& operator=(WinHttpHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                handle = other.handle;
                other.handle = nullptr;
            }
            return *this;
        }

        void reset(HINTERNET h = nullptr)
        {
            if (handle)
            {
                WinHttpCloseHandle(handle);
            }
            handle = h;
        }

        [[nodiscard]] bool valid() const { return handle != nullptr; }
        [[nodiscard]] HINTERNET get() const { return handle; }

    private:
        HINTERNET handle{nullptr};
    };

    std::wstring toWide(const std::string& s)
    {
        if (s.empty())
        {
            return {};
        }
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring out(len - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
        return out;
    }

    WinHttpHandle openSession(const Config &cfg)
    {
        WinHttpHandle session;
        if (cfg.forceNoProxy)
        {
            session.reset(WinHttpOpen(L"Ghost/1.0",
                                      WINHTTP_ACCESS_TYPE_NO_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS,
                                      0));
        }
        else if (!cfg.winProxy.empty())
        {
            session.reset(WinHttpOpen(L"Ghost/1.0",
                                      WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                                      cfg.winProxy.c_str(),
                                      WINHTTP_NO_PROXY_BYPASS,
                                      0));
        }
        else
        {
            session.reset(WinHttpOpen(L"Ghost/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      nullptr,
                                      nullptr,
                                      0));
        }
        if (session.valid())
        {
            // Improve perceived startup: don't hang for tens of seconds on DNS/connect.
            // Keep conservative values to avoid spurious failures on slow networks.
            static constexpr int kTimeoutMs = 7000;
            WinHttpSetTimeouts(session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
        }
        return session;
    }

    void applyProxyCredentials(const Config &cfg, HINTERNET request)
    {
        if (!request || cfg.proxyUser.empty())
        {
            return;
        }
        // Best-effort: many proxies accept Basic auth.
        WinHttpSetCredentials(request,
                              WINHTTP_AUTH_TARGET_PROXY,
                              WINHTTP_AUTH_SCHEME_BASIC,
                              cfg.proxyUser.c_str(),
                              cfg.proxyPass.c_str(),
                              nullptr);
    }

    std::optional<std::string> httpGet(const Config &cfg,
                                       const std::string& host,
                                       const std::string& pathAndQuery,
                                       bool secure)
    {
        WinHttpHandle session = openSession(cfg);
        if (!session.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
            return std::nullopt;
        }

        const INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        WinHttpHandle connection(
            WinHttpConnect(session.get(), toWide(host).c_str(), port, 0));
        if (!connection.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
            return std::nullopt;
        }

        WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                                 L"GET",
                                                 toWide(pathAndQuery).c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 secure ? WINHTTP_FLAG_SECURE : 0));
        if (!request.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
            return std::nullopt;
        }

        applyProxyCredentials(cfg, request.get());
        if (!WinHttpSendRequest(request.get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
            return std::nullopt;
        }

        if (!WinHttpReceiveResponse(request.get(), nullptr))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
            return std::nullopt;
        }

        std::string buffer;
        for (;;)
        {
            DWORD bytesAvailable = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &bytesAvailable))
            {
                std::cerr << "[backend] " << winhttpError("WinHttpQueryDataAvailable") << std::endl;
                return std::nullopt;
            }
            if (bytesAvailable == 0)
            {
                break;
            }

            std::string chunk(bytesAvailable, '\0');
            DWORD bytesRead = 0;
            if (!WinHttpReadData(request.get(), chunk.data(), bytesAvailable, &bytesRead))
            {
                std::cerr << "[backend] " << winhttpError("WinHttpReadData") << std::endl;
                return std::nullopt;
            }
            chunk.resize(bytesRead);
            buffer += chunk;
        }

        return buffer;
    }

    extern std::mutex g_bookMutex;
    void emitLadder(const Config& config,
                    const dom::OrderBook& book,
                    double bestBid,
                    double bestAsk,
                    std::int64_t ts);

    bool parseIntStrict(std::string_view s, int &out)
    {
        if (s.empty())
        {
            return false;
        }
        int v = 0;
        const auto *first = s.data();
        const auto *last = s.data() + s.size();
        auto res = std::from_chars(first, last, v);
        if (res.ec != std::errc() || res.ptr != last)
        {
            return false;
        }
        out = v;
        return true;
    }

    std::string upperAscii(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return s;
    }

    std::string alnumUpperKey(std::string s)
    {
        s = upperAscii(std::move(s));
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    }

    std::string stripQuoteSuffix(std::string key)
    {
        static const std::vector<std::string> suffixes{
            "USDT",
            "USDC",
            "USDQ",
            "USDR",
            "USD",
            "EURQ",
            "EURR",
        };
        for (const auto &suf : suffixes)
        {
            if (key.size() > suf.size() && key.ends_with(suf))
            {
                key.resize(key.size() - suf.size());
                break;
            }
        }
        return key;
    }

#if defined(ORDERBOOK_BACKEND_QT)
    bool shouldUseQtSocks5(const Config &cfg)
    {
        // Use Qt stack for any proxy type (HTTP/SOCKS5). WinHTTP proxy+WebSocket behavior is
        // inconsistent across providers, especially with authentication.
        return !trimAscii(cfg.proxy).empty();
    }

    QNetworkProxy toQtProxy(const Config &cfg, bool &outOk)
    {
        outOk = false;
        const std::string raw = trimAscii(cfg.proxy);
        if (raw.empty())
        {
            outOk = true;
            return QNetworkProxy(QNetworkProxy::NoProxy);
        }

        std::string type;
        std::string host;
        int port = 0;
        std::string user;
        std::string pass;
        std::string err;
        if (!parseProxyString(raw, cfg.proxyType, type, host, port, user, pass, err))
        {
            std::cerr << "[backend] Invalid proxy: " << err << std::endl;
            return QNetworkProxy(QNetworkProxy::NoProxy);
        }

        QNetworkProxy proxy((type == "socks5") ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                            QString::fromStdString(host),
                            port,
                            QString::fromStdString(user),
                            QString::fromStdString(pass));
        outOk = true;
        return proxy;
    }

    std::optional<std::string> httpGetQt(const Config &cfg,
                                         const char *host,
                                         const std::string &path,
                                         bool secure,
                                         int timeoutMs = 15000)
    {
        bool ok = false;
        const QNetworkProxy proxy = toQtProxy(cfg, ok);
        if (!ok)
        {
            return std::nullopt;
        }

        QNetworkAccessManager nam;
        nam.setProxy(proxy);

        const QString fullUrl = QStringLiteral("%1://%2%3")
                                    .arg(secure ? QStringLiteral("https") : QStringLiteral("http"),
                                         QString::fromLatin1(host),
                                         QString::fromStdString(path));
        QUrl url(fullUrl);
        if (!url.isValid())
        {
            std::cerr << "[backend] httpGetQt invalid url: " << fullUrl.toStdString() << std::endl;
            return std::nullopt;
        }

        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Ghost/1.0"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { loop.quit(); });

        QNetworkReply *reply = nam.get(req);
        QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() { loop.quit(); });
        timer.start(timeoutMs);
        loop.exec();

        if (!timer.isActive())
        {
            reply->abort();
        }

        const auto err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        if (err != QNetworkReply::NoError || status <= 0 || status >= 400)
        {
            std::cerr << "[backend] httpGetQt failed: host=" << host << " status=" << status
                      << " err=" << static_cast<int>(err) << std::endl;
            return std::nullopt;
        }
        return body.toStdString();
    }
#endif

    bool fetchLighterMarketInfo(const Config &cfg, int &marketIdOut, double &tickSizeOut)
    {
        constexpr const char *kHost = "mainnet.zklighter.elliot.ai";
        constexpr bool kSecure = true;

        int marketId = -1;
        const std::string symUpper = upperAscii(cfg.symbol);
        if (parseIntStrict(symUpper, marketId))
        {
            std::ostringstream path;
            path << "/api/v1/orderBookDetails?market_id=" << marketId;
#if defined(ORDERBOOK_BACKEND_QT)
            auto body = shouldUseQtSocks5(cfg) ? httpGetQt(cfg, kHost, path.str(), kSecure)
                                              : httpGet(cfg, kHost, path.str(), kSecure);
#else
            auto body = httpGet(cfg, kHost, path.str(), kSecure);
#endif
            if (!body)
            {
                std::cerr << "[backend] lighter orderBookDetails fetch failed" << std::endl;
                return false;
            }

            json j;
            try
            {
                j = json::parse(*body);
            }
            catch (const std::exception &ex)
            {
                std::cerr << "[backend] lighter orderBookDetails JSON parse error: " << ex.what() << std::endl;
                return false;
            }

            auto extractFrom = [&](const json &arr) -> bool {
                if (!arr.is_array() || arr.empty() || !arr.front().is_object())
                {
                    return false;
                }
                const auto &obj = arr.front();
                const int priceDecimals = obj.value("price_decimals", -1);
                if (priceDecimals < 0 || priceDecimals > 12)
                {
                    return false;
                }
                marketIdOut = obj.value("market_id", marketId);
                tickSizeOut = std::pow(10.0, -static_cast<double>(priceDecimals));
                return tickSizeOut > 0.0;
            };

            if (extractFrom(j.value("order_book_details", json::array())))
            {
                return true;
            }
            if (extractFrom(j.value("spot_order_book_details", json::array())))
            {
                return true;
            }

            std::cerr << "[backend] lighter orderBookDetails: no details found for market_id=" << marketId << std::endl;
            return false;
        }

        // Symbol mode: resolve market_id via filter=all.
#if defined(ORDERBOOK_BACKEND_QT)
        auto body = shouldUseQtSocks5(cfg) ? httpGetQt(cfg, kHost, "/api/v1/orderBookDetails?filter=all", kSecure)
                                          : httpGet(cfg, kHost, "/api/v1/orderBookDetails?filter=all", kSecure);
#else
        auto body = httpGet(cfg, kHost, "/api/v1/orderBookDetails?filter=all", kSecure);
#endif
        if (!body)
        {
            std::cerr << "[backend] lighter orderBookDetails(all) fetch failed" << std::endl;
            return false;
        }

        json j;
        try
        {
            j = json::parse(*body);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "[backend] lighter orderBookDetails(all) JSON parse error: " << ex.what() << std::endl;
            return false;
        }

        auto findIn = [&](const json &arr) -> bool {
            if (!arr.is_array())
            {
                return false;
            }
            const std::string wantKey = alnumUpperKey(cfg.symbol);
            const std::string wantKeyNoQuote = stripQuoteSuffix(wantKey);
            for (const auto &obj : arr)
            {
                if (!obj.is_object())
                {
                    continue;
                }
                const std::string symRaw = obj.value("symbol", std::string());
                const std::string symKey = alnumUpperKey(symRaw);
                if (symKey.empty() || (symKey != wantKey && symKey != wantKeyNoQuote))
                {
                    continue;
                }
                const int priceDecimals = obj.value("price_decimals", -1);
                if (priceDecimals < 0 || priceDecimals > 12)
                {
                    continue;
                }
                marketIdOut = obj.value("market_id", -1);
                if (marketIdOut < 0)
                {
                    continue;
                }
                tickSizeOut = std::pow(10.0, -static_cast<double>(priceDecimals));
                return tickSizeOut > 0.0;
            }
            return false;
        };

        if (findIn(j.value("order_book_details", json::array())))
        {
            return true;
        }
        if (findIn(j.value("spot_order_book_details", json::array())))
        {
            return true;
        }

        std::cerr << "[backend] lighter: symbol not found: " << cfg.symbol
                  << " (note: perps use base symbols like AAVE/ONDO, spot uses pairs like ETH/USDC)"
                  << std::endl;
        return false;
    }

    bool runLighterWebSocket(const Config &config, dom::OrderBook &book, int marketId)
    {
#if defined(ORDERBOOK_BACKEND_QT)
        if (shouldUseQtSocks5(config))
        {
            bool proxyOk = false;
            const QNetworkProxy proxy = toQtProxy(config, proxyOk);
            if (!proxyOk)
            {
                return false;
            }

            QWebSocket ws;
            ws.setProxy(proxy);

            const QUrl url(QStringLiteral("wss://mainnet.zklighter.elliot.ai/stream"));
            std::cerr << "[backend] lighter: using Qt WebSocket (proxy)\n";

            const std::string subscribeBookStr =
                json({{"type", "subscribe"}, {"channel", "order_book/" + std::to_string(marketId)}}).dump();
            const std::string subscribeTradeStr =
                json({{"type", "subscribe"}, {"channel", "trade/" + std::to_string(marketId)}}).dump();

            bool subscribedBook = false;
            bool subscribedTrade = false;
            long long lastTradeId = 0;
            auto lastEmit = std::chrono::steady_clock::now();

            auto parseSide = [&](const json &levels) {
                std::vector<std::pair<dom::OrderBook::Tick, double>> out;
                if (!levels.is_array())
                {
                    return out;
                }
                out.reserve(levels.size());
                const double tickSize = book.tickSize();
                for (const auto &lvl : levels)
                {
                    if (!lvl.is_object())
                    {
                        continue;
                    }
                    const double price = jsonToDouble(lvl.value("price", json(0.0)));
                    const double qty = jsonToDouble(lvl.value("size", json(0.0)));
                    if (!(price > 0.0) || !std::isfinite(price) || !std::isfinite(qty))
                    {
                        continue;
                    }
                    dom::OrderBook::Tick tick = 0;
                    double snapped = price;
                    if (!quantizeTickFromPrice(price, tickSize, tick, snapped))
                    {
                        continue;
                    }
                    out.emplace_back(tick, qty);
                }
                return out;
            };

            auto applyBookUpdate = [&](const json &orderBook, bool snapshot) {
                if (!orderBook.is_object())
                {
                    return;
                }
                const auto bids = parseSide(orderBook.value("bids", json::array()));
                const auto asks = parseSide(orderBook.value("asks", json::array()));
                if (snapshot)
                {
                    std::lock_guard<std::mutex> lock(g_bookMutex);
                    book.loadSnapshot(bids, asks);
                }
                else
                {
                    std::lock_guard<std::mutex> lock(g_bookMutex);
                    book.applyDelta(bids, asks, config.cacheLevelsPerSide);
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - lastEmit >= config.throttle)
                {
                    lastEmit = now;
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                    emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                }
            };

            auto emitTradeBatch = [&](const json &arr) {
                if (!arr.is_array())
                {
                    return;
                }
                auto toLongLong = [](const json &v) -> long long {
                    try
                    {
                        if (v.is_number_integer())
                        {
                            return v.get<long long>();
                        }
                        if (v.is_number_float())
                        {
                            const double d = v.get<double>();
                            if (std::isfinite(d))
                            {
                                return static_cast<long long>(std::llround(d));
                            }
                            return 0LL;
                        }
                        if (v.is_string())
                        {
                            const std::string s = v.get<std::string>();
                            if (s.empty())
                            {
                                return 0LL;
                            }
                            std::size_t idx = 0;
                            const long long out = std::stoll(s, &idx, 10);
                            return idx > 0 ? out : 0LL;
                        }
                    }
                    catch (...)
                    {
                    }
                    return 0LL;
                };
                const double tickSize = book.tickSize();
                if (!(tickSize > 0.0) || !std::isfinite(tickSize))
                {
                    return;
                }
                for (const auto &tIn : arr)
                {
                    if (!tIn.is_object())
                    {
                        continue;
                    }
                    const long long tradeId = toLongLong(tIn.value("trade_id", json(0LL)));
                    if (tradeId > 0 && tradeId <= lastTradeId)
                    {
                        continue;
                    }
                    const double price = jsonToDouble(tIn.value("price", json(0.0)));
                    const double size = jsonToDouble(tIn.value("size", json(0.0)));
                    if (!(price > 0.0) || !(size > 0.0) || !std::isfinite(price) || !std::isfinite(size))
                    {
                        continue;
                    }
                    const bool isMakerAsk = tIn.value("is_maker_ask", false);
                    const bool buy = isMakerAsk;
                    const long long ts = toLongLong(tIn.value("timestamp", json(0LL)));

                    json out;
                    out["type"] = "trade";
                    out["symbol"] = config.symbol;
                    dom::OrderBook::Tick tick = 0;
                    double snapped = price;
                    if (quantizeTickFromPrice(price, tickSize, tick, snapped))
                    {
                        out["tick"] = tick;
                        out["price"] = snapped;
                    }
                    else
                    {
                        out["price"] = price;
                    }
                    out["qty"] = size;
                    out["side"] = buy ? "buy" : "sell";
                    if (ts > 0)
                    {
                        out["timestamp"] = ts;
                    }
                    stdoutWriter().writeLine(out.dump());
                    if (tradeId > 0)
                    {
                        lastTradeId = std::max(lastTradeId, tradeId);
                    }
                }
            };

            QEventLoop loop;
            QTimer watchdog;
            watchdog.setSingleShot(false);
            watchdog.start(20000);
            QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() {
                std::cerr << "[backend] lighter ws watchdog timeout\n";
                ws.abort();
                loop.quit();
            });

            QObject::connect(&ws, &QWebSocket::connected, &loop, [&]() {
                std::cerr << "[backend] connected to Lighter ws (Qt)\n";
            });
            QObject::connect(&ws, &QWebSocket::disconnected, &loop, [&]() {
                std::cerr << "[backend] Lighter WS disconnected (Qt)\n";
                loop.quit();
            });
            QObject::connect(&ws,
                             QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
                             &loop,
                             [&](QAbstractSocket::SocketError) {
                                 std::cerr << "[backend] Lighter WS error (Qt): "
                                           << ws.errorString().toStdString() << "\n";
                                 loop.quit();
                             });

            QObject::connect(&ws, &QWebSocket::textMessageReceived, &loop, [&](const QString &msg) {
                watchdog.start(20000);
                json j;
                try
                {
                    j = json::parse(msg.toStdString());
                }
                catch (const std::exception &ex)
                {
                    std::cerr << "[backend] Lighter JSON parse error (Qt): " << ex.what() << "\n";
                    return;
                }

                const std::string typeStr = j.value("type", std::string());
                if (typeStr == "ping")
                {
                    ws.sendTextMessage(QStringLiteral("{\"type\":\"pong\"}"));
                    return;
                }
                if (typeStr == "connected")
                {
                    if (!subscribedBook)
                    {
                        ws.sendTextMessage(QString::fromStdString(subscribeBookStr));
                        subscribedBook = true;
                        std::cerr << "[backend] lighter subscribed: " << subscribeBookStr << "\n";
                    }
                    if (!subscribedTrade)
                    {
                        ws.sendTextMessage(QString::fromStdString(subscribeTradeStr));
                        subscribedTrade = true;
                        std::cerr << "[backend] lighter subscribed: " << subscribeTradeStr << "\n";
                    }
                    return;
                }
                if (typeStr == "subscribed/order_book")
                {
                    applyBookUpdate(j.value("order_book", json::object()), true);
                    return;
                }
                if (typeStr == "update/order_book")
                {
                    applyBookUpdate(j.value("order_book", json::object()), false);
                    return;
                }
                if (typeStr == "subscribed/trade" || typeStr == "update/trade")
                {
                    emitTradeBatch(j.value("trades", json::array()));
                    return;
                }
            });

            ws.open(url);
            loop.exec();
            return true;
        }
#endif
        const std::wstring host = L"mainnet.zklighter.elliot.ai";
        const std::wstring path = L"/stream";

        WinHttpHandle session = openSession(config);
        if (!session.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
            return false;
        }

        WinHttpHandle connection(
            WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!connection.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
            return false;
        }

        WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                                 L"GET",
                                                 path.c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE));
        if (!request.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
            return false;
        }

        applyProxyCredentials(config, request.get());
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSetOption") << std::endl;
            return false;
        }

        if (!WinHttpSendRequest(request.get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
            return false;
        }

        if (!WinHttpReceiveResponse(request.get(), nullptr))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
            return false;
        }

        HINTERNET rawSocket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
        if (!rawSocket)
        {
            std::cerr << "[backend] " << winhttpError("WinHttpWebSocketCompleteUpgrade") << std::endl;
            return false;
        }
        request.reset();

        std::cerr << "[backend] connected to Lighter ws" << std::endl;

        const std::string subscribeBookStr =
            json({{"type", "subscribe"}, {"channel", "order_book/" + std::to_string(marketId)}}).dump();
        const std::string subscribeTradeStr =
            json({{"type", "subscribe"}, {"channel", "trade/" + std::to_string(marketId)}}).dump();

        std::vector<unsigned char> buffer(256 * 1024);
        std::string fragmentBuffer;
        bool subscribedBook = false;
        bool subscribedTrade = false;
        long long lastTradeId = 0;
        auto lastEmit = std::chrono::steady_clock::now();

        auto parseSide = [&](const json &levels) {
            std::vector<std::pair<dom::OrderBook::Tick, double>> out;
            if (!levels.is_array())
            {
                return out;
            }
            out.reserve(levels.size());
            const double tickSize = book.tickSize();
            for (const auto &lvl : levels)
            {
                if (!lvl.is_object())
                {
                    continue;
                }
                const double price = jsonToDouble(lvl.value("price", json(0.0)));
                const double qty = jsonToDouble(lvl.value("size", json(0.0)));
                if (!(price > 0.0) || !std::isfinite(price) || !std::isfinite(qty))
                {
                    continue;
                }
                dom::OrderBook::Tick tick = 0;
                double snapped = price;
                if (!quantizeTickFromPrice(price, tickSize, tick, snapped))
                {
                    continue;
                }
                out.emplace_back(tick, qty);
            }
            return out;
        };

        auto applyBookUpdate = [&](const json &orderBook, bool snapshot) {
            if (!orderBook.is_object())
            {
                return;
            }
            const auto bids = parseSide(orderBook.value("bids", json::array()));
            const auto asks = parseSide(orderBook.value("asks", json::array()));
            if (snapshot)
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                book.loadSnapshot(bids, asks);
            }
            else
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                book.applyDelta(bids, asks, config.cacheLevelsPerSide);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - lastEmit >= config.throttle)
            {
                lastEmit = now;
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
            }
        };

        auto emitTradeBatch = [&](const json &arr) {
            if (!arr.is_array())
            {
                return;
            }
            auto toLongLong = [](const json &v) -> long long {
                try
                {
                    if (v.is_number_integer())
                    {
                        return v.get<long long>();
                    }
                    if (v.is_number_float())
                    {
                        const double d = v.get<double>();
                        if (std::isfinite(d))
                        {
                            return static_cast<long long>(std::llround(d));
                        }
                        return 0LL;
                    }
                    if (v.is_string())
                    {
                        const std::string s = v.get<std::string>();
                        if (s.empty())
                        {
                            return 0LL;
                        }
                        std::size_t idx = 0;
                        const long long out = std::stoll(s, &idx, 10);
                        return idx > 0 ? out : 0LL;
                    }
                }
                catch (...)
                {
                }
                return 0LL;
            };
            const double tickSize = book.tickSize();
            if (!(tickSize > 0.0) || !std::isfinite(tickSize))
            {
                return;
            }
            for (const auto &tIn : arr)
            {
                if (!tIn.is_object())
                {
                    continue;
                }
                const long long tradeId = toLongLong(tIn.value("trade_id", json(0LL)));
                if (tradeId > 0 && tradeId <= lastTradeId)
                {
                    continue;
                }
                const double price = jsonToDouble(tIn.value("price", json(0.0)));
                const double size = jsonToDouble(tIn.value("size", json(0.0)));
                if (!(price > 0.0) || !(size > 0.0) || !std::isfinite(price) || !std::isfinite(size))
                {
                    continue;
                }
                const bool isMakerAsk = tIn.value("is_maker_ask", false);
                // If maker is ask (sell), taker is buy; use taker direction for prints.
                const bool buy = isMakerAsk;
                const long long ts = toLongLong(tIn.value("timestamp", json(0LL)));

                json out;
                out["type"] = "trade";
                out["symbol"] = config.symbol;
                dom::OrderBook::Tick tick = 0;
                double snapped = price;
                if (quantizeTickFromPrice(price, tickSize, tick, snapped))
                {
                    out["tick"] = tick;
                    out["price"] = snapped;
                }
                else
                {
                    out["price"] = price;
                }
                out["qty"] = size;
                out["side"] = buy ? "buy" : "sell";
                if (ts > 0)
                {
                    out["timestamp"] = ts;
                }
                stdoutWriter().writeLine(out.dump());
                if (tradeId > 0)
                {
                    lastTradeId = std::max(lastTradeId, tradeId);
                }
            }
        };

        for (;;)
        {
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            HRESULT hr =
                WinHttpWebSocketReceive(rawSocket, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
            if (FAILED(hr))
            {
                std::cerr << "[backend] Lighter WS receive failed: " << std::hex << hr << std::dec << std::endl;
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                std::cerr << "[backend] Lighter WS closed by server\n";
                break;
            }
            if (received == 0)
            {
                continue;
            }
            if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                continue;
            }

            const std::string_view chunk(reinterpret_cast<char *>(buffer.data()), received);
            const bool isFragment = (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);
            if (isFragment)
            {
                fragmentBuffer.append(chunk.data(), chunk.size());
                if (fragmentBuffer.size() > 4 * 1024 * 1024)
                {
                    std::cerr << "[backend] Lighter WS fragment buffer too large, dropping\n";
                    fragmentBuffer.clear();
                }
                continue;
            }

            std::string message;
            if (!fragmentBuffer.empty())
            {
                fragmentBuffer.append(chunk.data(), chunk.size());
                message.swap(fragmentBuffer);
            }
            else
            {
                message.assign(chunk);
            }

            json j;
            try
            {
                j = json::parse(message);
            }
            catch (const std::exception &ex)
            {
                std::cerr << "[backend] Lighter JSON parse error: " << ex.what() << std::endl;
                continue;
            }

            const std::string typeStr = j.value("type", std::string());
            if (typeStr == "ping")
            {
                const std::string pong = R"({"type":"pong"})";
                WinHttpWebSocketSend(rawSocket,
                                     WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     (void *)pong.data(),
                                     static_cast<DWORD>(pong.size()));
                continue;
            }

            if (typeStr == "connected")
            {
                if (!subscribedBook)
                {
                    WinHttpWebSocketSend(rawSocket,
                                         WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (void *)subscribeBookStr.data(),
                                         static_cast<DWORD>(subscribeBookStr.size()));
                    subscribedBook = true;
                    std::cerr << "[backend] lighter subscribed: " << subscribeBookStr << std::endl;
                }
                if (!subscribedTrade)
                {
                    WinHttpWebSocketSend(rawSocket,
                                         WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (void *)subscribeTradeStr.data(),
                                         static_cast<DWORD>(subscribeTradeStr.size()));
                    subscribedTrade = true;
                    std::cerr << "[backend] lighter subscribed: " << subscribeTradeStr << std::endl;
                }
                continue;
            }

            if (typeStr == "subscribed/order_book")
            {
                applyBookUpdate(j.value("order_book", json::object()), true);
                continue;
            }

            if (typeStr == "update/order_book")
            {
                applyBookUpdate(j.value("order_book", json::object()), false);
                continue;
            }

            if (typeStr == "subscribed/trade" || typeStr == "update/trade")
            {
                emitTradeBatch(j.value("trades", json::array()));
                continue;
            }
        }

        WinHttpWebSocketClose(rawSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(rawSocket);
        return true;
    }

    bool fetchExchangeInfo(const Config& cfg, double& tickSizeOut)
    {
        std::ostringstream path;
        path << "/api/v3/exchangeInfo?symbol=" << cfg.symbol;

        auto body = httpGet(cfg, "api.mexc.com", path.str(), true);
        if (!body)
        {
            std::cerr << "[backend] failed to fetch exchangeInfo" << std::endl;
            return false;
        }

        json j;
        try
        {
            j = json::parse(*body);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[backend] exchangeInfo JSON parse error: " << ex.what() << std::endl;
            return false;
        }

        if (!j.contains("symbols") || !j["symbols"].is_array() || j["symbols"].empty())
        {
            std::cerr << "[backend] exchangeInfo: no symbols array" << std::endl;
            return false;
        }

        const auto& sym = j["symbols"].front();

        // Prefer explicit tick size; quotePrecision is not a reliable substitute.
        double tickSize = 0.0;
        if (sym.contains("filters") && sym["filters"].is_array())
        {
            for (const auto& f : sym["filters"])
            {
                const std::string type = f.value("filterType", std::string());
                if (type == "PRICE_FILTER")
                {
                    tickSize = jsonToDouble(f.value("tickSize", json(0.0)));
                    if (tickSize > 0.0)
                    {
                        break;
                    }
                }
            }
        }
        if (tickSize <= 0.0 && sym.contains("tickSize"))
        {
            tickSize = jsonToDouble(sym.value("tickSize", json(0.0)));
        }
        if (tickSize <= 0.0)
        {
            int quotePrecision = 0;
            if (sym.contains("quotePrecision") && sym["quotePrecision"].is_number_integer())
            {
                quotePrecision = sym["quotePrecision"].get<int>();
            }
            else if (sym.contains("quoteAssetPrecision") && sym["quoteAssetPrecision"].is_number_integer())
            {
                quotePrecision = sym["quoteAssetPrecision"].get<int>();
            }
            if (quotePrecision > 0)
            {
                tickSize = std::pow(10.0, -quotePrecision);
            }
        }

        tickSizeOut = tickSize;
        std::cerr << "[backend] exchangeInfo: tickSize=" << tickSizeOut << std::endl;
        return tickSizeOut > 0.0;
    }

    bool fetchSnapshot(const Config& cfg, dom::OrderBook& book)
    {
        const double tickSize = book.tickSize();
        if (tickSize <= 0.0)
        {
            std::cerr << "[backend] fetchSnapshot: tickSize is not set" << std::endl;
            return false;
        }

        std::ostringstream path;
        path << "/api/v3/depth?symbol=" << cfg.symbol << "&limit=" << cfg.snapshotDepth;

        auto body = httpGet(cfg, "api.mexc.com", path.str(), true);
        if (!body)
        {
            return false;
        }

        json j;
        try
        {
            j = json::parse(*body);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[backend] depth JSON parse error: " << ex.what() << std::endl;
            return false;
        }

        std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
        std::vector<std::pair<dom::OrderBook::Tick, double>> asks;

        auto parseSide = [tickSize](const json& arr,
                                    std::vector<std::pair<dom::OrderBook::Tick, double>>& out) {
            out.clear();
            for (const auto& e : arr)
            {
                if (!e.is_array() || e.size() < 2) continue;
                double price = std::stod(e[0].get<std::string>());
                double qty = std::stod(e[1].get<std::string>());
                const auto tick = tickFromPrice(price, tickSize);
                out.emplace_back(tick, qty);
            }
        };

        parseSide(j["bids"], bids);
        parseSide(j["asks"], asks);
        book.loadSnapshot(bids, asks);

        std::cerr << "[backend] snapshot loaded: bids=" << bids.size() << " asks=" << asks.size() << std::endl;
        return true;
    }

    bool fetchFuturesContractInfo(const Config &cfg, double &tickSizeOut, double &contractSizeOut)
    {
        std::ostringstream path;
        path << "/api/v1/contract/detail?symbol=" << cfg.symbol;
        auto body = httpGet(cfg, "contract.mexc.com", path.str(), true);
        if (!body)
        {
            std::cerr << "[backend] futures contract detail fetch failed" << std::endl;
            return false;
        }
        json j;
        try
        {
            j = json::parse(*body);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "[backend] futures contract detail parse error: " << ex.what() << std::endl;
            return false;
        }
        json data = j.value("data", json{});
        if (data.is_array() && !data.empty())
        {
            data = data.front();
        }
        if (!data.is_object())
        {
            std::cerr << "[backend] futures contract detail: missing data" << std::endl;
            return false;
        }
        contractSizeOut = jsonToDouble(data.value("contractSize", json(1.0)));
        if (contractSizeOut <= 0.0) {
            contractSizeOut = 1.0;
        }
        double priceUnit = jsonToDouble(data.value("priceUnit", json(0.0)));
        const double priceScale = jsonToDouble(data.value("priceScale", json(0.0)));
        if (priceUnit <= 0.0 && priceScale > 0.0)
        {
            priceUnit = std::pow(10.0, -priceScale);
        }
        if (priceUnit <= 0.0)
        {
            priceUnit = 0.0001;
        }
        tickSizeOut = priceUnit;
        std::cerr << "[backend] futures contract: tickSize=" << tickSizeOut
                  << " contractSize=" << contractSizeOut << std::endl;
        return tickSizeOut > 0.0;
    }

    bool fetchFuturesSnapshot(const Config &cfg, dom::OrderBook &book, double contractSize)
    {
        const double tickSize = book.tickSize();
        if (tickSize <= 0.0)
        {
            std::cerr << "[backend] futures snapshot: tickSize missing" << std::endl;
            return false;
        }
        if (contractSize <= 0.0) {
            contractSize = 1.0;
        }
        std::ostringstream path;
        path << "/api/v1/contract/depth/" << cfg.symbol << "?limit=" << cfg.snapshotDepth;
        auto body = httpGet(cfg, "contract.mexc.com", path.str(), true);
        if (!body)
        {
            std::cerr << "[backend] futures snapshot fetch failed" << std::endl;
            return false;
        }
        json j;
        try
        {
            j = json::parse(*body);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "[backend] futures depth parse error: " << ex.what() << std::endl;
            return false;
        }
        const json &data = j.value("data", json::object());
        if (!data.is_object())
        {
            std::cerr << "[backend] futures snapshot: invalid payload" << std::endl;
            return false;
        }
        std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
        std::vector<std::pair<dom::OrderBook::Tick, double>> asks;
        auto parseSide = [&](const json &side, std::vector<std::pair<dom::OrderBook::Tick,double>> &out) {
            out.clear();
            if (!side.is_array())
            {
                return;
            }
            for (const auto &row : side)
            {
                if (row.is_array() && !row.empty())
                {
                    const double price = row.size() > 0 ? jsonToDouble(row[0]) : 0.0;
                    double qty = row.size() > 1 ? jsonToDouble(row[1]) : 0.0;
                    if (price <= 0.0 || qty < 0.0)
                    {
                        continue;
                    }
                    qty *= contractSize;
                    const auto tick = tickFromPrice(price, tickSize);
                    out.emplace_back(tick, qty);
                }
            }
        };
        parseSide(data.value("bids", json::array()), bids);
        parseSide(data.value("asks", json::array()), asks);
        book.loadSnapshot(bids, asks);
        std::cerr << "[backend] futures snapshot loaded: bids=" << bids.size()
                  << " asks=" << asks.size() << std::endl;
        return true;
    }

    // --- минимальный парсер protobuf под нужные сообщения ---

    struct ProtoReader
    {
        const std::uint8_t* data{};
        std::size_t size{};
        std::size_t pos{};

        ProtoReader() = default;
        ProtoReader(const void* ptr, std::size_t len)
            : data(static_cast<const std::uint8_t*>(ptr))
            , size(len)
            , pos(0)
        {
        }

        bool eof() const { return pos >= size; }

        bool readVarint(std::uint64_t& out)
        {
            out = 0;
            int shift = 0;
            while (pos < size && shift < 64)
            {
                std::uint8_t b = data[pos++];
                out |= (std::uint64_t(b & 0x7F) << shift);
                if ((b & 0x80) == 0)
                {
                    return true;
                }
                shift += 7;
            }
            return false;
        }

        bool readBytes(std::size_t n, std::string& out)
        {
            if (pos + n > size)
            {
                return false;
            }
            out.assign(reinterpret_cast<const char*>(data + pos), n);
            pos += n;
            return true;
        }

        bool readLengthDelimited(std::string& out)
        {
            std::uint64_t len = 0;
            if (!readVarint(len))
            {
                return false;
            }
            return readBytes(static_cast<std::size_t>(len), out);
        }

        bool skipField(std::uint64_t key)
        {
            const auto wireType = key & 0x7;
            switch (wireType)
            {
            case 0: // varint
            {
                std::uint64_t dummy;
                return readVarint(dummy);
            }
            case 1: // 64-bit
                if (pos + 8 > size) return false;
                pos += 8;
                return true;
            case 2: // length-delimited
            {
                std::uint64_t len = 0;
                if (!readVarint(len) || pos + len > size)
                {
                    return false;
                }
                pos += static_cast<std::size_t>(len);
                return true;
            }
            case 5: // 32-bit
                if (pos + 4 > size) return false;
                pos += 4;
                return true;
            default:
                return false;
            }
        }
    };

    void parseDepthItem(const std::string& buf,
                        double tickSize,
                        std::vector<std::pair<dom::OrderBook::Tick, double>>& out)
    {
        ProtoReader r(buf.data(), buf.size());
        std::string priceStr;
        std::string qtyStr;
        while (!r.eof())
        {
            std::uint64_t key = 0;
            if (!r.readVarint(key)) break;
            const auto field = key >> 3;
            if ((key & 0x7) != 2)
            {
                if (!r.skipField(key)) break;
                continue;
            }

            std::string value;
            if (!r.readLengthDelimited(value)) break;

            if (field == 1)
            {
                priceStr = value;
            }
            else if (field == 2)
            {
                qtyStr = value;
            }
        }

        if (!priceStr.empty() && tickSize > 0.0)
        {
            double price = std::stod(priceStr);
            double qty = qtyStr.empty() ? 0.0 : std::stod(qtyStr);
            const auto tick = tickFromPrice(price, tickSize);
            out.emplace_back(tick, qty);
        }
    }

    void parseAggreDepth(const std::string& buf,
                         double tickSize,
                         std::vector<std::pair<dom::OrderBook::Tick, double>>& asks,
                         std::vector<std::pair<dom::OrderBook::Tick, double>>& bids)
    {
        ProtoReader r(buf.data(), buf.size());
        while (!r.eof())
        {
            std::uint64_t key = 0;
            if (!r.readVarint(key)) break;
            const auto field = key >> 3;
            if ((key & 0x7) != 2)
            {
                if (!r.skipField(key)) break;
                continue;
            }

            std::string msg;
            if (!r.readLengthDelimited(msg)) break;

            if (field == 1) // asks
            {
                parseDepthItem(msg, tickSize, asks);
            }
            else if (field == 2) // bids
            {
                parseDepthItem(msg, tickSize, bids);
            }
            // fromVersion / toVersion мы игнорируем
        }
    }

    struct PublicAggreDeal
    {
        double price{};
        double quantity{};
        bool buy{};
        std::int64_t time{};
    };

    void parseAggreDealItem(const std::string& buf,
                            std::vector<PublicAggreDeal>& out)
    {
        ProtoReader r(buf.data(), buf.size());
        std::string priceStr;
        std::string qtyStr;
        int tradeType = 0;
        std::int64_t time = 0;

        while (!r.eof())
        {
            std::uint64_t key = 0;
            if (!r.readVarint(key)) break;
            const auto field = key >> 3;
            const auto wire = key & 0x7;

            if (wire == 2)
            {
                std::string value;
                if (!r.readLengthDelimited(value)) break;
                if (field == 1)
                {
                    priceStr = value;
                }
                else if (field == 2)
                {
                    qtyStr = value;
                }
            }
            else if (wire == 0)
            {
                std::uint64_t v = 0;
                if (!r.readVarint(v)) break;
                if (field == 3)
                {
                    tradeType = static_cast<int>(v);
                }
                else if (field == 4)
                {
                    time = static_cast<std::int64_t>(v);
                }
            }
            else
            {
                if (!r.skipField(key)) break;
            }
        }

        if (!priceStr.empty())
        {
            double price = std::stod(priceStr);
            double qty = qtyStr.empty() ? 0.0 : std::stod(qtyStr);
            if (qty <= 0.0) return;

            PublicAggreDeal d;
            d.price = price;
            d.quantity = qty;
            d.time = time;
            // tradeType: 1/2 — точное значение зависит от биржи; считаем 1=buy,2=sell
            d.buy = (tradeType != 2);
            out.push_back(d);
        }
    }

    void parseAggreDeals(const std::string& buf,
                         std::vector<PublicAggreDeal>& out)
    {
        ProtoReader r(buf.data(), buf.size());
        while (!r.eof())
        {
            std::uint64_t key = 0;
            if (!r.readVarint(key)) break;
            const auto field = key >> 3;
            if ((key & 0x7) != 2)
            {
                if (!r.skipField(key)) break;
                continue;
            }

            std::string msg;
            if (!r.readLengthDelimited(msg)) break;

            if (field == 1) // repeated deals
            {
                parseAggreDealItem(msg, out);
            }
            // field 2 = eventType (string) — игнорируем
        }
    }

    bool parsePushWrapper(const void* data,
                          std::size_t len,
                          std::string& channelOut,
                          double tickSize,
                          std::vector<std::pair<dom::OrderBook::Tick, double>>& asks,
                          std::vector<std::pair<dom::OrderBook::Tick, double>>& bids)
    {
        ProtoReader r(data, len);
        std::string depthBody;

        while (!r.eof())
        {
            std::uint64_t key = 0;
            if (!r.readVarint(key)) break;
            const auto field = key >> 3;

            if ((key & 0x7) != 2)
            {
                if (!r.skipField(key)) break;
                continue;
            }

            std::string value;
            if (!r.readLengthDelimited(value)) break;

            if (field == 1)
            {
                channelOut = value;
            }
            else if (field == 313)
            {
                depthBody = std::move(value);
            }
        }

        if (depthBody.empty())
        {
            return false;
        }

        asks.clear();
        bids.clear();
        parseAggreDepth(depthBody, tickSize, asks, bids);
        return true;
    }

    bool parseDealsFromWrapper(const void* data,
                               std::size_t len,
                               std::string& channelOut,
                               std::vector<PublicAggreDeal>& deals)
    {
        ProtoReader r(data, len);
        std::string dealsBody;

        while (!r.eof())
        {
            std::uint64_t key = 0;
            if (!r.readVarint(key)) break;
            const auto field = key >> 3;

            if ((key & 0x7) != 2)
            {
                if (!r.skipField(key)) break;
                continue;
            }

            std::string value;
            if (!r.readLengthDelimited(value)) break;

            if (field == 1)
            {
                channelOut = value;
            }
            else if (field == 314)
            {
                dealsBody = std::move(value);
            }
        }

        if (dealsBody.empty())
        {
            return false;
        }

        deals.clear();
        parseAggreDeals(dealsBody, deals);
        return !deals.empty();
    }

    void emitLadder(const Config& config,
                    const dom::OrderBook& book,
                    double bestBid,
                    double bestAsk,
                    std::int64_t ts);

    std::mutex g_bookMutex;
    std::atomic<bool> g_bookReady{false};
    dom::OrderBook* g_bookPtr = nullptr;
    Config g_activeConfig;
    std::vector<dom::OrderBook::Row> g_lastLadderRows;
    dom::OrderBook::Tick g_lastWindowMinTick = 0;
    dom::OrderBook::Tick g_lastWindowMaxTick = 0;
    bool g_haveLastLadder = false;
    bool g_forceFullLadder = false;

    void heartbeatThread()
    {
        using namespace std::chrono_literals;
        // Keep the GUI watchdog alive even when a WS connection is idle (WinHTTP receive blocks).
        for (;;)
        {
            std::this_thread::sleep_for(5s);
            if (!g_bookReady.load())
            {
                continue;
            }
            std::lock_guard<std::mutex> lock(g_bookMutex);
            if (!g_bookPtr)
            {
                continue;
            }
            json hb;
            hb["type"] = "hb";
            hb["symbol"] = g_activeConfig.symbol;
            hb["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
            hb["bestBid"] = g_bookPtr->bestBid();
            hb["bestAsk"] = g_bookPtr->bestAsk();
            stdoutWriter().writeLine(hb.dump());
        }
    }

    void emitCurrentLadderLocked()
    {
        const double bestBid = g_bookPtr->bestBid();
        const double bestAsk = g_bookPtr->bestAsk();
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        emitLadder(g_activeConfig, *g_bookPtr, bestBid, bestAsk, nowMs);
    }

    void emitCurrentLadder()
    {
        if (!g_bookReady.load()) return;
        std::lock_guard<std::mutex> lock(g_bookMutex);
        if (!g_bookPtr) return;
        emitCurrentLadderLocked();
    }

    void applyShiftAndEmit(dom::OrderBook::Tick delta)
    {
        if (!g_bookReady.load()) return;
        std::lock_guard<std::mutex> lock(g_bookMutex);
        if (!g_bookPtr) return;
        g_bookPtr->shiftManualCenterTicks(delta);
        emitCurrentLadderLocked();
    }

    void clearManualCenterAndEmit()
    {
        if (!g_bookReady.load()) return;
        std::lock_guard<std::mutex> lock(g_bookMutex);
        if (!g_bookPtr) return;
        g_lastLadderRows.clear();
        g_haveLastLadder = false;
        g_forceFullLadder = true;
        g_bookPtr->clearManualCenter();
        emitCurrentLadderLocked();
    }

    void controlReaderThread()
    {
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line.empty()) continue;
            try
            {
                const auto j = json::parse(line);
                const std::string cmd = j.value("cmd", std::string());
                if (cmd == "shift")
                {
                    const double ticks = j.value("ticks", 0.0);
                    const auto delta = static_cast<dom::OrderBook::Tick>(std::llround(ticks));
                    applyShiftAndEmit(delta);
                }
                else if (cmd == "center_auto")
                {
                    clearManualCenterAndEmit();
                }
                else if (cmd == "force_full")
                {
                    if (!g_bookReady.load()) continue;
                    std::lock_guard<std::mutex> lock(g_bookMutex);
                    if (!g_bookPtr) continue;
                    g_lastLadderRows.clear();
                    g_haveLastLadder = false;
                    g_forceFullLadder = true;
                    emitCurrentLadderLocked();
                }
            }
            catch (const std::exception& ex)
            {
                std::cerr << "[backend] control input error: " << ex.what() << std::endl;
            }
        }
    }

    void emitLadder(const Config& config,
                    const dom::OrderBook& book,
                    double bestBid,
                    double bestAsk,
                    std::int64_t ts)
    {
        dom::OrderBook::Tick winMin = 0;
        dom::OrderBook::Tick winMax = 0;
        dom::OrderBook::Tick centerTick = 0;
        // Ladder emission must reflect what's actually visible. `cacheLevelsPerSide` is just
        // storage capacity and can be much larger; using it here can generate massive JSON
        // payloads (and stall/crash the GUI pipe), causing LadderClient restarts.
        const std::size_t ladderLevels =
            std::max<std::size_t>(config.ladderLevelsPerSide, 1);
        auto rowsSparse = book.ladderSparse(ladderLevels, &winMin, &winMax, &centerTick);
        const double tickSize = book.tickSize();

        auto enrich = [&](json &out) {
            out["symbol"] = config.symbol;
            out["timestamp"] = ts;
            out["bestBid"] = bestBid;
            out["bestAsk"] = bestAsk;
            out["tickSize"] = tickSize;
            out["windowMinTick"] = winMin;
            out["windowMaxTick"] = winMax;
            out["centerTick"] = centerTick;
        };

        const bool needFull = !g_haveLastLadder || g_forceFullLadder;
        if (needFull)
        {
            json out;
            out["type"] = "ladder";
            out["sparse"] = true;
            json rows = json::array();
            for (const auto &row : rowsSparse)
            {
                json r;
                r["tick"] = row.tick;
                if (row.bidQuantity > 0.0) {
                    r["bid"] = row.bidQuantity;
                }
                if (row.askQuantity > 0.0) {
                    r["ask"] = row.askQuantity;
                }
                rows.push_back(std::move(r));
            }
            out["rows"] = std::move(rows);
            enrich(out);
            stdoutWriter().writeLine(out.dump());
            g_haveLastLadder = true;
            g_forceFullLadder = false;
        }
        else
        {
            json updates = json::array();
            json removals = json::array();

            const auto prevCount = static_cast<std::ptrdiff_t>(g_lastLadderRows.size());
            const auto currCount = static_cast<std::ptrdiff_t>(rowsSparse.size());

            std::ptrdiff_t i = 0;
            std::ptrdiff_t j = 0;
            while (i < currCount && j < prevCount)
            {
                const auto &cur = rowsSparse[static_cast<std::size_t>(i)];
                const auto &prev = g_lastLadderRows[static_cast<std::size_t>(j)];
                if (cur.tick == prev.tick)
                {
                    const bool bidChanged = (std::abs(prev.bidQuantity - cur.bidQuantity) > 1e-9);
                    const bool askChanged = (std::abs(prev.askQuantity - cur.askQuantity) > 1e-9);
                    if (bidChanged || askChanged)
                    {
                        json u;
                        u["tick"] = cur.tick;
                        if (bidChanged) {
                            // Include zeros: we must be able to clear one side while keeping the other.
                            u["bid"] = cur.bidQuantity;
                        }
                        if (askChanged) {
                            u["ask"] = cur.askQuantity;
                        }
                        updates.push_back(std::move(u));
                    }
                    ++i;
                    ++j;
                }
                else if (cur.tick > prev.tick)
                {
                    // New tick appears in current window.
                    json u;
                    u["tick"] = cur.tick;
                    if (cur.bidQuantity > 0.0) {
                        u["bid"] = cur.bidQuantity;
                    }
                    if (cur.askQuantity > 0.0) {
                        u["ask"] = cur.askQuantity;
                    }
                    updates.push_back(std::move(u));
                    ++i;
                }
                else
                {
                    // Tick removed from the current window (or became empty).
                    removals.push_back(prev.tick);
                    ++j;
                }
            }

            for (; i < currCount; ++i)
            {
                const auto &cur = rowsSparse[static_cast<std::size_t>(i)];
                json u;
                u["tick"] = cur.tick;
                if (cur.bidQuantity > 0.0) {
                    u["bid"] = cur.bidQuantity;
                }
                if (cur.askQuantity > 0.0) {
                    u["ask"] = cur.askQuantity;
                }
                updates.push_back(std::move(u));
            }
            for (; j < prevCount; ++j)
            {
                const auto &prev = g_lastLadderRows[static_cast<std::size_t>(j)];
                removals.push_back(prev.tick);
            }
            if (!updates.empty() || !removals.empty()
                || winMin != g_lastWindowMinTick || winMax != g_lastWindowMaxTick)
            {
                json out;
                out["type"] = "ladder_delta";
                out["sparse"] = true;
                out["updates"] = std::move(updates);
                out["removals"] = std::move(removals);
                enrich(out);
                stdoutWriter().writeLine(out.dump());
            }
        }

        g_lastLadderRows = std::move(rowsSparse);
        g_lastWindowMinTick = winMin;
        g_lastWindowMaxTick = winMax;
    }

    // Legacy MEXC spot protobuf WS implementation (kept for reference / debugging).
    bool runWebSocket(const Config& config, dom::OrderBook& book)
    {
        WinHttpHandle session = openSession(config);
        if (!session.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
            return false;
        }

        const std::wstring host = L"wbs-api.mexc.com";
        const std::wstring path = L"/ws";

        WinHttpHandle connection(
            WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!connection.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
            return false;
        }

        WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                                 L"GET",
                                                 path.c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE));
        if (!request.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
            return false;
        }

        applyProxyCredentials(config, request.get());
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSetOption") << std::endl;
            return false;
        }

        if (!WinHttpSendRequest(request.get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
            return false;
        }

        if (!WinHttpReceiveResponse(request.get(), nullptr))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
            return false;
        }

        HINTERNET rawSocket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
        if (!rawSocket)
        {
            std::cerr << "[backend] " << winhttpError("WinHttpWebSocketCompleteUpgrade") << std::endl;
            return false;
        }
        request.reset();

        std::cerr << "[backend] connected to Mexc ws" << std::endl;

        // Подписка на aggre.depth и aggre.deals
        std::ostringstream depthChannel;
        depthChannel << "spot@public.aggre.depth.v3.api.pb@" << config.mexcStreamIntervalMs << "ms@" << config.symbol;
        // Aggre deals channel also requires an interval suffix (10ms/100ms); without it
        // the server replies with "Blocked" and sends no trades.
        std::ostringstream dealsChannel;
        dealsChannel << "spot@public.aggre.deals.v3.api.pb@" << config.mexcStreamIntervalMs << "ms@" << config.symbol;
        json sub = {{"method", "SUBSCRIPTION"},
                    {"params", json::array({depthChannel.str(), dealsChannel.str()})}};
        const std::string subStr = sub.dump();

        if (WinHttpWebSocketSend(rawSocket,
                                 WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 (void*) subStr.data(),
                                 static_cast<DWORD>(subStr.size())) != S_OK)
        {
            std::cerr << "[backend] failed to send SUBSCRIPTION" << std::endl;
            WinHttpCloseHandle(rawSocket);
            return false;
        }

        std::cerr << "[backend] sent " << subStr << std::endl;

        // MEXC spot public streams are protobuf and can exceed 64KB; using a larger receive buffer
        // reduces fragmentation and prevents decode stalls/crashes behind some proxies.
        std::vector<unsigned char> buffer(256 * 1024);
        std::vector<unsigned char> binBuffer;
        binBuffer.reserve(256 * 1024);
        std::string textBuffer;
        textBuffer.reserve(16 * 1024);
        std::uint64_t unknownBinaryFrames = 0;
        auto lastEmit = std::chrono::steady_clock::now();

        for (;;)
        {
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            HRESULT hr =
                WinHttpWebSocketReceive(rawSocket, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
            if (FAILED(hr))
            {
                std::cerr << "[backend] WebSocket receive failed: " << std::hex << hr << std::dec << std::endl;
                break;
            }

            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                std::cerr << "[backend] ws closed by server" << std::endl;
                break;
            }

            if (received == 0)
            {
                continue;
            }

            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                // WinHTTP may deliver JSON frames split into fragments; assemble before parsing.
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                {
                    textBuffer.append(reinterpret_cast<const char*>(buffer.data()), received);
                    if (textBuffer.size() > 1024 * 1024)
                    {
                        std::cerr << "[backend] WS text fragment buffer too large, dropping\n";
                        textBuffer.clear();
                    }
                    continue;
                }

                std::string text;
                if (!textBuffer.empty())
                {
                    textBuffer.append(reinterpret_cast<const char*>(buffer.data()), received);
                    text.swap(textBuffer);
                    textBuffer.clear();
                }
                else
                {
                    text.assign(reinterpret_cast<const char*>(buffer.data()), received);
                }
                // PING / служебные сообщения
                try
                {
                    auto j = json::parse(text);
                    const auto methodIt = j.find("method");
                    if (methodIt != j.end() && methodIt->is_string() && *methodIt == "PING")
                    {
                        const std::string pong = R"({"method":"PONG"})";
                        WinHttpWebSocketSend(rawSocket,
                                             WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                             (void*) pong.data(),
                                             static_cast<DWORD>(pong.size()));
                    }
                    else
                    {
                        std::cerr << "[backend] control: " << text << std::endl;
                    }
                }
                catch (...)
                {
                    std::cerr << "[backend] text frame: " << text << std::endl;
                }
                continue;
            }

            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
            {
                try
                {
                    // WinHTTP may deliver protobuf frames split into fragments; assemble before decoding.
                    if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE)
                    {
                        binBuffer.insert(binBuffer.end(), buffer.begin(), buffer.begin() + received);
                        if (binBuffer.size() > 4 * 1024 * 1024)
                        {
                            std::cerr << "[backend] WS binary fragment buffer too large, dropping\n";
                            binBuffer.clear();
                        }
                        continue;
                    }
                    const double tickSize = book.tickSize();
                    if (tickSize <= 0.0)
                    {
                        continue;
                    }

                    std::string channelName;
                    std::vector<std::pair<dom::OrderBook::Tick, double>> asks;
                    std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
                    std::vector<PublicAggreDeal> deals;

                    const unsigned char* payloadPtr = buffer.data();
                    std::size_t payloadSize = static_cast<std::size_t>(received);
                    if (!binBuffer.empty())
                    {
                        binBuffer.insert(binBuffer.end(), buffer.begin(), buffer.begin() + received);
                        payloadPtr = binBuffer.data();
                        payloadSize = binBuffer.size();
                    }

                    // Try trades first
                    if (parseDealsFromWrapper(payloadPtr, payloadSize, channelName, deals))
                    {
                        for (const auto& d : deals)
                        {
                            json t;
                            t["type"] = "trade";
                            t["symbol"] = config.symbol;
                            dom::OrderBook::Tick tick = 0;
                            double snappedPrice = d.price;
                            if (quantizeTickFromPrice(d.price, tickSize, tick, snappedPrice))
                            {
                                t["tick"] = tick;
                                t["price"] = snappedPrice;
                            }
                            else
                            {
                                t["price"] = d.price;
                            }
                            t["qty"] = d.quantity;
                            t["side"] = d.buy ? "buy" : "sell";
                            t["timestamp"] = d.time;
                            tradeBatcher().add(config.symbol, std::move(t));
                        }
                        binBuffer.clear();
                        continue;
                    }

                    // Depth updates
                    if (parsePushWrapper(payloadPtr, payloadSize, channelName, tickSize, asks, bids))
                    {
                        const auto now = std::chrono::steady_clock::now();
                        std::lock_guard<std::mutex> lock(g_bookMutex);
                        book.applyDelta(bids, asks, config.cacheLevelsPerSide);
                        if (now - lastEmit >= config.throttle)
                        {
                            lastEmit = now;
                            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch())
                                                   .count();
                            emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                        }
                    }
                    else
                    {
                        // If the wrapper isn't recognized, emit a very low-frequency hint to stderr for debugging.
                        // (Some proxies can mangle frames; also MEXC can occasionally send other channels.)
                        ++unknownBinaryFrames;
                        if (unknownBinaryFrames <= 3 || (unknownBinaryFrames % 5000) == 0)
                        {
                            std::cerr << "[backend] mexc unknown binary frame len=" << payloadSize
                                      << " count=" << unknownBinaryFrames << std::endl;
                        }
                    }
                    binBuffer.clear();
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "[backend] decode/apply error: " << ex.what() << std::endl;
                    binBuffer.clear();
                }
            }
        }

        WinHttpCloseHandle(rawSocket);
        return true;
    }

    bool runMexcSpotJsonWebSocket(const Config& config, dom::OrderBook& book)
    {
        WinHttpHandle session = openSession(config);
        if (!session.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
            return false;
        }

        const std::wstring host = L"wbs-api.mexc.com";
        const std::wstring path = L"/ws";

        WinHttpHandle connection(
            WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        if (!connection.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
            return false;
        }

        WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                                 L"GET",
                                                 path.c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE));
        if (!request.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
            return false;
        }

        applyProxyCredentials(config, request.get());
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSetOption") << std::endl;
            return false;
        }

        if (!WinHttpSendRequest(request.get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
            return false;
        }

        if (!WinHttpReceiveResponse(request.get(), nullptr))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
            return false;
        }

        HINTERNET rawSocket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
        request.reset();
        if (!rawSocket)
        {
            std::cerr << "[backend] " << winhttpError("WinHttpWebSocketCompleteUpgrade") << std::endl;
            return false;
        }

        // Subscribe to JSON channels (no protobuf).
        // Use the same channel names as protobuf, but without the `.pb` suffix.
        std::ostringstream depthChannel;
        depthChannel << "spot@public.aggre.depth.v3.api@" << config.mexcStreamIntervalMs << "ms@" << config.symbol;
        std::ostringstream dealsChannel;
        dealsChannel << "spot@public.aggre.deals.v3.api@" << config.mexcStreamIntervalMs << "ms@" << config.symbol;
        json sub = {{"method", "SUBSCRIPTION"},
                    {"params", json::array({depthChannel.str(), dealsChannel.str()})}};
        const std::string subStr = sub.dump();

        if (WinHttpWebSocketSend(rawSocket,
                                 WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 (void*)subStr.data(),
                                 static_cast<DWORD>(subStr.size())) != S_OK)
        {
            std::cerr << "[backend] failed to send SUBSCRIPTION" << std::endl;
            WinHttpCloseHandle(rawSocket);
            return false;
        }
        std::cerr << "[backend] sent " << subStr << std::endl;

        std::vector<unsigned char> buffer(128 * 1024);
        std::string textBuffer;
        textBuffer.reserve(16 * 1024);
        auto lastEmit = std::chrono::steady_clock::now();

        auto parseSide = [&](const json& side, std::vector<std::pair<dom::OrderBook::Tick, double>>& out) {
            out.clear();
            if (!side.is_array())
            {
                return;
            }
            out.reserve(side.size());
            const double tickSize = book.tickSize();
            for (const auto& lvl : side)
            {
                if (!lvl.is_array() || lvl.size() < 2) continue;
                const double price = jsonToDouble(lvl[0]);
                const double qty = jsonToDouble(lvl[1]);
                if (!(price > 0.0) || !(qty >= 0.0)) continue;
                const auto tick = tickFromPrice(price, tickSize);
                out.emplace_back(tick, qty);
            }
        };

        auto handleDepthOrTrades = [&](const json& j) {
            // Common envelopes:
            // - { "c": "<channel>", "d": <data> }
            // - { "channel": "<channel>", "data": <data> }
            // - { "stream": "<channel>", "data": <data> }  (binance-style)
            std::string channel;
            const json* dataPtr = nullptr;

            auto takeStr = [&](const char* k) -> std::optional<std::string> {
                auto it = j.find(k);
                if (it != j.end() && it->is_string()) return it->get<std::string>();
                return std::nullopt;
            };

            if (auto c = takeStr("c")) channel = *c;
            else if (auto c2 = takeStr("channel")) channel = *c2;
            else if (auto s = takeStr("stream")) channel = *s;

            if (auto it = j.find("d"); it != j.end()) dataPtr = &(*it);
            else if (auto it = j.find("data"); it != j.end()) dataPtr = &(*it);

            if (!dataPtr)
            {
                return;
            }
            const json& data = *dataPtr;

            // Depth
            if (data.is_object() && (data.contains("bids") || data.contains("asks") || data.contains("b") || data.contains("a")))
            {
                const json bidsSide = data.contains("bids") ? data["bids"]
                                   : data.contains("b") ? data["b"]
                                                        : json::array();
                const json asksSide = data.contains("asks") ? data["asks"]
                                   : data.contains("a") ? data["a"]
                                                        : json::array();
                std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
                std::vector<std::pair<dom::OrderBook::Tick, double>> asks;
                parseSide(bidsSide, bids);
                parseSide(asksSide, asks);

                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(g_bookMutex);
                book.applyDelta(bids, asks, config.cacheLevelsPerSide);
                if (now - lastEmit >= config.throttle)
                {
                    lastEmit = now;
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                    emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                }
                return;
            }

            // Trades / deals
            auto emitTrade = [&](double price, double qty, bool isBuy, std::int64_t ts) {
                json t;
                t["type"] = "trade";
                t["symbol"] = config.symbol;
                t["price"] = price;
                t["qty"] = qty;
                t["side"] = isBuy ? "buy" : "sell";
                t["timestamp"] = ts;
                tradeBatcher().add(config.symbol, std::move(t));
            };

            auto parseDealObj = [&](const json& d) {
                if (!d.is_object()) return;
                const double price = d.contains("p") ? jsonToDouble(d["p"])
                                    : d.contains("price") ? jsonToDouble(d["price"])
                                                      : 0.0;
                const double qty = d.contains("v") ? jsonToDouble(d["v"])
                                  : d.contains("q") ? jsonToDouble(d["q"])
                                  : d.contains("qty") ? jsonToDouble(d["qty"])
                                                  : 0.0;
                if (!(price > 0.0) || !(qty > 0.0)) return;

                std::int64_t ts = 0;
                if (d.contains("t")) ts = static_cast<std::int64_t>(jsonToDouble(d["t"]));
                else if (d.contains("ts")) ts = static_cast<std::int64_t>(jsonToDouble(d["ts"]));
                else if (d.contains("time")) ts = static_cast<std::int64_t>(jsonToDouble(d["time"]));
                if (ts <= 0)
                {
                    ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
                }

                bool isBuy = true;
                if (d.contains("S"))
                {
                    const int s = static_cast<int>(jsonToDouble(d["S"]));
                    // Common: 1=BUY, 2=SELL
                    isBuy = (s == 1);
                }
                else if (d.contains("side") && d["side"].is_string())
                {
                    const std::string side = toLowerAscii(d["side"].get<std::string>());
                    isBuy = (side == "buy" || side == "bid");
                }
                emitTrade(price, qty, isBuy, ts);
            };

            if (data.is_object() && data.contains("deals") && data["deals"].is_array())
            {
                for (const auto& d : data["deals"])
                {
                    parseDealObj(d);
                }
                return;
            }
            if (data.is_array())
            {
                for (const auto& d : data)
                {
                    parseDealObj(d);
                }
                return;
            }

            (void)channel;
        };

        for (;;)
        {
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            HRESULT hr =
                WinHttpWebSocketReceive(rawSocket, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
            if (FAILED(hr))
            {
                std::cerr << "[backend] WebSocket receive failed: " << std::hex << hr << std::dec << std::endl;
                break;
            }

            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                std::cerr << "[backend] ws closed by server" << std::endl;
                break;
            }
            if (received == 0) continue;

            if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                // JSON WS should be UTF8; ignore other frames.
                continue;
            }

            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                textBuffer.append(reinterpret_cast<const char*>(buffer.data()), received);
                if (textBuffer.size() > 1024 * 1024)
                {
                    std::cerr << "[backend] WS text fragment buffer too large, dropping\n";
                    textBuffer.clear();
                }
                continue;
            }

            std::string text;
            if (!textBuffer.empty())
            {
                textBuffer.append(reinterpret_cast<const char*>(buffer.data()), received);
                text.swap(textBuffer);
                textBuffer.clear();
            }
            else
            {
                text.assign(reinterpret_cast<const char*>(buffer.data()), received);
            }

            try
            {
                auto j = json::parse(text);
                const auto methodIt = j.find("method");
                if (methodIt != j.end() && methodIt->is_string() && *methodIt == "PING")
                {
                    const std::string pong = R"({"method":"PONG"})";
                    WinHttpWebSocketSend(rawSocket,
                                         WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                         (void*)pong.data(),
                                         static_cast<DWORD>(pong.size()));
                    continue;
                }

                // Server acks / errors.
                if (j.contains("code") && j["code"].is_number_integer() && j["code"].get<int>() != 0)
                {
                    std::cerr << "[backend] mexc ws error: " << text << std::endl;
                }

                handleDepthOrTrades(j);
            }
            catch (const std::exception& ex)
            {
                std::cerr << "[backend] mexc ws json parse/apply error: " << ex.what() << std::endl;
            }
        }

        WinHttpCloseHandle(rawSocket);
        return true;
    }

    bool runMexcFuturesWebSocket(const Config &config, dom::OrderBook &book)
    {
        WinHttpHandle session = openSession(config);
        if (!session.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
            return false;
        }

        const std::wstring host = L"contract.mexc.com";
        const std::wstring path = L"/edge";
        std::vector<unsigned char> buffer(128 * 1024);

        for (;;)
        {
            WinHttpHandle connection(
                WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
            if (!connection.valid())
            {
                std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
                std::this_thread::sleep_for(1000ms);
                continue;
            }

            WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                                     L"GET",
                                                     path.c_str(),
                                                     nullptr,
                                                     WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                     WINHTTP_FLAG_SECURE));
            if (!request.valid())
            {
                std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
                std::this_thread::sleep_for(1000ms);
                continue;
            }
            applyProxyCredentials(config, request.get());
            if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            {
                std::cerr << "[backend] " << winhttpError("WinHttpSetOption") << std::endl;
                std::this_thread::sleep_for(1000ms);
                continue;
            }
            if (!WinHttpSendRequest(request.get(),
                                    WINHTTP_NO_ADDITIONAL_HEADERS,
                                    0,
                                    WINHTTP_NO_REQUEST_DATA,
                                    0,
                                    0,
                                    0))
            {
                std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
                std::this_thread::sleep_for(1000ms);
                continue;
            }
            if (!WinHttpReceiveResponse(request.get(), nullptr))
            {
                std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
                std::this_thread::sleep_for(1000ms);
                continue;
            }

            HINTERNET rawSocket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
            if (!rawSocket)
            {
                std::cerr << "[backend] " << winhttpError("WinHttpWebSocketCompleteUpgrade") << std::endl;
                std::this_thread::sleep_for(1000ms);
                continue;
            }
            request.reset();

            std::mutex sendMutex;
            auto sendJson = [&](const json &msg) -> bool {
                const std::string payload = msg.dump();
                std::lock_guard<std::mutex> lock(sendMutex);
                return WinHttpWebSocketSend(rawSocket,
                                            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                            (void*)payload.data(),
                                            static_cast<DWORD>(payload.size())) == S_OK;
            };

            const int depthLimit =
                std::max(50, static_cast<int>(
                                 std::max<std::size_t>(config.ladderLevelsPerSide,
                                                       config.cacheLevelsPerSide)));
            json depthSub = {{"method","sub.depth"},
                             {"param", {{"symbol", config.symbol},
                                        {"limit", depthLimit}}}};
            json dealSub = {{"method","sub.deal"}, {"param", {{"symbol", config.symbol}}}};
            sendJson(depthSub);
            sendJson(dealSub);

            std::atomic<bool> running{true};
            std::thread pingThread([&]() {
                while (running.load())
                {
                    std::this_thread::sleep_for(45s);
                    if (!running.load())
                    {
                        break;
                    }
                    json ping = {{"method","ping"}};
                    if (!sendJson(ping))
                    {
                        // Socket likely closed; receiver loop will reconnect.
                        break;
                    }
                }
            });

            auto lastEmit = std::chrono::steady_clock::now();
            bool shouldReconnect = false;
            std::string textBuffer;
            textBuffer.reserve(64 * 1024);

            while (true)
            {
                DWORD received = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
                HRESULT hr = WinHttpWebSocketReceive(rawSocket,
                                                     buffer.data(),
                                                     static_cast<DWORD>(buffer.size()),
                                                     &received,
                                                     &type);
                if (FAILED(hr))
                {
                    std::cerr << "[backend] futures WS receive failed: 0x" << std::hex << hr << std::dec << std::endl;
                    shouldReconnect = true;
                    break;
                }
                if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                {
                    std::cerr << "[backend] futures WS closed by server" << std::endl;
                    shouldReconnect = true;
                    break;
                }
                if (received == 0)
                {
                    continue;
                }
                if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
                    type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                {
                    continue;
                }

                // WinHTTP may deliver JSON frames split into fragments; assemble before parsing.
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
                {
                    textBuffer.append(reinterpret_cast<const char*>(buffer.data()), received);
                    if (textBuffer.size() > 1024 * 1024)
                    {
                        std::cerr << "[backend] futures WS fragment buffer too large, dropping\n";
                        textBuffer.clear();
                    }
                    continue;
                }

                std::string text;
                if (!textBuffer.empty())
                {
                    textBuffer.append(reinterpret_cast<const char*>(buffer.data()), received);
                    text.swap(textBuffer);
                    textBuffer.clear();
                }
                else
                {
                    text.assign(reinterpret_cast<const char*>(buffer.data()), received);
                }
                json message;
                try
                {
                    message = json::parse(text);
                }
                catch (const std::exception &ex)
                {
                    std::cerr << "[backend] futures WS parse error: " << ex.what() << " payload=" << text << std::endl;
                    continue;
                }

                const std::string channel = message.value("channel", std::string());
                if (channel.empty())
                {
                    // Some servers still send method-based ping frames.
                    const auto methodIt = message.find("method");
                    if (methodIt != message.end() && methodIt->is_string())
                    {
                        const std::string method = methodIt->get<std::string>();
                        if (method == "PING")
                        {
                            json pong = {{"method","PONG"}};
                            sendJson(pong);
                        }
                        else if (method == "ping")
                        {
                            json pong = {{"method","pong"}};
                            sendJson(pong);
                        }
                    }
                    continue;
                }

                if (channel == "pong" || channel == "rs.pong")
                {
                    continue;
                }
                if (channel == "rs.error")
                {
                    std::cerr << "[backend] futures WS error: " << text << std::endl;
                    // Typical kick reason: no keepalive. Reconnect immediately.
                    shouldReconnect = true;
                    break;
                }
                if (channel == "push.depth")
                {
                    const json data = message.value("data", json::object());
                    const double tickSize = book.tickSize();
                    if (tickSize <= 0.0 || !data.is_object())
                    {
                        continue;
                    }
                    const double contractSize = config.futuresContractSize > 0.0 ? config.futuresContractSize : 1.0;
                    std::vector<std::pair<dom::OrderBook::Tick,double>> bids;
                    std::vector<std::pair<dom::OrderBook::Tick,double>> asks;
                    auto parseSide = [&](const json &side,
                                         std::vector<std::pair<dom::OrderBook::Tick,double>> &out) {
                        out.clear();
                        if (!side.is_array())
                        {
                            return;
                        }
                        for (const auto &row : side)
                        {
                            if (row.is_array() && !row.empty())
                            {
                                const double price = row.size() > 0 ? jsonToDouble(row[0]) : 0.0;
                                double qty = row.size() > 1 ? jsonToDouble(row[1]) : 0.0;
                                if (price <= 0.0 || qty < 0.0)
                                {
                                    continue;
                                }
                                qty *= contractSize;
                                const auto tick = tickFromPrice(price, tickSize);
                                out.emplace_back(tick, qty);
                            }
                        }
                    };
                    parseSide(data.value("bids", json::array()), bids);
                    parseSide(data.value("asks", json::array()), asks);
                    if (!bids.empty() || !asks.empty())
                    {
                        std::lock_guard<std::mutex> lock(g_bookMutex);
                        book.applyDelta(bids, asks, config.cacheLevelsPerSide);
                        const auto now = std::chrono::steady_clock::now();
                        if (now - lastEmit >= config.throttle)
                        {
                            lastEmit = now;
                            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch())
                                                   .count();
                            emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                        }
                    }
                    continue;
                }
                if (channel == "push.deal")
                {
                    const json deals = message.value("data", json::array());
                    if (!deals.is_array())
                    {
                        continue;
                    }
                    const double tickSize = book.tickSize();
                    const double contractSize = config.futuresContractSize > 0.0 ? config.futuresContractSize : 1.0;
                    for (const auto &d : deals)
                    {
                        const double price = jsonToDouble(d.value("p", json(0.0)));
                        double qty = jsonToDouble(d.value("v", json(0.0)));
                        if (price <= 0.0 || qty <= 0.0)
                        {
                            continue;
                        }
                        qty *= contractSize;
                        json t;
                        t["type"] = "trade";
                        t["symbol"] = config.symbol;
                        dom::OrderBook::Tick tick = 0;
                        double snappedPrice = price;
                        if (quantizeTickFromPrice(price, tickSize, tick, snappedPrice))
                        {
                            t["tick"] = tick;
                            t["price"] = snappedPrice;
                        }
                        else
                        {
                            t["price"] = price;
                        }
                        t["qty"] = qty;
                        const int sideCode = d.value("T", 1);
                        t["side"] = (sideCode == 2) ? "sell" : "buy";
                        t["timestamp"] = d.value("t", 0);
                        tradeBatcher().add(config.symbol, std::move(t));
                    }
                    continue;
                }
            }

            running.store(false);
            if (pingThread.joinable())
            {
                pingThread.join();
            }
            WinHttpCloseHandle(rawSocket);

            if (!shouldReconnect)
            {
                break;
            }
            std::this_thread::sleep_for(500ms);
        }

        return true;
    }
} // namespace

bool fetchUzxSnapshot(const Config& config, dom::OrderBook& book, double& tickSizeOut, bool isSwap)
{
    const std::string host = "api-v2.uzx.com";
    std::ostringstream path;
    path << "/notification/" << (isSwap ? "swap" : "spot") << "/" << config.symbol << "/orderbook";
    auto body = httpGet(config, host, path.str(), true);
    if (!body)
    {
        std::cerr << "[backend] uzx snapshot failed\n";
        return false;
    }
    json doc;
    try
    {
        doc = json::parse(*body);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[backend] uzx snapshot parse error: " << ex.what() << std::endl;
        return false;
    }

    auto parseBookSide = [](const json& arr, std::vector<std::pair<dom::OrderBook::Tick, double>>& out, double tickSize) {
        out.clear();
        for (const auto& lvl : arr)
        {
            if (!lvl.is_array() || lvl.size() < 2) continue;
            const std::string priceStr = lvl[0].get<std::string>();
            const std::string qtyStr = lvl[1].get<std::string>();
            const double price = std::atof(priceStr.c_str());
            const double qty = std::atof(qtyStr.c_str());
            if (price <= 0.0 || qty <= 0.0 || tickSize <= 0.0) continue;
            const auto tick = tickFromPrice(price, tickSize);
            out.emplace_back(tick, qty);
        }
    };

    const auto bidsArr = doc["data"]["bids"];
    const auto asksArr = doc["data"]["asks"];

    // Derive tick size from adjacent price deltas rather than decimal count.
    // UZX often returns prices with fixed decimals (e.g. 8) even when the true tick is larger,
    // so "count decimals" can yield a wildly wrong tick size and break the ladder scale.
    tickSizeOut = tickSizeOut > 0.0 ? tickSizeOut : 0.0;
    auto inferTickFromSide = [](const json& side) -> double {
        if (!side.is_array())
        {
            return 0.0;
        }
        std::vector<double> prices;
        prices.reserve(64);
        for (const auto& lvl : side)
        {
            if (!lvl.is_array() || lvl.size() < 1) continue;
            const double price = jsonToDouble(lvl[0]);
            if (price > 0.0 && std::isfinite(price))
            {
                prices.push_back(price);
                if (prices.size() >= 64) break;
            }
        }
        if (prices.size() < 2)
        {
            return 0.0;
        }
        std::sort(prices.begin(), prices.end());
        prices.erase(std::unique(prices.begin(), prices.end()), prices.end());
        if (prices.size() < 2)
        {
            return 0.0;
        }
        double best = 0.0;
        for (std::size_t i = 1; i < prices.size(); ++i)
        {
            const double d = prices[i] - prices[i - 1];
            if (!(d > 0.0) || !std::isfinite(d)) continue;
            if (d < 1e-12) continue;
            if (best <= 0.0 || d < best)
            {
                best = d;
            }
        }
        if (!(best > 0.0))
        {
            return 0.0;
        }
        // Quantize to avoid floating drift (1e-12 granularity is enough for our UI).
        best = std::round(best * 1e12) / 1e12;
        return (best > 0.0) ? best : 0.0;
    };
    auto detectTickByDecimals = [](const json& side) -> double {
        if (!side.is_array())
        {
            return 0.0;
        }
        for (const auto& lvl : side)
        {
            if (!lvl.is_array() || lvl.size() < 1) continue;
            if (!lvl[0].is_string()) continue;
            const std::string priceStr = lvl[0].get<std::string>();
            auto pos = priceStr.find('.');
            if (pos == std::string::npos)
            {
                continue;
            }
            const int decimals = static_cast<int>(priceStr.size() - pos - 1);
            if (decimals > 0 && decimals < 18)
            {
                return std::pow(10.0, -decimals);
            }
        }
        return 0.0;
    };
    if (tickSizeOut <= 0.0)
    {
        const double bidTick = inferTickFromSide(bidsArr);
        const double askTick = inferTickFromSide(asksArr);
        if (bidTick > 0.0 && askTick > 0.0)
        {
            tickSizeOut = std::min(bidTick, askTick);
        }
        else if (bidTick > 0.0)
        {
            tickSizeOut = bidTick;
        }
        else if (askTick > 0.0)
        {
            tickSizeOut = askTick;
        }
        if (tickSizeOut <= 0.0)
        {
            tickSizeOut = detectTickByDecimals(bidsArr);
            if (tickSizeOut <= 0.0)
            {
                tickSizeOut = detectTickByDecimals(asksArr);
            }
        }
    }

    if (tickSizeOut <= 0.0)
    {
        tickSizeOut = 0.0001;
    }
    book.setTickSize(tickSizeOut);

    std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
    std::vector<std::pair<dom::OrderBook::Tick, double>> asks;
    parseBookSide(bidsArr, bids, tickSizeOut);
    parseBookSide(asksArr, asks, tickSizeOut);
    book.loadSnapshot(bids, asks);
    return true;
}

bool fetchBinanceExchangeInfoSpot(const Config &cfg, double &tickSizeOut)
{
    const std::string symbol = normalizeBinanceSymbol(cfg.symbol);
    std::ostringstream path;
    path << "/api/v3/exchangeInfo?symbol=" << symbol;

    auto body = httpGet(cfg, "api.binance.com", path.str(), true);
    if (!body)
    {
        std::cerr << "[backend] binance exchangeInfo fetch failed" << std::endl;
        return false;
    }

    json j;
    try
    {
        j = json::parse(*body);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "[backend] binance exchangeInfo JSON parse error: " << ex.what() << std::endl;
        return false;
    }

    if (!j.contains("symbols") || !j["symbols"].is_array() || j["symbols"].empty())
    {
        std::cerr << "[backend] binance exchangeInfo: no symbols array" << std::endl;
        return false;
    }
    const auto &sym = j["symbols"].front();
    double tickSize = 0.0;
    if (sym.contains("filters") && sym["filters"].is_array())
    {
        for (const auto &f : sym["filters"])
        {
            const std::string type = f.value("filterType", std::string());
            if (type == "PRICE_FILTER")
            {
                tickSize = jsonToDouble(f.value("tickSize", json(0.0)));
                if (tickSize > 0.0)
                {
                    break;
                }
            }
        }
    }
    tickSizeOut = tickSize;
    std::cerr << "[backend] binance exchangeInfo: tickSize=" << tickSizeOut << std::endl;
    return tickSizeOut > 0.0;
}

bool fetchBinanceExchangeInfoFutures(const Config &cfg, double &tickSizeOut)
{
    const std::string symbol = normalizeBinanceSymbol(cfg.symbol);
    std::ostringstream path;
    path << "/fapi/v1/exchangeInfo?symbol=" << symbol;

    auto body = httpGet(cfg, "fapi.binance.com", path.str(), true);
    if (!body)
    {
        std::cerr << "[backend] binance futures exchangeInfo fetch failed" << std::endl;
        return false;
    }

    json j;
    try
    {
        j = json::parse(*body);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "[backend] binance futures exchangeInfo JSON parse error: " << ex.what() << std::endl;
        return false;
    }

    if (!j.contains("symbols") || !j["symbols"].is_array() || j["symbols"].empty())
    {
        std::cerr << "[backend] binance futures exchangeInfo: no symbols array" << std::endl;
        return false;
    }

    // Binance USDT-M futures may ignore the `symbol=` query param and return the full list.
    // Do not assume `symbols[0]` matches the requested instrument (it is often BTCUSDT).
    const json *symPtr = nullptr;
    for (const auto &entry : j["symbols"])
    {
        if (!entry.is_object()) continue;
        const std::string s = entry.value("symbol", std::string());
        if (s == symbol)
        {
            symPtr = &entry;
            break;
        }
    }
    if (!symPtr)
    {
        std::cerr << "[backend] binance futures exchangeInfo: symbol not found: " << symbol << std::endl;
        symPtr = &j["symbols"].front();
    }
    const auto &sym = *symPtr;
    double tickSize = 0.0;
    if (sym.contains("filters") && sym["filters"].is_array())
    {
        for (const auto &f : sym["filters"])
        {
            const std::string type = f.value("filterType", std::string());
            if (type == "PRICE_FILTER")
            {
                tickSize = jsonToDouble(f.value("tickSize", json(0.0)));
                if (tickSize > 0.0)
                {
                    break;
                }
            }
        }
    }
    tickSizeOut = tickSize;
    std::cerr << "[backend] binance futures exchangeInfo: tickSize=" << tickSizeOut << std::endl;
    return tickSizeOut > 0.0;
}

bool fetchBinanceSnapshotSpot(const Config &cfg, double tickSize, BinanceDepthSnapshot &out)
{
    if (tickSize <= 0.0)
    {
        std::cerr << "[backend] binance snapshot: tickSize missing" << std::endl;
        return false;
    }

    const std::string symbol = normalizeBinanceSymbol(cfg.symbol);
    std::ostringstream path;
    path << "/api/v3/depth?symbol=" << symbol << "&limit=" << cfg.snapshotDepth;

    auto body = httpGet(cfg, "api.binance.com", path.str(), true);
    if (!body)
    {
        std::cerr << "[backend] binance depth fetch failed" << std::endl;
        return false;
    }

    json j;
    try
    {
        j = json::parse(*body);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "[backend] binance depth JSON parse error: " << ex.what() << std::endl;
        return false;
    }

    auto parseSide = [tickSize](const json &arr,
                                std::vector<std::pair<dom::OrderBook::Tick, double>> &out) {
        out.clear();
        if (!arr.is_array())
        {
            return;
        }
        out.reserve(arr.size());
        for (const auto &e : arr)
        {
            if (!e.is_array() || e.size() < 2) continue;
            const double price = jsonToDouble(e[0]);
            const double qty = jsonToDouble(e[1]);
            const auto tick = tickFromPrice(price, tickSize);
            out.emplace_back(tick, qty);
        }
    };

    out.lastUpdateId = j.value("lastUpdateId", 0LL);
    parseSide(j.value("bids", json::array()), out.bids);
    parseSide(j.value("asks", json::array()), out.asks);

    std::cerr << "[backend] binance snapshot loaded: bids=" << out.bids.size()
              << " asks=" << out.asks.size() << " lastUpdateId=" << out.lastUpdateId << std::endl;
    return out.lastUpdateId > 0;
}

bool fetchBinanceSnapshotFutures(const Config &cfg, double tickSize, BinanceDepthSnapshot &out)
{
    if (tickSize <= 0.0)
    {
        std::cerr << "[backend] binance futures snapshot: tickSize missing" << std::endl;
        return false;
    }

    const std::string symbol = normalizeBinanceSymbol(cfg.symbol);
    std::ostringstream path;
    path << "/fapi/v1/depth?symbol=" << symbol << "&limit=" << cfg.snapshotDepth;

    auto body = httpGet(cfg, "fapi.binance.com", path.str(), true);
    if (!body)
    {
        std::cerr << "[backend] binance futures depth fetch failed" << std::endl;
        return false;
    }

    json j;
    try
    {
        j = json::parse(*body);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "[backend] binance futures depth JSON parse error: " << ex.what() << std::endl;
        return false;
    }

    auto parseSide = [tickSize](const json &arr,
                                std::vector<std::pair<dom::OrderBook::Tick, double>> &out) {
        out.clear();
        if (!arr.is_array())
        {
            return;
        }
        out.reserve(arr.size());
        for (const auto &e : arr)
        {
            if (!e.is_array() || e.size() < 2) continue;
            const double price = jsonToDouble(e[0]);
            const double qty = jsonToDouble(e[1]);
            const auto tick = tickFromPrice(price, tickSize);
            out.emplace_back(tick, qty);
        }
    };

    out.lastUpdateId = j.value("lastUpdateId", 0LL);
    parseSide(j.value("bids", json::array()), out.bids);
    parseSide(j.value("asks", json::array()), out.asks);

    std::cerr << "[backend] binance futures snapshot loaded: bids=" << out.bids.size()
              << " asks=" << out.asks.size() << " lastUpdateId=" << out.lastUpdateId << std::endl;
    return out.lastUpdateId > 0;
}

bool runBinanceWebSocket(const Config &config, dom::OrderBook &book, bool futures, long long snapshotLastUpdateId)
{
    WinHttpHandle session = openSession(config);
    if (!session.valid())
    {
        std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
        return false;
    }

    const std::wstring host = futures ? L"fstream.binance.com" : L"stream.binance.com";
    const INTERNET_PORT port = futures ? INTERNET_DEFAULT_HTTPS_PORT : 9443;
    const std::wstring path = L"/ws";

    const std::string symbolLower = [&]() {
        std::string s = normalizeBinanceSymbol(config.symbol);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return s;
    }();

    auto lastUpdateId = snapshotLastUpdateId;
    bool synced = false;
    auto lastResyncAttempt = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    auto resyncSnapshot = [&]() -> bool {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastResyncAttempt < std::chrono::seconds(1))
        {
            return false;
        }
        lastResyncAttempt = now;

        BinanceDepthSnapshot snap;
        const double tickSize = book.tickSize();
        const bool ok =
            futures ? fetchBinanceSnapshotFutures(config, tickSize, snap)
                    : fetchBinanceSnapshotSpot(config, tickSize, snap);
        if (!ok)
        {
            std::cerr << "[backend] binance resync snapshot failed" << std::endl;
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_bookMutex);
            book.loadSnapshot(snap.bids, snap.asks);
        }
        lastUpdateId = snap.lastUpdateId;
        synced = false;
        std::cerr << "[backend] binance resynced: lastUpdateId=" << lastUpdateId << std::endl;
        return true;
    };

    for (;;)
    {
        WinHttpHandle connection(
            WinHttpConnect(session.get(), host.c_str(), port, 0));
        if (!connection.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
            return false;
        }

        WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                                 L"GET",
                                                 path.c_str(),
                                                 nullptr,
                                                 WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE));
        if (!request.valid())
        {
            std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
            return false;
        }

        applyProxyCredentials(config, request.get());
        if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSetOption") << std::endl;
            return false;
        }

        if (!WinHttpSendRequest(request.get(),
                                WINHTTP_NO_ADDITIONAL_HEADERS,
                                0,
                                WINHTTP_NO_REQUEST_DATA,
                                0,
                                0,
                                0))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
            return false;
        }

        if (!WinHttpReceiveResponse(request.get(), nullptr))
        {
            std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
            return false;
        }

        HINTERNET rawSocket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
        if (!rawSocket)
        {
            std::cerr << "[backend] " << winhttpError("WinHttpWebSocketCompleteUpgrade") << std::endl;
            return false;
        }
        request.reset();

        std::cerr << "[backend] connected to Binance ws" << (futures ? " (futures)" : " (spot)") << std::endl;

        if (lastUpdateId <= 0)
        {
            resyncSnapshot();
        }

        const std::string depthStream = symbolLower + "@depth@100ms";
        const std::string tradesStream = symbolLower + "@aggTrade";
        json sub = {{"method", "SUBSCRIBE"},
                    {"params", json::array({depthStream, tradesStream})},
                    {"id", 1}};
        const std::string subStr = sub.dump();
        if (WinHttpWebSocketSend(rawSocket,
                                 WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                 (void *)subStr.data(),
                                 static_cast<DWORD>(subStr.size())) != S_OK)
        {
            std::cerr << "[backend] failed to send Binance SUBSCRIBE" << std::endl;
            WinHttpCloseHandle(rawSocket);
            return false;
        }
        std::cerr << "[backend] sent " << subStr << std::endl;

        std::vector<unsigned char> buffer(256 * 1024);
        std::string fragmentBuffer;
        auto lastEmit = std::chrono::steady_clock::now();

        for (;;)
        {
            DWORD received = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            HRESULT hr = WinHttpWebSocketReceive(rawSocket,
                                                 buffer.data(),
                                                 static_cast<DWORD>(buffer.size()),
                                                 &received,
                                                 &type);
            if (FAILED(hr))
            {
                std::cerr << "[backend] Binance WS receive failed" << std::endl;
                break;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            {
                std::cerr << "[backend] Binance WS closed" << std::endl;
                break;
            }
            if (received == 0)
            {
                continue;
            }
            if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
                type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                continue;
            }

            const std::string text(reinterpret_cast<char *>(buffer.data()), received);
            if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            {
                fragmentBuffer.append(text);
                // Safety: avoid unbounded growth if we keep getting garbage fragments.
                if (fragmentBuffer.size() > (1024 * 1024))
                {
                    fragmentBuffer.clear();
                }
                continue;
            }

            std::string fullText;
            if (!fragmentBuffer.empty())
            {
                fragmentBuffer.append(text);
                fullText.swap(fragmentBuffer);
                fragmentBuffer.clear();
            }
            else
            {
                fullText = text;
            }
            json j;
            try
            {
                j = json::parse(fullText);
            }
            catch (...)
            {
                continue;
            }
            if (j.contains("result"))
            {
                continue;
            }
            const std::string event = j.value("e", std::string());
            if (event == "depthUpdate")
            {
                const double tickSize = book.tickSize();
                if (tickSize <= 0.0)
                {
                    continue;
                }

                const long long U = j.value("U", 0LL);
                const long long u = j.value("u", 0LL);
                if (lastUpdateId > 0 && (U <= 0 || u <= 0))
                {
                    continue;
                }

                if (lastUpdateId > 0 && !synced)
                {
                    // Drop events that are completely before our snapshot.
                    if (u <= lastUpdateId)
                    {
                        continue;
                    }
                    // First applied event must satisfy: U <= lastUpdateId+1 <= u
                    if (!(U <= lastUpdateId + 1 && u >= lastUpdateId + 1))
                    {
                        resyncSnapshot();
                        continue;
                    }
                    synced = true;
                }
                else if (lastUpdateId > 0 && synced)
                {
                    if (futures)
                    {
                        const long long pu = j.value("pu", 0LL);
                        if (pu != lastUpdateId)
                        {
                            resyncSnapshot();
                            continue;
                        }
                    }
                    else
                    {
                        if (U != lastUpdateId + 1)
                        {
                            // Missed updates; force snapshot resync.
                            resyncSnapshot();
                            continue;
                        }
                    }
                }

                std::vector<std::pair<dom::OrderBook::Tick, double>> bids;
                std::vector<std::pair<dom::OrderBook::Tick, double>> asks;
                auto parseSide = [tickSize](const json &arr,
                                            std::vector<std::pair<dom::OrderBook::Tick, double>> &out) {
                    out.clear();
                    if (!arr.is_array())
                    {
                        return;
                    }
                    out.reserve(arr.size());
                    for (const auto &e : arr)
                    {
                        if (!e.is_array() || e.size() < 2) continue;
                        const double price = jsonToDouble(e[0]);
                        const double qty = jsonToDouble(e[1]);
                        const auto tick = tickFromPrice(price, tickSize);
                        out.emplace_back(tick, qty);
                    }
                };
                parseSide(j.value("b", json::array()), bids);
                parseSide(j.value("a", json::array()), asks);

                const auto now = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(g_bookMutex);
                book.applyDelta(bids, asks, config.cacheLevelsPerSide);
                if (lastUpdateId > 0 && u > 0)
                {
                    lastUpdateId = u;
                }
                if (now - lastEmit >= config.throttle)
                {
                    lastEmit = now;
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                    emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                }
            }
            else if (event == "aggTrade")
            {
                const double tickSize = book.tickSize();
                if (tickSize <= 0.0)
                {
                    continue;
                }
                const double price = jsonToDouble(j.value("p", json(0.0)));
                const double qty = jsonToDouble(j.value("q", json(0.0)));
                const bool buyerIsMaker = j.value("m", false);
                const bool buy = !buyerIsMaker;
                const auto ts = j.value("T", j.value("E", 0LL));
                json t;
                t["type"] = "trade";
                t["symbol"] = config.symbol;
                dom::OrderBook::Tick tick = 0;
                double snappedPrice = price;
                if (quantizeTickFromPrice(price, tickSize, tick, snappedPrice))
                {
                    t["tick"] = tick;
                    t["price"] = snappedPrice;
                }
                else
                {
                    t["price"] = price;
                }
                t["qty"] = qty;
                t["side"] = buy ? "buy" : "sell";
                t["timestamp"] = ts;
                tradeBatcher().add(config.symbol, std::move(t));
            }
        }

        WinHttpWebSocketClose(rawSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(rawSocket);
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool runUzxWebSocket(const Config& config, dom::OrderBook& book, double tickSize, bool isSwap)
{
    const std::wstring host = L"stream.uzx.com";
    const std::wstring path = L"/notification/ws";

    WinHttpHandle session = openSession(config);
    if (!session.valid())
    {
        std::cerr << "[backend] " << winhttpError("WinHttpOpen") << std::endl;
        return false;
    }

    WinHttpHandle connection(
        WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection.valid())
    {
        std::cerr << "[backend] " << winhttpError("WinHttpConnect") << std::endl;
        return false;
    }

    WinHttpHandle request(WinHttpOpenRequest(connection.get(),
                                             L"GET",
                                             path.c_str(),
                                             nullptr,
                                             WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!request.valid())
    {
        std::cerr << "[backend] " << winhttpError("WinHttpOpenRequest") << std::endl;
        return false;
    }

    applyProxyCredentials(config, request.get());
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
    {
        std::cerr << "[backend] " << winhttpError("WinHttpSetOption") << std::endl;
        return false;
    }

    if (!WinHttpSendRequest(request.get(),
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0))
    {
        std::cerr << "[backend] " << winhttpError("WinHttpSendRequest") << std::endl;
        return false;
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr))
    {
        std::cerr << "[backend] " << winhttpError("WinHttpReceiveResponse") << std::endl;
        return false;
    }

    HINTERNET rawSocket = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
    if (!rawSocket)
    {
        std::cerr << "[backend] " << winhttpError("WinHttpWebSocketCompleteUpgrade") << std::endl;
        return false;
    }
    request.reset();

    auto normalizeSymbol = [](std::string symbol, bool swap) -> std::string {
        if (symbol.empty())
        {
            return symbol;
        }
        for (char& c : symbol)
        {
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        for (char& c : symbol)
        {
            if (c == '/') c = '-';
        }
        if (swap)
        {
            symbol.erase(std::remove(symbol.begin(), symbol.end(), '-'), symbol.end());
            return symbol;
        }
        if (symbol.find('-') != std::string::npos)
        {
            return symbol;
        }
        static const std::vector<std::string> quotes = {
            "USDT", "USDC", "USDR", "USDQ", "EURQ", "EURR", "BTC", "ETH"};
        for (const auto& q : quotes)
        {
            if (symbol.size() > q.size()
                && symbol.compare(symbol.size() - q.size(), q.size(), q) == 0)
            {
                const std::string base = symbol.substr(0, symbol.size() - q.size());
                if (!base.empty())
                {
                    return base + "-" + q;
                }
            }
        }
        return symbol;
    };

    const std::string channel = isSwap ? "swap.orderbook" : "spot.orderbook";
    const std::string biz = "market";
    const std::string symbol = normalizeSymbol(config.symbol, isSwap);

    // Seed the book and tick size via REST snapshot first. This avoids unstable start-up states
    // where the first WS frames don't carry enough precision to infer tick size reliably.
    {
        double snapTick = tickSize;
        if (!fetchUzxSnapshot(config, book, snapTick, isSwap))
        {
            std::cerr << "[backend] uzx snapshot failed, continuing with empty book" << std::endl;
        }
        if (snapTick > 0.0)
        {
            tickSize = snapTick;
        }
        if (book.tickSize() > 0.0 && book.bestBid() > 0.0 && book.bestAsk() > 0.0)
        {
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
            emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
        }
    }

    json sub = {{"event", "sub"},
                {"params",
                 {{"biz", biz}, {"type", channel}, {"symbol", symbol}, {"interval", "0"}}},
                {"zip", false}};
    const std::string subStr = sub.dump();
    WinHttpWebSocketSend(rawSocket,
                         WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                         (void*) subStr.data(),
                         static_cast<DWORD>(subStr.size()));
    const std::string fillsChannel = isSwap ? "swap.fills" : "spot.fills";
    auto sendSub = [&](const json &payload) {
        const std::string out = payload.dump();
        WinHttpWebSocketSend(rawSocket,
                             WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                             (void*) out.data(),
                             static_cast<DWORD>(out.size()));
    };
    json fillsSubMarket = {{"event", "sub"},
                           {"params",
                            {{"biz", "market"}, {"type", fillsChannel}, {"symbol", symbol}}},
                           {"zip", false}};
    sendSub(fillsSubMarket);
    json fillsSubScoped = {{"event", "sub"},
                           {"params",
                            {{"biz", isSwap ? "swap" : "spot"},
                             {"type", fillsChannel},
                             {"symbol", symbol}}},
                           {"zip", false}};
    sendSub(fillsSubScoped);

    std::vector<unsigned char> buffer(256 * 1024);
    auto lastEmit = std::chrono::steady_clock::now();

    auto detectTick = [](std::string_view priceStr) -> double {
        auto pos = priceStr.find('.');
        if (pos == std::string_view::npos)
        {
            return 0.0;
        }
        const int decimals = static_cast<int>(priceStr.size() - pos - 1);
        if (decimals <= 0 || decimals > 12) return 0.0;
        return std::pow(10.0, -decimals);
    };

    auto parseNumber = [](const json& value) -> double {
        if (value.is_string())
        {
            return std::atof(value.get<std::string>().c_str());
        }
        if (value.is_number())
        {
            return value.get<double>();
        }
        return 0.0;
    };

    const std::size_t maxLevelsPerSide =
        std::max<std::size_t>(1, config.ladderLevelsPerSide);
    auto parseSide = [&](const json& side, double& dynamicTickSize) {
        std::vector<std::pair<dom::OrderBook::Tick, double>> out;
        if (!side.is_array()) return out;
        std::size_t kept = 0;
        for (const auto& lvl : side)
        {
            if (!lvl.is_array() || lvl.size() < 2) continue;
            const std::string priceStr =
                lvl[0].is_string() ? lvl[0].get<std::string>() : std::string();
            const double price = parseNumber(lvl[0]);
            const double qty = parseNumber(lvl[1]);
            if (price <= 0.0 || qty <= 0.0) continue;
            if (dynamicTickSize <= 0.0)
            {
                double detected = detectTick(priceStr);
                if (detected > 0.0)
                {
                    dynamicTickSize = detected;
                    book.setTickSize(dynamicTickSize);
                }
            }
            if (dynamicTickSize <= 0.0) continue;
            const auto tick = static_cast<dom::OrderBook::Tick>(std::llround(price / dynamicTickSize));
            out.emplace_back(tick, qty);
            if (++kept >= maxLevelsPerSide)
            {
                break;
            }
        }
        return out;
    };

    std::string fragmentBuffer;

    for (;;)
    {
        DWORD received = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        HRESULT hr =
            WinHttpWebSocketReceive(rawSocket, buffer.data(), static_cast<DWORD>(buffer.size()), &received, &type);
        if (FAILED(hr))
        {
            std::cerr << "[backend] UZX ws receive failed: " << std::hex << hr << std::dec << std::endl;
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
        {
            std::cerr << "[backend] UZX ws closed by server\n";
            break;
        }
        if (received == 0) continue;
        if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE &&
            type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
        {
            continue;
        }

        // Accumulate only when the server actually fragments the frame. WinHTTP already
        // preserves message boundaries, so we only stitch together continuation fragments.
        const std::string_view chunk(reinterpret_cast<char*>(buffer.data()), received);
        const bool isFragment = (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE);
        if (isFragment)
        {
            fragmentBuffer.append(chunk.data(), chunk.size());
            if (fragmentBuffer.size() > 4 * 1024 * 1024)
            {
                std::cerr << "[backend] UZX ws fragment buffer too large, dropping\n";
                fragmentBuffer.clear();
            }
            continue;
        }

        std::string message;
        if (!fragmentBuffer.empty())
        {
            fragmentBuffer.append(chunk.data(), chunk.size());
            message.swap(fragmentBuffer);
        }
        else
        {
            message.assign(chunk);
        }

        if (message.empty())
        {
            continue;
        }

        auto processJson = [&](const std::string& text) {
            static const bool debugUzx = []() {
                const char* v = std::getenv("BACKEND_DEBUG_UZX");
                return v && *v && std::string(v) != "0";
            }();
            auto j = json::parse(text);
            if (j.contains("event") && j.contains("status"))
            {
                const std::string event = j.value("event", std::string());
                if (event == "sub" || event == "subscribe")
                {
                    if (debugUzx) {
                        std::cerr << "[backend] UZX ws subscribed: " << text << std::endl;
                    }
                }
            }
            if (debugUzx && text.find("fills") != std::string::npos)
            {
                std::cerr << "[backend] UZX fills raw: " << text << std::endl;
            }
            if (j.contains("ping"))
            {
                json pong = {{"pong", j["ping"]}};
                const std::string pongStr = pong.dump();
                WinHttpWebSocketSend(rawSocket,
                                     WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                     (void*) pongStr.data(),
                                     static_cast<DWORD>(pongStr.size()));
                return;
            }
            auto isFillsType = [](const std::string &t) {
                return t.find("fills") != std::string::npos;
            };
            auto emitFill = [&](const json &fill, long long parentTs) {
                if (!fill.is_object()) {
                    return;
                }
                const double price = parseNumber(fill.value("price", json(0.0)));
                const double qtyBase = parseNumber(fill.value("vol", json(0.0)));
                if (price <= 0.0 || qtyBase <= 0.0) {
                    return;
                }
                json t;
                t["type"] = "trade";
                t["symbol"] = config.symbol;
                dom::OrderBook::Tick tick = 0;
                double snappedPrice = price;
                if (quantizeTickFromPrice(price, tickSize, tick, snappedPrice)) {
                    t["tick"] = tick;
                    t["price"] = snappedPrice;
                } else {
                    t["price"] = price;
                }
                t["qty"] = qtyBase;
                const std::string side = fill.value("direction", std::string("buy"));
                t["side"] = (side == "sell") ? "sell" : "buy";
                const long long ts = fill.value("ts", parentTs);
                t["timestamp"] = ts;
                tradeBatcher().add(config.symbol, std::move(t));
            };
            const std::string topType = j.value("type", std::string());
            if (isFillsType(topType)) {
                const long long parentTs = j.value("ts", 0LL);
                const json data = j.value("data", json::object());
                if (data.is_array()) {
                    for (const auto &entry : data) {
                        if (entry.is_object() && entry.contains("data")) {
                            const json nested = entry.value("data", json::array());
                            if (nested.is_array()) {
                                for (const auto &fill : nested) {
                                    emitFill(fill, parentTs);
                                }
                                continue;
                            }
                        }
                        emitFill(entry, parentTs);
                    }
                } else if (data.is_object()) {
                    if (data.contains("data") && data.value("data", json::array()).is_array()) {
                        for (const auto &fill : data.value("data", json::array())) {
                            emitFill(fill, parentTs);
                        }
                    } else {
                        emitFill(data, parentTs);
                    }
                }
                return;
            }
            const json maybeData = j.value("data", json::object());
            if (maybeData.is_object() && isFillsType(maybeData.value("type", std::string()))) {
                const long long parentTs = j.value("ts", 0LL);
                const json nested = maybeData.value("data", json::array());
                if (nested.is_array()) {
                    for (const auto &fill : nested) {
                        emitFill(fill, parentTs);
                    }
                } else {
                    emitFill(maybeData, parentTs);
                }
                return;
            }
            if (maybeData.is_array()) {
                const long long parentTs = j.value("ts", 0LL);
                for (const auto &entry : maybeData) {
                    if (entry.is_object() && isFillsType(entry.value("type", std::string()))) {
                        const json nested = entry.value("data", json::array());
                        if (nested.is_array()) {
                            for (const auto &fill : nested) {
                                emitFill(fill, parentTs);
                            }
                        } else {
                            emitFill(entry, parentTs);
                        }
                        return;
                    }
                }
            }
            const json* dataPtr = nullptr;
            const auto dataIt = j.find("data");
            if (dataIt != j.end() && dataIt->is_object())
            {
                dataPtr = &(*dataIt);
            }
            const json& data = dataPtr ? *dataPtr : j;
            const json bidsSide = data.contains("bids") ? data["bids"]
                               : data.contains("b") ? data["b"]
                                                    : json::array();
            const json asksSide = data.contains("asks") ? data["asks"]
                               : data.contains("a") ? data["a"]
                                                    : json::array();
            const auto bids = parseSide(bidsSide, tickSize);
            const auto asks = parseSide(asksSide, tickSize);
            if (book.tickSize() <= 0.0 && tickSize > 0.0)
            {
                book.setTickSize(tickSize);
            }
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                book.loadSnapshot(bids, asks);
                const auto now = std::chrono::steady_clock::now();
                if (now - lastEmit >= config.throttle)
                {
                    lastEmit = now;
                    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch())
                                           .count();
                    emitLadder(config, book, book.bestBid(), book.bestAsk(), nowMs);
                }
            }
        };

        try
        {
            processJson(message);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[backend] UZX parse error: " << ex.what() << std::endl;
        }
    }

    WinHttpCloseHandle(rawSocket);
    return true;
}

int main(int argc, char** argv)
{
#if defined(ORDERBOOK_BACKEND_QT)
    QCoreApplication qtApp(argc, argv);
#endif
    try
    {
        auto cfg = parseArgs(argc, argv);
        std::cerr << "[backend] protocol=2 tickQuant=scaled" << std::endl;
        if (!cfg.winProxy.empty())
        {
            std::cerr << "[backend] proxy enabled: type=" << cfg.proxyType
                      << " auth=" << (cfg.proxyUser.empty() ? "0" : "1") << std::endl;
        }
        dom::OrderBook book;
        book.setCacheLevelsPerSide(cfg.cacheLevelsPerSide);
        std::thread(heartbeatThread).detach();
        std::thread(controlReaderThread).detach();

        if (cfg.exchange == "mexc")
        {
            std::cerr << "[backend] starting MEXC spot depth for " << cfg.symbol << std::endl;
            double tickSize = 0.0;
            if (!fetchExchangeInfo(cfg, tickSize))
            {
                std::cerr << "[backend] failed to determine tick size, exiting" << std::endl;
                return 1;
            }
            book.setTickSize(tickSize);

            if (!fetchSnapshot(cfg, book))
            {
                std::cerr << "[backend] snapshot failed, continuing with empty book" << std::endl;
            }
            if (book.tickSize() > 0.0 && book.bestBid() > 0.0 && book.bestAsk() > 0.0)
            {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                emitLadder(cfg, book, book.bestBid(), book.bestAsk(), nowMs);
            }
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                g_bookPtr = &book;
                g_activeConfig = cfg;
            }
            g_bookReady.store(true);
            if (cfg.mexcSpotMode == "rest")
            {
                runMexcSpotPolling(cfg, book);
            }
            else
            {
                // Default: protobuf WebSocket market streams (the official MEXC spot WS is protobuf-only).
                // REST polling remains as a fallback mode for problematic networks.
                runWebSocket(cfg, book);
            }
            return 0;
        }
        else if (cfg.exchange == "mexc_futures")
        {
            std::cerr << "[backend] starting MEXC futures depth for " << cfg.symbol << std::endl;
            double tickSize = 0.0;
            double contractSize = 1.0;
            if (!fetchFuturesContractInfo(cfg, tickSize, contractSize))
            {
                std::cerr << "[backend] failed to determine futures tick size, exiting" << std::endl;
                return 1;
            }
            cfg.futuresContractSize = contractSize;
            book.setTickSize(tickSize);
            if (!fetchFuturesSnapshot(cfg, book, contractSize))
            {
                std::cerr << "[backend] futures snapshot failed, continuing with empty book" << std::endl;
            }
            if (book.tickSize() > 0.0 && book.bestBid() > 0.0 && book.bestAsk() > 0.0)
            {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                emitLadder(cfg, book, book.bestBid(), book.bestAsk(), nowMs);
            }
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                g_bookPtr = &book;
                g_activeConfig = cfg;
            }
            g_bookReady.store(true);
            runMexcFuturesWebSocket(cfg, book);
        }
        else if (cfg.exchange == "binance" || cfg.exchange == "binance_futures")
        {
            const bool futures = cfg.exchange == "binance_futures";
            std::cerr << "[backend] starting Binance " << (futures ? "futures" : "spot")
                      << " depth for " << cfg.symbol << std::endl;
            double tickSize = 0.0;
            const bool tickOk = futures ? fetchBinanceExchangeInfoFutures(cfg, tickSize)
                                        : fetchBinanceExchangeInfoSpot(cfg, tickSize);
            if (!tickOk)
            {
                std::cerr << "[backend] failed to determine tick size, exiting" << std::endl;
                return 1;
            }
            book.setTickSize(tickSize);
            BinanceDepthSnapshot snap;
            const bool snapshotOk =
                futures ? fetchBinanceSnapshotFutures(cfg, tickSize, snap)
                        : fetchBinanceSnapshotSpot(cfg, tickSize, snap);
            if (!snapshotOk)
            {
                std::cerr << "[backend] snapshot failed, continuing with empty book" << std::endl;
            }
            else
            {
                book.loadSnapshot(snap.bids, snap.asks);
            }
            if (book.tickSize() > 0.0 && book.bestBid() > 0.0 && book.bestAsk() > 0.0)
            {
                const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                emitLadder(cfg, book, book.bestBid(), book.bestAsk(), nowMs);
            }
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                g_bookPtr = &book;
                g_activeConfig = cfg;
            }
            g_bookReady.store(true);
            runBinanceWebSocket(cfg, book, futures, snap.lastUpdateId);
        }
        else if (cfg.exchange == "lighter")
        {
            std::cerr << "[backend] starting Lighter depth for " << cfg.symbol << std::endl;
            int marketId = -1;
            double tickSize = 0.0;
            int attempts = 0;
            while (!fetchLighterMarketInfo(cfg, marketId, tickSize))
            {
                attempts++;
                const int capped = std::min(attempts, 8);
                const int delayMs = std::min(30000, 350 * (1 << capped));
                std::cerr << "[backend] lighter: failed to resolve market_id/tickSize (attempt "
                          << attempts << "), retrying in " << delayMs << "ms" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            }
            if (tickSize > 0.0 && book.tickSize() <= 0.0)
            {
                book.setTickSize(tickSize);
            }
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                g_bookPtr = &book;
                g_activeConfig = cfg;
            }
            g_bookReady.store(true);
            runLighterWebSocket(cfg, book, marketId);
        }
        else
        {
            const bool isSwap = cfg.exchange == "uzxswap";
            std::cerr << "[backend] starting UZX " << (isSwap ? "swap" : "spot") << " depth for " << cfg.symbol
                      << std::endl;
            double tickSize = 0.0;
            {
                std::lock_guard<std::mutex> lock(g_bookMutex);
                g_bookPtr = &book;
                g_activeConfig = cfg;
            }
            g_bookReady.store(true);
            runUzxWebSocket(cfg, book, tickSize, isSwap);
        }
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "fatal: " << ex.what() << std::endl;
        return 1;
    }
}
