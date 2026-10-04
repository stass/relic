/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_TEST_FUZZ_H
#define RELIC_TEST_FUZZ_H

#include <stddef.h>
#include <stdint.h>

#include <relic.h>

/*
 * The fuzz targets.
 *
 * Each target defines LLVMFuzzerTestOneInput(), the entry point libFuzzer
 * calls with one input at a time.  A target is linked two ways: with
 * libFuzzer, which supplies main() and searches for inputs, when configure
 * is given --enable-fuzzers; and always with tests/fuzz/replay.c, whose
 * main() passes each file of a corpus to the entry point once, so that every
 * input the corpus holds is run by make check on every compiler.
 *
 * An input that reveals a bug makes the target abort, which both drivers
 * report: libFuzzer by saving the input, the replay driver by dying on it.
 */
int	LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/*
 * Load the input into a context as program text, every byte of it --
 * embedded NULs included, which a NUL-terminated string would cut short.
 * Returns what relic_load_file() returns.
 */
int	fuzz_load(relic_t *ctx, const uint8_t *data, size_t size);

/*
 * Abort when the diagnostic `ctx` is reporting at `level` is an internal
 * error: an invariant the library checks was false, which only a bug
 * reaches.  Every target's diagnostic callback calls this for each message,
 * so an internal error is a finding wherever it is reported, in a load or a
 * check as much as in a query.
 */
void	fuzz_refuse_internal(relic_t *ctx, enum relic_diag_level level);

/*
 * A diagnostic callback whose callback context is the context reporting:
 * it discards every message, but aborts on an internal error.
 */
void	fuzz_watch_diagnostic(void *callback_ctx,
	    enum relic_diag_level level, const relic_location_t *where,
	    const char *message);

#endif /* RELIC_TEST_FUZZ_H */
