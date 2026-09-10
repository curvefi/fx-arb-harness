#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

#include "pools/twocrypto_fx/twocrypto.hpp"
#include "trading/arbitrageur.hpp"
#include "trading/cex_depth.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "CEX depth test failed: " << message << '\n';
        std::exit(1);
    }
}

using Book = arb::trading::CexDepthBook<long double>;
using Pool = arb::pools::twocrypto_fx::TwoCryptoPool<double>;

arb::trading::CexDepthSnapshot snapshot() {
    return {42, {{10.0, 2.0}, {9.0, 3.0}}, {{11.0, 1.0}, {12.0, 4.0}}};
}

void quotes_cross_levels() {
    const auto data = snapshot();
    const Book book(&data);
    require(*book.sell_base(4.0L) == 38.0L, "sell quote did not cross bids");
    require(*book.buy_base(3.0L) == 35.0L, "buy quote did not cross asks");
    require(*book.bid_price() == 10.0 && *book.ask_price() == 11.0,
            "best prices are wrong");
}

void consume_copy_and_reset() {
    const auto data = snapshot();
    Book book(&data);
    require(book.consume_sell(2.0L), "first sell consumption rejected");
    Book preview = book;
    require(preview.consume_sell(2.0L) && *preview.bid_price() == 9.0,
            "copy was not an independent preview");
    require(*book.bid_capacity() == 3.0L, "original consumption changed by copy");
    book.reset(&data);
    require(*book.bid_capacity() == 5.0L && *book.sell_base(0.0L) == 0.0L,
            "reset did not restore depth");
}

void invalid_and_closed() {
    const auto data = snapshot();
    Book book(&data);
    require(!book.sell_base(-1.0L) && !book.sell_base(std::numeric_limits<long double>::infinity()),
            "invalid quantity was quoted");
    require(!book.consume_sell(6.0L), "over-capacity sell was consumed");
    require(book.consume_sell(5.0L) && !book.sell_base(1.0L) && !book.bid_capacity(),
            "exhausted bids remained available");
    bool rejected = false;
    try {
        arb::trading::CexDepthSnapshot invalid{0, {{10.0, 1.0}}, {{9.0, 1.0}}};
        book.reset(&invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "invalid snapshot was accepted");
}

void linear_depth_integrates_marginal_prices() {
    // Midpoint 100: 100 BTC at +200 bp, 200 BTC at +500 bp.
    arb::trading::CexDepthSnapshot data{1, {{100.,10.},{98.,90.},{95.,100.}},
        {{100.,10.},{102.,90.},{105.,100.}}, arb::trading::DepthInterpolation::Linear};
    Book book(&data);
    require(std::abs(*book.buy_base(150.)-15227.5L) < 1e-10L &&
                std::abs(*book.sell_base(150.)-14772.5L) < 1e-10L,
            "linear depth did not integrate marginal prices");
    require(book.consume_buy(100.) && *book.ask_price() == 102. &&
                std::abs(*book.buy_base(50.)-5137.5L) < 1e-10L,
            "partial linear fill has the wrong incremental cost");
    require(book.consume_buy(50.) && *book.ask_price() == 103.5 && !book.buy_base(51.),
            "150 BTC must reach 350 bp and retain only 50 BTC capacity");
    data.asks.insert(data.asks.begin()+2, {103.,0.});
    book.reset(&data);
    require(book.consume_buy(100.) && *book.ask_price() == 103. &&
                std::abs(*book.buy_base(50.)-5175.L) < 1e-10L,
            "zero-volume price gap invented liquidity");
}

void native_sizer_uses_finite_depth() {
    const auto run = [](const arb::trading::CexDepthSnapshot& data, int direction) {
        Pool pool({1.0, 1.0}, 50'000.0, 1.1111111111e-8, 0.0146, 0.0170,
                  0.054202748, 1e-10, 5e-3, 865.0, 77'235.68, 0.301010101, 0.0);
        pool.set_block_timestamp(1'779'753'600 - 1);
        pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
        pool.set_block_timestamp(1'779'753'600);
        arb::trading::Costs<double> costs;
        costs.arb_fee_bps = 0.0;
        arb::trading::CexDepthBook<double> depth(&data);
        const auto capacity = direction == 0 ? depth.bid_capacity() : depth.ask_capacity();
        require(capacity.has_value(), "finite depth capacity was unavailable");
        const double upper = direction == 0
            ? std::min(pool.balances[0], pool.get_dx(0, 1, *capacity, 5))
            : std::min(pool.balances[1], *capacity);
        const double lower = pool.balances[direction] * 1e-8;
        const auto decision = arb::trading::decide_trade(
            pool, 1.0, costs, std::numeric_limits<double>::infinity(), 1e-8, 1.0,
            1.0, 1.0, static_cast<const double*>(nullptr),
            static_cast<const double*>(nullptr), static_cast<const std::array<double, 2>*>(nullptr),
            &depth);
        require(decision.do_trade && decision.i == direction, "finite depth direction was not selected");
        require(decision.dx <= upper, "selected size exceeded finite depth cap");
        arb::trading::CexDepthBook<double> exact_book(&data);
        const double cash = direction == 0
            ? *exact_book.sell_base(decision.dy_after_fee) - decision.dx
            : decision.dy_after_fee - *exact_book.buy_base(decision.dx);
        require(std::abs(decision.profit - cash) < 1e-9, "profit did not match depth cashflow");
        double grid_best = -std::numeric_limits<double>::infinity();
        for (int k = 0; k <= 512; ++k) {
            const double dx = lower * std::pow(upper / lower, static_cast<double>(k) / 512.0);
            const auto sim = arb::pools::twocrypto_fx::simulate_exchange_once(
                pool, static_cast<size_t>(direction), static_cast<size_t>(1 - direction), dx);
            arb::trading::CexDepthBook<double> grid_book(&data);
            const auto quote = direction == 0
                ? grid_book.sell_base(sim.first) : grid_book.buy_base(dx);
            if (quote) {
                const double profit = direction == 0
                    ? *quote - dx : sim.first - *quote;
                grid_best = std::max(grid_best, profit);
            }
        }
        require(decision.profit + std::max(1e-6, std::abs(grid_best) * 0.02) >= grid_best,
                "selected profit missed the 512-point reference grid");
        return decision;
    };
    const arb::trading::CexDepthSnapshot sell{1, {{120'000.0, 0.25}, {110'000.0, 0.25}}, {{200'000.0, 1.0}}};
    const arb::trading::CexDepthSnapshot buy{1, {{50'000.0, 1.0}}, {{70'000.0, 0.25}, {80'000.0, 0.25}}};
    const auto sell_decision = run(sell, 0);
    const auto buy_decision = run(buy, 1);
    auto linear_sell = sell, linear_buy = buy;
    linear_sell.interpolation = linear_buy.interpolation = arb::trading::DepthInterpolation::Linear;
    run(linear_sell, 0);
    run(linear_buy, 1);
    Pool legacy_pool({1.0, 1.0}, 50'000.0, 1.1111111111e-8, 0.0146, 0.0170,
                     0.054202748, 1e-10, 5e-3, 865.0, 77'235.68, 0.301010101, 0.0);
    legacy_pool.set_block_timestamp(1'779'753'599);
    legacy_pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
    legacy_pool.set_block_timestamp(1'779'753'600);
    arb::trading::Costs<double> costs;
    const arb::trading::CexDepthSnapshot large_cap{
        1, {{120'000.0, legacy_pool.balances[1] * 0.999}}, {{200'000.0, 1.0}}};
    arb::trading::CexDepthBook<double> large_book(&large_cap);
    const auto large_decision = arb::trading::decide_trade(
        legacy_pool, 1.0, costs, 1e9, 1e-8, 0.01, 1.0, 1.0,
        static_cast<const double*>(nullptr), static_cast<const double*>(nullptr),
        static_cast<const std::array<double, 2>*>(nullptr), &large_book);
    require(large_decision.do_trade,
            "valid large bid capacity failed before bounded sizing");
    const auto omitted = arb::trading::decide_trade(legacy_pool, 1.0, costs, 1e9, 1e-8, 1.0, 1.0, 1.0);
    const auto explicit_null = arb::trading::decide_trade(
        legacy_pool, 1.0, costs, 1e9, 1e-8, 1.0, 1.0, 1.0,
        static_cast<const double*>(nullptr), static_cast<const double*>(nullptr),
        static_cast<const std::array<double, 2>*>(nullptr),
        static_cast<const arb::trading::CexDepthBook<double>*>(nullptr));
    require(omitted.do_trade == explicit_null.do_trade && omitted.dx == explicit_null.dx &&
                omitted.profit == explicit_null.profit,
            "omitted and null depth changed legacy arithmetic");
    require(sell_decision.profit > 0.0 && buy_decision.profit > 0.0,
            "finite depth trades were not profitable");
}

} // namespace

int main() {
    quotes_cross_levels();
    consume_copy_and_reset();
    invalid_and_closed();
    linear_depth_integrates_marginal_prices();
    native_sizer_uses_finite_depth();
    std::cout << "test_cex_depth: PASSED\n";
}
