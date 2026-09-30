# compat/

Two kinds of file live here, and they are governed by different rules.

**Replacements.** `getentropy.c`, `memmem.c`, `reallocarray.c`,
`strlcat.c`, `strlcpy.c` and `strtonum.c` are relic-owned implementations of BSD
interfaces that some platforms lack. `configure` compiles one only where the
C library does not provide the interface, through `AC_REPLACE_FUNCS`, so
engine sources call each interface unconditionally and no source outside
this directory contains a platform test. The exception is a
MemorySanitizer build, which compiles `strlcat.c` and `strlcpy.c` everywhere,
because the sanitizer cannot see what the C library's copies write.
`compat.h` declares whichever interfaces the platform's headers do not.

**Bundled upstream headers.** `queue.h` is a copy of an upstream file, not
relic's work. It is bundled rather than probed because the macro set of a
system `<sys/queue.h>` differs between glibc, musl, macOS and the BSDs in
ways that are tedious to probe correctly and easy to probe incompletely; one
known copy makes list behaviour identical everywhere. Its upstream license
notice stands as it arrived and relic's own license does not replace it.

## Bundled copies

| File | Upstream | Revision | License |
| --- | --- | --- | --- |
| `queue.h` | OpenBSD `sys/sys/queue.h` | `1.47` | BSD-3-Clause, Regents of the University of California |

Retrieved from
`https://raw.githubusercontent.com/openbsd/src/master/sys/sys/queue.h`.

The upstream file, before the patches below, has SHA-256:

```text
a2abb97bd570e63c6f547174d2557d5d8c640bd669c61efdfd20d707e7a8dd13
```

### Patches to `queue.h`

1. **`#include <sys/_null.h>` becomes `#include <stddef.h>`.** The former is
   an OpenBSD-private header that exists nowhere else; `NULL` comes from
   `<stddef.h>` on every platform relic builds on. The changed line carries a
   trailing comment naming what it replaced.

2. **The file is wrapped in a `RELIC_COMPAT_QUEUE_H` guard that rejects a
   system copy read earlier in the same translation unit.** Upstream guards
   itself with `_SYS_QUEUE_H_`, which a system `<sys/queue.h>` also defines,
   so a header that pulled in the system copy first would make this file
   expand to nothing and the program would silently use the system macros
   instead. That is precisely the outcome bundling exists to prevent, and it
   would leave no trace, so it is a compile error rather than a convention to
   remember.

Nothing else is changed. Keep it that way: a patch that is easy to enumerate
is a copy that is easy to audit and to update.

## Updating a bundled copy

Fetch the new upstream file, record its revision and checksum in the table
above, reapply the patches listed here, and rerun the test suite. If a patch
no longer applies, say why in this file rather than leaving the discrepancy
for the next reader to discover.
