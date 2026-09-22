# relic code style

relic follows FreeBSD `style(9)` (Kernel Normal Form), with the project
conventions below. These rules apply to relic-owned code; preserve upstream
style and license notices in bundled and vendored sources, such as
`compat/queue.h` and `compat/tree.h`.

**Human readability is a requirement.** Code should make its purpose, data
flow, invariants and failure paths understandable to another developer.
Use clear names and straightforward control flow, separate logical phases
visually, and explain non-obvious reasoning or ownership in concise comments.
Prefer several clear steps when combining operations would obscure their order
or purpose. Review readability explicitly before committing; passing tests and
mechanical formatting checks alone do not establish it.

## Readability standard

Write for a maintainer encountering the code for the first time. The main
flow should read almost like English: validate the inputs, prepare the result,
perform the operation, and publish the result. Use the actual operations of
the subsystem in those steps. The reader should see what a function accomplishes
before having to understand how its arrays are laid out.

Before writing nontrivial code, read the analogous functions already in the
tree and identify what makes their structure understandable. The qualities to
look for, and to aim at:

| Quality to follow | Where it matters in relic |
| --- | --- |
| Named stages with immediate error checks make execution order visible; a file comment explains why a failed stage prevents later ones from running. | `compile_query()` in `lib/rule.c`, which states why the body precedes the head and the name comes last; `run()`, `validate()` and `evaluate()` in `cmd/relic/main.c`. |
| Validation precedes reading and state advances only after success; the explanation includes the boundary case that motivates the algorithm. | `relic_relation_insert()` and `relation_reserve()` in `lib/relation.c`, which gather every fallible step ahead of every mutation and say what a tuple stored but not indexed would break. |
| Allocation, rehashing and installation of a new table are distinct steps; the comment explains why the fallible step must come first. | `relic_symbol_intern()` in `lib/symbol.c`, over `relic_idtab_reserve()` and `relic_idtab_place()` in `lib/util.c`. |
| Names carry their role and saved state, and comments explain restoration obligations -- or why there are none. | `execute_plan()`, `step()` and `struct relic_evaluation` in `lib/eval.c`: `level` and `resume` say what the loop is doing, and the frame's comment says why nothing is saved or restored across join levels. |

Name concrete functions in this table as the tree acquires them, choosing ones
that genuinely read well rather than whichever happen to exist. Existing files
may contain older formatting, naming, or comment conventions; their presence
does not override this guide.

### Structure around meaningful operations

A function should have one coherent responsibility and a clear level of
detail. An operation that coordinates several stages should express those
stages through named helpers. A helper that implements a traversal may show
its indexing directly. Avoid mixing the semantic decision, buffer growth,
label copying, and index translation in one deeply nested loop.

Extract a helper when its name expresses a meaningful operation or its contract
contains reasoning the caller should not have to repeat. Do not create a
helper for every assignment or scatter a simple operation across many tiny
functions. The caller must remain understandable without opening every callee.
Prefer a little explicit repetition to a generic framework that hides the
algorithm. Function length alone does not measure readability.

Keep the normal path visible. Reject invalid input early, skip irrelevant
entries with `continue` when appropriate, and put distinct phases in separate
blocks. If nesting comes from several responsibilities, separate those
responsibilities instead of merely moving the same tangle into another function.

### Names and representations must explain the data

Use verbs and domain nouns for operations: a name such as
`add_constructor_alternative` explains more than `process_entry`. Predicate
names should describe a question; a function that constructs a graph or performs
fallible analysis needs an action name and an explicit failure contract.

Name values by their role where ambiguity matters. Distinguish a vertex ID
from an array index, a parameter ordinal from a parameter count, a byte offset
from a bit offset, and a queue's occupied length from its unprocessed length.
For example, `argument_root`, `destination_vertex_index`, and `saved_limit_bit`
carry information that `root`, `vertex`, and `saved` may lose. A short `i` or
`src` is fine in a small loop with one obvious interpretation. Do not burden
every name with context already clear from its scope.

Represent a logical object explicitly. Prefer an edge with named `source` and
`target` fields to an integer vector whose even and odd positions have different
meanings. Use enums or designated fields when positional booleans or numeric
tags would require the reader to memorize a convention. Compact representations
are appropriate when justified by the format, algorithm, or measured cost;
document their meaning once near the representation and localize their encoding
and decoding. Do not spread sentinel handling and index conversions through
unrelated semantic code.

### Make execution and failure handling explicit

Do not hide several state-changing steps in `&&`, `||`, a ternary, or nested
function arguments. Give each meaningful operation its own statement and check
its result before continuing. Compound pure predicates and a direct check of
one fallible operation remain ordinary, readable C.

For example, these illustrative helpers copy two parts of a result and report
failure with `false`. Avoid compressing their sequence:

```c
return (copy_scheme(builder) && copy_arguments(builder));
```

Make the stages visible:

```c
if (!copy_scheme(builder))
	return (false);

if (!copy_arguments(builder))
	return (false);

return (true);
```

The helper names must also describe coherent operations; expanding a boolean
expression alone is not enough. Preserve the existing failure contract when
restructuring code. In particular, distinguish invalid input, resource exhaustion,
and internal failure where the interface distinguishes them.

Make ownership and publication easy to locate. Show what is borrowed, what is
owned, and what cleans up after an unsuccessful operation. Prefer preparing a
complete object before making it visible to its consumer. If construction is
incremental, make the partial state and its cleanup contract explicit. Keep
count updates separate from complex assignments when they determine which
entries are initialized or visible. Resource accounting and bounds checks must
remain part of the operation's contract; do not remove them to shorten the flow.

### Explain the model before the mechanics

For a non-obvious algorithm, give a self-contained explanation near its
implementation: what its graph or work item represents, how information moves,
and what makes the process finish with the required result. State representation
conventions where they are defined. Explain a surprising boundary case with a
small example when that is clearer than abstract shorthand.

For instance, saying that each work item enters a queue once is insufficient
if the reader cannot tell what an item means, what creates it, or how it affects
the result. Explain those relationships, then let the code express the individual
steps. The explanation may need several sentences or paragraphs. A one-line
comment is not inherently better. Do not use comments to compensate for names
or helper boundaries that can be made clearer.

## Readable tests

Tests follow the same standard as production code. Name the scenario and make
its setup, operation, and expected result distinct. Use fixture helpers that
express the value or graph being built; avoid nested mutations in call arguments
that force the reader to reconstruct the fixture's state. Use named fields when
positional data would obscure the case being tested.

Keep an independent oracle understandable on its own. Explain the property it
checks and how it differs from the production approach; copying the implementation
into the test does not provide independent evidence. A failed assertion should
identify the relevant case and expectation without requiring a debugger to
discover what the test intended.

## Layout and whitespace

- Use tabs for indentation, with tab stops every eight columns. Indent
  continuations four columns beyond the statement's indentation, using tabs
  followed by spaces. Never put spaces before tabs or leave trailing whitespace.
- Aim for 80 columns. Keep searchable diagnostic strings intact; a slightly
  longer line is acceptable when it reads better.
- Separate logical blocks with one blank line: declarations, validation,
  preparation, processing, and cleanup should be visually distinct when they
  perform separate tasks. Keep closely related statements together; do not
  insert a blank line after every statement.
- Put one blank line between declarations and statements, and between functions.
  Do not add a blank line immediately inside a block with no declarations or
  before its closing brace.
- Put spaces after control keywords and commas, and around binary operators.
  Do not put spaces after function names, inside parentheses or brackets, or
  between unary operators and their operands.
- Attach pointer stars to the name: `const char *name`, `data_fill_t *fill`.
  Write casts and `sizeof` without a following space: `(size_t)n`,
  `sizeof(*fill)`. Always parenthesize the operand of `sizeof`.
- Wrap long expressions with the operator at the end of the preceding line.
  Add parentheses where they clarify precedence.
- Source files must be ASCII-only, including comments and strings. Markdown
  prose is exempt.

## Names, types, and declarations

- Use `lower_snake_case` for functions, variables, struct tags, and typedefs.
  Use uppercase names for constants, enum values, and macros with side effects.
  Avoid identifiers reserved to the implementation.
- **Give every relic-defined struct a matching `_t` typedef and use that typedef
  in declarations, fields, parameters, and return types.** For example,
  `typedef struct data_fill data_fill_t;`. Keep the struct tag for its definition
  and interfaces that require a tag, such as `queue(3)` macros.
- Typedef the struct itself, never its pointer: write `data_fill_t *`, not a
  typedef that hides the indirection. Define each typedef in its owning header
  or source file; include that header where needed instead of duplicating it.
  Forward typedefs support opaque and self-referential types:

```c
typedef struct data_fill data_fill_t;

struct data_fill {
	data_fill_t	*next;
	size_t		 length;
	uint8_t		 value;
};
```

- Put shared types in the appropriate header and private types near the top
  of their source file. Separate type definitions from object declarations;
  use `extern` for shared object declarations in headers.
- Give each struct member its own line. Order members by logical use, then
  size (largest first), then name. Align member names with one or two tabs
  where practical; use one space after unusually long types.
- Put local declarations at the start of their block, ordered by size and
  then name. Simple initializers are fine; put complicated calls in the
  statement block. A `for` loop may declare its counter in the initializer.
- Prefer standard fixed-width types such as `uint32_t`, and `bool`, `true`,
  and `false` from `<stdbool.h>`. Use designated initializers when they help
  identify fields, and `_Static_assert()` for compile-time assertions.
- Mark unused parameters and locals with `RELIC_UNUSED` from `lib/internal.h`;
  do not use `(void)arg;` casts.

## Functions and control flow

- Prefer functional style: separate pure computation from code with side
  effects where practical. Create reusable functions without side effects
  when doing so does not add complexity.
- Declare function prototypes at file scope: private `static` functions near
  the top of the source file, shared functions in their owning header. Keep
  prototypes in logical order, alphabetically where practical. Use prototype
  syntax, including `(void)` for no parameters; never use K&R declarations.
- Put a function definition's return type on its own line and its opening
  brace on the next line after the parameter list.
- Put control-flow opening braces on the same line as the condition. Write
  `} else {` on one line. Braces around single statements may be omitted when
  clear; be consistent within each function.
- Align `case` and `default` with `switch`, and indent their bodies one tab.
  Mark intentional fallthrough with `/* FALLTHROUGH */`.
- Use `for (;;)` for an indefinite loop. Put an empty loop body's semicolon
  on its own line.
- Compare pointers with `NULL`, numbers with `0`, and characters with `'\0'`.
  Only booleans should be tested directly or with unary `!`.
- Use `NULL` without a cast when the pointer type is known. Supply the required
  pointer type for a null variadic argument. Do not cast a `void *` result in C.
- Parenthesize returned values: `return (result);`. A void function may end
  without a `return` statement.

## Comments

- Add a concise comment before a logical block only when its purpose,
  constraint, or reasoning is not obvious from the code. Explain the reason
  for a check or ordering requirement instead of narrating statements.
- Make comments self-contained and self-explanatory. State the relevant
  invariant or contract directly; do not refer to design documents or require
  the reader to follow a document path, section number, or link.
- Do not put temporal or campaign markers in comments: change history, dates
  of fixes, phase labels, issue references, and commit references belong in
  commit messages. Preserve required copyright and license notices.
- Use enough sentences to explain the reasoning. Keep a simple observation
  short; give an algorithm, representation, or subtle contract the explanation
  it needs. Document non-obvious ownership, lifetime, units, failure behavior,
  and concurrency requirements where needed. Remove repetition and irrelevant
  detail, not information the reader needs to understand the code.
- Keep comment syntax consistent within each file. Use `/* ... */` for short
  comments in C-style files; use aligned leading `*` lines for a necessary
  multiline comment. Avoid decorative banners and section-number hierarchies.

This example separates validation from the result and explains a format
constraint that the arithmetic alone does not convey:

```c
static bool
data_fill_fits(const data_fill_t *fill, size_t capacity)
{
	size_t available;

	/* The output format reserves one byte for a terminator. */
	if (capacity == 0)
		return (false);
	available = capacity - 1;

	return (fill->length <= available);
}
```

## File organization and headers

- Keep required copyright and SPDX notices at the top in a comment starting
  in column one, followed by a blank line. Preserve attribution in imported
  code; do not add revision-history annotations to relic code.
- Group includes in this order, with a blank line between groups:
  1. System headers (`<sys/...>`). Put `<sys/types.h>` or `<sys/param.h>`
     first when needed; do not include both. Sort the rest alphabetically.
  2. Network headers, when needed.
  3. Other library headers, sorted alphabetically.
  4. Project headers, sorted alphabetically.
- Use angle brackets for system and installed library headers, and double
  quotes for local headers. The public header `include/relic.h` must remain
  strict C11, must not include `config.h`, and must be includable from C++.
- Keep function declarations outside functions. Headers must include what
  they need so their users do not depend on a particular include order.

## Macros and conditional compilation

- Prefer established library facilities to custom machinery, including
  `queue(3)` macros for linked lists where suitable.
- Parenthesize expression-like macro expansions and their arguments where
  needed for precedence. Avoid evaluating an argument more than once.
- Wrap statement macros in `do { ... } while (0)`, leaving the final
  semicolon to the caller. Align continuation backslashes.
- Use a consistent space or tab after `#define`. A macro replacing a function
  uses the uppercase version of that function's name.
- Add condition comments to `#endif` or `#else` only for long or confusingly
  nested regions. Match the opening condition for `#endif` and the inverse
  condition for `#else`; separate the comment with one space.
- Use branch-prediction hints only when measurements show a benefit in hot
  code, and apply them to the complete condition.

## Library calls and command-line code

- Prefer the BSD memory primitives from `<strings.h>` when they fit:

  | Operation | Preferred form |
  | --- | --- |
  | Zero memory | `bzero(buf, n)` |
  | Copy memory | `bcopy(src, dst, n)` |
  | Compare for equality | `bcmp(a, b, n) == 0` |

  Watch `bcopy`'s source-first argument order. Use `memset` for nonzero fills,
  `memcmp` for ordering, and `memcpy` when its call-site types make it clearer.
- Use the project's error interfaces in library code. CLI diagnostics should
  use `err(3)` / `warn(3)` and their variants where appropriate. Prefer
  `printf(3)`-family calls for formatted output.
- Parse CLI options with `getopt(3)` and single-letter flags. Keep each
  letter's project-wide meaning; use a repeatable `-l key=value` for resource
  limits. Validate numeric operands, including range and trailing characters.
- Sort option strings, switch cases, and option descriptions alphabetically
  unless intentional fallthrough requires another order.
- Format usage like a manual-page synopsis: grouped flags without operands,
  separately bracketed options with operands, required arguments, then
  bracketed optional arguments. Use `|` for alternatives. Exit with zero on
  success and one on ordinary failure.

## Applying the guide

Keep changes focused and consistent. Follow these conventions when creating
or editing code; make broad style sweeps deliberate, reviewable work. Compile
with the project's warnings enabled and use its required reviews, tests, and
sanitizers for the behavior affected.

### Readability review

Before submitting code, read the main path as if you had not written it. Explain
its operation in plain language using its function and variable names. If that
requires translating vague names, tracing positional arrays, or consulting the
conversation, revise the code. This is a comprehension check, not a requirement
to add a comment above every step.

The required review must assess structure and explanation, as well as formatting.
Compare the changed code with the qualities listed above. Cite concrete
functions or passages in the review and explain where the flow is understandable
and where the reader must reconstruct missing meaning. Check whether the reader
can follow the algorithm, distinguish the values it manipulates, and identify
state changes, failure handling, and ownership from the implementation and its
local contracts.

Treat difficult-to-follow code as a finding even when it is correct, formatted,
and commented. Fix the cause: unclear responsibility, unsuitable representation,
hidden sequencing, vague naming, or missing reasoning. Adding whitespace and
comments is insufficient when the structure itself needs to change. Address
readability findings before committing; passing tests does not resolve them.
