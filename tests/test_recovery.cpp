#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace abex;

namespace {

class TemporaryJournal final {
public:
    TemporaryJournal()
        : path_(std::filesystem::temp_directory_path() /
                ("abex-test-" + std::to_string(unix_time_ms()) + '-' +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".jsonl")) {}
    ~TemporaryJournal() { std::filesystem::remove(path_); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("append-only journal recovers live and filled state", "[recovery]") {
    TemporaryJournal journal;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture first(store);
        REQUIRE(first.gateway->place(test::limit_order("recover-me")).ok);
        REQUIRE(first.okx->emit("recover-me", OrderStatus::PartiallyFilled,
                                Decimal::parse("0.04"), Decimal::parse("50000"), "persisted-fill", 2));
        first.gateway->flush_events();
    }

    auto recovered_store = std::make_shared<FileOrderStore>(journal.path(), false);
    test::GatewayFixture second(recovered_store, {}, true);
    const auto recovered = second.gateway->get("recover-me");
    REQUIRE(recovered);
    CHECK(recovered->status == OrderStatus::PartiallyFilled);
    CHECK(recovered->filled_quantity == Decimal::parse("0.04"));
    CHECK_FALSE(recovered->exchange_order_id.empty());

    REQUIRE(second.okx->emit("recover-me", OrderStatus::Filled, Decimal::parse("0.1"),
                             Decimal::parse("50100"), "post-restart-fill", 3));
    second.gateway->flush_events();
    CHECK(second.gateway->get("recover-me")->status == OrderStatus::Filled);
}

TEST_CASE("journal ignores a torn final append", "[recovery]") {
    TemporaryJournal journal;
    auto order = make_order(test::limit_order("torn-write"));
    order.status = OrderStatus::Live;
    {
        FileOrderStore store(journal.path(), false);
        store.append(order);
    }
    {
        std::ofstream output(journal.path(), std::ios::app);
        output << "{\"schemaVersion\":1,\"payload\":";
    }
    auto second = order;
    second.client_order_id = "after-repair";
    {
        FileOrderStore repaired(journal.path(), false);
        const auto loaded = repaired.load_latest();
        REQUIRE(loaded.size() == 1);
        CHECK(loaded.front().client_order_id == "torn-write");
        repaired.append(second);
    }
    {
        FileOrderStore restarted(journal.path(), false);
        const auto loaded = restarted.load_latest();
        REQUIRE(loaded.size() == 2);
        CHECK(std::ranges::any_of(loaded, [](const auto& recovered) {
            return recovered.client_order_id == "torn-write";
        }));
        CHECK(std::ranges::any_of(loaded, [](const auto& recovered) {
            return recovered.client_order_id == "after-repair";
        }));
    }
}

TEST_CASE("simulated sequence and event identity survive repeated restarts", "[recovery][regression]") {
    TemporaryJournal journal;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture first(store);
        REQUIRE(first.gateway->place(test::limit_order("restart-lifecycle")).ok);
    }
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture second(store, {}, true);
        REQUIRE(second.gateway
                    ->amend({.client_order_id = "restart-lifecycle",
                             .request_id = "restart-amend",
                             .new_price = Decimal::parse("49000"),
                             .new_quantity = Decimal::parse("0.08")})
                    .ok);
        second.gateway->flush_events();
        CHECK(second.gateway->get("restart-lifecycle")->status == OrderStatus::Live);
    }
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture third(store, {}, true);
        REQUIRE(third.gateway->cancel({"restart-lifecycle", "restart-cancel"}).ok);
        third.gateway->flush_events();
        CHECK(third.gateway->get("restart-lifecycle")->status == OrderStatus::Canceled);
        CHECK(third.gateway->get("restart-lifecycle")->pending_action == PendingAction::None);
    }
}

TEST_CASE("restored simulator allocates a new physical id for Binance replacement",
          "[recovery][replacement][regression]") {
    TemporaryJournal journal;
    std::string original_exchange_id;
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture first(store);
        const auto placed = first.gateway->place(
            test::limit_order("restart-binance", Venue::Binance));
        REQUIRE(placed.ok);
        original_exchange_id = placed.order->exchange_order_id;
    }
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture restarted(store, {}, true);
        const auto amended = restarted.gateway->amend({
            .client_order_id = "restart-binance",
            .request_id = "restart-binance-amend",
            .new_price = Decimal::parse("49000"),
            .new_quantity = Decimal::parse("0.08"),
        });
        REQUIRE(amended.ok);
        REQUIRE(amended.order);
        CHECK(amended.order->exchange_order_id != original_exchange_id);
        CHECK(amended.order->exchange_order_id_aliases.contains(original_exchange_id));
        CHECK(amended.order->quantity == Decimal::parse("0.08"));
        CHECK(amended.order->rejection_reason.empty());
    }
}

TEST_CASE("restart and retry events survive in the OMS journal", "[recovery][operations]") {
    TemporaryJournal journal;
    const auto request = test::limit_order("durable-retry");
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture first(store);
        REQUIRE(first.gateway->place(request).ok);
        const auto replay = first.gateway->place(request);
        REQUIRE(replay.ok);
        CHECK(replay.idempotent_replay);
        CHECK(first.gateway->stability().idempotent_replays == 1);
    }
    {
        auto store = std::make_shared<FileOrderStore>(journal.path(), false);
        test::GatewayFixture restarted(store, {}, true);
        CHECK(restarted.gateway->stability().recovered_orders == 1);
        const auto events = restarted.gateway->operational_events();
        CHECK(std::ranges::any_of(events, [](const auto& event) {
            return event.code == "GATEWAY_RESTARTED";
        }));
        CHECK(std::ranges::any_of(events, [](const auto& event) {
            return event.code == "IDEMPOTENT_REPLAY";
        }));
        const auto pipeline = restarted.gateway->order_events("durable-retry");
        const auto sent = std::ranges::find_if(pipeline, [](const auto& event) {
            return event.code == "ORDER_SENT_TO_EXCHANGE";
        });
        REQUIRE(sent != pipeline.end());
        REQUIRE(sent->order.has_value());
        CHECK(sent->order->symbol == "BTC-USDT");
        CHECK(sent->order->pending_action == "NEW");
    }
    FileOrderStore audit(journal.path(), false);
    CHECK(audit.load_latest().size() == 1);
    CHECK(audit.load_events().size() >= 2);
}
