/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_INTERNAL_H
#define RELIC_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relic.h>

#include "compat.h"
#include "queue.h"
#include "util.h"

/*
 * Types and prototypes shared across lib/.  Nothing here is installed and
 * nothing here is exported from the shared library: every symbol declared
 * below has external linkage so that the modules can call one another, and
 * hidden visibility so that a program linked against librelic cannot.
 */

/*
 * Mark a parameter the implementation does not use.  A function whose shape
 * is fixed by a callback type will have some; writing the attribute is
 * clearer than a (void) cast in the body, which reads like a statement that
 * does something.
 */
#if defined(__GNUC__)
#define	RELIC_UNUSED	__attribute__((__unused__))
#else
#define	RELIC_UNUSED
#endif

/*
 * Ask the compiler to check a printf-style call against its format string.
 * The diagnostic interface is the one place in the library where a format
 * string and its arguments are separated by a call boundary.
 */
#if defined(__GNUC__)
#define	RELIC_PRINTFLIKE(fmt, args) \
	__attribute__((__format__(__printf__, fmt, args)))
#else
#define	RELIC_PRINTFLIKE(fmt, args)
#endif

typedef struct relic_arena relic_arena_t;
typedef struct relic_arena_chunk relic_arena_chunk_t;
typedef struct relic_arena_chunk_list relic_arena_chunk_list_t;
typedef struct relic_diag relic_diag_t;
typedef struct relic_symbol relic_symbol_t;
typedef struct relic_symtab relic_symtab_t;
typedef struct relic_valtab relic_valtab_t;

/*
 * A value, interned.  Every value relic handles -- integers included -- is
 * reduced to one of these, so a tuple column is four bytes whatever it holds
 * and a join compares plain integers without inspecting a payload.
 *
 * Identifiers are assigned in order of first appearance, so they are
 * reproducible across runs, but their numeric order records only the order
 * the input mentioned things in.  Nothing may sort by a domain identifier;
 * ordering goes through relic_value_order() or relic_value_total_order().
 */
typedef uint32_t relic_dom_t;

/*
 * The result of comparing two values.
 *
 * Ordering is defined only within a type: a mixed-type pair is unordered
 * rather than erroneous, because predicate arguments are untyped and a column
 * may legitimately hold more than one type.  The evaluator turns
 * RELIC_ORDER_UNORDERED into a filtered-out binding.
 */
enum relic_order {
	RELIC_ORDER_LESS = -1,
	RELIC_ORDER_EQUAL = 0,
	RELIC_ORDER_GREATER = 1,
	RELIC_ORDER_UNORDERED = 2
};

/*
 * One allocation holding this header and, after it, the payload that
 * relic_arena_alloc() hands out in pieces.
 */
struct relic_arena_chunk {
	SLIST_ENTRY(relic_arena_chunk)	 link;
	size_t				 size;	/* payload bytes */
	size_t				 used;	/* handed out, at most size */
};

SLIST_HEAD(relic_arena_chunk_list, relic_arena_chunk);

/*
 * A region whose allocations share a lifetime and are released together.
 *
 * Once an allocation fails, `failed` stays set and every later request is
 * refused without reaching the allocator.  Callers check the returned
 * pointer as usual; the flag lets a helper that is already several
 * allocations into a structure stop without threading a status back through
 * every level, and it lets the phase that owns the arena confirm at its
 * boundary that nothing was quietly truncated.
 */
struct relic_arena {
	relic_arena_chunk_list_t	 chunks;	/* newest first */
	size_t				 next_chunk_size;
	size_t				 total_bytes;	/* from the allocator */
	bool				 failed;
};

/*
 * Where diagnostics go, and the text of the last error.
 *
 * With no callback installed, diagnostics are discarded and an error is still
 * recoverable through relic_error().  Silence is the right default for a
 * library: a caller must not be made to accept output on its stderr.
 */
struct relic_diag {
	relic_diag_cb	 callback;
	void		*callback_ctx;
	char		 last_error[512];
	bool		 has_error;
};

/*
 * One interned byte string, found at index `id - 1` of the symbol table.
 *
 * The bytes live in the context arena, where they never move, so a pointer
 * handed out by relic_string() stays valid until the context is destroyed
 * even after the array of these has been reallocated several times.
 *
 * The length is recorded because a string literal may contain NUL bytes: the
 * lexer is byte-oriented and a string holds whatever bytes the source did.  A
 * terminator is stored past the recorded length anyway, so that a caller with
 * no use for the length can treat the result as a C string.
 */
struct relic_symbol {
	const char	*bytes;
	size_t		 length;
};

/*
 * Byte strings interned to 32-bit identifiers.
 *
 * Symbols and string literals share this pool.  What distinguishes the symbol
 * foo from the string "foo" is the value that refers to the bytes, not the
 * bytes themselves, so the two intern to one symbol identifier and to two
 * different domain identifiers.
 */
struct relic_symtab {
	relic_symbol_t	*entries;	/* indexed by id - 1 */
	size_t		 count;
	size_t		 capacity;
	relic_idtab_t	 index;
};

/*
 * Values interned to domain identifiers.
 *
 * Interning happens here for every type, integers included, so that the
 * parser and relic_fact_add() cannot disagree about a value's canonical
 * form: there is one path to a domain identifier and this is it.
 */
struct relic_valtab {
	relic_value_t	*entries;	/* indexed by dom - 1 */
	size_t		 count;
	size_t		 capacity;
	relic_idtab_t	 index;
};

/*
 * A relic context owns everything it allocates, and two contexts share
 * nothing: the library holds no mutable global state at all, which is what
 * lets an embedding program keep several contexts without a lock.  A single
 * context is not itself thread-safe.
 */
struct relic {
	relic_arena_t	 arena;		/* context lifetime: symbol bytes */
	relic_symtab_t	 symbols;
	relic_valtab_t	 values;
	relic_diag_t	 diag;
	relic_config_t	 config;
};

/*
 * lib/arena.c
 *
 * relic_arena_reset() returns an arena to its initialized state and releases
 * all of its memory, so it serves both as a reset between phases and as the
 * teardown operation.  There is no per-allocation free: the point of an arena
 * is that a phase's allocations are forgotten together.
 */
void	*relic_arena_alloc(relic_arena_t *arena, size_t size);
void	 relic_arena_clear_failure(relic_arena_t *arena);
bool	 relic_arena_failed(const relic_arena_t *arena);
void	 relic_arena_init(relic_arena_t *arena);
void	 relic_arena_reset(relic_arena_t *arena);
size_t	 relic_arena_total_bytes(const relic_arena_t *arena);

/*
 * lib/api.c
 *
 * Allocate from the context arena, reporting exhaustion and returning NULL.
 *
 * Every context-lifetime allocation goes through here rather than calling
 * the arena directly, because that arena is never reset while the context
 * lives.  A failure flag left standing on it would refuse every later
 * request and turn one transient exhaustion into a context that can no
 * longer intern anything, so the flag is cleared here, once the failure has
 * been observed and reported.
 *
 * The flag's other purpose, letting a half-finished construction stop
 * without threading a status back through every level, belongs to the arenas
 * that are reset between phases.  A context-lifetime allocation is a single
 * object whose caller checks the pointer it gets.
 */
void	*relic_context_alloc(relic_t *ctx, size_t size);

/*
 * lib/diag.c
 *
 * relic_diagf() formats a diagnostic, records it as the last error when its
 * level is RELIC_ERROR, and hands it to the installed callback.  A message
 * longer than the buffer is truncated rather than allocated for: a
 * diagnostic reports a failure and must not be able to cause one.
 */
void	 relic_diag_init(relic_diag_t *diag);
void	 relic_diagf(relic_t *ctx, enum relic_diag_level level,
	    const relic_location_t *where, const char *format, ...)
	    RELIC_PRINTFLIKE(4, 5);

/*
 * Report that an allocation failed, in the one wording the whole library
 * uses.  Out-of-memory is the failure every module can hit and the one whose
 * message carries the least information, so it is written once.
 */
void	 relic_diag_no_memory(relic_t *ctx);

/*
 * lib/symbol.c
 */
const char *relic_symbol_bytes(const relic_t *ctx, uint32_t id,
	    size_t *lengthp);
size_t	 relic_symbol_count(const relic_t *ctx);
int	 relic_symbol_intern(relic_t *ctx, const void *bytes, size_t length,
	    uint32_t *idp);
void	 relic_symtab_init(relic_symtab_t *table);
void	 relic_symtab_reset(relic_symtab_t *table);

/*
 * lib/value.c
 *
 * relic_value_of() returns NULL for an identifier the context never assigned.
 * Both order functions treat such an identifier as unordered and unequal
 * rather than reading past the table.
 */
size_t	 relic_value_count(const relic_t *ctx);
const relic_value_t *relic_value_of(const relic_t *ctx, relic_dom_t dom);
enum relic_order relic_value_order(const relic_t *ctx, relic_dom_t a,
	    relic_dom_t b);
int	 relic_value_total_order(const relic_t *ctx, relic_dom_t a,
	    relic_dom_t b);
int	 relic_value_intern(relic_t *ctx, const relic_value_t *value,
	    relic_dom_t *domp);
int	 relic_value_intern_bytes(relic_t *ctx, enum relic_type type,
	    const void *bytes, size_t length, relic_dom_t *domp);
int	 relic_value_intern_integer(relic_t *ctx, int64_t value,
	    relic_dom_t *domp);
void	 relic_valtab_init(relic_valtab_t *table);
void	 relic_valtab_reset(relic_valtab_t *table);

#endif /* RELIC_INTERNAL_H */
