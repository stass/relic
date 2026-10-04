# _RELIC_ATOMICS_PROGRAM
# ----------------------
# A program using every C11 atomic operation the library does, on a word
# and on a 64-bit integer.
m4_define([_RELIC_ATOMICS_PROGRAM], [AC_LANG_PROGRAM([[
#ifdef __STDC_NO_ATOMICS__
#error "no atomics"
#endif
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
]], [[
	atomic_size_t word;
	_Atomic(uint64_t) wide;
	size_t expected = 0;

	atomic_init(&word, 0);
	atomic_init(&wide, 0);
	(void)atomic_compare_exchange_weak(&word, &expected, 1);
	atomic_store_explicit(&wide, 1, memory_order_release);
	return ((int)(atomic_fetch_add_explicit(&word, 1,
	    memory_order_acq_rel) + atomic_load_explicit(&wide,
	    memory_order_acquire)));
]])])

# RELIC_ARG_THREADS
# -----------------
# Declare --disable-threads and, unless it is given, find POSIX threads, a
# monotonic clock and C11 atomics, defining RELIC_THREADS when all three are
# there.
#
# A context's worker pool starts threads of its own only in a build with
# all three; the clock times how long a waiting worker spins before it
# parks.  Without them the pool is the calling thread alone and -j has no
# effect; nothing in the language or the store differs.  So a platform that
# lacks any builds without threads and says so, unless threads were
# asked for by name with --enable-threads, in which case their absence is an
# error rather than a quiet downgrade.
#
# All are system interfaces, not dependencies: POSIX threads and
# clock_gettime(2) are in the C library or beside it, as older glibc keeps
# the clock in librt, and the atomics are the compiler's.  The atomics
# are linked as well as compiled, since some targets implement an atomic
# of 64 bits in libatomic, which the compiler does not link by default; the
# link is tried without it and then with it.  RELIC_THREAD_LIBS is what a
# static link of the library needs besides it, for librelic.pc.
AC_DEFUN_ONCE([RELIC_ARG_THREADS], [
AC_REQUIRE([AC_PROG_CC])

AC_ARG_ENABLE([threads],
    [AS_HELP_STRING([--disable-threads],
	[evaluate on the calling thread alone (default: use threads)])],
    [relic_threads_asked=$enable_threads],
    [enable_threads=yes
     relic_threads_asked=default])

_RELIC_REQUIRE_YES_NO([enable-threads], [$enable_threads])

RELIC_THREAD_LIBS=
relic_threads=no

AS_VAR_IF([enable_threads], [yes], [
    relic_saved_LIBS=$LIBS

    AC_CACHE_CHECK([for the library C11 atomics need],
	[relic_cv_c11_atomics_libs],
	[relic_cv_c11_atomics_libs=no
	 for relic_atomics_lib in "none required" -latomic; do
	     AS_CASE([$relic_atomics_lib],
		["none required"], [LIBS=$relic_saved_LIBS],
		[LIBS="$relic_saved_LIBS $relic_atomics_lib"])
	     AC_LINK_IFELSE([_RELIC_ATOMICS_PROGRAM],
		[relic_cv_c11_atomics_libs=$relic_atomics_lib
		 break])
	 done
	 LIBS=$relic_saved_LIBS])

    AS_CASE([$relic_cv_c11_atomics_libs],
	[no|"none required"], [],
	[LIBS="$LIBS $relic_cv_c11_atomics_libs"
	 RELIC_THREAD_LIBS=$relic_cv_c11_atomics_libs])

    AC_CHECK_HEADER([pthread.h],
	[AC_SEARCH_LIBS([pthread_create], [pthread],
	    [relic_pthreads=yes], [relic_pthreads=no])],
	[relic_pthreads=no])

    AC_SEARCH_LIBS([clock_gettime], [rt],
	[relic_clock=yes], [relic_clock=no])

    AS_IF([test "x$relic_cv_c11_atomics_libs" != xno &&
	   test "x$relic_pthreads" = xyes && test "x$relic_clock" = xyes],
	[relic_threads=yes
	 AS_CASE([$ac_cv_search_pthread_create],
	    ["none required"], [],
	    [RELIC_THREAD_LIBS="$ac_cv_search_pthread_create $RELIC_THREAD_LIBS"])
	 AS_CASE([$ac_cv_search_clock_gettime],
	    ["none required"], [],
	    [RELIC_THREAD_LIBS="$ac_cv_search_clock_gettime $RELIC_THREAD_LIBS"])],
	[LIBS=$relic_saved_LIBS
	 RELIC_THREAD_LIBS=])
])

AS_IF([test "x$relic_threads" = xyes],
    [AC_DEFINE([RELIC_THREADS], [1],
	[Define to 1 to give a context's worker pool threads of its own.])],
    [test "x$relic_threads_asked" = xyes],
    [AC_MSG_ERROR([--enable-threads was requested but $CC lacks POSIX threads, clock_gettime or C11 atomics])],
    [test "x$enable_threads" = xyes],
    [AC_MSG_WARN([no POSIX threads, clock_gettime or C11 atomics; building without threads])])

AC_MSG_CHECKING([whether evaluation may use threads])
AC_MSG_RESULT([$relic_threads])

AC_SUBST([RELIC_THREAD_LIBS])
])
