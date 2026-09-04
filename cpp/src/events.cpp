// Events module - implementation (non-templated)
#include "events/loader.hpp"

#include <boost/json.hpp>
#include <boost/json/basic_parser_impl.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace json = boost::json;

namespace arb {

namespace {

// Canonical input: an array of six numeric OHLCV fields per candle.
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
        if (depth == 2 && !unsupported) emit_row();
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
        if (row_len != 6) { unsupported = true; return; }
        for (const auto& value : row) {
            if (!std::isfinite(to_double(value))) { unsupported = true; return; }
        }
        const double raw_ts = to_double(row[0]);
        if (raw_ts < 0 || raw_ts >= 18446744073709551616.0 ||
            std::floor(raw_ts) != raw_ts) { unsupported = true; return; }
        for (size_t i = 1; i < 5; ++i) {
            if (!(to_double(row[i]) > 0)) { unsupported = true; return; }
        }
        if (to_double(row[5]) < 0) { unsupported = true; return; }
        if (capped) return;
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
    bool any_price_feed = false;
    for (const auto& e : evs) {
        if (e.p_price_feed != 0.0) {
            any_price_feed = true;
            break;
        }
    }
    if (any_price_feed) {
        s.p_price_feed.reserve(n);
        s.price_feed_ts.reserve(n);
    }
    for (const auto& e : evs) {
        s.ts.push_back(e.ts);
        s.p_cex.push_back(e.p_cex);
        s.volume.push_back(e.volume);
        s.candle_idx.push_back(e.candle_idx);
        if (any_price_feed) {
            s.p_price_feed.push_back(e.p_price_feed);
            s.price_feed_ts.push_back(e.price_feed_ts);
        }
    }
    s.price_blocks.build(s.p_cex);
    return s;
}

void PriceBlockIndex::build(const std::vector<double>& prices) {
    block_count = (prices.size() + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (block_count == 0) {
        min_positive.clear();
        max_positive.clear();
        return;
    }

    min_positive.assign(
        block_count, std::numeric_limits<double>::infinity()
    );
    max_positive.assign(block_count, 0.0);

    for (size_t block = 0; block < block_count; ++block) {
        const size_t begin = block * BLOCK_SIZE;
        const size_t end = std::min(begin + BLOCK_SIZE, prices.size());
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = 0.0;
        for (size_t index = begin; index < end; ++index) {
            const double price = prices[index];
            if (!(price > 0.0)) continue;
            minimum = std::min(minimum, price);
            maximum = std::max(maximum, price);
        }
        min_positive[block] = minimum;
        max_positive[block] = maximum;
    }
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
        throw std::runtime_error("Candles must be an array of six numeric OHLCV fields: " + path);
    }

    // Sort candles by timestamp (input may not be ordered)
    std::sort(out.begin(), out.end(), [](const Candle& a, const Candle& b) {
        return a.ts < b.ts;
    });

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

        evs.push_back(Event{ts0, first_low ? c.low  : c.high, 0.0, 0, c.volume / 2.0, static_cast<uint32_t>(idx)});
        evs.push_back(Event{ts1, first_low ? c.high : c.low, 0.0, 0, c.volume / 2.0, static_cast<uint32_t>(idx)});
    }

    std::sort(evs.begin(), evs.end(), [](const Event& a, const Event& b) {
        return a.ts < b.ts;
    });
    return evs;
}

} // namespace arb
