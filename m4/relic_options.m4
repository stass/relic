# RELIC_ARG_DEBUG
# ---------------
# --enable-debug builds without optimization and with full debugging
# information.
#
# The switches are appended to CFLAGS rather than to AM_CFLAGS because
# automake places CFLAGS last on the compile command line: a -O0 in AM_CFLAGS
# would be overridden by the -O2 that autoconf puts in the default CFLAGS, and
# the option would appear to work while changing nothing.
AC_DEFUN_ONCE([RELIC_ARG_DEBUG], [
AC_REQUIRE([AC_PROG_CC])
AC_ARG_ENABLE([debug],
    [AS_HELP_STRING([--enable-debug],
	[build without optimization and with debugging information (default: no)])],
    [], [enable_debug=no])

_RELIC_REQUIRE_YES_NO([enable-debug], [$enable_debug])
AS_VAR_IF([enable_debug], [yes], [CFLAGS="$CFLAGS -O0 -g3"])
])

# RELIC_SET_ASSERTIONS
# --------------------
# Decide whether assert(3) is compiled in, and report the decision.
#
# Assertions state invariants that the engine's correctness rests on, so they
# are kept in every build whose purpose is to find defects: a debug build, and
# any instrumented build, where an assertion failure is exactly the signal the
# run was started to collect.  An ordinary optimized build defines NDEBUG.
#
# The two options it reads are required rather than assumed to have been
# processed already, so that this macro cannot be placed ahead of them and
# silently decide from unset variables.
AC_DEFUN([RELIC_SET_ASSERTIONS], [
AC_REQUIRE([RELIC_ARG_DEBUG])
AC_REQUIRE([RELIC_ARG_SANITIZERS])
RELIC_ASSERT_CPPFLAGS=
relic_assertions=yes

AS_IF([test "x$enable_debug" != xyes && test -z "$RELIC_SAN_CFLAGS"],
    [RELIC_ASSERT_CPPFLAGS=-DNDEBUG
     relic_assertions=no])

AC_MSG_CHECKING([whether assertions are compiled in])
AC_MSG_RESULT([$relic_assertions])
AC_SUBST([RELIC_ASSERT_CPPFLAGS])
])
