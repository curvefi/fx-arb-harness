#include "price_feeds/price_feed.hpp"
#include "io/numeric_npz.hpp"
#include <cstring>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace arb {
namespace price_feeds {

namespace {

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

} // namespace

std::vector<PriceFeedPoint> load_price_feed_csv(const std::string& path) {
    if (std::filesystem::path(path).extension()==".npz") return load_price_feed_npz(path);
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Cannot open price feed: " + path);
    }

    std::vector<PriceFeedPoint> points;
    std::string line;
    bool schema_selected = false;
    size_t timestamp_column = 0;
    size_t price_column = std::numeric_limits<size_t>::max();
    while (std::getline(in, line)) {
        if (line.empty()) continue;

        const auto cols = split_csv_line(line);
        if (!schema_selected) {
            size_t named_timestamp = std::numeric_limits<size_t>::max();
            size_t named_price = std::numeric_limits<size_t>::max();
            for (size_t index = 0; index < cols.size(); ++index) {
                std::string name = cols[index];
                if (!name.empty() && name.back() == '\r') name.pop_back();
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (name == "timestamp" || name == "ts") named_timestamp = index;
                if (name == "price" || name == "nav" || name == "value") {
                    named_price = index;
                }
            }
            if (named_timestamp != std::numeric_limits<size_t>::max() ||
                named_price != std::numeric_limits<size_t>::max()) {
                if (named_timestamp == std::numeric_limits<size_t>::max() ||
                    named_price == std::numeric_limits<size_t>::max()) {
                    throw std::runtime_error(
                        "Price-feed header requires timestamp/ts and price/nav/value"
                    );
                }
                timestamp_column = named_timestamp;
                price_column = named_price;
                schema_selected = true;
                continue;
            }
            price_column = cols.size() == 2
                ? 1
                : (cols.size() >= 8 ? 7 : std::numeric_limits<size_t>::max());
            schema_selected = true;
        }

        if (price_column == std::numeric_limits<size_t>::max() ||
            timestamp_column >= cols.size() || price_column >= cols.size()) {
            continue;
        }
        try {
            double ts = std::stod(cols[timestamp_column]);
            if (ts > 10000000000.0) ts /= 1000.0;
            const double price = std::stod(cols[price_column]);
            if (std::isfinite(ts) && ts > 0 && std::isfinite(price) && price > 0.0) {
                points.push_back(PriceFeedPoint{ts, price});
            }
        } catch (...) {
            continue;
        }
    }

    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.ts < b.ts;
    });
    if (points.empty()) {
        throw std::runtime_error("Price feed has no usable rows: " + path);
    }
    return points;
}

void attach_price_feed(
    std::vector<Event>& events,
    const std::vector<PriceFeedPoint>& points
) {
    if (points.empty()) {
        throw std::runtime_error("Cannot attach prices from empty feed");
    }

    size_t idx = 0;
    for (auto& ev : events) {
        while (idx + 1 < points.size() && points[idx + 1].ts <= ev.ts) {
            ++idx;
        }
        if (points[idx].ts > ev.ts) {
            throw std::runtime_error(
                "Price feed has no value at or before first event timestamp"
            );
        }
        ev.p_price_feed = points[idx].price;
        ev.price_feed_ts = points[idx].ts;
    }
}

} // namespace price_feeds
} // namespace arb

namespace arb::price_feeds {
std::vector<PriceFeedPoint> load_price_feed_npz(const std::string& path) {
    auto arrays = io::read_arrays(path, {"ts.npy", "price.npy"});
    if (arrays.size()!=2) throw std::runtime_error("Price NPZ requires ts and price");
    const auto& ts=arrays.at("ts.npy"); const auto& price=arrays.at("price.npy");
    if (ts.shape.size()!=1 || price.shape!=ts.shape || price.dtype!="<f8" ||
        (ts.dtype!="<i8" && ts.dtype!="<f8"))
        throw std::runtime_error("Price NPZ requires Unix seconds ts and float64 price");
    std::vector<PriceFeedPoint> out; out.reserve(ts.shape[0]);
    double previous=0;
    for (size_t i=0; i<ts.shape[0]; ++i) {
        auto bits=io::little(ts.data()+8*i,8); double t,p;
        if (ts.dtype=="<i8") {
            if (bits>INT64_MAX) throw std::runtime_error("Invalid price timestamp");
            t=static_cast<double>(bits);
        } else std::memcpy(&t,&bits,8);
        bits=io::little(price.data()+8*i,8); std::memcpy(&p,&bits,8);
        if (!std::isfinite(t) || !std::isfinite(p) || t<=previous || p<=0)
            throw std::runtime_error("Invalid or unordered price NPZ");
        previous=t; out.push_back({t,p});
    }
    return out;
}
} // namespace arb::price_feeds
