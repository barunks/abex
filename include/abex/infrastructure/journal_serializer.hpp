#pragma once

// Journal serializer policies for FileOrderStore.
//
// A serializer must satisfy the JournalSerializer concept:
//
//   static constexpr std::string_view format_id;   // written into every envelope
//   static void write_order(std::string& buf, const Order& o, bool intent_only);
//   static void write_event(std::string& buf, const OperationalEvent& e);
//
// Both methods append the *payload* text only — no envelope, no checksum.
// FileOrderStore wraps the payload in the standard envelope and computes the
// FNV-1a checksum inline over the payload bytes before writing.
//
// Two implementations are provided:
//   JsonSerializer  — hand-written JSON, byte-compatible with the original
//                     nlohmann-based journal. Existing journals load unchanged.
//   KvSerializer    — tab-separated key=value pairs. Faster to write (no
//                     JSON escaping for most fields), trivially grep-able.
//                     Requires a matching KvSerializer reader (startup only).

#include "abex/domain/order.hpp"
#include "abex/domain/operational_event.hpp"

#include <concepts>
#include <string>
#include <string_view>

namespace abex {

// ── concept ──────────────────────────────────────────────────────────────────

template <typename S>
concept JournalSerializer = requires {
    { S::format_id } -> std::convertible_to<std::string_view>;
    requires std::is_invocable_v<decltype(S::write_order),
                                 std::string&, const Order&, bool>;
    requires std::is_invocable_v<decltype(S::write_event),
                                 std::string&, const OperationalEvent&>;
};

// ── shared low-level writers ──────────────────────────────────────────────────

namespace detail {

inline void jw_str(std::string& b, std::string_view s) {
    b += '"';
    for (const unsigned char c : s) {
        switch (c) {
        case '"':  b += '\\'; b += '"';  break;
        case '\\': b += '\\'; b += '\\'; break;
        case '\n': b += '\\'; b += 'n';  break;
        case '\r': b += '\\'; b += 'r';  break;
        case '\t': b += '\\'; b += 't';  break;
        default:
            if (c < 0x20) {
                // Escape all other control characters as \uXXXX
                b += '\\'; b += 'u'; b += '0'; b += '0';
                b += "0123456789abcdef"[c >> 4];
                b += "0123456789abcdef"[c & 0xf];
            } else {
                b += static_cast<char>(c);
            }
        }
    }
    b += '"';
}

inline void jw_key(std::string& b, std::string_view k) { jw_str(b, k); b += ':'; }

inline void jw_decimal(std::string& b, const Decimal& d) {
    b += '"'; d.append_to(b); b += '"';
}

inline void jw_u64(std::string& b, std::uint64_t v) { b += std::to_string(v); }
inline void jw_i64(std::string& b, std::int64_t  v) { b += std::to_string(v); }

inline void jw_opt_decimal(std::string& b, std::string_view key,
                            const std::optional<Decimal>& v) {
    if (!v) return;
    b += ','; jw_key(b, key); jw_decimal(b, *v);
}

inline void jw_opt_u64(std::string& b, std::string_view key,
                        const std::optional<std::uint64_t>& v) {
    if (!v) return;
    b += ','; jw_key(b, key); jw_u64(b, *v);
}

inline void jw_strset(std::string& b, const StringSet& s) {
    b += '[';
    bool first = true;
    for (const auto& v : s) { if (!first) b += ','; jw_str(b, v); first = false; }
    b += ']';
}

inline void jw_strmap_str(std::string& b, const StringMap<std::string>& m) {
    b += '{';
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) b += ',';
        jw_str(b, k); b += ':'; jw_str(b, v);
        first = false;
    }
    b += '}';
}

inline void jw_strmap_decimal(std::string& b, const StringMap<Decimal>& m) {
    b += '{';
    bool first = true;
    for (const auto& [k, v] : m) {
        if (!first) b += ',';
        jw_str(b, k); b += ':'; jw_decimal(b, v);
        first = false;
    }
    b += '}';
}

// KV helpers — no quoting needed for enum/numeric values; strings use
// length-prefixed encoding to avoid delimiter ambiguity: <len>:<bytes>
inline void kv_field(std::string& b, std::string_view key, std::string_view val) {
    b += key; b += '=';
    b += std::to_string(val.size()); b += ':'; b.append(val);
    b += '\t';
}

inline void kv_decimal(std::string& b, std::string_view key, const Decimal& d) {
    b += key; b += '=';
    const auto start = b.size();
    d.append_to(b);
    // no length prefix needed — decimals contain only [0-9.-]
    (void)start;
    b += '\t';
}

inline void kv_u64(std::string& b, std::string_view key, std::uint64_t v) {
    b += key; b += '='; b += std::to_string(v); b += '\t';
}

inline void kv_i64(std::string& b, std::string_view key, std::int64_t v) {
    b += key; b += '='; b += std::to_string(v); b += '\t';
}

inline void kv_opt_decimal(std::string& b, std::string_view key,
                            const std::optional<Decimal>& v) {
    if (v) kv_decimal(b, key, *v);
}

inline void kv_opt_u64(std::string& b, std::string_view key,
                        const std::optional<std::uint64_t>& v) {
    if (v) kv_u64(b, key, *v);
}

// Sets and maps: pipe-delimited, each element length-prefixed.
inline void kv_strset(std::string& b, std::string_view key, const StringSet& s) {
    if (s.empty()) return;
    b += key; b += '=';
    for (const auto& v : s) { b += std::to_string(v.size()); b += ':'; b += v; b += '|'; }
    b += '\t';
}

inline void kv_strmap_str(std::string& b, std::string_view key,
                           const StringMap<std::string>& m) {
    if (m.empty()) return;
    b += key; b += '=';
    for (const auto& [k, v] : m) {
        b += std::to_string(k.size()); b += ':'; b += k; b += '=';
        b += std::to_string(v.size()); b += ':'; b += v; b += '|';
    }
    b += '\t';
}

inline void kv_strmap_decimal(std::string& b, std::string_view key,
                               const StringMap<Decimal>& m) {
    if (m.empty()) return;
    b += key; b += '=';
    for (const auto& [k, v] : m) {
        b += std::to_string(k.size()); b += ':'; b += k; b += '=';
        v.append_to(b); b += '|';
    }
    b += '\t';
}

} // namespace detail

// ── JsonSerializer ────────────────────────────────────────────────────────────
// Hand-written JSON. Byte-compatible with the original nlohmann journal.
// Existing journals load without any migration.

struct JsonSerializer {
    static constexpr std::string_view format_id = "json";

    static void write_order(std::string& b, const Order& o, bool intent_only) {
        using namespace detail;
        b += '{';
        jw_key(b, "clientOrderId");    jw_str(b, o.client_order_id);     b += ',';
        jw_key(b, "venue");            jw_str(b, to_string(o.venue));     b += ',';
        jw_key(b, "symbol");           jw_str(b, o.symbol);               b += ',';
        jw_key(b, "side");             jw_str(b, to_string(o.side));      b += ',';
        jw_key(b, "type");             jw_str(b, to_string(o.type));      b += ',';
        jw_key(b, "quantity");         jw_decimal(b, o.quantity);         b += ',';
        jw_key(b, "timeInForce");      jw_str(b, to_string(o.time_in_force)); b += ',';
        jw_key(b, "status");           jw_str(b, to_string(o.status));    b += ',';
        jw_key(b, "pendingAction");    jw_str(b, to_string(o.pending_action)); b += ',';
        jw_key(b, "filledQuantity");   jw_decimal(b, o.filled_quantity);  b += ',';
        jw_key(b, "cumulativeQuote");  jw_decimal(b, o.cumulative_quote); b += ',';
        jw_key(b, "rejectionReason");  jw_str(b, o.rejection_reason);     b += ',';
        jw_key(b, "version");          jw_u64(b, o.version);              b += ',';
        jw_key(b, "createdAt");        jw_i64(b, o.created_at_ms);        b += ',';
        jw_key(b, "updatedAt");        jw_i64(b, o.updated_at_ms);        b += ',';
        jw_key(b, "createFingerprint"); jw_str(b, o.create_fingerprint);
        if (o.price)                { b += ','; jw_key(b, "price");               jw_decimal(b, *o.price); }
        jw_opt_decimal(b, "averageFillPrice",     o.average_fill_price);
        jw_opt_decimal(b, "pendingAmendPrice",    o.pending_amend_price);
        jw_opt_decimal(b, "pendingAmendQuantity", o.pending_amend_quantity);
        jw_opt_u64    (b, "lastSequence",         o.last_sequence);
        if (!intent_only) {
            b += ','; jw_key(b, "exchangeOrderId");          jw_str(b, o.exchange_order_id);
            b += ','; jw_key(b, "exchangeClientIdAliases");  jw_strset(b, o.exchange_client_id_aliases);
            b += ','; jw_key(b, "exchangeOrderIdAliases");   jw_strset(b, o.exchange_order_id_aliases);
            b += ','; jw_key(b, "exchangeFillOffsets");      jw_strmap_decimal(b, o.exchange_fill_offsets);
            b += ','; jw_key(b, "exchangeQuoteOffsets");     jw_strmap_decimal(b, o.exchange_quote_offsets);
            b += ','; jw_key(b, "processedRequests");        jw_strmap_str(b, o.processed_requests);
            b += ','; jw_key(b, "processedRequestOutcomes"); jw_strmap_str(b, o.processed_request_outcomes);
            b += ','; jw_key(b, "processedEventIds");        jw_strset(b, o.processed_event_ids);
        }
        b += '}';
    }

    static void write_event(std::string& b, const OperationalEvent& e) {
        using namespace detail;
        b += '{';
        jw_key(b, "occurredAt");  jw_i64(b, e.occurred_at_ms);                          b += ',';
        jw_key(b, "severity");    jw_str(b, to_string(e.severity));                      b += ',';
        jw_key(b, "category");    jw_str(b, e.category);                                 b += ',';
        jw_key(b, "code");        jw_str(b, e.code);                                     b += ',';
        jw_key(b, "message");     jw_str(b, e.message);                                  b += ',';
        jw_key(b, "instanceId");  jw_str(b, e.instance_id);                              b += ',';
        jw_key(b, "clientOrderId"); jw_str(b, e.client_order_id);                        b += ',';
        jw_key(b, "requestId");   jw_str(b, e.request_id);
        if (e.venue) { b += ','; jw_key(b, "venue"); jw_str(b, to_string(*e.venue)); }
        if (e.order) {
            b += ',';
            jw_key(b, "order");
            const auto& o = *e.order;
            b += '{';
            jw_key(b, "exchangeOrderId"); jw_str(b, o.exchange_order_id);  b += ',';
            jw_key(b, "symbol");          jw_str(b, o.symbol);              b += ',';
            jw_key(b, "side");            jw_str(b, to_string(o.side));     b += ',';
            jw_key(b, "type");            jw_str(b, to_string(o.type));     b += ',';
            jw_key(b, "quantity");        jw_decimal(b, o.quantity);        b += ',';
            jw_key(b, "filledQuantity");  jw_decimal(b, o.filled_quantity); b += ',';
            jw_key(b, "status");          jw_str(b, to_string(o.status));   b += ',';
            jw_key(b, "pendingAction");   jw_str(b, to_string(o.pending_action)); b += ',';
            jw_key(b, "rejectionReason"); jw_str(b, o.rejection_reason);    b += ',';
            jw_key(b, "version");         jw_u64(b, o.version);             b += ',';
            jw_key(b, "exchangeTime");    jw_i64(b, o.exchange_time_ms);
            if (o.price)               { b += ','; jw_key(b, "price");          jw_decimal(b, *o.price); }
            if (o.average_fill_price)  { b += ','; jw_key(b, "averageFillPrice"); jw_decimal(b, *o.average_fill_price); }
            if (o.venue_sequence)      { b += ','; jw_key(b, "venueSequence");   jw_u64(b, *o.venue_sequence); }
            b += '}';
        }
        b += '}';
    }
};

// ── KvSerializer ─────────────────────────────────────────────────────────────
// Tab-separated key=value pairs. Values that may contain arbitrary bytes use
// length-prefix encoding (<len>:<bytes>). Numeric and enum values are raw.
// Faster to write than JSON (no per-character escaping loop for most fields).
// Human-readable and trivially grep-able. Requires the KvSerializer reader
// in file_order_store.cpp (startup path only — latency irrelevant).

struct KvSerializer {
    static constexpr std::string_view format_id = "kv1";

    static void write_order(std::string& b, const Order& o, bool intent_only) {
        using namespace detail;
        kv_field  (b, "id",      o.client_order_id);
        kv_field  (b, "venue",   to_string(o.venue));
        kv_field  (b, "sym",     o.symbol);
        kv_field  (b, "side",    to_string(o.side));
        kv_field  (b, "type",    to_string(o.type));
        kv_decimal(b, "qty",     o.quantity);
        kv_field  (b, "tif",     to_string(o.time_in_force));
        kv_field  (b, "status",  to_string(o.status));
        kv_field  (b, "pa",      to_string(o.pending_action));
        kv_decimal(b, "filled",  o.filled_quantity);
        kv_decimal(b, "quote",   o.cumulative_quote);
        kv_field  (b, "reason",  o.rejection_reason);
        kv_u64    (b, "ver",     o.version);
        kv_i64    (b, "cat",     o.created_at_ms);
        kv_i64    (b, "uat",     o.updated_at_ms);
        kv_field  (b, "fp",      o.create_fingerprint);
        kv_opt_decimal(b, "price",  o.price);
        kv_opt_decimal(b, "afp",    o.average_fill_price);
        kv_opt_decimal(b, "pap",    o.pending_amend_price);
        kv_opt_decimal(b, "paq",    o.pending_amend_quantity);
        kv_opt_u64    (b, "lseq",   o.last_sequence);
        if (!intent_only) {
            kv_field        (b, "exid",   o.exchange_order_id);
            kv_strset       (b, "ecid",   o.exchange_client_id_aliases);
            kv_strset       (b, "eoid",   o.exchange_order_id_aliases);
            kv_strmap_decimal(b, "efo",   o.exchange_fill_offsets);
            kv_strmap_decimal(b, "eqo",   o.exchange_quote_offsets);
            kv_strmap_str   (b, "preq",   o.processed_requests);
            kv_strmap_str   (b, "prout",  o.processed_request_outcomes);
            kv_strset       (b, "evids",  o.processed_event_ids);
        }
    }

    static void write_event(std::string& b, const OperationalEvent& e) {
        using namespace detail;
        kv_i64 (b, "at",       e.occurred_at_ms);
        kv_field(b, "sev",     to_string(e.severity));
        kv_field(b, "cat",     e.category);
        kv_field(b, "code",    e.code);
        kv_field(b, "msg",     e.message);
        kv_field(b, "inst",    e.instance_id);
        if (e.venue)                    kv_field(b, "venue", to_string(*e.venue));
        if (!e.client_order_id.empty()) kv_field(b, "oid",  e.client_order_id);
        if (!e.request_id.empty())      kv_field(b, "rid",  e.request_id);
    }
};

static_assert(JournalSerializer<JsonSerializer>);
static_assert(JournalSerializer<KvSerializer>);

} // namespace abex
