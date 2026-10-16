/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_ATOMIC_H
#define RELIC_ATOMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The words the workers of a round share, and the operations on them.
 *
 * They are few: the claim, abandon and completion words of a job, the
 * generation a job is published by, the memory account's total for a
 * round's buffers, the frontiers of an index's keys, and the flag a caller
 * interrupts its context's evaluation by.  Each operation's name gives the
 * ordering it makes, so the reader of a call sees what it synchronizes
 * without looking here: a relaxed operation is atomic and orders nothing,
 * an acquire load sees everything written before the release store or
 * read-modify-write it reads from, and an operation whose name gives no
 * ordering is sequentially consistent.  Only the account's and the flag's
 * are, since they are rare.  An initialization is not atomic, and is made
 * before any other thread can see the word.
 *
 * A build with threads (RELIC_THREADS) uses C11 atomics.  A build without
 * them has one worker and nothing shared, and the same operations are
 * plain loads and stores, so no code but this header tells the two apart.
 * The interrupt flag, at the end, is shared with the caller rather than
 * with the pool, and is the same in either build.
 */

#ifdef RELIC_THREADS

#include <stdatomic.h>

typedef atomic_bool		relic_atomic_bool_t;
typedef atomic_size_t		relic_atomic_size_t;
typedef _Atomic(uint32_t)	relic_atomic_u32_t;
typedef _Atomic(uint64_t)	relic_atomic_u64_t;

/* Only instantiate the dense collector's CAS on always-lock-free types. */
#if ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_LLONG_LOCK_FREE == 2 && \
    ATOMIC_INT_LOCK_FREE == 2
#define	RELIC_DENSE_ATOMICS	1
static inline bool
relic_atomic_u64_swap_if_relaxed(relic_atomic_u64_t *word,
    uint64_t *expected, uint64_t desired)
{
	return (atomic_compare_exchange_strong_explicit(word, expected,
	    desired, memory_order_relaxed, memory_order_relaxed));
}
#else
#define	RELIC_DENSE_ATOMICS	0
#endif

static inline uint64_t
relic_atomic_u64_load_relaxed(const relic_atomic_u64_t *word)
{
	return (atomic_load_explicit(word, memory_order_relaxed));
}

static inline void
relic_atomic_bool_init(relic_atomic_bool_t *word, bool value)
{
	atomic_init(word, value);
}

static inline bool
relic_atomic_bool_load_relaxed(const relic_atomic_bool_t *word)
{
	return (atomic_load_explicit(word, memory_order_relaxed));
}

static inline void
relic_atomic_bool_store_relaxed(relic_atomic_bool_t *word, bool value)
{
	atomic_store_explicit(word, value, memory_order_relaxed);
}

static inline void
relic_atomic_size_init(relic_atomic_size_t *word, size_t value)
{
	atomic_init(word, value);
}

static inline size_t
relic_atomic_size_load(const relic_atomic_size_t *word)
{
	return (atomic_load(word));
}

static inline size_t
relic_atomic_size_load_acquire(const relic_atomic_size_t *word)
{
	return (atomic_load_explicit(word, memory_order_acquire));
}

static inline void
relic_atomic_size_store(relic_atomic_size_t *word, size_t value)
{
	atomic_store(word, value);
}

/* Returns the value before the addition. */
static inline size_t
relic_atomic_size_add_relaxed(relic_atomic_size_t *word, size_t addend)
{
	return (atomic_fetch_add_explicit(word, addend, memory_order_relaxed));
}

/* Returns the value before the addition. */
static inline size_t
relic_atomic_size_add_acq_rel(relic_atomic_size_t *word, size_t addend)
{
	return (atomic_fetch_add_explicit(word, addend, memory_order_acq_rel));
}

static inline void
relic_atomic_size_subtract(relic_atomic_size_t *word, size_t subtrahend)
{
	atomic_fetch_sub(word, subtrahend);
}

/*
 * Replace the word with `desired` if it holds *expected, and otherwise set
 * *expected to what it holds.  Returns whether it was replaced.
 */
static inline bool
relic_atomic_size_swap_if(relic_atomic_size_t *word, size_t *expected,
    size_t desired)
{
	return (atomic_compare_exchange_weak(word, expected, desired));
}

static inline uint32_t
relic_atomic_u32_load_relaxed(const relic_atomic_u32_t *word)
{
	return (atomic_load_explicit(word, memory_order_relaxed));
}

static inline void
relic_atomic_u32_store_relaxed(relic_atomic_u32_t *word, uint32_t value)
{
	atomic_store_explicit(word, value, memory_order_relaxed);
}

static inline void
relic_atomic_u64_init(relic_atomic_u64_t *word, uint64_t value)
{
	atomic_init(word, value);
}

static inline uint64_t
relic_atomic_u64_load_acquire(const relic_atomic_u64_t *word)
{
	return (atomic_load_explicit(word, memory_order_acquire));
}

static inline void
relic_atomic_u64_store_release(relic_atomic_u64_t *word, uint64_t value)
{
	atomic_store_explicit(word, value, memory_order_release);
}

#else /* !RELIC_THREADS */

typedef bool		relic_atomic_bool_t;
typedef size_t		relic_atomic_size_t;
typedef uint32_t	relic_atomic_u32_t;
typedef uint64_t	relic_atomic_u64_t;

#define	RELIC_DENSE_ATOMICS	1
static inline bool
relic_atomic_u64_swap_if_relaxed(relic_atomic_u64_t *word,
    uint64_t *expected, uint64_t desired)
{
	if (*word != *expected) {
		*expected = *word;
		return (false);
	}
	*word = desired;
	return (true);
}

static inline uint64_t
relic_atomic_u64_load_relaxed(const relic_atomic_u64_t *word)
{
	return (*word);
}

static inline void
relic_atomic_bool_init(relic_atomic_bool_t *word, bool value)
{
	*word = value;
}

static inline bool
relic_atomic_bool_load_relaxed(const relic_atomic_bool_t *word)
{
	return (*word);
}

static inline void
relic_atomic_bool_store_relaxed(relic_atomic_bool_t *word, bool value)
{
	*word = value;
}

static inline void
relic_atomic_size_init(relic_atomic_size_t *word, size_t value)
{
	*word = value;
}

static inline size_t
relic_atomic_size_load(const relic_atomic_size_t *word)
{
	return (*word);
}

static inline size_t
relic_atomic_size_load_acquire(const relic_atomic_size_t *word)
{
	return (*word);
}

static inline void
relic_atomic_size_store(relic_atomic_size_t *word, size_t value)
{
	*word = value;
}

static inline size_t
relic_atomic_size_add_relaxed(relic_atomic_size_t *word, size_t addend)
{
	size_t before;

	before = *word;
	*word += addend;

	return (before);
}

static inline size_t
relic_atomic_size_add_acq_rel(relic_atomic_size_t *word, size_t addend)
{
	return (relic_atomic_size_add_relaxed(word, addend));
}

static inline void
relic_atomic_size_subtract(relic_atomic_size_t *word, size_t subtrahend)
{
	*word -= subtrahend;
}

static inline bool
relic_atomic_size_swap_if(relic_atomic_size_t *word, size_t *expected,
    size_t desired)
{
	if (*word != *expected) {
		*expected = *word;
		return (false);
	}

	*word = desired;

	return (true);
}

static inline uint32_t
relic_atomic_u32_load_relaxed(const relic_atomic_u32_t *word)
{
	return (*word);
}

static inline void
relic_atomic_u32_store_relaxed(relic_atomic_u32_t *word, uint32_t value)
{
	*word = value;
}

static inline void
relic_atomic_u64_init(relic_atomic_u64_t *word, uint64_t value)
{
	*word = value;
}

static inline uint64_t
relic_atomic_u64_load_acquire(const relic_atomic_u64_t *word)
{
	return (*word);
}

static inline void
relic_atomic_u64_store_release(relic_atomic_u64_t *word, uint64_t value)
{
	*word = value;
}

#endif /* RELIC_THREADS */

/*
 * A flag raised from outside the operation that reads it: by a signal
 * handler, by another thread, or by caller code the operation calls.
 * Raising it is all such a caller does, so the flag must be a word a signal
 * handler may store to.  Where the compiler has C11 atomics it is a
 * lock-free atomic_bool, in a build without threads as well, since the
 * thread raising it is the caller's and not the pool's; where it has none
 * it is a volatile sig_atomic_t, the one type C promises a handler may
 * store to, and the flag is then for a handler and a callback alone: a
 * store from another thread would race with the evaluation's loads.
 */
#if !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#endif

#if !defined(__STDC_NO_ATOMICS__) && ATOMIC_BOOL_LOCK_FREE == 2

typedef atomic_bool		relic_signal_flag_t;

static inline void
relic_signal_flag_init(relic_signal_flag_t *flag)
{
	atomic_init(flag, false);
}

static inline void
relic_signal_flag_raise(relic_signal_flag_t *flag)
{
	atomic_store(flag, true);
}

static inline void
relic_signal_flag_lower(relic_signal_flag_t *flag)
{
	atomic_store(flag, false);
}

static inline bool
relic_signal_flag_raised(const relic_signal_flag_t *flag)
{
	return (atomic_load(flag));
}

#else /* no lock-free atomic_bool */

#include <signal.h>

typedef volatile sig_atomic_t	relic_signal_flag_t;

static inline void
relic_signal_flag_init(relic_signal_flag_t *flag)
{
	*flag = 0;
}

static inline void
relic_signal_flag_raise(relic_signal_flag_t *flag)
{
	*flag = 1;
}

static inline void
relic_signal_flag_lower(relic_signal_flag_t *flag)
{
	*flag = 0;
}

static inline bool
relic_signal_flag_raised(const relic_signal_flag_t *flag)
{
	return (*flag != 0);
}

#endif /* lock-free atomic_bool */

#endif /* RELIC_ATOMIC_H */
