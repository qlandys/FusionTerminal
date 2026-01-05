#include "OrderBook.hpp"

#include <algorithm>
#include <limits>

namespace dom
{
    OrderBook::OrderBook() = default;

    void OrderBook::clear()
    {
        bids_.clear();
        asks_.clear();
        // tickSize_ is configured separately via setTickSize()
        centerTick_ = 0;
        hasCenter_ = false;
    }

    void OrderBook::setTickSize(double tickSize)
    {
        tickSize_ = tickSize > 0.0 ? tickSize : 0.0;
    }

    void OrderBook::setCacheLevelsPerSide(std::size_t levels)
    {
        if (levels == 0)
        {
            cacheLevelsPerSide_ = 0;
            return;
        }
        const std::size_t maxPerSide = static_cast<std::size_t>(kMaxLevels / 2);
        cacheLevelsPerSide_ = std::min(levels, maxPerSide);
    }

    void OrderBook::loadSnapshot(const std::vector<std::pair<Tick, double>>& bids,
                                 const std::vector<std::pair<Tick, double>>& asks)
    {
        clear();

        for (const auto& [tick, qty] : bids)
        {
            if (qty > 0.0)
            {
                bids_[tick] += qty;
            }
        }

        for (const auto& [tick, qty] : asks)
        {
            if (qty > 0.0)
            {
                asks_[tick] += qty;
            }
        }

        Tick anchorTick = 0;
        if (resolveAutoCenterTick(anchorTick))
        {
            pruneToCacheWindow(anchorTick);
        }
    }

    void OrderBook::applyDelta(const std::vector<std::pair<Tick, double>>& bids,
                               const std::vector<std::pair<Tick, double>>& asks,
                               std::size_t cacheLevelsHint)
    {
        applySide(bids_, bids);
        applySide(asks_, asks);

        // Чтобы не держать бесконечный хвост старых уровней, которые уже ушли
        // далеко от текущего мида, чистим карту за окном вокруг середины.
        if (tickSize_ <= 0.0 || (bids_.empty() && asks_.empty())) {
            return;
        }

        Tick midTick = 0;
        if (!resolveAutoCenterTick(midTick)) {
            return;
        }

        if (cacheLevelsHint > cacheLevelsPerSide_) {
            setCacheLevelsPerSide(cacheLevelsHint);
        }
        pruneToCacheWindow(midTick);

        // Защитный инвариант: bestBid < bestAsk. Если данные пришли кривые или
        // из-за округления стороны пересеклись, вычищаем перекрытие.
        if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
            const Tick askTick = asks_.begin()->first;
            const Tick bidTick = bids_.rbegin()->first;
            // Удаляем бидовые уровни, которые не могут существовать выше/на ask.
            auto badBidIt = bids_.lower_bound(askTick);
            while (badBidIt != bids_.end()) {
                badBidIt = bids_.erase(badBidIt);
            }
            // И удаляем аски, которые не могут быть ниже/на bid.
            auto badAskEnd = asks_.upper_bound(bidTick);
            asks_.erase(asks_.begin(), badAskEnd);
            // Сдвигаем центр при сильной чистке.
            hasCenter_ = false;
        }
    }

    double OrderBook::bestBid() const
    {
        if (bids_.empty() || tickSize_ <= 0.0)
        {
            return 0.0;
        }
        const Tick tick = bids_.rbegin()->first;
        return static_cast<double>(tick) * tickSize_;
    }

    double OrderBook::bestAsk() const
    {
        if (asks_.empty() || tickSize_ <= 0.0)
        {
            return 0.0;
        }
        const Tick tick = asks_.begin()->first;
        return static_cast<double>(tick) * tickSize_;
    }

    double OrderBook::tickSize() const
    {
        return tickSize_;
    }

    std::vector<Level> OrderBook::ladder(std::size_t levelsPerSide,
                                         Tick *outWindowMin,
                                         Tick *outWindowMax,
                                         Tick *outCenter) const
    {
        std::vector<Level> result;

        if (tickSize_ <= 0.0)
        {
            return result;
        }

        if (bids_.empty() && asks_.empty())
        {
            return result;
        }

        // Center around best bid / best ask with some inertia
        // so that the ladder does not jump every tick.
        Tick midTick = 0;
        bool hasMid = false;

        if (manualCenterActive_)
        {
            midTick = manualCenterTick_;
            hasMid = true;
        }
        else
        {
            if (!bids_.empty() && !asks_.empty())
            {
                const Tick bestBidTick = bids_.rbegin()->first;
                const Tick bestAskTick = asks_.begin()->first;
                midTick = (bestBidTick + bestAskTick) / 2;
                hasMid = true;
            }
            else if (!bids_.empty())
            {
                midTick = bids_.rbegin()->first;
                hasMid = true;
            }
            else if (!asks_.empty())
            {
                midTick = asks_.begin()->first;
                hasMid = true;
            }
        }

        if (!hasMid)
        {
            return result;
        }

        constexpr Tick maxLevels = kMaxLevels;

        // Special mode: levelsPerSide == 0 means "full current book"
        // (bounded only by maxLevels). We don't keep a sliding window here,
        // we just cover from min(bids/asks) to max(bids/asks).
        if (levelsPerSide == 0)
        {
            Tick minTick = std::numeric_limits<Tick>::max();
            Tick maxTick = std::numeric_limits<Tick>::min();

            if (!bids_.empty())
            {
                minTick = std::min(minTick, bids_.begin()->first);
                maxTick = std::max(maxTick, bids_.rbegin()->first);
            }
            if (!asks_.empty())
            {
                minTick = std::min(minTick, asks_.begin()->first);
                maxTick = std::max(maxTick, asks_.rbegin()->first);
            }

            if (minTick > maxTick)
            {
                return result;
            }

            Tick count = maxTick - minTick + 1;
            if (count > maxLevels)
            {
                minTick = maxTick - (maxLevels - 1);
                count = maxLevels;
            }

            if (outWindowMin)
            {
                *outWindowMin = minTick;
            }
            if (outWindowMax)
            {
                *outWindowMax = maxTick;
            }
            if (outCenter)
            {
                if (manualCenterActive_)
                {
                    *outCenter = manualCenterTick_;
                }
                else
                {
                    Tick autoCenter = 0;
                    if (resolveAutoCenterTick(autoCenter))
                    {
                        *outCenter = autoCenter;
                    }
                    else
                    {
                        *outCenter = 0;
                    }
                }
            }

            result.reserve(static_cast<std::size_t>(count));
            for (Tick tick = maxTick; tick >= minTick; --tick)
            {
                const double price = static_cast<double>(tick) * tickSize_;

                double bidQty = 0.0;
                double askQty = 0.0;

                auto bidIt = bids_.find(tick);
                if (bidIt != bids_.end())
                {
                    bidQty = bidIt->second;
                }

                auto askIt = asks_.find(tick);
                if (askIt != asks_.end())
                {
                    askQty = askIt->second;
                }

                result.push_back(Level{price, bidQty, askQty});

                if (tick == std::numeric_limits<Tick>::min())
                {
                    break;
                }
            }

            return result;
        }

        const Tick padding = static_cast<Tick>(levelsPerSide);
        if (manualCenterActive_)
        {
            centerTick_ = midTick;
            hasCenter_ = true;
        }
        else if (!hasCenter_)
        {
            centerTick_ = midTick;
            hasCenter_ = true;
        }
        else if (padding > 0)
        {
            // Current window around stored center.
            Tick windowMin = centerTick_ - padding;
            Tick windowMax = centerTick_ + padding;

            // Use an inner band; as long as mid stays inside,
            // we do not move the center. This gives a stable ladder.
            const Tick margin = padding / 4;
            const Tick innerMin = windowMin + margin;
            const Tick innerMax = windowMax - margin;

            if (midTick < innerMin)
            {
                // Shift center down so that midTick is closer to middle again.
                centerTick_ = midTick + (padding - margin);
            }
            else if (midTick > innerMax)
            {
                centerTick_ = midTick - (padding - margin);
            }
        }

        Tick maxTick;
        if (centerTick_ > std::numeric_limits<Tick>::max() - padding)
        {
            maxTick = std::numeric_limits<Tick>::max();
        }
        else
        {
            maxTick = centerTick_ + padding;
        }

        Tick minTick;
        if (centerTick_ < std::numeric_limits<Tick>::min() + padding)
        {
            minTick = std::numeric_limits<Tick>::min();
        }
        else
        {
            minTick = centerTick_ - padding;
        }

        if (maxTick < minTick)
        {
            return result;
        }

        Tick count = maxTick - minTick + 1;
        if (count > maxLevels)
        {
            minTick = maxTick - (maxLevels - 1);
            count = maxLevels;
        }

        if (outWindowMin)
        {
            *outWindowMin = minTick;
        }
        if (outWindowMax)
        {
            *outWindowMax = maxTick;
        }
        if (outCenter)
        {
            *outCenter = centerTick_;
        }

        result.reserve(static_cast<std::size_t>(count));

        for (Tick tick = maxTick; tick >= minTick; --tick)
        {
            const double price = static_cast<double>(tick) * tickSize_;

            double bidQty = 0.0;
            double askQty = 0.0;

            auto bidIt = bids_.find(tick);
            if (bidIt != bids_.end())
            {
                bidQty = bidIt->second;
            }

            auto askIt = asks_.find(tick);
            if (askIt != asks_.end())
            {
                askQty = askIt->second;
            }

            result.push_back(Level{price, bidQty, askQty});

            if (tick == std::numeric_limits<Tick>::min())
            {
                break; // prevent overflow on next --tick
            }
        }

        return result;
    }

    std::vector<OrderBook::Row> OrderBook::ladderSparse(std::size_t levelsPerSide,
                                                        Tick *outWindowMin,
                                                        Tick *outWindowMax,
                                                        Tick *outCenter) const
    {
        std::vector<Row> result;

        Tick minTick = 0;
        Tick maxTick = 0;
        Tick centerTick = 0;
        if (!computeWindow(levelsPerSide, minTick, maxTick, centerTick))
        {
            return result;
        }

        if (outWindowMin)
        {
            *outWindowMin = minTick;
        }
        if (outWindowMax)
        {
            *outWindowMax = maxTick;
        }
        if (outCenter)
        {
            *outCenter = centerTick;
        }

        if (!bids_.empty() || !asks_.empty())
        {
            result.reserve(bids_.size() + asks_.size());
        }

        auto addSide = [&](const BookSide& side, bool isBid) {
            if (side.empty())
            {
                return;
            }
            auto it = side.lower_bound(minTick);
            for (; it != side.end() && it->first <= maxTick; ++it)
            {
                const Tick tick = it->first;
                const double qty = it->second;
                if (qty <= 0.0)
                {
                    continue;
                }
                if (isBid)
                {
                    result.push_back(Row{tick, qty, 0.0});
                }
                else
                {
                    result.push_back(Row{tick, 0.0, qty});
                }
            }
        };

        addSide(bids_, true);
        addSide(asks_, false);

        if (result.empty())
        {
            return result;
        }

        std::sort(result.begin(), result.end(), [](const Row& a, const Row& b) {
            return a.tick > b.tick;
        });

        std::size_t write = 0;
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            if (write == 0 || result[i].tick != result[write - 1].tick)
            {
                result[write++] = result[i];
                continue;
            }
            result[write - 1].bidQuantity += result[i].bidQuantity;
            result[write - 1].askQuantity += result[i].askQuantity;
        }
        result.resize(write);
        return result;
    }

    void OrderBook::shiftManualCenterTicks(Tick delta)
    {
        if (!manualCenterActive_)
        {
            manualCenterTick_ = hasCenter_ ? centerTick_ : Tick{0};
            manualCenterActive_ = true;
        }
        manualCenterTick_ += delta;
    }

    void OrderBook::clearManualCenter()
    {
        manualCenterActive_ = false;
    }

    void OrderBook::applySide(BookSide& side, const std::vector<std::pair<Tick, double>>& updates)
    {
        for (const auto& [tick, qty] : updates)
        {
            if (qty <= 0.0)
            {
                auto it = side.find(tick);
                if (it != side.end())
                {
                    side.erase(it);
                }
            }
            else
            {
                side[tick] = qty;
            }
        }
    }

    void OrderBook::pruneOutsideWindow(BookSide& side, Tick minTick, Tick maxTick)
    {
        if (side.empty()) {
            return;
        }
        auto it = side.begin();
        while (it != side.end() && it->first < minTick) {
            it = side.erase(it);
        }
        if (side.empty()) {
            return;
        }
        it = side.upper_bound(maxTick);
        while (it != side.end()) {
            it = side.erase(it);
        }
    }

    bool OrderBook::resolveAutoCenterTick(Tick& outTick) const
    {
        if (!bids_.empty() && !asks_.empty()) {
            outTick = (bids_.rbegin()->first + asks_.begin()->first) / 2;
            return true;
        }
        if (!bids_.empty()) {
            outTick = bids_.rbegin()->first;
            return true;
        }
        if (!asks_.empty()) {
            outTick = asks_.begin()->first;
            return true;
        }
        return false;
    }

    void OrderBook::pruneToCacheWindow(Tick anchorTick)
    {
        if (cacheLevelsPerSide_ == 0 || (bids_.empty() && asks_.empty())) {
            return;
        }
        const std::size_t maxPerSide = static_cast<std::size_t>(kMaxLevels / 2);
        std::size_t spanLevels = std::min(cacheLevelsPerSide_, maxPerSide);
        if (spanLevels == 0) {
            return;
        }
        const Tick span = static_cast<Tick>(spanLevels);

        Tick maxTick = (anchorTick > std::numeric_limits<Tick>::max() - span)
                           ? std::numeric_limits<Tick>::max()
                           : anchorTick + span;
        Tick minTick = (anchorTick < std::numeric_limits<Tick>::min() + span)
                           ? std::numeric_limits<Tick>::min()
                           : anchorTick - span;

        pruneOutsideWindow(bids_, minTick, maxTick);
        pruneOutsideWindow(asks_, minTick, maxTick);
    }

    bool OrderBook::computeWindow(std::size_t levelsPerSide,
                                  Tick& outMinTick,
                                  Tick& outMaxTick,
                                  Tick& outCenterTick) const
    {
        outMinTick = 0;
        outMaxTick = 0;
        outCenterTick = 0;

        if (tickSize_ <= 0.0)
        {
            return false;
        }

        if (bids_.empty() && asks_.empty())
        {
            return false;
        }

        // Center around best bid / best ask with some inertia
        // so that the ladder does not jump every tick.
        Tick midTick = 0;
        bool hasMid = false;

        if (manualCenterActive_)
        {
            midTick = manualCenterTick_;
            hasMid = true;
        }
        else
        {
            if (!bids_.empty() && !asks_.empty())
            {
                const Tick bestBidTick = bids_.rbegin()->first;
                const Tick bestAskTick = asks_.begin()->first;
                midTick = (bestBidTick + bestAskTick) / 2;
                hasMid = true;
            }
            else if (!bids_.empty())
            {
                midTick = bids_.rbegin()->first;
                hasMid = true;
            }
            else if (!asks_.empty())
            {
                midTick = asks_.begin()->first;
                hasMid = true;
            }
        }

        if (!hasMid)
        {
            return false;
        }

        constexpr Tick maxLevels = kMaxLevels;

        // Special mode: levelsPerSide == 0 means "full current book"
        // (bounded only by maxLevels). We don't keep a sliding window here,
        // we just cover from min(bids/asks) to max(bids/asks).
        if (levelsPerSide == 0)
        {
            Tick minTick = std::numeric_limits<Tick>::max();
            Tick maxTick = std::numeric_limits<Tick>::min();

            if (!bids_.empty())
            {
                minTick = std::min(minTick, bids_.begin()->first);
                maxTick = std::max(maxTick, bids_.rbegin()->first);
            }
            if (!asks_.empty())
            {
                minTick = std::min(minTick, asks_.begin()->first);
                maxTick = std::max(maxTick, asks_.rbegin()->first);
            }

            if (minTick > maxTick)
            {
                return false;
            }

            Tick count = maxTick - minTick + 1;
            if (count > maxLevels)
            {
                minTick = maxTick - (maxLevels - 1);
                count = maxLevels;
            }

            outMinTick = minTick;
            outMaxTick = maxTick;
            if (manualCenterActive_)
            {
                outCenterTick = manualCenterTick_;
            }
            else
            {
                Tick autoCenter = 0;
                if (resolveAutoCenterTick(autoCenter))
                {
                    outCenterTick = autoCenter;
                }
            }
            return true;
        }

        const Tick padding = static_cast<Tick>(levelsPerSide);
        if (manualCenterActive_)
        {
            centerTick_ = midTick;
            hasCenter_ = true;
        }
        else if (!hasCenter_)
        {
            centerTick_ = midTick;
            hasCenter_ = true;
        }
        else if (padding > 0)
        {
            // Current window around stored center.
            Tick windowMin = centerTick_ - padding;
            Tick windowMax = centerTick_ + padding;

            // Use an inner band; as long as mid stays inside,
            // we do not move the center. This gives a stable ladder.
            const Tick margin = padding / 4;
            const Tick innerMin = windowMin + margin;
            const Tick innerMax = windowMax - margin;

            if (midTick < innerMin)
            {
                // Shift center down so that midTick is closer to middle again.
                centerTick_ = midTick + (padding - margin);
            }
            else if (midTick > innerMax)
            {
                centerTick_ = midTick - (padding - margin);
            }
        }

        Tick maxTick;
        if (centerTick_ > std::numeric_limits<Tick>::max() - padding)
        {
            maxTick = std::numeric_limits<Tick>::max();
        }
        else
        {
            maxTick = centerTick_ + padding;
        }

        Tick minTick;
        if (centerTick_ < std::numeric_limits<Tick>::min() + padding)
        {
            minTick = std::numeric_limits<Tick>::min();
        }
        else
        {
            minTick = centerTick_ - padding;
        }

        if (maxTick < minTick)
        {
            return false;
        }

        Tick count = maxTick - minTick + 1;
        if (count > maxLevels)
        {
            minTick = maxTick - (maxLevels - 1);
            count = maxLevels;
        }

        outMinTick = minTick;
        outMaxTick = maxTick;
        outCenterTick = centerTick_;
        return true;
    }
} // namespace dom
