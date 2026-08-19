#include "abex/application/market_data_book.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace abex {

MarketDataBook::MarketDataBook(std::chrono::milliseconds maximum_age)
    : maximum_age_(maximum_age) {
    if (maximum_age_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("market-data maximum age must be positive");
    }
}

void MarketDataBook::publish(MarketQuote quote) {
    if (!valid_quote(quote)) throw std::invalid_argument("invalid market quote");
    if (quote.published_at_ms == 0) quote.published_at_ms = unix_time_ms();

    std::vector<QuoteObserver> observers;
    {
        std::scoped_lock lock(mutex_);
        auto& venue_quotes = quotes_[venue_index(quote.venue)];
        const auto found = venue_quotes.find(quote.symbol);
        if (found != venue_quotes.end()) {
            if (found->second.sequence > quote.sequence && quote.sequence != 0) return;
            found->second = quote;
        } else {
            venue_quotes.emplace(quote.symbol, quote);
        }
        status_.last_update_ms = std::max(status_.last_update_ms, quote.published_at_ms);
        status_.last_sequence = std::max(status_.last_sequence, quote.sequence);
        observers.reserve(observers_.size());
        for (const auto& [token, observer] : observers_) {
            (void)token;
            observers.push_back(observer);
        }
    }
    for (const auto& observer : observers) observer(quote);
}

std::vector<MarketQuote> MarketDataBook::snapshot() const {
    std::vector<MarketQuote> result;
    {
        std::scoped_lock lock(mutex_);
        result.reserve(quotes_[0].size() + quotes_[1].size());
        for (const auto& venue_quotes : quotes_) {
            for (const auto& [symbol, quote] : venue_quotes) {
                (void)symbol;
                result.push_back(quote);
            }
        }
    }
    std::ranges::sort(result, [](const auto& lhs, const auto& rhs) {
        if (lhs.symbol != rhs.symbol) return lhs.symbol < rhs.symbol;
        return lhs.venue < rhs.venue;
    });
    return result;
}

std::optional<MarketQuote> MarketDataBook::latest(Venue venue, std::string_view symbol) const {
    std::scoped_lock lock(mutex_);
    const auto& venue_quotes = quotes_[venue_index(venue)];
    const auto found = venue_quotes.find(symbol);
    if (found == venue_quotes.end()) return std::nullopt;
    return found->second;
}

std::optional<Decimal> MarketDataBook::price(Venue venue,
                                             std::string_view symbol,
                                             Side side) const {
    const auto quote = latest(venue, symbol);
    if (!quote || !fresh(*quote)) return std::nullopt;
    return executable_price(*quote, side);
}

bool MarketDataBook::fresh(const MarketQuote& quote, std::int64_t now_ms) const noexcept {
    const auto age = now_ms - quote.published_at_ms;
    return age >= 0 && age <= maximum_age_.count();
}

MarketDataBook::ObserverToken MarketDataBook::add_observer(QuoteObserver observer) {
    if (!observer) throw std::invalid_argument("market-data observer is required");
    std::scoped_lock lock(mutex_);
    const auto token = next_observer_token_++;
    observers_.emplace(token, std::move(observer));
    return token;
}

void MarketDataBook::remove_observer(ObserverToken token) noexcept {
    std::scoped_lock lock(mutex_);
    observers_.erase(token);
}

void MarketDataBook::set_ring_status(bool mapped,
                                     std::uint64_t generation,
                                     std::uint64_t last_sequence,
                                     std::string error) {
    std::scoped_lock lock(mutex_);
    if (generation != 0 && status_.generation != 0 && generation != status_.generation) {
        for (auto& venue_quotes : quotes_) venue_quotes.clear();
        status_.last_sequence = 0;
        status_.last_update_ms = 0;
    }
    status_.ring_mapped = mapped;
    status_.generation = generation;
    status_.last_sequence = last_sequence;
    status_.last_error = std::move(error);
}

MarketDataStatus MarketDataBook::status() const {
    std::scoped_lock lock(mutex_);
    auto result = status_;
    const auto age = unix_time_ms() - result.last_update_ms;
    result.ring_connected = result.ring_mapped && result.last_update_ms != 0 && age >= 0 &&
                            age <= maximum_age_.count();
    if (result.ring_mapped && !result.ring_connected && result.last_error.empty()) {
        result.last_error = result.last_update_ms == 0
                                ? "waiting for the first publisher update"
                                : "market-data publisher updates are stale";
    }
    return result;
}

} // namespace abex
