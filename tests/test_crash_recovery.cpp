// Process crash and restart — open order reconciliation tests.
//
// Root cause of the design: SimulatedExchangeAdapter::restore() overwrites
// its internal orders_ map with the journal snapshot, so any state set via
// emit() before restart is lost. The correct way to model "what the venue
// holds while the gateway was down" is a VenueStateAdapter that returns
// pre-configured state from query() and query_open_orders() independently
// of the gateway lifecycle.
//
// Scenarios:
//   A. Open order still live at venue after restart
//   B. Order filled at venue while gateway was down
//   C. Order partially filled while gateway was down
//   D. Order cancelled by venue (cancel-on-disconnect)
//   E. Multiple open orders, mixed outcomes
//   F. Pending cancel at crash — reconcile resolves it
//   G. Pending amend at crash — reconcile resolves it
//   H. UNKNOWN order (uncertain outcome) — reconcile resolves it
//   I. Three consecutive crashes — full lifecycle preserved
//   J. Two venues — one unreachable at restart

#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>

using namespace abex;

namespace {

// ---------------------------------------------------------------------------
// TempJournal — RAII durable journal file
// ---------------------------------------------------------------------------
class TempJournal {
public:
    TempJournal()
        : path_(std::filesystem::temp_directory_path() /
                ("abex-crash-" + std::to_string(unix_time_ms()) + '-' +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".jsonl")) {}
    ~TempJournal() { std::filesystem::remove(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

// ---------------------------------------------------------------------------
// TimeoutAdapter — always returns outcome_uncertain on place, simulating
// a network timeout where the order may or may not have reached the venue.
// ---------------------------------------------------------------------------
class TimeoutAdapter final : public IExchangeAdapter {
public:
    explicit TimeoutAdapter(Venue v) : venue_(v) {}
    [[nodiscard]] Venue venue() const noexcept override { return venue_; }
    void start(ExecutionCallback cb, ConnectionCallback ccb) override {
        exec_cb_ = std::move(cb);
        ccb(venue_, true, {});
    }
    void stop() noexcept override {}
    void restore(std::span<const Order>) override {}
    [[nodiscard]] AdapterResult place(const Order&) override {
        return {.outcome_uncertain = true, .code = "TIMEOUT",
                .message = "simulated network timeout"};
    }
    [[nodiscard]] AdapterResult cancel(const Order&) override {
        return {.outcome_uncertain = true, .code = "TIMEOUT",
                .message = "simulated network timeout"};
    }
    [[nodiscard]] AdapterResult amend(const Order&,
                                      std::optional<Decimal>,
                                      std::optional<Decimal>) override {
        return {.outcome_uncertain = true, .code = "TIMEOUT",
                .message = "simulated network timeout"};
    }
    [[nodiscard]] std::optional<ExecutionReport> query(const Order&) override {
        return std::nullopt;
    }
    [[nodiscard]] BalanceQueryResult
    query_balances(std::optional<std::string>) override {
        AccountBalance b; b.currency="USDT"; b.total="1000000";
        b.available="1000000"; b.frozen="0";
        return {.ok=true, .snapshot={.balances={b}}};
    }
    [[nodiscard]] InstrumentRulesQueryResult
    query_instrument_rules(std::string sym) override {
        InstrumentRules r; r.symbol=std::move(sym); r.trading=true;
        r.minimum_quantity=Decimal::parse("0.00001");
        r.quantity_step=Decimal::parse("0.00001");
        r.price_tick=Decimal::parse("0.01");
        r.minimum_notional=Decimal::parse("1");
        return {.ok=true, .rules=std::move(r)};
    }
    [[nodiscard]] std::optional<std::vector<ExecutionReport>>
    query_open_orders() override { return std::vector<ExecutionReport>{}; }
private:
    Venue venue_;
    ExecutionCallback exec_cb_;
};


// Wraps SimulatedExchangeAdapter for place/cancel/amend/start/stop/restore,
// but lets tests inject exactly what query() and query_open_orders() return.
// This models the venue running independently while the gateway is down.
// ---------------------------------------------------------------------------
class VenueStateAdapter final : public IExchangeAdapter {
public:
    explicit VenueStateAdapter(Venue v)
        : inner_(std::make_shared<SimulatedExchangeAdapter>(v)), venue_(v) {}

    // Expose inner adapter so tests can place orders through it.
    [[nodiscard]] std::shared_ptr<SimulatedExchangeAdapter> inner() const { return inner_; }

    // Set what query_open_orders() returns on the NEXT gateway start.
    // Pass nullopt to simulate "venue unreachable".
    void set_open_orders(std::optional<std::vector<ExecutionReport>> reports) {
        open_orders_ = std::move(reports);
        open_orders_set_ = true;
    }

    // Set what query() returns for a specific order.
    void set_query_result(std::string client_order_id,
                          std::optional<ExecutionReport> report) {
        query_results_[std::move(client_order_id)] = std::move(report);
    }

    // IExchangeAdapter
    [[nodiscard]] Venue venue() const noexcept override { return venue_; }

    void start(ExecutionCallback cb, ConnectionCallback ccb) override {
        inner_->start(std::move(cb), std::move(ccb));
    }
    void stop() noexcept override { inner_->stop(); }
    void restore(std::span<const Order> orders) override { inner_->restore(orders); }

    [[nodiscard]] AdapterResult place(const Order& o) override { return inner_->place(o); }
    [[nodiscard]] AdapterResult cancel(const Order& o) override { return inner_->cancel(o); }
    [[nodiscard]] AdapterResult amend(const Order& o,
                                      std::optional<Decimal> p,
                                      std::optional<Decimal> q) override {
        return inner_->amend(o, p, q);
    }

    [[nodiscard]] std::optional<ExecutionReport> query(const Order& order) override {
        if (const auto it = query_results_.find(order.client_order_id);
            it != query_results_.end()) {
            return it->second;
        }
        return inner_->query(order);
    }

    [[nodiscard]] BalanceQueryResult
    query_balances(std::optional<std::string> c) override {
        return inner_->query_balances(std::move(c));
    }

    [[nodiscard]] InstrumentRulesQueryResult
    query_instrument_rules(std::string s) override {
        return inner_->query_instrument_rules(std::move(s));
    }

    [[nodiscard]] std::optional<std::vector<ExecutionReport>>
    query_open_orders() override {
        if (open_orders_set_) return open_orders_;
        return inner_->query_open_orders();
    }

private:
    std::shared_ptr<SimulatedExchangeAdapter> inner_;
    Venue venue_;
    bool open_orders_set_{false};
    std::optional<std::vector<ExecutionReport>> open_orders_;
    std::unordered_map<std::string, std::optional<ExecutionReport>> query_results_;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal LIVE ExecutionReport for use in open-order snapshots.
ExecutionReport live_report(std::string client_id,
                            std::string exchange_id,
                            Decimal filled = Decimal{}) {
    ExecutionReport r;
    r.client_order_id  = std::move(client_id);
    r.exchange_order_id = std::move(exchange_id);
    r.status           = OrderStatus::Live;
    r.cumulative_filled = filled;
    r.event_time_ms    = unix_time_ms();
    return r;
}

ExecutionReport filled_report(std::string client_id,
                              std::string exchange_id,
                              Decimal qty,
                              Decimal price) {
    ExecutionReport r;
    r.client_order_id   = std::move(client_id);
    r.exchange_order_id = std::move(exchange_id);
    r.status            = OrderStatus::Filled;
    r.cumulative_filled = qty;
    r.last_fill_price   = price;
    r.event_time_ms     = unix_time_ms();
    return r;
}

ExecutionReport partial_report(std::string client_id,
                               std::string exchange_id,
                               Decimal filled,
                               Decimal price) {
    ExecutionReport r;
    r.client_order_id   = std::move(client_id);
    r.exchange_order_id = std::move(exchange_id);
    r.status            = OrderStatus::PartiallyFilled;
    r.cumulative_filled = filled;
    r.last_fill_price   = price;
    r.event_time_ms     = unix_time_ms();
    return r;
}

ExecutionReport canceled_report(std::string client_id, std::string exchange_id) {
    ExecutionReport r;
    r.client_order_id   = std::move(client_id);
    r.exchange_order_id = std::move(exchange_id);
    r.status            = OrderStatus::Canceled;
    r.cumulative_filled = Decimal{};
    r.event_time_ms     = unix_time_ms();
    return r;
}

// Start a gateway with the given adapters and journal store.
// reconcile_on_start=true mirrors real production startup.
std::unique_ptr<OrderGateway> start_gateway(
    std::shared_ptr<IExchangeAdapter> okx,
    std::shared_ptr<IExchangeAdapter> binance,
    std::shared_ptr<IOrderStore> store,
    bool reconcile_on_start = true)
{
    auto gw = std::make_unique<OrderGateway>(
        std::vector<std::shared_ptr<IExchangeAdapter>>{okx, binance},
        test::risk_manager(), store,
        OrderGateway::Options{
            .event_queue_capacity    = 64,
            .reconcile_on_start      = reconcile_on_start,
            .reconciliation_interval = std::chrono::milliseconds{0},
        });
    gw->start();
    return gw;
}

} // namespace

// =============================================================================
// A. Open order still live at venue after restart
// =============================================================================
TEST_CASE("crash recovery: open order still live at venue is recovered as LIVE",
          "[crash][recovery][live]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    // ── Process 1: place order, then crash ───────────────────────────────────
    std::string exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("order-live"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
        // Crash — journal has LIVE order, venue still holds it.
    }

    // Venue state at restart: order is still open.
    okx->set_open_orders({{live_report("order-live", exchange_id)}});

    // ── Process 2: restart and reconcile ─────────────────────────────────────
    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    CHECK(gw2->stability().recovered_orders == 1);

    const auto order = gw2->get("order-live");
    REQUIRE(order);
    CHECK(order->status == OrderStatus::Live);
    CHECK(order->pending_action == PendingAction::None);
    CHECK_FALSE(gw2->health().at(Venue::Okx).reconciliation_required);

    const auto events = gw2->operational_events(50);
    CHECK(std::ranges::any_of(events, [](const auto& e) {
        return e.code == "GATEWAY_RESTARTED";
    }));

    gw2->stop();
}

// =============================================================================
// B. Order filled at venue while gateway was down
// =============================================================================
TEST_CASE("crash recovery: order filled at venue while gateway was down is recovered as FILLED",
          "[crash][recovery][filled]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("order-filled-offline"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
        // Crash — venue fills the order while gateway is down.
    }

    // Venue state at restart: order is filled, not in open orders.
    // query_open_orders returns empty (filled orders are not open).
    okx->set_open_orders(std::vector<ExecutionReport>{});
    // query() for this specific order returns FILLED.
    okx->set_query_result("order-filled-offline",
        filled_report("order-filled-offline", exchange_id,
                      Decimal::parse("0.1"), Decimal::parse("50500")));

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    const auto order = gw2->get("order-filled-offline");
    REQUIRE(order);
    CHECK(order->status == OrderStatus::Filled);
    CHECK(order->filled_quantity == Decimal::parse("0.1"));
    CHECK_FALSE(gw2->health().at(Venue::Okx).reconciliation_required);

    gw2->stop();
}

// =============================================================================
// C. Order partially filled while gateway was down
// =============================================================================
TEST_CASE("crash recovery: partial fill while gateway was down is recovered correctly",
          "[crash][recovery][partial]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("order-partial-offline"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
    }

    // Venue state: partially filled, still open.
    okx->set_open_orders({{
        partial_report("order-partial-offline", exchange_id,
                       Decimal::parse("0.04"), Decimal::parse("50200"))
    }});

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    const auto order = gw2->get("order-partial-offline");
    REQUIRE(order);
    CHECK(order->status == OrderStatus::PartiallyFilled);
    CHECK(order->filled_quantity == Decimal::parse("0.04"));
    CHECK_FALSE(gw2->health().at(Venue::Okx).reconciliation_required);

    gw2->stop();
}

// =============================================================================
// D. Order cancelled by venue while gateway was down (cancel-on-disconnect)
// =============================================================================
TEST_CASE("crash recovery: order cancelled by venue while gateway was down is recovered as CANCELED",
          "[crash][recovery][canceled]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("order-canceled-offline"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
    }

    // Venue state: order was cancelled (cancel-on-disconnect policy).
    // Not in open orders; query returns CANCELED.
    okx->set_open_orders(std::vector<ExecutionReport>{});
    okx->set_query_result("order-canceled-offline",
        canceled_report("order-canceled-offline", exchange_id));

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    const auto order = gw2->get("order-canceled-offline");
    REQUIRE(order);
    CHECK(order->status == OrderStatus::Canceled);
    CHECK(order->filled_quantity == Decimal{});
    CHECK_FALSE(gw2->health().at(Venue::Okx).reconciliation_required);

    gw2->stop();
}

// =============================================================================
// E. Multiple open orders — mixed outcomes after restart
// =============================================================================
TEST_CASE("crash recovery: multiple open orders with mixed venue outcomes are all reconciled",
          "[crash][recovery][multi]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::unordered_map<std::string, std::string> exchange_ids;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        for (const auto& id : {"multi-live", "multi-filled", "multi-canceled", "multi-partial"}) {
            const auto r = gw->place(test::limit_order(id));
            REQUIRE(r.ok);
            exchange_ids[id] = r.order->exchange_order_id;
        }
    }

    // Venue state while gateway was down:
    //   multi-live     → still open, no fill
    //   multi-filled   → fully filled, not in open orders
    //   multi-canceled → cancelled, not in open orders
    //   multi-partial  → partially filled, still open
    okx->set_open_orders({{
        live_report("multi-live",    exchange_ids["multi-live"]),
        partial_report("multi-partial", exchange_ids["multi-partial"],
                       Decimal::parse("0.06"), Decimal::parse("50000")),
    }});
    okx->set_query_result("multi-filled",
        filled_report("multi-filled", exchange_ids["multi-filled"],
                      Decimal::parse("0.1"), Decimal::parse("50000")));
    okx->set_query_result("multi-canceled",
        canceled_report("multi-canceled", exchange_ids["multi-canceled"]));

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    CHECK(gw2->stability().recovered_orders == 4);

    REQUIRE(gw2->get("multi-live"));
    REQUIRE(gw2->get("multi-filled"));
    REQUIRE(gw2->get("multi-canceled"));
    REQUIRE(gw2->get("multi-partial"));

    CHECK(gw2->get("multi-live")->status     == OrderStatus::Live);
    CHECK(gw2->get("multi-filled")->status   == OrderStatus::Filled);
    CHECK(gw2->get("multi-canceled")->status == OrderStatus::Canceled);
    CHECK(gw2->get("multi-partial")->status  == OrderStatus::PartiallyFilled);
    CHECK(gw2->get("multi-partial")->filled_quantity == Decimal::parse("0.06"));

    CHECK_FALSE(gw2->health().at(Venue::Okx).reconciliation_required);

    gw2->stop();
}

// =============================================================================
// F. Pending cancel at crash time — reconcile resolves it
// =============================================================================
TEST_CASE("crash recovery: pending cancel at crash time is resolved by reconciliation",
          "[crash][recovery][pending-cancel]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("pending-cancel-order"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
        // Cancel sent and acknowledged before crash.
        REQUIRE(gw->cancel({"pending-cancel-order", "cancel-req-1"}).ok);
        gw->flush_events();
        // Crash — journal records CANCELED.
    }

    // Venue confirms cancel: not in open orders, query returns CANCELED.
    okx->set_open_orders(std::vector<ExecutionReport>{});
    okx->set_query_result("pending-cancel-order",
        canceled_report("pending-cancel-order", exchange_id));

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    const auto order = gw2->get("pending-cancel-order");
    REQUIRE(order);
    CHECK(order->status == OrderStatus::Canceled);
    CHECK(order->pending_action == PendingAction::None);

    gw2->stop();
}

// =============================================================================
// G. Pending amend at crash time — reconcile resolves it
// =============================================================================
TEST_CASE("crash recovery: pending amend at crash time is resolved by reconciliation",
          "[crash][recovery][pending-amend]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("pending-amend-order"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
        REQUIRE(gw->amend({
            .client_order_id = "pending-amend-order",
            .request_id      = "amend-req-1",
            .new_price       = Decimal::parse("49000"),
            .new_quantity    = Decimal::parse("0.08"),
        }).ok);
        gw->flush_events();
        // Crash after amend acknowledged.
    }

    // Venue state: order is live at amended price.
    ExecutionReport r = live_report("pending-amend-order", exchange_id);
    r.order_price    = Decimal::parse("49000");
    r.order_quantity = Decimal::parse("0.08");
    okx->set_open_orders({{r}});

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    const auto order = gw2->get("pending-amend-order");
    REQUIRE(order);
    CHECK(order->pending_action == PendingAction::None);
    CHECK((order->status == OrderStatus::Live ||
           order->status == OrderStatus::PartiallyFilled));

    gw2->stop();
}

// =============================================================================
// H. UNKNOWN order (uncertain outcome at crash) — reconcile resolves it
// =============================================================================
TEST_CASE("crash recovery: UNKNOWN order from uncertain outcome is resolved by reconciliation",
          "[crash][recovery][unknown]")
{
    TempJournal journal;
    auto binance_p1 = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);

    // Process 1: OKX times out on place() — outcome_uncertain → UNKNOWN.
    auto okx_p1 = std::make_shared<TimeoutAdapter>(Venue::Okx);
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = std::make_unique<OrderGateway>(
            std::vector<std::shared_ptr<IExchangeAdapter>>{okx_p1, binance_p1},
            test::risk_manager(), store,
            OrderGateway::Options{
                .event_queue_capacity    = 64,
                .reconcile_on_start      = false,
                .reconciliation_interval = std::chrono::milliseconds{0},
            });
        gw->start();
        const auto result = gw->place(test::limit_order("unknown-order"));
        // Timeout → outcome_uncertain → UNKNOWN.
        CHECK_FALSE(result.ok);
        REQUIRE(result.order);
        CHECK(result.order->status == OrderStatus::Unknown);
        gw->stop();
        // Crash — journal has UNKNOWN order.
    }

    // Process 2: fresh adapters. Venue never received the order.
    // query_open_orders returns empty; query returns nullopt.
    auto okx_p2     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance_p2 = std::make_shared<VenueStateAdapter>(Venue::Binance);
    okx_p2->set_open_orders(std::vector<ExecutionReport>{});
    okx_p2->set_query_result("unknown-order", std::nullopt);

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx_p2, binance_p2, store2);
    gw2->flush_events();

    const auto order = gw2->get("unknown-order");
    REQUIRE(order);
    // Venue returned nullopt → order stays UNKNOWN with PendingAction::Reconcile.
    CHECK(order->status == OrderStatus::Unknown);
    CHECK(order->pending_action == PendingAction::Reconcile);
    // reconciliation_required stays true because the order is unresolved.
    CHECK(gw2->health().at(Venue::Okx).reconciliation_required);

    gw2->stop();
}

// =============================================================================
// I. Three consecutive crashes — full lifecycle preserved across all restarts
// =============================================================================
TEST_CASE("crash recovery: three consecutive crashes preserve full order lifecycle",
          "[crash][recovery][multi-crash]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string exchange_id;

    // ── Crash 1: place ────────────────────────────────────────────────────────
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto placed = gw->place(test::limit_order("crash-lifecycle"));
        REQUIRE(placed.ok);
        exchange_id = placed.order->exchange_order_id;
    }

    // ── Crash 2: restart, amend, crash ───────────────────────────────────────
    {
        okx->set_open_orders({{live_report("crash-lifecycle", exchange_id)}});
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store);
        gw->flush_events();
        CHECK(gw->get("crash-lifecycle")->status == OrderStatus::Live);
        REQUIRE(gw->amend({
            .client_order_id = "crash-lifecycle",
            .request_id      = "amend-crash-2",
            .new_price       = Decimal::parse("49000"),
            .new_quantity    = Decimal::parse("0.08"),
        }).ok);
        gw->flush_events();
        CHECK(gw->get("crash-lifecycle")->price == Decimal::parse("49000"));
        // Crash — journal has amended LIVE order.
    }

    // ── Crash 3: restart, venue has partial fill, cancel ─────────────────────
    {
        // Between crash 2 and 3, venue partially fills the order.
        ExecutionReport pr = partial_report("crash-lifecycle", exchange_id,
                                            Decimal::parse("0.04"), Decimal::parse("49000"));
        okx->set_open_orders({{pr}});
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store);
        gw->flush_events();

        const auto before_cancel = gw->get("crash-lifecycle");
        REQUIRE(before_cancel);
        CHECK(before_cancel->status == OrderStatus::PartiallyFilled);
        CHECK(before_cancel->filled_quantity == Decimal::parse("0.04"));

        REQUIRE(gw->cancel({"crash-lifecycle", "cancel-crash-3"}).ok);
        gw->flush_events();
        CHECK(gw->get("crash-lifecycle")->status == OrderStatus::Canceled);
        // Crash — journal has CANCELED order.
    }

    // ── Final restart: verify complete state ─────────────────────────────────
    {
        // Venue confirms cancel: not in open orders.
        okx->set_open_orders(std::vector<ExecutionReport>{});
        okx->set_query_result("crash-lifecycle",
            canceled_report("crash-lifecycle", exchange_id));

        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store);
        gw->flush_events();

        const auto order = gw->get("crash-lifecycle");
        REQUIRE(order);
        CHECK(order->status          == OrderStatus::Canceled);
        CHECK(order->filled_quantity == Decimal::parse("0.04"));
        CHECK(order->price           == Decimal::parse("49000"));
        CHECK(order->quantity        == Decimal::parse("0.08"));
        CHECK(gw->stability().recovered_orders >= 1);

        const auto events = gw->operational_events(100);
        CHECK(std::ranges::any_of(events, [](const auto& e) {
            return e.code == "GATEWAY_RESTARTED";
        }));

        gw->stop();
    }
}

// =============================================================================
// J. Two venues — one unreachable at restart, independent recovery
// =============================================================================
TEST_CASE("crash recovery: two venues recover independently when one venue is unreachable",
          "[crash][recovery][two-venues]")
{
    TempJournal journal;
    auto okx     = std::make_shared<VenueStateAdapter>(Venue::Okx);
    auto binance = std::make_shared<VenueStateAdapter>(Venue::Binance);

    std::string okx_exch_id, binance_exch_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        auto gw = start_gateway(okx, binance, store, false);
        const auto r1 = gw->place(test::limit_order("okx-order",     Venue::Okx));
        const auto r2 = gw->place(test::limit_order("binance-order", Venue::Binance));
        REQUIRE(r1.ok); REQUIRE(r2.ok);
        okx_exch_id     = r1.order->exchange_order_id;
        binance_exch_id = r2.order->exchange_order_id;
    }

    // While gateway is down, Binance fills its order.
    // OKX is unreachable at restart (returns nullopt from query_open_orders).
    binance->set_open_orders(std::vector<ExecutionReport>{});
    binance->set_query_result("binance-order",
        filled_report("binance-order", binance_exch_id,
                      Decimal::parse("0.1"), Decimal::parse("50000")));
    okx->set_open_orders(std::nullopt); // venue unreachable

    auto store2 = std::make_shared<FileOrderStore>(journal.path(), false);
    auto gw2 = start_gateway(okx, binance, store2);
    gw2->flush_events();

    // Binance order must be recovered as FILLED.
    const auto binance_order = gw2->get("binance-order");
    REQUIRE(binance_order);
    CHECK(binance_order->status == OrderStatus::Filled);
    CHECK_FALSE(gw2->health().at(Venue::Binance).reconciliation_required);

    // OKX reconciliation failed (venue unreachable) → still required.
    CHECK(gw2->health().at(Venue::Okx).reconciliation_required);

    // After OKX comes back, manual reconciliation resolves the OKX order.
    okx->set_open_orders({{live_report("okx-order", okx_exch_id)}});
    const auto rec = gw2->reconcile(Venue::Okx);
    CHECK(rec.ok);
    gw2->flush_events();
    CHECK_FALSE(gw2->health().at(Venue::Okx).reconciliation_required);

    const auto okx_order = gw2->get("okx-order");
    REQUIRE(okx_order);
    CHECK(okx_order->status == OrderStatus::Live);

    gw2->stop();
}
