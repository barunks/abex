#include "abex/infrastructure/market_data_ring.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace abex {
namespace {

constexpr std::array<char, 8> ring_magic{'A', 'B', 'E', 'X', 'M', 'D', '0', '1'};
constexpr std::uint32_t ring_version = 1;
constexpr std::size_t symbol_capacity = 16;

struct alignas(64) RingHeader {
    std::array<char, 8> magic{};
    std::uint32_t version{0};
    std::uint32_t capacity{0};
    std::uint32_t record_size{0};
    std::uint32_t reserved{0};
    std::uint64_t generation{0};
    alignas(8) std::uint64_t published_sequence{0};
};

struct alignas(64) RingRecord {
    alignas(8) std::uint64_t committed_sequence{0};
    std::uint64_t sequence{0};
    std::int64_t source_time_ms{0};
    std::int64_t published_at_ms{0};
    std::int64_t bid_raw{0};
    std::int64_t ask_raw{0};
    std::uint8_t venue{0};
    std::array<char, symbol_capacity> symbol{};
};

static_assert(std::is_trivially_copyable_v<RingHeader>);
static_assert(std::is_trivially_copyable_v<RingRecord>);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "the mmap ring requires lock-free 64-bit atomics");

[[noreturn]] void system_error(std::string_view operation,
                               const std::filesystem::path& path) {
    throw std::runtime_error(std::string(operation) + " " + path.string() + ": " +
                             std::strerror(errno));
}

[[nodiscard]] std::size_t mapping_size(std::size_t capacity) {
    if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("market-data ring capacity is out of range");
    }
    return sizeof(RingHeader) + capacity * sizeof(RingRecord);
}

[[nodiscard]] std::uint64_t load_acquire(const std::uint64_t& value) noexcept {
    return std::atomic_ref<std::uint64_t>(const_cast<std::uint64_t&>(value))
        .load(std::memory_order_acquire);
}

void store_release(std::uint64_t& target, std::uint64_t value) noexcept {
    std::atomic_ref<std::uint64_t>(target).store(value, std::memory_order_release);
}

[[nodiscard]] std::uint8_t encode_venue(Venue venue) noexcept {
    return venue == Venue::Okx ? std::uint8_t{1} : std::uint8_t{2};
}

[[nodiscard]] Venue decode_venue(std::uint8_t venue) {
    if (venue == 1) return Venue::Okx;
    if (venue == 2) return Venue::Binance;
    throw std::runtime_error("market-data ring contains an invalid venue");
}

struct OwnedDescriptorTag {};

class Mapping final {
public:
    Mapping(std::filesystem::path path, int open_flags, int protection, std::size_t size)
        : path_(std::move(path)), size_(size) {
        descriptor_ = ::open(path_.c_str(), open_flags, 0640);
        if (descriptor_ < 0) system_error("cannot open", path_);
        address_ = ::mmap(nullptr, size_, protection, MAP_SHARED, descriptor_, 0);
        if (address_ == MAP_FAILED) {
            address_ = nullptr;
            const auto saved_error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            errno = saved_error;
            system_error("cannot map", path_);
        }
    }

    Mapping(std::filesystem::path path,
            int descriptor,
            int protection,
            std::size_t size,
            OwnedDescriptorTag)
        : path_(std::move(path)), size_(size), descriptor_(descriptor) {
        address_ = ::mmap(nullptr, size_, protection, MAP_SHARED, descriptor_, 0);
        if (address_ == MAP_FAILED) {
            address_ = nullptr;
            const auto saved_error = errno;
            ::close(descriptor_);
            descriptor_ = -1;
            errno = saved_error;
            system_error("cannot map", path_);
        }
    }

    ~Mapping() {
        if (address_) ::munmap(address_, size_);
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    [[nodiscard]] int descriptor() const noexcept { return descriptor_; }
    [[nodiscard]] void* address() const noexcept { return address_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::filesystem::path path_;
    std::size_t size_{0};
    int descriptor_{-1};
    void* address_{nullptr};
};

[[nodiscard]] std::size_t existing_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("cannot inspect " + path.string() + ": " + error.message());
    if (size < sizeof(RingHeader)) {
        throw std::runtime_error("market-data ring is smaller than its header: " + path.string());
    }
    return static_cast<std::size_t>(size);
}

} // namespace

class MarketDataRingWriter::Impl final {
public:
    Impl(std::filesystem::path path, std::size_t capacity)
        : path_(std::move(path)), capacity_(capacity), size_(mapping_size(capacity_)) {
        if (!path_.parent_path().empty()) std::filesystem::create_directories(path_.parent_path());

        const auto descriptor = ::open(path_.c_str(), O_RDWR | O_CREAT, 0640);
        if (descriptor < 0) system_error("cannot create", path_);
        if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
            const auto saved_error = errno;
            ::close(descriptor);
            errno = saved_error;
            system_error("cannot lock", path_);
        }
        if (::ftruncate(descriptor, static_cast<off_t>(size_)) != 0) {
            const auto saved_error = errno;
            ::close(descriptor);
            errno = saved_error;
            system_error("cannot size", path_);
        }
        mapping_ = std::make_unique<Mapping>(path_, descriptor, PROT_READ | PROT_WRITE, size_,
                                             OwnedDescriptorTag{});
        std::memset(mapping_->address(), 0, size_);
        header_ = static_cast<RingHeader*>(mapping_->address());
        records_ = reinterpret_cast<RingRecord*>(static_cast<std::byte*>(mapping_->address()) +
                                                sizeof(RingHeader));
        header_->version = ring_version;
        header_->capacity = static_cast<std::uint32_t>(capacity_);
        header_->record_size = sizeof(RingRecord);
        const auto generation = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
                                static_cast<std::uint64_t>(::getpid());
        header_->magic = ring_magic;
        store_release(header_->published_sequence, 0);
        store_release(header_->generation, generation);
    }

    void publish(std::span<const MarketQuote> quotes) {
        for (const auto& quote : quotes) {
            if (!valid_quote(quote)) throw std::invalid_argument("cannot publish invalid quote");
            if (quote.symbol.size() >= symbol_capacity) {
                throw std::invalid_argument("market-data symbol is too long for the ring");
            }

            const auto sequence = ++sequence_;
            auto& record = records_[(sequence - 1) % capacity_];
            store_release(record.committed_sequence, 0);
            record.sequence = sequence;
            record.source_time_ms = quote.source_time_ms;
            record.published_at_ms = quote.published_at_ms == 0 ? unix_time_ms()
                                                                 : quote.published_at_ms;
            record.bid_raw = quote.bid_price.raw();
            record.ask_raw = quote.ask_price.raw();
            record.venue = encode_venue(quote.venue);
            record.symbol.fill('\0');
            std::memcpy(record.symbol.data(), quote.symbol.data(), quote.symbol.size());
            store_release(record.committed_sequence, sequence);
            store_release(header_->published_sequence, sequence);
        }
    }

    [[nodiscard]] std::uint64_t generation() const noexcept { return header_->generation; }
    [[nodiscard]] std::uint64_t sequence() const noexcept { return sequence_; }

private:
    std::filesystem::path path_;
    std::size_t capacity_;
    std::size_t size_;
    std::unique_ptr<Mapping> mapping_;
    RingHeader* header_{nullptr};
    RingRecord* records_{nullptr};
    std::uint64_t sequence_{0};
};

MarketDataRingWriter::MarketDataRingWriter(std::filesystem::path path, std::size_t capacity)
    : impl_(std::make_unique<Impl>(std::move(path), capacity)) {}

MarketDataRingWriter::~MarketDataRingWriter() = default;

void MarketDataRingWriter::publish(std::span<const MarketQuote> quotes) {
    impl_->publish(quotes);
}

std::uint64_t MarketDataRingWriter::generation() const noexcept { return impl_->generation(); }

std::uint64_t MarketDataRingWriter::sequence() const noexcept { return impl_->sequence(); }

class MarketDataRingReader::Impl final {
public:
    explicit Impl(std::filesystem::path path)
        : path_(std::move(path)), size_(existing_file_size(path_)),
          mapping_(std::make_unique<Mapping>(path_, O_RDONLY, PROT_READ, size_)) {
        header_ = static_cast<const RingHeader*>(mapping_->address());
        if (header_->magic != ring_magic || header_->version != ring_version ||
            header_->record_size != sizeof(RingRecord) || header_->capacity == 0) {
            throw std::runtime_error("market-data ring has an incompatible layout: " +
                                     path_.string());
        }
        capacity_ = header_->capacity;
        if (size_ < mapping_size(capacity_)) {
            throw std::runtime_error("market-data ring is truncated: " + path_.string());
        }
        records_ = reinterpret_cast<const RingRecord*>(
            static_cast<const std::byte*>(mapping_->address()) + sizeof(RingHeader));
    }

    [[nodiscard]] std::size_t read_available(MarketDataCursor& cursor,
                                             std::span<MarketQuote> output) const {
        const auto generation = load_acquire(header_->generation);
        const auto latest = load_acquire(header_->published_sequence);
        if (generation == 0 || output.empty()) return 0;
        if (cursor.generation != generation || cursor.sequence > latest) {
            cursor = {.generation = generation, .sequence = 0};
        }
        if (latest <= cursor.sequence) return 0;

        const auto first_available = latest > capacity_ ? latest - capacity_ + 1 : 1;
        const auto first = std::max(cursor.sequence + 1, first_available);
        std::size_t count = 0;
        auto processed_sequence = cursor.sequence;
        for (auto sequence = first; sequence <= latest; ++sequence) {
            if (count == output.size()) break;
            processed_sequence = sequence;
            const auto& record = records_[(sequence - 1) % capacity_];
            if (load_acquire(record.committed_sequence) != sequence) continue;

            const auto record_sequence = record.sequence;
            const auto source_time_ms = record.source_time_ms;
            const auto published_at_ms = record.published_at_ms;
            const auto bid_raw = record.bid_raw;
            const auto ask_raw = record.ask_raw;
            const auto venue = record.venue;
            const auto symbol = record.symbol;
            if (load_acquire(record.committed_sequence) != sequence ||
                record_sequence != sequence) {
                continue;
            }

            const auto symbol_length = std::find(symbol.begin(), symbol.end(), '\0') - symbol.begin();
            auto& quote = output[count];
            quote.venue = decode_venue(venue);
            quote.symbol.assign(symbol.data(), static_cast<std::size_t>(symbol_length));
            quote.bid_price = Decimal::from_raw(bid_raw);
            quote.ask_price = Decimal::from_raw(ask_raw);
            quote.source_time_ms = source_time_ms;
            quote.published_at_ms = published_at_ms;
            quote.sequence = sequence;
            if (valid_quote(quote)) ++count;
        }
        cursor.generation = generation;
        cursor.sequence = processed_sequence;
        return count;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    std::filesystem::path path_;
    std::size_t size_{0};
    std::unique_ptr<Mapping> mapping_;
    const RingHeader* header_{nullptr};
    const RingRecord* records_{nullptr};
    std::size_t capacity_{0};
};

MarketDataRingReader::MarketDataRingReader(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(std::move(path))) {}

MarketDataRingReader::~MarketDataRingReader() = default;

std::vector<MarketQuote> MarketDataRingReader::read_available(MarketDataCursor& cursor) const {
    std::vector<MarketQuote> result(capacity());
    result.resize(impl_->read_available(cursor, result));
    return result;
}

std::size_t MarketDataRingReader::read_available(MarketDataCursor& cursor,
                                                  std::span<MarketQuote> output) const {
    return impl_->read_available(cursor, output);
}

std::size_t MarketDataRingReader::capacity() const noexcept { return impl_->capacity(); }

} // namespace abex
