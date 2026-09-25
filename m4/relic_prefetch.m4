# RELIC_CHECK_PREFETCH
# --------------------
# Decide whether the compiler offers a prefetch builtin, and define
# HAVE_BUILTIN_PREFETCH when it does.
#
# Batched insertion prefetches the slots and tuples a batch will probe so
# that their misses overlap.  A prefetch is a hint: it reads nothing the
# program depends on and cannot fault, so a compiler without one builds a
# relic that inserts the same tuples in the same order and only loses the
# overlap.  C11 has no prefetch, and the builtin is a property of the
# compiler rather than a dependency, so it is probed rather than required.
AC_DEFUN([RELIC_CHECK_PREFETCH], [
AC_CACHE_CHECK([for __builtin_prefetch], [relic_cv_builtin_prefetch],
    [AC_LINK_IFELSE(
	[AC_LANG_PROGRAM([[static int relic_word;]],
	    [[__builtin_prefetch(&relic_word);]])],
	[relic_cv_builtin_prefetch=yes],
	[relic_cv_builtin_prefetch=no])])

AS_IF([test "x$relic_cv_builtin_prefetch" = xyes],
    [AC_DEFINE([HAVE_BUILTIN_PREFETCH], [1],
	[Define to 1 if the compiler offers __builtin_prefetch.])])
])
