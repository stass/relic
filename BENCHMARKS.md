# Benchmark results

AMD Ryzen Threadripper PRO 5995WX: **64 physical cores, 128 hardware threads, 256 GiB installed RAM**.
OS: FreeBSD 16.0-CURRENT. Parallel runs request 64 workers; placement follows the OS scheduler.
Source revision: `9287f6801c39182db8871d29c58752cffcb2d852`.

Rival timings on every workload except `chain-closure` and `tree-sg-all` come from an earlier run on this host with the same engine versions, builds and conditions, started 2026-11-05T02:59:15Z at revision `9a1ed59d7b59888f933b58ccfac87bf8716e798e`. For each of those timings relic's answer is the same set at both revisions, so the carried answer was checked against relic. All relic timings, and every engine's on the rerun workloads, come from the run started 2026-11-07T10:24:59Z at the revision above.

## Comparison overview

| Scale | Threads | Repetitions | relic lowest median / comparable workloads | Rival cells without a completed time |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 1 | 10 | 25 / 31 | 18 |
| 1 | 64 | 10 | 31 / 31 | 5 |
| 2 | 1 | 3 | 22 / 28 | 16 |
| 2 | 64 | 3 | 26 / 28 | 2 |
| 4 | 1 | 3 | 22 / 28 | 30 |
| 4 | 64 | 3 | 26 / 28 | 4 |
| 8 | 1 | 3 | 20 / 28 | 54 |
| 8 | 64 | 3 | 25 / 28 | 8 |

## Method and limits

Scale 1 uses ten repetitions and all 31 workloads, including three fixed Graspan datasets. Scales 2, 4 and 8 use three repetitions and the 28 generated workloads. Scaling follows each generator; the small tree of `tree-sg-all` has 1024 * scale - 1 nodes, while the tree of `tree-sg-bound` has depth 11 + scale, so its size grows exponentially.

Every engine's complete answer is checked as an exact set against relic before timing. Times include process startup, input loading, evaluation and serialization of the complete answer to discarded stdout for every engine. Verification and compilation are excluded. User CPU time sums across threads. Peak memory is the maximum observed across timed runs. Small timing differences should be read alongside the recorded spread.

relic is configured with `--disable-shared --enable-threads` and `CFLAGS='-O3 -g'`. Compiler: FreeBSD clang 19.1.7. Generated Souffle programs use `-O3` and OpenMP.

Engine versions: relic 1.0.0; SWI-Prolog version 10.0.2; souffle 2.5; clingo version 5.8.0; nemo-cli 0.10.1; duckdb v1.5.6; sqlite 3.54.1; hyperfine 2.0.0; FreeBSD clang version 19.1.7.

`timeout` means verification exceeded 300 seconds; it is not a timed 300-second result. `n/a` means no supported program; `no-reference` means relic did not finish verification. `engine-limit` denotes an investigated engine capacity limit; `resource-limit` denotes investigated memory exhaustion. Unavailable outcomes are excluded from rankings. Bold marks the lowest observed median without asserting statistical significance. A comparable workload has a completed relic time and at least one completed rival time.

The machine is idle at launch apart from the benchmark driver and background OS work. Build logs, executable hashes, start/end context, exact source revision and raw samples are retained with the measurement cohort. CPU frequency and thermal control follow the host OS. These results describe this host; operating systems, compilers, engine versions and output policy differ from the Apple M3 Max study, so cross-host differences cannot be attributed solely to hardware.

## Engine builds

No explicit whole-program LTO is enabled for any engine. Each engine retains its documented release-build defaults; optimization levels, CPU targeting and SWI-Prolog PGO are recorded rather than treated as identical compiler settings. Nemo retains Cargo release defaults, including any within-crate compiler optimization.

| Engine | Build settings |
| --- | --- |
| relic | `--disable-shared --enable-threads`, `CFLAGS=-O3 -g`; ThinLTO disabled; default CPU target |
| Souffle interpreter | Release build, `-O3`, OpenMP, 32-bit domains, runtime assertions enabled; no LTO; default CPU target |
| Souffle generated programs | `-std=c++17 -O3 -DRAM_DOMAIN_SIZE=32`; `-fopenmp` at 64 workers; no LTO; default CPU target |
| Nemo | Locked Cargo release build with upstream release profile; no explicit cross-crate LTO |
| SWI-Prolog | FreeBSD port PGO build with native CPU targeting; no explicit LTO |
| clingo | FreeBSD port release defaults with native CPU targeting; no explicit LTO |
| DuckDB | FreeBSD port release defaults with native CPU targeting; no explicit LTO; default memory allowance |
| SQLite | `--disable-shared`, `CFLAGS=-O3 -g -march=native`; no LTO; upstream 3.54.1 source |

## Full comparisons

### Scale 1, 1 thread

**Median wall time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **82 ms** | timeout | 208 ms | 141 ms | timeout | 443 ms | 698 ms | n/a | n/a |
| `chain-closure` | **321 ms** | 2.68 s | 977 ms | 663 ms | 851 ms | 2.25 s | 110 s | 1.21 s | 10.1 s |
| `chain-left` | 2.82 ms | 29.8 ms | 613 ms | 419 ms | **1.96 ms** | 1.46 s | 111 s | 690 ms | 8.99 s |
| `chain-right` | **2.99 ms** | 457 ms | 675 ms | 470 ms | 573 ms | 1.46 s | 98.5 s | 722 ms | 9 s |
| `cspa` | **161 ms** | timeout | 522 ms | 321 ms | 197 s | 707 ms | 1.21 s | n/a | n/a |
| `cspa-httpd` | 101 s | timeout | 151 s | **92.3 s** | timeout | timeout | timeout | n/a | n/a |
| `cspa-linux` | **26.9 s** | timeout | 54.7 s | 36.9 s | timeout | 119 s | 89.4 s | n/a | n/a |
| `cspa-postgresql` | **106 s** | timeout | 176 s | 114 s | timeout | timeout | timeout | n/a | n/a |
| `degree-count` | **47.2 ms** | 959 ms | 87.3 ms | 61.9 ms | 60.6 ms | 590 ms | 253 ms | 72.3 ms | 201 ms |
| `dist-bounded` | **382 ms** | 3.5 s | 1.36 s | 978 ms | 1.06 s | 2.44 s | 2.48 s | 656 ms | 2.77 s |
| `facts-join` | 126 ms | 2.86 s | 203 ms | 163 ms | 161 ms | 1.63 s | 680 ms | **115 ms** | 171 ms |
| `files-grouped` | 150 ms | 2.81 s | 1.57 s | 693 ms | 1.33 s | n/a | 1.44 s | **105 ms** | 295 ms |
| `graph-left` | **38.7 ms** | 906 ms | timeout | 267 s | 47.1 ms | timeout | timeout | timeout | timeout |
| `graph-reach` | **37.4 ms** | 890 ms | 62.6 ms | 43.7 ms | 43.5 ms | 456 ms | 142 ms | 91.9 ms | 178 ms |
| `grid-reach` | 18 ms | 417 ms | 30.7 ms | **17.7 ms** | 18.3 ms | 214 ms | 157 ms | 464 ms | 81.8 ms |
| `sg-acyclic` | **285 ms** | 2.17 s | 835 ms | 619 ms | 927 ms | 1.86 s | 3.11 s | 455 ms | 3.3 s |
| `sg-acyclic-bf` | **2.7 ms** | 52.1 ms | 711 ms | 492 ms | 3.12 ms | 1.58 s | 3.04 s | 343 ms | 3.09 s |
| `sg-cyclic` | **762 ms** | 8.99 s | 2.91 s | 2.13 s | 3.67 s | 8.63 s | 14.6 s | 1.66 s | 14.5 s |
| `startup` | **720 us** | 10.1 ms | 4.96 ms | 1.04 ms | 1.06 ms | 1.55 ms | 1.82 ms | 22.9 ms | 1.08 ms |
| `tasks-ready` | **51.1 ms** | 1.39 s | 142 ms | 88.6 ms | 97.1 ms | 633 ms | 197 ms | 123 ms | 68.3 s |
| `tc-acyclic` | **368 ms** | 3.67 s | 787 ms | 477 ms | 997 ms | 3.4 s | 6.79 s | 549 ms | 14.4 s |
| `tc-acyclic-bf` | **9.85 ms** | 513 ms | 672 ms | 350 ms | 51 ms | 3.16 s | 6.8 s | 441 ms | 14.1 s |
| `tc-acyclic-fb` | **9.41 ms** | 425 ms | 1.28 s | 902 ms | 22.6 ms | 3.14 s | 6.75 s | 441 ms | 14.1 s |
| `tc-cyclic` | **986 ms** | 23.8 s | 2.25 s | 1.27 s | 3.26 s | 13.2 s | 34.5 s | 2.65 s | 70.1 s |
| `tc-cyclic-bf` | **12.9 ms** | 17.8 s | 1.97 s | 973 ms | 1.01 s | 12.8 s | 34.3 s | 2.44 s | 69.3 s |
| `tc-cyclic-fb` | **11.2 ms** | 429 ms | 5.47 s | 4.22 s | 24.7 ms | 12.7 s | 34.4 s | 2.44 s | 69.4 s |
| `tc-nonlinear` | **102 ms** | 747 ms | 296 ms | 188 ms | 291 ms | 569 ms | 1.1 s | n/a | n/a |
| `tc-sparse-acyclic` | **32.1 ms** | 262 ms | 83.1 ms | 61.2 ms | 69.1 ms | 210 ms | 151 ms | 80.2 ms | 291 ms |
| `tc-sparse-cyclic` | **270 ms** | 3.48 s | 653 ms | 465 ms | 669 ms | 2.31 s | 2.04 s | 547 ms | 8.68 s |
| `tree-sg-all` | **31.8 ms** | 366 ms | 63.2 ms | 32.3 ms | 32.7 ms | 177 ms | 189 ms | 91.7 ms | 305 ms |
| `tree-sg-bound` | 5.03 ms | 89.8 ms | 1.95 s | 851 ms | **4 ms** | 8.94 s | 8.39 s | 2.35 s | 18.7 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 81.1 ms | timeout | 204 ms | 140 ms | timeout | 438 ms | 689 ms | n/a | n/a |
| `chain-closure` | 313 ms | 2.61 s | 970 ms | 658 ms | 843 ms | 2.16 s | 110 s | 1.18 s | 5.28 s |
| `chain-left` | 526 us | 26.5 ms | 601 ms | 415 ms | 414 us | 1.37 s | 111 s | 726 ms | 4.34 s |
| `chain-right` | 857 us | 369 ms | 665 ms | 460 ms | 567 ms | 1.38 s | 99.2 s | 691 ms | 4.28 s |
| `cspa` | 157 ms | timeout | 522 ms | 321 ms | 197 s | 699 ms | 1.21 s | n/a | n/a |
| `cspa-httpd` | 97.6 s | timeout | 150 s | 91 s | timeout | timeout | timeout | n/a | n/a |
| `cspa-linux` | 26.1 s | timeout | 54.5 s | 36.7 s | timeout | 115 s | 88 s | n/a | n/a |
| `cspa-postgresql` | 103 s | timeout | 174 s | 112 s | timeout | timeout | timeout | n/a | n/a |
| `degree-count` | 46.4 ms | 900 ms | 83.4 ms | 60.4 ms | 59.1 ms | 585 ms | 252 ms | 143 ms | 190 ms |
| `dist-bounded` | 376 ms | 3.26 s | 1.35 s | 967 ms | 1.05 s | 2.37 s | 2.45 s | 628 ms | 2.64 s |
| `facts-join` | 120 ms | 2.7 s | 189 ms | 159 ms | 152 ms | 1.62 s | 664 ms | 139 ms | 171 ms |
| `files-grouped` | 139 ms | 2.68 s | 1.56 s | 688 ms | 1.32 s | n/a | 1.43 s | 160 ms | 282 ms |
| `graph-left` | 37.2 ms | 860 ms | timeout | 264 s | 47.2 ms | timeout | timeout | timeout | timeout |
| `graph-reach` | 35.9 ms | 839 ms | 57.8 ms | 40.7 ms | 42.1 ms | 453 ms | 141 ms | 128 ms | 168 ms |
| `grid-reach` | 17.4 ms | 393 ms | 26 ms | 16 ms | 18.3 ms | 211 ms | 156 ms | 493 ms | 80.7 ms |
| `sg-acyclic` | 284 ms | 2.14 s | 830 ms | 616 ms | 924 ms | 1.85 s | 3.08 s | 501 ms | 3.06 s |
| `sg-acyclic-bf` | 752 us | 47.4 ms | 709 ms | 489 ms | 944 us | 1.57 s | 3 s | 329 ms | 2.83 s |
| `sg-cyclic` | 756 ms | 8.93 s | 2.92 s | 2.13 s | 3.67 s | 8.69 s | 14.5 s | 1.64 s | 11.1 s |
| `startup` | 57.5 us | 9.01 ms | 985 us | 104 us | 236 us | 272 us | 171 us | 76.1 ms | 132 us |
| `tasks-ready` | 49.7 ms | 1.29 s | 139 ms | 86.9 ms | 96.8 ms | 624 ms | 195 ms | 197 ms | 68.2 s |
| `tc-acyclic` | 363 ms | 3.63 s | 786 ms | 474 ms | 994 ms | 3.39 s | 6.7 s | 607 ms | 7.57 s |
| `tc-acyclic-bf` | 9.22 ms | 479 ms | 671 ms | 348 ms | 48.5 ms | 3.16 s | 6.66 s | 453 ms | 7.2 s |
| `tc-acyclic-fb` | 8.93 ms | 404 ms | 1.29 s | 900 ms | 21.8 ms | 3.14 s | 6.65 s | 433 ms | 7.32 s |
| `tc-cyclic` | 986 ms | 23.7 s | 2.23 s | 1.27 s | 3.25 s | 13.3 s | 33.9 s | 2.7 s | 28.3 s |
| `tc-cyclic-bf` | 12.3 ms | 17.8 s | 1.96 s | 968 ms | 1.01 s | 12.8 s | 33.9 s | 2.4 s | 27.6 s |
| `tc-cyclic-fb` | 10.6 ms | 410 ms | 5.45 s | 4.22 s | 24.6 ms | 12.7 s | 33.8 s | 2.38 s | 27.7 s |
| `tc-nonlinear` | 98.7 ms | 730 ms | 291 ms | 187 ms | 290 ms | 565 ms | 1.09 s | n/a | n/a |
| `tc-sparse-acyclic` | 30.4 ms | 232 ms | 80.9 ms | 61.2 ms | 67.6 ms | 199 ms | 147 ms | 127 ms | 290 ms |
| `tc-sparse-cyclic` | 266 ms | 3.42 s | 650 ms | 461 ms | 666 ms | 2.28 s | 2.03 s | 507 ms | 4.37 s |
| `tree-sg-all` | 30.1 ms | 365 ms | 60.9 ms | 31.5 ms | 31.9 ms | 170 ms | 189 ms | 121 ms | 301 ms |
| `tree-sg-bound` | 1.99 ms | 82.4 ms | 1.92 s | 814 ms | 1.59 ms | 8.47 s | 8.37 s | 2.13 s | 18 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 11.3 MiB | timeout | 14.7 MiB | 8.27 MiB | timeout | 35.7 MiB | 22.5 MiB | n/a | n/a |
| `chain-closure` | 51 MiB | 638 MiB | 30.4 MiB | 23.1 MiB | 41.2 MiB | 438 MiB | 58.1 MiB | 491 MiB | 7.71 MiB |
| `chain-left` | 3.42 MiB | 14.4 MiB | 30.3 MiB | 23.3 MiB | 3.4 MiB | 332 MiB | 77.6 MiB | 213 MiB | 7.71 MiB |
| `chain-right` | 3.41 MiB | 286 MiB | 30.3 MiB | 23.3 MiB | 23.3 MiB | 334 MiB | 59.1 MiB | 213 MiB | 7.7 MiB |
| `cspa` | 14.1 MiB | timeout | 14.8 MiB | 8.37 MiB | 944 MiB | 49.5 MiB | 32.2 MiB | n/a | n/a |
| `cspa-httpd` | 8.44 GiB | timeout | 3.99 GiB | 3.77 GiB | timeout | timeout | timeout | n/a | n/a |
| `cspa-linux` | 2.29 GiB | timeout | 894 MiB | 866 MiB | timeout | 10.4 GiB | 3.33 GiB | n/a | n/a |
| `cspa-postgresql` | 8.95 GiB | timeout | 4.59 GiB | 4.37 GiB | timeout | timeout | timeout | n/a | n/a |
| `degree-count` | 8.79 MiB | 44.5 MiB | 14.2 MiB | 7.83 MiB | 7.82 MiB | 52.1 MiB | 16.1 MiB | 58.6 MiB | 18.6 MiB |
| `dist-bounded` | 44.7 MiB | 388 MiB | 61.5 MiB | 52.9 MiB | 66.8 MiB | 298 MiB | 143 MiB | 209 MiB | 9.97 MiB |
| `facts-join` | 29.3 MiB | 117 MiB | 43.1 MiB | 37.4 MiB | 37.4 MiB | 68.7 MiB | 53.9 MiB | 65.9 MiB | 13.1 MiB |
| `files-grouped` | 26 MiB | 132 MiB | 44.6 MiB | 39.3 MiB | 42.2 MiB | n/a | 43.9 MiB | 65.3 MiB | 17.7 MiB |
| `graph-left` | 8.05 MiB | 37.9 MiB | timeout | 7.74 GiB | 6.73 MiB | timeout | timeout | timeout | timeout |
| `graph-reach` | 7.87 MiB | 37.9 MiB | 12.8 MiB | 6.56 MiB | 6.57 MiB | 27.8 MiB | 15 MiB | 55.1 MiB | 10.3 MiB |
| `grid-reach` | 6.44 MiB | 27.9 MiB | 12.2 MiB | 5.78 MiB | 5.8 MiB | 18 MiB | 14.2 MiB | 61 MiB | 7.94 MiB |
| `sg-acyclic` | 16.1 MiB | 142 MiB | 20.9 MiB | 14.1 MiB | 16.1 MiB | 116 MiB | 142 MiB | 133 MiB | 10.1 MiB |
| `sg-acyclic-bf` | 3.39 MiB | 14.6 MiB | 20.9 MiB | 14.1 MiB | 3.69 MiB | 89.9 MiB | 145 MiB | 122 MiB | 10.1 MiB |
| `sg-cyclic` | 28.7 MiB | 275 MiB | 34.4 MiB | 26.7 MiB | 27.6 MiB | 197 MiB | 613 MiB | 318 MiB | 10.1 MiB |
| `startup` | 3.11 MiB | 13.4 MiB | 9.53 MiB | 3.2 MiB | 3.4 MiB | 6.24 MiB | 10.8 MiB | 48.9 MiB | 4.67 MiB |
| `tasks-ready` | 10.6 MiB | 76.1 MiB | 15.1 MiB | 8.73 MiB | 8.96 MiB | 45 MiB | 17.6 MiB | 68.7 MiB | 12.8 MiB |
| `tc-acyclic` | 16.8 MiB | 155 MiB | 20.6 MiB | 13.9 MiB | 14 MiB | 110 MiB | 470 MiB | 109 MiB | 11.3 MiB |
| `tc-acyclic-bf` | 4.73 MiB | 34.8 MiB | 20.6 MiB | 13.9 MiB | 5.83 MiB | 88 MiB | 474 MiB | 95.5 MiB | 11.4 MiB |
| `tc-acyclic-fb` | 4.73 MiB | 28.3 MiB | 20.9 MiB | 14.1 MiB | 4.99 MiB | 88 MiB | 474 MiB | 95.5 MiB | 11.4 MiB |
| `tc-cyclic` | 28.7 MiB | 304 MiB | 34.3 MiB | 27 MiB | 27 MiB | 199 MiB | 1.96 GiB | 184 MiB | 11.4 MiB |
| `tc-cyclic-bf` | 4.79 MiB | 119 MiB | 34.3 MiB | 27 MiB | 27 MiB | 170 MiB | 1.96 GiB | 153 MiB | 11.4 MiB |
| `tc-cyclic-fb` | 4.79 MiB | 28.8 MiB | 34.6 MiB | 27.2 MiB | 4.94 MiB | 170 MiB | 1.96 GiB | 153 MiB | 11.4 MiB |
| `tc-nonlinear` | 9.44 MiB | 52.3 MiB | 13 MiB | 6.55 MiB | 8.11 MiB | 34.7 MiB | 71.3 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 8.31 MiB | 53.8 MiB | 12.2 MiB | 5.65 MiB | 6.39 MiB | 33.6 MiB | 17.5 MiB | 65.2 MiB | 7.68 MiB |
| `tc-sparse-cyclic` | 27.8 MiB | 276 MiB | 28.7 MiB | 21.4 MiB | 23.8 MiB | 198 MiB | 103 MiB | 158 MiB | 9.95 MiB |
| `tree-sg-all` | 8.49 MiB | 47.1 MiB | 13.3 MiB | 6.74 MiB | 6.88 MiB | 42.1 MiB | 16.4 MiB | 75.4 MiB | 9.99 MiB |
| `tree-sg-bound` | 4.05 MiB | 16.3 MiB | 197 MiB | 182 MiB | 4.45 MiB | 1.55 GiB | 171 MiB | 1.17 GiB | 10.4 MiB |

</details>

### Scale 1, 64 threads

**Median wall time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **87.5 ms** | 109 ms | 93.3 ms | timeout | n/a |
| `chain-closure` | **323 ms** | 2.2 s | 2.02 s | 2.17 s | 1.87 s |
| `chain-left` | **2.87 ms** | 1.81 s | 1.63 s | 28 ms | 1.42 s |
| `chain-right` | **3.08 ms** | 1.72 s | 1.55 s | 1.6 s | 1.42 s |
| `cspa` | **97.5 ms** | 152 ms | 117 ms | 11.9 s | n/a |
| `cspa-httpd` | **21.3 s** | 36.3 s | 23.9 s | timeout | n/a |
| `cspa-linux` | **8.28 s** | 11 s | 8.97 s | timeout | n/a |
| `cspa-postgresql` | **24.6 s** | 42.4 s | 29.3 s | timeout | n/a |
| `degree-count` | **58.7 ms** | 95.4 ms | 86.6 ms | 85.8 ms | 89 ms |
| `dist-bounded` | **294 ms** | 969 ms | 942 ms | 997 ms | 841 ms |
| `facts-join` | **104 ms** | 226 ms | 218 ms | 220 ms | 116 ms |
| `files-grouped` | **74.1 ms** | 185 ms | 167 ms | 199 ms | 87.6 ms |
| `graph-left` | **47.6 ms** | 46.5 s | 29.4 s | 70.8 ms | timeout |
| `graph-reach` | **43.4 ms** | 74 ms | 66.3 ms | 67.3 ms | 109 ms |
| `grid-reach` | **18 ms** | 112 ms | 100 ms | 106 ms | 585 ms |
| `sg-acyclic` | **146 ms** | 266 ms | 244 ms | 291 ms | 528 ms |
| `sg-acyclic-bf` | **2.67 ms** | 87.4 ms | 64.1 ms | 15.6 ms | 412 ms |
| `sg-cyclic` | **189 ms** | 519 ms | 476 ms | 583 ms | 1.81 s |
| `startup` | **762 us** | 4.98 ms | 1.9 ms | 2.01 ms | 25.5 ms |
| `tasks-ready` | **65.6 ms** | 85.4 ms | 74.2 ms | 76.9 ms | 155 ms |
| `tc-acyclic` | **155 ms** | 257 ms | 247 ms | 283 ms | 724 ms |
| `tc-acyclic-bf` | **12.6 ms** | 90.2 ms | 63 ms | 40.6 ms | 605 ms |
| `tc-acyclic-fb` | **12 ms** | 134 ms | 118 ms | 33 ms | 614 ms |
| `tc-cyclic` | **226 ms** | 519 ms | 495 ms | 583 ms | 2.67 s |
| `tc-cyclic-bf` | **18.2 ms** | 161 ms | 103 ms | 107 ms | 2.53 s |
| `tc-cyclic-fb` | **12.9 ms** | 305 ms | 274 ms | 37.5 ms | 2.51 s |
| `tc-nonlinear` | **54.4 ms** | 92 ms | 81.1 ms | 107 ms | n/a |
| `tc-sparse-acyclic` | **35.1 ms** | 93.3 ms | 87.6 ms | 90.8 ms | 106 ms |
| `tc-sparse-cyclic` | **183 ms** | 479 ms | 468 ms | 504 ms | 618 ms |
| `tree-sg-all` | **36.8 ms** | 79.4 ms | 74.2 ms | 77.3 ms | 122 ms |
| `tree-sg-bound` | **5.07 ms** | 839 ms | 417 ms | 18.2 ms | 2.27 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 877 ms | 4.69 s | 4.1 s | timeout | n/a |
| `chain-closure` | 312 ms | 121 s | 110 s | 112 s | 2.65 s |
| `chain-left` | 823 us | 114 s | 102 s | 1.05 s | 2.52 s |
| `chain-right` | 1.19 ms | 108 s | 97.3 s | 101 s | 2.6 s |
| `cspa` | 1.19 s | 7.66 s | 6.12 s | 575 s | n/a |
| `cspa-httpd` | 263 s | 631 s | 603 s | timeout | n/a |
| `cspa-linux` | 59.5 s | 193 s | 161 s | timeout | n/a |
| `cspa-postgresql` | 265 s | 776 s | 729 s | timeout | n/a |
| `degree-count` | 146 ms | 961 ms | 1.01 s | 929 ms | 122 ms |
| `dist-bounded` | 1.23 s | 25.3 s | 21 s | 21 s | 960 ms |
| `facts-join` | 336 ms | 1.62 s | 1.61 s | 1.62 s | 202 ms |
| `files-grouped` | 474 ms | 4.07 s | 3.39 s | 5.18 s | 201 ms |
| `graph-left` | 141 ms | 568 s | 481 s | 1.32 s | timeout |
| `graph-reach` | 144 ms | 1.05 s | 872 ms | 925 ms | 212 ms |
| `grid-reach` | 18 ms | 4.94 s | 4.72 s | 4.85 s | 971 ms |
| `sg-acyclic` | 1.03 s | 15.7 s | 14.7 s | 16.3 s | 617 ms |
| `sg-acyclic-bf` | 738 us | 4.47 s | 3.29 s | 121 ms | 697 ms |
| `sg-cyclic` | 2.1 s | 22 s | 19.4 s | 21.8 s | 2.77 s |
| `startup` | 72.3 us | 2.02 ms | 374 us | 675 us | 60 ms |
| `tasks-ready` | 307 ms | 1.92 s | 1.56 s | 1.38 s | 281 ms |
| `tc-acyclic` | 1.32 s | 14.1 s | 13.9 s | 16.3 s | 877 ms |
| `tc-acyclic-bf` | 38 ms | 3.61 s | 2.19 s | 1.04 s | 1.04 s |
| `tc-acyclic-fb` | 49.7 ms | 5.95 s | 5.36 s | 348 ms | 1.12 s |
| `tc-cyclic` | 2.52 s | 20.7 s | 17.3 s | 21.2 s | 2.85 s |
| `tc-cyclic-bf` | 63.5 ms | 7.92 s | 4.78 s | 5.13 s | 4.45 s |
| `tc-cyclic-fb` | 31 ms | 16.7 s | 15.1 s | 570 ms | 4.68 s |
| `tc-nonlinear` | 584 ms | 4.77 s | 4.36 s | 5.94 s | n/a |
| `tc-sparse-acyclic` | 300 ms | 4.88 s | 4.68 s | 5.01 s | 160 ms |
| `tc-sparse-cyclic` | 1.46 s | 19.7 s | 17.2 s | 17.8 s | 670 ms |
| `tree-sg-all` | 217 ms | 4.04 s | 4.03 s | 4.22 s | 206 ms |
| `tree-sg-bound` | 2.53 ms | 46.7 s | 25 s | 252 ms | 4.88 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 30 MiB | 29.2 MiB | 22.5 MiB | timeout | n/a |
| `chain-closure` | 51 MiB | 44.5 MiB | 39 MiB | 66.5 MiB | 526 MiB |
| `chain-left` | 3.39 MiB | 44.7 MiB | 39.3 MiB | 16.8 MiB | 311 MiB |
| `chain-right` | 3.49 MiB | 44.6 MiB | 39.1 MiB | 39.1 MiB | 313 MiB |
| `cspa` | 48.3 MiB | 31.6 MiB | 23.3 MiB | 1.44 GiB | n/a |
| `cspa-httpd` | 16.2 GiB | 5.17 GiB | 5.35 GiB | timeout | n/a |
| `cspa-linux` | 4.68 GiB | 1.01e+03 MiB | 997 MiB | timeout | n/a |
| `cspa-postgresql` | 16.6 GiB | 5.33 GiB | 5.63 GiB | timeout | n/a |
| `degree-count` | 35.9 MiB | 27.2 MiB | 21.4 MiB | 21.4 MiB | 96.6 MiB |
| `dist-bounded` | 284 MiB | 74.7 MiB | 68.8 MiB | 88.5 MiB | 258 MiB |
| `facts-join` | 75.9 MiB | 54.7 MiB | 49.3 MiB | 49.3 MiB | 75.7 MiB |
| `files-grouped` | 75.4 MiB | 58.4 MiB | 55 MiB | 58.5 MiB | 73 MiB |
| `graph-left` | 10.7 MiB | 8.73 GiB | 8.79 GiB | 20.1 MiB | timeout |
| `graph-reach` | 18.1 MiB | 25.6 MiB | 19.9 MiB | 19.9 MiB | 68.3 MiB |
| `grid-reach` | 6.59 MiB | 25.7 MiB | 20.2 MiB | 20.2 MiB | 112 MiB |
| `sg-acyclic` | 34.4 MiB | 34.6 MiB | 28.6 MiB | 29 MiB | 191 MiB |
| `sg-acyclic-bf` | 3.25 MiB | 34.5 MiB | 28.6 MiB | 16.3 MiB | 188 MiB |
| `sg-cyclic` | 66.4 MiB | 47.4 MiB | 41.5 MiB | 42.8 MiB | 368 MiB |
| `startup` | 3.1 MiB | 9.52 MiB | 4.51 MiB | 4.34 MiB | 51.6 MiB |
| `tasks-ready` | 32.9 MiB | 28.2 MiB | 22.6 MiB | 22.8 MiB | 105 MiB |
| `tc-acyclic` | 133 MiB | 33.7 MiB | 27.9 MiB | 29.1 MiB | 147 MiB |
| `tc-acyclic-bf` | 7.02 MiB | 33.7 MiB | 28 MiB | 19.6 MiB | 127 MiB |
| `tc-acyclic-fb` | 6.01 MiB | 33.8 MiB | 28.1 MiB | 18 MiB | 127 MiB |
| `tc-cyclic` | 229 MiB | 47.2 MiB | 41.6 MiB | 42.1 MiB | 221 MiB |
| `tc-cyclic-bf` | 12.6 MiB | 47.3 MiB | 41.6 MiB | 41.6 MiB | 187 MiB |
| `tc-cyclic-fb` | 6.18 MiB | 47.5 MiB | 41.9 MiB | 18.1 MiB | 187 MiB |
| `tc-nonlinear` | 39.4 MiB | 26.3 MiB | 20.2 MiB | 22.9 MiB | n/a |
| `tc-sparse-acyclic` | 25.6 MiB | 25.3 MiB | 19.7 MiB | 20.2 MiB | 89.7 MiB |
| `tc-sparse-cyclic` | 131 MiB | 43.5 MiB | 37.9 MiB | 39 MiB | 208 MiB |
| `tree-sg-all` | 35.1 MiB | 26.2 MiB | 20.4 MiB | 20.5 MiB | 109 MiB |
| `tree-sg-bound` | 4.22 MiB | 211 MiB | 204 MiB | 17.3 MiB | 1.57 GiB |

</details>

### Scale 2, 1 thread

**Median wall time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **168 ms** | timeout | 398 ms | 279 ms | timeout | 836 ms | 1.32 s | n/a | n/a |
| `chain-closure` | **1.48 s** | 11.1 s | 4.11 s | 2.85 s | 3.61 s | 9.82 s | timeout | 4.45 s | 45.5 s |
| `chain-left` | 4.15 ms | 49 ms | 2.56 s | 1.82 s | **3.12 ms** | 6.2 s | timeout | 2.38 s | 40.7 s |
| `chain-right` | **4.87 ms** | 1.68 s | 2.87 s | 2.03 s | 2.48 s | 6.21 s | timeout | 2.41 s | 40.9 s |
| `cspa` | **390 ms** | timeout | 1.18 s | 734 ms | timeout | 1.81 s | 3.03 s | n/a | n/a |
| `degree-count` | **99.5 ms** | 1.93 s | 175 ms | 126 ms | 124 ms | 1.21 s | 516 ms | 105 ms | 411 ms |
| `dist-bounded` | **1.17 s** | 9.46 s | 3.71 s | 2.72 s | 2.98 s | 7.12 s | 7.13 s | 1.97 s | 7.59 s |
| `facts-join` | 253 ms | 5.6 s | 450 ms | 367 ms | 366 ms | 3.31 s | 1.52 s | **192 ms** | 360 ms |
| `files-grouped` | 301 ms | 5.69 s | 3.73 s | 1.5 s | 2.82 s | n/a | 2.9 s | **182 ms** | 599 ms |
| `graph-left` | **81 ms** | 1.84 s | timeout | timeout | 98.4 ms | timeout | timeout | timeout | timeout |
| `graph-reach` | **77.9 ms** | 1.8 s | 123 ms | 89.2 ms | 89.5 ms | 913 ms | 287 ms | 147 ms | 531 ms |
| `grid-reach` | 72.7 ms | 1.66 s | 109 ms | **71 ms** | 73.2 ms | 857 ms | 860 ms | 2.77 s | 435 ms |
| `sg-acyclic` | **1.02 s** | 8.07 s | 3.14 s | 2.34 s | 3.47 s | 9.87 s | 12.4 s | 2 s | 15.4 s |
| `sg-acyclic-bf` | **5.21 ms** | 97.3 ms | 2.67 s | 1.85 s | 6.78 ms | 8.64 s | 12.2 s | 1.57 s | 14.5 s |
| `sg-cyclic` | **3.22 s** | 40.4 s | 12.5 s | 9.33 s | 16.4 s | 45.8 s | 77.3 s | 8.44 s | 83 s |
| `startup` | **1.21 ms** | 9.58 ms | 5.36 ms | 1.4 ms | 1.38 ms | 1.86 ms | 3.03 ms | 23.5 ms | 1.46 ms |
| `tasks-ready` | **117 ms** | 2.8 s | 359 ms | 219 ms | 230 ms | 1.34 s | 445 ms | 225 ms | 265 s |
| `tc-acyclic` | **1.44 s** | 14.3 s | 3.3 s | 1.98 s | 4.53 s | 19.4 s | 35.8 s | 4 s | 88.8 s |
| `tc-acyclic-bf` | **20.3 ms** | 1.27 s | 2.84 s | 1.49 s | 188 ms | 18.3 s | 35.4 s | 3.61 s | 87.9 s |
| `tc-acyclic-fb` | **19.5 ms** | 876 ms | 6.79 s | 5.41 s | 47.5 ms | 18.4 s | 35.5 s | 3.62 s | 88.5 s |
| `tc-cyclic` | **4.08 s** | 105 s | 11.4 s | 6.81 s | 15.9 s | 65.5 s | 166 s | 14.5 s | timeout |
| `tc-cyclic-bf` | **27.1 ms** | 82.4 s | 10.4 s | 5.82 s | 5.96 s | 63.4 s | 166 s | 13.6 s | timeout |
| `tc-cyclic-fb` | **24.1 ms** | 868 ms | 29 s | 24.3 s | 52.8 ms | 62.6 s | 166 s | 13.6 s | timeout |
| `tc-nonlinear` | **337 ms** | 2.18 s | 905 ms | 581 ms | 880 ms | 1.72 s | 3.6 s | n/a | n/a |
| `tc-sparse-acyclic` | **89.7 ms** | 683 ms | 226 ms | 171 ms | 190 ms | 597 ms | 417 ms | 178 ms | 1.32 s |
| `tc-sparse-cyclic` | **1.2 s** | 15.3 s | 2.67 s | 1.89 s | 2.82 s | 11.2 s | 8.7 s | 2.53 s | 44.3 s |
| `tree-sg-all` | 123 ms | 1.94 s | 233 ms | **121 ms** | 124 ms | 730 ms | 705 ms | 286 ms | 1.24 s |
| `tree-sg-bound` | 8.76 ms | 172 ms | 8.01 s | 3.56 s | **6.84 ms** | 38.8 s | 35.5 s | 10.6 s | 75.3 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 163 ms | timeout | 398 ms | 279 ms | timeout | 815 ms | 1.32 s | n/a | n/a |
| `chain-closure` | 1.44 s | 10.5 s | 4.09 s | 2.82 s | 3.57 s | 9.4 s | timeout | 4.12 s | 22.5 s |
| `chain-left` | 1.22 ms | 41 ms | 2.54 s | 1.8 s | 1.12 ms | 5.99 s | timeout | 2.21 s | 18.5 s |
| `chain-right` | 1.61 ms | 1.39 s | 2.85 s | 2.01 s | 2.44 s | 5.91 s | timeout | 2.42 s | 18.3 s |
| `cspa` | 388 ms | timeout | 1.18 s | 729 ms | timeout | 1.79 s | 3.03 s | n/a | n/a |
| `degree-count` | 94.1 ms | 1.82 s | 164 ms | 124 ms | 124 ms | 1.19 s | 509 ms | 92 ms | 385 ms |
| `dist-bounded` | 1.14 s | 8.79 s | 3.68 s | 2.68 s | 2.94 s | 6.95 s | 7.04 s | 1.9 s | 7.19 s |
| `facts-join` | 242 ms | 5.43 s | 432 ms | 350 ms | 353 ms | 3.29 s | 1.51 s | 249 ms | 342 ms |
| `files-grouped` | 285 ms | 5.42 s | 3.75 s | 1.49 s | 2.8 s | n/a | 2.89 s | 190 ms | 582 ms |
| `graph-left` | 78.6 ms | 1.76 s | timeout | timeout | 95.3 ms | timeout | timeout | timeout | timeout |
| `graph-reach` | 75.3 ms | 1.73 s | 123 ms | 86.8 ms | 81.5 ms | 905 ms | 282 ms | 171 ms | 394 ms |
| `grid-reach` | 72.2 ms | 1.54 s | 98.4 ms | 70.8 ms | 70.5 ms | 853 ms | 862 ms | 2.59 s | 383 ms |
| `sg-acyclic` | 1.02 s | 7.98 s | 3.13 s | 2.32 s | 3.46 s | 9.75 s | 12.3 s | 1.97 s | 11.9 s |
| `sg-acyclic-bf` | 3.33 ms | 89.6 ms | 2.66 s | 1.84 s | 6.62 ms | 8.55 s | 12.1 s | 1.52 s | 10.9 s |
| `sg-cyclic` | 3.22 s | 40.3 s | 12.5 s | 9.31 s | 16.3 s | 45.7 s | 76.4 s | 7.92 s | 51.2 s |
| `startup` | 0 us | 4.9 ms | 3.57 ms | 0 us | 447 us | 492 us | 971 us | 71.3 ms | 566 us |
| `tasks-ready` | 106 ms | 2.63 s | 354 ms | 217 ms | 230 ms | 1.32 s | 434 ms | 276 ms | 264 s |
| `tc-acyclic` | 1.43 s | 14.2 s | 3.28 s | 1.98 s | 4.53 s | 19.4 s | 35.4 s | 4.03 s | 35.2 s |
| `tc-acyclic-bf` | 15.8 ms | 1.22 s | 2.81 s | 1.49 s | 185 ms | 18.3 s | 35 s | 3.64 s | 35 s |
| `tc-acyclic-fb` | 19.5 ms | 831 ms | 6.77 s | 5.39 s | 45.2 ms | 18.3 s | 35 s | 3.48 s | 34.8 s |
| `tc-cyclic` | 4.06 s | 105 s | 11.4 s | 6.8 s | 15.9 s | 65.4 s | 164 s | 14.5 s | timeout |
| `tc-cyclic-bf` | 24.8 ms | 82 s | 10.4 s | 5.82 s | 5.95 s | 64.9 s | 164 s | 13.5 s | timeout |
| `tc-cyclic-fb` | 23.9 ms | 826 ms | 29 s | 24.2 s | 52.8 ms | 62.7 s | 165 s | 13.5 s | timeout |
| `tc-nonlinear` | 331 ms | 2.17 s | 900 ms | 578 ms | 874 ms | 1.71 s | 3.55 s | n/a | n/a |
| `tc-sparse-acyclic` | 87.3 ms | 668 ms | 223 ms | 171 ms | 188 ms | 588 ms | 412 ms | 126 ms | 964 ms |
| `tc-sparse-cyclic` | 1.18 s | 15.1 s | 2.65 s | 1.89 s | 2.8 s | 11.1 s | 8.61 s | 2.51 s | 19.6 s |
| `tree-sg-all` | 121 ms | 1.93 s | 227 ms | 121 ms | 121 ms | 709 ms | 702 ms | 358 ms | 1.22 s |
| `tree-sg-bound` | 7.28 ms | 162 ms | 7.88 s | 3.42 s | 6.8 ms | 36.7 s | 35.4 s | 9.16 s | 72.1 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 19.1 MiB | timeout | 19 MiB | 12.9 MiB | timeout | 63.8 MiB | 30.2 MiB | n/a | n/a |
| `chain-closure` | 216 MiB | 2.2 GiB | 89.5 MiB | 80 MiB | 152 MiB | 1.67 GiB | timeout | 1.48 GiB | 7.81 MiB |
| `chain-left` | 3.68 MiB | 14.9 MiB | 89.4 MiB | 80 MiB | 3.23 MiB | 1.27 GiB | timeout | 703 MiB | 7.72 MiB |
| `chain-right` | 3.66 MiB | 961 MiB | 89.4 MiB | 80 MiB | 80 MiB | 1.27 GiB | timeout | 703 MiB | 7.72 MiB |
| `cspa` | 25.3 MiB | timeout | 20.5 MiB | 13.7 MiB | timeout | 97.8 MiB | 57.9 MiB | n/a | n/a |
| `degree-count` | 14.9 MiB | 69.9 MiB | 18.9 MiB | 12.2 MiB | 12.2 MiB | 98.4 MiB | 22.2 MiB | 68.1 MiB | 22.7 MiB |
| `dist-bounded` | 158 MiB | 1.17 GiB | 160 MiB | 149 MiB | 166 MiB | 938 MiB | 382 MiB | 570 MiB | 10.2 MiB |
| `facts-join` | 53.7 MiB | 212 MiB | 78.9 MiB | 71.9 MiB | 71.9 MiB | 130 MiB | 98.2 MiB | 82.9 MiB | 21.9 MiB |
| `files-grouped` | 51.9 MiB | 238 MiB | 80.4 MiB | 75.7 MiB | 81.9 MiB | n/a | 80.3 MiB | 80.7 MiB | 28.6 MiB |
| `graph-left` | 12.5 MiB | 60.8 MiB | timeout | timeout | 9.71 MiB | timeout | timeout | timeout | timeout |
| `graph-reach` | 12 MiB | 60.8 MiB | 16.3 MiB | 9.34 MiB | 9.34 MiB | 49.1 MiB | 18.6 MiB | 62.3 MiB | 13.3 MiB |
| `grid-reach` | 14.3 MiB | 72.5 MiB | 18.3 MiB | 12.9 MiB | 13 MiB | 53.4 MiB | 21.4 MiB | 84.2 MiB | 14.1 MiB |
| `sg-acyclic` | 52.4 MiB | 574 MiB | 49.3 MiB | 40.7 MiB | 47 MiB | 408 MiB | 530 MiB | 404 MiB | 10.3 MiB |
| `sg-acyclic-bf` | 3.92 MiB | 16.9 MiB | 49.3 MiB | 40.7 MiB | 3.91 MiB | 318 MiB | 529 MiB | 350 MiB | 10.3 MiB |
| `sg-cyclic` | 108 MiB | 1.17 GiB | 104 MiB | 93.9 MiB | 121 MiB | 850 MiB | 3.28 GiB | 1.2 GiB | 10.3 MiB |
| `startup` | 0 MiB | 13.3 MiB | 9.74 MiB | 0 MiB | 3.45 MiB | 6.71 MiB | 11.1 MiB | 49.8 MiB | 4.44 MiB |
| `tasks-ready` | 19.3 MiB | 152 MiB | 21.4 MiB | 15.9 MiB | 16.5 MiB | 91.5 MiB | 24.6 MiB | 89.4 MiB | 18.6 MiB |
| `tc-acyclic` | 52.4 MiB | 598 MiB | 53.4 MiB | 45 MiB | 44 MiB | 404 MiB | 1.61 GiB | 295 MiB | 13 MiB |
| `tc-acyclic-bf` | 6.14 MiB | 74.9 MiB | 53.4 MiB | 45 MiB | 11.6 MiB | 306 MiB | 1.61 GiB | 243 MiB | 13.1 MiB |
| `tc-acyclic-fb` | 6.15 MiB | 43.2 MiB | 54 MiB | 45.5 MiB | 6.41 MiB | 317 MiB | 1.61 GiB | 243 MiB | 13.1 MiB |
| `tc-cyclic` | 110 MiB | 1.22 GiB | 108 MiB | 96.5 MiB | 100 MiB | 855 MiB | 6.07 GiB | 581 MiB | timeout |
| `tc-cyclic-bf` | 6.47 MiB | 453 MiB | 108 MiB | 96.5 MiB | 96.5 MiB | 645 MiB | 6.07 GiB | 474 MiB | timeout |
| `tc-cyclic-fb` | 6.46 MiB | 44.4 MiB | 109 MiB | 97.8 MiB | 6.3 MiB | 643 MiB | 6.07 GiB | 474 MiB | timeout |
| `tc-nonlinear` | 17.4 MiB | 118 MiB | 19.4 MiB | 12.5 MiB | 17.2 MiB | 99 MiB | 176 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 15.8 MiB | 107 MiB | 16.1 MiB | 9.48 MiB | 11.9 MiB | 93.4 MiB | 31.1 MiB | 89.7 MiB | 9.56 MiB |
| `tc-sparse-cyclic` | 109 MiB | 1.18 GiB | 87.3 MiB | 77 MiB | 85.6 MiB | 855 MiB | 420 MiB | 485 MiB | 10.1 MiB |
| `tree-sg-all` | 21.7 MiB | 148 MiB | 22.5 MiB | 15.4 MiB | 16.4 MiB | 138 MiB | 26.4 MiB | 148 MiB | 10.1 MiB |
| `tree-sg-bound` | 5.28 MiB | 18.9 MiB | 744 MiB | 705 MiB | 4.93 MiB | 6.24 GiB | 590 MiB | 4.54 GiB | 11.1 MiB |

</details>

### Scale 2, 64 threads

**Median wall time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **122 ms** | 175 ms | 144 ms | timeout | n/a |
| `chain-closure` | **2.94 s** | 7.45 s | 7.04 s | 7.56 s | 6.2 s |
| `chain-left` | **4.1 ms** | 5.93 s | 5.36 s | 39.5 ms | 4.16 s |
| `chain-right` | **4.81 ms** | 5.55 s | 5.17 s | 5.22 s | 4.27 s |
| `cspa` | **163 ms** | 241 ms | 195 ms | 45.9 s | n/a |
| `degree-count` | **89.8 ms** | 178 ms | 164 ms | 163 ms | 111 ms |
| `dist-bounded` | **722 ms** | 2.76 s | 2.69 s | 2.83 s | 2.34 s |
| `facts-join` | 203 ms | 473 ms | 465 ms | 464 ms | **168 ms** |
| `files-grouped` | 133 ms | 392 ms | 348 ms | 425 ms | **98.7 ms** |
| `graph-left` | **86.3 ms** | 192 s | 122 s | 132 ms | timeout |
| `graph-reach` | **75.1 ms** | 125 ms | 119 ms | 119 ms | 138 ms |
| `grid-reach` | **62.6 ms** | 327 ms | 314 ms | 322 ms | 2.23 s |
| `sg-acyclic` | **290 ms** | 942 ms | 879 ms | 1.01 s | 2.23 s |
| `sg-acyclic-bf` | **10.7 ms** | 272 ms | 186 ms | 19.4 ms | 1.76 s |
| `sg-cyclic` | **467 ms** | 2.04 s | 1.86 s | 2.28 s | 9.05 s |
| `startup` | **1.23 ms** | 5.25 ms | 2.4 ms | 2.28 ms | 26.2 ms |
| `tasks-ready` | **117 ms** | 165 ms | 139 ms | 143 ms | 262 ms |
| `tc-acyclic` | **392 ms** | 940 ms | 912 ms | 1.05 s | 4.32 s |
| `tc-acyclic-bf` | **25 ms** | 273 ms | 183 ms | 76.3 ms | 4.06 s |
| `tc-acyclic-fb` | **22.7 ms** | 495 ms | 454 ms | 58.1 ms | 4.07 s |
| `tc-cyclic` | **711 ms** | 2.03 s | 1.96 s | 2.34 s | 15.3 s |
| `tc-cyclic-bf` | **33.7 ms** | 604 ms | 376 ms | 374 ms | 14.5 s |
| `tc-cyclic-fb` | **25.1 ms** | 1.31 s | 1.23 s | 64.1 ms | 14.3 s |
| `tc-nonlinear` | **99.5 ms** | 225 ms | 202 ms | 266 ms | n/a |
| `tc-sparse-acyclic` | **75.3 ms** | 225 ms | 219 ms | 228 ms | 231 ms |
| `tc-sparse-cyclic` | **486 ms** | 1.85 s | 1.83 s | 1.97 s | 2.7 s |
| `tree-sg-all` | **98.6 ms** | 246 ms | 232 ms | 257 ms | 341 ms |
| `tree-sg-bound` | **8.93 ms** | 3.37 s | 1.56 s | 21.2 ms | 9.68 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 1.19 s | 7.69 s | 6.15 s | timeout | n/a |
| `chain-closure` | 29 s | 365 s | 340 s | 342 s | 7.31 s |
| `chain-left` | 2.66 ms | 375 s | 342 s | 1.77 s | 7.43 s |
| `chain-right` | 3.23 ms | 351 s | 327 s | 328 s | 8.2 s |
| `cspa` | 1.95 s | 12.6 s | 10.5 s | 1.86e+03 s | n/a |
| `degree-count` | 500 ms | 1.8 s | 1.74 s | 1.69 s | 189 ms |
| `dist-bounded` | 3.07 s | 39.4 s | 35.2 s | 35.4 s | 2.46 s |
| `facts-join` | 718 ms | 3.49 s | 3.43 s | 3.57 s | 154 ms |
| `files-grouped` | 1.03 s | 9.09 s | 6.95 s | 11.3 s | 300 ms |
| `graph-left` | 343 ms | 2.05e+03 s | 1.63e+03 s | 2.31 s | timeout |
| `graph-reach` | 464 ms | 1.59 s | 1.35 s | 1.55 s | 341 ms |
| `grid-reach` | 236 ms | 15.1 s | 14.6 s | 14.8 s | 5.55 s |
| `sg-acyclic` | 3.11 s | 28.5 s | 23.5 s | 24.3 s | 3.46 s |
| `sg-acyclic-bf` | 33.2 ms | 15.8 s | 10.8 s | 400 ms | 3.56 s |
| `sg-cyclic` | 6.61 s | 50 s | 38.6 s | 48.6 s | 16.3 s |
| `startup` | 465 us | 0 us | 0 us | 662 us | 39.8 ms |
| `tasks-ready` | 665 ms | 4.21 s | 3.22 s | 2.67 s | 380 ms |
| `tc-acyclic` | 4 s | 26.5 s | 21.3 s | 26.2 s | 5.78 s |
| `tc-acyclic-bf` | 85.3 ms | 13.7 s | 8.37 s | 2.21 s | 7.89 s |
| `tc-acyclic-fb` | 65.2 ms | 26.8 s | 24.8 s | 438 ms | 7.56 s |
| `tc-cyclic` | 9.59 s | 47.3 s | 33.7 s | 49.4 s | 29.1 s |
| `tc-cyclic-bf` | 129 ms | 34.4 s | 20.6 s | 20.9 s | 29.3 s |
| `tc-cyclic-fb` | 47.7 ms | 63.8 s | 61.9 s | 667 ms | 28.6 s |
| `tc-nonlinear` | 1.09 s | 13.1 s | 11.8 s | 15.9 s | n/a |
| `tc-sparse-acyclic` | 454 ms | 12.9 s | 13 s | 13.5 s | 299 ms |
| `tc-sparse-cyclic` | 4.43 s | 39.2 s | 29.3 s | 31.3 s | 3.21 s |
| `tree-sg-all` | 578 ms | 14.4 s | 13.9 s | 13.9 s | 418 ms |
| `tree-sg-bound` | 7.36 ms | 85.4 s | 65.5 s | 324 ms | 19.7 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 41.5 MiB | 33.6 MiB | 26.8 MiB | timeout | n/a |
| `chain-closure` | 274 MiB | 104 MiB | 98.2 MiB | 195 MiB | 1.5 GiB |
| `chain-left` | 3.3 MiB | 104 MiB | 98.3 MiB | 17 MiB | 816 MiB |
| `chain-right` | 3.68 MiB | 104 MiB | 98.4 MiB | 98.4 MiB | 825 MiB |
| `cspa` | 72.9 MiB | 37.5 MiB | 29.4 MiB | 5.97 GiB | n/a |
| `degree-count` | 79.5 MiB | 31.7 MiB | 26.1 MiB | 26 MiB | 123 MiB |
| `dist-bounded` | 720 MiB | 174 MiB | 168 MiB | 225 MiB | 614 MiB |
| `facts-join` | 109 MiB | 91.4 MiB | 83.7 MiB | 83.9 MiB | 96.4 MiB |
| `files-grouped` | 104 MiB | 96.7 MiB | 93.5 MiB | 100 MiB | 88.9 MiB |
| `graph-left` | 61.9 MiB | 33.7 GiB | 33.8 GiB | 23.5 MiB | timeout |
| `graph-reach` | 67.9 MiB | 28.7 MiB | 22.9 MiB | 22.9 MiB | 77.1 MiB |
| `grid-reach` | 41.3 MiB | 32.6 MiB | 27 MiB | 27 MiB | 165 MiB |
| `sg-acyclic` | 162 MiB | 66 MiB | 60.1 MiB | 62.1 MiB | 466 MiB |
| `sg-acyclic-bf` | 6.02 MiB | 66 MiB | 60.1 MiB | 17 MiB | 481 MiB |
| `sg-cyclic` | 324 MiB | 121 MiB | 115 MiB | 120 MiB | 1.3 GiB |
| `startup` | 3.09 MiB | 9.56 MiB | 0 MiB | 4.54 MiB | 51.2 MiB |
| `tasks-ready` | 54.1 MiB | 34.9 MiB | 29.4 MiB | 29.9 MiB | 165 MiB |
| `tc-acyclic` | 286 MiB | 67.2 MiB | 61.5 MiB | 62.4 MiB | 345 MiB |
| `tc-acyclic-bf` | 9.96 MiB | 67.2 MiB | 61.5 MiB | 25.9 MiB | 289 MiB |
| `tc-acyclic-fb` | 7.81 MiB | 67.6 MiB | 61.8 MiB | 19.8 MiB | 289 MiB |
| `tc-cyclic` | 556 MiB | 121 MiB | 115 MiB | 116 MiB | 649 MiB |
| `tc-cyclic-bf` | 19.9 MiB | 121 MiB | 115 MiB | 115 MiB | 516 MiB |
| `tc-cyclic-fb` | 8.12 MiB | 122 MiB | 116 MiB | 19.8 MiB | 514 MiB |
| `tc-nonlinear` | 91.2 MiB | 32.6 MiB | 26.5 MiB | 33.4 MiB | n/a |
| `tc-sparse-acyclic` | 50.4 MiB | 30.5 MiB | 24.8 MiB | 25.6 MiB | 134 MiB |
| `tc-sparse-cyclic` | 287 MiB | 104 MiB | 98.1 MiB | 103 MiB | 566 MiB |
| `tree-sg-all` | 75.2 MiB | 35.7 MiB | 29.8 MiB | 30.3 MiB | 205 MiB |
| `tree-sg-bound` | 5.23 MiB | 753 MiB | 747 MiB | 18.1 MiB | 5.97 GiB |

</details>

### Scale 4, 1 thread

**Median wall time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **411 ms** | timeout | 1e+03 ms | 722 ms | timeout | 1.99 s | 3.7 s | n/a | n/a |
| `chain-closure` | **7.22 s** | 48.6 s | 17.1 s | 12.1 s | 15.3 s | 43.4 s | timeout | 17.1 s | 202 s |
| `chain-left` | 6.93 ms | 88.3 ms | 10.9 s | 7.85 s | **5.11 ms** | 27.7 s | timeout | 9.32 s | 182 s |
| `chain-right` | **8.63 ms** | 8.24 s | 12.1 s | 8.88 s | 10.5 s | 27.7 s | timeout | 9.23 s | 183 s |
| `cspa` | **793 ms** | timeout | 2.45 s | 1.55 s | timeout | 4.1 s | 6.42 s | n/a | n/a |
| `degree-count` | 203 ms | 3.87 s | 349 ms | 264 ms | 258 ms | 2.6 s | 1.06 s | **172 ms** | 843 ms |
| `dist-bounded` | **2.97 s** | 23.5 s | 9.25 s | 6.88 s | 7.59 s | 18 s | 18.3 s | 5.02 s | 18.9 s |
| `facts-join` | 540 ms | 11.4 s | 1.02 s | 839 ms | 836 ms | 6.54 s | 3.6 s | **364 ms** | 794 ms |
| `files-grouped` | 629 ms | 11.7 s | 9.84 s | 3.39 s | 6.36 s | n/a | 5.88 s | **342 ms** | 1.27 s |
| `graph-left` | **167 ms** | 3.68 s | timeout | timeout | 202 ms | engine-limit | timeout | timeout | timeout |
| `graph-reach` | **161 ms** | 3.68 s | 245 ms | 185 ms | 186 ms | 1.88 s | 583 ms | 261 ms | 1.44 s |
| `grid-reach` | **297 ms** | 6.72 s | 446 ms | 304 ms | 316 ms | 3.59 s | 5.67 s | 17.9 s | 2.01 s |
| `sg-acyclic` | **4 s** | 29.7 s | 10.9 s | 8.29 s | 12.2 s | 35.6 s | 47.1 s | 7.3 s | 61.4 s |
| `sg-acyclic-bf` | **7.09 ms** | 177 ms | 9.3 s | 6.62 s | 9.69 ms | 32.2 s | 45.8 s | 5.78 s | 58.4 s |
| `sg-cyclic` | **13.9 s** | 177 s | 53.6 s | 40.2 s | 72.2 s | 210 s | timeout | 39.1 s | timeout |
| `startup` | **776 us** | 10.5 ms | 5.27 ms | 1.76 ms | 1.4 ms | 1.93 ms | 2.03 ms | 23.2 ms | 1.46 ms |
| `tasks-ready` | **273 ms** | 5.72 s | 931 ms | 561 ms | 568 ms | 2.93 s | 1.04 s | 480 ms | timeout |
| `tc-acyclic` | **6.46 s** | 57.3 s | 14.2 s | 8.82 s | 19.3 s | 88.8 s | 177 s | 17.7 s | timeout |
| `tc-acyclic-bf` | **42.4 ms** | 3.65 s | 12.4 s | 6.85 s | 733 ms | 83.1 s | 177 s | 16 s | timeout |
| `tc-acyclic-fb` | **40.4 ms** | 1.76 s | 34.5 s | 28.5 s | 103 ms | 83.7 s | 177 s | 16.1 s | timeout |
| `tc-cyclic` | **17.1 s** | timeout | 50 s | 31.2 s | 68.5 s | 289 s | timeout | 65.4 s | timeout |
| `tc-cyclic-bf` | **54.7 ms** | timeout | 45.7 s | 27.1 s | 27.6 s | 274 s | timeout | 60 s | timeout |
| `tc-cyclic-fb` | **46.9 ms** | 1.78 s | 135 s | 114 s | 113 ms | 277 s | timeout | 59.5 s | timeout |
| `tc-nonlinear` | **964 ms** | 5.83 s | 2.52 s | 1.69 s | 2.55 s | 5.93 s | 9.98 s | n/a | n/a |
| `tc-sparse-acyclic` | **235 ms** | 1.95 s | 639 ms | 491 ms | 536 ms | 1.93 s | 1.16 s | 457 ms | 5.6 s |
| `tc-sparse-cyclic` | **5.47 s** | 62.2 s | 10.8 s | 7.68 s | 11.4 s | 49.3 s | 36.7 s | 11.1 s | 211 s |
| `tree-sg-all` | 493 ms | 12 s | 910 ms | **473 ms** | 491 ms | 3.22 s | 2.81 s | 1.13 s | 5.55 s |
| `tree-sg-bound` | 31 ms | 639 ms | 136 s | 62.8 s | **23.6 ms** | engine-limit | timeout | 185 s | timeout |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 409 ms | timeout | 997 ms | 720 ms | timeout | 1.97 s | 3.67 s | n/a | n/a |
| `chain-closure` | 7.29 s | 45.4 s | 17.1 s | 12 s | 15.1 s | 40.7 s | timeout | 15.3 s | 93.9 s |
| `chain-left` | 2.29 ms | 80.2 ms | 10.9 s | 7.78 s | 0 us | 26.5 s | timeout | 8.52 s | 77.1 s |
| `chain-right` | 5.73 ms | 6.58 s | 12.1 s | 8.8 s | 10.4 s | 26 s | timeout | 8.37 s | 77.5 s |
| `cspa` | 799 ms | timeout | 2.44 s | 1.55 s | timeout | 4.07 s | 6.41 s | n/a | n/a |
| `degree-count` | 204 ms | 3.75 s | 348 ms | 263 ms | 252 ms | 2.56 s | 1.06 s | 140 ms | 818 ms |
| `dist-bounded` | 2.88 s | 21.6 s | 9.17 s | 6.8 s | 7.51 s | 17.3 s | 18.1 s | 4.77 s | 17.8 s |
| `facts-join` | 526 ms | 10.9 s | 977 ms | 810 ms | 786 ms | 6.48 s | 3.52 s | 323 ms | 787 ms |
| `files-grouped` | 598 ms | 11.1 s | 10 s | 3.37 s | 6.35 s | n/a | 5.84 s | 354 ms | 1.23 s |
| `graph-left` | 164 ms | 3.5 s | timeout | timeout | 202 ms | engine-limit | timeout | timeout | timeout |
| `graph-reach` | 161 ms | 3.43 s | 239 ms | 186 ms | 181 ms | 1.87 s | 576 ms | 364 ms | 885 ms |
| `grid-reach` | 290 ms | 6.32 s | 433 ms | 296 ms | 315 ms | 3.56 s | 5.59 s | 17.8 s | 1.53 s |
| `sg-acyclic` | 4.02 s | 29.9 s | 10.9 s | 8.28 s | 12.1 s | 35.3 s | 46.8 s | 7.15 s | 40.8 s |
| `sg-acyclic-bf` | 4.59 ms | 168 ms | 9.27 s | 6.6 s | 9.68 ms | 32 s | 45.5 s | 5.58 s | 38.1 s |
| `sg-cyclic` | 13.9 s | 176 s | 53.5 s | 40.1 s | 72.1 s | 210 s | timeout | 37.8 s | timeout |
| `startup` | 0 us | 6.65 ms | 0 us | 0 us | 533 us | 0 us | 661 us | 104 ms | 0 us |
| `tasks-ready` | 262 ms | 5.35 s | 920 ms | 556 ms | 565 ms | 2.91 s | 1.03 s | 507 ms | timeout |
| `tc-acyclic` | 6.48 s | 56.9 s | 14.2 s | 8.89 s | 19.2 s | 88.2 s | 176 s | 17.4 s | timeout |
| `tc-acyclic-bf` | 42.4 ms | 3.52 s | 12.4 s | 6.81 s | 722 ms | 83.1 s | 175 s | 15.9 s | timeout |
| `tc-acyclic-fb` | 34.9 ms | 1.67 s | 34.4 s | 28.6 s | 103 ms | 83.3 s | 175 s | 16 s | timeout |
| `tc-cyclic` | 17.1 s | timeout | 49.8 s | 31.1 s | 68.6 s | 288 s | timeout | 64.6 s | timeout |
| `tc-cyclic-bf` | 49.6 ms | timeout | 45.6 s | 27 s | 27.5 s | 273 s | timeout | 59.5 s | timeout |
| `tc-cyclic-fb` | 46.8 ms | 1.65 s | 134 s | 114 s | 108 ms | 275 s | timeout | 59 s | timeout |
| `tc-nonlinear` | 959 ms | 5.8 s | 2.52 s | 1.68 s | 2.55 s | 5.84 s | 9.89 s | n/a | n/a |
| `tc-sparse-acyclic` | 223 ms | 1.91 s | 635 ms | 490 ms | 536 ms | 1.88 s | 1.14 s | 523 ms | 3.04 s |
| `tc-sparse-cyclic` | 5.37 s | 60.9 s | 10.8 s | 7.62 s | 11.3 s | 48.2 s | 36.4 s | 10.5 s | 84.2 s |
| `tree-sg-all` | 466 ms | 12 s | 894 ms | 473 ms | 484 ms | 3.12 s | 2.81 s | 1.04 s | 5.36 s |
| `tree-sg-bound` | 28.4 ms | 629 ms | 132 s | 59.5 s | 21.1 ms | engine-limit | timeout | 159 s | timeout |

**Maximum peak resident memory across timed runs**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 33.7 MiB | timeout | 29.6 MiB | 23.9 MiB | timeout | 129 MiB | 49.9 MiB | n/a | n/a |
| `chain-closure` | 639 MiB | 9.68 GiB | 320 MiB | 302 MiB | 591 MiB | 6.82 GiB | timeout | 4.95 GiB | 8.12 MiB |
| `chain-left` | 4.26 MiB | 17 MiB | 320 MiB | 302 MiB | 4.02 MiB | 5.1 GiB | timeout | 2.57 GiB | 8.01 MiB |
| `chain-right` | 4.16 MiB | 4.15 GiB | 320 MiB | 302 MiB | 302 MiB | 5.1 GiB | timeout | 2.57 GiB | 8 MiB |
| `cspa` | 45.8 MiB | timeout | 30.6 MiB | 23.9 MiB | timeout | 192 MiB | 126 MiB | n/a | n/a |
| `degree-count` | 25.4 MiB | 128 MiB | 28.8 MiB | 21.8 MiB | 21.8 MiB | 196 MiB | 33 MiB | 83.6 MiB | 30.1 MiB |
| `dist-bounded` | 343 MiB | 2.94 GiB | 405 MiB | 382 MiB | 382 MiB | 2.24 GiB | 841 MiB | 1.32 GiB | 10.5 MiB |
| `facts-join` | 101 MiB | 403 MiB | 143 MiB | 135 MiB | 135 MiB | 259 MiB | 188 MiB | 120 MiB | 39.8 MiB |
| `files-grouped` | 93 MiB | 491 MiB | 145 MiB | 142 MiB | 150 MiB | n/a | 162 MiB | 110 MiB | 50.6 MiB |
| `graph-left` | 20.2 MiB | 113 MiB | timeout | timeout | 17 MiB | engine-limit | timeout | timeout | timeout |
| `graph-reach` | 19.6 MiB | 112 MiB | 22.1 MiB | 16.1 MiB | 16.1 MiB | 92.1 MiB | 26 MiB | 73.8 MiB | 19.4 MiB |
| `grid-reach` | 44.3 MiB | 257 MiB | 49.5 MiB | 43.8 MiB | 43.7 MiB | 203 MiB | 58.1 MiB | 158 MiB | 31.1 MiB |
| `sg-acyclic` | 205 MiB | 1.69 GiB | 146 MiB | 133 MiB | 193 MiB | 1.32 GiB | 1.39 GiB | 1.26 GiB | 10.8 MiB |
| `sg-acyclic-bf` | 3.73 MiB | 18.6 MiB | 148 MiB | 133 MiB | 4.21 MiB | 963 MiB | 1.38 GiB | 1.09 GiB | 10.9 MiB |
| `sg-cyclic` | 418 MiB | 3.95 GiB | 377 MiB | 353 MiB | 381 MiB | 3.31 GiB | timeout | 4.51 GiB | timeout |
| `startup` | 0 MiB | 12.6 MiB | 9.74 MiB | 0.535 MiB | 3.24 MiB | 7.11 MiB | 11.1 MiB | 49.9 MiB | 4.71 MiB |
| `tasks-ready` | 40.3 MiB | 316 MiB | 36.7 MiB | 31.8 MiB | 32.6 MiB | 189 MiB | 41.2 MiB | 128 MiB | timeout |
| `tc-acyclic` | 215 MiB | 2.04 GiB | 178 MiB | 163 MiB | 172 MiB | 1.55 GiB | 4.41 GiB | 946 MiB | timeout |
| `tc-acyclic-bf` | 9.51 MiB | 238 MiB | 178 MiB | 163 MiB | 32.5 MiB | 1.22 GiB | 4.41 GiB | 738 MiB | timeout |
| `tc-acyclic-fb` | 9.43 MiB | 74.1 MiB | 180 MiB | 164 MiB | 9.16 MiB | 1.23 GiB | 4.41 GiB | 738 MiB | timeout |
| `tc-cyclic` | 416 MiB | timeout | 414 MiB | 386 MiB | 390 MiB | 3.31 GiB | timeout | 2.19 GiB | timeout |
| `tc-cyclic-bf` | 10 MiB | timeout | 414 MiB | 386 MiB | 386 MiB | 2.52 GiB | timeout | 1.73 GiB | timeout |
| `tc-cyclic-fb` | 10 MiB | 83.6 MiB | 416 MiB | 388 MiB | 8.93 MiB | 2.52 GiB | timeout | 1.73 GiB | timeout |
| `tc-nonlinear` | 47.5 MiB | 333 MiB | 37.7 MiB | 29.9 MiB | 43.8 MiB | 266 MiB | 546 MiB | n/a | n/a |
| `tc-sparse-acyclic` | 38.4 MiB | 335 MiB | 27.6 MiB | 20.4 MiB | 27.9 MiB | 254 MiB | 65.7 MiB | 155 MiB | 10.4 MiB |
| `tc-sparse-cyclic` | 411 MiB | 3.96 GiB | 323 MiB | 301 MiB | 390 MiB | 3.31 GiB | 1.37 GiB | 1.85 GiB | 10.5 MiB |
| `tree-sg-all` | 72.7 MiB | 644 MiB | 58 MiB | 49.3 MiB | 54.4 MiB | 550 MiB | 73.5 MiB | 449 MiB | 10.3 MiB |
| `tree-sg-bound` | 11 MiB | 34.3 MiB | 11.1 GiB | 10.6 GiB | 8.46 MiB | engine-limit | timeout | 71.6 GiB | timeout |

</details>

### Scale 4, 64 threads

**Median wall time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **221 ms** | 325 ms | 274 ms | timeout | n/a |
| `chain-closure` | **11.6 s** | 26.2 s | 24.7 s | 26.7 s | 21.9 s |
| `chain-left` | **7.05 ms** | 19.6 s | 18.1 s | 67.9 ms | 14.3 s |
| `chain-right` | **11.8 ms** | 18.6 s | 17.1 s | 17.4 s | 14.6 s |
| `cspa` | **222 ms** | 424 ms | 330 ms | 199 s | n/a |
| `degree-count` | **166 ms** | 353 ms | 327 ms | 324 ms | 170 ms |
| `dist-bounded` | **1.84 s** | 7.23 s | 7.01 s | 7.51 s | 5.77 s |
| `facts-join` | 422 ms | 1.06 s | 1.04 s | 1.04 s | **206 ms** |
| `files-grouped` | 265 ms | 885 ms | 773 ms | 968 ms | **104 ms** |
| `graph-left` | **151 ms** | timeout | timeout | 263 ms | timeout |
| `graph-reach` | **118 ms** | 234 ms | 217 ms | 227 ms | 196 ms |
| `grid-reach` | **238 ms** | 930 ms | 913 ms | 947 ms | 4.95 s |
| `sg-acyclic` | **1.06 s** | 3.16 s | 2.95 s | 3.43 s | 8.06 s |
| `sg-acyclic-bf` | **7.48 ms** | 856 ms | 617 ms | 23.9 ms | 6.42 s |
| `sg-cyclic` | **1.89 s** | 8.09 s | 7.63 s | 9.33 s | 39.7 s |
| `startup` | **758 us** | 5.67 ms | 2.63 ms | 2.4 ms | 27.2 ms |
| `tasks-ready` | **205 ms** | 345 ms | 292 ms | 286 ms | 425 ms |
| `tc-acyclic` | **1.34 s** | 3.6 s | 3.51 s | 3.98 s | 18.7 s |
| `tc-acyclic-bf` | **48 ms** | 971 ms | 604 ms | 183 ms | 16.1 s |
| `tc-acyclic-fb` | **45.3 ms** | 2.1 s | 1.91 s | 110 ms | 16.2 s |
| `tc-cyclic` | **2.69 s** | 8.13 s | 7.66 s | 9.03 s | 61.8 s |
| `tc-cyclic-bf` | **59.5 ms** | 2.35 s | 1.34 s | 1.4 s | 58.3 s |
| `tc-cyclic-fb` | **48.8 ms** | 5.86 s | 5.19 s | 121 ms | 58.1 s |
| `tc-nonlinear` | **227 ms** | 615 ms | 560 ms | 716 ms | n/a |
| `tc-sparse-acyclic` | **166 ms** | 610 ms | 609 ms | 616 ms | 564 ms |
| `tc-sparse-cyclic` | **2 s** | 7.25 s | 7.19 s | 7.8 s | 11.6 s |
| `tree-sg-all` | **309 ms** | 892 ms | 858 ms | 955 ms | 1.17 s |
| `tree-sg-bound` | **31 ms** | 56.9 s | 25.5 s | 45.3 ms | 164 s |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 1.91 s | 14.3 s | 12 s | timeout | n/a |
| `chain-closure` | 121 s | 1.19e+03 s | 1.09e+03 s | 1.1e+03 s | 26.3 s |
| `chain-left` | 7.03 ms | 1.24e+03 s | 1.15e+03 s | 3.51 s | 29 s |
| `chain-right` | 8.92 ms | 1.17e+03 s | 1.08e+03 s | 1.1e+03 s | 29.5 s |
| `cspa` | 2.93 s | 23.4 s | 17.6 s | 7.53e+03 s | n/a |
| `degree-count` | 598 ms | 3.49 s | 3.46 s | 3.51 s | 247 ms |
| `dist-bounded` | 6.9 s | 54.2 s | 48.8 s | 49.6 s | 5.69 s |
| `facts-join` | 1.35 s | 8.15 s | 8.03 s | 8.27 s | 490 ms |
| `files-grouped` | 2.02 s | 21.2 s | 14.6 s | 26.4 s | 368 ms |
| `graph-left` | 632 ms | timeout | timeout | 5.18 s | timeout |
| `graph-reach` | 453 ms | 2.58 s | 2.23 s | 2.45 s | 378 ms |
| `grid-reach` | 741 ms | 42.5 s | 41.9 s | 43.1 s | 25.2 s |
| `sg-acyclic` | 11.1 s | 62 s | 50.8 s | 52.9 s | 13.7 s |
| `sg-acyclic-bf` | 7.33 ms | 48.5 s | 37.5 s | 316 ms | 14 s |
| `sg-cyclic` | 30.6 s | 120 s | 110 s | 153 s | 70.8 s |
| `startup` | 0 us | 3.65 ms | 853 us | 852 us | 106 ms |
| `tasks-ready` | 963 ms | 10.6 s | 7.42 s | 5.5 s | 1.04 s |
| `tc-acyclic` | 20.4 s | 62.1 s | 47 s | 63.7 s | 34.9 s |
| `tc-acyclic-bf` | 113 ms | 47.9 s | 32.4 s | 6.83 s | 28.1 s |
| `tc-acyclic-fb` | 91.9 ms | 79.2 s | 73.8 s | 920 ms | 36.4 s |
| `tc-cyclic` | 47.1 s | 108 s | 80.3 s | 140 s | 104 s |
| `tc-cyclic-bf` | 107 ms | 91.6 s | 65.5 s | 69.3 s | 131 s |
| `tc-cyclic-fb` | 61.6 ms | 214 s | 185 s | 1.59 s | 119 s |
| `tc-nonlinear` | 2.67 s | 22.9 s | 19.5 s | 24 s | n/a |
| `tc-sparse-acyclic` | 1.18 s | 22.6 s | 20 s | 18.2 s | 607 ms |
| `tc-sparse-cyclic` | 20.7 s | 78.7 s | 67 s | 74.3 s | 18.1 s |
| `tree-sg-all` | 1.58 s | 26.4 s | 20.1 s | 16.2 s | 1.43 s |
| `tree-sg-bound` | 28.4 ms | 313 s | 215 s | 490 ms | 391 s |

**Maximum peak resident memory across timed runs**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 62.4 MiB | 43.4 MiB | 38.4 MiB | timeout | n/a |
| `chain-closure` | 761 MiB | 335 MiB | 328 MiB | 677 MiB | 4.97 GiB |
| `chain-left` | 3.93 MiB | 334 MiB | 329 MiB | 17.6 MiB | 2.71 GiB |
| `chain-right` | 7.58 MiB | 335 MiB | 329 MiB | 329 MiB | 2.79 GiB |
| `cspa` | 120 MiB | 49.5 MiB | 41.2 MiB | 23.3 GiB | n/a |
| `degree-count` | 116 MiB | 40.9 MiB | 35.1 MiB | 35.1 MiB | 179 MiB |
| `dist-bounded` | 1.71 GiB | 415 MiB | 409 MiB | 560 MiB | 1.36 GiB |
| `facts-join` | 160 MiB | 153 MiB | 150 MiB | 150 MiB | 154 MiB |
| `files-grouped` | 148 MiB | 165 MiB | 164 MiB | 177 MiB | 125 MiB |
| `graph-left` | 91.5 MiB | timeout | timeout | 30.4 MiB | timeout |
| `graph-reach` | 92.8 MiB | 34.9 MiB | 29.3 MiB | 29.3 MiB | 101 MiB |
| `grid-reach` | 80.6 MiB | 62.5 MiB | 57 MiB | 56.9 MiB | 264 MiB |
| `sg-acyclic` | 802 MiB | 168 MiB | 161 MiB | 172 MiB | 1.62 GiB |
| `sg-acyclic-bf` | 4.3 MiB | 167 MiB | 161 MiB | 16.4 MiB | 1.45 GiB |
| `sg-cyclic` | 993 MiB | 396 MiB | 391 MiB | 426 MiB | 5.1 GiB |
| `startup` | 0 MiB | 9.57 MiB | 4.48 MiB | 4.18 MiB | 51.2 MiB |
| `tasks-ready` | 121 MiB | 50.8 MiB | 45.5 MiB | 46.5 MiB | 257 MiB |
| `tc-acyclic` | 835 MiB | 197 MiB | 191 MiB | 194 MiB | 1.02 GiB |
| `tc-acyclic-bf` | 14 MiB | 197 MiB | 192 MiB | 50.3 MiB | 806 MiB |
| `tc-acyclic-fb` | 11.1 MiB | 198 MiB | 192 MiB | 23 MiB | 806 MiB |
| `tc-cyclic` | 1.35 GiB | 428 MiB | 422 MiB | 426 MiB | 2.35 GiB |
| `tc-cyclic-bf` | 24.4 MiB | 428 MiB | 422 MiB | 422 MiB | 1.8 GiB |
| `tc-cyclic-fb` | 11.3 MiB | 430 MiB | 424 MiB | 22.8 MiB | 1.8 GiB |
| `tc-nonlinear` | 240 MiB | 51.6 MiB | 45.4 MiB | 65 MiB | n/a |
| `tc-sparse-acyclic` | 114 MiB | 44.6 MiB | 38.8 MiB | 43.1 MiB | 221 MiB |
| `tc-sparse-cyclic` | 859 MiB | 358 MiB | 352 MiB | 371 MiB | 2.07 GiB |
| `tree-sg-all` | 235 MiB | 71.1 MiB | 65.1 MiB | 69.7 MiB | 531 MiB |
| `tree-sg-bound` | 10.9 MiB | 11.5 GiB | 11.3 GiB | 22.6 MiB | 90.6 GiB |

</details>

### Scale 8, 1 thread

**Median wall time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **822 ms** | timeout | 2.32 s | 1.68 s | timeout | 4.25 s | 8.86 s | n/a | n/a |
| `chain-closure` | **31.5 s** | timeout | 74.9 s | 54 s | 67.9 s | 186 s | timeout | 67.9 s | timeout |
| `chain-left` | 13 ms | 166 ms | 46.4 s | 33.8 s | **8.67 ms** | 124 s | timeout | 37.7 s | timeout |
| `chain-right` | **16 ms** | 32.4 s | 51.7 s | 38.4 s | 45 s | 124 s | timeout | 37.3 s | timeout |
| `cspa` | **2.01 s** | timeout | 5.38 s | 3.41 s | timeout | 10.8 s | 17 s | n/a | n/a |
| `degree-count` | 424 ms | 7.93 s | 731 ms | 560 ms | 552 ms | 5.57 s | 2.17 s | **338 ms** | 1.76 s |
| `dist-bounded` | **7.12 s** | 54 s | 21.4 s | 16 s | 17.6 s | 42.6 s | 46 s | 13.1 s | 44.2 s |
| `facts-join` | 1.22 s | 23.4 s | 2.35 s | 1.95 s | 1.94 s | 13.2 s | 9.13 s | **753 ms** | 1.86 s |
| `files-grouped` | 1.33 s | 24.4 s | 29.4 s | 8.28 s | 15 s | n/a | 12 s | **657 ms** | 2.61 s |
| `graph-left` | **349 ms** | 7.4 s | timeout | timeout | 428 ms | timeout | timeout | timeout | timeout |
| `graph-reach` | **337 ms** | 7.3 s | 507 ms | 398 ms | 392 ms | 3.93 s | 1.2 s | 486 ms | 3.46 s |
| `grid-reach` | **1.34 s** | 30.6 s | 2.01 s | 1.45 s | 1.52 s | 15 s | 42 s | 162 s | 13.2 s |
| `sg-acyclic` | **18.4 s** | 115 s | 43.2 s | 32.6 s | 47.3 s | 146 s | 213 s | 30.3 s | timeout |
| `sg-acyclic-bf` | **15.3 ms** | 345 ms | 36.5 s | 26.3 s | 21 ms | 129 s | 203 s | 24.3 s | 290 s |
| `sg-cyclic` | **93.9 s** | timeout | 233 s | 178 s | timeout | timeout | timeout | 170 s | timeout |
| `startup` | **826 us** | 9.78 ms | 5.78 ms | 1.79 ms | 1.5 ms | 1.87 ms | 2.73 ms | 23.3 ms | 1.39 ms |
| `tasks-ready` | **686 ms** | 11.7 s | 2.31 s | 1.37 s | 1.31 s | 6.88 s | 2.55 s | 1.08 s | timeout |
| `tc-acyclic` | 50.9 s | 219 s | 58.3 s | **36.9 s** | 81 s | timeout | timeout | 78.5 s | timeout |
| `tc-acyclic-bf` | **86.6 ms** | 10.3 s | 51.6 s | 29.2 s | 2.7 s | timeout | timeout | 69.7 s | timeout |
| `tc-acyclic-fb` | **83.4 ms** | 3.57 s | 158 s | 132 s | 220 ms | timeout | timeout | 69.9 s | timeout |
| `tc-cyclic` | 165 s | timeout | 191 s | **112 s** | 269 s | timeout | timeout | 289 s | timeout |
| `tc-cyclic-bf` | **113 ms** | timeout | 165 s | 93.1 s | 95.2 s | timeout | timeout | 271 s | timeout |
| `tc-cyclic-fb` | **97.9 ms** | 3.6 s | timeout | timeout | 238 ms | timeout | timeout | 269 s | timeout |
| `tc-nonlinear` | **2.99 s** | 17.5 s | 8.17 s | 5.73 s | 8.37 s | 21.3 s | 39.7 s | n/a | n/a |
| `tc-sparse-acyclic` | **781 ms** | 5.64 s | 1.97 s | 1.52 s | 1.63 s | 6.39 s | 3.54 s | 1.51 s | 20.8 s |
| `tc-sparse-cyclic` | **27.4 s** | timeout | 44.1 s | 31.1 s | 46.8 s | 217 s | 157 s | 48.2 s | timeout |
| `tree-sg-all` | 2.21 s | 83.2 s | 3.67 s | **1.9 s** | 1.97 s | 13.9 s | 11.6 s | 4.87 s | 23.9 s |
| `tree-sg-bound` | 537 ms | 11.1 s | timeout | timeout | **493 ms** | engine-limit | timeout | timeout | timeout |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 804 ms | timeout | 2.31 s | 1.66 s | timeout | 4.2 s | 8.82 s | n/a | n/a |
| `chain-closure` | 30.7 s | timeout | 74.5 s | 53.7 s | 66.9 s | 174 s | timeout | 61 s | timeout |
| `chain-left` | 10.8 ms | 153 ms | 46.1 s | 33.5 s | 8.46 ms | 116 s | timeout | 34.3 s | timeout |
| `chain-right` | 15.9 ms | 25.4 s | 51.2 s | 38.1 s | 44.7 s | 116 s | timeout | 33.7 s | timeout |
| `cspa` | 2 s | timeout | 5.37 s | 3.41 s | timeout | 10.7 s | 16.9 s | n/a | n/a |
| `degree-count` | 410 ms | 7.89 s | 722 ms | 550 ms | 550 ms | 5.49 s | 2.16 s | 277 ms | 1.73 s |
| `dist-bounded` | 6.89 s | 48.7 s | 21.1 s | 15.9 s | 17.4 s | 40.8 s | 45.3 s | 11.8 s | 41.8 s |
| `facts-join` | 1.15 s | 22.7 s | 2.29 s | 1.91 s | 1.88 s | 13.2 s | 9.03 s | 730 ms | 1.85 s |
| `files-grouped` | 1.28 s | 23.2 s | 29.3 s | 8.25 s | 14.9 s | n/a | 12 s | 625 ms | 2.53 s |
| `graph-left` | 342 ms | 6.97 s | timeout | timeout | 425 ms | timeout | timeout | timeout | timeout |
| `graph-reach` | 335 ms | 6.87 s | 496 ms | 387 ms | 390 ms | 3.91 s | 1.19 s | 504 ms | 1.83 s |
| `grid-reach` | 1.31 s | 29 s | 1.99 s | 1.39 s | 1.48 s | 14.9 s | 41 s | 161 s | 7.61 s |
| `sg-acyclic` | 18.3 s | 112 s | 43 s | 32.5 s | 47.1 s | 144 s | 211 s | 29 s | timeout |
| `sg-acyclic-bf` | 12.6 ms | 316 ms | 36.4 s | 26.2 s | 21 ms | 128 s | 202 s | 23.2 s | 153 s |
| `sg-cyclic` | 93.4 s | timeout | 232 s | 178 s | timeout | timeout | timeout | 163 s | timeout |
| `startup` | 0 us | 6.4 ms | 1.91 ms | 0 us | 0 us | 0 us | 0 us | 122 ms | 0 us |
| `tasks-ready` | 666 ms | 11 s | 2.29 s | 1.36 s | 1.3 s | 6.8 s | 2.52 s | 1.11 s | timeout |
| `tc-acyclic` | 50.7 s | 215 s | 58.3 s | 36.7 s | 80.8 s | timeout | timeout | 77.2 s | timeout |
| `tc-acyclic-bf` | 84.2 ms | 9.98 s | 51.3 s | 29.1 s | 2.68 s | timeout | timeout | 69.1 s | timeout |
| `tc-acyclic-fb` | 80.9 ms | 3.43 s | 158 s | 132 s | 215 ms | timeout | timeout | 68.9 s | timeout |
| `tc-cyclic` | 165 s | timeout | 191 s | 112 s | 268 s | timeout | timeout | 285 s | timeout |
| `tc-cyclic-bf` | 103 ms | timeout | 165 s | 92.8 s | 94.8 s | timeout | timeout | 269 s | timeout |
| `tc-cyclic-fb` | 94.9 ms | 3.47 s | timeout | timeout | 228 ms | timeout | timeout | 267 s | timeout |
| `tc-nonlinear` | 2.98 s | 17.6 s | 8.15 s | 5.71 s | 8.34 s | 21.3 s | 39.2 s | n/a | n/a |
| `tc-sparse-acyclic` | 744 ms | 5.75 s | 1.95 s | 1.51 s | 1.61 s | 6.22 s | 3.53 s | 1.23 s | 10.2 s |
| `tc-sparse-cyclic` | 26.9 s | timeout | 43.5 s | 30.7 s | 46.2 s | 211 s | 155 s | 45.6 s | timeout |
| `tree-sg-all` | 2.16 s | 82.4 s | 3.63 s | 1.86 s | 1.95 s | 13.1 s | 11.6 s | 4.49 s | 23.2 s |
| `tree-sg-bound` | 519 ms | 10.6 s | timeout | timeout | 458 ms | engine-limit | timeout | timeout | timeout |

**Maximum peak resident memory across timed runs**

| Workload | relic | SWI-Prolog | SF interp | Souffle compiled | Souffle Magic | clingo | Nemo | DuckDB | SQLite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 63 MiB | timeout | 50.1 MiB | 45 MiB | timeout | 255 MiB | 97.1 MiB | n/a | n/a |
| `chain-closure` | 2.41 GiB | timeout | 1.2 GiB | 1.15 GiB | 2.28 GiB | 26.8 GiB | timeout | 18.7 GiB | timeout |
| `chain-left` | 5.25 MiB | 19.9 MiB | 1.2 GiB | 1.15 GiB | 5 MiB | 20.3 GiB | timeout | 10.1 GiB | timeout |
| `chain-right` | 6.02 MiB | 14.6 GiB | 1.2 GiB | 1.15 GiB | 1.15 GiB | 20.3 GiB | timeout | 10.1 GiB | timeout |
| `cspa` | 85.8 MiB | timeout | 51 MiB | 44 MiB | timeout | 393 MiB | 305 MiB | n/a | n/a |
| `degree-count` | 46.4 MiB | 237 MiB | 47.7 MiB | 40.2 MiB | 40.2 MiB | 388 MiB | 52 MiB | 122 MiB | 43.6 MiB |
| `dist-bounded` | 998 MiB | 7.04 GiB | 939 MiB | 892 MiB | 892 MiB | 5.01 GiB | 1.83 GiB | 3.49 GiB | 11.1 MiB |
| `facts-join` | 212 MiB | 793 MiB | 294 MiB | 285 MiB | 285 MiB | 557 MiB | 351 MiB | 211 MiB | 73.9 MiB |
| `files-grouped` | 203 MiB | 938 MiB | 294 MiB | 292 MiB | 311 MiB | n/a | 277 MiB | 172 MiB | 96 MiB |
| `graph-left` | 36.7 MiB | 211 MiB | timeout | timeout | 30.4 MiB | timeout | timeout | timeout | timeout |
| `graph-reach` | 35.4 MiB | 209 MiB | 35.5 MiB | 29.6 MiB | 29.7 MiB | 188 MiB | 39.4 MiB | 98.4 MiB | 32.4 MiB |
| `grid-reach` | 187 MiB | 1.01e+03 MiB | 164 MiB | 154 MiB | 154 MiB | 795 MiB | 186 MiB | 393 MiB | 99.6 MiB |
| `sg-acyclic` | 613 MiB | 7.31 GiB | 515 MiB | 483 MiB | 556 MiB | 4.58 GiB | 4.59 GiB | 4 GiB | timeout |
| `sg-acyclic-bf` | 5.73 MiB | 24.8 MiB | 513 MiB | 482 MiB | 4.82 MiB | 3.24 GiB | 4.59 GiB | 3.48 GiB | 11.8 MiB |
| `sg-cyclic` | 1.52 GiB | timeout | 1.5 GiB | 1.42 GiB | timeout | timeout | timeout | 17.2 GiB | timeout |
| `startup` | 0 MiB | 13.3 MiB | 9.5 MiB | 2.06 MiB | 0 MiB | 2.07 MiB | 9.12 MiB | 49 MiB | 0 MiB |
| `tasks-ready` | 85.3 MiB | 719 MiB | 73.9 MiB | 68 MiB | 73.5 MiB | 422 MiB | 81.8 MiB | 277 MiB | timeout |
| `tc-acyclic` | 754 MiB | 8.79 GiB | 649 MiB | 602 MiB | 621 MiB | timeout | timeout | 3.5 GiB | timeout |
| `tc-acyclic-bf` | 15.4 MiB | 732 MiB | 655 MiB | 602 MiB | 108 MiB | timeout | timeout | 2.63 GiB | timeout |
| `tc-acyclic-fb` | 15.4 MiB | 134 MiB | 648 MiB | 606 MiB | 14.7 MiB | timeout | timeout | 2.63 GiB | timeout |
| `tc-cyclic` | 1.54 GiB | timeout | 1.54 GiB | 1.45 GiB | 1.45 GiB | timeout | timeout | 9.13 GiB | timeout |
| `tc-cyclic-bf` | 15.4 MiB | timeout | 1.54 GiB | 1.45 GiB | 1.45 GiB | timeout | timeout | 6.94 GiB | timeout |
| `tc-cyclic-fb` | 15.4 MiB | 154 MiB | timeout | timeout | 14.3 MiB | timeout | timeout | 6.94 GiB | timeout |
| `tc-nonlinear` | 121 MiB | 1e+03 MiB | 92.2 MiB | 81.3 MiB | 123 MiB | 905 MiB | 1.79 GiB | n/a | n/a |
| `tc-sparse-acyclic` | 106 MiB | 971 MiB | 63.2 MiB | 54.2 MiB | 79.1 MiB | 851 MiB | 174 MiB | 413 MiB | 11.1 MiB |
| `tc-sparse-cyclic` | 1.52 GiB | timeout | 1.2 GiB | 1.22 GiB | 1.27 GiB | 13.3 GiB | 7.09 GiB | 6.66 GiB | timeout |
| `tree-sg-all` | 312 MiB | 2.35 GiB | 197 MiB | 182 MiB | 206 MiB | 2.16 GiB | 217 MiB | 1.64 GiB | 10.6 MiB |
| `tree-sg-bound` | 120 MiB | 344 MiB | timeout | timeout | 118 MiB | engine-limit | timeout | timeout | timeout |

</details>

### Scale 8, 64 threads

**Median wall time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | **324 ms** | 576 ms | 516 ms | timeout | n/a |
| `chain-closure` | **34 s** | 98.5 s | 91.5 s | 98.6 s | 81.4 s |
| `chain-left` | **12.9 ms** | 68.8 s | 62.7 s | 122 ms | 51.6 s |
| `chain-right` | **18.6 ms** | 67.2 s | 60.8 s | 60.4 s | 51.1 s |
| `cspa` | **408 ms** | 812 ms | 666 ms | timeout | n/a |
| `degree-count` | 312 ms | 725 ms | 675 ms | 663 ms | **274 ms** |
| `dist-bounded` | **3.72 s** | 17.4 s | 17.1 s | 18.1 s | 14.8 s |
| `facts-join` | 903 ms | 2.39 s | 2.34 s | 2.35 s | **325 ms** |
| `files-grouped` | 530 ms | 2.11 s | 1.7 s | 2.23 s | **125 ms** |
| `graph-left` | **282 ms** | timeout | timeout | 538 ms | timeout |
| `graph-reach` | **226 ms** | 454 ms | 437 ms | 448 ms | 283 ms |
| `grid-reach` | **976 ms** | 3.02 s | 2.99 s | 3.09 s | 14 s |
| `sg-acyclic` | **3.78 s** | 12.1 s | 11.2 s | 13.1 s | 31.4 s |
| `sg-acyclic-bf` | **23 ms** | 3.25 s | 2.33 s | 34 ms | 25.6 s |
| `sg-cyclic` | **10.6 s** | 32.6 s | 30.4 s | 37 s | 170 s |
| `startup` | **766 us** | 5.16 ms | 2.34 ms | 2.44 ms | 25.6 ms |
| `tasks-ready` | **407 ms** | 790 ms | 628 ms | 602 ms | 790 ms |
| `tc-acyclic` | **5.56 s** | 14.1 s | 13.7 s | 15.8 s | 71.6 s |
| `tc-acyclic-bf` | **82.5 ms** | 3.69 s | 2.31 s | 508 ms | 64.8 s |
| `tc-acyclic-fb` | **79.6 ms** | 9.06 s | 8.78 s | 218 ms | 65 s |
| `tc-cyclic` | **10.8 s** | 31.9 s | 30.9 s | 37.1 s | 242 s |
| `tc-cyclic-bf` | **90.1 ms** | 8.92 s | 5.27 s | 5.24 s | 221 s |
| `tc-cyclic-fb` | **89.5 ms** | 30.9 s | 29.9 s | 241 ms | 224 s |
| `tc-nonlinear` | **927 ms** | 1.85 s | 1.68 s | 2.16 s | n/a |
| `tc-sparse-acyclic` | **409 ms** | 1.83 s | 1.82 s | 1.87 s | 1.69 s |
| `tc-sparse-cyclic` | **8.52 s** | 29.3 s | 29.1 s | 31.2 s | 48.3 s |
| `tree-sg-all` | **1.19 s** | 3.46 s | 3.31 s | 3.74 s | 4.79 s |
| `tree-sg-bound` | **457 ms** | timeout | timeout | 638 ms | timeout |

<details>
<summary>Mean user CPU time and peak resident memory</summary>

**Mean user CPU time**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 2.65 s | 25.3 s | 22.4 s | timeout | n/a |
| `chain-closure` | 324 s | 4.32e+03 s | 3.88e+03 s | 3.84e+03 s | 100 s |
| `chain-left` | 10.7 ms | 4.34e+03 s | 3.95e+03 s | 6.7 s | 102 s |
| `chain-right` | 44.5 ms | 4.24e+03 s | 3.83e+03 s | 3.79e+03 s | 108 s |
| `cspa` | 5.35 s | 43.8 s | 35.9 s | timeout | n/a |
| `degree-count` | 1.33 s | 6.84 s | 6.93 s | 6.59 s | 446 ms |
| `dist-bounded` | 16.6 s | 75 s | 70.6 s | 71.9 s | 15 s |
| `facts-join` | 3.09 s | 15.2 s | 15.1 s | 14.9 s | 769 ms |
| `files-grouped` | 3.89 s | 56.3 s | 30.3 s | 64.8 s | 614 ms |
| `graph-left` | 1.18 s | timeout | timeout | 10.7 s | timeout |
| `graph-reach` | 1.35 s | 4.43 s | 4.08 s | 4.41 s | 651 ms |
| `grid-reach` | 2.93 s | 116 s | 114 s | 115 s | 206 s |
| `sg-acyclic` | 45.5 s | 133 s | 125 s | 143 s | 50.7 s |
| `sg-acyclic-bf` | 106 ms | 116 s | 109 s | 644 ms | 58 s |
| `sg-cyclic` | 155 s | 355 s | 316 s | 489 s | 330 s |
| `startup` | 0 us | 1.94 ms | 0 us | 798 us | 100 ms |
| `tasks-ready` | 2.72 s | 27.8 s | 18.1 s | 12.5 s | 2.02 s |
| `tc-acyclic` | 98.6 s | 138 s | 117 s | 186 s | 111 s |
| `tc-acyclic-bf` | 310 ms | 121 s | 99.3 s | 23.2 s | 152 s |
| `tc-acyclic-fb` | 306 ms | 277 s | 268 s | 1.77 s | 150 s |
| `tc-cyclic` | 201 s | 293 s | 211 s | 429 s | 379 s |
| `tc-cyclic-bf` | 282 ms | 267 s | 188 s | 185 s | 496 s |
| `tc-cyclic-fb` | 187 ms | 1.15e+03 s | 1.05e+03 s | 3.22 s | 456 s |
| `tc-nonlinear` | 7.15 s | 43.8 s | 33 s | 46.3 s | n/a |
| `tc-sparse-acyclic` | 2.54 s | 42.4 s | 33.7 s | 28.2 s | 1.74 s |
| `tc-sparse-cyclic` | 85.8 s | 141 s | 130 s | 156 s | 77.2 s |
| `tree-sg-all` | 5.02 s | 60.8 s | 38.9 s | 24.2 s | 5.91 s |
| `tree-sg-bound` | 1.03 s | timeout | timeout | 4.64 s | timeout |

**Maximum peak resident memory across timed runs**

| Workload | relic | SF interp | Souffle compiled | Souffle Magic | DuckDB |
| --- | ---: | ---: | ---: | ---: | ---: |
| `andersen` | 106 MiB | 65.4 MiB | 60.6 MiB | timeout | n/a |
| `chain-closure` | 2.67 GiB | 1.22 GiB | 1.21 GiB | 2.76 GiB | 18.5 GiB |
| `chain-left` | 5.21 MiB | 1.22 GiB | 1.21 GiB | 18.3 MiB | 10.3 GiB |
| `chain-right` | 10.4 MiB | 1.22 GiB | 1.21 GiB | 1.21 GiB | 10.5 GiB |
| `cspa` | 212 MiB | 72.9 MiB | 66.5 MiB | timeout | n/a |
| `degree-count` | 185 MiB | 59.1 MiB | 53.3 MiB | 53.6 MiB | 292 MiB |
| `dist-bounded` | 3.84 GiB | 953 MiB | 947 MiB | 1.28 GiB | 3.54 GiB |
| `facts-join` | 271 MiB | 306 MiB | 299 MiB | 301 MiB | 278 MiB |
| `files-grouped` | 258 MiB | 337 MiB | 325 MiB | 353 MiB | 198 MiB |
| `graph-left` | 129 MiB | timeout | timeout | 45.6 MiB | timeout |
| `graph-reach` | 126 MiB | 47.2 MiB | 43.2 MiB | 43.2 MiB | 138 MiB |
| `grid-reach` | 223 MiB | 178 MiB | 173 MiB | 173 MiB | 521 MiB |
| `sg-acyclic` | 3.18 GiB | 566 MiB | 560 MiB | 609 MiB | 5.66 GiB |
| `sg-acyclic-bf` | 9.09 MiB | 567 MiB | 561 MiB | 18.1 MiB | 4.85 GiB |
| `sg-cyclic` | 8.58 GiB | 1.51 GiB | 1.5 GiB | 2.01 GiB | 20.4 GiB |
| `startup` | 0 MiB | 4.89 MiB | 1.82 MiB | 4.56 MiB | 51.1 MiB |
| `tasks-ready` | 239 MiB | 91.2 MiB | 86 MiB | 88.7 MiB | 503 MiB |
| `tc-acyclic` | 2.68 GiB | 703 MiB | 700 MiB | 734 MiB | 3.73 GiB |
| `tc-acyclic-bf` | 67.4 MiB | 703 MiB | 698 MiB | 136 MiB | 2.8 GiB |
| `tc-acyclic-fb` | 66.2 MiB | 704 MiB | 701 MiB | 29.4 MiB | 2.8 GiB |
| `tc-cyclic` | 5 GiB | 1.55 GiB | 1.55 GiB | 1.94 GiB | 9.18 GiB |
| `tc-cyclic-bf` | 75.5 MiB | 1.55 GiB | 1.55 GiB | 1.55 GiB | 6.99 GiB |
| `tc-cyclic-fb` | 66.5 MiB | 1.56 GiB | 1.55 GiB | 28.6 MiB | 6.99 GiB |
| `tc-nonlinear` | 1.31 GiB | 110 MiB | 104 MiB | 164 MiB | n/a |
| `tc-sparse-acyclic` | 290 MiB | 88.3 MiB | 82.6 MiB | 96 MiB | 492 MiB |
| `tc-sparse-cyclic` | 4.21 GiB | 1.3 GiB | 1.29 GiB | 1.84 GiB | 7.39 GiB |
| `tree-sg-all` | 928 MiB | 210 MiB | 204 MiB | 227 MiB | 1.92 GiB |
| `tree-sg-bound` | 158 MiB | timeout | timeout | 128 MiB | timeout |

</details>

## Raw results and reproduction

[Canonical results](bench/compare/results.tsv) record every cell, timing spread, memory, answer size, compilation time and requested/accepted repetitions. [Comparison summary](bench/compare/suite-summary.tsv) names each scenario's fastest completed rival. [Environment](bench/compare/environment.json) records the cohort context.

Use the [FreeBSD build instructions](bench/compare/tools/README-freebsd.md). Run the full suite on an idle host with the documented engine paths:

```sh
sh bench/compare/sweep.sh -j 64 -o /path/to/fresh-cohort
```

Keep every scenario, raw sample and diagnostic together. Supply `environment.json` beside the cohort, then audit and publish with:

```sh
python3 bench/compare/tools/publish-results.py --record /path/to/cohort --check
python3 bench/compare/tools/publish-results.py --record /path/to/cohort
python3.15 bench/compare/tools/plot-performance.py
python3 bench/compare/tools/export-website.py
```

The Apple M3 Max file-output study is preserved in git history. Its timings are a separate cohort and are not merged into these tables.
