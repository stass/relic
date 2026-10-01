/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include <relic.h>

#include "../unit/harness.h"
#include "fuzz.h"

/*
 * The input is copied because fmemopen(3) takes a writable buffer, and
 * libFuzzer's is not; a stream opened for reading never writes to it, but
 * the copy keeps that from resting on a cast.  An empty input is loaded as
 * an empty string, since a zero-length fmemopen(3) is an error on some
 * platforms.
 */
int
fuzz_load(relic_t *ctx, const uint8_t *data, size_t size)
{
	char *copy;
	FILE *stream;
	int status;

	if (size == 0)
		return (relic_load_string(ctx, "fuzz.dl", ""));

	copy = malloc(size);
	if (copy == NULL)
		abort();
	bcopy(data, copy, size);

	stream = fmemopen(copy, size, "r");
	if (stream == NULL)
		abort();

	status = relic_load_file(ctx, "fuzz.dl", stream);

	fclose(stream);
	free(copy);
	return (status);
}

void
fuzz_discard_diagnostic(void *callback_ctx RELIC_TEST_UNUSED,
    enum relic_diag_level level RELIC_TEST_UNUSED,
    const relic_location_t *where RELIC_TEST_UNUSED,
    const char *message RELIC_TEST_UNUSED)
{
}
