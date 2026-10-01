# Coverage gaps

`make coverage` lists every range of lines the suite never executes. Each
range was sorted into one of the kinds below; a range an ordinary input
reaches is covered by a test rather than listed here. Line numbers drift, so
each gap is named by its function.

## Allocation failures the sweep cannot reach

[Medium, component: `tests/unit/test_exhaustion.c`] Most of the uncovered
failure branches in the library are a failed arena allocation followed by
its propagation:

* `lib/eval.c`: `evaluation_track_deltas`, `build_running_plan`,
  `plan_members`, `plan_base`, `plan_variants`, `allocate_delta_work`,
  `plan_delta_work`, `plan_query`;
* `lib/magic.c`: `index_rules_by_head`, `demand_begin`,
  `relic_magic_find_complete`, `name_adornment`, `canonicalize`,
  `bound_args` and its callers `guard_literal` and `record_call`,
  `later_slots`, `intern_supplementary`, `supplementary_literal`,
  `cut_prefix`, `rewrite_body`;
* `lib/plan.c`: `place_atom`, `place_negated`, `emit_body`,
  `planner_init`;
* `lib/rule.c`: `compile_atom`, `compile_head`, `store_variables` and their
  callers, `compile_query_head`, `prepare_query`;
* `lib/stratify.c`: `assign_strata`, `order_by_stratum`, `build_schedule`,
  `analyse_program`;
* `lib/parse.c`: `append_statement`.

The exhaustion suite refuses the nth `malloc`, and an arena calls `malloc`
only when it opens a chunk, so each of these fails only when a program
happens to be sized to open a chunk at that request; `-l memory=N` sweeps
land on the same boundaries. One fault point in the exhaustion build that
refuses the kth `relic_arena_alloc` of any arena, swept across the existing
scenarios, would reach nearly all of them at once, and would keep reaching
new ones as the code grows, which sizing a program per branch would not.

Several are reachable with the present `malloc` sweep given a scenario it
lacks:

* `lib/lex.c` `scan_string_escape` and `scan_string`: a load whose first
  string literal starts with an escape, and one with a string of about 5000
  bytes, which needs an oversize chunk;
* `lib/parse.c` `collect_body`: a body longer than the scratch array's
  first capacity, about twenty literals;
* `lib/parse.c` `advance`, `parse_literal` and `parser_init`: a load whose
  constant, or whose interning of `not` or `_`, lands on the value or
  symbol table's growth point;
* `lib/parse.c` `read_stream`: `relic_load_file()` over a stream, which the
  sweep never calls;
* `lib/parse.c` `add_fact`: `relic_fact_add()` with a predicate never seen;
* `lib/plan.c` `place_atom`, `place_negated` and `score_positive`: a body
  naming a predicate with neither facts nor rules, whose relation is first
  made while planning;
* `lib/rule.c` `append_loaded_query` and `copy_loaded_query`: a check over
  more queries than the table's first capacity, and `relic_query_loaded()`,
  which the sweep never calls.

## Branches unreachable by construction

[Low] About seventy ranges are defensive: the `default` of a switch over
every enumerator, a lookup of an identifier a live plan holds, an overflow
guard on a product that fits a 64-bit `size_t`, a table reaching 2^32
entries, a check that a checked rule is safe, and
`reads_whole_on_demand()` in `lib/magic.c`, which guards a negation or an
aggregate against reading a predicate the rewrite evaluates only on demand.
They are kept deliberately.

One is worth a second look, since its reason for existing may have gone
rather than being a guard: `relic_diag_no_memory()` in `lib/diag.c` formats
a memory refusal with no activity recorded, and every allocation charged to
a limit follows an activity note. Deleting the branch or turning it into an
assertion is the choice to make.

`write_json_string` in `cmd/relic/output.c` escapes bytes no symbol can
hold, since strings reach JSON through `put_literal`; it is a generic
escaper kept generic.

The overflow guards in `lib/arena.c`, `lib/util.c`, `lib/relation.c` and
`lib/index.c` can be reached from a unit test by a direct call with a size
near `SIZE_MAX` or a forged table; the existing arena test passes `SIZE_MAX`
itself, which an earlier check catches first.
