# Benchmark results

Apple M3 Max: **12 performance + 4 efficiency cores, 128 GiB RAM**.
OS: macOS 27.0.1 (26A434), Darwin 27.0.0. Power: AC power, powermode 0 (automatic), battery charged.
Start load/context: 3.03, 3.87, 3.92 (1, 5, 15 minutes); user stopped competing workloads before start. Background OS activity remains.
Thermal observations at start: No thermal or performance warning recorded by pmset; at end: Note: No thermal warning level has been recorded; Note: No performance warning level has been recorded; Note: No CPU power status has been recorded. These results describe this machine. FreeBSD measurements
are excluded from these tables.

Scale 1 uses **10 repetitions**; scales 2, 4 and 8 use **3**. The 31 workloads
include three fixed Graspan datasets at scale 1. The other 28 workloads run
at all scales. Scaling follows each generator: tree depth increases with
scale, so tree size grows exponentially. All accepted answers are checked
as exact sets against relic before timing. Verification and compilation
are excluded from execution times.

Times include process startup, input loading, evaluation and output writing.
relic, SWI, clingo, DuckDB and SQLite write to discarded standard output;
Souffle and Nemo export files. Large-answer comparisons include this I/O
difference. User CPU time sums across threads; it can exceed wall time.
Small differences need to be read alongside the recorded timing spread.

## Comparison overview

| Scale | Threads | Repetitions | relic lowest median / comparable workloads | Rival cells without a completed time |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 10 | 22 / 31 | 14 |
| 1 | 16 | 10 | 26 / 31 | 4 |
| 2 | 1 | 3 | 18 / 28 | 13 |
| 2 | 16 | 3 | 23 / 28 | 2 |
| 4 | 1 | 3 | 14 / 28 | 24 |
| 4 | 16 | 3 | 21 / 28 | 5 |
| 8 | 1 | 3 | 13 / 27 | 56 |
| 8 | 16 | 3 | 19 / 28 | 11 |

## Method and limits

relic 1.0.0 uses Apple clang 21 with `-O3 -g`, `--disable-shared
--enable-threads`. Rivals are Souffle 2.5, Nemo 0.10.1, SWI-Prolog 9.2.9,
clingo 5.8.0, DuckDB 1.5.5 and SQLite 3.51.2; hyperfine is 2.0.0.
Generated Souffle programs use Homebrew clang 23.1.3 and `-O3`, with OpenMP
for parallel runs. The parallel Souffle build uses the
[documented ARM locking patch](bench/compare/tools/README.md); it differs
from the serial build, so its speedup cannot be attributed solely to cores.

`timeout` is a verification that exceeds 300 seconds. `n/a` means no program
for that engine. `no-reference` means relic did not complete verification;
rivals are unmeasured. `engine-limit` diagnoses clingo's internal ID limit.
`resource-limit` diagnoses memory exhaustion after a 120 GiB DuckDB retry.
None of these statuses is a successful timing or an invented 300-second
measurement. Bold marks the lowest observed wall median, without asserting
statistical significance. The summary compares only workloads with a
successful relic time and at least one successful rival time.

SWI's scale-2 `cspa` retry raises table space from 16 to 64 GiB and times out.
DuckDB's graph-left retries raise memory from 102.3 to 120 GiB and time out.
Its scale-8 bounded-tree retries fail while holding 114.1/114.2 GiB and
requesting another 32 GiB, beyond this machine's physical RAM. Raw failures
and retry outcomes are retained separately. The interrupted scale-8 serial
graph-left verification is replaced by a complete fresh attempt, which
times out. The scale-4 serial report's missing shell epilogue is documented
in environment metadata (relic-bench `m3-max/environment.json`);
its cell matrix and samples are independently validated.

The huge scale-8 tree verification uses the
[exact parallel bitmap verifier](bench/compare/tools/README.md#exact-tree-answer-verification).
It preserves set equality and duplicates semantics; its speed is excluded
from engine timings. The recovery retains the original 130 completed
parallel cells and records the remaining tree workloads separately.

## Full Mac comparisons

Souffle is the compiled mode; SF Magic is compiled with Magic Sets; SF interp is the interpreter. Each cell has explicit time or memory units. `us` denotes microseconds.

### Scale 1, 1 thread

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **80.4 ms** | 120 ms | timeout | 186 ms | 508 ms | timeout | 294 ms | n/a | n/a |
| `chain-closure` | **245 ms** | 492 ms | 655 ms | 711 ms | 70.2 s | 3.28 s | 1.43 s | 574 ms | 6.28 s |
| `chain-left` | 2.43 ms | 367 ms | **2.38 ms** | 524 ms | 70.8 s | 23 ms | 880 ms | 320 ms | 5.95 s |
| `chain-right` | **253 ms** | 369 ms | 451 ms | 534 ms | 63.6 s | 294 ms | 866 ms | 333 ms | 5.93 s |
| `cspa` | **128 ms** | 275 ms | 157 s | 438 ms | 800 ms | timeout | 512 ms | n/a | n/a |
| `cspa-httpd` | **71.1 s** | 75 s | timeout | 114 s | 274 s | timeout | engine-limit | n/a | n/a |
| `cspa-linux` | **19.8 s** | 28.2 s | timeout | 40.9 s | 55.4 s | timeout | 73.1 s | n/a | n/a |
| `cspa-postgresql` | **70.3 s** | 90.9 s | timeout | 133 s | 236 s | timeout | engine-limit | n/a | n/a |
| `degree-count` | **35.7 ms** | 44.1 ms | 44.2 ms | 79.8 ms | 170 ms | 686 ms | 352 ms | 46.1 ms | 165 ms |
| `dist-bounded` | **251 ms** | 579 ms | 642 ms | 806 ms | 1.64 s | 3.53 s | 1.42 s | 378 ms | 1.87 s |
| `facts-join` | 92.3 ms | 110 ms | 110 ms | 150 ms | 447 ms | 1.88 s | 988 ms | **75.2 ms** | 171 ms |
| `files-grouped` | 149 ms | 519 ms | 942 ms | 1.09 s | 1.01 s | 1.92 s | n/a | **67.3 ms** | 301 ms |
| `graph-left` | **29.7 ms** | 239 s | 33.8 ms | 290 s | timeout | 591 ms | engine-limit | 138 s | timeout |
| `graph-reach` | **29.1 ms** | 31.1 ms | 32 ms | 61.4 ms | 101 ms | 587 ms | 263 ms | 57.3 ms | 148 ms |
| `grid-reach` | 15.7 ms | **12.9 ms** | 13.4 ms | 40.3 ms | 109 ms | 285 ms | 126 ms | 233 ms | 67.3 ms |
| `sg-acyclic` | **253 ms** | 502 ms | 697 ms | 597 ms | 1.75 s | 1.95 s | 1.38 s | 258 ms | 2.15 s |
| `sg-acyclic-bf` | **2.47 ms** | 459 ms | 2.96 ms | 551 ms | 1.68 s | 35.2 ms | 1.18 s | 196 ms | 2.08 s |
| `sg-cyclic` | **826 ms** | 1.78 s | 2.83 s | 2.03 s | 7.9 s | 6.95 s | 6.48 s | 841 ms | 8.26 s |
| `startup` | **1.38 ms** | 1.74 ms | 1.53 ms | 25.2 ms | 3.5 ms | 8.18 ms | 2.09 ms | 12.1 ms | 2.27 ms |
| `tasks-ready` | **42.3 ms** | 69.3 ms | 76.7 ms | 120 ms | 145 ms | 901 ms | 392 ms | 73.4 ms | 49.4 s |
| `tc-acyclic` | 392 ms | **324 ms** | 721 ms | 570 ms | 3.7 s | 2.88 s | 2.43 s | 356 ms | 5.72 s |
| `tc-acyclic-bf` | 47.2 ms | 288 ms | **42.3 ms** | 524 ms | 3.61 s | 338 ms | 2.28 s | 294 ms | 5.62 s |
| `tc-acyclic-fb` | **8.49 ms** | 769 ms | 17.5 ms | 1.08 s | 3.66 s | 284 ms | 2.3 s | 294 ms | 5.62 s |
| `tc-cyclic` | 1.35 s | **864 ms** | 2.26 s | 1.65 s | 18.3 s | 16.5 s | 9.47 s | 1.29 s | 28.9 s |
| `tc-cyclic-bf` | 1.24 s | 789 ms | **788 ms** | 1.56 s | 18 s | 11.7 s | 9.37 s | 1.16 s | 28.7 s |
| `tc-cyclic-fb` | **9.7 ms** | 2.57 s | 18.7 ms | 3.6 s | 18.1 s | 289 ms | 9.24 s | 1.19 s | 28.6 s |
| `tc-nonlinear` | **92.8 ms** | 145 ms | 236 ms | 225 ms | 616 ms | 612 ms | 376 ms | n/a | n/a |
| `tc-sparse-acyclic` | **24.9 ms** | 36.3 ms | 41.7 ms | 70 ms | 101 ms | 277 ms | 120 ms | 45 ms | 199 ms |
| `tc-sparse-cyclic` | **231 ms** | 267 ms | 410 ms | 419 ms | 1.19 s | 3.03 s | 1.41 s | 286 ms | 4.09 s |
| `tree-sg-all` | **23.6 ms** | 24.5 ms | 23.7 ms | 63.8 ms | 128 ms | 286 ms | 108 ms | 50.4 ms | 217 ms |
| `tree-sg-bound` | 4.22 ms | 750 ms | **3.36 ms** | 1.56 s | 4.42 s | 54.4 ms | 5.65 s | 1.26 s | 14.9 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 78.7 ms | 117 ms | timeout | 170 ms | 502 ms | timeout | 290 ms | n/a | n/a |
| `chain-closure` | 241 ms | 473 ms | 633 ms | 679 ms | 70.2 s | 1.64 s | 1.41 s | 547 ms | 3.37 s |
| `chain-left` | 1.52 ms | 363 ms | 1.28 ms | 506 ms | 70.7 s | 17.9 ms | 858 ms | 307 ms | 3.07 s |
| `chain-right` | 248 ms | 366 ms | 447 ms | 517 ms | 64 s | 269 ms | 847 ms | 319 ms | 3.05 s |
| `cspa` | 127 ms | 272 ms | 156 s | 422 ms | 793 ms | timeout | 504 ms | n/a | n/a |
| `cspa-httpd` | 70.2 s | 74.3 s | timeout | 114 s | 272 s | timeout | engine-limit | n/a | n/a |
| `cspa-linux` | 19.5 s | 28 s | timeout | 40.6 s | 54.9 s | timeout | 72.5 s | n/a | n/a |
| `cspa-postgresql` | 69.7 s | 90.2 s | timeout | 132 s | 234 s | timeout | engine-limit | n/a | n/a |
| `degree-count` | 34 ms | 42.1 ms | 42.1 ms | 65.3 ms | 167 ms | 608 ms | 346 ms | 41.8 ms | 159 ms |
| `dist-bounded` | 247 ms | 565 ms | 626 ms | 779 ms | 1.62 s | 2.16 s | 1.4 s | 365 ms | 1.76 s |
| `facts-join` | 89 ms | 104 ms | 105 ms | 131 ms | 439 ms | 1.75 s | 977 ms | 70.3 ms | 168 ms |
| `files-grouped` | 145 ms | 513 ms | 931 ms | 1.07 s | 1.01 s | 1.78 s | n/a | 62.5 ms | 287 ms |
| `graph-left` | 28 ms | 238 s | 31.9 ms | 288 s | timeout | 532 ms | engine-limit | 134 s | timeout |
| `graph-reach` | 27.4 ms | 29.5 ms | 30.4 ms | 48.6 ms | 98.6 ms | 530 ms | 261 ms | 53.8 ms | 141 ms |
| `grid-reach` | 14.5 ms | 11.6 ms | 12 ms | 27.6 ms | 106 ms | 246 ms | 123 ms | 239 ms | 65.9 ms |
| `sg-acyclic` | 250 ms | 497 ms | 692 ms | 579 ms | 1.73 s | 1.53 s | 1.47 s | 249 ms | 2.13 s |
| `sg-acyclic-bf` | 1.63 ms | 457 ms | 1.91 ms | 538 ms | 1.67 s | 30.6 ms | 1.17 s | 188 ms | 2.07 s |
| `sg-cyclic` | 821 ms | 1.77 s | 2.82 s | 2.01 s | 7.84 s | 6.16 s | 6.45 s | 817 ms | 7.87 s |
| `startup` | 562 us | 699 us | 595 us | 12 ms | 2 ms | 5.78 ms | 1.1 ms | 9.23 ms | 1.26 ms |
| `tasks-ready` | 40.7 ms | 67.2 ms | 74.5 ms | 107 ms | 142 ms | 820 ms | 387 ms | 69.1 ms | 49.4 s |
| `tc-acyclic` | 389 ms | 319 ms | 715 ms | 556 ms | 3.67 s | 2.47 s | 2.42 s | 350 ms | 4.79 s |
| `tc-acyclic-bf` | 45.8 ms | 285 ms | 40.7 ms | 510 ms | 3.58 s | 315 ms | 2.28 s | 286 ms | 4.72 s |
| `tc-acyclic-fb` | 7.38 ms | 767 ms | 16.2 ms | 1.06 s | 3.63 s | 261 ms | 2.3 s | 287 ms | 4.72 s |
| `tc-cyclic` | 1.34 s | 856 ms | 2.25 s | 1.63 s | 18.1 s | 15.7 s | 9.46 s | 1.27 s | 18.5 s |
| `tc-cyclic-bf` | 1.24 s | 785 ms | 785 ms | 1.54 s | 18.5 s | 11.7 s | 9.42 s | 1.15 s | 18.3 s |
| `tc-cyclic-fb` | 8.6 ms | 2.57 s | 17.6 ms | 3.58 s | 18 s | 266 ms | 9.2 s | 1.18 s | 18.3 s |
| `tc-nonlinear` | 91.1 ms | 142 ms | 233 ms | 210 ms | 610 ms | 494 ms | 372 ms | n/a | n/a |
| `tc-sparse-acyclic` | 23.6 ms | 33.8 ms | 39 ms | 56 ms | 97.6 ms | 160 ms | 117 ms | 40.6 ms | 198 ms |
| `tc-sparse-cyclic` | 229 ms | 259 ms | 402 ms | 398 ms | 1.17 s | 2.24 s | 1.4 s | 275 ms | 2.77 s |
| `tree-sg-all` | 22.4 ms | 20.9 ms | 20.5 ms | 49.1 ms | 124 ms | 142 ms | 104 ms | 45.3 ms | 213 ms |
| `tree-sg-bound` | 3.33 ms | 735 ms | 2.34 ms | 1.53 s | 4.4 s | 47.8 ms | 5.53 s | 1.18 s | 14.2 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 10.7 MiB | 7.25 MiB | timeout | 18.7 MiB | 25.6 MiB | timeout | 37.7 MiB | n/a | n/a |
| `chain-closure` | 35.6 MiB | 21.2 MiB | 56.2 MiB | 26.4 MiB | 44.3 MiB | 598 MiB | 465 MiB | 380 MiB | 5.92 MiB |
| `chain-left` | 2.53 MiB | 21.2 MiB | 2.33 MiB | 26.5 MiB | 57.8 MiB | 11 MiB | 345 MiB | 194 MiB | 5.91 MiB |
| `chain-right` | 45.4 MiB | 21.3 MiB | 21.2 MiB | 26.5 MiB | 44.1 MiB | 274 MiB | 349 MiB | 193 MiB | 5.89 MiB |
| `cspa` | 11.2 MiB | 7.3 MiB | 914 MiB | 18.7 MiB | 44.5 MiB | timeout | 56.4 MiB | n/a | n/a |
| `cspa-httpd` | 7.04 GiB | 4.12 GiB | timeout | 3.84 GiB | 12.5 GiB | timeout | engine-limit | n/a | n/a |
| `cspa-linux` | 2.6 GiB | 868 MiB | timeout | 841 MiB | 3.33 GiB | timeout | 10.6 GiB | n/a | n/a |
| `cspa-postgresql` | 7.28 GiB | 4.28 GiB | timeout | 4.44 GiB | 11.5 GiB | timeout | engine-limit | n/a | n/a |
| `degree-count` | 10.5 MiB | 6.67 MiB | 6.66 MiB | 18.7 MiB | 18.4 MiB | 39 MiB | 58 MiB | 38.9 MiB | 11.9 MiB |
| `dist-bounded` | 28.9 MiB | 50.2 MiB | 67.1 MiB | 57 MiB | 180 MiB | 367 MiB | 331 MiB | 187 MiB | 10.8 MiB |
| `facts-join` | 35.3 MiB | 36.7 MiB | 36.7 MiB | 41.4 MiB | 63.6 MiB | 113 MiB | 74.4 MiB | 42.2 MiB | 11.6 MiB |
| `files-grouped` | 30.6 MiB | 37.9 MiB | 41 MiB | 42.6 MiB | 53.8 MiB | 117 MiB | n/a | 40.7 MiB | 16.2 MiB |
| `graph-left` | 9.84 MiB | 7.93 GiB | 5.58 MiB | 7.9 GiB | timeout | 33.4 MiB | engine-limit | 35 GiB | timeout |
| `graph-reach` | 9.78 MiB | 5.25 MiB | 5.3 MiB | 18.7 MiB | 16.4 MiB | 33.4 MiB | 28.4 MiB | 32.8 MiB | 8.7 MiB |
| `grid-reach` | 6.5 MiB | 4.56 MiB | 4.64 MiB | 18.6 MiB | 17 MiB | 24.2 MiB | 16.5 MiB | 47.7 MiB | 6.27 MiB |
| `sg-acyclic` | 13.2 MiB | 13 MiB | 21 MiB | 18.7 MiB | 196 MiB | 133 MiB | 122 MiB | 124 MiB | 12.5 MiB |
| `sg-acyclic-bf` | 2.7 MiB | 12.4 MiB | 2.3 MiB | 18.7 MiB | 196 MiB | 11.4 MiB | 90.6 MiB | 108 MiB | 12.4 MiB |
| `sg-cyclic` | 19.8 MiB | 25 MiB | 44.7 MiB | 30.4 MiB | 577 MiB | 257 MiB | 207 MiB | 322 MiB | 13.3 MiB |
| `startup` | 2.06 MiB | 2.06 MiB | 2.05 MiB | 18.7 MiB | 10.5 MiB | 10 MiB | 4.02 MiB | 23.6 MiB | 3.12 MiB |
| `tasks-ready` | 11.2 MiB | 8.47 MiB | 8.59 MiB | 18.7 MiB | 18.6 MiB | 71.4 MiB | 47.3 MiB | 44.8 MiB | 11.4 MiB |
| `tc-acyclic` | 16.2 MiB | 12.4 MiB | 20.8 MiB | 18.7 MiB | 411 MiB | 142 MiB | 122 MiB | 93.2 MiB | 11.4 MiB |
| `tc-acyclic-bf` | 6.27 MiB | 12.3 MiB | 4.42 MiB | 18.7 MiB | 411 MiB | 30.2 MiB | 90.1 MiB | 75.5 MiB | 11.3 MiB |
| `tc-acyclic-fb` | 4.44 MiB | 12.6 MiB | 3.5 MiB | 18.7 MiB | 411 MiB | 23.6 MiB | 87.7 MiB | 75.5 MiB | 11.3 MiB |
| `tc-cyclic` | 21.8 MiB | 24.9 MiB | 42.8 MiB | 30.3 MiB | 1.3 GiB | 283 MiB | 207 MiB | 168 MiB | 11.5 MiB |
| `tc-cyclic-bf` | 25.3 MiB | 24.9 MiB | 24.9 MiB | 30.4 MiB | 1.3 GiB | 116 MiB | 172 MiB | 135 MiB | 11.5 MiB |
| `tc-cyclic-fb` | 4.5 MiB | 25.1 MiB | 3.47 MiB | 30.7 MiB | 1.3 GiB | 24.1 MiB | 172 MiB | 136 MiB | 11.5 MiB |
| `tc-nonlinear` | 9.2 MiB | 5.06 MiB | 7.16 MiB | 18.7 MiB | 80.9 MiB | 49.2 MiB | 40 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 6.61 MiB | 4.55 MiB | 6.41 MiB | 18.7 MiB | 17.4 MiB | 48.2 MiB | 39.2 MiB | 44.3 MiB | 6.09 MiB |
| `tc-sparse-cyclic` | 20 MiB | 21.2 MiB | 35.6 MiB | 24.8 MiB | 128 MiB | 259 MiB | 204 MiB | 148 MiB | 10.7 MiB |
| `tree-sg-all` | 6.77 MiB | 5.22 MiB | 6.19 MiB | 18.7 MiB | 16.7 MiB | 49.1 MiB | 42.6 MiB | 55.4 MiB | 8.33 MiB |
| `tree-sg-bound` | 3.55 MiB | 175 MiB | 3.02 MiB | 188 MiB | 263 MiB | 13.2 MiB | 1.58 GiB | 1.32 GiB | 10.8 MiB |

</details>

### Scale 1, 16 threads

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **33 ms** | 61.5 ms | timeout | 91.8 ms | n/a |
| `chain-closure` | **245 ms** | 898 ms | 1.01 s | 1.04 s | 690 ms |
| `chain-left` | **2.51 ms** | 791 ms | 200 ms | 899 ms | 402 ms |
| `chain-right` | **249 ms** | 773 ms | 989 ms | 871 ms | 427 ms |
| `cspa` | **32.8 ms** | 93.7 ms | 21.5 s | 142 ms | n/a |
| `cspa-httpd` | **11.3 s** | 18.6 s | timeout | 35.9 s | n/a |
| `cspa-linux` | 6.91 s | **6.35 s** | timeout | 9.18 s | n/a |
| `cspa-postgresql` | **12.5 s** | 22.3 s | timeout | 39.2 s | n/a |
| `degree-count` | **30.9 ms** | 50 ms | 49.9 ms | 81.4 ms | 50.7 ms |
| `dist-bounded` | **96.5 ms** | 290 ms | 374 ms | 414 ms | 396 ms |
| `facts-join` | 91.2 ms | 126 ms | 124 ms | 153 ms | **72.4 ms** |
| `files-grouped` | 94.4 ms | 254 ms | 338 ms | 261 ms | **52.6 ms** |
| `graph-left` | **31.6 ms** | 34.4 s | 42.6 ms | 68.2 s | 139 s |
| `graph-reach` | **26.7 ms** | 34.7 ms | 35 ms | 59.6 ms | 65 ms |
| `grid-reach` | **15.7 ms** | 52.1 ms | 53.2 ms | 77.3 ms | 253 ms |
| `sg-acyclic` | **50.7 ms** | 117 ms | 151 ms | 185 ms | 266 ms |
| `sg-acyclic-bf` | **2.64 ms** | 73.2 ms | 5.75 ms | 141 ms | 200 ms |
| `sg-cyclic` | **151 ms** | 272 ms | 406 ms | 436 ms | 847 ms |
| `startup` | **1.32 ms** | 2.87 ms | 2.83 ms | 24.3 ms | 11.8 ms |
| `tasks-ready` | **34.6 ms** | 43.4 ms | 46.3 ms | 72.5 ms | 80.9 ms |
| `tc-acyclic` | **72.5 ms** | 101 ms | 156 ms | 211 ms | 367 ms |
| `tc-acyclic-bf` | 25.4 ms | 61.3 ms | **22.7 ms** | 155 ms | 303 ms |
| `tc-acyclic-fb` | **9.41 ms** | 137 ms | 20.5 ms | 238 ms | 297 ms |
| `tc-cyclic` | **190 ms** | 205 ms | 366 ms | 521 ms | 1.28 s |
| `tc-cyclic-bf` | 255 ms | 124 ms | **121 ms** | 391 ms | 1.14 s |
| `tc-cyclic-fb` | **10.5 ms** | 352 ms | 21.3 ms | 629 ms | 1.15 s |
| `tc-nonlinear` | **22.1 ms** | 39.2 ms | 59.9 ms | 100 ms | n/a |
| `tc-sparse-acyclic` | **13.3 ms** | 32.2 ms | 36.2 ms | 63.7 ms | 50.5 ms |
| `tc-sparse-cyclic` | **64.3 ms** | 150 ms | 195 ms | 264 ms | 293 ms |
| `tree-sg-all` | **13.9 ms** | 29.2 ms | 30.5 ms | 61.1 ms | 51.5 ms |
| `tree-sg-bound` | **4.54 ms** | 353 ms | 7.46 ms | 819 ms | 1.01 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 166 ms | 244 ms | timeout | 315 ms | n/a |
| `chain-closure` | 240 ms | 2.45 s | 2.9 s | 2.76 s | 1.11 s |
| `chain-left` | 1.59 ms | 2.51 s | 98 ms | 2.83 s | 659 ms |
| `chain-right` | 244 ms | 2.38 s | 2.62 s | 2.67 s | 712 ms |
| `cspa` | 274 ms | 499 ms | 270 s | 748 ms | n/a |
| `cspa-httpd` | 135 s | 94.9 s | timeout | 157 s | n/a |
| `cspa-linux` | 28.8 s | 36 s | timeout | 61.6 s | n/a |
| `cspa-postgresql` | 130 s | 111 s | timeout | 177 s | n/a |
| `degree-count` | 54.1 ms | 53.4 ms | 53.5 ms | 77 ms | 77.4 ms |
| `dist-bounded` | 483 ms | 768 ms | 830 ms | 1.47 s | 414 ms |
| `facts-join` | 87.8 ms | 122 ms | 121 ms | 137 ms | 77.6 ms |
| `files-grouped` | 198 ms | 2.73 s | 4.04 s | 2.54 s | 66.6 ms |
| `graph-left` | 40.5 ms | 300 s | 43.4 ms | 571 s | 147 s |
| `graph-reach` | 41.5 ms | 58.5 ms | 59.2 ms | 76.9 ms | 69.8 ms |
| `grid-reach` | 14.5 ms | 130 ms | 133 ms | 147 ms | 309 ms |
| `sg-acyclic` | 406 ms | 666 ms | 1.01 s | 1.07 s | 348 ms |
| `sg-acyclic-bf` | 1.73 ms | 633 ms | 3.73 ms | 1.06 s | 261 ms |
| `sg-cyclic` | 1.55 s | 2.3 s | 3.78 s | 3.84 s | 1.51 s |
| `startup` | 556 us | 1.46 ms | 1.46 ms | 11.5 ms | 9.3 ms |
| `tasks-ready` | 75.1 ms | 133 ms | 143 ms | 162 ms | 107 ms |
| `tc-acyclic` | 633 ms | 479 ms | 1.06 s | 1.81 s | 564 ms |
| `tc-acyclic-bf` | 69.9 ms | 429 ms | 75.8 ms | 1.5 s | 531 ms |
| `tc-acyclic-fb` | 11.5 ms | 1.05 s | 23.2 ms | 2.2 s | 462 ms |
| `tc-cyclic` | 2.09 s | 1.23 s | 3.13 s | 6.17 s | 1.98 s |
| `tc-cyclic-bf` | 1.62 s | 1.14 s | 1.16 s | 4.71 s | 1.78 s |
| `tc-cyclic-fb` | 11.1 ms | 3.41 s | 23.6 ms | 8.3 s | 1.81 s |
| `tc-nonlinear` | 163 ms | 224 ms | 365 ms | 728 ms | n/a |
| `tc-sparse-acyclic` | 57.3 ms | 90.7 ms | 106 ms | 142 ms | 68 ms |
| `tc-sparse-cyclic` | 398 ms | 393 ms | 633 ms | 1.23 s | 399 ms |
| `tree-sg-all` | 54.9 ms | 80 ms | 76.6 ms | 109 ms | 64.5 ms |
| `tree-sg-bound` | 3.53 ms | 1.07 s | 5.3 ms | 2.96 s | 2.07 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 14.8 MiB | 8.81 MiB | timeout | 18.7 MiB | n/a |
| `chain-closure` | 43.6 MiB | 23.7 MiB | 60.4 MiB | 27.8 MiB | 381 MiB |
| `chain-left` | 2.55 MiB | 23.9 MiB | 3.56 MiB | 28.2 MiB | 195 MiB |
| `chain-right` | 45.4 MiB | 23.6 MiB | 23.6 MiB | 27.8 MiB | 194 MiB |
| `cspa` | 25.4 MiB | 9.42 MiB | 1.2 GiB | 18.7 MiB | n/a |
| `cspa-httpd` | 9.75 GiB | 4.35 GiB | timeout | 3.84 GiB | n/a |
| `cspa-linux` | 3.99 GiB | 929 MiB | timeout | 844 MiB | n/a |
| `cspa-postgresql` | 11.2 GiB | 4.55 GiB | timeout | 4.49 GiB | n/a |
| `degree-count` | 21.9 MiB | 8.23 MiB | 8.28 MiB | 18.7 MiB | 73.3 MiB |
| `dist-bounded` | 174 MiB | 54.4 MiB | 74.1 MiB | 58.4 MiB | 194 MiB |
| `facts-join` | 33.4 MiB | 38.3 MiB | 38.3 MiB | 42.2 MiB | 50.1 MiB |
| `files-grouped` | 77.2 MiB | 42.6 MiB | 45.4 MiB | 45.4 MiB | 47.5 MiB |
| `graph-left` | 12.1 MiB | 8.34 GiB | 6.95 MiB | 7.9 GiB | 35 GiB |
| `graph-reach` | 12.5 MiB | 6.81 MiB | 6.75 MiB | 18.7 MiB | 35.9 MiB |
| `grid-reach` | 6.52 MiB | 6.05 MiB | 6.09 MiB | 18.7 MiB | 50.5 MiB |
| `sg-acyclic` | 71.2 MiB | 15.3 MiB | 24 MiB | 18.7 MiB | 151 MiB |
| `sg-acyclic-bf` | 2.7 MiB | 14.6 MiB | 3.55 MiB | 18.7 MiB | 124 MiB |
| `sg-cyclic` | 230 MiB | 28 MiB | 49.2 MiB | 31.5 MiB | 358 MiB |
| `startup` | 2.06 MiB | 2.61 MiB | 2.66 MiB | 18.7 MiB | 24 MiB |
| `tasks-ready` | 17 MiB | 10.1 MiB | 10.4 MiB | 18.7 MiB | 57.1 MiB |
| `tc-acyclic` | 112 MiB | 14.7 MiB | 23.6 MiB | 18.7 MiB | 99.5 MiB |
| `tc-acyclic-bf` | 11 MiB | 14.5 MiB | 6.05 MiB | 18.6 MiB | 86.7 MiB |
| `tc-acyclic-fb` | 4.89 MiB | 15 MiB | 5.05 MiB | 18.7 MiB | 83 MiB |
| `tc-cyclic` | 210 MiB | 27.9 MiB | 46.7 MiB | 31.7 MiB | 175 MiB |
| `tc-cyclic-bf` | 59.2 MiB | 27.9 MiB | 27.9 MiB | 31.8 MiB | 143 MiB |
| `tc-cyclic-fb` | 5 MiB | 28.2 MiB | 5.03 MiB | 32 MiB | 143 MiB |
| `tc-nonlinear` | 28.9 MiB | 6.92 MiB | 9.36 MiB | 18.7 MiB | n/a |
| `tc-sparse-acyclic` | 14.8 MiB | 6.34 MiB | 8.39 MiB | 18.7 MiB | 51 MiB |
| `tc-sparse-cyclic` | 106 MiB | 24 MiB | 40.5 MiB | 26.1 MiB | 153 MiB |
| `tree-sg-all` | 17.3 MiB | 7.09 MiB | 8.08 MiB | 18.7 MiB | 60.7 MiB |
| `tree-sg-bound` | 3.55 MiB | 186 MiB | 4.28 MiB | 190 MiB | 1.29 GiB |

</details>

### Scale 2, 1 thread

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **143 ms** | 237 ms | timeout | 337 ms | 963 ms | timeout | 551 ms | n/a | n/a |
| `chain-closure` | **1.11 s** | 2.21 s | 2.84 s | 2.94 s | timeout | 12.7 s | 5.95 s | 2.03 s | 28.6 s |
| `chain-left` | **3.25 ms** | 1.66 s | 3.39 ms | 2.22 s | timeout | 36.5 ms | 3.94 s | 1.12 s | 27.5 s |
| `chain-right` | 1.13 s | 1.72 s | 2.04 s | 2.33 s | timeout | **1.09 s** | 3.86 s | 1.13 s | 27.3 s |
| `cspa` | **313 ms** | 628 ms | timeout | 956 ms | 1.94 s | timeout | 1.25 s | n/a | n/a |
| `degree-count` | 69.9 ms | 90 ms | 90.2 ms | 135 ms | 336 ms | 1.36 s | 712 ms | **67.5 ms** | 337 ms |
| `dist-bounded` | **718 ms** | 1.52 s | 1.7 s | 2.11 s | 4.51 s | 9.49 s | 4.08 s | 1.1 s | 5.06 s |
| `facts-join` | 180 ms | 234 ms | 236 ms | 290 ms | 970 ms | 3.71 s | 2.01 s | **125 ms** | 344 ms |
| `files-grouped` | 297 ms | 1.13 s | 1.98 s | 2.7 s | 2.02 s | 3.89 s | n/a | **118 ms** | 607 ms |
| `graph-left` | **56.6 ms** | timeout | 72.2 ms | timeout | timeout | 1.16 s | engine-limit | timeout | timeout |
| `graph-reach` | **56.7 ms** | 65.8 ms | 65.6 ms | 103 ms | 200 ms | 1.19 s | 540 ms | 89 ms | 399 ms |
| `grid-reach` | 60.3 ms | **49.5 ms** | 51.6 ms | 91 ms | 573 ms | 1.14 s | 501 ms | 1.25 s | 322 ms |
| `sg-acyclic` | **894 ms** | 1.86 s | 2.67 s | 2.19 s | 7.01 s | 6.98 s | 7.09 s | 1.07 s | 8.76 s |
| `sg-acyclic-bf` | **4.53 ms** | 1.69 s | 5.79 ms | 2.02 s | 6.71 s | 65.5 ms | 6.51 s | 827 ms | 8.49 s |
| `sg-cyclic` | **3.83 s** | 7.54 s | 12.6 s | 8.62 s | 37.6 s | 28.9 s | 36.5 s | 4.38 s | 39.5 s |
| `startup` | **1.55 ms** | 1.67 ms | 1.69 ms | 25.3 ms | 3.79 ms | 8.81 ms | 2.26 ms | 12.9 ms | 2.45 ms |
| `tasks-ready` | **93.6 ms** | 173 ms | 180 ms | 275 ms | 313 ms | 1.83 s | 805 ms | 132 ms | 193 s |
| `tc-acyclic` | 1.67 s | **1.38 s** | 3.18 s | 2.32 s | 18.3 s | 11.1 s | 13.9 s | 2 s | 45 s |
| `tc-acyclic-bf` | 169 ms | 1.23 s | **153 ms** | 2.16 s | 17.7 s | 847 ms | 13.2 s | 1.73 s | 49.3 s |
| `tc-acyclic-fb` | **17.4 ms** | 4.05 s | 37.7 ms | 5.05 s | 17.7 s | 572 ms | 13.1 s | 1.7 s | 43.2 s |
| `tc-cyclic` | 6.29 s | **4.7 s** | 10.9 s | 8.09 s | 95 s | 74.9 s | 46.5 s | 7.64 s | 173 s |
| `tc-cyclic-bf` | 10.1 s | **4.42 s** | 4.44 s | 7.77 s | 88.3 s | 55.9 s | 45.3 s | 6.82 s | 209 s |
| `tc-cyclic-fb` | **19.6 ms** | 15.9 s | 38.8 ms | 18.6 s | 89.6 s | 580 ms | 45.5 s | 6.83 s | 208 s |
| `tc-nonlinear` | **263 ms** | 449 ms | 714 ms | 640 ms | 2.06 s | 1.75 s | 1.29 s | n/a | n/a |
| `tc-sparse-acyclic` | **70.2 ms** | 98.6 ms | 113 ms | 154 ms | 268 ms | 749 ms | 347 ms | 104 ms | 877 ms |
| `tc-sparse-cyclic` | **1.05 s** | 1.07 s | 1.74 s | 1.65 s | 5 s | 12.8 s | 7.24 s | 1.35 s | 28.8 s |
| `tree-sg-all` | **90.3 ms** | 92.9 ms | 92 ms | 181 ms | 468 ms | 1.09 s | 458 ms | 155 ms | 942 ms |
| `tree-sg-bound` | 7.48 ms | 3.24 s | **5.72 ms** | 6.19 s | 20.1 s | 99.6 ms | 23.5 s | 5.28 s | 64.5 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 140 ms | 233 ms | timeout | 318 ms | 960 ms | timeout | 545 ms | n/a | n/a |
| `chain-closure` | 1.1 s | 2.15 s | 2.78 s | 2.87 s | timeout | 6.25 s | 5.84 s | 1.95 s | 14.1 s |
| `chain-left` | 2.32 ms | 1.65 s | 2.17 ms | 2.2 s | timeout | 29 ms | 3.85 s | 1.08 s | 12.9 s |
| `chain-right` | 1.11 s | 1.7 s | 2.02 s | 2.3 s | timeout | 1.02 s | 3.78 s | 1.09 s | 12.9 s |
| `cspa` | 312 ms | 625 ms | timeout | 941 ms | 1.93 s | timeout | 1.24 s | n/a | n/a |
| `degree-count` | 68.2 ms | 87.2 ms | 87.3 ms | 120 ms | 331 ms | 1.2 s | 699 ms | 63.1 ms | 326 ms |
| `dist-bounded` | 707 ms | 1.48 s | 1.66 s | 2.07 s | 4.46 s | 5.68 s | 4.02 s | 1.07 s | 4.75 s |
| `facts-join` | 174 ms | 224 ms | 228 ms | 268 ms | 954 ms | 3.44 s | 1.99 s | 120 ms | 341 ms |
| `files-grouped` | 290 ms | 1.12 s | 1.97 s | 2.71 s | 2.03 s | 3.64 s | n/a | 113 ms | 580 ms |
| `graph-left` | 54.3 ms | timeout | 69.2 ms | timeout | timeout | 1.04 s | engine-limit | timeout | timeout |
| `graph-reach` | 54.1 ms | 62.6 ms | 62.9 ms | 87.6 ms | 195 ms | 1.07 s | 542 ms | 84.3 ms | 308 ms |
| `grid-reach` | 58.1 ms | 46.5 ms | 48.3 ms | 75.7 ms | 567 ms | 991 ms | 495 ms | 1.24 s | 289 ms |
| `sg-acyclic` | 886 ms | 1.85 s | 2.65 s | 2.15 s | 6.97 s | 5.44 s | 7.02 s | 1.04 s | 8.06 s |
| `sg-acyclic-bf` | 3.58 ms | 1.69 s | 4.67 ms | 1.99 s | 6.68 s | 58.1 ms | 6.48 s | 805 ms | 7.8 s |
| `sg-cyclic` | 3.81 s | 7.5 s | 12.5 s | 8.57 s | 37.2 s | 25.6 s | 36.4 s | 4.29 s | 34.7 s |
| `startup` | 625 us | 653 us | 695 us | 12.1 ms | 2.25 ms | 6.14 ms | 1.19 ms | 9.94 ms | 1.39 ms |
| `tasks-ready` | 91.2 ms | 170 ms | 176 ms | 259 ms | 309 ms | 1.67 s | 791 ms | 127 ms | 193 s |
| `tc-acyclic` | 1.67 s | 1.36 s | 3.15 s | 2.29 s | 20.5 s | 9.55 s | 13.8 s | 1.99 s | 24 s |
| `tc-acyclic-bf` | 167 ms | 1.23 s | 151 ms | 2.14 s | 17.6 s | 803 ms | 13.2 s | 1.71 s | 23.9 s |
| `tc-acyclic-fb` | 15.9 ms | 4.03 s | 36.1 ms | 5.05 s | 17.6 s | 528 ms | 13.1 s | 1.68 s | 24.1 s |
| `tc-cyclic` | 6.26 s | 4.67 s | 10.9 s | 8.08 s | 94.6 s | 71.7 s | 46.7 s | 7.57 s | 86.9 s |
| `tc-cyclic-bf` | 10 s | 4.4 s | 4.42 s | 7.75 s | 88 s | 55.7 s | 45.2 s | 6.77 s | 87.5 s |
| `tc-cyclic-fb` | 18.3 ms | 15.9 s | 37.3 ms | 18.5 s | 89.4 s | 551 ms | 45.4 s | 6.78 s | 87.6 s |
| `tc-nonlinear` | 261 ms | 444 ms | 709 ms | 622 ms | 2.05 s | 1.41 s | 1.27 s | n/a | n/a |
| `tc-sparse-acyclic` | 68.8 ms | 92.8 ms | 106 ms | 137 ms | 263 ms | 414 ms | 338 ms | 98.3 ms | 624 ms |
| `tc-sparse-cyclic` | 1.04 s | 1.05 s | 1.71 s | 1.6 s | 4.94 s | 9.54 s | 7.18 s | 1.32 s | 13 s |
| `tree-sg-all` | 88.3 ms | 80 ms | 80 ms | 158 ms | 462 ms | 524 ms | 453 ms | 146 ms | 904 ms |
| `tree-sg-bound` | 6.25 ms | 3.19 s | 4.41 ms | 6.12 s | 20 s | 90.4 ms | 23.2 s | 5.03 s | 61.6 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 18.8 MiB | 11.3 MiB | timeout | 18.7 MiB | 33.1 MiB | timeout | 71 MiB | n/a | n/a |
| `chain-closure` | 166 MiB | 76.3 MiB | 216 MiB | 83.7 MiB | timeout | 2.03 GiB | 1.98 GiB | 1.23 GiB | 6.11 MiB |
| `chain-left` | 2.88 MiB | 76.3 MiB | 2.53 MiB | 83.7 MiB | timeout | 11.6 MiB | 1.31 GiB | 679 MiB | 6.05 MiB |
| `chain-right` | 199 MiB | 76.4 MiB | 76.3 MiB | 83.7 MiB | timeout | 913 MiB | 1.31 GiB | 676 MiB | 6.05 MiB |
| `cspa` | 21.9 MiB | 12.8 MiB | timeout | 18.7 MiB | 82.8 MiB | timeout | 102 MiB | n/a | n/a |
| `degree-count` | 18.4 MiB | 11.4 MiB | 11.4 MiB | 18.7 MiB | 25.5 MiB | 63.8 MiB | 105 MiB | 46.6 MiB | 16.6 MiB |
| `dist-bounded` | 86.5 MiB | 142 MiB | 191 MiB | 153 MiB | 501 MiB | 1.11 GiB | 1e+03 MiB | 531 MiB | 10.8 MiB |
| `facts-join` | 59.6 MiB | 72.6 MiB | 72.5 MiB | 77.5 MiB | 110 MiB | 211 MiB | 140 MiB | 58.1 MiB | 20.5 MiB |
| `files-grouped` | 67.8 MiB | 74.5 MiB | 80.5 MiB | 79.3 MiB | 88.8 MiB | 224 MiB | n/a | 55.1 MiB | 27.2 MiB |
| `graph-left` | 16.8 MiB | timeout | 9.19 MiB | timeout | timeout | 55.4 MiB | engine-limit | timeout | timeout |
| `graph-reach` | 16.6 MiB | 8.62 MiB | 8.66 MiB | 18.7 MiB | 21.4 MiB | 55.5 MiB | 50.5 MiB | 38.6 MiB | 11.8 MiB |
| `grid-reach` | 18.1 MiB | 11.2 MiB | 11.6 MiB | 18.7 MiB | 25 MiB | 67.5 MiB | 55.3 MiB | 75.1 MiB | 12.5 MiB |
| `sg-acyclic` | 36.9 MiB | 41.8 MiB | 44 MiB | 44.7 MiB | 650 MiB | 552 MiB | 443 MiB | 444 MiB | 13.3 MiB |
| `sg-acyclic-bf` | 3.14 MiB | 38.2 MiB | 2.52 MiB | 44.8 MiB | 639 MiB | 13.4 MiB | 337 MiB | 422 MiB | 13.2 MiB |
| `sg-cyclic` | 71.2 MiB | 92.6 MiB | 171 MiB | 98 MiB | 2.75 GiB | 1.12 GiB | 916 MiB | 1.38 GiB | 13.3 MiB |
| `startup` | 2.06 MiB | 2.03 MiB | 2.05 MiB | 18.7 MiB | 10.5 MiB | 10 MiB | 4 MiB | 23.7 MiB | 3.11 MiB |
| `tasks-ready` | 18.5 MiB | 15 MiB | 15.3 MiB | 19 MiB | 25.2 MiB | 146 MiB | 97 MiB | 68.2 MiB | 19.4 MiB |
| `tc-acyclic` | 44.8 MiB | 43.4 MiB | 75.4 MiB | 48.6 MiB | 1.48 GiB | 567 MiB | 449 MiB | 295 MiB | 12.3 MiB |
| `tc-acyclic-bf` | 14 MiB | 42.3 MiB | 9.98 MiB | 48.7 MiB | 1.47 GiB | 67.8 MiB | 347 MiB | 225 MiB | 12.2 MiB |
| `tc-acyclic-fb` | 7.33 MiB | 43.7 MiB | 4.84 MiB | 49.3 MiB | 1.47 GiB | 38.6 MiB | 347 MiB | 236 MiB | 12.2 MiB |
| `tc-cyclic` | 86.1 MiB | 92.2 MiB | 163 MiB | 101 MiB | 4.8 GiB | 1.18 GiB | 929 MiB | 597 MiB | 12.3 MiB |
| `tc-cyclic-bf` | 91 MiB | 92.2 MiB | 92.2 MiB | 101 MiB | 4.8 GiB | 464 MiB | 705 MiB | 472 MiB | 12.2 MiB |
| `tc-cyclic-fb` | 7.12 MiB | 93.4 MiB | 4.77 MiB | 103 MiB | 4.8 GiB | 39.2 MiB | 708 MiB | 472 MiB | 12.2 MiB |
| `tc-nonlinear` | 14.8 MiB | 10.8 MiB | 16.8 MiB | 18.7 MiB | 199 MiB | 114 MiB | 108 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 12.3 MiB | 9.03 MiB | 14.1 MiB | 18.7 MiB | 28.8 MiB | 110 MiB | 99.6 MiB | 69.4 MiB | 8 MiB |
| `tc-sparse-cyclic` | 77.4 MiB | 76.2 MiB | 138 MiB | 81.5 MiB | 551 MiB | 1.13 GiB | 915 MiB | 472 MiB | 10.9 MiB |
| `tree-sg-all` | 14.8 MiB | 13.7 MiB | 17.7 MiB | 18.8 MiB | 25.8 MiB | 152 MiB | 158 MiB | 129 MiB | 9.75 MiB |
| `tree-sg-bound` | 4.72 MiB | 679 MiB | 4.05 MiB | 713 MiB | 1.05 GiB | 16.1 MiB | 6.51 GiB | 4.61 GiB | 13.4 MiB |

</details>

### Scale 2, 16 threads

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **56.1 ms** | 92.9 ms | timeout | 131 ms | n/a |
| `chain-closure` | **1.16 s** | 3.45 s | 3.94 s | 3.91 s | 2.31 s |
| `chain-left` | **3.43 ms** | 3.09 s | 399 ms | 3.46 s | 1.31 s |
| `chain-right` | **1.09 s** | 2.94 s | 3.4 s | 3.31 s | 1.57 s |
| `cspa` | **62 ms** | 173 ms | 91.1 s | 253 ms | n/a |
| `degree-count` | **58.2 ms** | 99.2 ms | 100 ms | 130 ms | 61.1 ms |
| `dist-bounded` | **284 ms** | 784 ms | 1.03 s | 1.11 s | 1.12 s |
| `facts-join` | 184 ms | 275 ms | 270 ms | 300 ms | **105 ms** |
| `files-grouped` | 186 ms | 525 ms | 710 ms | 549 ms | **58.6 ms** |
| `graph-left` | **60.1 ms** | 145 s | 82.3 ms | 283 s | timeout |
| `graph-reach` | **51.7 ms** | 66.5 ms | 66.5 ms | 92.7 ms | 75.7 ms |
| `grid-reach` | **59.8 ms** | 149 ms | 150 ms | 177 ms | 984 ms |
| `sg-acyclic` | **185 ms** | 386 ms | 517 ms | 587 ms | 1.07 s |
| `sg-acyclic-bf` | **5.54 ms** | 228 ms | 9.27 ms | 380 ms | 812 ms |
| `sg-cyclic` | **671 ms** | 1.07 s | 1.67 s | 1.7 s | 4.42 s |
| `startup` | **1.44 ms** | 3.08 ms | 3.37 ms | 24.1 ms | 11.8 ms |
| `tasks-ready` | **67.2 ms** | 90.7 ms | 92.1 ms | 131 ms | 133 ms |
| `tc-acyclic` | **296 ms** | 353 ms | 582 ms | 901 ms | 1.99 s |
| `tc-acyclic-bf` | 62.3 ms | 202 ms | **56.1 ms** | 538 ms | 1.74 s |
| `tc-acyclic-fb` | **17.5 ms** | 571 ms | 38 ms | 1.01 s | 1.74 s |
| `tc-cyclic` | 1.11 s | **889 ms** | 1.61 s | 2.07 s | 7.29 s |
| `tc-cyclic-bf` | 1.39 s | 563 ms | **539 ms** | 1.72 s | 6.74 s |
| `tc-cyclic-fb` | **20.3 ms** | 1.87 s | 40.6 ms | 2.94 s | 6.73 s |
| `tc-nonlinear` | **50.5 ms** | 102 ms | 151 ms | 219 ms | n/a |
| `tc-sparse-acyclic` | **29.4 ms** | 79.1 ms | 87.8 ms | 126 ms | 110 ms |
| `tc-sparse-cyclic` | **279 ms** | 549 ms | 740 ms | 946 ms | 1.35 s |
| `tree-sg-all` | **43.8 ms** | 91.2 ms | 97.8 ms | 149 ms | 152 ms |
| `tree-sg-bound` | **7.28 ms** | 1.29 s | 9.33 ms | 3.14 s | 4.22 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 249 ms | 383 ms | timeout | 543 ms | n/a |
| `chain-closure` | 5.13 s | 8.2 s | 9.52 s | 9.36 s | 2.96 s |
| `chain-left` | 2.45 ms | 8.23 s | 197 ms | 9.53 s | 2.66 s |
| `chain-right` | 4.74 s | 7.92 s | 8.61 s | 9.07 s | 2.75 s |
| `cspa` | 556 ms | 987 ms | 1.16e+03 s | 1.53 s | n/a |
| `degree-count` | 100 ms | 107 ms | 121 ms | 138 ms | 102 ms |
| `dist-bounded` | 1.47 s | 1.95 s | 2.18 s | 4.04 s | 1.15 s |
| `facts-join` | 178 ms | 274 ms | 267 ms | 290 ms | 132 ms |
| `files-grouped` | 390 ms | 5.59 s | 8.53 s | 5.58 s | 124 ms |
| `graph-left` | 69.8 ms | 1.31e+03 s | 83.9 ms | 2.32e+03 s | timeout |
| `graph-reach` | 78.8 ms | 113 ms | 115 ms | 144 ms | 110 ms |
| `grid-reach` | 57.6 ms | 496 ms | 497 ms | 515 ms | 1.63 s |
| `sg-acyclic` | 1.53 s | 2.38 s | 3.55 s | 3.79 s | 1.63 s |
| `sg-acyclic-bf` | 9.65 ms | 2.23 s | 9.34 ms | 3.5 s | 1.3 s |
| `sg-cyclic` | 7.32 s | 9.65 s | 16.4 s | 16.4 s | 7.47 s |
| `startup` | 587 us | 1.55 ms | 1.67 ms | 11.7 ms | 9.44 ms |
| `tasks-ready` | 158 ms | 317 ms | 300 ms | 462 ms | 193 ms |
| `tc-acyclic` | 2.99 s | 1.85 s | 4.35 s | 8.87 s | 3.7 s |
| `tc-acyclic-bf` | 263 ms | 1.7 s | 245 ms | 5.56 s | 3.42 s |
| `tc-acyclic-fb` | 20 ms | 5 s | 44.3 ms | 11.1 s | 2.87 s |
| `tc-cyclic` | 13.7 s | 6.38 s | 14.7 s | 21.9 s | 14 s |
| `tc-cyclic-bf` | 18.1 s | 6.1 s | 6.05 s | 25.9 s | 11.9 s |
| `tc-cyclic-fb` | 20.6 ms | 20.2 s | 49 ms | 36.8 s | 13.5 s |
| `tc-nonlinear` | 432 ms | 626 ms | 1.01 s | 1.94 s | n/a |
| `tc-sparse-acyclic` | 140 ms | 179 ms | 227 ms | 318 ms | 155 ms |
| `tc-sparse-cyclic` | 1.86 s | 1.37 s | 2.3 s | 4.82 s | 1.98 s |
| `tree-sg-all` | 163 ms | 191 ms | 191 ms | 319 ms | 198 ms |
| `tree-sg-bound` | 6.23 ms | 3.74 s | 7.17 ms | 11.5 s | 9.06 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 24.7 MiB | 13 MiB | timeout | 18.7 MiB | n/a |
| `chain-closure` | 180 MiB | 81.3 MiB | 227 MiB | 85.7 MiB | 1.24 GiB |
| `chain-left` | 2.88 MiB | 81.5 MiB | 3.77 MiB | 86.1 MiB | 680 MiB |
| `chain-right` | 209 MiB | 81.3 MiB | 81.3 MiB | 85.6 MiB | 688 MiB |
| `cspa` | 49 MiB | 15.2 MiB | 4.4 GiB | 18.7 MiB | n/a |
| `degree-count` | 35.6 MiB | 13.2 MiB | 13.3 MiB | 18.7 MiB | 93 MiB |
| `dist-bounded` | 455 MiB | 151 MiB | 208 MiB | 155 MiB | 538 MiB |
| `facts-join` | 59.6 MiB | 74.5 MiB | 74.5 MiB | 78.4 MiB | 70.3 MiB |
| `files-grouped` | 145 MiB | 81 MiB | 87.6 MiB | 84.5 MiB | 63.7 MiB |
| `graph-left` | 19 MiB | 32.6 GiB | 10.7 MiB | 30.2 GiB | timeout |
| `graph-reach` | 19.4 MiB | 10.3 MiB | 10.1 MiB | 18.7 MiB | 40.8 MiB |
| `grid-reach` | 18.1 MiB | 12.8 MiB | 13.2 MiB | 18.6 MiB | 76 MiB |
| `sg-acyclic` | 232 MiB | 46 MiB | 76.9 MiB | 45.8 MiB | 452 MiB |
| `sg-acyclic-bf` | 4.09 MiB | 42 MiB | 3.89 MiB | 45.9 MiB | 381 MiB |
| `sg-cyclic` | 877 MiB | 99.4 MiB | 182 MiB | 99.1 MiB | 1.39 GiB |
| `startup` | 2.06 MiB | 2.61 MiB | 2.61 MiB | 18.7 MiB | 24 MiB |
| `tasks-ready` | 32.5 MiB | 16.1 MiB | 17.2 MiB | 20 MiB | 95.1 MiB |
| `tc-acyclic` | 288 MiB | 47.5 MiB | 81.1 MiB | 50.1 MiB | 294 MiB |
| `tc-acyclic-bf` | 35.8 MiB | 46.3 MiB | 12.2 MiB | 50.3 MiB | 236 MiB |
| `tc-acyclic-fb` | 7.92 MiB | 47.9 MiB | 6.52 MiB | 50.9 MiB | 240 MiB |
| `tc-cyclic` | 1.13 GiB | 99 MiB | 173 MiB | 103 MiB | 585 MiB |
| `tc-cyclic-bf` | 1.12 GiB | 99.1 MiB | 99.1 MiB | 103 MiB | 461 MiB |
| `tc-cyclic-fb` | 7.69 MiB | 100 MiB | 6.47 MiB | 104 MiB | 462 MiB |
| `tc-nonlinear` | 79.8 MiB | 13.1 MiB | 19.4 MiB | 18.7 MiB | n/a |
| `tc-sparse-acyclic` | 32.3 MiB | 11.2 MiB | 16.9 MiB | 18.7 MiB | 86.6 MiB |
| `tc-sparse-cyclic` | 325 MiB | 79.2 MiB | 151 MiB | 83.2 MiB | 501 MiB |
| `tree-sg-all` | 47.2 MiB | 16.2 MiB | 20.5 MiB | 19.9 MiB | 140 MiB |
| `tree-sg-bound` | 4.72 MiB | 711 MiB | 5.28 MiB | 715 MiB | 4.93 GiB |

</details>

### Scale 4, 1 thread

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **349 ms** | 619 ms | timeout | 816 ms | 2.69 s | timeout | 1.36 s | n/a | n/a |
| `chain-closure` | **5.22 s** | 9.89 s | 12.6 s | 12.9 s | timeout | 52.6 s | 25 s | 7.89 s | 131 s |
| `chain-left` | 5.53 ms | 7.62 s | **4.38 ms** | 9.85 s | timeout | 65.5 ms | 16.8 s | 4.41 s | 129 s |
| `chain-right` | 5.59 s | 8.09 s | 9.49 s | 10.5 s | timeout | 5.18 s | 16.9 s | **4.45 s** | 128 s |
| `cspa` | **652 ms** | 1.34 s | timeout | 1.98 s | 4.1 s | timeout | 2.91 s | n/a | n/a |
| `degree-count` | 146 ms | 191 ms | 191 ms | 260 ms | 665 ms | 2.82 s | 1.55 s | **114 ms** | 703 ms |
| `dist-bounded` | **1.9 s** | 3.84 s | 4.31 s | 5.27 s | 11.7 s | 23.9 s | 10.3 s | 2.8 s | 12.7 s |
| `facts-join` | 374 ms | 552 ms | 553 ms | 674 ms | 2.33 s | 7.94 s | 3.93 s | **234 ms** | 733 ms |
| `files-grouped` | 603 ms | 2.6 s | 4.35 s | 7.6 s | 4.02 s | 7.98 s | n/a | **220 ms** | 1.26 s |
| `graph-left` | **113 ms** | timeout | 144 ms | timeout | timeout | 2.36 s | engine-limit | timeout | timeout |
| `graph-reach` | **112 ms** | 132 ms | 133 ms | 183 ms | 388 ms | 2.35 s | 1.09 s | 153 ms | 1.04 s |
| `grid-reach` | 241 ms | **213 ms** | 217 ms | 306 ms | 3.57 s | 4.65 s | 2.12 s | 7.21 s | 1.37 s |
| `sg-acyclic` | **3.59 s** | 6.59 s | 9.34 s | 7.43 s | 26.3 s | 22.9 s | 26.9 s | 3.89 s | 31.8 s |
| `sg-acyclic-bf` | **6.44 ms** | 5.99 s | 7.98 ms | 6.79 s | 25.4 s | 117 ms | 24.8 s | 3.05 s | 31.3 s |
| `sg-cyclic` | 19.3 s | 33.2 s | 56 s | 36.6 s | 175 s | 125 s | 162 s | **19.1 s** | 197 s |
| `startup` | **1.51 ms** | 1.72 ms | 1.81 ms | 23.8 ms | 3.78 ms | 8.63 ms | 2.39 ms | 12.4 ms | 2.49 ms |
| `tasks-ready` | **212 ms** | 441 ms | 444 ms | 685 ms | 727 ms | 3.67 s | 1.75 s | 282 ms | timeout |
| `tc-acyclic` | 8 s | **6.09 s** | 13.8 s | 9.84 s | 92.1 s | 44.2 s | 64.2 s | 9.01 s | 222 s |
| `tc-acyclic-bf` | 3.21 s | 5.53 s | **594 ms** | 9.29 s | 91.7 s | 2.42 s | 62.2 s | 8.07 s | 223 s |
| `tc-acyclic-fb` | **33.7 ms** | 22.2 s | 77.2 ms | 24.2 s | 91.1 s | 1.15 s | 62.6 s | 7.97 s | 223 s |
| `tc-cyclic` | 29 s | **21.6 s** | 49.2 s | 34.3 s | timeout | timeout | 198 s | 31.6 s | timeout |
| `tc-cyclic-bf` | 44.5 s | **19.9 s** | 19.9 s | 32.9 s | timeout | 233 s | 193 s | 29.7 s | timeout |
| `tc-cyclic-fb` | **38.1 ms** | 89.5 s | 82.9 ms | 92.4 s | timeout | 1.17 s | 193 s | 30 s | timeout |
| `tc-nonlinear` | **744 ms** | 1.28 s | 2.03 s | 1.72 s | 5.6 s | 4.77 s | 4.02 s | n/a | n/a |
| `tc-sparse-acyclic` | **195 ms** | 279 ms | 309 ms | 399 ms | 744 ms | 2.16 s | 1.07 s | 260 ms | 2.93 s |
| `tc-sparse-cyclic` | 4.96 s | **4.26 s** | 7.12 s | 6.63 s | 20.7 s | 52.1 s | 30.4 s | 5.67 s | 122 s |
| `tree-sg-all` | 1.64 s | **1.33 s** | 1.4 s | 2.4 s | 7.3 s | 17.3 s | 8.43 s | 2.41 s | 16.2 s |
| `tree-sg-bound` | 23.8 ms | 58.2 s | **16.3 ms** | 104 s | timeout | 362 ms | engine-limit | 92.7 s | timeout |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 347 ms | 612 ms | timeout | 796 ms | 2.68 s | timeout | 1.35 s | n/a | n/a |
| `chain-closure` | 5.15 s | 9.62 s | 12.3 s | 12.6 s | timeout | 26.7 s | 24.6 s | 7.64 s | 60.1 s |
| `chain-left` | 4.46 ms | 7.55 s | 3.16 ms | 9.75 s | timeout | 53.1 ms | 16.5 s | 4.26 s | 55.2 s |
| `chain-right` | 5.49 s | 8.03 s | 9.41 s | 10.4 s | timeout | 4.84 s | 16.6 s | 4.29 s | 55.5 s |
| `cspa` | 650 ms | 1.33 s | timeout | 1.96 s | 4.08 s | timeout | 2.89 s | n/a | n/a |
| `degree-count` | 141 ms | 187 ms | 187 ms | 242 ms | 660 ms | 2.51 s | 1.53 s | 109 ms | 684 ms |
| `dist-bounded` | 1.87 s | 3.74 s | 4.2 s | 5.14 s | 11.5 s | 14 s | 10.1 s | 2.74 s | 11.8 s |
| `facts-join` | 364 ms | 531 ms | 530 ms | 638 ms | 2.31 s | 7.36 s | 3.9 s | 226 ms | 725 ms |
| `files-grouped` | 591 ms | 2.58 s | 4.33 s | 7.69 s | 4.01 s | 7.49 s | n/a | 212 ms | 1.2 s |
| `graph-left` | 110 ms | timeout | 140 ms | timeout | timeout | 2.14 s | engine-limit | timeout | timeout |
| `graph-reach` | 108 ms | 128 ms | 130 ms | 168 ms | 384 ms | 2.12 s | 1.08 s | 147 ms | 651 ms |
| `grid-reach` | 234 ms | 203 ms | 208 ms | 284 ms | 3.54 s | 4.06 s | 2.09 s | 7.18 s | 1.23 s |
| `sg-acyclic` | 3.58 s | 6.55 s | 9.29 s | 7.36 s | 26.2 s | 17.6 s | 26.7 s | 3.83 s | 27.5 s |
| `sg-acyclic-bf` | 5.46 ms | 5.99 s | 6.92 ms | 6.76 s | 25.3 s | 106 ms | 24.7 s | 2.99 s | 26.6 s |
| `sg-cyclic` | 19.3 s | 33 s | 55.7 s | 36.4 s | 174 s | 112 s | 162 s | 18.9 s | 152 s |
| `startup` | 616 us | 681 us | 709 us | 12.1 ms | 2.24 ms | 6.05 ms | 1.22 ms | 9.74 ms | 1.39 ms |
| `tasks-ready` | 208 ms | 435 ms | 438 ms | 665 ms | 721 ms | 3.35 s | 1.73 s | 273 ms | timeout |
| `tc-acyclic` | 7.93 s | 6.03 s | 13.7 s | 9.81 s | 91.7 s | 38.2 s | 64 s | 8.94 s | 107 s |
| `tc-acyclic-bf` | 3.2 s | 5.51 s | 589 ms | 9.25 s | 91.1 s | 2.32 s | 61.9 s | 8.02 s | 106 s |
| `tc-acyclic-fb` | 31.6 ms | 22.1 s | 75.6 ms | 24.2 s | 90.6 s | 1.07 s | 62.3 s | 7.94 s | 107 s |
| `tc-cyclic` | 28.9 s | 21.4 s | 48.9 s | 34.2 s | timeout | timeout | 198 s | 31.4 s | timeout |
| `tc-cyclic-bf` | 44.3 s | 19.8 s | 19.9 s | 32.8 s | timeout | 233 s | 193 s | 29.6 s | timeout |
| `tc-cyclic-fb` | 36.1 ms | 89.6 s | 80.8 ms | 91.8 s | timeout | 1.08 s | 192 s | 29.8 s | timeout |
| `tc-nonlinear` | 741 ms | 1.27 s | 2.01 s | 1.69 s | 5.57 s | 3.78 s | 4 s | n/a | n/a |
| `tc-sparse-acyclic` | 191 ms | 261 ms | 294 ms | 372 ms | 733 ms | 1.15 s | 1.05 s | 250 ms | 1.9 s |
| `tc-sparse-cyclic` | 4.92 s | 4.14 s | 7 s | 6.5 s | 20.5 s | 39.3 s | 30.1 s | 5.55 s | 56.1 s |
| `tree-sg-all` | 1.64 s | 1.24 s | 1.3 s | 2.29 s | 7.22 s | 8.4 s | 8.29 s | 2.32 s | 15.6 s |
| `tree-sg-bound` | 22.4 ms | 57.3 s | 14.4 ms | 103 s | timeout | 333 ms | engine-limit | 86.5 s | timeout |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 33.4 MiB | 22 MiB | timeout | 26.8 MiB | 55.9 MiB | timeout | 143 MiB | n/a | n/a |
| `chain-closure` | 737 MiB | 292 MiB | 833 MiB | 307 MiB | timeout | 8.97 GiB | 7.22 GiB | 4.42 GiB | 6.34 MiB |
| `chain-left` | 3.44 MiB | 292 MiB | 2.97 MiB | 307 MiB | timeout | 13.7 MiB | 5.38 GiB | 2.53 GiB | 6.36 MiB |
| `chain-right` | 855 MiB | 292 MiB | 292 MiB | 307 MiB | timeout | 3.94 GiB | 5.38 GiB | 2.53 GiB | 6.33 MiB |
| `cspa` | 36.8 MiB | 22.7 MiB | timeout | 27.2 MiB | 136 MiB | timeout | 207 MiB | n/a | n/a |
| `degree-count` | 36.1 MiB | 19.9 MiB | 19.8 MiB | 25 MiB | 38.3 MiB | 120 MiB | 212 MiB | 61.3 MiB | 26.1 MiB |
| `dist-bounded` | 228 MiB | 365 MiB | 377 MiB | 387 MiB | 1.24 GiB | 2.78 GiB | 2.46 GiB | 1.25 GiB | 11 MiB |
| `facts-join` | 125 MiB | 145 MiB | 145 MiB | 151 MiB | 205 MiB | 408 MiB | 273 MiB | 97.5 MiB | 38.8 MiB |
| `files-grouped` | 126 MiB | 149 MiB | 161 MiB | 155 MiB | 178 MiB | 438 MiB | n/a | 84.8 MiB | 49.6 MiB |
| `graph-left` | 33.2 MiB | timeout | 15.4 MiB | timeout | timeout | 106 MiB | engine-limit | timeout | timeout |
| `graph-reach` | 33.2 MiB | 14.3 MiB | 14.3 MiB | 19 MiB | 28.9 MiB | 106 MiB | 99.4 MiB | 50.6 MiB | 18 MiB |
| `grid-reach` | 56.4 MiB | 41.7 MiB | 43.3 MiB | 46.4 MiB | 67 MiB | 246 MiB | 211 MiB | 150 MiB | 32.3 MiB |
| `sg-acyclic` | 156 MiB | 125 MiB | 239 MiB | 136 MiB | 1.96 GiB | 1.56 GiB | 1.44 GiB | 1.56 GiB | 13.4 MiB |
| `sg-acyclic-bf` | 3.78 MiB | 124 MiB | 2.75 MiB | 136 MiB | 1.96 GiB | 15 MiB | 1.01e+03 MiB | 1.39 GiB | 13.4 MiB |
| `sg-cyclic` | 354 MiB | 347 MiB | 667 MiB | 362 MiB | 9.83 GiB | 3.61 GiB | 3.01 GiB | 4.7 GiB | 13.4 MiB |
| `startup` | 2.06 MiB | 2.06 MiB | 2.05 MiB | 18.7 MiB | 10.5 MiB | 10 MiB | 4 MiB | 23.6 MiB | 3.11 MiB |
| `tasks-ready` | 36.8 MiB | 31.2 MiB | 32.8 MiB | 35.2 MiB | 46.6 MiB | 306 MiB | 204 MiB | 105 MiB | timeout |
| `tc-acyclic` | 168 MiB | 163 MiB | 288 MiB | 170 MiB | 5.19 GiB | 1.9 GiB | 1.47 GiB | 1.01 GiB | 13.9 MiB |
| `tc-acyclic-bf` | 41.7 MiB | 157 MiB | 30.2 MiB | 170 MiB | 5.19 GiB | 226 MiB | 1.28 GiB | 806 MiB | 13.8 MiB |
| `tc-acyclic-fb` | 13.2 MiB | 164 MiB | 7.56 MiB | 171 MiB | 5.19 GiB | 69.6 MiB | 1.28 GiB | 804 MiB | 13.8 MiB |
| `tc-cyclic` | 362 MiB | 373 MiB | 654 MiB | 398 MiB | timeout | timeout | 3.2 GiB | 2.2 GiB | timeout |
| `tc-cyclic-bf` | 420 MiB | 373 MiB | 373 MiB | 398 MiB | timeout | 1.32 GiB | 2.67 GiB | 1.91 GiB | timeout |
| `tc-cyclic-fb` | 13.3 MiB | 374 MiB | 7.38 MiB | 400 MiB | timeout | 79.4 MiB | 2.67 GiB | 1.71 GiB | timeout |
| `tc-nonlinear` | 31.5 MiB | 27.7 MiB | 44 MiB | 33.5 MiB | 519 MiB | 319 MiB | 314 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 22.3 MiB | 22 MiB | 36.5 MiB | 23.7 MiB | 79.6 MiB | 310 MiB | 288 MiB | 139 MiB | 11 MiB |
| `tc-sparse-cyclic` | 354 MiB | 312 MiB | 548 MiB | 309 MiB | 2.06 GiB | 3.67 GiB | 3.16 GiB | 1.94 GiB | 11 MiB |
| `tree-sg-all` | 191 MiB | 175 MiB | 242 MiB | 188 MiB | 298 MiB | 2.21 GiB | 2.37 GiB | 1.78 GiB | 10.8 MiB |
| `tree-sg-bound` | 11.2 MiB | 10.3 GiB | 9.19 MiB | 10.7 GiB | timeout | 33.7 MiB | engine-limit | 74.2 GiB | timeout |

</details>

### Scale 4, 16 threads

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **114 ms** | 194 ms | timeout | 254 ms | n/a |
| `chain-closure` | **3.73 s** | 12.5 s | 14.1 s | 14.1 s | 8.56 s |
| `chain-left` | **5.52 ms** | 10.4 s | 790 ms | 11.7 s | 8.35 s |
| `chain-right` | **3.35 s** | 10.3 s | 11.3 s | 11.6 s | 4.83 s |
| `cspa` | **113 ms** | 286 ms | timeout | 419 ms | n/a |
| `degree-count` | 115 ms | 206 ms | 207 ms | 242 ms | **94.1 ms** |
| `dist-bounded` | **709 ms** | 1.98 s | 2.61 s | 2.66 s | 2.83 s |
| `facts-join` | 383 ms | 657 ms | 648 ms | 702 ms | **124 ms** |
| `files-grouped` | 387 ms | 1.13 s | 1.49 s | 1.33 s | **62.4 ms** |
| `graph-left` | **121 ms** | timeout | 168 ms | timeout | timeout |
| `graph-reach` | **101 ms** | 129 ms | 130 ms | 157 ms | 106 ms |
| `grid-reach` | **243 ms** | 447 ms | 453 ms | 477 ms | 2.27 s |
| `sg-acyclic` | **660 ms** | 1.31 s | 1.75 s | 1.9 s | 3.99 s |
| `sg-acyclic-bf` | **6.55 ms** | 764 ms | 11.4 ms | 1.28 s | 3.09 s |
| `sg-cyclic` | **2.87 s** | 4.58 s | 7.14 s | 7.01 s | 19.6 s |
| `startup` | **1.37 ms** | 3.3 ms | 2.93 ms | 23.8 ms | 12.3 ms |
| `tasks-ready` | **138 ms** | 200 ms | 201 ms | 278 ms | 214 ms |
| `tc-acyclic` | **1.3 s** | 1.38 s | 2.34 s | 2.85 s | 9.01 s |
| `tc-acyclic-bf` | 376 ms | 775 ms | **155 ms** | 2.26 s | 8 s |
| `tc-acyclic-fb` | **34.1 ms** | 2.78 s | 76.3 ms | 3.96 s | 8.05 s |
| `tc-cyclic` | 4.93 s | **3.63 s** | 6.72 s | 8.4 s | 30.3 s |
| `tc-cyclic-bf` | 5.93 s | 2.32 s | **2.3 s** | 10.1 s | 28.2 s |
| `tc-cyclic-fb` | **39.1 ms** | 10.6 s | 80.6 ms | 16.5 s | 28.2 s |
| `tc-nonlinear` | **134 ms** | 268 ms | 396 ms | 502 ms | n/a |
| `tc-sparse-acyclic` | **68.5 ms** | 197 ms | 226 ms | 290 ms | 274 ms |
| `tc-sparse-cyclic` | **1.1 s** | 2.12 s | 2.95 s | 3.31 s | 5.75 s |
| `tree-sg-all` | **625 ms** | 1.12 s | 1.33 s | 1.69 s | 2.27 s |
| `tree-sg-bound` | 24.5 ms | 20.2 s | **23 ms** | 53.4 s | 72 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 540 ms | 916 ms | timeout | 1.37 s | n/a |
| `chain-closure` | 22.1 s | 27.7 s | 31.1 s | 33 s | 10.8 s |
| `chain-left` | 4.4 ms | 26.1 s | 384 ms | 30.5 s | 9.77 s |
| `chain-right` | 21.6 s | 25.7 s | 27.3 s | 30.3 s | 6.57 s |
| `cspa` | 1.07 s | 1.93 s | timeout | 3.03 s | n/a |
| `degree-count` | 194 ms | 223 ms | 225 ms | 285 ms | 169 ms |
| `dist-bounded` | 3.92 s | 4.78 s | 5.39 s | 8.16 s | 2.88 s |
| `facts-join` | 372 ms | 654 ms | 646 ms | 710 ms | 256 ms |
| `files-grouped` | 793 ms | 11.8 s | 18.3 s | 14.4 s | 235 ms |
| `graph-left` | 132 ms | timeout | 169 ms | timeout | timeout |
| `graph-reach` | 145 ms | 220 ms | 219 ms | 252 ms | 179 ms |
| `grid-reach` | 236 ms | 1.48 s | 1.52 s | 1.44 s | 10.3 s |
| `sg-acyclic` | 5.99 s | 8.01 s | 11.8 s | 12.7 s | 7.34 s |
| `sg-acyclic-bf` | 5.57 ms | 7.51 s | 10.2 ms | 11.9 s | 5.5 s |
| `sg-cyclic` | 33.1 s | 41.8 s | 71.9 s | 68.7 s | 41.9 s |
| `startup` | 574 us | 1.56 ms | 1.48 ms | 11.7 ms | 9.62 ms |
| `tasks-ready` | 353 ms | 726 ms | 731 ms | 1.18 s | 440 ms |
| `tc-acyclic` | 14.4 s | 7.98 s | 18.5 s | 34.6 s | 16.5 s |
| `tc-acyclic-bf` | 4.36 s | 7.43 s | 849 ms | 28.1 s | 19.5 s |
| `tc-acyclic-fb` | 35.6 ms | 27.3 s | 94.9 ms | 46.2 s | 17.1 s |
| `tc-cyclic` | 62.6 s | 28.7 s | 65.7 s | 118 s | 54.1 s |
| `tc-cyclic-bf` | 80 s | 27.4 s | 27 s | 133 s | 64.5 s |
| `tc-cyclic-fb` | 38.8 ms | 128 s | 99.3 ms | 218 s | 63.3 s |
| `tc-nonlinear` | 1.19 s | 1.72 s | 2.7 s | 4.4 s | n/a |
| `tc-sparse-acyclic` | 343 ms | 390 ms | 479 ms | 776 ms | 388 ms |
| `tc-sparse-cyclic` | 8.37 s | 5.17 s | 9.07 s | 13.1 s | 9.71 s |
| `tree-sg-all` | 2.44 s | 1.74 s | 1.88 s | 3.96 s | 3.3 s |
| `tree-sg-bound` | 22.8 ms | 56.2 s | 20 ms | 187 s | 163 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 47 MiB | 23.7 MiB | timeout | 27.7 MiB | n/a |
| `chain-closure` | 754 MiB | 305 MiB | 886 MiB | 309 MiB | 4.44 GiB |
| `chain-left` | 3.44 MiB | 305 MiB | 4.17 MiB | 310 MiB | 2.53 GiB |
| `chain-right` | 870 MiB | 305 MiB | 305 MiB | 309 MiB | 2.55 GiB |
| `cspa` | 91.4 MiB | 25.7 MiB | timeout | 28.5 MiB | n/a |
| `degree-count` | 76.2 MiB | 21.9 MiB | 22.1 MiB | 25.9 MiB | 147 MiB |
| `dist-bounded` | 1.01 GiB | 385 MiB | 535 MiB | 389 MiB | 1.25 GiB |
| `facts-join` | 115 MiB | 148 MiB | 148 MiB | 152 MiB | 119 MiB |
| `files-grouped` | 291 MiB | 161 MiB | 174 MiB | 163 MiB | 98.7 MiB |
| `graph-left` | 37.9 MiB | timeout | 17.1 MiB | timeout | timeout |
| `graph-reach` | 37.2 MiB | 16.1 MiB | 16.1 MiB | 19.9 MiB | 54.5 MiB |
| `grid-reach` | 56.4 MiB | 43.8 MiB | 45.2 MiB | 47.9 MiB | 163 MiB |
| `sg-acyclic` | 772 MiB | 143 MiB | 253 MiB | 137 MiB | 1.58 GiB |
| `sg-acyclic-bf` | 3.8 MiB | 133 MiB | 4.12 MiB | 137 MiB | 1.36 GiB |
| `sg-cyclic` | 2.77 GiB | 366 MiB | 703 MiB | 363 MiB | 4.54 GiB |
| `startup` | 2.06 MiB | 2.61 MiB | 2.64 MiB | 18.7 MiB | 23.9 MiB |
| `tasks-ready` | 70.8 MiB | 33.4 MiB | 34.7 MiB | 36.4 MiB | 166 MiB |
| `tc-acyclic` | 1.19 GiB | 174 MiB | 304 MiB | 171 MiB | 988 MiB |
| `tc-acyclic-bf` | 213 MiB | 168 MiB | 33.9 MiB | 172 MiB | 750 MiB |
| `tc-acyclic-fb` | 13.8 MiB | 175 MiB | 9.67 MiB | 173 MiB | 756 MiB |
| `tc-cyclic` | 5.42 GiB | 396 MiB | 689 MiB | 400 MiB | 2.23 GiB |
| `tc-cyclic-bf` | 5.42 GiB | 396 MiB | 396 MiB | 400 MiB | 1.74 GiB |
| `tc-cyclic-fb` | 14 MiB | 398 MiB | 9.39 MiB | 402 MiB | 1.74 GiB |
| `tc-nonlinear` | 220 MiB | 31 MiB | 48.8 MiB | 34.8 MiB | n/a |
| `tc-sparse-acyclic` | 76.6 MiB | 25.1 MiB | 40.4 MiB | 25.1 MiB | 161 MiB |
| `tc-sparse-cyclic` | 1.23 GiB | 330 MiB | 593 MiB | 311 MiB | 1.96 GiB |
| `tree-sg-all` | 666 MiB | 186 MiB | 208 MiB | 189 MiB | 1.81 GiB |
| `tree-sg-bound` | 11.2 MiB | 10.7 GiB | 10.5 MiB | 10.7 GiB | 71.5 GiB |

</details>

### Scale 8, 1 thread

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **706 ms** | 1.45 s | timeout | 1.87 s | 6.57 s | timeout | 2.94 s | n/a | n/a |
| `chain-closure` | **22.5 s** | 45 s | 56.6 s | 57.4 s | timeout | timeout | 104 s | 31.1 s | timeout |
| `chain-left` | 8.8 ms | 33.9 s | **7.7 ms** | 43.2 s | timeout | 122 ms | 68.5 s | 17.3 s | timeout |
| `chain-right` | 23.1 s | 36.8 s | 42.6 s | 46.5 s | timeout | 19.8 s | 68.6 s | **17.2 s** | timeout |
| `cspa` | **1.71 s** | 2.97 s | timeout | 4.38 s | 10.6 s | timeout | 8.22 s | n/a | n/a |
| `degree-count` | 304 ms | 451 ms | 450 ms | 564 ms | 1.37 s | 5.97 s | 3.69 s | **235 ms** | 1.53 s |
| `dist-bounded` | **4.74 s** | 8.85 s | 9.98 s | 12 s | 28 s | 54.9 s | 24.1 s | 6.89 s | 29.9 s |
| `facts-join` | 937 ms | 1.54 s | 1.54 s | 1.77 s | 6.47 s | 17.3 s | 8.37 s | **480 ms** | 1.69 s |
| `files-grouped` | 1.33 s | 6.96 s | 11.7 s | 24.7 s | 8.51 s | 17.1 s | n/a | **433 ms** | 2.62 s |
| `graph-left` | **252 ms** | timeout | 326 ms | timeout | timeout | 5.11 s | engine-limit | timeout | timeout |
| `graph-reach` | **250 ms** | 298 ms | 299 ms | 382 ms | 803 ms | 5.04 s | 2.58 s | 317 ms | 2.64 s |
| `grid-reach` | 1.14 s | **1.03 s** | 1.06 s | 1.33 s | 24.6 s | 21.7 s | 9.23 s | 55.3 s | 7.41 s |
| `sg-acyclic` | 15.7 s | 24.6 s | 35.3 s | 28.9 s | 110 s | 84.6 s | 104 s | **15.2 s** | 145 s |
| `sg-acyclic-bf` | **12.7 ms** | 22.2 s | 16.3 ms | 26.7 s | 106 s | 229 ms | 97.8 s | 12.2 s | 139 s |
| `sg-cyclic` | 85.7 s | 147 s | 247 s | 159 s | timeout | timeout | timeout | **78.7 s** | timeout |
| `startup` | **1.49 ms** | 1.85 ms | 1.55 ms | 24.3 ms | 3.75 ms | 8.49 ms | 2.2 ms | 12.5 ms | 2.49 ms |
| `tasks-ready` | **547 ms** | 1.09 s | 1.05 s | 1.72 s | 1.78 s | 7.76 s | 4.37 s | 618 ms | timeout |
| `tc-acyclic` | 35.7 s | **25 s** | 59.5 s | 40.6 s | timeout | 166 s | 293 s | 37.6 s | timeout |
| `tc-acyclic-bf` | 13.7 s | 22.7 s | **2.15 s** | 37.9 s | timeout | 7 s | 283 s | 34 s | timeout |
| `tc-acyclic-fb` | **67 ms** | 110 s | 168 ms | 111 s | timeout | 2.32 s | 282 s | 33.8 s | timeout |
| `tc-cyclic` | 121 s | **76 s** | 198 s | 127 s | timeout | timeout | timeout | 133 s | timeout |
| `tc-cyclic-bf` | 189 s | **70.6 s** | 71.1 s | 121 s | timeout | timeout | timeout | 125 s | timeout |
| `tc-cyclic-fb` | **76.6 ms** | timeout | 175 ms | timeout | timeout | 2.34 s | timeout | 124 s | timeout |
| `tc-nonlinear` | **2.41 s** | 4.29 s | 6.45 s | 5.57 s | 20.4 s | 14.1 s | 15.5 s | n/a | n/a |
| `tc-sparse-acyclic` | **624 ms** | 809 ms | 906 ms | 1.16 s | 2.24 s | 6.28 s | 3.74 s | 824 ms | 12.5 s |
| `tc-sparse-cyclic` | 21.7 s | **17.2 s** | 29.7 s | 27.1 s | 85.8 s | 211 s | 127 s | 23.5 s | timeout |
| `tree-sg-all` | timeout | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference |
| `tree-sg-bound` | 399 ms | timeout | **313 ms** | timeout | timeout | 5.92 s | engine-limit | resource-limit | timeout |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 706 ms | 1.43 s | timeout | 1.85 s | 6.55 s | timeout | 2.91 s | n/a | n/a |
| `chain-closure` | 22.3 s | 43.9 s | 55.4 s | 56.2 s | timeout | timeout | 102 s | 30.2 s | timeout |
| `chain-left` | 7.81 ms | 33.6 s | 6.32 ms | 43 s | timeout | 99.7 ms | 67.2 s | 16.8 s | timeout |
| `chain-right` | 22.8 s | 36.7 s | 42.9 s | 46.2 s | timeout | 18.7 s | 67.2 s | 16.7 s | timeout |
| `cspa` | 1.71 s | 2.95 s | timeout | 4.34 s | 10.5 s | timeout | 8.17 s | n/a | n/a |
| `degree-count` | 299 ms | 439 ms | 441 ms | 541 ms | 1.36 s | 5.36 s | 3.64 s | 226 ms | 1.48 s |
| `dist-bounded` | 4.67 s | 8.57 s | 9.68 s | 11.7 s | 27.7 s | 31.6 s | 23.7 s | 6.7 s | 27.6 s |
| `facts-join` | 920 ms | 1.52 s | 1.51 s | 1.71 s | 6.35 s | 16 s | 8.28 s | 465 ms | 1.65 s |
| `files-grouped` | 1.31 s | 6.89 s | 11.6 s | 24.7 s | 8.43 s | 16.1 s | n/a | 419 ms | 2.49 s |
| `graph-left` | 245 ms | timeout | 315 ms | timeout | timeout | 4.71 s | engine-limit | timeout | timeout |
| `graph-reach` | 247 ms | 290 ms | 290 ms | 359 ms | 794 ms | 4.57 s | 2.52 s | 311 ms | 1.45 s |
| `grid-reach` | 1.11 s | 994 ms | 1.03 s | 1.28 s | 24.6 s | 19.4 s | 9.15 s | 55.3 s | 5.54 s |
| `sg-acyclic` | 15.7 s | 24.4 s | 35 s | 28.6 s | 109 s | 64.2 s | 104 s | 14.9 s | 109 s |
| `sg-acyclic-bf` | 11.5 ms | 22.1 s | 15.2 ms | 26.6 s | 106 s | 207 ms | 97.2 s | 12 s | 104 s |
| `sg-cyclic` | 85.5 s | 146 s | 245 s | 159 s | timeout | timeout | timeout | 77.3 s | timeout |
| `startup` | 617 us | 707 us | 607 us | 12.2 ms | 2.3 ms | 6.04 ms | 1.18 ms | 9.71 ms | 1.41 ms |
| `tasks-ready` | 538 ms | 1.08 s | 1.04 s | 1.7 s | 1.76 s | 7.14 s | 4.33 s | 600 ms | timeout |
| `tc-acyclic` | 35.6 s | 24.8 s | 58.9 s | 40.4 s | timeout | 142 s | 290 s | 37.3 s | timeout |
| `tc-acyclic-bf` | 13.6 s | 22.7 s | 2.14 s | 37.7 s | timeout | 6.78 s | 281 s | 33.7 s | timeout |
| `tc-acyclic-fb` | 63.6 ms | 110 s | 164 ms | 111 s | timeout | 2.16 s | 281 s | 33.7 s | timeout |
| `tc-cyclic` | 120 s | 75.4 s | 197 s | 126 s | timeout | timeout | timeout | 132 s | timeout |
| `tc-cyclic-bf` | 189 s | 70.4 s | 70.8 s | 120 s | timeout | timeout | timeout | 124 s | timeout |
| `tc-cyclic-fb` | 73.5 ms | timeout | 172 ms | timeout | timeout | 2.17 s | timeout | 124 s | timeout |
| `tc-nonlinear` | 2.39 s | 4.25 s | 6.42 s | 5.53 s | 20.3 s | 11.1 s | 15.4 s | n/a | n/a |
| `tc-sparse-acyclic` | 611 ms | 782 ms | 878 ms | 1.11 s | 2.21 s | 3.3 s | 3.69 s | 795 ms | 6.48 s |
| `tc-sparse-cyclic` | 21.5 s | 16.7 s | 29.2 s | 26.6 s | 84.7 s | 160 s | 126 s | 23 s | timeout |
| `tree-sg-all` | timeout | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference |
| `tree-sg-bound` | 389 ms | timeout | 299 ms | timeout | timeout | 5.52 s | engine-limit | resource-limit | timeout |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | Nemo | SWI | clingo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 62.2 MiB | 42.9 MiB | timeout | 48 MiB | 110 MiB | timeout | 285 MiB | n/a | n/a |
| `chain-closure` | 2.94 GiB | 1.12 GiB | 3.23 GiB | 1.16 GiB | timeout | timeout | 24.1 GiB | 16.6 GiB | timeout |
| `chain-left` | 4.58 MiB | 1.12 GiB | 3.75 MiB | 1.16 GiB | timeout | 17.2 MiB | 21.2 GiB | 9.96 GiB | timeout |
| `chain-right` | 3.41 GiB | 1.12 GiB | 1.12 GiB | 1.16 GiB | timeout | 13.7 GiB | 21.2 GiB | 9.95 GiB | timeout |
| `cspa` | 69.2 MiB | 44.1 MiB | timeout | 48.6 MiB | 465 MiB | timeout | 404 MiB | n/a | n/a |
| `degree-count` | 67 MiB | 38.8 MiB | 38.8 MiB | 44.6 MiB | 80.5 MiB | 222 MiB | 425 MiB | 106 MiB | 42 MiB |
| `dist-bounded` | 520 MiB | 862 MiB | 1.14 GiB | 910 MiB | 2.59 GiB | 6.66 GiB | 5.48 GiB | 3.34 GiB | 11.4 MiB |
| `facts-join` | 241 MiB | 293 MiB | 293 MiB | 300 MiB | 425 MiB | 799 MiB | 576 MiB | 205 MiB | 73.5 MiB |
| `files-grouped` | 258 MiB | 305 MiB | 329 MiB | 311 MiB | 364 MiB | 869 MiB | n/a | 145 MiB | 95.9 MiB |
| `graph-left` | 58.6 MiB | timeout | 29.9 MiB | timeout | timeout | 195 MiB | engine-limit | timeout | timeout |
| `graph-reach` | 58.4 MiB | 27.6 MiB | 27.7 MiB | 32.6 MiB | 45.9 MiB | 195 MiB | 199 MiB | 74.8 MiB | 32.2 MiB |
| `grid-reach` | 238 MiB | 163 MiB | 163 MiB | 169 MiB | 170 MiB | 952 MiB | 821 MiB | 359 MiB | 102 MiB |
| `sg-acyclic` | 421 MiB | 463 MiB | 561 MiB | 493 MiB | 5.68 GiB | 6.8 GiB | 4.72 GiB | 4.77 GiB | 13.8 MiB |
| `sg-acyclic-bf` | 5.17 MiB | 463 MiB | 3.39 MiB | 493 MiB | 5.68 GiB | 20.5 MiB | 3.01 GiB | 3.95 GiB | 13.8 MiB |
| `sg-cyclic` | 1.45 GiB | 1.37 GiB | 2.64 GiB | 1.44 GiB | timeout | timeout | timeout | 15.7 GiB | timeout |
| `startup` | 2.06 MiB | 2.05 MiB | 2.05 MiB | 18.7 MiB | 10.5 MiB | 10 MiB | 3.98 MiB | 23.7 MiB | 3.12 MiB |
| `tasks-ready` | 88.9 MiB | 69.7 MiB | 73.9 MiB | 72.2 MiB | 88.9 MiB | 704 MiB | 466 MiB | 268 MiB | timeout |
| `tc-acyclic` | 721 MiB | 631 MiB | 1.07 GiB | 620 MiB | timeout | 8.16 GiB | 6.02 GiB | 3.57 GiB | timeout |
| `tc-acyclic-bf` | 117 MiB | 582 MiB | 103 MiB | 621 MiB | timeout | 695 MiB | 4.12 GiB | 2.93 GiB | timeout |
| `tc-acyclic-fb` | 26.4 MiB | 630 MiB | 13 MiB | 624 MiB | timeout | 128 MiB | 4.11 GiB | 2.89 GiB | timeout |
| `tc-cyclic` | 1.46 GiB | 1.4 GiB | 2.5 GiB | 1.49 GiB | timeout | timeout | timeout | 7.97 GiB | timeout |
| `tc-cyclic-bf` | 1.7 GiB | 1.4 GiB | 1.4 GiB | 1.49 GiB | timeout | timeout | timeout | 6.06 GiB | timeout |
| `tc-cyclic-fb` | 26.5 MiB | timeout | 12.6 MiB | timeout | timeout | 147 MiB | timeout | 6.06 GiB | timeout |
| `tc-nonlinear` | 96.8 MiB | 77.5 MiB | 125 MiB | 86.3 MiB | 1.7 GiB | 929 MiB | 961 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 68.3 MiB | 62.2 MiB | 107 MiB | 58.2 MiB | 223 MiB | 901 MiB | 907 MiB | 439 MiB | 11.4 MiB |
| `tc-sparse-cyclic` | 1.45 GiB | 1.18 GiB | 2.1 GiB | 1.16 GiB | 7.73 GiB | 16.8 GiB | 13.1 GiB | 6.39 GiB | timeout |
| `tree-sg-all` | timeout | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference | no-reference |
| `tree-sg-bound` | 135 MiB | timeout | 116 MiB | timeout | timeout | 398 MiB | engine-limit | resource-limit | timeout |

</details>

### Scale 8, 16 threads

**Median wall time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **213 ms** | 383 ms | timeout | 498 ms | n/a |
| `chain-closure` | **12.2 s** | 49.1 s | 56.5 s | 55.4 s | 33.3 s |
| `chain-left` | **8.96 ms** | 40.9 s | 1.58 s | 45.6 s | 19.3 s |
| `chain-right` | **9.94 s** | 41.4 s | 43.2 s | 46.2 s | 19.3 s |
| `cspa` | **272 ms** | 552 ms | timeout | 895 ms | n/a |
| `degree-count` | 240 ms | 474 ms | 481 ms | 521 ms | **142 ms** |
| `dist-bounded` | **1.75 s** | 4.69 s | 6.13 s | 6.19 s | 6.94 s |
| `facts-join` | 892 ms | 1.62 s | 1.63 s | 1.68 s | **191 ms** |
| `files-grouped` | 868 ms | 2.62 s | 3.5 s | 3.69 s | **76 ms** |
| `graph-left` | **268 ms** | timeout | 367 ms | timeout | timeout |
| `graph-reach` | 214 ms | 272 ms | 277 ms | 310 ms | **167 ms** |
| `grid-reach` | **1.17 s** | 1.57 s | 1.61 s | 1.66 s | 6.62 s |
| `sg-acyclic` | **2.5 s** | 4.95 s | 6.65 s | 7.26 s | 15.7 s |
| `sg-acyclic-bf` | **14.3 ms** | 2.93 s | 20.3 ms | 4.87 s | 12.5 s |
| `sg-cyclic` | **14.8 s** | 19.1 s | 30.3 s | 28.5 s | 81.6 s |
| `startup` | **1.43 ms** | 3.34 ms | 3.02 ms | 24.3 ms | 12.2 ms |
| `tasks-ready` | **302 ms** | 463 ms | 449 ms | 619 ms | 379 ms |
| `tc-acyclic` | 5.95 s | **5.5 s** | 9.63 s | 14.1 s | 36.3 s |
| `tc-acyclic-bf` | 1.46 s | 3.14 s | **466 ms** | 11.6 s | 32.6 s |
| `tc-acyclic-fb` | **69.9 ms** | 15.2 s | 155 ms | 19.9 s | 32.8 s |
| `tc-cyclic` | 22.5 s | **13.6 s** | 27.1 s | 42.5 s | 122 s |
| `tc-cyclic-bf` | 27 s | 8.37 s | **8.25 s** | 29.8 s | 114 s |
| `tc-cyclic-fb` | **77.2 ms** | 73.5 s | 169 ms | 95.2 s | 114 s |
| `tc-nonlinear` | **421 ms** | 784 ms | 1.18 s | 1.66 s | n/a |
| `tc-sparse-acyclic` | **201 ms** | 542 ms | 622 ms | 756 ms | 860 ms |
| `tc-sparse-cyclic` | **5.35 s** | 8.46 s | 12.1 s | 13.5 s | 24 s |
| `tree-sg-all` | **191 s** | 287 s | timeout | timeout | timeout |
| `tree-sg-bound` | 407 ms | timeout | **364 ms** | timeout | resource-limit |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 995 ms | 2.01 s | timeout | 3.03 s | n/a |
| `chain-closure` | 71.4 s | 104 s | 119 s | 125 s | 49.2 s |
| `chain-left` | 7.91 ms | 94.6 s | 767 ms | 112 s | 33.3 s |
| `chain-right` | 69.1 s | 97.2 s | 101 s | 117 s | 32.4 s |
| `cspa` | 2.54 s | 4.01 s | timeout | 6.89 s | n/a |
| `degree-count` | 400 ms | 503 ms | 518 ms | 603 ms | 339 ms |
| `dist-bounded` | 10.4 s | 10.8 s | 12.2 s | 17.6 s | 7.04 s |
| `facts-join` | 870 ms | 1.63 s | 1.65 s | 1.76 s | 552 ms |
| `files-grouped` | 1.68 s | 25.9 s | 40.8 s | 40.6 s | 481 ms |
| `graph-left` | 279 ms | timeout | 369 ms | timeout | timeout |
| `graph-reach` | 290 ms | 429 ms | 433 ms | 505 ms | 408 ms |
| `grid-reach` | 1.14 s | 4.43 s | 4.46 s | 4.5 s | 77.1 s |
| `sg-acyclic` | 24.4 s | 31.2 s | 45.5 s | 49.7 s | 30.5 s |
| `sg-acyclic-bf` | 23.8 ms | 29.2 s | 26.3 ms | 45.8 s | 26.1 s |
| `sg-cyclic` | 185 s | 182 s | 312 s | 282 s | 149 s |
| `startup` | 578 us | 1.65 ms | 1.49 ms | 11.9 ms | 9.5 ms |
| `tasks-ready` | 888 ms | 1.76 s | 1.64 s | 2.78 s | 974 ms |
| `tc-acyclic` | 71.6 s | 33 s | 78 s | 154 s | 75.2 s |
| `tc-acyclic-bf` | 18 s | 31 s | 2.91 s | 134 s | 76 s |
| `tc-acyclic-fb` | 73.4 ms | 159 s | 200 ms | 232 s | 79.1 s |
| `tc-cyclic` | 279 s | 101 s | 262 s | 463 s | 263 s |
| `tc-cyclic-bf` | 352 s | 95.5 s | 94.8 s | 437 s | 266 s |
| `tc-cyclic-fb` | 75.6 ms | 942 s | 208 ms | 1.32e+03 s | 290 s |
| `tc-nonlinear` | 3.99 s | 5.44 s | 8.52 s | 16.6 s | n/a |
| `tc-sparse-acyclic` | 1.03 s | 1.06 s | 1.21 s | 1.88 s | 1.17 s |
| `tc-sparse-cyclic` | 47.3 s | 20.4 s | 37.2 s | 55 s | 38 s |
| `tree-sg-all` | 1.01e+03 s | 403 s | timeout | timeout | timeout |
| `tree-sg-bound` | 394 ms | timeout | 354 ms | timeout | resource-limit |

**Maximum peak resident memory across timed runs**

| Workload | relic | Souffle | SF Magic | SF interp | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 74.4 MiB | 45 MiB | timeout | 49.1 MiB | n/a |
| `chain-closure` | 2.95 GiB | 1.16 GiB | 3.43 GiB | 1.16 GiB | 16.7 GiB |
| `chain-left` | 4.58 MiB | 1.16 GiB | 5.03 MiB | 1.16 GiB | 9.96 GiB |
| `chain-right` | 3.42 GiB | 1.16 GiB | 1.16 GiB | 1.16 GiB | 9.99 GiB |
| `cspa` | 185 MiB | 48.3 MiB | timeout | 50.1 MiB | n/a |
| `degree-count` | 159 MiB | 41.6 MiB | 41.8 MiB | 45.5 MiB | 237 MiB |
| `dist-bounded` | 2.56 GiB | 909 MiB | 1.24 GiB | 913 MiB | 3.36 GiB |
| `facts-join` | 244 MiB | 298 MiB | 298 MiB | 302 MiB | 236 MiB |
| `files-grouped` | 585 MiB | 320 MiB | 344 MiB | 324 MiB | 164 MiB |
| `graph-left` | 69.8 MiB | timeout | 31.9 MiB | timeout | timeout |
| `graph-reach` | 69.9 MiB | 29.8 MiB | 29.9 MiB | 33.7 MiB | 86.7 MiB |
| `grid-reach` | 226 MiB | 166 MiB | 166 MiB | 171 MiB | 430 MiB |
| `sg-acyclic` | 1.93 GiB | 490 MiB | 938 MiB | 494 MiB | 4.1 GiB |
| `sg-acyclic-bf` | 6.84 MiB | 490 MiB | 4.89 MiB | 494 MiB | 3.82 GiB |
| `sg-cyclic` | 7.69 GiB | 1.45 GiB | 2.77 GiB | 1.45 GiB | 15.4 GiB |
| `startup` | 2.06 MiB | 2.61 MiB | 2.62 MiB | 18.7 MiB | 24 MiB |
| `tasks-ready` | 196 MiB | 70.8 MiB | 77.8 MiB | 74.6 MiB | 345 MiB |
| `tc-acyclic` | 5.68 GiB | 669 MiB | 1.13 GiB | 624 MiB | 3.5 GiB |
| `tc-acyclic-bf` | 690 MiB | 620 MiB | 111 MiB | 623 MiB | 2.67 GiB |
| `tc-acyclic-fb` | 30.1 MiB | 669 MiB | 15.8 MiB | 627 MiB | 2.7 GiB |
| `tc-cyclic` | 23.5 GiB | 1.49 GiB | 2.62 GiB | 1.49 GiB | 7.42 GiB |
| `tc-cyclic-bf` | 23.7 GiB | 1.49 GiB | 1.49 GiB | 1.49 GiB | 5.48 GiB |
| `tc-cyclic-fb` | 27.4 MiB | 1.49 GiB | 15.1 MiB | 1.5 GiB | 5.48 GiB |
| `tc-nonlinear` | 662 MiB | 83.8 MiB | 135 MiB | 87.6 MiB | n/a |
| `tc-sparse-acyclic` | 208 MiB | 67.6 MiB | 114 MiB | 59.8 MiB | 442 MiB |
| `tc-sparse-cyclic` | 4.16 GiB | 1.25 GiB | 2.26 GiB | 1.16 GiB | 6.19 GiB |
| `tree-sg-all` | 109 GiB | 42.5 GiB | timeout | timeout | timeout |
| `tree-sg-bound` | 135 MiB | timeout | 118 MiB | timeout | resource-limit |

</details>

## Raw results and reproduction

| Artifact | Contents |
| --- | --- |
| Canonical results (relic-bench `m3-max/results.tsv`) | All 1,610 cells; median, fastest, spread, user CPU, peak bytes, row counts, compilation time and requested/accepted repetitions |
| [Comparison summary](bench/compare/suite-summary.tsv) | Fastest completed competitor for every workload and scenario |
| Raw cohort reports (relic-bench `m3-max/`) | Original per-scenario TSVs, text reports, exit statuses and diagnostics |
| Individual timed samples (relic-bench `m3-max/samples/`) | Every original timing sample in hyperfine JSON |
| Retry samples (relic-bench `m3-max/retry-samples/`) | Fresh reference measurements; original failures remain in the cohort reports |
| Investigated failures (relic-bench `m3-max/investigated-failures.json`) | Cell-specific diagnostic evidence and classifications |
| Environment (relic-bench `m3-max/environment.json`) | Hardware, versions, flags, resource allowances and interruption provenance |
| [Build instructions](bench/compare/tools/README.md) | Native engine builds and patches |
| Execution counters (relic-bench `m3-max/baseline.txt`) | One instrumented relic execution per workload/thread count at scale 1 |
| Earlier Xeon study (relic-bench `xeon-w3245/README.md`) | Separate sessions preserved with their conditions |

The results directories contain measurements and diagnostics; query answer dumps and temporary engine binaries are generated rather than committed.

Regenerate and validate this report without running engines. The recorded evidence is the relic-bench repository, a sibling checkout of this one; set `RELIC_BENCH` to use another location:

```sh
python3 bench/compare/tools/render-mac-results.py --check
python3 bench/compare/tools/render-mac-results.py
```

The recorded tables use file exports for Souffle/Nemo. The comparison harness serializes timed answers to discarded stdout for every engine. Preserve measurements from these output policies in separate cohorts; rerunning these commands does not reproduce the recorded file-output policy.

Run with the documented engines installed (use a fresh artifacts directory):

```sh
RELIC=/path/to/relic BENCH_SCALE=1 BENCH_RUNS=10 BENCH_THREADS=1 \
    BENCH_ARTIFACTS=/path/to/fresh-artifacts bench/compare/run.sh -o single.tsv

RELIC=/path/to/relic BENCH_SCALE=1 BENCH_RUNS=10 BENCH_THREADS=16 \
    BENCH_ARTIFACTS=/path/to/fresh-artifacts bench/compare/run.sh \
    -e souffle-interp,souffle,souffle-magic,duckdb -o parallel.tsv
```

Include `-w cspa-httpd,cspa-postgresql,cspa-linux` in separate scale-1 invocations for the downloaded datasets. Repeat generated workloads at scales 2, 4 and 8 with `BENCH_RUNS=3`. See [methodology](specs/design-bench.md).
