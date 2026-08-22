// Events module - implementation (non-templated)
#include "events/loader.hpp"

#include <boost/json.hpp>
#include <boost/json/basic_parser_impl.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace json = boost::json;

namespace arb {

namespace {

constexpr uint32_t CANDLE_CACHE_VERSION = 3;
constexpr uint64_t CANDLE_CACHE_ENDIAN_TAG = 0x0102030405060708ULL;
constexpr char CANDLE_CACHE_MAGIC[8] = {'A','R','B','C','N','D','L','3'};
constexpr size_t CANDLE_CACHE_HEADER_BYTES = 64;
constexpr size_t CANDLE_CACHE_TRAILER_BYTES = 16;

static_assert(sizeof(Candle) == 48, "Candle layout changed; bump cache version");
static_assert(
    std::is_trivially_copyable<Candle>::value,
    "Candle cache requires trivially copyable Candle"
);

struct CandleCacheKey {
    uint64_t source_key{0};
    uint64_t max_candles{0};
    uint64_t squeeze_bits{0};
    uint64_t start_ts{0};
};

uint64_t fnv1a64(const void* bytes, size_t size) {
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    uint64_t hash = 1469598103934665603ULL;
    for (size_t index = 0; index < size; ++index) {
        hash ^= cursor[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

void serialize_cache_header(
    char* bytes,
    const CandleCacheKey& key,
    uint64_t count
) {
    size_t offset = 0;
    auto append = [&](const void* source, size_t size) {
        std::memcpy(bytes + offset, source, size);
        offset += size;
    };
    append(CANDLE_CACHE_MAGIC, sizeof(CANDLE_CACHE_MAGIC));
    const uint32_t version = CANDLE_CACHE_VERSION;
    const uint32_t candle_size = static_cast<uint32_t>(sizeof(Candle));
    append(&version, sizeof(version));
    append(&candle_size, sizeof(candle_size));
    append(&CANDLE_CACHE_ENDIAN_TAG, sizeof(CANDLE_CACHE_ENDIAN_TAG));
    append(&key.source_key, sizeof(key.source_key));
    append(&key.max_candles, sizeof(key.max_candles));
    append(&key.squeeze_bits, sizeof(key.squeeze_bits));
    append(&key.start_ts, sizeof(key.start_ts));
    append(&count, sizeof(count));
}

std::string candle_cache_path(const char* directory, const CandleCacheKey& key) {
    uint64_t params_hash = 1469598103934665603ULL;
    auto mix = [&](uint64_t value) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            params_hash ^= (value >> (byte * 8U)) & 0xffULL;
            params_hash *= 1099511628211ULL;
        }
    };
    mix(key.max_candles);
    mix(key.squeeze_bits);
    mix(key.start_ts);
    mix(CANDLE_CACHE_VERSION);
    mix(sizeof(Candle));
    char filename[80]{};
    std::snprintf(
        filename,
        sizeof(filename),
        "candles_%016llx_%016llx.bin",
        static_cast<unsigned long long>(key.source_key),
        static_cast<unsigned long long>(params_hash)
    );
    return (std::filesystem::path(directory) / filename).string();
}

bool try_read_candle_cache(
    const std::string& path,
    const CandleCacheKey& key,
    std::vector<Candle>& out
) {
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        input.seekg(0, std::ios::end);
        const std::streampos end = input.tellg();
        if (end < static_cast<std::streampos>(
                CANDLE_CACHE_HEADER_BYTES + CANDLE_CACHE_TRAILER_BYTES
            )) {
            return false;
        }
        input.seekg(0, std::ios::beg);
        char header[CANDLE_CACHE_HEADER_BYTES]{};
        input.read(header, static_cast<std::streamsize>(sizeof(header)));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(header))) {
            return false;
        }
        uint64_t count = 0;
        std::memcpy(&count, header + CANDLE_CACHE_HEADER_BYTES - 8, 8);
        if (count > std::numeric_limits<uint64_t>::max() / sizeof(Candle)) {
            return false;
        }
        const uint64_t payload_bytes = count * sizeof(Candle);
        const uint64_t expected_size = CANDLE_CACHE_HEADER_BYTES +
            payload_bytes + CANDLE_CACHE_TRAILER_BYTES;
        if (static_cast<uint64_t>(end) != expected_size) return false;

        char expected_header[CANDLE_CACHE_HEADER_BYTES]{};
        serialize_cache_header(expected_header, key, count);
        if (std::memcmp(header, expected_header, sizeof(header)) != 0) {
            return false;
        }

        std::vector<Candle> candidate(static_cast<size_t>(count));
        if (payload_bytes != 0) {
            input.read(
                reinterpret_cast<char*>(candidate.data()),
                static_cast<std::streamsize>(payload_bytes)
            );
            if (input.gcount() != static_cast<std::streamsize>(payload_bytes)) {
                return false;
            }
        }
        uint64_t trailer_bytes = 0;
        uint64_t trailer_checksum = 0;
        input.read(
            reinterpret_cast<char*>(&trailer_bytes),
            static_cast<std::streamsize>(sizeof(trailer_bytes))
        );
        input.read(
            reinterpret_cast<char*>(&trailer_checksum),
            static_cast<std::streamsize>(sizeof(trailer_checksum))
        );
        if (!input || trailer_bytes != payload_bytes) return false;
        if (trailer_checksum != fnv1a64(candidate.data(), payload_bytes)) {
            return false;
        }
        out = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

long process_id() {
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

void write_candle_cache_atomic(
    const std::string& path,
    const CandleCacheKey& key,
    const std::vector<Candle>& candles
) {
    static std::atomic<uint64_t> sequence{0};
    const uint64_t ordinal = sequence.fetch_add(1, std::memory_order_relaxed);
    const std::string temporary = path + ".tmp." +
        std::to_string(process_id()) + "." + std::to_string(ordinal);
    try {
        const uint64_t count = static_cast<uint64_t>(candles.size());
        const uint64_t payload_bytes = count * sizeof(Candle);
        const uint64_t checksum = fnv1a64(candles.data(), payload_bytes);
        {
            std::ofstream output(
                temporary, std::ios::binary | std::ios::trunc
            );
            if (!output) return;
            char header[CANDLE_CACHE_HEADER_BYTES]{};
            serialize_cache_header(header, key, count);
            output.write(header, static_cast<std::streamsize>(sizeof(header)));
            if (payload_bytes != 0) {
                output.write(
                    reinterpret_cast<const char*>(candles.data()),
                    static_cast<std::streamsize>(payload_bytes)
                );
            }
            output.write(
                reinterpret_cast<const char*>(&payload_bytes),
                static_cast<std::streamsize>(sizeof(payload_bytes))
            );
            output.write(
                reinterpret_cast<const char*>(&checksum),
                static_cast<std::streamsize>(sizeof(checksum))
            );
            output.flush();
            if (!output) {
                output.close();
                std::remove(temporary.c_str());
                return;
            }
        }
        if (std::rename(temporary.c_str(), path.c_str()) != 0) {
            std::remove(temporary.c_str());
        }
    } catch (...) {
        std::remove(temporary.c_str());
    }
}

// Fast path for the canonical top-level array of numeric candle rows. Boost's
// basic_parser uses the same number parser and default parse_options as the
// DOM parser below. Any shape anomaly or parse error abandons these results
// and falls back to the historical DOM path verbatim.
struct CandleSaxHandler {
    static constexpr std::size_t max_object_size = std::size_t(-1);
    static constexpr std::size_t max_array_size = std::size_t(-1);
    static constexpr std::size_t max_key_size = std::size_t(-1);
    static constexpr std::size_t max_string_size = std::size_t(-1);

    enum class Kind : unsigned char { I64, U64, Dbl };
    struct Tagged {
        Kind kind{Kind::I64};
        std::int64_t i{0};
        std::uint64_t u{0};
        double d{0.0};
    };

    std::vector<Candle>& out;
    size_t max_candles;
    double squeeze_frac;
    uint64_t start_ts;
    int depth{0};
    bool unsupported{false};
    bool capped{false};
    Tagged row[6]{};
    size_t row_len{0};

    CandleSaxHandler(
        std::vector<Candle>& output,
        size_t max_rows,
        double squeeze,
        uint64_t start
    ) : out(output), max_candles(max_rows), squeeze_frac(squeeze),
        start_ts(start) {}

    bool ok() const { return !unsupported; }
    bool on_document_begin(boost::system::error_code&) { return true; }
    bool on_document_end(boost::system::error_code&) { return true; }
    bool on_object_begin(boost::system::error_code&) {
        unsupported = true;
        return true;
    }
    bool on_object_end(std::size_t, boost::system::error_code&) { return true; }
    bool on_array_begin(boost::system::error_code&) {
        ++depth;
        if (depth == 2) row_len = 0;
        else if (depth > 2) unsupported = true;
        return true;
    }
    bool on_array_end(std::size_t, boost::system::error_code&) {
        if (depth == 2 && !unsupported && !capped) emit_row();
        --depth;
        return true;
    }
    bool on_key_part(
        boost::json::string_view,
        std::size_t,
        boost::system::error_code&
    ) {
        unsupported = true;
        return true;
    }
    bool on_key(
        boost::json::string_view,
        std::size_t,
        boost::system::error_code&
    ) {
        unsupported = true;
        return true;
    }
    bool on_string_part(
        boost::json::string_view,
        std::size_t,
        boost::system::error_code&
    ) {
        unsupported = true;
        return true;
    }
    bool on_string(
        boost::json::string_view,
        std::size_t,
        boost::system::error_code&
    ) {
        unsupported = true;
        return true;
    }
    bool on_number_part(boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_int64(
        std::int64_t value,
        boost::json::string_view,
        boost::system::error_code&
    ) {
        Tagged tagged;
        tagged.kind = Kind::I64;
        tagged.i = value;
        push(tagged);
        return true;
    }
    bool on_uint64(
        std::uint64_t value,
        boost::json::string_view,
        boost::system::error_code&
    ) {
        Tagged tagged;
        tagged.kind = Kind::U64;
        tagged.u = value;
        push(tagged);
        return true;
    }
    bool on_double(
        double value,
        boost::json::string_view,
        boost::system::error_code&
    ) {
        Tagged tagged;
        tagged.kind = Kind::Dbl;
        tagged.d = value;
        push(tagged);
        return true;
    }
    bool on_bool(bool, boost::system::error_code&) {
        unsupported = true;
        return true;
    }
    bool on_null(boost::system::error_code&) {
        unsupported = true;
        return true;
    }
    bool on_comment_part(boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_comment(boost::json::string_view, boost::system::error_code&) {
        return true;
    }

    void push(const Tagged& tagged) {
        if (depth != 2) {
            unsupported = true;
            return;
        }
        if (row_len < 6) row[row_len] = tagged;
        ++row_len;
    }

    double to_double(const Tagged& tagged) const {
        if (tagged.kind == Kind::Dbl) return tagged.d;
        if (tagged.kind == Kind::I64) return static_cast<double>(tagged.i);
        return static_cast<double>(tagged.u);
    }

    void emit_row() {
        if (row_len < 6) return;
        Candle candle{};
        const Tagged& timestamp = row[0];
        uint64_t ts = 0;
        if (timestamp.kind == Kind::U64) ts = timestamp.u;
        else if (timestamp.kind == Kind::I64) {
            ts = static_cast<uint64_t>(timestamp.i);
        } else {
            ts = static_cast<uint64_t>(timestamp.d);
        }
        if (ts > 10000000000ULL) ts /= 1000ULL;
        if (start_ts > 0 && ts < start_ts) return;
        candle.ts = ts;
        candle.open = to_double(row[1]);
        candle.high = to_double(row[2]);
        candle.low = to_double(row[3]);
        candle.close = to_double(row[4]);
        candle.volume = to_double(row[5]);

        if (squeeze_frac > 0.0) {
            const double oc_mid = 0.5 * (candle.open + candle.close);
            if (oc_mid > 0) {
                const double max_high = oc_mid * (1.0 + squeeze_frac);
                const double min_low = oc_mid * (1.0 - squeeze_frac);
                if (candle.high > max_high) candle.high = max_high;
                if (candle.low < min_low) candle.low = min_low;
            }
        }
        out.push_back(candle);
        if (max_candles > 0 && out.size() >= max_candles) capped = true;
    }
};

} // namespace

EventSoA EventSoA::from_events(const std::vector<Event>& evs) {
    EventSoA s;
    const size_t n = evs.size();
    s.ts.reserve(n);
    s.p_cex.reserve(n);
    s.volume.reserve(n);
    s.candle_idx.reserve(n);
    bool any_chainlink = false;
    for (const auto& e : evs) {
        if (e.p_chainlink != 0.0) {
            any_chainlink = true;
            break;
        }
    }
    if (any_chainlink) {
        s.p_chainlink.reserve(n);
        s.chainlink_ts.reserve(n);
        s.chainlink_answer_index.reserve(n);
    }
    for (const auto& e : evs) {
        s.ts.push_back(e.ts);
        s.p_cex.push_back(e.p_cex);
        s.volume.push_back(e.volume);
        s.candle_idx.push_back(e.candle_idx);
        if (any_chainlink) {
            s.p_chainlink.push_back(e.p_chainlink);
            s.chainlink_ts.push_back(e.chainlink_ts);
            s.chainlink_answer_index.push_back(e.chainlink_answer_index);
        }
    }
    return s;
}

std::vector<Candle> load_candles(const std::string& path,
                                  size_t max_candles,
                                  double squeeze_frac,
                                  uint64_t start_ts) {
    std::vector<Candle> out;
    out.reserve(1024);

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open candles file: " + path);

    in.seekg(0, std::ios::end);
    const std::streampos end_pos = in.tellg();
    if (end_pos < 0) {
        throw std::runtime_error("Cannot stat candles file: " + path);
    }
    in.seekg(0, std::ios::beg);
    std::string s;
    s.resize(static_cast<size_t>(end_pos));
    if (!s.empty()) {
        in.read(s.data(), static_cast<std::streamsize>(s.size()));
        if (in.gcount() != static_cast<std::streamsize>(s.size())) {
            throw std::runtime_error("Short read on candles file: " + path);
        }
    }

    const char* cache_directory = std::getenv("ARB_CANDLE_CACHE_DIR");
    CandleCacheKey cache_key{};
    std::string cache_path;
    if (cache_directory != nullptr && *cache_directory != '\0') {
        std::error_code ec;
        std::filesystem::create_directories(cache_directory, ec);
        if (!ec) {
            cache_key.source_key = fnv1a64(s.data(), s.size());
            cache_key.max_candles = static_cast<uint64_t>(max_candles);
            static_assert(sizeof(cache_key.squeeze_bits) == sizeof(squeeze_frac));
            std::memcpy(
                &cache_key.squeeze_bits, &squeeze_frac, sizeof(squeeze_frac)
            );
            cache_key.start_ts = start_ts;
            cache_path = candle_cache_path(cache_directory, cache_key);
            if (try_read_candle_cache(cache_path, cache_key, out)) return out;
            out.clear();
            out.reserve(1024);
        }
    }

    bool sax_ok = false;
    {
        boost::system::error_code ec;
        json::basic_parser<CandleSaxHandler> parser(
            json::parse_options{}, out, max_candles, squeeze_frac, start_ts
        );
        const std::size_t consumed = parser.write_some(
            false, s.data(), s.size(), ec
        );
        sax_ok = !ec && parser.done() && parser.handler().ok();
        if (sax_ok && consumed != s.size()) {
            for (std::size_t index = consumed; index < s.size(); ++index) {
                const char ch = s[index];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') {
                    sax_ok = false;
                    break;
                }
            }
        }
        if (!sax_ok) out.clear();
    }

    if (!sax_ok) {
        json::value val = json::parse(s);
        if (!val.is_array()) throw std::runtime_error("Candles JSON must be an array of arrays");

        const auto& arr = val.as_array();
        out.reserve(max_candles ? std::min(max_candles, arr.size()) : arr.size());

        auto to_d = [](const json::value& v) -> double {
            if (v.is_double()) return v.as_double();
            if (v.is_int64())  return static_cast<double>(v.as_int64());
            if (v.is_uint64()) return static_cast<double>(v.as_uint64());
            return 0.0;
        };

        for (size_t idx = 0; idx < arr.size(); ++idx) {
            const auto& a = arr[idx].as_array();
            if (a.size() < 6) continue;

            Candle c{};
            uint64_t ts = 0;
            const auto& tsv = a[0];
            if (tsv.is_uint64()) ts = tsv.as_uint64();
            else if (tsv.is_int64()) ts = static_cast<uint64_t>(tsv.as_int64());
            else if (tsv.is_double()) ts = static_cast<uint64_t>(tsv.as_double());
            if (ts > 10000000000ULL) ts /= 1000ULL; // ms->s
            if (start_ts > 0 && ts < start_ts) continue;
            c.ts = ts;

            c.open   = to_d(a[1]);
            c.high   = to_d(a[2]);
            c.low    = to_d(a[3]);
            c.close  = to_d(a[4]);
            c.volume = to_d(a[5]);

            // Apply squeeze filter
            if (squeeze_frac > 0.0) {
                const double oc_mid = 0.5 * (c.open + c.close);
                if (oc_mid > 0) {
                    const double max_h = oc_mid * (1.0 + squeeze_frac);
                    const double min_l = oc_mid * (1.0 - squeeze_frac);
                    if (c.high > max_h) c.high = max_h;
                    if (c.low  < min_l) c.low  = min_l;
                }
            }
            out.push_back(c);
            if (max_candles > 0 && out.size() >= max_candles) break;
        }
    }

    // Sort candles by timestamp (input may not be ordered)
    std::sort(out.begin(), out.end(), [](const Candle& a, const Candle& b) {
        return a.ts < b.ts;
    });

    if (!cache_path.empty()) {
        write_candle_cache_atomic(cache_path, cache_key, out);
    }

    return out;
}

std::vector<Event> gen_events(const std::vector<Candle>& cs) {
    if (cs.size() > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
        throw std::runtime_error("Too many candles for 32-bit candle_idx");
    }

    std::vector<Event> evs;
    evs.reserve(cs.size() * 2);

    for (size_t idx = 0; idx < cs.size(); ++idx) {
        const auto& c = cs[idx];
        // Choose path: "low first" vs "high first" based on which is shorter
        const double path1 = std::abs(c.open - c.low)  + std::abs(c.high - c.close);
        const double path2 = std::abs(c.open - c.high) + std::abs(c.low  - c.close);
        const bool first_low = path1 < path2;

        const uint64_t ts0 = c.ts >= 5 ? c.ts - 5 : 0;
        const uint64_t ts1 = c.ts + 5;

        evs.push_back(Event{ts0, first_low ? c.low  : c.high, 0.0, 0, 0, c.volume / 2.0, static_cast<uint32_t>(idx)});
        evs.push_back(Event{ts1, first_low ? c.high : c.low, 0.0, 0, 0, c.volume / 2.0, static_cast<uint32_t>(idx)});
    }

    std::sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) {
        return a.ts < b.ts;
    });
    return evs;
}

} // namespace arb
