# Results

Every number below was measured on the machine described here. The raw
output of each run is reproduced at the end of this file, and saved under
`bench/results/`. Nothing is estimated or rounded up. The ratios are computed
from the medians in the raw output.

## Hardware and software

* Apple M3 (4 performance + 4 efficiency cores), 16 GB RAM, macOS 15.7
  (Darwin 24.6.0)
* Apple clang 15 (`-O3 -DNDEBUG -ffp-contract=off`), LLVM 23.1.2 (Homebrew)
  for the JIT, single thread
* The benchmark thread requests `QOS_CLASS_USER_INTERACTIVE` so macOS prefers
  a performance core.

**Caveat: the machine was shared.** Other heavy jobs were running at the same
time: a load generator, a server and redis from another project, using 2–3
cores. Load averages are recorded before and after every run. They ranged from
3.3 to 22 during benchmark runs, and 29 was seen at one point in the
session. Swap was already 6–8.5 GB in use by other processes. The first 50M
attempts were badly distorted by this (see "Run history" below). The final
50M numbers come from quieter periods (load ≈ 3–6), and they are consistent
with the 10M runs, which fit comfortably in memory and are very stable
(min ≈ median). Treat the 50M window numbers as the least reliable: they are
the most memory-hungry query (8 output columns, about 3 GB of output).

## What is measured

`bench/bench_queries.cpp`, driven by `bench/run_bench.sh`. For each query
type, the same output is produced three ways:

* **interp**: the tree-walking interpreter (`src/interp.cpp`). It
  materializes every stage and evaluates expressions per row by recursion.
* **jit**: the LLVM-compiled fused loop, called via `run_raw` with output
  buffers that are pre-allocated and pre-faulted.
* **hand**: hand-written C++ loops (`bench/handwritten.cpp`), compiled with
  `-O3` and writing into the same pre-faulted buffers.

Methodology:

* JIT and hand runs are **interleaved** (jit, hand, jit, hand, …) after one
  warm-up each.
* The interpreter is timed in a **separate process** (`--interp-only`),
  because its multi-GB allocations otherwise push the inputs into macOS's
  compressor and slow down the runs that follow.
* `jit_cold` is a single JIT run through `run()`. It allocates fresh output
  buffers, so page faults are included.
* For each query, the output hash (rows plus every column, NaN canonicalized)
  must match across interp, jit and hand. Every run reported here printed
  `match yes`.
* Compile time is the median of 5 fresh compiles. Each compile creates a new
  LLJIT. `c_parse` is lexing, parsing and binding. `c_irgen` is BoundQuery to
  IR. `c_opt` is the `-O2` pipeline. `c_mc` is LLJIT machine-code generation
  plus lookup. `c_total` is the wall time of the whole `compile()` call,
  including target-machine and LLJIT setup.
* Data comes from `gen_trades` / `gen_quotes` (deterministic). `trades` is
  ts, sym, price and size. `quotes` is ts, sym, bid and ask. There are 8
  symbols, mean gaps of 1 ms, and AAPL's price mean-reverts around 100.

Queries:

| name | query |
|---|---|
| filter | `trades \| filter price > 100 and sym == "AAPL"` (≈6% of rows pass) |
| filter_wide | `trades \| filter price > 100 \| select ts, sym, price` (≈81% pass) |
| filter_project | `trades \| filter price > 100 and size < 500 \| select ts, notional = price * size` |
| window | `trades \| window 5m sum(size), mean(price), min(price), max(price)` |
| asof | `trades \| asof_join quotes on sym` |
| combined | `trades \| filter price > 100 and sym == "AAPL" \| window 5m mean(price) \| asof_join quotes on sym` |
| `<q>/&` | same query; the hand-written version uses a non-short-circuit `&` in the predicate |

## Headline: 50M rows (trades = 50M, quotes = 50M)

Commands:

```
bench/run_bench.sh --rows 50000000 --reps 7 --no-interp                 # bench/results/50M-native-final.txt
bench/run_bench.sh --rows 50000000 --reps 9 --no-interp --only window   # bench/results/50M-native-window-rerun.txt
bench/run_bench.sh --rows 50000000 --reps 5 --interp-only               # bench/results/50M-interp-final.txt
```

(These ran as `build/bench_queries` before `run_bench.sh` moved to its own
`build-bench/` directory. The code and flags are the same.)

| query | out rows | interp ms | JIT ms | hand ms | interp / JIT | JIT / hand |
|---|---:|---:|---:|---:|---:|---:|
| filter | 3,161,066 | 859.6 | 41.9 | 101.5 | **20.5×** | **0.41** |
| filter (hand with `&`) | 3,161,066 | 862.0 | 42.1 | 47.1 | 20.5× | 0.89 |
| filter_wide | 40,660,508 | 1011.3 | 85.4 | 94.9 | **11.8×** | **0.90** |
| filter_project | 20,289,968 | 1434.0 | 212.5 | 216.7 | **6.7×** | **0.98** |
| window ¹ | 50,000,000 | 1935.1 | 987.4 | 1014.2 | **1.96×** | **0.97** |
| asof | 50,000,000 | 508.2 | 306.8 | 310.5 | **1.66×** | **0.99** |
| combined | 3,161,066 | 946.0 | 97.6 | 159.8 | **9.7×** | **0.61** |
| combined (hand with `&`) | 3,161,066 | 956.0 | 95.8 | 97.4 | 10.0× | 0.98 |

¹ Window JIT/hand numbers are from the window-only rerun, which keeps the
quotes table out of memory. In the all-queries run, the same query measured
JIT median 1983.1 / min 1077.5 ms and hand median 2084.5 / min 1025.2 ms. The
large min-to-median spread there comes from memory pressure.

JIT compile time at 50M (median of 5, ms). Compile time does not depend on
data size:

| query | parse+bind | IR gen | -O2 | machine code | total `compile()` |
|---|---:|---:|---:|---:|---:|
| filter | 0.004 | 0.022 | 0.76 | 2.31 | 3.21 |
| filter_wide | 0.005 | 0.021 | 0.68 | 1.25 | 2.06 |
| filter_project | 0.006 | 0.020 | 0.65 | 1.24 | 2.01 |
| window | 0.004 | 0.046 | 2.11 | 3.51 | 5.78 |
| asof | 0.003 | 0.034 | 1.25 | 3.06 | 4.69 |
| combined | 0.007 | 0.046 | 1.68 | 3.44 | 5.28 |

## 10M rows (stable reference)

```
bench/run_bench.sh --rows 10000000 --reps 9 --no-interp   # bench/results/10M-native-final.txt
bench/run_bench.sh --rows 10000000 --reps 5 --interp-only # bench/results/10M-interp-final.txt
```

| query | interp ms | JIT ms (min) | hand ms (min) | interp / JIT | JIT / hand |
|---|---:|---:|---:|---:|---:|
| filter | 167.9 | 8.3 (8.2) | 20.1 (20.1) | 20.2× | 0.41 |
| filter (hand `&`) | 166.1 | 8.1 (8.1) | 9.1 (9.0) | 20.5× | 0.90 |
| filter_wide | 165.5 | 16.6 (16.5) | 18.7 (18.2) | 10.0× | 0.89 |
| filter_project | 275.7 | 42.4 (42.3) | 43.1 (42.8) | 6.5× | 0.98 |
| window | 325.0 | 189.3 (188.9) | 193.8 (191.2) | 1.72× | 0.98 |
| asof | 86.9 | 58.0 (57.7) | 58.7 (57.9) | 1.50× | 0.99 |
| combined | 183.6 | 18.6 (18.4) | 30.1 (29.8) | 9.9× | 0.62 |
| combined (hand `&`) | 182.3 | 19.0 (18.8) | 19.0 (18.5) | 9.6× | 1.00 |

## Summary

| comparison | result |
|---|---|
| JIT vs the interpreter | filter 20.5×, filter_wide 11.8×, filter_project 6.7×, combined 9.7×, window 1.96×, asof 1.66× (50M). |
| JIT vs hand-written C++ | Within 20% for every query. The JIT takes 0.41–0.99× the hand-written time at 50M. Against the best hand-written variant (with `&`) it is 0.89–0.99×. The JIT is never slower than the hand-written code by more than the run-to-run noise. |
| JIT compile time | 2.0–5.8 ms per query in total. Machine-code generation (1.2–3.5 ms) and `-O2` (0.65–2.1 ms) dominate. |

**Why window and asof gain less over the interpreter.** The interpreter is only
tree-walking for *expressions*. Its window and asof stages are compiled C++
loops that run the same incremental algorithms as the JIT: a running sum,
monotonic deques, and a forward merge cursor. For these two queries almost
all of the time goes into that algorithm itself. For min/max that means a
data-dependent branch and a dependent load per row. For asof it means the
merge loop. That cost is the same in all three implementations, which is why
JIT ≈ hand there. The JIT only removes the interpreter's materialization
(gather/copy per stage) and per-row dispatch, and these queries have almost
none of that. Queries that are expression-heavy or filter-heavy (filter,
filter_project, combined) show the large 6.7–20× gaps. A much larger gap on
window would need a slower reference: for example, a fully generic
row-at-a-time window operator that calls back into the expression
interpreter for every value. The reference is deliberately not slowed down
artificially.

**Why the JIT beats the natural hand-written filter by 2.4×.** The
hand-written `price > 100 && sym == aapl` short-circuits. clang keeps two
branches, and the first one (`price > 100`, about 62% taken) is unpredictable.
The JIT evaluates both comparisons and branches once on their `and`, which is
taken only about 6% of the time and predicts well. That is legal because
expressions cannot trap. With `&` in the hand-written code
(`filter (hand &)`), the gap shrinks to about 10%.

## Optimizations, one at a time (before → after)

Both are on by default, and both can be switched off in `CompileOptions` or
in `tsq.compile(...)`.

1. **Window history read from the input arrays** when no filter precedes the
   window (`window_direct_input`). Before this, every row's `ts` and aggregate
   inputs were copied into scratch history buffers.
   `bench/results/opt1-window-direct-10M.txt`, 10M rows, 3 alternating runs:
   JIT median **249.9 / 285.2 / 330.2 ms → 209.4 / 221.7 / 231.2 ms**.
   JIT/hand **1.17–1.40 → 0.95–0.96**.
2. **Window history kept in the output columns** when no filter follows the
   window (`window_output_history`). Every row that reaches the window is then
   output row k, so pass-through columns are stored to the output early and
   read back as history. `bench/results/opt2-window-output-history-10M.txt`,
   `combined/&`, 10M rows, 3 alternating runs: JIT median
   **20.9 / 20.9 / 20.9 ms → 18.9 / 18.5 / 18.6 ms**. JIT/hand
   **1.12 → 0.99**.

Both are covered by the JIT-vs-interpreter tests in on/off combinations, and
by the random differential tests, which run every query on the JIT with
default options, with both window optimizations off, and at `-O0`.

## End-to-end from Python vs pandas (10M rows)

```
.venv/bin/python bench/bench_python.py --rows 10000000 --reps 3   # bench/results/python-vs-pandas-10M.txt
```

These times include the Python call, input validation (ts-sorted and
sym-range scans) and fresh output allocation. They do not include JIT
compilation. pandas 3.0.6 and numpy 2.5.4.

| query | tsq JIT ms | tsq interp ms | pandas ms | pandas / JIT |
|---|---:|---:|---:|---:|
| filter | 14.4 | 201.3 | 37.3 | 2.6× |
| window (sum, mean, min, max over 5m) | 252.3 | 334.5 | 1021.4 | 4.0× |
| asof | 104.1 | 103.5 | 804.2 | 7.7× |
| combined | 39.1 | 199.6 | 291.3 | 7.4× |

(For asof the JIT and the interpreter are equal through Python. Fixed costs
dominate: validation scans both tables, and six 80 MB output columns are
allocated and page-faulted.)

## Correctness evidence from the benchmark runs

Every benchmark run printed `all outputs match: yes`. The interpreter, the
JIT and the hand-written code produced identical output hashes for every
query at 10M and at 50M rows.

## Run history (including the bad runs)

| file | notes |
|---|---|
| `50M-contended-run1.txt` | First 50M attempt. Load average 7.6 → 22, swap +1.4 GB. The interpreter ran in the same process. Numbers distorted (e.g. JIT window median 4.8 s). |
| `50M-contended-run2-mixed.txt` | Same, load 6.7 → 11. Window 2.4–3.1 s. |
| `50M-native-run1.txt`, `50M-interp-run1.txt` | First run with a separate interpreter process. Still memory-pressured (swap +1.1 GB). Before optimization 2. |
| `10M-*-pre-opt2.txt` | 10M runs before optimization 2 and before the `&` variants existed. |
| `50M-native-final.txt`, `50M-native-window-rerun.txt`, `50M-interp-final.txt` | **Headline numbers** (load ≈ 3–6). |
| `10M-native-final.txt`, `10M-interp-final.txt` | **10M reference** with the final code. |
| `50M-confirm-native-a.txt`, `50M-confirm-native-b.txt`, `50M-confirm-interp.txt` | Confirmation run, started automatically once load dropped below 3.5. Load climbed back to 8–9 during the run. The filter queries reproduce the headline numbers within 1–5% (filter JIT 42.4 vs 41.9 ms, filter_wide 87.3 vs 85.4, filter_project 221.7 vs 212.5; JIT/hand 0.41 / 0.90 / 0.91 / 0.99). Window, asof and combined were distorted by the load (e.g. asof JIT median 1816 ms vs 307 ms in the headline run, and the interpreter's filter at 1634 vs 860 ms). JIT/hand stayed between 0.62 and 1.15 for every query, so the within-20% result held apart from the noisy window median (min-based ratio 0.90). Not used for the headline table. |

To reproduce on a quiet machine, run the three 50M commands above. Expect
less variance than recorded here.

---

## Raw output


### bench/results/50M-native-final.txt

```
# date: 2026-10-19T06:08:42Z
# host: Apple M3, 8 cpus, 16 GB
# os: Darwin 24.6.0; llvm: 23.1.2
# load averages before run: 3.32 7.29 9.61
# command: build/bench_queries --rows 50000000 --reps 7 --no-interp
# tsq benchmark: rows=50000000 reps=7 window_direct_input=1 window_output_history=1 mode=native-only
# generated trades+quotes in 583 ms
# times in ms; med = median, min = minimum over reps; ratios use medians (and mins)
query             out_rows     interp   jit_med   jit_min  hand_med  hand_min  jit_cold  int/jit jit/hand    (min)  c_parse  c_irgen    c_opt     c_mc  c_total  match
filter             3161066        0.0      41.9      41.3     101.5     100.7      80.9     0.00    0.413    0.410    0.004    0.022     0.76     2.31     3.21    yes
#raw filter jit_ms=42.1782,42.0703,41.8892,42.3059,41.2722,41.3153,41.2956, hand_ms=101.544,102.164,103.056,100.742,100.728,100.873,102.02, jit_cold_ms=80.8599 interp_ms= compile_total_ms=5.97971,3.47242,3.20883,3.12858,3.12263,
filter/&           3161066        0.0      42.1      41.3      47.1      46.0      46.5     0.00    0.895    0.898    0.004    0.026     0.76     2.35     3.26    yes
#raw filter/& jit_ms=41.2905,41.7278,42.9507,42.3707,42.1123,42.8369,41.3156, hand_ms=45.9823,49.0539,47.0604,46.6905,47.7722,47.3238,45.9738, jit_cold_ms=46.5471 interp_ms= compile_total_ms=4.33492,3.5065,3.25621,3.19188,3.17321,
filter_wide       40660508        0.0      85.4      82.8      94.9      91.7     147.0     0.00    0.899    0.902    0.005    0.021     0.68     1.25     2.06    yes
#raw filter_wide jit_ms=85.5278,83.1461,82.7807,82.7823,85.3587,85.7638,85.8822, hand_ms=97.299,91.737,91.8153,92.654,94.8962,95.2023,95.4835, jit_cold_ms=146.964 interp_ms= compile_total_ms=3.03783,2.33833,2.06212,1.96154,1.95167,
filter_project    20289968        0.0     212.5     212.5     216.7     215.1     251.8     0.00    0.981    0.988    0.006    0.020     0.65     1.24     2.01    yes
#raw filter_project jit_ms=212.545,216.867,212.54,212.669,212.537,212.477,212.464, hand_ms=215.186,218.882,217.218,216.713,215.088,216.525,216.975, jit_cold_ms=251.818 interp_ms= compile_total_ms=2.92554,2.22375,2.00554,1.98196,1.95687,
window            50000000        0.0    1983.1    1077.5    2084.5    1025.2    1139.3     0.00    0.951    1.051    0.004    0.046     2.11     3.51     5.78    yes
#raw window jit_ms=1269.88,1834.09,1983.14,2117.99,2172.44,2056.02,1077.52, hand_ms=1314.14,1844.75,2327.78,2084.45,2141.31,2110.21,1025.15, jit_cold_ms=1139.34 interp_ms= compile_total_ms=6.84004,5.94437,5.78437,5.70592,5.71483,
asof              50000000        0.0     306.8     303.6     310.5     301.9     557.7     0.00    0.988    1.006    0.003    0.034     1.25     3.06     4.69    yes
#raw asof jit_ms=303.591,314.419,305.015,303.843,306.814,308.827,309.849, hand_ms=328.08,310.504,308.417,308.601,301.928,312.244,312.672, jit_cold_ms=557.719 interp_ms= compile_total_ms=5.35742,4.69508,4.68825,4.38075,4.28221,
combined           3161066        0.0      97.6      95.0     159.8     153.5     106.1     0.00    0.611    0.619    0.007    0.046     1.68     3.44     5.28    yes
#raw combined jit_ms=94.9542,97.5902,124.972,96.9784,98.2465,97.3951,97.978, hand_ms=154.896,153.491,162.676,155.171,160.392,160.478,159.798, jit_cold_ms=106.065 interp_ms= compile_total_ms=6.47596,5.51342,5.27958,5.23471,5.19167,
combined/&         3161066        0.0      95.8      93.1      97.4      95.0     116.9     0.00    0.984    0.979    0.007    0.042     1.80     3.57     5.56    yes
#raw combined/& jit_ms=95.6348,95.9824,93.0541,95.9185,94.6912,115.505,95.8234, hand_ms=97.2724,97.4683,96.8881,97.374,95.0255,104.807,98.265, jit_cold_ms=116.948 interp_ms= compile_total_ms=6.85246,5.55946,5.63996,5.20104,5.46883,
# peak RSS 5196 MB; all outputs match: yes
# load averages after run: 4.11 6.85 9.31
```

### bench/results/50M-native-window-rerun.txt

```
# date: 2026-10-19T06:09:42Z
# host: Apple M3, 8 cpus, 16 GB
# os: Darwin 24.6.0; llvm: 23.1.2
# load averages before run: 4.02 6.79 9.27
# command: build/bench_queries --rows 50000000 --reps 9 --no-interp --only window
# tsq benchmark: rows=50000000 reps=9 window_direct_input=1 window_output_history=1 mode=native-only
# generated trades+quotes in 293 ms
# times in ms; med = median, min = minimum over reps; ratios use medians (and mins)
query             out_rows     interp   jit_med   jit_min  hand_med  hand_min  jit_cold  int/jit jit/hand    (min)  c_parse  c_irgen    c_opt     c_mc  c_total  match
window            50000000        0.0     987.4     972.5    1014.2     993.8    1157.0     0.00    0.974    0.979    0.004    0.047     2.11     3.51     5.80    yes
#raw window jit_ms=988.581,987.382,979.722,991.303,982.734,983.469,972.473,993.624,1040.92, hand_ms=1042.74,1010.85,1023.72,993.815,1014.16,1009.22,1008.51,1021.94,1065.68, jit_cold_ms=1156.95 interp_ms= compile_total_ms=8.04642,6.10042,5.79688,5.78042,5.72854,
# peak RSS 4993 MB; all outputs match: yes
# load averages after run: 3.85 6.53 9.10
```

### bench/results/50M-interp-final.txt

```
# date: 2026-10-19T06:10:07Z
# host: Apple M3, 8 cpus, 16 GB
# os: Darwin 24.6.0; llvm: 23.1.2
# load averages before run: 3.85 6.53 9.10
# command: build/bench_queries --rows 50000000 --reps 5 --interp-only
# tsq benchmark: rows=50000000 reps=5 window_direct_input=1 window_output_history=1 mode=interp-only
# generated trades+quotes in 573 ms
# times in ms; med = median, min = minimum over reps; ratios use medians (and mins)
query             out_rows     interp   jit_med   jit_min  hand_med  hand_min  jit_cold  int/jit jit/hand    (min)  c_parse  c_irgen    c_opt     c_mc  c_total  match
filter             3161066      859.6       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.007    0.028     0.92     2.67     3.76    yes
#raw filter jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=859.603,849.721,911.236, compile_total_ms=6.9755,3.91858,3.763,3.52917,3.41133,
filter/&           3161066      862.0       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.010    0.048     1.87     3.52     6.12    yes
#raw filter/& jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=906.821,858.175,862.034, compile_total_ms=7.61017,12.1327,6.11813,5.14392,4.86412,
filter_wide       40660508     1011.3       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.005    0.020     0.69     1.39     2.22    yes
#raw filter_wide jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=1154.29,1011.29,899.107, compile_total_ms=3.13379,2.37475,2.21529,2.08921,2.00733,
filter_project    20289968     1434.0       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.007    0.024     0.69     1.30     2.14    yes
#raw filter_project jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=1400.49,1451.73,1434.04, compile_total_ms=3.11842,2.28304,2.1355,2.01063,2.02296,
window            50000000     1935.1       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.004    0.049     2.15     3.54     5.86    yes
#raw window jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=1935.08,2064.16,1801.89, compile_total_ms=6.93392,6.06775,5.86029,5.76579,5.7315,
asof              50000000      508.2       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.003    0.031     1.24     2.91     4.30    yes
#raw asof jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=568.183,508.213,498.676, compile_total_ms=5.447,4.46679,4.30279,4.22429,4.22458,
combined           3161066      946.0       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.008    0.049     1.80     3.64     5.61    yes
#raw combined jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=1003.21,922.678,946.028, compile_total_ms=6.61117,5.78988,5.61408,5.56817,5.51233,
combined/&         3161066      956.0       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.009    0.049     1.83     3.76     5.72    yes
#raw combined/& jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=1002.69,955.957,933.421, compile_total_ms=6.34575,5.60775,5.7095,5.71638,5.99033,
# peak RSS 4924 MB; all outputs match: yes
# load averages after run: 3.73 6.16 8.85
```

### bench/results/10M-native-final.txt

```
# date: 2026-10-19T06:13:52Z
# host: Apple M3, 8 cpus, 16 GB
# os: Darwin 24.6.0; llvm: 23.1.2
# load averages before run: 4.87 6.55 8.56
# command: build-bench/bench_queries --rows 10000000 --reps 9 --no-interp
# tsq benchmark: rows=10000000 reps=9 window_direct_input=1 window_output_history=1 mode=native-only
# generated trades+quotes in 115 ms
# times in ms; med = median, min = minimum over reps; ratios use medians (and mins)
query             out_rows     interp   jit_med   jit_min  hand_med  hand_min  jit_cold  int/jit jit/hand    (min)  c_parse  c_irgen    c_opt     c_mc  c_total  match
filter              639232        0.0       8.3       8.2      20.1      20.1       9.5     0.00    0.412    0.409    0.004    0.024     0.76     2.34     3.24    yes
#raw filter jit_ms=8.22592,8.39329,8.37792,8.27996,8.21179,8.31567,8.28988,8.31192,8.28171, hand_ms=20.1393,20.1744,20.1482,20.0585,20.1289,20.1187,20.0932,20.1683,20.0578, jit_cold_ms=9.48188 interp_ms= compile_total_ms=5.34329,3.48862,3.24192,3.21663,3.16692,
filter/&            639232        0.0       8.1       8.1       9.1       9.0       9.1     0.00    0.896    0.898    0.004    0.022     0.75     2.33     3.22    yes
#raw filter/& jit_ms=8.23713,8.13921,8.09238,8.11283,8.15842,8.13742,8.17104,8.14383,8.22304, hand_ms=9.10871,9.02321,9.018,9.08854,9.07121,9.08704,9.14217,9.00654,9.1985, jit_cold_ms=9.13612 interp_ms= compile_total_ms=4.20508,3.46117,3.21758,3.16521,3.11829,
filter_wide        8140733        0.0      16.6      16.5      18.7      18.2      23.5     0.00    0.886    0.904    0.005    0.022     0.67     1.25     2.06    yes
#raw filter_wide jit_ms=16.4624,16.557,16.479,16.4603,16.4701,16.9994,17.2525,17.2529,16.9754, hand_ms=18.2025,18.4242,18.2248,18.3612,18.6918,19.1228,19.049,19.1744,19.1447, jit_cold_ms=23.5353 interp_ms= compile_total_ms=2.69413,2.19992,2.06183,2.00263,1.98625,
filter_project     4062453        0.0      42.4      42.3      43.1      42.8      44.8     0.00    0.984    0.989    0.006    0.020     0.64     1.20     1.97    yes
#raw filter_project jit_ms=42.4027,42.363,42.4155,42.9739,42.3717,42.3426,42.3286,42.3927,42.307, hand_ms=42.7957,43.0437,43.3585,42.9817,43.0657,43.1202,43.2869,43.2345,42.9491, jit_cold_ms=44.8266 interp_ms= compile_total_ms=2.64517,2.02479,1.97104,1.86738,1.86933,
window            10000000        0.0     189.3     188.9     193.8     191.2     218.2     0.00    0.977    0.988    0.004    0.046     2.12     3.50     5.78    yes
#raw window jit_ms=189.19,189.082,188.897,189.31,189.187,189.679,189.952,192.522,189.977, hand_ms=191.543,193.633,191.152,192.004,196.551,193.898,193.972,198.764,193.835, jit_cold_ms=218.156 interp_ms= compile_total_ms=6.67575,5.87804,5.78379,5.72992,5.73346,
asof              10000000        0.0      58.0      57.7      58.7      57.9      79.9     0.00    0.990    0.997    0.004    0.039     1.26     2.99     4.42    yes
#raw asof jit_ms=57.8165,57.6757,57.9007,59.522,59.8768,59.0585,58.3958,58.046,57.9779, hand_ms=58.8673,58.0027,58.6507,60.7339,60.4591,60.0854,57.8526,58.1242,58.1208, jit_cold_ms=79.8617 interp_ms= compile_total_ms=5.19833,4.65946,4.42071,4.19692,4.06117,
combined            639232        0.0      18.6      18.4      30.1      29.8      20.3     0.00    0.616    0.618    0.009    0.048     1.78     3.58     5.51    yes
#raw combined jit_ms=18.443,18.4972,18.6042,18.9473,18.5021,18.526,18.6015,18.554,19.188, hand_ms=29.8206,29.9875,30.1212,30.1505,29.9127,30.8036,30.0973,30.0334,31.7674, jit_cold_ms=20.3082 interp_ms= compile_total_ms=6.28488,5.50925,5.41817,5.64629,5.49808,
combined/&          639232        0.0      19.0      18.8      19.0      18.5      20.8     0.00    1.002    1.017    0.010    0.053     1.89     3.66     5.75    yes
#raw combined/& jit_ms=19.0246,18.9729,18.9925,19.0303,19.0076,19.6835,19.4877,19.5834,18.8223, hand_ms=18.954,19.0506,18.8306,18.8573,19.0652,19.4347,19.9122,18.989,18.5166, jit_cold_ms=20.7772 interp_ms= compile_total_ms=6.86767,6.17104,5.75138,5.63162,5.51971,
# peak RSS 1460 MB; all outputs match: yes
# load averages after run: 4.58 6.43 8.50
```

### bench/results/10M-interp-final.txt

```
# date: 2026-10-19T06:14:01Z
# host: Apple M3, 8 cpus, 16 GB
# os: Darwin 24.6.0; llvm: 23.1.2
# load averages before run: 4.58 6.43 8.50
# command: build-bench/bench_queries --rows 10000000 --reps 5 --interp-only
# tsq benchmark: rows=10000000 reps=5 window_direct_input=1 window_output_history=1 mode=interp-only
# generated trades+quotes in 112 ms
# times in ms; med = median, min = minimum over reps; ratios use medians (and mins)
query             out_rows     interp   jit_med   jit_min  hand_med  hand_min  jit_cold  int/jit jit/hand    (min)  c_parse  c_irgen    c_opt     c_mc  c_total  match
filter              639232      167.9       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.004    0.023     0.77     2.37     3.28    yes
#raw filter jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=167.922,168.075,167.891, compile_total_ms=5.21258,3.50742,3.27921,3.24667,3.19892,
filter/&            639232      166.1       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.004    0.021     0.71     2.35     3.19    yes
#raw filter/& jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=166.017,166.526,166.118, compile_total_ms=3.83746,3.38083,3.19196,3.16783,3.14092,
filter_wide        8140733      165.5       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.004    0.020     0.66     1.30     2.10    yes
#raw filter_wide jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=172.657,165.506,164.331, compile_total_ms=3.03775,2.37183,2.09842,2.00046,2.05029,
filter_project     4062453      275.7       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.007    0.024     0.71     1.33     2.19    yes
#raw filter_project jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=275.903,272.598,275.695, compile_total_ms=2.94004,2.30242,2.19429,2.11925,2.07567,
window            10000000      325.0       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.005    0.049     2.24     3.78     6.26    yes
#raw window jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=346.977,324.984,322.934, compile_total_ms=7.05842,6.33413,6.26392,5.85079,5.95704,
asof              10000000       86.9       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.004    0.038     1.25     2.92     4.34    yes
#raw asof jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=86.1058,87.6351,86.8756, compile_total_ms=5.28937,4.51704,4.33579,4.23758,4.2455,
combined            639232      183.6       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.008    0.043     1.68     3.40     5.24    yes
#raw combined jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=186.986,182.741,183.62, compile_total_ms=5.838,5.36154,5.2435,5.17746,5.18954,
combined/&          639232      182.3       0.0       0.0       0.0       0.0       0.0      inf      nan      nan    0.011    0.056     1.92     4.09     6.22    yes
#raw combined/& jit_ms=0, hand_ms=0, jit_cold_ms=0 interp_ms=184.001,182.296,182.01, compile_total_ms=6.71279,6.42154,6.22333,5.80817,5.54138,
# peak RSS 1659 MB; all outputs match: yes
# load averages after run: 4.62 6.41 8.48
```

### bench/results/opt1-window-direct-10M.txt

```
# Optimization 1: window reads history from input arrays (window_direct_input)
# host: Apple M3, 8 cpus (4P+4E), 16 GB; macOS 15.7 (Darwin 24.6.0); LLVM 23.1.2
# NOTE: machine was shared with other heavy processes (load average 8-17 during the run).
# command (alternating 3x): build/bench_queries --rows 10000000 --reps 7 --only window --no-interp --window-direct {0,1}
# load averages: 8.61 15.72 11.21 (before)  7.94 14.99 11.07 (after)
query             out_rows     interp   jit_med   jit_min  hand_med  hand_min  jit_cold  int/jit jit/hand    (min)  c_parse  c_irgen    c_opt     c_mc  c_total  match
# window_direct_input=0
window            10000000        0.0     249.9     248.0     213.2     210.4     304.3     0.00    1.172    1.178    0.006    0.055     2.54     4.10     6.78    yes
# window_direct_input=1
window            10000000        0.0     209.4     204.1     219.7     210.6     242.5     0.00    0.953    0.969    0.006    0.056     2.60     4.06     6.87    yes
# window_direct_input=0
window            10000000        0.0     285.2     284.1     223.3     220.7     335.7     0.00    1.277    1.287    0.007    0.056     2.66     4.47     7.36    yes
# window_direct_input=1
window            10000000        0.0     221.7     221.2     230.4     228.8     257.8     0.00    0.962    0.967    0.005    0.059     2.61     4.19     7.02    yes
# window_direct_input=0
window            10000000        0.0     330.2     298.7     236.4     234.8     353.2     0.00    1.397    1.272    0.006    0.055     2.59     4.33     7.05    yes
# window_direct_input=1
window            10000000        0.0     231.2     218.6     242.2     228.6     257.7     0.00    0.954    0.956    0.007    0.054     2.43     4.08     6.70    yes
```

### bench/results/opt2-window-output-history-10M.txt

```
# Optimization 2: window history stored in (and read back from) output columns when no filter follows
# host: Apple M3, 8 cpus (4P+4E), 16 GB; macOS 15.7 (Darwin 24.6.0); LLVM 23.1.2
# command (alternating 3x): build/bench_queries --rows 10000000 --reps 7 --only combined/& --no-interp --window-outhist {0,1}
# load averages: 3.70 7.56 9.75 (before and after)
# window_output_history=0
combined/&          639232        0.0      20.9      20.8      18.7      18.6      24.2     0.00    1.117    1.115    0.009    0.045     1.71     3.53     5.41    yes
# window_output_history=1
combined/&          639232        0.0      18.9      18.7      18.9      18.7      24.9     0.00    1.003    0.999    0.008    0.043     1.66     3.43     5.27    yes
# window_output_history=0
combined/&          639232        0.0      20.9      20.8      18.7      18.6      23.6     0.00    1.117    1.119    0.008    0.044     1.74     3.47     5.37    yes
# window_output_history=1
combined/&          639232        0.0      18.5      18.4      18.7      18.6      23.7     0.00    0.985    0.986    0.008    0.042     1.70     3.45     5.32    yes
# window_output_history=0
combined/&          639232        0.0      20.9      20.9      18.7      18.5      26.1     0.00    1.116    1.127    0.008    0.044     1.70     3.48     5.35    yes
# window_output_history=1
combined/&          639232        0.0      18.6      18.5      18.8      18.7      22.9     0.00    0.987    0.985    0.007    0.041     1.69     3.44     5.30    yes
```

### bench/results/python-vs-pandas-10M.txt

```
# host: arm64 macOS-15.7-arm64-arm-64bit-Mach-O python 3.14.3
# numpy 2.5.4 pandas 3.0.6; rows=10000000 reps=3
# datagen 0.7s
query        rows_out    jit_ms  interp_ms  pandas_ms compile_ms pandas/jit
filter         610772      14.4      201.3       37.3       4.99        2.6
#raw filter jit=[18.847333267331123, 14.196333009749651, 14.350333251059055] interp=[202.18825014308095, 182.99233308061957, 201.28545770421624] pandas=[39.291790686547756, 37.29849960654974, 25.718290824443102]
window       10000000     252.3      334.5     1021.4       8.08        4.0
#raw window jit=[261.3530829548836, 252.32991622760892, 228.93004212528467] interp=[332.6413328759372, 390.1212499476969, 334.5217499881983] pandas=[1021.3821670040488, 1027.9035000130534, 977.4781251326203]
asof         10000000     104.1      103.5      804.2       5.34        7.7
#raw asof jit=[139.65900009498, 104.05483422800899, 94.55620823428035] interp=[103.45066711306572, 104.57354178652167, 103.39120915159583] pandas=[819.3008746020496, 804.1550829075277, 798.0127497576177]
combined       610772      39.1      199.6      291.3       6.42        7.4
#raw combined jit=[40.82041699439287, 39.14175042882562, 38.012292236089706] interp=[199.62866697460413, 200.58891596272588, 198.95037496462464] pandas=[291.25762498006225, 289.1074582003057, 293.5026250779629]
```
