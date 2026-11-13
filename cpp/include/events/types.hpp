// Events module - data types for candles and price events
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
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
    double price_feed_ts{0};
    double volume;
    uint32_t candle_idx;  // index into candle vector (used for detailed logging)
};

// Structure-of-arrays event stream consumed by the event loop. The hot path
// reads only ts and p_cex per event;
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
    std::vector<double> price_feed_ts;
    // Complete independent report tape, including observations between events.
    std::vector<double> report_ts;
    std::vector<double> report_prices;
    // Research, optional: a report bank (harness/report_bank.hpp): a rate series (sample times, values) read at each
    // delivery and the bank's average period in seconds (empty / 0: no bank).
    std::vector<double> report_rate_ts, report_rate;
    double report_bank_period_s{0};
    // Optional: reports published at or before each event (empty = search).
    std::vector<uint32_t> report_upper;
    // Block mode only: the event price at each delay after the block (index = seconds); p_cex is the column of
    // the session's own delay. Empty otherwise.
    std::array<std::vector<double>, 3> p_cex_delay;
    // Block mode, optional: the external price at which the native arbitrageur and the YieldBasis actor trade (e.g. the
    // asset in the pool's stablecoin), NaN where neither trades. Empty: the event price. Sizing, profit and notional use
    // this price; fee valuation, marks and metrics use the event price.
    std::vector<double> p_arb;
    // Block mode, optional (research, harness/oracle_rate.hpp): a rate oracle read at report delivery that divides the
    // reports the fee terms read: its live value and the cache other writers left (value, write time) at each event, and
    // the rule {calm age limit s, off-peg age limit s, peg band}. Empty: none.
    std::vector<double> oracle_live, oracle_cached, oracle_cached_ts, oracle_rule;

    // Event prices of one run: the session's (delay_s < 0) or those at an explicit delay after the block.
    const std::vector<double>& prices(int delay_s) const {
        if (delay_s < 0) return p_cex;
        if (delay_s >= 3 || p_cex_delay[static_cast<size_t>(delay_s)].size() != p_cex.size())
            throw std::invalid_argument("arb_settle_offset_s needs event_mode='block' and a delay of 0, 1 or 2 s");
        return p_cex_delay[static_cast<size_t>(delay_s)];
    }

    size_t size() const { return ts.size(); }
    bool empty() const { return ts.empty(); }

    static EventSoA from_events(const std::vector<Event>& evs);
};

} // namespace arb
