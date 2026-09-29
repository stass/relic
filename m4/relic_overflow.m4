# RELIC_CHECK_OVERFLOW_BUILTINS
# -----------------------------
# Decide whether the compiler offers the checked arithmetic builtins
# __builtin_add_overflow, __builtin_sub_overflow and __builtin_mul_overflow
# over int64_t, and define HAVE_BUILTIN_OVERFLOW when it does.
#
# Integer expressions are evaluated with every operation checked before its
# result is used, so that an overflow is an error the program is told of and
# never undefined behaviour.  The builtins compile to the processor's
# overflow flag; without them lib/arith.c compares the operands against the
# bounds instead, which gives the same answers more slowly.  C11 has no
# checked arithmetic, and the builtins are a property of the compiler rather
# than a dependency, so they are probed rather than required.
AC_DEFUN([RELIC_CHECK_OVERFLOW_BUILTINS], [
AC_CACHE_CHECK([for __builtin_add_overflow], [relic_cv_builtin_overflow],
    [AC_LINK_IFELSE(
	[AC_LANG_PROGRAM([[#include <stdint.h>
static volatile int64_t relic_a = 1, relic_b = 2;]],
	    [[int64_t r;
	    return (__builtin_add_overflow(relic_a, relic_b, &r) ||
		__builtin_sub_overflow(relic_a, relic_b, &r) ||
		__builtin_mul_overflow(relic_a, relic_b, &r));]])],
	[relic_cv_builtin_overflow=yes],
	[relic_cv_builtin_overflow=no])])

AS_IF([test "x$relic_cv_builtin_overflow" = xyes],
    [AC_DEFINE([HAVE_BUILTIN_OVERFLOW], [1],
	[Define to 1 if the compiler offers __builtin_add_overflow and its
	 siblings.])])
])
