// Events module - data types for candles and price events
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace arb {

// CEX candle data (OHLCV)
struct Candle {
    uint64_t ts;
    double open;
    double high;
    double low;
    double close;
    double volume;
};

// Simplified price event (timestamp + price + volume + source candle index)
struct Event {
    uint64_t ts;
    double p_cex;
    double p_price_feed{0.0};
    uint64_t price_feed_ts{0};
    double volume;
    uint32_t candle_idx;  // index into candle vector (used for detailed logging)
};

struct PriceBlockIndex {
    static constexpr size_t BLOCK_SIZE = 16;

    size_t block_count{0};
    std::vector<double> min_positive;
    std::vector<double> max_positive;

    void build(const std::vector<double>& prices);

    bool ready_for(size_t event_count) const {
        return block_count ==
                (event_count + BLOCK_SIZE - 1) / BLOCK_SIZE &&
            min_positive.size() == block_count &&
            max_positive.size() == block_count;
    }
};

// Structure-of-arrays event stream consumed by the event loop. The hot path
// reads only ts and p_cex per event; volume is touched on edge candidates,
// candle_idx only when detailed/YB sampling is on, and p_price_feed only by
// externally priced policy pools (the array stays empty when no feed was
// attached). Splitting the streams cuts the bytes touched per event from
// sizeof(Event) to 16-20, which matters when many threads each stream
// millions of events.
struct EventSoA {
    std::vector<uint64_t> ts;
    std::vector<double> p_cex;
    std::vector<double> volume;
    std::vector<uint32_t> candle_idx;
    std::vector<double> p_price_feed;  // empty unless a price feed was attached
    std::vector<uint64_t> price_feed_ts;
    PriceBlockIndex price_blocks;

    size_t size() const { return ts.size(); }
    bool empty() const { return ts.empty(); }

    static EventSoA from_events(const std::vector<Event>& evs);
};

} // namespace arb
