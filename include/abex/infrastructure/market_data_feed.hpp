#pragma once

#include "abex/application/market_data_book.hpp"
#include "abex/infrastructure/market_data_ring.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <thread>

namespace abex {

class MarketDataRingFeed final {
public:
    MarketDataRingFeed(std::filesystem::path ring_path,
                       std::shared_ptr<MarketDataBook> book,
                       std::chrono::milliseconds poll_interval = std::chrono::milliseconds{50},
                       std::string transport = "PUBLIC_REST")
        : ring_path_(std::move(ring_path)), book_(std::move(book)),
          poll_interval_(poll_interval), transport_(std::move(transport)) {
        if (!book_) throw std::invalid_argument("market-data book is required");
        if (poll_interval_ <= std::chrono::milliseconds::zero())
            throw std::invalid_argument("market-data ring poll interval must be positive");
    }

    ~MarketDataRingFeed() { stop(); }

    MarketDataRingFeed(const MarketDataRingFeed&) = delete;
    MarketDataRingFeed& operator=(const MarketDataRingFeed&) = delete;

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        thread_ = std::jthread([this](std::stop_token st) { run(st); });
    }

    void stop() noexcept {
        if (!running_.exchange(false)) return;
        thread_.request_stop();
        if (thread_.joinable()) thread_.join();
        const auto current = book_->status();
        book_->set_ring_status(false, current.generation, current.last_sequence,
                               "market-data ring feed stopped", transport_);
    }

private:
    void run(std::stop_token stop_token) {
        std::unique_ptr<MarketDataRingReader> reader;
        std::vector<MarketQuote> quote_buffer;
        MarketDataCursor cursor;
        while (!stop_token.stop_requested()) {
            try {
                if (!reader) {
                    reader = std::make_unique<MarketDataRingReader>(ring_path_);
                    quote_buffer.resize(reader->capacity());
                }
                const auto quote_count = reader->read_available(cursor, quote_buffer);
                if (cursor.generation != book_->status().generation)
                    book_->set_ring_status(true, cursor.generation, 0, {}, transport_);
                for (std::size_t i = 0; i < quote_count; ++i)
                    book_->publish(quote_buffer[i]);
                book_->set_ring_status(true, cursor.generation, cursor.sequence, {}, transport_);
            } catch (const std::exception& error) {
                reader.reset();
                quote_buffer.clear();
                book_->set_ring_status(false, cursor.generation, cursor.sequence,
                                       error.what(), transport_);
            }
            std::this_thread::sleep_for(poll_interval_);
        }
    }

    std::filesystem::path ring_path_;
    std::shared_ptr<MarketDataBook> book_;
    std::chrono::milliseconds poll_interval_;
    std::string transport_;
    std::atomic<bool> running_{false};
    std::jthread thread_;
};

} // namespace abex
