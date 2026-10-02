# Coverage gaps

`make coverage` lists every range of lines the suite never executes. Each
range was sorted into one of the kinds below; a range an ordinary input
reaches is covered by a test rather than listed here. Line numbers drift, so
each gap is named by its function.

## Allocation failures the sweep cannot reach

[Low, component: `tests/unit/test_exhaustion.c`] The exhaustion suite
refuses the allocator's requests, and through the arena fault point every
arena request of a program loaded, checked and answered both through the
Magic rewrite and from the whole fixed point. What neither reaches is an
intern that fails only when it lands on a table's growth point: in
`lib/parse.c`, `parse_literal` and the `advance` of `collect_body`, for a
load's constant or its interning of `not` or `_`; in `lib/magic.c`,
`relic_magic_find_complete` interning a base view; and in `lib/rule.c`,
`compile_head` interning a head's constant. A context filled to one entry
short of a growth before the operation would reach them.

## Branches unreachable by construction

[Low] About seventy ranges are defensive: the `default` of a switch over
every enumerator, a lookup of an identifier a live plan holds, an overflow
guard on a product that fits a 64-bit `size_t`, a table reaching 2^32
entries, a check that a checked rule is safe,
`reads_whole_on_demand()` in `lib/magic.c`, which guards a negation or an
aggregate against reading a predicate the rewrite evaluates only on demand.
They are kept deliberately.

`write_json_string` in `cmd/relic/output.c` escapes bytes no symbol can
hold, since strings reach JSON through `put_literal`; it is a generic
escaper kept generic.

The overflow guards in `lib/relation.c` and `lib/index.c` -- a count past
`RELIC_TUPLE_MAX`, a projection that saturates at `SIZE_MAX` -- can be
reached from a unit test only through a relation or an index forged to hold
a count near those limits.
