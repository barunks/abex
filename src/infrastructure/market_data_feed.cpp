#include "abex/infrastructure/market_data_feed.hpp"

#include "abex/infrastructure/market_data_ring.hpp"

#include <stdexcept>
#include <thread>

namespace abex {

MarketDataRingFeed::MarketDataRingFeed(std::filesystem::path ring_path,
                                       std::shared_ptr<MarketDataBook> book,
                                       std::chrono::milliseconds poll_interval)
    : ring_path_(std::move(ring_path)), book_(std::move(book)),
      poll_interval_(poll_interval) {
    if (!book_) throw std::invalid_argument("market-data book is required");
    if (poll_interval_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("market-data ring poll interval must be positive");
    }
}

MarketDataRingFeed::~MarketDataRingFeed() { stop(); }

void MarketDataRingFeed::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    thread_ = std::jthread([this](std::stop_token stop_token) { run(stop_token); });
}

void MarketDataRingFeed::stop() noexcept {
    if (!running_.exchange(false)) return;
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    const auto current = book_->status();
    book_->set_ring_status(false, current.generation, current.last_sequence,
                           "market-data ring feed stopped");
}

void MarketDataRingFeed::run(std::stop_token stop_token) {
    std::unique_ptr<MarketDataRingReader> reader;
    MarketDataCursor cursor;
    while (!stop_token.stop_requested()) {
        try {
            if (!reader) reader = std::make_unique<MarketDataRingReader>(ring_path_);
            auto quotes = reader->read_available(cursor);
            if (cursor.generation != book_->status().generation) {
                // Sequence numbers restart at one for every writer generation. Clear the
                // previous generation before applying its lower sequence numbers.
                book_->set_ring_status(true, cursor.generation, 0);
            }
            for (auto& quote : quotes) book_->publish(std::move(quote));
            book_->set_ring_status(true, cursor.generation, cursor.sequence);
        } catch (const std::exception& error) {
            reader.reset();
            book_->set_ring_status(false, cursor.generation, cursor.sequence, error.what());
        }
        std::this_thread::sleep_for(poll_interval_);
    }
}

} // namespace abex
