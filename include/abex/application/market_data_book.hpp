#pragma once

#include "abex/domain/market_data.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace abex {

struct MarketDataStatus {
    bool ring_mapped{false};
    bool ring_connected{false};
    std::uint64_t generation{0};
    std::uint64_t last_sequence{0};
    std::int64_t last_update_ms{0};
    std::string last_error;
};

class MarketDataBook final {
public:
    using ObserverToken = std::uint64_t;
    using QuoteObserver = std::function<void(const MarketQuote&)>;

    explicit MarketDataBook(std::chrono::milliseconds maximum_age = std::chrono::seconds{5});

    void publish(MarketQuote quote);
    [[nodiscard]] std::vector<MarketQuote> snapshot() const;
    [[nodiscard]] std::optional<MarketQuote> latest(Venue venue,
                                                     std::string_view symbol) const;
    [[nodiscard]] std::optional<Decimal> price(Venue venue,
                                                std::string_view symbol,
                                                Side side) const;
    [[nodiscard]] bool fresh(const MarketQuote& quote,
                             std::int64_t now_ms = unix_time_ms()) const noexcept;
    [[nodiscard]] std::chrono::milliseconds maximum_age() const noexcept {
        return maximum_age_;
    }

    [[nodiscard]] ObserverToken add_observer(QuoteObserver observer);
    void remove_observer(ObserverToken token) noexcept;

    void set_ring_status(bool mapped,
                         std::uint64_t generation,
                         std::uint64_t last_sequence,
                         std::string error = {});
    [[nodiscard]] MarketDataStatus status() const;

private:
    [[nodiscard]] static std::string key(Venue venue, std::string_view symbol);

    const std::chrono::milliseconds maximum_age_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, MarketQuote> quotes_;
    std::unordered_map<ObserverToken, QuoteObserver> observers_;
    ObserverToken next_observer_token_{1};
    MarketDataStatus status_;
};

} // namespace abex
