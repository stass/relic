/*
 * Copyright (c) 2026 Stanislav Sedov
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

/* A diagnostic callback that discards every message. */
void	fuzz_discard_diagnostic(void *callback_ctx,
	    enum relic_diag_level level, const relic_location_t *where,
	    const char *message);

#endif /* RELIC_TEST_FUZZ_H */
