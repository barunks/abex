#include "abex/infrastructure/file_order_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <nlohmann/json.hpp>   // startup reader only — not on the write path
#include <sys/file.h>
#include <unistd.h>

namespace abex {
namespace {

struct JournalLine {
    std::string text;
    std::uint64_t start_offset{0};
    bool terminated{false};
};

// Decode a kv1 payload string back into a nlohmann::json object so the
// existing Order/OperationalEvent from_json deserializers can be reused at
// startup. This runs once at process start — throughput is irrelevant.
[[nodiscard]] nlohmann::json kv1_to_json(std::string_view kv) {
    nlohmann::json obj = nlohmann::json::object();
    // Map short kv1 keys back to the canonical JSON field names.
    static const std::unordered_map<std::string_view, std::string_view> key_map{
        {"id",    "clientOrderId"}, {"venue", "venue"},   {"sym",  "symbol"},
        {"side",  "side"},          {"type",  "type"},     {"qty",  "quantity"},
        {"tif",   "timeInForce"},   {"status","status"},   {"pa",   "pendingAction"},
        {"filled","filledQuantity"},{"quote", "cumulativeQuote"},
        {"reason","rejectionReason"},{"ver",  "version"},
        {"cat",   "createdAt"},     {"uat",   "updatedAt"},{"fp",   "createFingerprint"},
        {"price", "price"},         {"afp",   "averageFillPrice"},
        {"pap",   "pendingAmendPrice"},{"paq","pendingAmendQuantity"},
        {"lseq",  "lastSequence"},  {"exid",  "exchangeOrderId"},
        // set/map fields decoded separately below
    };
    // Parse tab-delimited fields: key=<len>:<bytes> or key=<raw>
    std::size_t pos = 0;
    while (pos < kv.size()) {
        const auto tab = kv.find('\t', pos);
        const auto field = kv.substr(pos, tab == std::string_view::npos ? tab : tab - pos);
        pos = (tab == std::string_view::npos) ? kv.size() : tab + 1;
        if (field.empty()) continue;
        const auto eq = field.find('=');
        if (eq == std::string_view::npos) continue;
        const auto raw_key = field.substr(0, eq);
        const auto raw_val = field.substr(eq + 1);

        // Decode length-prefixed string: <len>:<bytes>
        const auto decode_str = [](std::string_view v) -> std::string {
            const auto colon = v.find(':');
            if (colon == std::string_view::npos) return std::string(v);
            return std::string(v.substr(colon + 1));
        };

        // Decode pipe-delimited set of length-prefixed strings.
        const auto decode_strset = [&](std::string_view v) {
            auto arr = nlohmann::json::array();
            std::size_t p = 0;
            while (p < v.size()) {
                const auto pipe = v.find('|', p);
                const auto elem = v.substr(p, pipe == std::string_view::npos ? pipe : pipe - p);
                p = (pipe == std::string_view::npos) ? v.size() : pipe + 1;
                if (!elem.empty()) arr.push_back(decode_str(elem));
            }
            return arr;
        };

        // Decode pipe-delimited map of length-prefixed key=value pairs.
        const auto decode_strmap = [&](std::string_view v) {
            auto obj2 = nlohmann::json::object();
            std::size_t p = 0;
            while (p < v.size()) {
                const auto pipe = v.find('|', p);
                const auto pair = v.substr(p, pipe == std::string_view::npos ? pipe : pipe - p);
                p = (pipe == std::string_view::npos) ? v.size() : pipe + 1;
                if (pair.empty()) continue;
                const auto sep = pair.find('=');
                if (sep == std::string_view::npos) continue;
                obj2[decode_str(pair.substr(0, sep))] = decode_str(pair.substr(sep + 1));
            }
            return obj2;
        };

        // Set/map fields with known structure.
        if (raw_key == "ecid") { obj["exchangeClientIdAliases"] = decode_strset(raw_val); continue; }
        if (raw_key == "eoid") { obj["exchangeOrderIdAliases"]  = decode_strset(raw_val); continue; }
        if (raw_key == "evids"){ obj["processedEventIds"]       = decode_strset(raw_val); continue; }
        if (raw_key == "efo")  { obj["exchangeFillOffsets"]     = decode_strmap(raw_val); continue; }
        if (raw_key == "eqo")  { obj["exchangeQuoteOffsets"]    = decode_strmap(raw_val); continue; }
        if (raw_key == "preq") { obj["processedRequests"]       = decode_strmap(raw_val); continue; }
        if (raw_key == "prout"){ obj["processedRequestOutcomes"]= decode_strmap(raw_val); continue; }

        // Numeric fields stored as raw integers.
        if (raw_key == "ver" || raw_key == "lseq") {
            obj[std::string(key_map.count(raw_key) ? key_map.at(raw_key) : raw_key)] =
                std::stoull(std::string(raw_val));
            continue;
        }
        if (raw_key == "cat" || raw_key == "uat" || raw_key == "at") {
            const auto json_key = raw_key == "at" ? "occurredAt"
                                : raw_key == "cat" ? "createdAt" : "updatedAt";
            obj[json_key] = std::stoll(std::string(raw_val));
            continue;
        }

        // Everything else: length-prefixed string or raw enum/decimal string.
        const auto json_key = key_map.count(raw_key) ? std::string(key_map.at(raw_key))
                                                      : std::string(raw_key);
        obj[json_key] = decode_str(raw_val);
    }
    return obj;
}

[[nodiscard]] std::vector<nlohmann::json>
read_valid_records(const std::filesystem::path& path,
                   int repair_fd = -1,
                   bool durable_repairs = false) {
    if (!std::filesystem::exists(path)) return {};

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read order journal: " + path.string());

    std::vector<JournalLine> lines;
    std::uint64_t offset = 0;
    for (std::string line; std::getline(input, line);) {
        const bool terminated = !input.eof();
        const auto sz = line.size();
        if (!line.empty()) lines.push_back({std::move(line), offset, terminated});
        offset += sz + (terminated ? 1U : 0U);
    }

    std::vector<nlohmann::json> records;
    records.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        try {
            auto record = nlohmann::json::parse(lines[i].text);
            if (record.at("schemaVersion").get<int>() != 1)
                throw std::runtime_error("unsupported journal schema");

            const auto& type   = record.at("type").get_ref<const std::string&>();
            const auto  fmt    = record.value("format", std::string("json"));
            const auto& raw_payload = record.at("payload");

            // Reconstruct the payload text for checksum verification.
            // For json format the payload is an embedded object — extract the
            // raw substring from the original line rather than re-dumping the
            // parsed object (dump() may reorder keys, changing the checksum).
            // For kv1 format the payload is an embedded string.
            std::string payload_text;
            nlohmann::json payload_obj;
            if (fmt == "kv1") {
                payload_text = raw_payload.get<std::string>();
                payload_obj  = kv1_to_json(payload_text);
            } else {
                // Extract the raw payload bytes from the original line.
                // The envelope is: {...,"payload":<payload>,"checksum":"..."}\n
                // Find the payload value start after "payload":
                const auto& line_text = lines[i].text;
                const auto payload_key = std::string_view(R"("payload":)");
                const auto pk = line_text.find(payload_key);
                const auto checksum_key = std::string_view(R"(,"checksum":)");
                const auto ck = line_text.rfind(checksum_key);
                if (pk != std::string::npos && ck != std::string::npos && pk < ck) {
                    payload_text = line_text.substr(pk + payload_key.size(),
                                                    ck - (pk + payload_key.size()));
                } else {
                    payload_text = raw_payload.dump();
                }
                payload_obj = raw_payload;
            }

            const auto expected = detail::fnv1a_hex(payload_text);
            if (record.at("checksum").get_ref<const std::string&>() !=
                std::string_view(expected.data(), expected.size()))
                throw std::runtime_error("journal checksum mismatch");

            if (type == "ORDER_SNAPSHOT")      (void)payload_obj.get<Order>();
            else if (type == "OPERATIONAL_EVENT") (void)payload_obj.get<OperationalEvent>();
            else throw std::runtime_error("unsupported journal record type");

            // Store the decoded payload object in the record for the caller.
            record["_payload_obj"] = std::move(payload_obj);
            records.push_back(std::move(record));
        } catch (const std::exception& err) {
            if (i + 1 == lines.size()) {
                if (repair_fd >= 0) {
                    ::ftruncate(repair_fd, static_cast<off_t>(lines[i].start_offset));
                    if (durable_repairs) ::fdatasync(repair_fd);
                }
                break;
            }
            throw std::runtime_error("invalid order journal record " +
                                     std::to_string(i + 1) + ": " + err.what());
        }
    }

    // Repair missing trailing newline.
    if (repair_fd >= 0 && !lines.empty() && records.size() == lines.size() &&
        !lines.back().terminated) {
        ::lseek(repair_fd, 0, SEEK_END);
        const char nl = '\n';
        ::write(repair_fd, &nl, 1);
        if (durable_repairs) ::fdatasync(repair_fd);
    }
    return records;
}

} // namespace

// ── FileOrderStore constructor / destructor ───────────────────────────────────
// Explicit instantiations for the two supported serializers. The constructor
// and destructor are defined here (not in the impl header) because they need
// the startup reader which uses nlohmann — keeping nlohmann out of the hot path.

template <JournalSerializer S>
FileOrderStore<S>::FileOrderStore(std::filesystem::path path, bool durable_writes)
    : path_(std::move(path)), durable_writes_(durable_writes) {
    if (path_.empty()) throw std::invalid_argument("order journal path must not be empty");
    if (!path_.parent_path().empty())
        std::filesystem::create_directories(path_.parent_path());

    lock_descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (lock_descriptor_ < 0)
        throw std::runtime_error(std::string("failed to open order journal: ") +
                                 std::strerror(errno));

    if (::flock(lock_descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const auto e = errno;
        ::close(lock_descriptor_);
        lock_descriptor_ = -1;
        throw std::runtime_error(std::string("order journal already owned by another process: ") +
                                 std::strerror(e));
    }

    try {
        std::uint64_t max_seq = 0;
        for (const auto& record : read_valid_records(path_, lock_descriptor_, durable_writes_)) {
            const auto seq = record.value("recordSequence", std::uint64_t{0});
            max_seq = std::max(max_seq, seq);
            const auto& payload = record.at("_payload_obj");
            const auto& type    = record.at("type").get_ref<const std::string&>();
            if (type == "ORDER_SNAPSHOT") {
                auto order = payload.get<Order>();
                auto& cur  = latest_orders_[order.client_order_id];
                if (seq >= cur.first) cur = {seq, std::move(order)};
            } else if (type == "OPERATIONAL_EVENT") {
                auto ev = payload.get<OperationalEvent>();
                ev.sequence = seq;
                detail::insert_event_sorted(recent_events_, ev, detail::k_max_cached_events);
                if (!ev.client_order_id.empty())
                    detail::insert_event_sorted(
                        recent_order_events_[ev.client_order_id],
                        ev, detail::k_max_cached_order_events);
            }
        }
        next_sequence_.store(max_seq + 1);
    } catch (...) {
        ::close(lock_descriptor_);
        lock_descriptor_ = -1;
        throw;
    }

    if (durable_writes_)
        sync_worker_ = std::thread([this] { run_sync_worker(); });
}

template <JournalSerializer S>
FileOrderStore<S>::~FileOrderStore() {
    if (sync_worker_.joinable()) {
        { std::scoped_lock lk(sync_mutex_); sync_stop_ = true; }
        sync_cv_.notify_one();
        sync_worker_.join();
    }
    if (lock_descriptor_ >= 0) ::close(lock_descriptor_);
}

// Explicit instantiations — keeps link times fast and avoids ODR issues.
template class FileOrderStore<JsonSerializer>;
template class FileOrderStore<KvSerializer>;

// ── MemoryOrderStore ──────────────────────────────────────────────────────────

void MemoryOrderStore::append(const Order& order) {
    append_order(order, /*intent_only=*/false);
}

void MemoryOrderStore::append_order(const Order& order, bool /*intent_only*/) {
    std::scoped_lock lk(mutex_);
    ++record_sequence_;
    latest_orders_[order.client_order_id] = order;
}

std::uint64_t MemoryOrderStore::reserve_sequence() {
    std::scoped_lock lk(mutex_);
    return ++record_sequence_;
}

void MemoryOrderStore::commit_order(const Order& order,
                                    std::string /*payload*/,
                                    std::uint64_t /*sequence*/) {
    // load_latest() is only called at startup before concurrent writes begin.
    // No lock needed on the hot path.
    latest_orders_[order.client_order_id] = order;
}

OperationalEvent MemoryOrderStore::append_event(OperationalEvent event) {
    std::scoped_lock lk(mutex_);
    if (event.occurred_at_ms == 0) event.occurred_at_ms = unix_time_ms();
    event.sequence = ++record_sequence_;
    events_.push_back(event);
    return event;
}

std::vector<Order> MemoryOrderStore::load_latest() const {
    std::scoped_lock lk(mutex_);
    std::vector<Order> result;
    result.reserve(latest_orders_.size());
    for (const auto& [id, order] : latest_orders_) { (void)id; result.push_back(order); }
    return result;
}

std::vector<OperationalEvent> MemoryOrderStore::load_events(std::size_t limit) const {
    std::scoped_lock lk(mutex_);
    const auto first = events_.size() > limit ? events_.size() - limit : 0;
    return {events_.begin() + static_cast<std::ptrdiff_t>(first), events_.end()};
}

std::vector<OperationalEvent>
MemoryOrderStore::load_order_events(std::string_view id, std::size_t limit) const {
    std::scoped_lock lk(mutex_);
    std::vector<OperationalEvent> result;
    for (const auto& ev : events_)
        if (ev.client_order_id == id) result.push_back(ev);
    if (result.size() > limit)
        result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(limit));
    return result;
}

OrderJournalStatus MemoryOrderStore::status() const {
    std::scoped_lock lk(mutex_);
    return {.location = "memory", .durable_writes = false, .record_sequence = record_sequence_};
}

} // namespace abex
