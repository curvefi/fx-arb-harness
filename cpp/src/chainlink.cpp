#include "oracles/chainlink.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace arb {
namespace oracles {

namespace detail {

std::vector<std::string> split_csv_line(const std::string& line) {
    // Match std::getline(stringstream, field, ',') exactly: preserve empty
    // interior fields and carriage returns, but do not append an empty field
    // for a trailing delimiter.
    std::vector<std::string> out;
    size_t pos = 0;
    const size_t n = line.size();
    while (pos < n) {
        const size_t comma = line.find(',', pos);
        if (comma == std::string::npos) {
            out.emplace_back(line, pos, n - pos);
            return out;
        }
        out.emplace_back(line, pos, comma - pos);
        pos = comma + 1;
    }
    return out;
}

} // namespace detail

std::vector<ChainlinkPoint> load_chainlink_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open Chainlink feed: " + path);
    }

    std::vector<ChainlinkPoint> points;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (first) {
            first = false;
            if (line.find("timestamp") != std::string::npos) {
                continue;
            }
        }

        const auto cols = detail::split_csv_line(line);
        if (cols.size() < 8) continue;
        try {
            uint64_t ts = static_cast<uint64_t>(std::stoull(cols[0]));
            if (ts > 10000000000ULL) ts /= 1000ULL;
            const double price = std::stod(cols[7]);
            if (ts > 0 && price > 0.0) {
                points.push_back(ChainlinkPoint{ts, price});
            }
        } catch (...) {
            continue;
        }
    }

    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.ts < b.ts;
    });
    if (points.empty()) {
        throw std::runtime_error("Chainlink feed has no usable rows: " + path);
    }
    return points;
}

void attach_chainlink_prices(
    std::vector<Event>& events,
    const std::vector<ChainlinkPoint>& points
) {
    if (points.empty()) {
        throw std::runtime_error("Cannot attach Chainlink prices from empty feed");
    }

    size_t idx = 0;
    for (auto& ev : events) {
        while (idx + 1 < points.size() && points[idx + 1].ts <= ev.ts) {
            ++idx;
        }
        if (points[idx].ts > ev.ts) {
            throw std::runtime_error(
                "Chainlink feed has no answer at or before first event timestamp"
            );
        }
        ev.p_chainlink = points[idx].price;
        ev.chainlink_ts = points[idx].ts;
        ev.chainlink_answer_index = static_cast<uint64_t>(idx);
    }
}

} // namespace oracles
} // namespace arb
