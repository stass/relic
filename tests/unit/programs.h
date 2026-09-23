/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_TEST_PROGRAMS_H
#define RELIC_TEST_PROGRAMS_H

/*
 * Datalog programs that more than one unit test builds.
 */

/*
 * A program whose query p(Y1, ..., Yn)? reaches every one of p's 2^n
 * adornments, n being `arity`: each rule calls p with one more column bound
 * than its head was called with.  f holds the single fact f(c, ..., c), so
 * the answer is one row of n c's.
 *
 * Returns the text, which the caller frees, or NULL when memory ran out.
 */
char	*widening_program(unsigned int arity);

#endif /* RELIC_TEST_PROGRAMS_H */
