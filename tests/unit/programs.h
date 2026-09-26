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

/*
 * The widening program with every rule's body filled out to `body_length`
 * literals by f literals over variables nothing else reads, which bind
 * nothing p is called with: the same adornments, each copying longer rules.
 */
char	*padded_widening_program(unsigned int arity, unsigned int body_length);

/*
 * The widening program with a rule deriving p from p itself and `length`
 * calls on f whose arguments are all constant, followed by `length` / 10
 * unary predicates derived from f.  Every adornment of p copies the long
 * rule and, with the constant calls placed first, the magic rule for its
 * call on p, and drops both, since each derives its head from itself.  The
 * unary predicates raise the budget on predicates, so that it does not stop
 * the rewrite before the budget on literals does.
 */
char	*self_calling_program(unsigned int arity, unsigned int length);

#endif /* RELIC_TEST_PROGRAMS_H */
