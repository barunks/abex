#pragma once

#include "abex/application/market_data_book.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

namespace abex {

class MarketDataRingFeed final {
public:
    MarketDataRingFeed(std::filesystem::path ring_path,
                       std::shared_ptr<MarketDataBook> book,
                       std::chrono::milliseconds poll_interval = std::chrono::milliseconds{50});
    ~MarketDataRingFeed();

    MarketDataRingFeed(const MarketDataRingFeed&) = delete;
    MarketDataRingFeed& operator=(const MarketDataRingFeed&) = delete;

    void start();
    void stop() noexcept;

private:
    void run(std::stop_token stop_token);

    std::filesystem::path ring_path_;
    std::shared_ptr<MarketDataBook> book_;
    std::chrono::milliseconds poll_interval_;
    std::atomic<bool> running_{false};
    std::jthread thread_;
};

} // namespace abex
