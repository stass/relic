/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_H
#define RELIC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The version relic was compiled against.  Only the three components are
 * written down; the string and the ordered number are derived from them, so
 * a release that bumps one of them cannot leave the others describing the
 * previous version.
 *
 * RELIC_VERSION_NUMBER orders releases so that a consumer can require a
 * minimum version in one comparison:
 *
 *	#if RELIC_VERSION_NUMBER < 10200
 *	#error relic 1.2.0 or later is required
 *	#endif
 *
 * The minor and patch fields are allotted two decimal digits each, so a
 * release must keep them below 100.
 */
#define	RELIC_VERSION_MAJOR	0
#define	RELIC_VERSION_MINOR	1
#define	RELIC_VERSION_PATCH	0

#define	RELIC_VERSION_TEXT_(v)	#v
#define	RELIC_VERSION_TEXT(v)	RELIC_VERSION_TEXT_(v)

#define	RELIC_VERSION_STRING \
	RELIC_VERSION_TEXT(RELIC_VERSION_MAJOR) "." \
	RELIC_VERSION_TEXT(RELIC_VERSION_MINOR) "." \
	RELIC_VERSION_TEXT(RELIC_VERSION_PATCH)

#define	RELIC_VERSION_NUMBER \
	((RELIC_VERSION_MAJOR * 10000) + (RELIC_VERSION_MINOR * 100) + \
	 RELIC_VERSION_PATCH)

/*
 * Mark a symbol as part of the library's public interface.  The library is
 * compiled with hidden visibility where the compiler supports it, so a
 * declaration that omits this would resolve within the library and be
 * unreachable from a program linked against it.  Expanding to nothing on a
 * compiler without visibility attributes is correct: there, nothing was
 * hidden in the first place.
 */
#if defined(__GNUC__) && __GNUC__ >= 4
#define	RELIC_EXPORT	__attribute__((__visibility__("default")))
#else
#define	RELIC_EXPORT
#endif

/*
 * The version of the library the caller is linked against, formatted as
 * "major.minor.patch".  It need not equal RELIC_VERSION_STRING: that macro
 * records the version the caller was compiled against, and a shared library
 * may since have been upgraded underneath it.  The returned string is static
 * and outlives the caller.
 */
RELIC_EXPORT const char *relic_version(void);

/*
 * A relic context: a loaded program, the values it mentions, and the
 * relations derived from it.  Contexts share nothing, so a program may hold
 * as many as it likes, but one context is not itself thread-safe.
 */
typedef struct relic relic_t;

typedef struct relic_config relic_config_t;

/*
 * Limits a context enforces, and switches that select internal behaviour.
 * A zero limit means unlimited, so a zeroed structure asks for the defaults.
 *
 * The fields correspond one to one with the tool's repeatable -l key=value
 * options, which exist only to parse into this structure: a limit is defined
 * in one place and nowhere else.
 *
 * A configuration is accepted and stored but not yet acted on: nothing in
 * the library allocates relation or index storage for a limit to apply to,
 * and no behaviour switch is defined for `flags`.  Setting a field therefore
 * has no effect today.
 */
struct relic_config {
	size_t		max_memory;	/* relation and index storage, bytes */
	size_t		max_tuples;
	size_t		max_magic_preds;
	size_t		max_magic_rules;
	unsigned int	flags;
};

/*
 * Create a context.  Returns NULL when memory is exhausted; there is no
 * other way for it to fail, so no diagnostic callback can be installed yet
 * and none is needed.
 *
 * relic_create() is relic_create_config() with every limit at its default.
 * relic_destroy() releases everything the context owns and accepts NULL.
 */
RELIC_EXPORT relic_t *relic_create(void);
RELIC_EXPORT relic_t *relic_create_config(const relic_config_t *config);
RELIC_EXPORT void relic_destroy(relic_t *ctx);

/*
 * The three value types.  They are distinct domains: the symbol foo and the
 * string "foo" are never equal, and a comparison between values of different
 * types is never true.
 */
enum relic_type {
	RELIC_SYMBOL,
	RELIC_STRING,
	RELIC_INTEGER
};

typedef struct relic_value relic_value_t;

/*
 * A value, as a caller sees it.  This is a complete type because callers
 * construct values to insert and read values out of query results.
 *
 * For RELIC_SYMBOL and RELIC_STRING, `v.id` identifies interned bytes that
 * relic_string() returns.  For RELIC_INTEGER, `v.integer` is the value.
 */
struct relic_value {
	enum relic_type	type;
	union {
		uint32_t	id;
		int64_t		integer;
	} v;
};

/*
 * The bytes of an interned symbol or string, or NULL for an identifier this
 * context never assigned.
 *
 * The result is NUL-terminated for the convenience of a caller that wants a
 * C string, but a string literal may contain NUL bytes, so a caller that must
 * not lose them passes lenp and uses the length.  The bytes belong to the
 * context and stay valid until it is destroyed; the caller neither frees nor
 * modifies them.
 */
RELIC_EXPORT const char *relic_string(relic_t *ctx, uint32_t id,
    size_t *lenp);

typedef struct relic_location relic_location_t;

/*
 * Where in the source a diagnostic points.  Line and column count from one;
 * `file` is the source name the text was loaded under and may be NULL when
 * the diagnostic belongs to no particular file.
 */
struct relic_location {
	const char	*file;
	uint32_t	 line;
	uint32_t	 column;
};

enum relic_diag_level {
	RELIC_NOTE,
	RELIC_WARNING,
	RELIC_ERROR
};

/*
 * Receives every diagnostic the context produces: the context pointer given
 * when it was installed, the level, where the diagnostic points or NULL, and
 * the message.  The message is borrowed and does not outlive the call.
 */
typedef void (*relic_diag_cb)(
    void *,
    enum relic_diag_level,
    const relic_location_t *,
    const char *);

/*
 * Install a diagnostic callback, replacing any previous one.  Passing NULL
 * discards diagnostics, which is the default: a library must not write to
 * its caller's stderr uninvited, and an error remains recoverable through
 * relic_error() whether or not a callback saw it.
 */
RELIC_EXPORT void relic_diag_set(relic_t *ctx, relic_diag_cb callback,
    void *callback_ctx);

/*
 * A diagnostic callback that writes to stderr, for a program that wants one
 * and has no opinion about the format.  This is what the relic tool installs.
 *
 * A located diagnostic is written as "file:line:column: level: message" and
 * an unlocated one as "prefix: level: message", where the prefix is the
 * callback context read as a NUL-terminated program name and is omitted when
 * that is NULL.
 *
 * Bytes outside printable ASCII are escaped, in the message and in the file
 * name alike.  Both can carry text taken from a source file or a command
 * line, and a diagnostic must not be able to send a terminal escape sequence
 * to whoever is reading it.
 */
RELIC_EXPORT void relic_diag_stderr(void *callback_ctx,
    enum relic_diag_level level, const relic_location_t *where,
    const char *message);

/*
 * Load a program.
 *
 * relic_load_path() opens the file and delegates; relic_load_file() reads
 * the stream whole and delegates; relic_load_string() takes a
 * NUL-terminated program text.  `source_name` is what diagnostics from that
 * text point at and is copied, so it need not outlive the call; NULL names
 * the input "<input>".
 *
 * Each returns 0, or -1 with every mistake already reported through the
 * diagnostic callback.  Several loads append to one logical program, which
 * is what lets facts and rules be split across files.
 *
 * A load is all or nothing: a failed one adds none of its statements, so a
 * program never holds half of a file.  A program with an error in it is not
 * a smaller program, it is a different one.
 */
RELIC_EXPORT int relic_load_file(relic_t *ctx, FILE *stream,
    const char *source_name);
RELIC_EXPORT int relic_load_path(relic_t *ctx, const char *path);
RELIC_EXPORT int relic_load_string(relic_t *ctx, const char *source_name,
    const char *text);

/*
 * The text of the most recent RELIC_ERROR diagnostic, or NULL if the context
 * has reported none.  The string belongs to the context and is replaced by
 * the next error, so a caller that needs to keep it copies it.
 *
 * The text is raw.  A diagnostic may quote an identifier, a string literal
 * or a file name taken from whatever relic was asked to read, so it can
 * contain control characters and terminal escape sequences.  A caller
 * sending it to a terminal should escape it, as relic_diag_stderr() does;
 * escaping it here would corrupt it for a caller that renders it some other
 * way.
 */
RELIC_EXPORT const char *relic_error(relic_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RELIC_H */
