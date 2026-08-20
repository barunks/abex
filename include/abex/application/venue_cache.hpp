#pragma once

#include "abex/ports/exchange_adapter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace abex {

// TTL cache for instrument rules and balance snapshots.
//
// The critical path (place()) does a single atomic load per lookup — zero
// network I/O, zero mutex contention. A background refresh thread re-fetches
// entries before they expire. On the first request for an unknown symbol the
// cache is populated synchronously (cold path only); all subsequent calls are
// served from the cache.
//
// Thread safety: reads are lock-free (atomic<shared_ptr>). Writes (refresh
// thread + cold-path fill) are serialized under write_mutex_.
class VenueCache final {
public:
    explicit VenueCache(IExchangeAdapter& adapter,
                        std::chrono::milliseconds instrument_ttl = std::chrono::seconds{30},
                        std::chrono::milliseconds balance_ttl    = std::chrono::seconds{5})
        : adapter_(adapter), instrument_ttl_(instrument_ttl), balance_ttl_(balance_ttl),
          worker_([this](std::stop_token token) { run(token); }) {}

    ~VenueCache() {
        worker_.request_stop();
        wake_.notify_all();
    }

    VenueCache(const VenueCache&) = delete;
    VenueCache& operator=(const VenueCache&) = delete;

    // Critical path: one atomic load. Returns nullopt only if the symbol has
    // never been requested (first call triggers a synchronous fetch).
    [[nodiscard]] InstrumentRulesQueryResult instrument_rules(const std::string& symbol) {
        {
            const auto snap = instrument_snap_.load(std::memory_order_acquire);
            if (snap) {
                const auto it = snap->find(symbol);
                if (it != snap->end() && !expired(it->second.fetched_at, instrument_ttl_))
                    return it->second.result;
            }
        }
        // Cold path or stale: fetch synchronously and populate.
        return refresh_instrument(symbol);
    }

    [[nodiscard]] BalanceQueryResult balances(const std::optional<std::string>& currency) {
        const auto key = currency.value_or("*");
        {
            const auto snap = balance_snap_.load(std::memory_order_acquire);
            if (snap) {
                const auto it = snap->find(key);
                if (it != snap->end() && !expired(it->second.fetched_at, balance_ttl_))
                    return it->second.result;
            }
        }
        return refresh_balance(currency);
    }

    // Called by the gateway when a new symbol is first seen — wakes the
    // background thread to pre-warm the cache entry.
    void warm(const std::string& symbol) {
        {
            std::scoped_lock lock(write_mutex_);
            pending_symbols_.insert(symbol);
        }
        wake_.notify_one();
    }

private:
    struct InstrumentEntry {
        InstrumentRulesQueryResult result;
        std::int64_t fetched_at{0};
    };
    struct BalanceEntry {
        BalanceQueryResult result;
        std::int64_t fetched_at{0};
    };
    using InstrumentMap = std::unordered_map<std::string, InstrumentEntry>;
    using BalanceMap    = std::unordered_map<std::string, BalanceEntry>;

    [[nodiscard]] static bool expired(std::int64_t fetched_at,
                                      std::chrono::milliseconds ttl) noexcept {
        return unix_time_ms() - fetched_at > ttl.count();
    }

    InstrumentRulesQueryResult refresh_instrument(const std::string& symbol) {
        auto result = adapter_.query_instrument_rules(symbol);
        // Only cache successful responses — a failed fetch (e.g. DISCONNECTED)
        // must not poison the cache and block subsequent successful calls.
        if (result.ok) {
            std::scoped_lock lock(write_mutex_);
            auto updated = std::make_shared<InstrumentMap>(
                instrument_snap_.load(std::memory_order_acquire)
                    ? *instrument_snap_.load(std::memory_order_acquire)
                    : InstrumentMap{});
            (*updated)[symbol] = {result, unix_time_ms()};
            instrument_snap_.store(std::move(updated), std::memory_order_release);
        }
        return result;
    }

    BalanceQueryResult refresh_balance(const std::optional<std::string>& currency) {
        auto result = adapter_.query_balances(currency);
        const auto key = currency.value_or("*");
        // Only cache successful responses.
        if (result.ok) {
            std::scoped_lock lock(write_mutex_);
            auto updated = std::make_shared<BalanceMap>(
                balance_snap_.load(std::memory_order_acquire)
                    ? *balance_snap_.load(std::memory_order_acquire)
                    : BalanceMap{});
            (*updated)[key] = {result, unix_time_ms()};
            balance_snap_.store(std::move(updated), std::memory_order_release);
        }
        return result;
    }

    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            std::unordered_set<std::string> symbols;
            {
                std::unique_lock lock(write_mutex_);
                // Wake when there are pending symbols or when any cached entry
                // is approaching expiry. Use half the TTL as the poll interval.
                const auto interval = std::min(instrument_ttl_, balance_ttl_) / 2;
                wake_.wait_for(lock, token, interval,
                               [this] { return !pending_symbols_.empty(); });
                if (token.stop_requested()) break;
                symbols = std::move(pending_symbols_);
                pending_symbols_.clear();
            }

            // Refresh any instrument entries that are stale or newly requested.
            {
                const auto snap = instrument_snap_.load(std::memory_order_acquire);
                if (snap) {
                    for (const auto& [sym, entry] : *snap) {
                        if (expired(entry.fetched_at, instrument_ttl_))
                            symbols.insert(sym);
                    }
                }
            }
            for (const auto& sym : symbols) {
                if (token.stop_requested()) break;
                try { refresh_instrument(sym); } catch (...) {}
            }

            // Refresh balance entries that are stale.
            {
                const auto snap = balance_snap_.load(std::memory_order_acquire);
                if (snap) {
                    for (const auto& [key, entry] : *snap) {
                        if (token.stop_requested()) break;
                        if (expired(entry.fetched_at, balance_ttl_)) {
                            try {
                                const auto currency = key == "*"
                                    ? std::optional<std::string>{}
                                    : std::optional<std::string>{key};
                                refresh_balance(currency);
                            } catch (...) {}
                        }
                    }
                }
            }
        }
    }

    IExchangeAdapter& adapter_;
    const std::chrono::milliseconds instrument_ttl_;
    const std::chrono::milliseconds balance_ttl_;

    // Lock-free read path: one atomic load per cache hit.
    std::atomic<std::shared_ptr<const InstrumentMap>> instrument_snap_{nullptr};
    std::atomic<std::shared_ptr<const BalanceMap>>    balance_snap_{nullptr};

    // Serializes all writes (refresh thread + cold-path synchronous fetches).
    std::mutex write_mutex_;
    std::unordered_set<std::string> pending_symbols_;
    std::condition_variable_any wake_;
    std::jthread worker_;
};

} // namespace abex
