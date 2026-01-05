#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dom
{
    struct Level
    {
        double price{};
        double bidQuantity{};
        double askQuantity{};
    };

    class OrderBook
    {
    public:
        using Tick = std::int64_t;

        struct Row
        {
            Tick tick{};
            double bidQuantity{};
            double askQuantity{};
        };

        OrderBook();

        void clear();

        // Set tick size (price step) in quote currency.
        void setTickSize(double tickSize);

        // Total book span cached around mid (per side, in ticks).
        void setCacheLevelsPerSide(std::size_t levels);

        // Snapshot from REST depth, prices in ticks.
        void loadSnapshot(const std::vector<std::pair<Tick, double>>& bids,
                          const std::vector<std::pair<Tick, double>>& asks);

        // Incremental updates from aggre.depth stream, prices in ticks.
        void applyDelta(const std::vector<std::pair<Tick, double>>& bids,
                        const std::vector<std::pair<Tick, double>>& asks,
                        std::size_t cacheLevelsHint);

        [[nodiscard]] double bestBid() const;
        [[nodiscard]] double bestAsk() const;
        [[nodiscard]] double tickSize() const;

        [[nodiscard]] std::vector<Level> ladder(std::size_t levelsPerSide,
                                                Tick *outWindowMin = nullptr,
                                                Tick *outWindowMax = nullptr,
                                                Tick *outCenter = nullptr) const;

        // Sparse ladder: returns only ticks with non-zero bid/ask inside the window.
        [[nodiscard]] std::vector<Row> ladderSparse(std::size_t levelsPerSide,
                                                    Tick *outWindowMin = nullptr,
                                                    Tick *outWindowMax = nullptr,
                                                    Tick *outCenter = nullptr) const;

        void shiftManualCenterTicks(Tick delta);
        void clearManualCenter();

    private:
        using BookSide = std::map<Tick, double, std::less<>>;

        BookSide bids_; // key: tick index, value: qty
        BookSide asks_;
        double tickSize_{0.0};

        // Center of the ladder in ticks; adjusted slowly to avoid jumping.
        mutable Tick centerTick_{0};
        mutable bool hasCenter_{false};
        mutable Tick manualCenterTick_{0};
        mutable bool manualCenterActive_{false};
        std::size_t cacheLevelsPerSide_{5000};

        static void applySide(BookSide& side,
                              const std::vector<std::pair<Tick, double>>& updates);
        static void pruneOutsideWindow(BookSide& side, Tick minTick, Tick maxTick);
        bool resolveAutoCenterTick(Tick& outTick) const;
        void pruneToCacheWindow(Tick anchorTick);
        bool computeWindow(std::size_t levelsPerSide,
                           Tick& outMinTick,
                           Tick& outMaxTick,
                           Tick& outCenterTick) const;

        static constexpr Tick kMaxLevels = 40000;
    };
} // namespace dom
