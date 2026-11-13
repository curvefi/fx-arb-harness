#pragma once

#include <cmath>

namespace arb::harness {

// Research model of a report bank: the newest report of an asset and, stored with each newer report, a rate read on chain
// at the delivery block (e.g. a Curve pool's price_oracle), with an exponential average of the rate of e-folding time
// `period` seconds. A pool reads the latest report times the rate's average projected to the block.
struct ReportBank {
    struct Stream {
        double price{0}, observation{0}, average{0};
        bool set{false};
    };
    double period{0};
    Stream asset, rate;

    // A value not newer than the stream's is skipped; the first one seeds the average; otherwise the new value enters
    // with the weight of the time since the stream's previous observation.
    static void store(Stream& s, double price, double observation, double period) {
        if (s.set && !(observation > s.observation)) return;
        if (!s.set) {
            s.average = price;
        } else {
            const double alpha = std::exp(-(observation - s.observation) / period);
            s.average = price * (1.0 - alpha) + s.average * alpha;
        }
        s.price = price, s.observation = observation, s.set = true;
    }
    // The stored average projected to `now` with the latest value held since its observation.
    static double view(const Stream& s, double now, double period) {
        if (!s.set || now <= s.observation) return s.average;
        const double alpha = std::exp(-(now - s.observation) / period);
        return s.price * (1.0 - alpha) + s.average * alpha;
    }
    // The rate the pool applies at `now`; 1 before the first delivery.
    double ratio(double now) const { return rate.set ? view(rate, now, period) : 1.0; }
    void deliver(double asset_price, double asset_observation, double rate_value, double now) {
        store(asset, asset_price, asset_observation, period);
        store(rate, rate_value, now, period);
    }
};

}  // namespace arb::harness
