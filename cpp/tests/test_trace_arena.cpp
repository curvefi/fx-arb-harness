#include <iostream>
#include <string>

#include <boost/json.hpp>

#include "curve_fx_evaluator/trace.hpp"

namespace {

void require(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace curve_fx::evaluator;

    auto& arena = TraceArena::global_instance();

    {
        auto lease = arena.acquire();

        arb::harness::DetailedEntry<RealT> entry{};
        entry.t = 1700000000;
        entry.vp = RealT(1.0);
        entry.token0 = RealT(1000000.0);
        entry.token1 = RealT(1000000.0);
        lease.detailed_entries().push_back(entry);

        arb::harness::TickAction<RealT> act{};
        act.ts = 1700000000;
        act.p_cex = RealT(1.0);
        lease.actions().push_back(act);

        const auto trace = boost::json::parse(
            serialize_detailed_entries_json(lease.detailed_entries()));
        const auto& trace_entry = trace.as_array().at(0).as_object();
        require(trace_entry.contains("xp_0"), "trace serializer omitted xp_0");
        require(trace_entry.contains("p_chainlink"), "trace serializer omitted p_chainlink");
        require(trace_entry.contains("yb_debt"), "trace serializer omitted YB position");

        const auto actions = boost::json::parse(
            serialize_actions_json(lease.actions()));
        const auto& action = actions.as_array().at(0).as_object();
        require(action.at("type").as_string() == "tick", "action serializer omitted type");
        require(action.contains("p_cex"), "action serializer omitted tick economics");
    }

    {
        auto lease2 = arena.acquire();
        require(lease2.detailed_entries().empty(), "re-acquired lease must have cleared entries");
        require(lease2.actions().empty(), "re-acquired lease must have cleared actions");
    }

    std::cout << "test_trace_arena: PASSED\n";
    return 0;
}
