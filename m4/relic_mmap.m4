# RELIC_CHECK_MAP_ANON
# --------------------
# Decide how <sys/mman.h> spells an anonymous mapping, and define
# RELIC_MAP_ANON to that flag.
#
# A growable array large enough to deserve pages of its own is allocated with
# mmap(2) rather than malloc(3), so that releasing it returns its pages to the
# system at once.  The BSDs and macOS spell the flag MAP_ANON; the Linux C
# libraries spell it MAP_ANONYMOUS and declare MAP_ANON beside it, both only
# under the system extensions configure enables.  The BSD spelling is tried
# first and the first spelling that links is used.  A platform declaring
# neither cannot build relic, so configure stops there rather than producing
# a library that cannot allocate.
AC_DEFUN([RELIC_CHECK_MAP_ANON], [
AC_CACHE_CHECK([for the anonymous mapping flag], [relic_cv_map_anon],
    [relic_cv_map_anon=no
    for relic_map_flag in MAP_ANON MAP_ANONYMOUS; do
	AS_IF([test "x$relic_cv_map_anon" = xno], [
	AC_LINK_IFELSE(
	    [AC_LANG_PROGRAM([[#include <sys/mman.h>
#include <stddef.h>]],
		[[void *region;

region = mmap(NULL, 1, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | $relic_map_flag, -1, 0);
return (region == MAP_FAILED);]])],
	    [relic_cv_map_anon=$relic_map_flag])])
    done])

AS_IF([test "x$relic_cv_map_anon" = xno],
    [AC_MSG_ERROR([<sys/mman.h> declares neither MAP_ANON nor MAP_ANONYMOUS])])

AC_DEFINE_UNQUOTED([RELIC_MAP_ANON], [$relic_cv_map_anon],
    [Define to the flag that asks mmap(2) for an anonymous mapping.])
])
