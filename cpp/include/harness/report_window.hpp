#pragma once

#include <algorithm>
#include <cstdint>
#include "events/types.hpp"

namespace arb::harness {

// Visit the complete immutable report tape, including observations between
// actor events. The caller evaluates economics; the window knows only time.
// An event index selects the tape's precomputed upper bound, when present.
class ReportWindow {
    const EventSoA& events;
    double max_age_s;
    size_t count;

    auto upper(double now, size_t event) const {
        return event < events.report_upper.size()
            ? events.report_ts.begin() + events.report_upper[event]
            : std::upper_bound(events.report_ts.begin(), events.report_ts.end(), now);
    }
public:
    static constexpr size_t NO_EVENT = SIZE_MAX;

    ReportWindow(const EventSoA& source, double age, size_t last_count = 0)
        : events(source), max_age_s(age), count(last_count) {}

    template <typename Visit>
    void each(double now, Visit visit, size_t event = NO_EVENT) const {
        const auto last = upper(now, event);
        const auto first = count > 0
            ? last - std::min(count, static_cast<size_t>(last-events.report_ts.begin()))
            : std::lower_bound(events.report_ts.begin(), last, now-max_age_s);
        for (auto it=first; it!=last; ++it) {
            const auto i = static_cast<size_t>(it-events.report_ts.begin());
            visit(events.report_prices[i], *it);
        }
    }

    template <typename Visit>
    void at_offset(double now, size_t offset, Visit visit, size_t event = NO_EVENT) const {
        const auto available = static_cast<size_t>(upper(now, event) - events.report_ts.begin());
        if (offset >= available) return;
        const size_t i = available - 1 - offset;
        visit(events.report_prices[i], events.report_ts[i]);
    }

    template <typename Visit>
    void sample_last(double now, size_t window_count, uint64_t draw, Visit visit,
                     size_t event = NO_EVENT) const {
        const size_t available = static_cast<size_t>(upper(now, event) - events.report_ts.begin());
        const size_t choices = std::min(window_count, available);
        if (choices == 0) return;
        const size_t offset = static_cast<size_t>(draw % choices);
        const size_t i = available - 1 - offset;
        visit(events.report_prices[i], events.report_ts[i]);
    }
};

// Reports published at or before each event, for the ReportWindow fast path.
inline void index_report_events(EventSoA& events) {
    events.report_upper.resize(events.size());
    for (size_t index = 0; index < events.size(); ++index)
        events.report_upper[index] = static_cast<uint32_t>(std::upper_bound(
            events.report_ts.begin(), events.report_ts.end(),
            static_cast<double>(events.ts[index])) - events.report_ts.begin());
}

} // namespace arb::harness
