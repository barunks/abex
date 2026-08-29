#pragma once

#include "abex/application/async_order_observer_queue.hpp"
#include "abex/application/market_data_book.hpp"
#include "abex/application/operational_event_writer.hpp"
#include "abex/application/risk_manager.hpp"
#include "abex/application/sequence_tracker.hpp"
#include "abex/application/spsc_execution_lane.hpp"
#include "abex/application/symbol_index.hpp"
#include "abex/application/venue_cache.hpp"
#include "abex/domain/string_lookup.hpp"
#include "abex/ports/exchange_adapter.hpp"
#include "abex/ports/order_store.hpp"

#include <atomic>
#include <array>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace abex {

struct OperationResult {
    bool ok{false};
    bool idempotent_replay{false};
    std::string code;
    std::string message;
    std::optional<Order> order;
};

// Read-only projection used by REST/CLI/WebSocket snapshots. It deliberately
// excludes idempotency maps, execution aliases, and dedup sets from Order so a
// UI read does not copy internal hash tables under the gateway lock.
struct OrderSnapshot {
    std::string client_order_id;
    std::string exchange_order_id;
    Venue venue{Venue::Okx};
    std::string symbol;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    std::optional<Decimal> price;
    Decimal quantity;
    TimeInForce time_in_force{TimeInForce::Gtc};
    OrderStatus status{OrderStatus::Unknown};
    PendingAction pending_action{PendingAction::None};
    std::optional<Decimal> pending_amend_price;
    std::optional<Decimal> pending_amend_quantity;
    Decimal filled_quantity;
    std::optional<Decimal> average_fill_price;
    std::string rejection_reason;
    std::uint64_t version{0};
    std::optional<std::uint64_t> last_sequence;
    std::int64_t created_at_ms{0};
    std::int64_t updated_at_ms{0};
};

struct VenueHealth {
    bool connected{false};
    bool ever_connected{false};
    bool reconciliation_required{false};
    std::uint64_t sequence_gaps{0};
    std::uint64_t dropped_events{0};
    std::string last_error;
};

struct GatewayStability {
    std::string instance_id;
    std::int64_t started_at_ms{0};
    std::size_t recovered_orders{0};
    std::uint64_t idempotent_replays{0};
    std::uint64_t reconciliations{0};
    std::uint64_t alerts{0};
    std::uint64_t logging_failures{0};
    std::string last_logging_error;
    OrderJournalStatus journal;
};

class OrderGateway final {
public:
    using ObserverToken = std::uint64_t;
    using OrderObserver = std::function<void(const Order&)>;
    using OperationalObserver = std::function<void(const OperationalEvent&)>;

    struct Options {
        std::size_t event_queue_capacity{4096};
        std::chrono::milliseconds event_submit_timeout{1};
        bool reconcile_on_start{true};
        bool reconcile_on_reconnect{true};
        std::chrono::milliseconds reconciliation_interval{30000};
    };

    OrderGateway(std::vector<std::shared_ptr<IExchangeAdapter>> adapters,
                 RiskManager risk_manager,
                 std::shared_ptr<IOrderStore> order_store);
    OrderGateway(std::vector<std::shared_ptr<IExchangeAdapter>> adapters,
                 RiskManager risk_manager,
                 std::shared_ptr<IOrderStore> order_store,
                 Options options,
                 std::shared_ptr<MarketDataBook> market_data = {});
    ~OrderGateway();

    OrderGateway(const OrderGateway&) = delete;
    OrderGateway& operator=(const OrderGateway&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] OperationResult place(const OrderRequest& request);
    [[nodiscard]] OperationResult cancel(CancelRequest request);
    [[nodiscard]] OperationResult amend(AmendRequest request);
    [[nodiscard]] OperationResult reconcile(Venue venue);

    [[nodiscard]] std::optional<Order> get(std::string_view client_order_id) const;
    [[nodiscard]] std::optional<OrderSnapshot>
    get_snapshot(std::string_view client_order_id) const;
    [[nodiscard]] std::vector<Order> list(std::optional<Venue> venue = std::nullopt,
                                          std::optional<OrderStatus> status = std::nullopt) const;
    [[nodiscard]] std::vector<OrderSnapshot>
    list_snapshots(std::optional<Venue> venue = std::nullopt,
                   std::optional<OrderStatus> status = std::nullopt) const;
    // Immutable position snapshot: flat array indexed by SymbolId slot.
    // Callers iterate directly — zero copy, zero allocation on the read path.
    struct PositionSnapshot {
        std::array<Decimal, kSymbolCount> values{};
    };

    // Returns a ref-counted handle to the latest immutable snapshot.
    // One atomic load, no mutex, no copy.
    [[nodiscard]] std::shared_ptr<const PositionSnapshot> positions() const;
    [[nodiscard]] BalanceQueryResult
    balances(Venue venue, std::optional<std::string> currency = std::nullopt) const;
    [[nodiscard]] InstrumentRulesQueryResult
    instrument_rules(Venue venue, std::string symbol) const;
    [[nodiscard]] std::unordered_map<Venue, VenueHealth> health() const;
    [[nodiscard]] GatewayStability stability() const;
    [[nodiscard]] std::vector<OperationalEvent>
    operational_events(std::size_t limit = 100) const;
    [[nodiscard]] std::vector<OperationalEvent>
    order_events(std::string_view client_order_id, std::size_t limit = 500) const;
    // Order observers are now dispatched asynchronously from a dedicated thread.
    [[nodiscard]] ObserverToken add_order_observer(OrderObserver observer);
    void remove_order_observer(ObserverToken token) noexcept;
    [[nodiscard]] ObserverToken add_operational_observer(OperationalObserver observer);
    void remove_operational_observer(ObserverToken token) noexcept;
    void flush_events();

private:
    [[nodiscard]] std::shared_ptr<IExchangeAdapter> adapter_for(Venue venue) const;
    [[nodiscard]] Decimal conservative_position_locked(
        std::string_view symbol,
        std::string_view excluded_client_order_id = {}) const;
    [[nodiscard]] static Decimal position_contribution(const Order& order);
    void adjust_position_locked(std::string_view symbol,
                                Decimal previous,
                                Decimal current);
    void rebuild_indexes_locked();
    // Publish atomic snapshots after any write-path mutation. Called inside
    // mutex_ so the snapshots are always consistent with orders_ state.
    // Mark dirty — deferred publish happens once per place/cancel/amend cycle
    // via publish_positions_if_dirty_locked(), called just before mutex_ release.
    void mark_positions_dirty_locked() noexcept { positions_dirty_ = true; }
    void publish_positions_if_dirty_locked();
    void publish_health_error_locked(Venue venue, std::string error);
    void persist_order(const Order& order, bool intent_only = false);
    // prepare_persist: called inside mutex_ — snapshots the Order into an
    // immutable shared_ptr and reserves a sequence number (one atomic fetch_add).
    // Returns {snapshot, intent_only_flag, sequence}.
    [[nodiscard]] std::tuple<std::shared_ptr<const Order>, bool, std::uint64_t>
    prepare_persist(const Order& order, bool intent_only = false);
    // commit_persist: called outside mutex_ — serializes the snapshot and writes
    // to the journal. Sequence was reserved under the lock so ordering is correct.
    // Takes shared_ptr so the same allocation is reused by notify_order_observers.
    void commit_persist(std::shared_ptr<const Order> snapshot, bool intent_only, std::uint64_t sequence);
    // Post to async observer queue — never blocks the caller.
    void notify_order_observers(std::shared_ptr<Order> order) noexcept;
    void complete_operational_event(std::optional<OperationalEvent> event,
                                    std::string error) noexcept;
    // record_event / record_event2: pass string_view literals — no std::string
    // construction on the caller thread (P4 fix).
    void record_event(OperationalSeverity severity,
                      std::string_view category,
                      std::string_view code,
                      std::string message,
                      std::optional<Venue> venue = std::nullopt,
                      std::string_view client_order_id = {},
                      std::string_view request_id = {},
                      std::optional<OrderEventContext> order = std::nullopt) noexcept;
    void record_event2(OperationalSeverity sev_a, std::string_view cat_a,
                       std::string_view code_a, std::string msg_a,
                       OperationalSeverity sev_b, std::string_view cat_b,
                       std::string_view code_b, std::string msg_b,
                       std::optional<Venue> venue = std::nullopt,
                       std::string_view client_order_id = {},
                       std::string_view request_id = {},
                       std::optional<OrderEventContext> order = std::nullopt) noexcept;
    void receive_execution(Venue venue, ExecutionReport report);
    void apply_execution(Venue venue, const ExecutionReport& report);
    void connection_changed(Venue venue, bool connected, std::string reason);
    void start_reconciliation_worker();
    void stop_reconciliation_worker() noexcept;
    void schedule_reconciliation(Venue venue);
    void run_reconciliation_worker(std::stop_token token);
    [[nodiscard]] Order* locate_order_locked(Venue venue, const ExecutionReport& report);

    std::unordered_map<Venue, std::shared_ptr<IExchangeAdapter>> adapters_;
    RiskManager risk_manager_;
    std::shared_ptr<IOrderStore> order_store_;
    std::shared_ptr<MarketDataBook> market_data_;
    Options options_;

    // Single mutex guards all mutable order state. Callers: acquire, mutate,
    // capture a local snapshot, release, then call persist_order() and
    // notify_order_observers() outside the lock. The order store is
    // self-synchronized; no second gateway mutex is needed for journal writes.
    mutable std::mutex mutex_;
    StringMap<std::shared_ptr<Order>> orders_;
    StringMap<std::string> exchange_id_index_;
    StringMap<std::string> exchange_client_id_index_;
    // Startup-time symbol interning table. Populated once in the constructor
    // from the risk manager limits. All hot-path position operations use the
    // uint8_t slot — zero string hashing, zero heap, O(1) array index.
    // Flat position array indexed by SymbolId slot. Replaces StringMap.
    // kSymbolCount entries — fits in one cache line.
    std::array<Decimal, kSymbolCount> conservative_positions_{};
    bool positions_dirty_{false};
    // Atomic snapshot published once per operation cycle (not per intermediate mutation).
    // positions() is one atomic load — zero copy, zero allocation.
    std::atomic<std::shared_ptr<const PositionSnapshot>> positions_snap_;
    // Atomic snapshot of per-venue last errors — updated under mutex_ but read
    // by health() without acquiring it.
    std::atomic<std::shared_ptr<const std::unordered_map<Venue, std::string>>> health_errors_snap_;
    StringMap<std::vector<ExecutionReport>> deferred_amend_reports_;
    StringSet active_operations_;
    struct AtomicVenueHealth {
        std::atomic<bool> connected{false};
        std::atomic<bool> ever_connected{false};
        std::atomic<bool> reconciliation_required{false};
        std::atomic<std::uint64_t> sequence_gaps{0};
        std::atomic<std::uint64_t> dropped_events{0};
    };

    std::unordered_map<Venue, SequenceTracker> sequence_trackers_; // under mutex_
    std::unordered_map<Venue, AtomicVenueHealth> health_;          // atomic fields, no lock needed
    // Async observer dispatch — post() is the only call on the critical path.
    std::unique_ptr<AsyncOrderObserverQueue> order_observer_queue_;
    std::atomic<bool> started_{false};
    std::mutex reconciliation_mutex_;
    std::condition_variable_any reconciliation_condition_;
    std::atomic<std::uint8_t> reconciliation_pending_{0};
    std::jthread reconciliation_worker_;

    const std::int64_t started_at_ms_;
    const std::string instance_id_;
    std::size_t recovered_orders_{0};
    bool previous_instance_present_{false};
    mutable std::mutex operational_mutex_;
    std::vector<OperationalEvent> operational_events_; // bounded sliding window, max 200
    std::unordered_map<ObserverToken, OperationalObserver> operational_observers_;
    ObserverToken next_operational_observer_token_{1};
    std::atomic<std::uint64_t> idempotent_replays_{0};
    std::atomic<std::uint64_t> reconciliations_{0};
    std::atomic<std::uint64_t> alerts_{0};
    std::atomic<std::uint64_t> logging_failures_{0};
    // Guarded by its own mutex (not operational_mutex_) so the queue-full write
    // path never contends with the observer fanout in complete_operational_event.
    mutable std::mutex logging_error_mutex_;
    std::string last_logging_error_;
    std::array<std::unique_ptr<SpscExecutionLane>, 2> execution_lanes_;
    std::unique_ptr<OperationalEventWriter> operational_event_writer_;
    // Per-venue TTL caches — critical path reads are lock-free atomic loads.
    std::unordered_map<Venue, std::unique_ptr<VenueCache>> venue_caches_;
};

} // namespace abex
