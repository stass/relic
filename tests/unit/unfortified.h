/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * Turn fortification off for a translation unit, whatever the command line
 * asked for.
 *
 * A build forces this header in with -include, which the compiler reads
 * after every -D and -U on the command line, so a fortification level a
 * packager's hardening flags add after the build's own options still loses
 * to it.  The only build that uses it is the one that tests the
 * compatibility replacements; the reason is given where that build is
 * defined.
 */

#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0
