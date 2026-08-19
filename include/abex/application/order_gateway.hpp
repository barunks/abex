#pragma once

#include "abex/application/market_data_book.hpp"
#include "abex/application/operational_event_writer.hpp"
#include "abex/application/risk_manager.hpp"
#include "abex/application/sequence_tracker.hpp"
#include "abex/application/spsc_execution_lane.hpp"
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
    [[nodiscard]] std::unordered_map<std::string, Decimal> positions() const;
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
    void persist_order(const Order& order);
    void notify_order_observers(const Order& order) noexcept;
    void complete_operational_event(std::optional<OperationalEvent> event,
                                    std::string error) noexcept;
    void record_event(OperationalSeverity severity,
                      std::string_view category,
                      std::string_view code,
                      std::string_view message,
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

    mutable std::mutex mutex_;
    // All order mutations that become journal records acquire this gate before
    // mutex_. State is unlocked during append/fdatasync while write order stays
    // identical to mutation order.
    mutable std::mutex persistence_mutex_;
    StringMap<Order> orders_;
    StringMap<std::string> exchange_id_index_;
    StringMap<std::string> exchange_client_id_index_;
    StringMap<Decimal> conservative_positions_;
    StringMap<std::vector<ExecutionReport>> deferred_amend_reports_;
    StringSet active_operations_;
    std::unordered_map<Venue, SequenceTracker> sequence_trackers_;
    std::unordered_map<Venue, VenueHealth> health_;
    mutable std::mutex order_observer_mutex_;
    std::vector<std::pair<ObserverToken, OrderObserver>> order_observers_;
    ObserverToken next_observer_token_{1};
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
    std::deque<OperationalEvent> operational_events_;
    std::unordered_map<ObserverToken, OperationalObserver> operational_observers_;
    ObserverToken next_operational_observer_token_{1};
    std::uint64_t idempotent_replays_{0};
    std::uint64_t reconciliations_{0};
    std::uint64_t alerts_{0};
    std::uint64_t logging_failures_{0};
    std::string last_logging_error_;
    std::array<std::unique_ptr<SpscExecutionLane>, 2> execution_lanes_;
    std::unique_ptr<OperationalEventWriter> operational_event_writer_;
};

} // namespace abex
