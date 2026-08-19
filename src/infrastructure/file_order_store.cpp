#include "abex/infrastructure/file_order_store.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <sys/file.h>
#include <unistd.h>

namespace abex {
namespace {

using Checksum = std::array<char, 16>;

[[nodiscard]] Checksum checksum(std::string_view text) {
    // FNV-1a is used as a corruption detector, not as a cryptographic authenticator.
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto character : text) {
        value ^= static_cast<unsigned char>(character);
        value *= 1099511628211ULL;
    }
    constexpr char hex[] = "0123456789abcdef";
    Checksum result{};
    for (int index = 15; index >= 0; --index) {
        result[static_cast<std::size_t>(index)] = hex[value & 0x0fU];
        value >>= 4U;
    }
    return result;
}

void write_all(int descriptor, std::string_view data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const auto result = ::write(descriptor, data.data() + written, data.size() - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("failed to append order journal: ") +
                                     std::strerror(errno));
        }
        written += static_cast<std::size_t>(result);
    }
}

struct JournalLine {
    std::string text;
    std::uint64_t start_offset{0};
    bool terminated{false};
};

[[nodiscard]] std::vector<nlohmann::json>
read_valid_records(const std::filesystem::path& path,
                   int repair_descriptor = -1,
                   bool durable_repairs = false) {
    if (!std::filesystem::exists(path)) return {};

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to read order journal: " + path.string());
    std::vector<JournalLine> lines;
    std::uint64_t offset = 0;
    for (std::string line; std::getline(input, line);) {
        const bool terminated = !input.eof();
        const auto line_size = line.size();
        if (!line.empty()) {
            lines.push_back({
                .text = std::move(line),
                .start_offset = offset,
                .terminated = terminated,
            });
        }
        offset += line_size + (terminated ? 1U : 0U);
    }

    std::vector<nlohmann::json> records;
    records.reserve(lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        try {
            auto record = nlohmann::json::parse(lines[index].text);
            if (record.at("schemaVersion").get<int>() != 1) {
                throw std::runtime_error("unsupported journal schema");
            }
            const auto& type = record.at("type").get_ref<const std::string&>();
            const auto& payload = record.at("payload");
            const auto expected_checksum = checksum(payload.dump());
            if (record.at("checksum").get_ref<const std::string&>() !=
                std::string_view(expected_checksum.data(), expected_checksum.size())) {
                throw std::runtime_error("journal checksum mismatch");
            }
            if (type == "ORDER_SNAPSHOT") {
                (void)payload.get<Order>();
            } else if (type == "OPERATIONAL_EVENT") {
                (void)payload.get<OperationalEvent>();
            } else {
                throw std::runtime_error("unsupported journal record");
            }
            records.push_back(std::move(record));
        } catch (const std::exception& error) {
            // A process may die between write(2) calls. Only an invalid final record is
            // treated as a torn append; corruption in the middle is a hard startup error.
            if (index + 1 == lines.size()) {
                if (repair_descriptor >= 0) {
                    if (::ftruncate(repair_descriptor,
                                    static_cast<off_t>(lines[index].start_offset)) != 0) {
                        throw std::runtime_error(std::string("failed to repair torn journal: ") +
                                                 std::strerror(errno));
                    }
                    if (durable_repairs && ::fdatasync(repair_descriptor) != 0) {
                        throw std::runtime_error(std::string("failed to sync journal repair: ") +
                                                 std::strerror(errno));
                    }
                }
                break;
            }
            throw std::runtime_error("invalid order journal record " +
                                     std::to_string(index + 1) + ": " + error.what());
        }
    }

    // A crash can leave a fully written JSON record without its trailing newline. It is valid,
    // but a later O_APPEND would otherwise concatenate the next record onto the same line.
    if (repair_descriptor >= 0 && !lines.empty() && records.size() == lines.size() &&
        !lines.back().terminated) {
        if (::lseek(repair_descriptor, 0, SEEK_END) < 0) {
            throw std::runtime_error(std::string("failed to seek order journal: ") +
                                     std::strerror(errno));
        }
        write_all(repair_descriptor, "\n");
        if (durable_repairs && ::fdatasync(repair_descriptor) != 0) {
            throw std::runtime_error(std::string("failed to sync journal repair: ") +
                                     std::strerror(errno));
        }
    }
    return records;
}

} // namespace

FileOrderStore::FileOrderStore(std::filesystem::path path, bool durable_writes)
    : path_(std::move(path)), durable_writes_(durable_writes) {
    if (path_.empty()) {
        throw std::invalid_argument("order journal path must not be empty");
    }
    if (!path_.parent_path().empty()) {
        std::filesystem::create_directories(path_.parent_path());
    }

    lock_descriptor_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (lock_descriptor_ < 0) {
        throw std::runtime_error(std::string("failed to open order journal lock: ") +
                                 std::strerror(errno));
    }
    if (::flock(lock_descriptor_, LOCK_EX | LOCK_NB) != 0) {
        const auto saved_error = errno;
        ::close(lock_descriptor_);
        lock_descriptor_ = -1;
        throw std::runtime_error(std::string("order journal is already owned by another process: ") +
                                 std::strerror(saved_error));
    }

    try {
        std::uint64_t maximum_sequence = 0;
        for (const auto& record :
             read_valid_records(path_, lock_descriptor_, durable_writes_)) {
            maximum_sequence = std::max(
                maximum_sequence,
                record.value("recordSequence", std::uint64_t{0}));
        }
        next_sequence_.store(maximum_sequence + 1);
    } catch (...) {
        ::close(lock_descriptor_);
        lock_descriptor_ = -1;
        throw;
    }
}

FileOrderStore::~FileOrderStore() {
    if (lock_descriptor_ >= 0) ::close(lock_descriptor_);
}

void FileOrderStore::append(const Order& order) {
    (void)append_record("ORDER_SNAPSHOT", nlohmann::json(order));
}

OperationalEvent FileOrderStore::append_event(OperationalEvent event) {
    if (event.occurred_at_ms == 0) event.occurred_at_ms = unix_time_ms();
    event.sequence = append_record("OPERATIONAL_EVENT", nlohmann::json(event));
    return event;
}

std::uint64_t FileOrderStore::append_record(std::string_view type,
                                            nlohmann::json payload) {
    if (type != "ORDER_SNAPSHOT" && type != "OPERATIONAL_EVENT") {
        throw std::logic_error("unsupported journal record type");
    }
    std::scoped_lock lock(mutex_);
    const auto payload_text = payload.dump();
    const auto sequence = next_sequence_.fetch_add(1);
    const auto written_at = unix_time_ms();
    const auto payload_checksum = checksum(payload_text);
    std::string line;
    line.reserve(payload_text.size() + type.size() + 128);
    line += "{\"schemaVersion\":1,\"recordSequence\":";
    line += std::to_string(sequence);
    line += ",\"type\":\"";
    line.append(type);
    line += "\",\"writtenAt\":";
    line += std::to_string(written_at);
    line += ",\"payload\":";
    line += payload_text;
    line += ",\"checksum\":\"";
    line.append(payload_checksum.data(), payload_checksum.size());
    line += "\"}";
    line.push_back('\n');

    write_all(lock_descriptor_, line);
    if (durable_writes_ && ::fdatasync(lock_descriptor_) != 0) {
        throw std::runtime_error(std::string("failed to sync order journal: ") +
                                 std::strerror(errno));
    }
    return sequence;
}

std::vector<Order> FileOrderStore::load_latest() const {
    std::scoped_lock lock(mutex_);
    std::unordered_map<std::string, std::pair<std::uint64_t, Order>> latest;
    for (const auto& record : read_valid_records(path_)) {
        if (record.at("type") != "ORDER_SNAPSHOT") continue;
        auto order = record.at("payload").get<Order>();
        const auto sequence = record.at("recordSequence").get<std::uint64_t>();
        auto& current = latest[order.client_order_id];
        if (sequence >= current.first) current = {sequence, std::move(order)};
    }

    std::vector<Order> orders;
    orders.reserve(latest.size());
    for (auto& [id, entry] : latest) {
        (void)id;
        orders.push_back(std::move(entry.second));
    }
    return orders;
}

std::vector<OperationalEvent> FileOrderStore::load_events(std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    std::vector<OperationalEvent> events;
    for (const auto& record : read_valid_records(path_)) {
        if (record.at("type") != "OPERATIONAL_EVENT") continue;
        auto event = record.at("payload").get<OperationalEvent>();
        event.sequence = record.at("recordSequence").get<std::uint64_t>();
        events.push_back(std::move(event));
    }
    if (events.size() > limit) {
        events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return events;
}

std::vector<OperationalEvent>
FileOrderStore::load_order_events(std::string_view client_order_id, std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    std::vector<OperationalEvent> events;
    for (const auto& record : read_valid_records(path_)) {
        if (record.at("type") != "OPERATIONAL_EVENT") continue;
        auto event = record.at("payload").get<OperationalEvent>();
        if (event.client_order_id != client_order_id) continue;
        event.sequence = record.at("recordSequence").get<std::uint64_t>();
        events.push_back(std::move(event));
    }
    if (events.size() > limit) {
        events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return events;
}

OrderJournalStatus FileOrderStore::status() const {
    return {
        .location = path_.string(),
        .durable_writes = durable_writes_,
        .record_sequence = next_sequence_.load() - 1,
    };
}

void MemoryOrderStore::append(const Order& order) {
    std::scoped_lock lock(mutex_);
    ++record_sequence_;
    records_.push_back(order);
}

OperationalEvent MemoryOrderStore::append_event(OperationalEvent event) {
    std::scoped_lock lock(mutex_);
    if (event.occurred_at_ms == 0) event.occurred_at_ms = unix_time_ms();
    event.sequence = ++record_sequence_;
    events_.push_back(event);
    return event;
}

std::vector<Order> MemoryOrderStore::load_latest() const {
    std::scoped_lock lock(mutex_);
    std::unordered_map<std::string, Order> latest;
    for (const auto& order : records_) latest[order.client_order_id] = order;
    std::vector<Order> result;
    result.reserve(latest.size());
    for (auto& [id, order] : latest) {
        (void)id;
        result.push_back(std::move(order));
    }
    return result;
}

std::vector<OperationalEvent> MemoryOrderStore::load_events(std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    const auto first = events_.size() > limit ? events_.size() - limit : 0;
    return {events_.begin() + static_cast<std::ptrdiff_t>(first), events_.end()};
}

std::vector<OperationalEvent>
MemoryOrderStore::load_order_events(std::string_view client_order_id, std::size_t limit) const {
    std::scoped_lock lock(mutex_);
    std::vector<OperationalEvent> result;
    for (const auto& event : events_) {
        if (event.client_order_id == client_order_id) result.push_back(event);
    }
    if (result.size() > limit) {
        result.erase(result.begin(), result.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return result;
}

OrderJournalStatus MemoryOrderStore::status() const {
    std::scoped_lock lock(mutex_);
    return {
        .location = "memory",
        .durable_writes = false,
        .record_sequence = record_sequence_,
    };
}

} // namespace abex
