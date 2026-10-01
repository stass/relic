/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <relic.h>

#include "fuzz.h"

/*
 * The load target: an input is program text, loaded and checked.
 *
 * This reaches the lexer, the parser, the program model and its safety
 * analysis, and the dependency analysis and stratification -- everything a
 * program passes through before anything is evaluated, and the part of the
 * library most exposed to input nobody chose.  Nothing is evaluated, so an
 * input costs time in proportion to its length and the fuzzer can explore
 * quickly.
 *
 * The library must return from every input, reporting a malformed one
 * through its diagnostics; a crash, a sanitizer report or a hang is the
 * finding.
 */
int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	relic_t *ctx;

	ctx = relic_create();
	if (ctx == NULL)
		abort();
	relic_diag_set(ctx, fuzz_discard_diagnostic, NULL);

	if (fuzz_load(ctx, data, size) == 0)
		relic_check(ctx);

	relic_destroy(ctx);
	return (0);
}
