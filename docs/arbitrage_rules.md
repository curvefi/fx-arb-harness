# Arbitrage rules

These are the rules the simulator must obey when it arbitrages. There are two
actors. Each has one rule and a small, fixed set of settings. Nothing in either
rule is random.

## Common ground

- **One external price per event.** Every actor sees the same event price `P`.
  In block mode `P` is the external price `arb_settle_offset_s` seconds after
  the block timestamp. Standard setting: 1.
- **Order within an event.** The native arbitrageur acts first, then the
  YieldBasis actor, both at the same `P`.
- **At most one trade per actor per event.**
- **No volume limit.** The size of a trade does not depend on the volume that
  traded on the external venue.
- **No inventory limit.** An actor can always fund its trade.
- **Deterministic.** The same inputs give the same trades. There is no miss
  rate, no noise and no seed.

## Rule 1: native arbitrageur (the pool)

The native arbitrageur trades the pool against `P`.

1. **Entry.** Let `p` be the pool spot price and `f` its current fee. The first
   unit's edge is `max((1 - f) * P / p, (1 - f) * p / P)`. It trades only if
   that edge is above `exp(entry_edge_bps / 1e4)`.
2. **Size.** It takes the size with the largest profit, valuing the external
   leg at `P` less `arb_fee_bps` and subtracting `gas_coin0`. The size is
   bounded only by `min_swap` and `max_swap` of the input coin's balance.
3. **No trade** if the best size has no positive profit.

| Setting | Meaning | Standard |
| --- | --- | --- |
| `pool.costs.entry_edge_bps` | least first-unit edge to enter | 1.5 |
| `pool.costs.arb_fee_bps` | cost of the external leg | 0 |
| `pool.costs.gas_coin0` | fixed cost of a trade | 0 |

Report costs and report selection, where a policy uses them, are described in
the protocol specification. They do not change this rule.

## Rule 2: YieldBasis actor (the LEVAMM)

The YieldBasis actor (`yb_mode = "active_2l"`, `yb_arb = "levamm"`) trades the
LEVAMM through its fee-paying exchange. It has its own execution setting and
does not use the native arbitrageur's costs.

1. **Fair value.** The fair LP price is the pool's balances valued at `P`,
   divided by the LP supply. Coin 1 is valued at `P` less `yb_execution_bps`
   when the actor sells it and `P` plus `yb_execution_bps` when it buys it.
2. **Entry.** It trades only when the LEVAMM LP price, after the LEVAMM fee, is
   on the profitable side of that fair value.
3. **Size.** It fills until the LEVAMM LP price, after the fee, is back at the
   fair value.
4. **Floor.** It trades only if its net profit is above
   `yb_min_net_profit_coin0`.

| Setting | Meaning | Standard |
| --- | --- | --- |
| `yb_execution_bps` | execution margin of the actor's external leg | 5 |
| `yb_min_net_profit_coin0` | least net profit of a fill | 1 |

## What the rules exclude

A run that follows these rules does not use any of the following. Where an
option for one of them still exists, it stays at its off value.

- Fill caps on the YieldBasis actor: `yb_cap_crvusd`, `yb_cap_vol_ref_bp`,
  `yb_max_fill_frac`.
- The whole-route option (`yb_whole_route`), the leverage-restoring deposit
  actor (`yb_releverage`) and strict contract checks (`yb_strict_checks`).
- A second decision within an event.
- Any volume cap, maker fill book or order-book depth.
- Random entry, random misses and partial closure.

## Checks

A change to the arbitrage code must keep these true:

- With `entry_edge_bps = 0` and zero costs, the native arbitrageur leaves no
  profitable trade in the pool at `P`.
- Raising `entry_edge_bps` never adds a trade at an event; it can only remove
  one.
- The fast event cursor and the scalar cursor give the same trades and the
  same final pool state.
- Two runs with the same inputs give identical results.
