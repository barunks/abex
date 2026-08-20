#include "test_support.hpp"
#include "abex/application/spsc_execution_lane.hpp"
#include "abex/cli/command_processor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

using namespace abex;

namespace {

class FailingEventStore final : public IOrderStore {
public:
    void append(const Order& order) override { memory_.append(order); }
    void append_order(const Order& order, bool intent_only) override { memory_.append_order(order, intent_only); }
    std::uint64_t reserve_sequence() override { return memory_.reserve_sequence(); }
    void commit_order(const Order& order, std::string payload, std::uint64_t sequence) override {
        memory_.commit_order(order, std::move(payload), sequence);
    }
    OperationalEvent append_event(OperationalEvent) override {
        throw std::runtime_error("injected operational append failure");
    }
    std::vector<Order> load_latest() const override { return memory_.load_latest(); }
    std::vector<OperationalEvent> load_events(std::size_t limit) const override {
        return memory_.load_events(limit);
    }
    std::vector<OperationalEvent> load_order_events(std::string_view id,
                                                    std::size_t limit) const override {
        return memory_.load_order_events(id, limit);
    }
    OrderJournalStatus status() const override { return memory_.status(); }

private:
    MemoryOrderStore memory_;
};

} // namespace

TEST_CASE("SPSC execution lane reuses its fixed ring across wraparound",
          "[gateway][performance]") {
    std::atomic<std::size_t> handled{0};
    SpscExecutionLane lane(2, [&](const ExecutionReport&) { ++handled; });

    for (std::size_t index = 0; index < 100; ++index) {
        ExecutionReport report{.event_id = "ring-" + std::to_string(index)};
        REQUIRE(lane.submit(std::move(report), std::chrono::seconds{1}));
    }
    lane.flush();
    CHECK(handled.load() == 100);
    CHECK(lane.size() == 0);
    CHECK_THROWS_AS(SpscExecutionLane(0, [](const ExecutionReport&) {}),
                    std::invalid_argument);
    CHECK_THROWS_AS(SpscExecutionLane(1, {}), std::invalid_argument);
}

TEST_CASE("MPSC execution lane bounds backpressure correctly",
          "[gateway][performance][concurrency]") {
    std::atomic<bool> handler_entered{false};
    std::atomic<bool> release_handler{false};
    std::atomic<bool> waiting_submit_succeeded{false};
    SpscExecutionLane lane(1, [&](const ExecutionReport&) {
        handler_entered.store(true, std::memory_order_release);
        while (!release_handler.load(std::memory_order_acquire)) std::this_thread::yield();
    });

    REQUIRE(lane.submit(ExecutionReport{.event_id = "in-flight"}));
    while (!handler_entered.load(std::memory_order_acquire)) std::this_thread::yield();
    REQUIRE(lane.submit(ExecutionReport{.event_id = "queued"}));

    // A second concurrent producer is now valid (MPSC); it blocks until a slot opens.
    std::jthread producer([&] {
        waiting_submit_succeeded.store(
            lane.submit(ExecutionReport{.event_id = "waiting"},
                        std::chrono::milliseconds{100}),
            std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    // Zero-timeout submit fails only because the queue is full, not due to concurrency.
    const auto full_queue_reject = lane.submit(
        ExecutionReport{.event_id = "full-queue"},
        std::chrono::milliseconds::zero());
    release_handler.store(true, std::memory_order_release);
    producer.join();
    lane.flush();

    CHECK_FALSE(full_queue_reject);
    CHECK(waiting_submit_succeeded.load(std::memory_order_acquire));
}

TEST_CASE("gateway routes both venues and enforces create idempotency", "[gateway]") {
    test::GatewayFixture fixture;
    auto okx_request = test::limit_order("same-id", Venue::Okx);
    const auto first = fixture.gateway->place(okx_request);
    REQUIRE(first.ok);
    CHECK(first.order->exchange_order_id.starts_with("OKX-SIM-"));

    const auto replay = fixture.gateway->place(okx_request);
    CHECK(replay.ok);
    CHECK(replay.idempotent_replay);
    CHECK(replay.order->exchange_order_id == first.order->exchange_order_id);

    auto conflict = okx_request;
    conflict.quantity = Decimal::parse("0.2");
    CHECK(fixture.gateway->place(conflict).code == "IDEMPOTENCY_CONFLICT");

    auto binance_request = test::limit_order("binance-id", Venue::Binance);
    const auto binance = fixture.gateway->place(binance_request);
    REQUIRE(binance.ok);
    CHECK(binance.order->exchange_order_id.starts_with("BINANCE-SIM-"));
}

TEST_CASE("operational journal failures are isolated and visible in stability",
          "[gateway][observability]") {
    test::GatewayFixture fixture(std::make_shared<FailingEventStore>());
    REQUIRE(fixture.gateway->place(test::limit_order("logging-failure")).ok);
    fixture.gateway->flush_events();
    const auto stability = fixture.gateway->stability();
    CHECK(stability.logging_failures > 0);
    CHECK(stability.last_logging_error == "injected operational append failure");
    CHECK(fixture.gateway->get("logging-failure")->status == OrderStatus::Live);
}

TEST_CASE("gateway handles execution-before-ack and duplicate fills", "[gateway][race]") {
    test::GatewayFixture fixture(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.report_before_ack = true,
                                         .request_burst = 100,
                                         .requests_per_second = 100});
    const auto result = fixture.gateway->place(test::limit_order("race-order"));
    REQUIRE(result.ok);
    fixture.gateway->flush_events();
    CHECK(fixture.gateway->get("race-order")->status == OrderStatus::Live);

    REQUIRE(fixture.okx->emit("race-order", OrderStatus::PartiallyFilled,
                              Decimal::parse("0.05"), Decimal::parse("49900"), "fill-a", 4));
    fixture.gateway->flush_events();
    const auto version = fixture.gateway->get("race-order")->version;
    REQUIRE(fixture.okx->emit("race-order", OrderStatus::PartiallyFilled,
                              Decimal::parse("0.05"), Decimal::parse("49900"), "fill-a", 4));
    fixture.gateway->flush_events();
    CHECK(fixture.gateway->get("race-order")->version == version);
}

TEST_CASE("gateway completes amend and cancel lifecycle", "[gateway]") {
    test::GatewayFixture fixture;
    REQUIRE(fixture.gateway->place(test::limit_order("lifecycle")).ok);

    auto amend = fixture.gateway->amend({
        .client_order_id = "lifecycle",
        .request_id = "amend-1",
        .new_price = Decimal::parse("49000"),
        .new_quantity = Decimal::parse("0.08"),
    });
    REQUIRE(amend.ok);
    fixture.gateway->flush_events();
    CHECK(fixture.gateway->get("lifecycle")->price == Decimal::parse("49000"));
    CHECK(fixture.gateway->get("lifecycle")->quantity == Decimal::parse("0.08"));
    CHECK(fixture.gateway->get("lifecycle")->pending_action == PendingAction::None);

    const auto cancel = fixture.gateway->cancel({"lifecycle", "cancel-1"});
    REQUIRE(cancel.ok);
    fixture.gateway->flush_events();
    CHECK(fixture.gateway->get("lifecycle")->status == OrderStatus::Canceled);
    const auto replay = fixture.gateway->cancel({"lifecycle", "cancel-1"});
    CHECK(replay.ok);
    CHECK(replay.idempotent_replay);
}

TEST_CASE("Binance amend rolls physical generations into one canonical order",
          "[gateway][replacement]") {
    test::GatewayFixture fixture;
    REQUIRE(fixture.gateway->place(
                test::limit_order("binance-replace", Venue::Binance))
                .ok);
    const auto original_id = fixture.gateway->get("binance-replace")->exchange_order_id;

    const auto amended = fixture.gateway->amend({
        .client_order_id = "binance-replace",
        .request_id = "replace-1",
        .new_price = Decimal::parse("49000"),
        .new_quantity = Decimal::parse("0.08"),
    });
    REQUIRE(amended.ok);
    const auto current = fixture.gateway->get("binance-replace");
    REQUIRE(current);
    CHECK(current->exchange_order_id != original_id);
    CHECK(current->exchange_order_id_aliases.contains(original_id));
    CHECK(current->price == Decimal::parse("49000"));
    CHECK(current->quantity == Decimal::parse("0.08"));
    CHECK(current->pending_action == PendingAction::None);
    CHECK(current->status == OrderStatus::Live);
}

TEST_CASE("Binance reports arriving before amend acknowledgement are deferred and merged",
          "[gateway][replacement][race]") {
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    auto binance = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Binance,
        SimulatedExchangeAdapter::Config{.amend_reports_before_ack = true});
    OrderGateway gateway(
        {okx, binance}, test::risk_manager(), std::make_shared<MemoryOrderStore>(),
        {.event_queue_capacity = 64, .reconcile_on_start = false});
    gateway.start();
    REQUIRE(gateway.place(test::limit_order("deferred-amend", Venue::Binance)).ok);
    const auto original_id = gateway.get("deferred-amend")->exchange_order_id;

    const auto amended = gateway.amend({
        .client_order_id = "deferred-amend",
        .request_id = "deferred-amend-request",
        .new_price = Decimal::parse("49000"),
        .new_quantity = Decimal::parse("0.08"),
    });
    REQUIRE(amended.ok);
    gateway.flush_events();
    const auto current = gateway.get("deferred-amend");
    REQUIRE(current);
    CHECK(current->exchange_order_id != original_id);
    CHECK(current->exchange_order_id_aliases.contains(original_id));
    CHECK(current->price == Decimal::parse("49000"));
    CHECK(current->quantity == Decimal::parse("0.08"));
    CHECK(current->pending_action == PendingAction::None);
    CHECK(std::ranges::any_of(gateway.order_events("deferred-amend"), [](const auto& event) {
        return event.code == "AMEND_REPORT_DEFERRED";
    }));
    gateway.stop();
}

TEST_CASE("Binance cancel-replace emits REPLACEMENT_QUANTITY_DRIFT when a fill races the amend",
          "[gateway][replacement]") {
    // fill_before_replace injects a fill on the old generation inside authoritative_reports,
    // advancing final_old_fill beyond what the adapter used to compute replacement_quantity.
    // actual_quantity = (filled + race_fill_delta) + replacement_quantity != requested_quantity.
    auto binance = std::make_shared<SimulatedExchangeAdapter>(
        Venue::Binance,
        SimulatedExchangeAdapter::Config{.fill_before_replace = true});
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    OrderGateway gateway(
        {okx, binance}, test::risk_manager(), std::make_shared<MemoryOrderStore>(),
        {.event_queue_capacity = 64, .reconcile_on_start = false});
    gateway.start();

    REQUIRE(gateway.place(test::limit_order("drift-order", Venue::Binance)).ok);
    const auto amended = gateway.amend({
        .client_order_id = "drift-order",
        .request_id      = "amend-drift-1",
        .new_quantity    = Decimal::parse("0.08"),
    });
    REQUIRE(amended.ok);
    gateway.flush_events();

    const auto events = gateway.order_events("drift-order");
    CHECK(std::ranges::any_of(events, [](const OperationalEvent& event) {
        return event.code == "REPLACEMENT_QUANTITY_DRIFT";
    }));
    gateway.stop();
}

TEST_CASE("disconnection fails closed at route preflight and reconnects cleanly", "[gateway]") {
    test::GatewayFixture fixture;
    fixture.okx->disconnect();
    CHECK(fixture.gateway->health().at(Venue::Okx).last_error ==
          "simulated exchange disconnected");
    const auto result = fixture.gateway->place(test::limit_order("offline"));
    CHECK_FALSE(result.ok);
    CHECK(result.code == "INSTRUMENT_RULES_UNAVAILABLE");
    REQUIRE(result.order);
    CHECK(result.order->status == OrderStatus::Rejected);
    CHECK(result.order->pending_action == PendingAction::None);

    const auto reconciliation_count = fixture.gateway->stability().reconciliations;
    fixture.okx->reconnect();
    for (int attempt = 0;
         attempt < 100 &&
         (fixture.gateway->stability().reconciliations == reconciliation_count ||
          fixture.gateway->health().at(Venue::Okx).reconciliation_required);
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    CHECK(fixture.gateway->stability().reconciliations > reconciliation_count);
    CHECK_FALSE(fixture.gateway->health().at(Venue::Okx).reconciliation_required);
}

TEST_CASE("sequence gaps degrade health without losing the report", "[gateway][sequence]") {
    test::GatewayFixture fixture;
    REQUIRE(fixture.gateway->place(test::limit_order("gap-order")).ok);
    REQUIRE(fixture.okx->emit("gap-order", OrderStatus::Live, Decimal{}, std::nullopt,
                              "seq-10", 10));
    REQUIRE(fixture.okx->emit("gap-order", OrderStatus::PartiallyFilled,
                              Decimal::parse("0.02"), Decimal::parse("50000"), "seq-12", 12));
    fixture.gateway->flush_events();
    const auto health = fixture.gateway->health().at(Venue::Okx);
    CHECK(health.sequence_gaps == 1);
    CHECK(health.reconciliation_required);
    CHECK(fixture.gateway->get("gap-order")->filled_quantity == Decimal::parse("0.02"));
}

TEST_CASE("reconciliation ignores account orders outside the durable journal",
          "[gateway][reconciliation][ownership]") {
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    okx->start({}, {});
    auto foreign = make_order(test::limit_order("foreign-account-order"));
    REQUIRE(okx->place(foreign).accepted);
    okx->stop();

    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    auto store = std::make_shared<MemoryOrderStore>();
    OrderGateway gateway(
        {okx, binance}, test::risk_manager(), store,
        OrderGateway::Options{.event_queue_capacity = 64, .reconcile_on_start = false});
    gateway.start();

    const auto result = gateway.reconcile(Venue::Okx);
    CHECK(result.ok);
    CHECK(result.message == "0 orders reconciled against venue state");
    CHECK_FALSE(gateway.health().at(Venue::Okx).reconciliation_required);

    REQUIRE(gateway.place(test::limit_order("journal-owned-order")).ok);
    const auto owned_result = gateway.reconcile(Venue::Okx);
    CHECK(owned_result.ok);
    CHECK(owned_result.message == "1 orders reconciled against venue state");

    const auto events = gateway.operational_events();
    CHECK(std::ranges::none_of(events, [](const auto& event) {
        return event.code == "ORPHAN_OPEN_ORDER" ||
               event.code == "RECONCILIATION_INCOMPLETE";
    }));
}

TEST_CASE("reconciliation flags a journal-terminal order still open at the venue",
          "[gateway][reconciliation][conflict]") {
    test::GatewayFixture fixture(
        std::make_shared<MemoryOrderStore>(),
        SimulatedExchangeAdapter::Config{.report_terminal_orders_as_open = true});
    REQUIRE(fixture.gateway->place(test::limit_order("terminal-still-open")).ok);
    REQUIRE(fixture.okx->emit(
        "terminal-still-open", OrderStatus::Filled, Decimal::parse("0.1"),
        Decimal::parse("50000"), "terminal-local-fill", 10));
    fixture.gateway->flush_events();
    REQUIRE(fixture.gateway->get("terminal-still-open")->status == OrderStatus::Filled);

    const auto result = fixture.gateway->reconcile(Venue::Okx);
    CHECK_FALSE(result.ok);
    CHECK(result.code == "RECONCILIATION_INCOMPLETE");
    CHECK(fixture.gateway->health().at(Venue::Okx).reconciliation_required);
    CHECK(std::ranges::any_of(
        fixture.gateway->order_events("terminal-still-open"), [](const auto& event) {
            return event.code == "TERMINAL_ORDER_STILL_OPEN";
        }));
}

TEST_CASE("account-wide execution reports outside the journal are silently ignored",
          "[gateway][stream][ownership]") {
    auto okx = std::make_shared<SimulatedExchangeAdapter>(Venue::Okx);
    okx->start({}, {});
    auto foreign = make_order(test::limit_order("foreign-stream-order"));
    REQUIRE(okx->place(foreign).accepted);
    okx->stop();

    auto binance = std::make_shared<SimulatedExchangeAdapter>(Venue::Binance);
    auto store = std::make_shared<MemoryOrderStore>();
    OrderGateway gateway(
        {okx, binance}, test::risk_manager(), store,
        OrderGateway::Options{.event_queue_capacity = 64, .reconcile_on_start = false});
    gateway.start();

    REQUIRE(okx->emit("foreign-stream-order", OrderStatus::PartiallyFilled,
                      Decimal::parse("0.01"), Decimal::parse("50000"),
                      "foreign-fill", 100));
    gateway.flush_events();

    const auto health = gateway.health().at(Venue::Okx);
    CHECK_FALSE(health.reconciliation_required);
    CHECK(health.sequence_gaps == 0);
    const auto events = gateway.operational_events();
    CHECK(std::ranges::none_of(events, [](const auto& event) {
        return event.code == "UNCORRELATED_EXECUTION" ||
               event.code == "EXECUTION_SEQUENCE_GAP";
    }));
}

TEST_CASE("risk rejections are durable orders and never reach the adapter", "[gateway][risk]") {
    test::GatewayFixture fixture;
    auto request = test::limit_order("too-large");
    request.quantity = Decimal::parse("3");
    const auto result = fixture.gateway->place(request);
    CHECK_FALSE(result.ok);
    CHECK(result.code == "MAX_ORDER_SIZE");
    CHECK(fixture.gateway->get("too-large")->status == OrderStatus::Rejected);
    CHECK_FALSE(fixture.okx->query(*fixture.gateway->get("too-large")));
    const auto replay = fixture.gateway->place(request);
    CHECK_FALSE(replay.ok);
    CHECK(replay.idempotent_replay);
}

TEST_CASE("insufficient available balance rejects before venue order I/O",
          "[gateway][risk][balance]") {
    SimulatedExchangeAdapter::Config config;
    config.initial_balances = {
        {"BTC", Decimal::parse("1")},
        {"USDT", Decimal::parse("100")},
    };
    test::GatewayFixture fixture(std::make_shared<MemoryOrderStore>(), std::move(config));
    const auto request = test::limit_order("underfunded", Venue::Okx, Side::Buy,
                                           "0.01", "50000");
    const auto result = fixture.gateway->place(request);

    CHECK_FALSE(result.ok);
    CHECK(result.code == "INSUFFICIENT_AVAILABLE_BALANCE");
    REQUIRE(result.order);
    CHECK(result.order->status == OrderStatus::Rejected);
    CHECK(result.message.find("available USDT balance 100") != std::string::npos);
    CHECK(result.message.find("required 500") != std::string::npos);
    CHECK_FALSE(fixture.okx->query(*result.order));

    const auto balance = fixture.gateway->balances(Venue::Okx, "USDT");
    REQUIRE(balance.ok);
    REQUIRE(balance.snapshot.balances.size() == 1);
    CHECK(balance.snapshot.balances.front().available == "100");
}

TEST_CASE("venue minimums steps and ticks reject before order I/O",
          "[gateway][risk][rules]") {
    SECTION("OKX minimum quantity") {
        test::GatewayFixture fixture;
        auto request = test::limit_order("okx-below-minimum", Venue::Okx, Side::Buy,
                                         "0.00000008", "3000");
        request.symbol = "ETH-USDT";
        request.type = OrderType::Market;
        request.price.reset();
        const auto result = fixture.gateway->place(request);
        CHECK_FALSE(result.ok);
        CHECK(result.code == "MIN_ORDER_QUANTITY");
        CHECK(result.message.find("venue minimum 0.0001") != std::string::npos);
        REQUIRE(result.order);
        CHECK_FALSE(fixture.okx->query(*result.order));
        const auto events = fixture.gateway->order_events(request.client_order_id);
        CHECK(std::ranges::none_of(events, [](const OperationalEvent& event) {
            return event.code == "ORDER_SENT_TO_EXCHANGE";
        }));
    }

    SECTION("Binance minimum notional") {
        test::GatewayFixture fixture;
        const auto request = test::limit_order("binance-below-notional", Venue::Binance,
                                               Side::Buy, "0.00001", "50000");
        const auto result = fixture.gateway->place(request);
        CHECK_FALSE(result.ok);
        CHECK(result.code == "MIN_ORDER_NOTIONAL");
        CHECK(result.message.find("venue minimum 5") != std::string::npos);
        REQUIRE(result.order);
        CHECK_FALSE(fixture.binance->query(*result.order));
    }

    SECTION("OKX price tick") {
        test::GatewayFixture fixture;
        const auto request = test::limit_order("okx-invalid-tick", Venue::Okx,
                                               Side::Buy, "0.1", "50000.01");
        const auto result = fixture.gateway->place(request);
        CHECK_FALSE(result.ok);
        CHECK(result.code == "INVALID_PRICE_TICK");
        REQUIRE(result.order);
        CHECK_FALSE(fixture.okx->query(*result.order));
    }
}

TEST_CASE("CLI returns selected-route balance and quantity guidance", "[cli][balance]") {
    test::GatewayFixture fixture;
    CommandProcessor processor(
        *fixture.gateway,
        {{Venue::Okx, fixture.okx}, {Venue::Binance, fixture.binance}});

    const auto response = processor.execute(
        "balances --venue OKX --symbol BTC-USDT --side BUY --price 50000");
    const auto json = nlohmann::json::parse(response.output);
    CHECK(json.at("ok") == true);
    CHECK(json.at("venue") == "OKX");
    CHECK(json.at("symbol") == "BTC-USDT");
    CHECK(json.at("side") == "BUY");
    CHECK(json.at("fundingCurrency") == "USDT");
    REQUIRE(json.at("balances").size() == 1);
    CHECK(json.at("balances").front().at("currency") == "USDT");
    CHECK(json.at("balances").front().at("available") == "1000000");
    CHECK(json.at("quantityGuidance").at("minimumRouteableQuantity") == "0.00001");
    CHECK(json.at("quantityGuidance").at("suggestedMaxQuantity") == "19.9");
    CHECK(json.at("quantityGuidance").at("routeable") == true);

    const auto broad = nlohmann::json::parse(
        processor.execute("balances --venue OKX --currency USDT").output);
    CHECK(broad.at("ok") == false);
    CHECK(broad.at("code") == "INVALID_COMMAND");

    const auto rules = nlohmann::json::parse(
        processor.execute("rules --venue OKX --symbol ETH-USDT").output);
    CHECK(rules.at("ok") == true);
    CHECK(rules.at("minimumQuantity") == "0.0001");
    CHECK(rules.at("quantityStep") == "0.000001");
}

TEST_CASE("failed operation retries replay the original outcome", "[gateway][idempotency]") {
    test::GatewayFixture fixture;
    REQUIRE(fixture.gateway->place(test::limit_order("cancel-offline")).ok);
    fixture.okx->disconnect();
    const CancelRequest request{"cancel-offline", "offline-cancel-1"};
    const auto first = fixture.gateway->cancel(request);
    CHECK_FALSE(first.ok);
    const auto replay = fixture.gateway->cancel(request);
    CHECK_FALSE(replay.ok);
    CHECK(replay.idempotent_replay);
    CHECK(replay.code == first.code);
}
