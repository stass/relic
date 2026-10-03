# relic error class design

**Status.** Design document for the class of an error: the kinds a failure
falls into, how every error carries its kind by construction, the entry
point a caller reads it through, and what the tool and the test harnesses
do with it. Forward-looking, like every document under `specs/`; `TODO.md`
records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 56, 57 and 63;
`specs/design-api.md` sections 2, 7 and 9 for the interface this extends;
`specs/design-eval.md` section 8 and `specs/design-arithmetic.md` section
4.2 for the evaluation failures; `specs/design-relations.md` section 9 for
a limit's refusal; `specs/design-interrupt.md` for the interrupt;
`specs/design-hardening.md` section 5 for the fuzz targets that read the
class; `specs/decisions.md` D-5, D-7 and D-32.

---

## 1. What this document settles

* why a failure needs a kind beside its text, and what the kinds are;
* the rule that decides a failure's kind where two could apply;
* how a kind reaches every error without a list of call sites to keep;
* the entry point, its lifetime rules, and what a callback may read;
* what changes in the tool, which is nothing, and in the tests, which is
  that nothing matches message text any more.

---

## 2. Why a class exists

D-5 gives a failure two faces: a return of -1 or NULL, and at least one
RELIC_ERROR diagnostic whose last text `relic_error()` returns. The text is
for a person. A program that drives relic cannot read it without matching
words, and the words are the one part of a diagnostic the library may
improve from one release to the next.

Two drivers need the distinction. An embedder that evaluates a program it
does not trust must tell "the program is wrong" from "the program is too
big for the limits I set" from "this process is out of memory"; the first
is the program's author's problem, the second a configuration, the third
an emergency. And a reference implementation run against another
implementation of the language must agree with it on *why* a program
fails, not only that it does: an overflow is an answer the language
defines, `specs/design-arithmetic.md` section 4.2, and a limit's refusal
is not.

Today both tell a limit apart by the prefix `limit exceeded:`, and the
evaluation fuzz target tells an evaluation error from a crash by five
message prefixes. A class frees the wording from that duty.

---

## 3. The classes

```c
enum relic_error_class {
	RELIC_ERROR_NONE,	/* no error has been recorded */
	RELIC_ERROR_REJECTED,	/* the text given was refused */
	RELIC_ERROR_EVALUATION,	/* a result the language leaves undefined */
	RELIC_ERROR_LIMIT,	/* a limit refused the operation */
	RELIC_ERROR_IO,		/* a file could not be opened, read or closed */
	RELIC_ERROR_MEMORY,	/* the allocator refused */
	RELIC_ERROR_INTERRUPTED,/* the caller interrupted the operation */
	RELIC_ERROR_ARGUMENT,	/* the caller misused the interface */
	RELIC_ERROR_INTERNAL	/* an invariant the library checks failed */
};
```

| Class | What it covers | Where it comes from |
| --- | --- | --- |
| REJECTED | a lexical or syntax error; a safety or stratification failure; a type error the parser can see; an aggregate or function misused; a query that does not parse or check; a table field that is not a value of its column's type; a program that cannot be used because an earlier check failed | the parser, the checker, the import reader |
| EVALUATION | integer overflow, division by zero, a string longer than a value can hold, a pattern that is not a regular expression; a `sum` that does not fit | the executor's failure, reported by the evaluation |
| LIMIT | a configured limit refused an insertion, an index, a rewrite or an allocation; a fixed capacity was reached: the tables of symbols or integers full, too many predicates, a relation too large to grow | `relic_diag_limit()`, and the capacity sites |
| IO | `cannot open`, `cannot read`, `cannot close` on a source or a table | the loader and the import reader |
| MEMORY | an allocation failed and no limit was the cause | `relic_diag_no_memory()` |
| INTERRUPTED | `relic_interrupt()` was called during the operation | the evaluation, `specs/design-interrupt.md` section 3 |
| ARGUMENT | a NULL argument, a value or query of another context, no query at that index, an import description that contradicts itself, a call made from the context's own callback | the entry points' checks, and the re-entry refusal |
| INTERNAL | a diagnostic prefixed `internal error:` | the sites that report an invariant the library found false |

**The rule where two could apply.** A failure's class is what the caller
can do about it. A limit and a capacity are both LIMIT because the answer
to both is "give it less, or configure more"; the text names which. The
memory limit's refusal is LIMIT and the allocator's is MEMORY, which the
reporting function already tells apart. A string too long in the program
text is REJECTED, because the text can be fixed; the same length reached
by `cat` during evaluation is EVALUATION, because the language says the
result has no value, `specs/design-strings.md` section 4.3. A table field
that is not an integer is REJECTED and a table that cannot be read is IO,
as for a program. An interrupted evaluation fails as a limit fails it but
is its own class, since the caller asked for it.

A check that reports several errors in one call, as the parser does after
it resynchronizes, records the last one's class, as `relic_error()` keeps
the last one's text. Every error of one check has the same class, so
which of them is last does not matter; where it could, as when the
allocator fails in the middle of a parse, the last error is the one that
stopped the operation, and its class is the one the caller needs.

The enumeration is part of the interface. A value is never renumbered or
removed; a class added later takes the next value, and a caller that
switches on the class treats one it does not know as a failure it cannot
classify, which is what every class was before this design.

---

## 4. Every error has a class by construction

The library reports through one function, `relic_diagf()`, which takes a
level; at RELIC_ERROR it records the text as the last error. A class could
be stored by the same function, if every error site passed one, and the
correctness of that would rest on a list of about a hundred sites, each
free to pass the wrong class or none.

Instead the level and the class become one argument. A note and a warning
are reported through functions that take no class, and **an error is
reported only through a function that takes one**:

```c
void	relic_diag_note(relic_t *, const relic_location_t *, const char *, ...);
void	relic_diag_warning(relic_t *, const relic_location_t *, const char *, ...);
void	relic_diag_error(relic_t *, enum relic_error_class,
	    const relic_location_t *, const char *, ...);
```

There is no way to record an error without naming its class, so the
property does not depend on anyone remembering. The sites that already
funnel a kind of failure keep funnelling it: `relic_diag_limit()` passes
LIMIT, `relic_diag_no_memory()` MEMORY or LIMIT, the parser's error
function REJECTED, the evaluation's failure report EVALUATION, the import
reader's format errors REJECTED. The internal errors have a funnel of
their own, `relic_diag_internal()`, which supplies the `internal error:`
prefix and the class together, so a site cannot have one without the
other. What remains is the sites that report a kind nothing funnels, each
naming its class where the compiler demands it.

The class is recorded in the context beside the text, by the one function
that records the text, **before the callback is called**. A callback that
wants the class reads it through the entry point of section 5, which does
not enter the context and so may be called from a callback, as
`relic_error()` may.

The re-entry refusal, a call made while the context is busy, records no
text and answers `relic_error()` with a fixed message. It answers the
class entry point with ARGUMENT by the same rule.

---

## 5. The entry point

```c
RELIC_EXPORT enum relic_error_class relic_error_class(relic_t *ctx);
```

It returns the class of the error `relic_error()` returns the text of, and
NONE when that is NULL. The two have one lifetime: a class is set when an
error is recorded and stands until the next error is recorded, and a
later success clears neither, so a caller reads both right after the call
that failed. Like `relic_error()` it does not enter the context, so a
callback may call it, and a caller may call it on a context another call
has refused.

Nothing else in the interface changes. The diagnostic callback's signature
is a typedef callers have written functions against, and a class is not
added to it: a callback that wants one calls the entry point. The levels
are unchanged, and a warning has no class, since a warning is not a
failure; what a strict mode makes of a warning is a question for that
design, and it would report the resulting failure as REJECTED.

An added entry point is a compatible change under `specs/design-api.md`
section 9, `c+1:0:a+1`, and the enumeration is an added type the same
rule covers.

---

## 6. The tool

The tool's exit statuses are D-5's and do not change: 0, 1 for a program
or query error, 2 for usage. Every class but ARGUMENT exits 1, and the
class does not choose the status. The tool is a caller that reads
diagnostics as text for a
person, which the class does not replace; it does not print the class, so
no golden output changes.

---

## 7. Tests

Every place in the tree that matched an error's text to learn its kind
reads the class instead:

* `tests/fuzz/fuzz_eval.c` tells a limit's refusal, an evaluation error
  and anything else apart by class, and treats INTERNAL as the finding it
  is;
* the random suite's failure kinds that name an evaluation error,
  `specs/design-hardening.md` section 3.4, are decided by class and by
  the executor's failure, not by text;
* the exhaustion suite checks that an allocation the sanitizer refuses is
  MEMORY and one the limit refuses is LIMIT.

The unit tests assert the class at each funnel and at one site of each
kind nothing funnels, so a site that passes the wrong class is caught
where it is, and the re-entry refusal's class is pinned with its text.
The language tests compare text, as they should: the text is the user's
interface and the class is the program's.

---

## 8. Determinism

A class is a function of which failure happened, which is a function of
the program, the limits and the allocator's answers, as the text already
is. Nothing here reads a clock or a pointer.

---

## 9. What is deliberately absent

* **A code per message.** A class is coarse on purpose: it answers "what
  can the caller do", and the text answers "what happened". Numbering
  messages would freeze their wording, which is the property this design
  exists to avoid.
* **Stable message text.** No document promises it and this one does not
  start.
* **A class in the callback.** The callback's type is frozen; the class is
  a call away.
* **An `errno` mapping.** D-5 keeps `errno` for system calls; an IO class
  says a file failed and the text says which, and a caller that wants the
  system's reason has `errno` as the loader left it.
* **A class for a table apart from a program.** Both are text the caller
  handed over and both are REJECTED; the location says which file. A
  caller that must tell them apart knows which call it made.
* **A warning count or class.** A warning is not a failure. The strict
  mode that would make one into a failure is a design of its own.
