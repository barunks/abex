#pragma once
// Included by file_order_store.hpp — not meant to be included directly.

#include "abex/infrastructure/file_order_store.hpp"
#include "abex/domain/operational_event.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace abex {
namespace detail {

// FNV-1a over an arbitrary byte range — corruption detector, not crypto.
inline std::array<char, 16> fnv1a_hex(std::string_view text) {
    std::uint64_t h = 14695981039346656037ULL;
    for (const unsigned char c : text) { h ^= c; h *= 1099511628211ULL; }
    constexpr char hex[] = "0123456789abcdef";
    std::array<char, 16> out{};
    for (int i = 15; i >= 0; --i) { out[static_cast<std::size_t>(i)] = hex[h & 0xfU]; h >>= 4U; }
    return out;
}

inline void write_all(int fd, std::string_view data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const auto n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("failed to append order journal: ") +
                                     std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
}

constexpr std::size_t k_max_cached_events       = 4096;
constexpr std::size_t k_max_cached_order_events = 2048;

inline void insert_event_sorted(std::deque<OperationalEvent>& q,
                                 OperationalEvent ev,
                                 std::size_t max_size) {
    const auto pos = std::upper_bound(
        q.begin(), q.end(), ev.sequence,
        [](std::uint64_t s, const OperationalEvent& e) { return s < e.sequence; });
    q.insert(pos, std::move(ev));
    while (q.size() > max_size) q.pop_front();
}

} // namespace detail

// ── write_record ─────────────────────────────────────────────────────────────
// Assembles the full journal line from a pre-serialized payload string.
// Computes the FNV checksum inline — no second pass over the payload.
// Layout: {"schemaVersion":1,"recordSequence":<n>,"format":"<fmt>",
//          "type":"<t>","writtenAt":<ms>,"payload":<payload>,"checksum":"<hex>"}
// For JsonSerializer the payload is already valid JSON so it embeds directly.
// For KvSerializer the payload is a tab-delimited string embedded as a JSON
// string value (quoted + escaped) so the outer envelope stays valid JSON and
// the existing startup reader can locate and checksum-verify it.

template <JournalSerializer S>
void FileOrderStore<S>::write_record(std::string_view type,
                                     std::string_view payload,
                                     std::uint64_t sequence) {
    const auto cksum = detail::fnv1a_hex(payload);
    const auto now   = unix_time_ms();

    // Pre-size: envelope overhead ~120 bytes + payload + type + format_id.
    std::string line;
    line.reserve(payload.size() + type.size() + S::format_id.size() + 140);

    line += R"({"schemaVersion":1,"recordSequence":)";
    line += std::to_string(sequence);
    line += R"(,"format":")";
    line.append(S::format_id);
    line += R"(","type":")";
    line.append(type);
    line += R"(","writtenAt":)";
    line += std::to_string(now);
    line += R"(,"payload":)";

    if constexpr (std::string_view(S::format_id) == std::string_view("json")) {
        // JSON payload embeds verbatim — it is already valid JSON.
        line.append(payload);
    } else {
        // Non-JSON payload: embed as a JSON string so the envelope stays valid.
        line += '"';
        for (const char c : payload) {
            if (c == '"')  { line += '\\'; line += '"'; }
            else if (c == '\\') { line += '\\'; line += '\\'; }
            else if (c == '\t') { line += '\\'; line += 't'; }
            else line += c;
        }
        line += '"';
    }

    line += R"(,"checksum":")";
    line.append(cksum.data(), cksum.size());
    line += "\"}";
    line += '\n';

    detail::write_all(lock_descriptor_, line);
}

// ── signal_sync ───────────────────────────────────────────────────────────────

template <JournalSerializer S>
void FileOrderStore<S>::signal_sync() {
    sync_generation_.fetch_add(1, std::memory_order_release);
    sync_cv_.notify_one();
}

// ── append / append_order ─────────────────────────────────────────────────────

template <JournalSerializer S>
void FileOrderStore<S>::append(const Order& order) {
    append_order(order, /*intent_only=*/false);
}

template <JournalSerializer S>
void FileOrderStore<S>::append_order(const Order& order, bool intent_only) {
    std::string payload;
    payload.reserve(512);
    S::write_order(payload, order, intent_only);
    const auto sequence = next_seq();
    write_record("ORDER_SNAPSHOT", payload, sequence);
    {
        std::scoped_lock lk(index_mutex_);
        auto& current = latest_orders_[order.client_order_id];
        if (sequence >= current.first) current = {sequence, order};
    }
    if (durable_writes_) signal_sync();
}

template <JournalSerializer S>
std::uint64_t FileOrderStore<S>::reserve_sequence() {
    return next_seq();
}

template <JournalSerializer S>
void FileOrderStore<S>::commit_order(const Order& order,
                                     std::string payload,
                                     std::uint64_t sequence) {
    // O_APPEND + single write() is kernel-atomic for records under PIPE_BUF.
    // latest_orders_ is built in the constructor and updated via append_order();
    // the gateway two-phase path never calls load_latest() after startup.
    write_record("ORDER_SNAPSHOT", payload, sequence);
    (void)order;
    if (durable_writes_) signal_sync();
}

// ── append_event ──────────────────────────────────────────────────────────────

template <JournalSerializer S>
OperationalEvent FileOrderStore<S>::append_event(OperationalEvent event) {
    if (event.occurred_at_ms == 0) event.occurred_at_ms = unix_time_ms();

    std::string payload;
    payload.reserve(256);
    S::write_event(payload, event);

    std::uint64_t sequence;
    // append_event may be called from multiple threads; next_seq() is atomic
    // and write() is kernel-atomic under O_APPEND, so no write lock is needed.
    // index_mutex_ guards the in-memory event caches.
    sequence = next_seq();
    write_record("OPERATIONAL_EVENT", payload, sequence);
    event.sequence = sequence;
    {
        std::scoped_lock lk(index_mutex_);
        detail::insert_event_sorted(recent_events_, event, detail::k_max_cached_events);
        if (!event.client_order_id.empty()) {
            detail::insert_event_sorted(
                recent_order_events_[event.client_order_id],
                event, detail::k_max_cached_order_events);
        }
    }
    if (durable_writes_) signal_sync();
    event.sequence = sequence;
    return event;
}

// ── run_sync_worker ───────────────────────────────────────────────────────────

template <JournalSerializer S>
void FileOrderStore<S>::run_sync_worker() {
    for (;;) {
        std::uint64_t target;
        {
            std::unique_lock lk(sync_mutex_);
            sync_cv_.wait(lk, [this] {
                return sync_stop_ ||
                       sync_generation_.load(std::memory_order_acquire) !=
                           synced_generation_.load(std::memory_order_relaxed);
            });
            if (sync_stop_) {
                if (sync_generation_.load(std::memory_order_acquire) !=
                    synced_generation_.load(std::memory_order_relaxed))
                    ::fdatasync(lock_descriptor_);
                return;
            }
            target = sync_generation_.load(std::memory_order_acquire);
        }
        ::fdatasync(lock_descriptor_);
        synced_generation_.store(target, std::memory_order_release);
    }
}

// ── load_latest / load_events ─────────────────────────────────────────────────

template <JournalSerializer S>
std::vector<Order> FileOrderStore<S>::load_latest() const {
    std::scoped_lock lk(index_mutex_);
    std::vector<Order> result;
    result.reserve(latest_orders_.size());
    for (const auto& [id, entry] : latest_orders_) { (void)id; result.push_back(entry.second); }
    return result;
}

template <JournalSerializer S>
std::vector<OperationalEvent> FileOrderStore<S>::load_events(std::size_t limit) const {
    std::scoped_lock lk(index_mutex_);
    const auto count = std::min(limit, recent_events_.size());
    return {recent_events_.end() - static_cast<std::ptrdiff_t>(count), recent_events_.end()};
}

template <JournalSerializer S>
std::vector<OperationalEvent>
FileOrderStore<S>::load_order_events(std::string_view id, std::size_t limit) const {
    std::scoped_lock lk(index_mutex_);
    const auto found = recent_order_events_.find(id);
    if (found == recent_order_events_.end()) return {};
    const auto count = std::min(limit, found->second.size());
    return {found->second.end() - static_cast<std::ptrdiff_t>(count), found->second.end()};
}

template <JournalSerializer S>
OrderJournalStatus FileOrderStore<S>::status() const {
    return {
        .location       = path_.string(),
        .durable_writes = durable_writes_,
        .record_sequence = next_sequence_.load() - 1,
    };
}

} // namespace abex
