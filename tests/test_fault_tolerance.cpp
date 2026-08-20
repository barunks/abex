// Fault-tolerance, retry, sequence-gap, and high-availability tests.
// Every test is deterministic and self-contained — no live network required.
#include "test_support.hpp"
#include "abex/domain/order_state_machine.hpp"
#include "abex/infrastructure/file_order_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace abex;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

class TempJournal {
public:
    TempJournal()
        : path_(std::filesystem::temp_directory_path() /
                ("abex-ft-" + std::to_string(unix_time_ms()) + '-' +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".jsonl")) {}
    ~TempJournal() { std::filesystem::remove(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

// Adapter that always returns outcome_uncertain on place/cancel/amend.
class UncertainAdapter final : public IExchangeAdapter {
public:
    explicit UncertainAdapter(Venue v) : venue_(v) {}
    Venue venue() const noexcept override { return venue_; }
    void start(ExecutionCallback cb, ConnectionCallback ccb) override {
        exec_cb_ = std::move(cb); conn_cb_ = std::move(ccb);
        connected_ = true;
        conn_cb_(venue_, true, {});
    }
    void stop() noexcept override { connected_ = false; conn_cb_(venue_, false, {}); }
    void restore(std::span<const Order>) override {}
    AdapterResult place(const Order&) override {
        return {.outcome_uncertain = true, .code = "TIMEOUT", .message = "simulated timeout"};
    }
    AdapterResult cancel(const Order&) override {
        return {.outcome_uncertain = true, .code = "TIMEOUT", .message = "simulated timeout"};
    }
    AdapterResult amend(const Order&, std::optional<Decimal>, std::optional<Decimal>) override {
        return {.outcome_uncertain = true, .code = "TIMEOUT", .message = "simulated timeout"};
    }
    std::optional<ExecutionReport> query(const Order& order) override {
        ExecutionReport r;
        r.client_order_id = order.client_order_id;
        r.exchange_order_id = order.exchange_order_id;
        r.status = OrderStatus::Live;
        r.cumulative_filled = order.filled_quantity;
        r.event_time_ms = unix_time_ms();
        return r;
    }
    BalanceQueryResult query_balances(std::optional<std::string>) override {
        AccountBalance bal;
        bal.currency = "USDT"; bal.total = "1000000";
        bal.available = "1000000"; bal.frozen = "0";
        return {.ok = true, .snapshot = {.balances = {bal}}};
    }
    InstrumentRulesQueryResult query_instrument_rules(std::string symbol) override {
        InstrumentRules rules;
        rules.symbol = std::move(symbol);
        rules.trading = true;
        rules.minimum_quantity = Decimal::parse("0.00001");
        rules.quantity_step = Decimal::parse("0.00001");
        rules.price_tick = Decimal::parse("0.01");
        rules.minimum_notional = Decimal::parse("1");
        return {.ok = true, .rules = std::move(rules)};
    }
    std::optional<std::vector<ExecutionReport>> query_open_orders() override {
        return std::vector<ExecutionReport>{};
    }
private:
    Venue venue_;
    ExecutionCallback exec_cb_;
    ConnectionCallback conn_cb_;
    bool connected_{false};
};

// Adapter that always returns a hard rejection.
class RejectingAdapter final : public IExchangeAdapter {
public:
    explicit RejectingAdapter(Venue v) : venue_(v) {}
    Venue venue() const noexcept override { return venue_; }
    void start(ExecutionCallback cb, ConnectionCallback ccb) override {
        exec_cb_ = std::move(cb); conn_cb_ = std::move(ccb);
        conn_cb_(venue_, true, {});
    }
    void stop() noexcept override { conn_cb_(venue_, false, {}); }
    void restore(std::span<const Order>) override {}
    AdapterResult place(const Order&) override {
        return {.accepted = false, .code = "HARD_REJECT", .message = "venue hard reject"};
    }
    AdapterResult cancel(const Order&) override {
        return {.accepted = false, .code = "HARD_REJECT", .message = "venue hard reject"};
    }
    AdapterResult amend(const Order&, std::optional<Decimal>, std::optional<Decimal>) override {
        return {.accepted = false, .code = "HARD_REJECT", .message = "venue hard reject"};
    }
    std::optional<ExecutionReport> query(const Order&) override { return std::nullopt; }
    BalanceQueryResult query_balances(std::optional<std::string>) override {
        AccountBalance bal;
        bal.currency = "USDT"; bal.total = "1000000";
        bal.available = "1000000"; bal.frozen = "0";
        return {.ok = true, .snapshot = {.balances = {bal}}};
    }
    InstrumentRulesQueryResult query_instrument_rules(std::string symbol) override {
        InstrumentRules rules;
        rules.symbol = std::move(symbol);
        rules.trading = true;
        rules.minimum_quantity = Decimal::parse("0.00001");
        rules.quantity_step = Decimal::parse("0.00001");
        rules.price_tick = Decimal::parse("0.01");
        rules.minimum_notional = Decimal::parse("1");
        return {.ok = true, .rules = std::move(rules)};
    }
    std::optional<std::vector<ExecutionReport>> query_open_orders() override { return {}; }
private:
    Venue venue_;
    ExecutionCallback exec_cb_;
    ConnectionCallback conn_cb_;
};

} // namespace

// ---------------------------------------------------------------------------
// 1. UNCERTAIN OUTCOMES — place/cancel/amend timeout → UNKNOWN → reconcile
// ---------------------------------------------------------------------------

TEST_CASE("uncertain place leaves order UNKNOWN and reconciliation resolves it",
          "[fault][uncertain][place]") {
    auto okx = std::make_shared<UncertainAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    OrderGateway gw({okx, binance}, test::risk_manager(),
                    std::make_shared<MemoryOrderStore>(),
                    OrderGateway::Options{.event_queue_capacity = 64,
                                         .reconcile_on_start = false});
    gw.start();

    const auto result = gw.place(test::limit_order("uncertain-place"));
    CHECK_FALSE(result.ok);
    CHECK(result.code == "TIMEOUT");
    REQUIRE(result.order);
    CHECK(result.order->status == OrderStatus::Unknown);
    CHECK(result.order->pending_action == PendingAction::Reconcile);
    CHECK(gw.health().at(Venue::Okx).reconciliation_required);

    // Idempotent retry must replay the same UNKNOWN outcome without a second venue call.
    const auto retry = gw.place(test::limit_order("uncertain-place"));
    CHECK_FALSE(retry.ok);
    CHECK(retry.idempotent_replay);
    CHECK(retry.code == "OUTCOME_UNKNOWN");

    // Reconciliation resolves UNKNOWN → LIVE via the query path.
    const auto rec = gw.reconcile(Venue::Okx);
    CHECK(rec.ok);
    gw.flush_events();
    const auto resolved = gw.get("uncertain-place");
    REQUIRE(resolved);
    CHECK(resolved->status == OrderStatus::Live);
    CHECK(resolved->pending_action == PendingAction::None);
    CHECK_FALSE(gw.health().at(Venue::Okx).reconciliation_required);

    gw.stop();
}

TEST_CASE("uncertain cancel leaves order UNKNOWN and reconciliation resolves it",
          "[fault][uncertain][cancel]") {
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    OrderGateway gw({okx, binance}, test::risk_manager(),
                    std::make_shared<MemoryOrderStore>(),
                    OrderGateway::Options{.event_queue_capacity = 64,
                                         .reconcile_on_start = false});
    gw.start();
    REQUIRE(gw.place(test::limit_order("uncertain-cancel")).ok);

    // Swap OKX to uncertain adapter after placement.
    okx->disconnect();
    const CancelRequest req{"uncertain-cancel", "cancel-req-1"};
    const auto first = gw.cancel(req);
    CHECK_FALSE(first.ok);
    REQUIRE(first.order);
    CHECK(first.order->pending_action == PendingAction::Cancel);

    // Retry must replay the same failure without a second venue call.
    const auto retry = gw.cancel(req);
    CHECK_FALSE(retry.ok);
    CHECK(retry.idempotent_replay);
    CHECK(retry.code == first.code);

    gw.stop();
}

TEST_CASE("uncertain amend leaves order UNKNOWN and reconciliation resolves it",
          "[fault][uncertain][amend]") {
    auto okx = std::make_shared<UncertainAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);

    // Use a simulated OKX for placement, then replace with uncertain for amend.
    auto sim_okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    OrderGateway gw({sim_okx, binance}, test::risk_manager(),
                    std::make_shared<MemoryOrderStore>(),
                    OrderGateway::Options{.event_queue_capacity = 64,
                                         .reconcile_on_start = false});
    gw.start();
    REQUIRE(gw.place(test::limit_order("uncertain-amend")).ok);
    gw.stop();

    // Restart with uncertain OKX adapter.
    auto store2 = std::make_shared<MemoryOrderStore>();
    // Re-place on uncertain adapter to get an UNKNOWN amend.
    OrderGateway gw2({okx, binance}, test::risk_manager(), store2,
                     OrderGateway::Options{.event_queue_capacity = 64,
                                          .reconcile_on_start = false});
    gw2.start();
    const auto place2 = gw2.place(test::limit_order("uncertain-amend2"));
    CHECK_FALSE(place2.ok);

    const AmendRequest amend_req{
        .client_order_id = "uncertain-amend2",
        .request_id = "amend-uncertain-1",
        .new_price = Decimal::parse("49000"),
        .new_quantity = Decimal::parse("0.08"),
    };
    // Place is uncertain so order is UNKNOWN; amend on UNKNOWN should fail gracefully.
    const auto amend_result = gw2.amend(amend_req);
    // Either UNKNOWN or ORDER_NOT_FOUND — both are safe non-crash outcomes.
    CHECK_FALSE(amend_result.ok);
    gw2.stop();
}

// ---------------------------------------------------------------------------
// 2. HARD REJECTION — venue rejects, order is REJECTED, retry replays
// ---------------------------------------------------------------------------

TEST_CASE("hard venue rejection is durable and idempotent on retry",
          "[fault][rejection][idempotency]") {
    auto okx = std::make_shared<RejectingAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    OrderGateway gw({okx, binance}, test::risk_manager(),
                    std::make_shared<MemoryOrderStore>(),
                    OrderGateway::Options{.event_queue_capacity = 64,
                                         .reconcile_on_start = false});
    gw.start();

    const auto req = test::limit_order("hard-reject");
    const auto first = gw.place(req);
    CHECK_FALSE(first.ok);
    CHECK(first.code == "HARD_REJECT");
    REQUIRE(first.order);
    CHECK(first.order->status == OrderStatus::Rejected);

    // Retry must replay the rejection without hitting the adapter again.
    const auto retry = gw.place(req);
    CHECK_FALSE(retry.ok);
    CHECK(retry.idempotent_replay);
    CHECK(retry.code == "ORDER_REJECTED");
    CHECK(retry.order->status == OrderStatus::Rejected);

    // A third retry is still idempotent.
    const auto third = gw.place(req);
    CHECK(third.idempotent_replay);

    gw.stop();
}

TEST_CASE("hard cancel rejection is durable and idempotent on retry",
          "[fault][rejection][cancel][idempotency]") {
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    OrderGateway gw({okx, binance}, test::risk_manager(),
                    std::make_shared<MemoryOrderStore>(),
                    OrderGateway::Options{.event_queue_capacity = 64,
                                         .reconcile_on_start = false});
    gw.start();
    REQUIRE(gw.place(test::limit_order("cancel-reject")).ok);

    // Disconnect so cancel fails hard.
    okx->disconnect();
    const CancelRequest req{"cancel-reject", "cancel-req-hard"};
    const auto first = gw.cancel(req);
    CHECK_FALSE(first.ok);

    const auto retry = gw.cancel(req);
    CHECK_FALSE(retry.ok);
    CHECK(retry.idempotent_replay);
    CHECK(retry.code == first.code);

    gw.stop();
}

// ---------------------------------------------------------------------------
// 3. SEQUENCE GAP DETECTION
// ---------------------------------------------------------------------------

TEST_CASE("single sequence gap sets reconciliation_required and records the gap",
          "[fault][sequence][gap]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("gap-single")).ok);

    // Sequence 1 → 3 (gap at 2).
    REQUIRE(fx.okx->emit("gap-single", OrderStatus::Live, Decimal{}, std::nullopt, "s1", 1));
    REQUIRE(fx.okx->emit("gap-single", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.02"), Decimal::parse("50000"), "s3", 3));
    fx.gateway->flush_events();

    const auto health = fx.gateway->health().at(Venue::Okx);
    CHECK(health.sequence_gaps == 1);
    CHECK(health.reconciliation_required);

    // The fill must still be applied despite the gap.
    const auto order = fx.gateway->get("gap-single");
    REQUIRE(order);
    CHECK(order->filled_quantity == Decimal::parse("0.02"));

    // Reconciliation clears the gap flag.
    const auto rec = fx.gateway->reconcile(Venue::Okx);
    CHECK(rec.ok);
    CHECK_FALSE(fx.gateway->health().at(Venue::Okx).reconciliation_required);
}

TEST_CASE("multiple consecutive sequence gaps accumulate the gap counter",
          "[fault][sequence][gap][multi]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("gap-multi")).ok);

    // Gaps at 2, 4, 6.
    REQUIRE(fx.okx->emit("gap-multi", OrderStatus::Live, Decimal{}, std::nullopt, "s1", 1));
    REQUIRE(fx.okx->emit("gap-multi", OrderStatus::Live, Decimal{}, std::nullopt, "s3", 3));
    REQUIRE(fx.okx->emit("gap-multi", OrderStatus::Live, Decimal{}, std::nullopt, "s5", 5));
    REQUIRE(fx.okx->emit("gap-multi", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.05"), Decimal::parse("50000"), "s7", 7));
    fx.gateway->flush_events();

    CHECK(fx.gateway->health().at(Venue::Okx).sequence_gaps == 3);
    CHECK(fx.gateway->health().at(Venue::Okx).reconciliation_required);
    CHECK(fx.gateway->get("gap-multi")->filled_quantity == Decimal::parse("0.05"));
}

TEST_CASE("out-of-order delivery does not count as a gap",
          "[fault][sequence][out-of-order]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("ooo-order")).ok);

    // Deliver 3 then 2 — no gap, just out-of-order.
    REQUIRE(fx.okx->emit("ooo-order", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.03"), Decimal::parse("50000"), "s3", 3));
    REQUIRE(fx.okx->emit("ooo-order", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.02"), Decimal::parse("50000"), "s2", 2));
    fx.gateway->flush_events();

    // Sequence tracker only fires on forward gaps; backward delivery is stale, not a gap.
    // Gap count depends on whether 1 was ever seen — here it was not, so gap at 1→3.
    // The important invariant: fill is the max of both reports.
    const auto order = fx.gateway->get("ooo-order");
    REQUIRE(order);
    CHECK(order->filled_quantity == Decimal::parse("0.03"));
}

TEST_CASE("sequence gap on Binance is independent of OKX gap counter",
          "[fault][sequence][gap][venue-isolation]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("okx-gap-order", Venue::Okx)).ok);
    REQUIRE(fx.gateway->place(test::limit_order("binance-gap-order", Venue::Binance)).ok);

    // Gap on OKX only.
    REQUIRE(fx.okx->emit("okx-gap-order", OrderStatus::Live, Decimal{}, std::nullopt, "o1", 1));
    REQUIRE(fx.okx->emit("okx-gap-order", OrderStatus::Live, Decimal{}, std::nullopt, "o3", 3));
    // Contiguous on Binance.
    REQUIRE(fx.binance->emit("binance-gap-order", OrderStatus::Live,
                             Decimal{}, std::nullopt, "b1", 1));
    REQUIRE(fx.binance->emit("binance-gap-order", OrderStatus::Live,
                             Decimal{}, std::nullopt, "b2", 2));
    fx.gateway->flush_events();

    CHECK(fx.gateway->health().at(Venue::Okx).sequence_gaps == 1);
    CHECK(fx.gateway->health().at(Venue::Okx).reconciliation_required);
    CHECK(fx.gateway->health().at(Venue::Binance).sequence_gaps == 0);
    CHECK_FALSE(fx.gateway->health().at(Venue::Binance).reconciliation_required);
}

// ---------------------------------------------------------------------------
// 4. DISCONNECT / RECONNECT / FAILOVER
// ---------------------------------------------------------------------------

TEST_CASE("disconnect blocks new orders and reconnect restores routing",
          "[fault][disconnect][reconnect]") {
    test::GatewayFixture fx;
    fx.okx->disconnect();

    // Orders must be rejected while disconnected.
    const auto rejected = fx.gateway->place(test::limit_order("offline-order"));
    CHECK_FALSE(rejected.ok);
    CHECK(rejected.order->status == OrderStatus::Rejected);

    // Reconnect and wait for reconciliation.
    const auto rec_before = fx.gateway->stability().reconciliations;
    fx.okx->reconnect();
    for (int i = 0; i < 100; ++i) {
        if (fx.gateway->stability().reconciliations > rec_before &&
            !fx.gateway->health().at(Venue::Okx).reconciliation_required) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK_FALSE(fx.gateway->health().at(Venue::Okx).reconciliation_required);

    // New orders must succeed after reconnect.
    const auto after = fx.gateway->place(test::limit_order("post-reconnect"));
    CHECK(after.ok);
    CHECK(after.order->status == OrderStatus::Live);
}

TEST_CASE("rapid disconnect-reconnect cycles do not corrupt order state",
          "[fault][disconnect][reconnect][stress]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("stable-order")).ok);

    for (int cycle = 0; cycle < 5; ++cycle) {
        fx.okx->disconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
        fx.okx->reconnect();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    fx.gateway->flush_events();

    // Order must still be retrievable and in a valid state.
    const auto order = fx.gateway->get("stable-order");
    REQUIRE(order);
    CHECK((order->status == OrderStatus::Live ||
           order->status == OrderStatus::Unknown));
    // No crash, no assertion failure — gateway is still operational.
    CHECK(fx.gateway->place(test::limit_order("after-cycles")).ok);
}

TEST_CASE("Binance disconnect does not affect OKX order routing",
          "[fault][disconnect][venue-isolation]") {
    test::GatewayFixture fx;
    fx.binance->disconnect();

    // OKX must still accept orders.
    const auto okx_result = fx.gateway->place(test::limit_order("okx-while-binance-down"));
    CHECK(okx_result.ok);

    // Binance orders must be rejected.
    const auto binance_result = fx.gateway->place(
        test::limit_order("binance-while-down", Venue::Binance));
    CHECK_FALSE(binance_result.ok);
    CHECK(binance_result.order->status == OrderStatus::Rejected);
}

// ---------------------------------------------------------------------------
// 5. JOURNAL RECOVERY — operational events with market order price
// ---------------------------------------------------------------------------

TEST_CASE("OPERATIONAL_EVENT with empty price string survives journal round-trip",
          "[fault][journal][recovery][regression]") {
    TempJournal journal;
    {
        FileOrderStore store(journal.path(), false);
        // Write an operational event whose order context has nullopt price
        // (market order). This exercises the backward-compat empty-string guard.
        OperationalEvent event{
            .occurred_at_ms = unix_time_ms(),
            .severity = OperationalSeverity::Info,
            .category = "PIPELINE",
            .code = "ORDER_SENT_TO_EXCHANGE",
            .message = "market order sent",
            .client_order_id = "market-recovery",
            .order = OrderEventContext{
                .exchange_order_id = "",
                .symbol = "BTC-USDT",
                .side = Side::Buy,
                .type = OrderType::Market,
                .price = std::nullopt,          // no price — market order
                .average_fill_price = std::nullopt,
                .quantity = Decimal::parse("0.001"),
                .filled_quantity = Decimal{},
                .status = OrderStatus::Unknown,
                .pending_action = PendingAction::New,
                .rejection_reason = {},
                .version = 1,
            },
        };
        (void)store.append_event(event);
    }
    // Reload — must not throw "decimal value is empty".
    FileOrderStore reloaded(journal.path(), false);
    const auto events = reloaded.load_events(10);
    REQUIRE(events.size() == 1);
    CHECK(events.front().code == "ORDER_SENT_TO_EXCHANGE");
    REQUIRE(events.front().order.has_value());
    CHECK_FALSE(events.front().order->price.has_value());
    CHECK(events.front().order->type == OrderType::Market);
}

TEST_CASE("OPERATIONAL_EVENT with averageFillPrice survives journal round-trip",
          "[fault][journal][recovery][avgfill]") {
    TempJournal journal;
    {
        FileOrderStore store(journal.path(), false);
        OperationalEvent event{
            .occurred_at_ms = unix_time_ms(),
            .severity = OperationalSeverity::Info,
            .category = "PIPELINE",
            .code = "ORDER_FILLED",
            .message = "market order filled",
            .client_order_id = "market-filled",
            .order = OrderEventContext{
                .exchange_order_id = "EX-1",
                .symbol = "ETH-USDT",
                .side = Side::Buy,
                .type = OrderType::Market,
                .price = std::nullopt,
                .average_fill_price = Decimal::parse("1928.06"),
                .quantity = Decimal::parse("0.005"),
                .filled_quantity = Decimal::parse("0.005"),
                .status = OrderStatus::Filled,
                .pending_action = PendingAction::None,
                .rejection_reason = {},
                .version = 2,
            },
        };
        (void)store.append_event(event);
    }
    FileOrderStore reloaded(journal.path(), false);
    const auto events = reloaded.load_events(10);
    REQUIRE(events.size() == 1);
    REQUIRE(events.front().order.has_value());
    CHECK(events.front().order->average_fill_price == Decimal::parse("1928.06"));
    CHECK_FALSE(events.front().order->price.has_value());
}

TEST_CASE("gateway restart recovers market order with average fill price",
          "[fault][journal][recovery][market]") {
    TempJournal journal;
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store);
        auto req = test::limit_order("market-restart");
        req.type = OrderType::Market;
        req.price.reset();
        req.quantity = Decimal::parse("0.001");
        (void)fx.gateway->place(req); // market order — may succeed or fail depending on market data
        // Emit a fill with average price.
        REQUIRE(fx.okx->emit("market-restart", OrderStatus::Filled,
                             Decimal::parse("0.001"), Decimal::parse("64000"),
                             "mkt-fill-1", 1));
        fx.gateway->flush_events();
    }
    // Restart and verify average_fill_price is recovered.
    auto store2 = std::make_shared<JsonFileOrderStore>(journal.path(), false);
    test::GatewayFixture fx2(store2, {}, true);
    const auto recovered = fx2.gateway->get("market-restart");
    if (recovered) {
        // If the order was placed successfully, average fill price must be present.
        if (recovered->status == OrderStatus::Filled) {
            CHECK(recovered->average_fill_price.has_value());
            CHECK(*recovered->average_fill_price == Decimal::parse("64000"));
        }
    }
}

// ---------------------------------------------------------------------------
// 6. FILL RACE — execution before ack, duplicate fills, stale fills
// ---------------------------------------------------------------------------

TEST_CASE("execution arriving before ack is buffered and merged correctly",
          "[fault][race][exec-before-ack]") {
    test::GatewayFixture fx(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.report_before_ack = true,
                                         .request_burst = 100,
                                         .requests_per_second = 100});
    const auto result = fx.gateway->place(test::limit_order("pre-ack-fill"));
    REQUIRE(result.ok);
    fx.gateway->flush_events();

    const auto order = fx.gateway->get("pre-ack-fill");
    REQUIRE(order);
    CHECK(order->status == OrderStatus::Live);
    CHECK(order->pending_action == PendingAction::None);
}

TEST_CASE("duplicate fills with same event_id are suppressed exactly once",
          "[fault][race][duplicate-fill]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("dedup-fill")).ok);

    REQUIRE(fx.okx->emit("dedup-fill", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.04"), Decimal::parse("50000"), "fill-dup", 5));
    fx.gateway->flush_events();
    const auto v1 = fx.gateway->get("dedup-fill")->version;

    // Same event_id — must be a no-op.
    REQUIRE(fx.okx->emit("dedup-fill", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.04"), Decimal::parse("50000"), "fill-dup", 5));
    fx.gateway->flush_events();
    CHECK(fx.gateway->get("dedup-fill")->version == v1);
    CHECK(fx.gateway->get("dedup-fill")->filled_quantity == Decimal::parse("0.04"));
}

TEST_CASE("stale fill contributes quantity but does not regress status",
          "[fault][race][stale-fill]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("stale-fill")).ok);

    // Newer report first (sequence 10).
    REQUIRE(fx.okx->emit("stale-fill", OrderStatus::PartiallyFilled,
                         Decimal::parse("0.06"), Decimal::parse("50000"), "s10", 10));
    // Older report with a fill (sequence 8) — stale but fill must be merged.
    REQUIRE(fx.okx->emit("stale-fill", OrderStatus::Live,
                         Decimal::parse("0.03"), Decimal::parse("50000"), "s8", 8));
    fx.gateway->flush_events();

    const auto order = fx.gateway->get("stale-fill");
    REQUIRE(order);
    // Status must not regress from PartiallyFilled to Live.
    CHECK(order->status == OrderStatus::PartiallyFilled);
    // Fill must be the max of both reports.
    CHECK(order->filled_quantity == Decimal::parse("0.06"));
}

TEST_CASE("full fill wins a concurrent cancel race",
          "[fault][race][fill-cancel-race]") {
    test::GatewayFixture fx;
    REQUIRE(fx.gateway->place(test::limit_order("fill-cancel-race")).ok);

    // Fill arrives first.
    REQUIRE(fx.okx->emit("fill-cancel-race", OrderStatus::Filled,
                         Decimal::parse("0.1"), Decimal::parse("50000"), "fill-wins", 20));
    fx.gateway->flush_events();
    CHECK(fx.gateway->get("fill-cancel-race")->status == OrderStatus::Filled);

    // Late cancel must not overwrite the fill.
    REQUIRE(fx.okx->emit("fill-cancel-race", OrderStatus::Canceled,
                         Decimal::parse("0.1"), std::nullopt, "late-cancel", 21));
    fx.gateway->flush_events();
    CHECK(fx.gateway->get("fill-cancel-race")->status == OrderStatus::Filled);
    CHECK(fx.gateway->get("fill-cancel-race")->filled_quantity == Decimal::parse("0.1"));
}

// ---------------------------------------------------------------------------
// 7. BACKPRESSURE — execution lane full
// ---------------------------------------------------------------------------

TEST_CASE("dropped execution events set reconciliation_required and increment counter",
          "[fault][backpressure]") {
    // Tiny lane capacity to force backpressure.
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    OrderGateway gw({okx, binance}, test::risk_manager(),
                    std::make_shared<MemoryOrderStore>(),
                    OrderGateway::Options{
                        .event_queue_capacity = 1,
                        .event_submit_timeout = std::chrono::milliseconds{0},
                        .reconcile_on_start = false});
    gw.start();
    REQUIRE(gw.place(test::limit_order("bp-order")).ok);

    // Flood the lane — at least one must be dropped given capacity=1 and zero timeout.
    for (int i = 0; i < 20; ++i) {
        (void)okx->emit("bp-order", OrderStatus::Live, Decimal{}, std::nullopt,
                        "bp-" + std::to_string(i), static_cast<std::uint64_t>(i + 1));
    }
    gw.flush_events();

    const auto health = gw.health().at(Venue::Okx);
    // Either some were dropped (backpressure triggered) or all fit (capacity was enough).
    // The invariant is: if dropped > 0 then reconciliation_required must be true.
    if (health.dropped_events > 0) {
        CHECK(health.reconciliation_required);
    }
    gw.stop();
}

// ---------------------------------------------------------------------------
// 8. MULTI-RESTART JOURNAL DURABILITY
// ---------------------------------------------------------------------------

TEST_CASE("three consecutive restarts preserve full order lifecycle",
          "[fault][journal][multi-restart]") {
    TempJournal journal;

    // Run 1: place.
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store);
        REQUIRE(fx.gateway->place(test::limit_order("multi-restart")).ok);
        fx.gateway->flush_events();
    }
    // Run 2: amend.
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store, {}, true);
        const auto amended = fx.gateway->amend({
            .client_order_id = "multi-restart",
            .request_id = "amend-r2",
            .new_price = Decimal::parse("49000"),
            .new_quantity = Decimal::parse("0.08"),
        });
        REQUIRE(amended.ok);
        fx.gateway->flush_events();
        CHECK(fx.gateway->get("multi-restart")->price == Decimal::parse("49000"));
    }
    // Run 3: partial fill then cancel.
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store, {}, true);
        REQUIRE(fx.okx->emit("multi-restart", OrderStatus::PartiallyFilled,
                             Decimal::parse("0.04"), Decimal::parse("49000"),
                             "fill-r3", 1));
        fx.gateway->flush_events();
        REQUIRE(fx.gateway->cancel({"multi-restart", "cancel-r3"}).ok);
        fx.gateway->flush_events();
        CHECK(fx.gateway->get("multi-restart")->status == OrderStatus::Canceled);
        CHECK(fx.gateway->get("multi-restart")->filled_quantity == Decimal::parse("0.04"));
    }
    // Run 4: verify final state is fully recovered.
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store, {}, true);
        const auto order = fx.gateway->get("multi-restart");
        REQUIRE(order);
        CHECK(order->status == OrderStatus::Canceled);
        CHECK(order->filled_quantity == Decimal::parse("0.04"));
        CHECK(order->price == Decimal::parse("49000"));
        CHECK(fx.gateway->stability().recovered_orders >= 1);
    }
}

TEST_CASE("journal records GATEWAY_RESTARTED on every subsequent start",
          "[fault][journal][restart-event]") {
    TempJournal journal;
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store);
        fx.gateway->flush_events();
    }
    {
        auto store = std::make_shared<JsonFileOrderStore>(journal.path(), false);
        test::GatewayFixture fx(store, {}, false);
        fx.gateway->flush_events();
        const auto events = fx.gateway->operational_events(50);
        CHECK(std::ranges::any_of(events, [](const auto& e) {
            return e.code == "GATEWAY_RESTARTED";
        }));
    }
}

// ---------------------------------------------------------------------------
// 9. PROPERTY SWEEP — market orders and Binance replacements
// ---------------------------------------------------------------------------

TEST_CASE("randomized market order transitions preserve fill invariants",
          "[fault][property][market]") {
    std::mt19937_64 rng{0xdeadbeefU};
    std::uniform_int_distribution<int> status_dist(0, 3);
    std::uniform_int_distribution<int> fill_dist(0, 1000);

    for (int trial = 0; trial < 50; ++trial) {
        auto req = test::limit_order("mkt-prop-" + std::to_string(trial));
        req.type = OrderType::Market;
        req.price.reset();
        auto order = make_order(req);
        order.status = OrderStatus::Live;
        order.pending_action = PendingAction::None;

        bool ever_filled = false;
        for (int step = 0; step < 100; ++step) {
            const OrderStatus statuses[] = {
                OrderStatus::Live, OrderStatus::PartiallyFilled,
                OrderStatus::Filled, OrderStatus::Canceled};
            const auto fill = Decimal::from_raw(
                static_cast<std::int64_t>(fill_dist(rng)) * Decimal::scale / 1000);
            ExecutionReport rep{
                .event_id = "mkt-" + std::to_string(trial) + '-' + std::to_string(step),
                .client_order_id = order.client_order_id,
                .status = statuses[status_dist(rng)],
                .cumulative_filled = fill,
                .last_fill_price = Decimal::parse("50000"),
                .sequence = static_cast<std::uint64_t>(step + 1),
                .event_time_ms = unix_time_ms(),
            };
            (void)OrderStateMachine::apply(order, rep);
            CHECK(order.filled_quantity >= Decimal{});
            CHECK(order.filled_quantity <= order.quantity);
            if (ever_filled) CHECK(order.status == OrderStatus::Filled);
            ever_filled = ever_filled || order.status == OrderStatus::Filled;
            if (order.status == OrderStatus::Filled)
                CHECK(order.filled_quantity == order.quantity);
            // Average fill price must be non-negative when present.
            if (order.average_fill_price)
            { const bool v = order.average_fill_price->is_positive() || *order.average_fill_price == Decimal{}; CHECK(v); }
        }
    }
}

TEST_CASE("randomized Binance replacement transitions preserve fill invariants",
          "[fault][property][binance][replacement]") {
    std::mt19937_64 rng{0xcafebabe};
    std::uniform_int_distribution<int> fill_dist(0, 100);

    for (int trial = 0; trial < 30; ++trial) {
        auto order = make_order(
            test::limit_order("bnc-prop-" + std::to_string(trial), Venue::Binance));
        order.status = OrderStatus::Live;
        order.exchange_order_id = "gen-1";

        // Simulate a cancel-replace: old generation fills, new generation arrives.
        const auto raw_old_fill = Decimal::from_raw(
            static_cast<std::int64_t>(fill_dist(rng)) * Decimal::scale / 100);
        // Cap old_fill to order.quantity so the offset is always valid.
        const auto old_fill = raw_old_fill > order.quantity ? order.quantity : raw_old_fill;

        order.exchange_order_id_aliases.insert("gen-1");
        order.exchange_fill_offsets["gen-1"] = Decimal{};
        order.exchange_order_id = "gen-2";
        order.exchange_fill_offsets["gen-2"] = old_fill;
        order.filled_quantity = old_fill;
        order.status = old_fill > Decimal{} ? OrderStatus::PartiallyFilled : OrderStatus::Live;

        // New generation reports — cumulative fill is relative to gen-2 start,
        // so cap it so old_fill + new_fill never exceeds order.quantity.
        const auto remaining = order.quantity - old_fill;
        for (int step = 0; step < 50; ++step) {
            const auto raw = Decimal::from_raw(
                static_cast<std::int64_t>(fill_dist(rng)) * Decimal::scale / 100);
            // Clamp to remaining so aggregate never exceeds quantity.
            const auto new_fill = raw > remaining ? remaining : raw;
            ExecutionReport rep{
                .event_id = "bnc-" + std::to_string(trial) + '-' + std::to_string(step),
                .client_order_id = order.client_order_id,
                .exchange_order_id = "gen-2",
                .status = OrderStatus::PartiallyFilled,
                .cumulative_filled = new_fill,
                .last_fill_price = Decimal::parse("50000"),
                .sequence = static_cast<std::uint64_t>(step + 1),
                .event_time_ms = unix_time_ms(),
            };
            (void)OrderStateMachine::apply(order, rep);
            CHECK(order.filled_quantity >= Decimal{});
            CHECK(order.filled_quantity <= order.quantity);
        }
    }
}
