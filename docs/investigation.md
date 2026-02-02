# Benchmark Investigation

Investigation of observations raised about the ql-benchmarks CI results.
Each section corresponds to one of the flagged keypoints.

CI run data used: Linux `ql-benchmarks` combined report (ubuntu-latest container).

---

## 1. Unexpected magnitude of fixed overheads

**Observation:** FD Production shows ~9,282 ms at 10 paths, barely changing to ~9,368 ms at 100 paths and ~10,232 ms at 1K paths. The path-independent floor is ~9.3 seconds.

**Root cause: 46 full curve bootstraps per measurement.**

The FD benchmark (`benchmark_fd.cpp:88-167`) times the **entire bump-and-revalue cycle** within a single clock span. For Production config (45 market quotes), each measurement performs:

- 1 base price call to `priceSwaptionDualCurve<double>()`
- 45 bumped price calls (one per market quote)
- **Total: 46 calls**

Each call to `priceSwaptionDualCurve()` (`benchmark_pricing.hpp:330-504`) internally:
1. Bootstraps a `PiecewiseYieldCurve<ZeroYield, Linear>` for the forecasting curve (21 instruments)
2. Bootstraps a `PiecewiseYieldCurve<ZeroYield, Linear>` for the discounting curve (24 OIS instruments)
3. Constructs a `LiborForwardModelProcess` with covariance parametrization
4. Creates a `VanillaSwap` + `DiscountingSwapEngine` to compute the fair swap rate
5. Runs the MC loop

At 10 paths the MC loop is trivial. The ~9,300 ms is almost entirely **92 curve bootstraps** (46 calls x 2 curves each). This gives ~200 ms per call, consistent with:
- JIT Production Phase 1 (single dual-curve bootstrap + Jacobian): **218.6 ms**
- XAD-Split Production fixed cost (same): **~209 ms**

**This is not a measurement artifact.** FD inherently repeats the full curve construction for every bump. The AAD methods (XAD, XAD-Split, JIT) build curves once or a small number of times.

**Why ~200ms per call for plain double?** The bootstrap root-finding itself is not the main cost.
Each call to `priceSwaptionDualCurve<double>()` reconstructs the full QuantLib infrastructure:
- 45 RateHelper objects (each a `shared_ptr` allocation; 15 `SwapRateHelper` each construct a
  `VanillaSwap` with schedule generation; 20 `OISRateHelper` each construct an
  `OvernightIndexedSwap` with complex overnight-compounding schedules)
- 2 `PiecewiseYieldCurve` objects + lazy bootstrap triggers
- `LiborForwardModelProcess` + 20x20 covariance matrices
- `VanillaSwap` + `DiscountingSwapEngine` for fair swap rate

Evidence that QuantLib object construction dominates over numerical work:
FD per-call cost (~202ms, plain double) is nearly identical to XAD-Split fixed cost
(~209ms, which does 1 AD curve bootstrap + 41 adjoint passes). If the numerical bootstrap
were dominant, the AD version would be much more expensive. The similar cost implies that
`shared_ptr` allocations, Observer/Observable pattern setup, date/calendar computations,
and schedule generation are the true bottleneck — and these don't depend on the numeric type.

**Cross-check (Lite vs Production):** Lite config (9 inputs, single-curve) shows FD at 10 paths = 5.1 ms, meaning 10 calls x ~0.5 ms per call. A single-curve setup with 9 instruments and simpler helpers is ~400x cheaper, consistent with the overhead being driven by object count and complexity (OIS helpers are especially expensive).

---

## 2. Surprisingly small gap in per-path scaling between methods

**Observation:** XAD-Split and Forge JIT have very different computational approaches per path (tape re-recording vs compiled native code), yet their per-path timings are closer than expected.

**Analysis using Production config (subtracting fixed costs):**

| Method | 10K total (ms) | Fixed (ms) | Per-path (ms) | Per-path cost (us/path) |
|--------|---------------|------------|---------------|------------------------|
| XAD-Split | 2,315 | ~210 | ~2,105 | ~210.5 |
| Forge JIT | 1,549 | ~252 | ~1,297 | ~129.7 |
| Forge-AVX2 | 481 | ~252 | ~229 | ~22.9 |

So JIT is ~1.6x faster than XAD-Split per path — not a dramatic gap despite JIT executing compiled native code vs XAD-Split re-recording an XAD tape per path.

**Why the gap is smaller than expected:**

1. **XAD-Split's per-path tape is small.** It only records the payoff computation from intermediates (forward rates + swap rate + OIS discounts) to price. The expensive curve bootstrap is excluded. For Production config this is: LMM evolve (20 forward rates, 20 steps, ~20 factors) + discount factor computation + NPV. The tape is cleared and reused each path (`mcTape.clearAll()`), so memory allocation is amortized.

2. **JIT per-path loop has non-trivial overhead.** Each path in scalar JIT (`benchmark_aad.cpp:1413-1442`) does:
   - Set ~421 input values (20 initRates + 1 swapRate + 20 OIS discounts + 380 randoms)
   - `jit.forward()` — execute compiled kernel
   - `jit.clearDerivatives()`
   - `jit.setDerivative()`
   - `jit.computeAdjoints()` — execute compiled adjoint kernel
   - Read ~41 output derivatives

   The input-setting loop alone iterates 421 values per path. The kernel execution is fast, but the per-call overhead of setting up and reading back values through the JIT API adds up.

3. **The payoff computation is not purely arithmetic.** The LMM evolve step calls `exp()` and involves matrix-vector products (covariance x drift). XAD tape replay for such operations is reasonably efficient since the tape structure is fixed and reused.

**Verdict:** The ~1.6x gap is real and explained by the overhead structure. JIT's advantage grows at higher path counts where the compiled kernel execution dominates over setup. At 100K paths: JIT=12,862ms, XAD-Split=23,216ms, giving ~1.9x (after subtracting fixed costs: ~1.8x).

---

## 3. Vectorisation speedup appears stronger than lane width alone would imply

**Observation:** Forge-AVX2 processes 4 paths per SIMD instruction (AVX2 double = 256-bit / 64-bit = 4 lanes), yet the speedup over scalar Forge JIT appears larger than 4x in some cases.

**Measured speedups (JIT scalar vs JIT-AVX2, Production config):**

| Paths | JIT (ms) | JIT-AVX2 (ms) | Speedup |
|-------|----------|---------------|---------|
| 1K | 385.6 | 280.3 | 1.4x |
| 10K | 1,549 | 481.2 | 3.2x |
| 100K | 13,104 | 2,369 | 5.5x |

**After subtracting fixed cost (~252 ms):**

| Paths | JIT per-path (ms) | AVX2 per-path (ms) | Speedup |
|-------|-------------------|---------------------|---------|
| 1K | 133 | 28 | 4.8x |
| 10K | 1,297 | 229 | 5.7x |
| 100K | 12,852 | 2,117 | 6.1x |

The per-path speedup is indeed >4x, reaching ~6x at high path counts.

**Possible explanations:**

1. **Reduced per-path API overhead.** Scalar JIT processes 1 path per forward+backward call, requiring per-path: set 421 inputs, forward(), clearDerivatives(), setDerivative(), computeAdjoints(), read 41 gradients. AVX2 processes 4 paths per call via `forwardAndBackward()` — a single combined call that does forward + backward together. This eliminates 3/4 of the call overhead and may enable the backend to fuse forward/backward internally.

2. **Better memory access patterns.** AVX2 batched execution (`benchmark_aad.cpp:1547-1617`) sets inputs for all lanes at once via `avxBackend.setInput(k, inputBatch.data())`, allowing the backend to use aligned SIMD loads. The scalar JIT must go through the `value(vars.X) = ...` API per path.

3. **Instruction-level parallelism.** SIMD operations on 4 lanes can hide latency of dependent operations better than scalar code, especially for transcendentals like `exp()` which dominate the LMM evolve step.

4. **The `forwardAndBackward()` fusion.** The AVX2 backend uses a single `forwardAndBackward()` call rather than separate `forward()` + `computeAdjoints()`. This may allow the backend to avoid materializing intermediate values between forward and backward passes, reducing memory bandwidth.

**Lite config cross-check (to isolate kernel size effects):**

| Paths | JIT (ms) | AVX2 (ms) | Speedup (raw) | Per-path speedup (fixed~5ms) |
|-------|----------|-----------|--------------|------------------------------|
| 10K | 209.2 | 41.2 | 5.1x | ~5.7x |
| 100K | 2,056 | 366.2 | 5.6x | ~5.7x |

Consistent >5x even for Lite, confirming it's not config-specific.

**Verdict:** The >4x speedup is explained by a combination of reduced API call overhead (4x fewer calls), forwardAndBackward fusion, and better memory access patterns. This is not anomalous — it's expected when comparing a per-element API-call pattern against a batched SIMD execution model.

---

## 4. Inconsistencies between reported component timings and observed fixed costs

**Observation:** The JIT phase breakdown for Lite and Lite-Extended configs shows all zeros, despite these methods clearly having non-zero fixed costs visible from the scaling behavior.

**Evidence:**

Lite config:
```
JIT_PHASES_LITE:0.00,0.00,0.00
```
Yet JIT at 10 paths = 5.25 ms and at 100K paths = 2,056 ms. Linear extrapolation of per-path cost from 10K/100K gives ~0.0205 ms/path, implying ~5.0 ms fixed cost at 10 paths (5.25 - 10*0.0205). This small but real fixed cost is not captured.

Lite-Extended config:
```
JIT_PHASES_LITEEXT:0.00,0.00,0.00
```
Yet JIT at 10 paths = 39.4 ms. The per-path rate (from 10K/100K) is ~0.128 ms/path, giving ~38.1 ms fixed cost. This is significant and unreported.

Production config correctly reports:
```
JIT_PHASES_PRODUCTION:218.58,0.00,33.49
Total: 252.07 ms
```

**Root cause: Phase decomposition is only implemented for dual-curve path.**

Looking at the code:

- `runJITBenchmarkDualCurveImpl()` (`benchmark_aad.cpp:1371-1478`) — **has phase timing**: records `t_start`, `t_curve_end`, `t_compile_end`, and stores phase1/phase2/phase3 times.

- `runJITBenchmarkImpl()` (`benchmark_aad.cpp:1141-1226`) — **no phase timing**: only records `t_start` and `t_end`. Phase times are left at their default of 0.0. `jit_fixed_mean` is never set, so it stays at 0.0.

For single-curve configs (Lite, Lite-Extended), the code calls `runJITBenchmark()` → `runJITBenchmarkImpl()`, which doesn't decompose phases. The `jit_fixed_mean` remains 0 in the `TimingResult`, causing:
- The "Setup*" column to show 0.00 / not appear
- `JIT_PHASES_*` to output `0.00,0.00,0.00`
- The CI combined report to show "Total setup: 0.00"

Similarly for `runJITAVXBenchmark()` (single-curve AVX): no phase decomposition.

**The fixed cost is real but unmeasured for single-curve configs.** It can be inferred from the data:
- Lite: ~5 ms (curve bootstrap + JIT compile for 10 fwd rates, 8 steps, ~80 randoms)
- Lite-Extended: ~38 ms (curve bootstrap + JIT compile for 20 fwd rates, 20 steps, ~400 randoms)
- Production: ~252 ms (explicitly measured)

**XAD-Split fixed cost IS reported for all configs** because `runXADSplitBenchmark()` always records `fixed_times` (line 706). The raw data confirms:
```
XADSPLIT_LITE: fixed_cost ~1.3-1.6 ms
XADSPLIT_LITEEXT: fixed_cost ~4.5-4.7 ms (except 100K which shows 24.6 ms — possible outlier)
XADSPLIT_PRODUCTION: fixed_cost ~209 ms
```

**Additional inconsistency in Lite-Extended XAD-Split at 100K:**
```
XADSPLIT_LITEEXT: ...;100000=23216.43,0.00,1,24.63
```
The fixed cost jumps to 24.63 ms at 100K (vs ~4.6 ms at other path counts). This is the single-run case (0 warmup, 1 measured iteration), suggesting the first run includes JIT warm-up or memory allocation costs that subsequent runs amortize.

---

## 5. Potential timing attribution or measurement effects

**Observation:** Do the overall trends suggest that certain costs are being measured, amortized, or attributed inconsistently across methods?

### 5a. What each method's timer includes

| Method | Timer span | Includes curve build? | Includes MC loop? | Notes |
|--------|-----------|----------------------|-------------------|-------|
| FD | `t_start` to `t_end` | Yes, 46x (base + 45 bumps) | Yes, 46x | Each bump rebuilds curves from scratch |
| XAD | `t_start` to `t_end` | Yes, 1x (inside pricing fn) | Yes, 1x (all paths on one tape) | Tape records entire MC |
| XAD-Split | `t_start` to `t_end` | Yes, 1x (Phase 1) | Yes (per-path tape replay) | Fixed cost = Phase 1 only |
| JIT scalar | `t_start` to `t_end` | Yes, 1x (Phase 1) | Yes (compiled kernel per path) | Single-curve: no phase decomposition |
| JIT-AVX2 | `t_start` to `t_end` | Yes, 1x (Phase 1) | Yes (batched SIMD kernel) | Same as JIT re: phase reporting |

All methods measure wall-clock time from start to finish of a complete sensitivity computation. This is consistent.

### 5b. Warmup and iteration count differences

| Path count | Warmup | Measured | Notes |
|-----------|--------|----------|-------|
| < 10K | 2 (default) or 1 (quick) | 5 (default) or 2 (quick) | Standard warmup |
| 10K | 0 | 2 | Reduced for cost |
| >= 100K | 0 | 1 | Single measurement |

FD at >= 10K paths: 0 warmup, 1 measured run.

The 100K data points have **no warmup and a single measurement** (stddev = 0.0), making them more susceptible to system noise, JIT warm-up effects (for XAD-Split tape reuse), and cache cold-start effects.

### 5c. XAD full-tape anomaly at 100K paths (Lite-Extended)

```
XAD_LITEEXT: 100000=49217.60,0.00
```

XAD full-tape at 100K paths for Lite-Extended = **49,218 ms**, while FD = 35,607 ms. XAD is *slower* than FD here. This is because the XAD full-tape approach records all 100K MC paths on a single tape, causing massive memory consumption. At 100K paths with 20 forward rates and 20 time steps, the tape becomes extremely large, leading to poor cache behavior and potential memory allocation overhead. This is expected and is precisely why XAD-Split was developed.

### 5d. JIT fixed cost reported for JIT-AVX2

In `runAADBenchmarkDualCurve()` (line 1832), `jit_fixed_mean` is set from the **scalar JIT** run's phase timings. The same value is then used for JIT-AVX2's output (line 1907: `<< r.jit_fixed_mean`). However, JIT-AVX2 has its own setup via `runJITAVXBenchmarkDualCurve()` which independently measures `avx_p1, avx_p2, avx_p3` — but these are **not stored** in the TimingResult. The `jit_fixed_mean` used for JIT-AVX2 output is actually the scalar JIT's fixed cost.

In practice the fixed costs should be similar (same curve bootstrap, similar compilation), but this is a measurement attribution issue — AVX2's own phase data is computed then discarded.

### 5e. LMMSetup pre-computation is excluded from all timers

`LMMSetup` construction (random number generation, grid setup, etc.) happens **before** any benchmark loop. This is correct and consistent — all methods share the same pre-computed randoms. The random generation message "Generating 100000 x 380 random numbers..." appears before timing starts.

### Summary of measurement concerns

1. **Single-curve JIT phase reporting is missing** (Section 4) — fixed costs are real but reported as 0
2. **100K path data is single-shot** — no warmup, no stddev, susceptible to noise
3. **AVX2 uses scalar JIT's fixed cost** in output — minor attribution issue
4. **XAD full-tape at high path counts** hits memory wall, making it non-competitive (expected, not a measurement bug)
5. **Lite-Extended XAD-Split fixed cost at 100K** shows anomalous jump (24.6 ms vs ~4.6 ms) — likely cold-start effect in single-shot measurement

No evidence of systematic timing errors or unfair comparisons. The measurement methodology is sound for the comparative purpose. The main actionable items are: (1) add phase decomposition to single-curve JIT benchmarks, and (2) consider adding at least 1 warmup iteration for 100K path measurements to reduce cold-start effects.
