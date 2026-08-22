#include "events/loader.hpp"
#include "oracles/chainlink.hpp"
#include "pools/pool_config_parse.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace json = boost::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> reference_split(const std::string& line) {
    std::vector<std::string> out;
    std::string field;
    std::stringstream ss(line);
    while (std::getline(ss, field, ',')) {
        out.push_back(field);
    }
    return out;
}

void check_line(const std::string& line) {
    require(
        arb::oracles::detail::split_csv_line(line) == reference_split(line),
        "CSV scanner diverged from std::getline reference"
    );
}

void test_csv_line_splitting() {
    check_line("1700000000,1.0,2.0,3.0,4.0,5.0,6.0,7.0");
    check_line("1700000000,,,,,,,7.0");
    check_line("1700000000,1.0,2.0,3.0,4.0,5.0,6.0,7.0\r");
}

void test_pool_init_binary64_boundary() {
    const auto entry = json::parse(R"JSON(
        {
          "pool": {
            "initial_liquidity": [
              "1500000000000000123456789",
              "2500000000000000987654321"
            ],
            "A": "1.1400000000000001",
            "gamma": "70000000000000123",
            "mid_fee": "1280000",
            "out_fee": "3270000",
            "fee_gamma": "30000000000000123",
            "adjustment_step_min": "1100000000000123",
            "adjustment_step_max": "5400000000000123",
            "ma_time": "866.0000000000001",
            "reserved_profit_fraction": "3400000123",
            "admin_fee": "5000000123",
            "initial_price": "64000000000000001234567",
            "start_timestamp": "1704066924",
            "donation_apy": "0.011400000000000001",
            "donation_frequency": "86400.00000000001",
            "donation_duration": "604800.0000000001",
            "initial_donation_days": "28.000000000000004",
            "donation_coins_ratio": "0.5000000000000001",
            "user_swap_size_frac": "0.037500000000000006"
          },
          "costs": {
            "arb_fee_bps": "2.0000000000000004",
            "gas_coin0": "1.1400000000000001",
            "use_volume_cap": true,
            "volume_cap_mult": "3.0000000000000004",
            "volume_cap_is_coin_1": true
          }
        }
    )JSON").as_object();

    arb::pools::PoolInit<double> pool_d{};
    arb::trading::Costs<double> costs_d{};
    arb::pools::parse_pool_entry(entry, pool_d, costs_d);

    arb::pools::PoolInit<long double> pool_ld{};
    arb::trading::Costs<long double> costs_ld{};
    arb::pools::parse_pool_entry(entry, pool_ld, costs_ld);

    require(pool_ld.start_ts == pool_d.start_ts, "start_timestamp changed by numeric mode");
    require(
        pool_d.user_swap_size_frac && pool_ld.user_swap_size_frac,
        "user_swap_size_frac override presence changed by numeric mode"
    );
    require(
        *pool_d.user_swap_size_frac == *pool_ld.user_swap_size_frac,
        "user_swap_size_frac override changed by numeric mode"
    );
    require(
        std::abs(*pool_d.user_swap_size_frac - 0.037500000000000006) < 1e-15,
        "user_swap_size_frac parsed wrong"
    );
}

void test_candidate_binary64_identity() {
    const double first = 1.0;
    const double second = std::nextafter(
        first, std::numeric_limits<double>::infinity());
    require(
        arb::canonical_binary64_string(first) ==
            arb::canonical_binary64_string(static_cast<long double>(first)),
        "one binary64 input must keep one identity after arithmetic widening"
    );
    require(
        arb::canonical_binary64_string(first) !=
            arb::canonical_binary64_string(second),
        "adjacent binary64 candidate inputs must have distinct identities"
    );
    require(
        arb::canonical_binary64_string(-0.0L) == "0",
        "signed zero must have one canonical identity"
    );
}

void test_binary64_parser_rejects_non_finite_values() {
    for (const char* text : {"nan", "inf", "-inf", "1e999"}) {
        bool rejected = false;
        try {
            (void)arb::parse_input_double(json::value(text));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "non-finite binary64 input was accepted");
    }
}

void test_bounded_uint_parser_is_strict() {
    const auto values = json::parse(R"JSON({
        "valid": 7, "negative": -1, "fraction": 1.0,
        "string": "1", "overflow": 4294967296
    })JSON").as_object();
    uint32_t parsed = 0;
    require(
        arb::parse_bounded_uint_field(values, "valid", uint32_t{0}, parsed) &&
            parsed == 7,
        "bounded uint parser rejected an in-range JSON integer"
    );
    for (const char* key : {"negative", "fraction", "string", "overflow"}) {
        require(
            !arb::parse_bounded_uint_field(values, key, uint32_t{0}, parsed),
            "bounded uint parser accepted an invalid integer representation"
        );
    }
}

} // namespace

void test_pool_override_materialized_once();

int main() {
    test_csv_line_splitting();
    test_pool_override_materialized_once();
    test_pool_init_binary64_boundary();
    test_candidate_binary64_identity();
    test_binary64_parser_rejects_non_finite_values();
    test_bounded_uint_parser_is_strict();
    std::cout << "test_io_fastpaths: PASSED\n";
    return 0;
}

void test_pool_override_materialized_once() {
    const auto entry = json::parse(R"JSON(
        {
          "pool": {"A": "2.0", "initial_price": "3000000000000000000"},
          "costs": {"gas_coin0": "4.0"}
        }
    )JSON").as_object();

    const auto override = arb::pools::parse_pool_override<double>(entry);
    arb::pools::PoolInit<double> first{};
    arb::pools::PoolInit<double> second{};
    first.gamma = 7.0;
    second.gamma = 9.0;
    arb::trading::Costs<double> first_costs{};
    arb::trading::Costs<double> second_costs{};
    override.apply(first, first_costs);
    override.apply(second, second_costs);

    require(first.A == 2.0 && second.A == 2.0, "pool override was not applied");
    require(first.initial_price == 3.0 && second.initial_price == 3.0, "scalar pool override was not applied");
    require(first.gamma == 7.0 && second.gamma == 9.0, "unspecified pool fields were overwritten");
    require(first_costs.gas_coin0 == 4.0 && second_costs.gas_coin0 == 4.0, "cost override was not applied");
    require(
        !first.user_swap_size_frac && !second.user_swap_size_frac,
        "unspecified user_swap_size_frac must stay unset"
    );

    const auto usw_entry = json::parse(R"JSON(
        {"pool": {"user_swap_size_frac": "0.08"}}
    )JSON").as_object();
    const auto usw_override = arb::pools::parse_pool_override<double>(usw_entry);
    usw_override.apply(first, first_costs);
    require(
        first.user_swap_size_frac && *first.user_swap_size_frac == 0.08,
        "pool-level user_swap_size_frac override was not applied"
    );
    require(
        !second.user_swap_size_frac,
        "user_swap_size_frac override leaked to an unselected pool"
    );
}
