# _RELIC_TRY_SANITIZER(CACHE-SUFFIX, DESCRIPTION, FLAGS, RESULT-VAR)
# ------------------------------------------------------------------
# Set RESULT-VAR to yes when the compiler can build and link a program with
# FLAGS, caching the answer under CACHE-SUFFIX and describing the attempt as
# DESCRIPTION.  The probe links rather than only compiling, because every sanitizer
# needs a runtime library: a compiler accepts -fsanitize=memory long before
# the matching runtime is installed, and the failure would otherwise surface
# as a link error in the middle of a build.
AC_DEFUN([_RELIC_TRY_SANITIZER], [
AC_CACHE_CHECK([whether $CC can build with $2], [relic_cv_sanitizer_$1],
    [relic_saved_CFLAGS=$CFLAGS
     relic_saved_LDFLAGS=$LDFLAGS
     CFLAGS="$CFLAGS $3"
     LDFLAGS="$LDFLAGS $3"
     AC_LINK_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
	[relic_cv_sanitizer_$1=yes],
	[relic_cv_sanitizer_$1=no])
     CFLAGS=$relic_saved_CFLAGS
     LDFLAGS=$relic_saved_LDFLAGS])
$4=$relic_cv_sanitizer_$1
])

# _RELIC_REQUIRE_YES_NO(OPTION, VALUE)
# ------------------------------------
# Reject any value for a boolean option other than yes or no.
#
# AS_VAR_IF tests for exactly "yes", so --enable-asan=1 would otherwise
# configure an uninstrumented build without a word, which is the failure this
# file exists to prevent: a suite that was asked to run instrumented and
# quietly did not reports a success it has not earned.
AC_DEFUN([_RELIC_REQUIRE_YES_NO], [
AS_CASE([$2],
    [yes|no], [],
    [AC_MSG_ERROR([--$1 accepts yes or no, not "$2"])])
])

# _RELIC_ENABLE_SANITIZER(NAME, HELP, FLAGS)
# ------------------------------------------
# Declare --enable-NAME and, when it is given, append FLAGS to
# RELIC_SAN_CFLAGS after confirming the toolchain can use them.  A requested
# sanitizer that the toolchain cannot provide is an error rather than a
# silent downgrade: a test suite that was asked to run instrumented and
# quietly ran uninstrumented reports success it has not earned.
AC_DEFUN([_RELIC_ENABLE_SANITIZER], [
AC_ARG_ENABLE([$1],
    [AS_HELP_STRING([--enable-$1], [$2 (default: no)])],
    [], [enable_$1=no])

_RELIC_REQUIRE_YES_NO([enable-$1], [$enable_$1])

AS_VAR_IF([enable_$1], [yes],
    [_RELIC_TRY_SANITIZER([$1], [$3], [$3], [relic_sanitizer_usable])
     AS_VAR_IF([relic_sanitizer_usable], [yes],
	[RELIC_SAN_CFLAGS="$RELIC_SAN_CFLAGS $3"],
	[AC_MSG_ERROR([--enable-$1 was requested but $CC cannot build with it])])])
])

# RELIC_CHECK_ASAN_LEAKS
# ----------------------
# Set RELIC_ASAN_DETECT_LEAKS to 1 where AddressSanitizer can detect leaks at
# run time, and to 0 where it cannot.
#
# Leak detection is a separate capability from AddressSanitizer itself and is
# missing on some platforms, macOS on Apple silicon among them.  Asking for it
# there is not ignored: the runtime reports that it is unsupported and aborts,
# so every test in the suite fails before it runs.  Asking for it where it
# works is equally necessary, since the runtime leaves it off by default on
# more platforms than it is broken on.
#
# The answer cannot be deduced from the compiler, which accepts the option
# either way, so the probe builds an instrumented program and runs it with
# leak detection demanded.  A cross build cannot run anything and settles for
# the conservative answer, which costs leak coverage rather than correctness.
AC_DEFUN([RELIC_CHECK_ASAN_LEAKS], [
AC_REQUIRE([AC_PROG_CC])
AC_CACHE_CHECK([whether AddressSanitizer can detect leaks],
    [relic_cv_asan_detect_leaks],
    [relic_saved_CFLAGS=$CFLAGS
     relic_saved_LDFLAGS=$LDFLAGS
     relic_saved_ASAN_OPTIONS=${ASAN_OPTIONS-}
     relic_had_ASAN_OPTIONS=${ASAN_OPTIONS+set}
     CFLAGS="$CFLAGS -fsanitize=address"
     LDFLAGS="$LDFLAGS -fsanitize=address"
     ASAN_OPTIONS=detect_leaks=1
     export ASAN_OPTIONS
     AC_RUN_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
	[relic_cv_asan_detect_leaks=yes],
	[relic_cv_asan_detect_leaks=no],
	[relic_cv_asan_detect_leaks=no])
     AS_IF([test "x$relic_had_ASAN_OPTIONS" = xset],
	[ASAN_OPTIONS=$relic_saved_ASAN_OPTIONS],
	[AS_UNSET([ASAN_OPTIONS])])
     CFLAGS=$relic_saved_CFLAGS
     LDFLAGS=$relic_saved_LDFLAGS])

AS_VAR_IF([relic_cv_asan_detect_leaks], [yes],
    [RELIC_ASAN_DETECT_LEAKS=1],
    [RELIC_ASAN_DETECT_LEAKS=0])
AC_SUBST([RELIC_ASAN_DETECT_LEAKS])
])

# RELIC_ARG_SANITIZERS
# --------------------
# Declare the sanitizer and coverage build options and record which of the
# sanitizers this toolchain could run.
#
# The instrumentation flags are substituted into AM_CFLAGS alone.  Automake
# passes AM_CFLAGS to both the compile and the link command, and every
# sanitizer needs its flag in both places, so a separate set of link flags
# would only be a second place to forget one.
#
# Each sanitizer is probed whether or not its option was given, because the
# check-asan, check-lsan, check-msan and check-ubsan targets must be able to
# report that a platform does not support one and succeed, rather than
# failing partway through configuring a tree that could never have been
# built.  OpenBSD, NetBSD and a Linux without the sanitizer runtimes
# installed all reach that case.
AC_DEFUN_ONCE([RELIC_ARG_SANITIZERS], [
AC_REQUIRE([AC_PROG_CC])

RELIC_SAN_CFLAGS=

_RELIC_ENABLE_SANITIZER([asan], [build with AddressSanitizer],
    [-fsanitize=address -fno-omit-frame-pointer -g])
_RELIC_ENABLE_SANITIZER([lsan], [build with LeakSanitizer],
    [-fsanitize=leak -fno-omit-frame-pointer -g])
_RELIC_ENABLE_SANITIZER([msan], [build with MemorySanitizer],
    [-fsanitize=memory -fsanitize-memory-track-origins -fno-omit-frame-pointer -g])
_RELIC_ENABLE_SANITIZER([ubsan], [build with UndefinedBehaviorSanitizer],
    [-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g])

dnl AddressSanitizer and MemorySanitizer instrument the same accesses in
dnl incompatible ways and cannot share a build.
AS_IF([test "x$enable_asan" = xyes && test "x$enable_msan" = xyes],
    [AC_MSG_ERROR([--enable-asan and --enable-msan cannot be combined])])

AC_ARG_ENABLE([coverage],
    [AS_HELP_STRING([--enable-coverage],
	[build with coverage instrumentation (default: no)])],
    [], [enable_coverage=no])

_RELIC_REQUIRE_YES_NO([enable-coverage], [$enable_coverage])

AS_VAR_IF([enable_coverage], [yes],
    [_RELIC_TRY_SANITIZER([coverage], [--coverage], [--coverage],
	[relic_coverage_usable])
     AS_VAR_IF([relic_coverage_usable], [yes],
	[RELIC_SAN_CFLAGS="$RELIC_SAN_CFLAGS --coverage"],
	[AC_MSG_ERROR([--enable-coverage was requested but $CC cannot build with it])])])

dnl The fuzz targets are linked with libFuzzer, whose main() drives them, and
dnl the probe therefore links a program that defines the entry point and no
dnl main().  Everything else is compiled with the coverage instrumentation
dnl libFuzzer steers by, which -fsanitize=fuzzer-no-link adds without
dnl linking libFuzzer's main() into every program.  The option fails where
dnl the compiler lacks libFuzzer, Apple's clang among them, rather than
dnl building a tree with nothing to fuzz.
AC_ARG_ENABLE([fuzzers],
    [AS_HELP_STRING([--enable-fuzzers],
	[build the libFuzzer targets in tests/fuzz, clang only (default: no)])],
    [], [enable_fuzzers=no])

_RELIC_REQUIRE_YES_NO([enable-fuzzers], [$enable_fuzzers])

AS_VAR_IF([enable_fuzzers], [yes],
    [AC_CACHE_CHECK([whether $CC can build libFuzzer targets],
	[relic_cv_fuzzers],
	[relic_saved_CFLAGS=$CFLAGS
	 relic_saved_LDFLAGS=$LDFLAGS
	 CFLAGS="$CFLAGS -fsanitize=fuzzer"
	 LDFLAGS="$LDFLAGS -fsanitize=fuzzer"
	 AC_LINK_IFELSE([AC_LANG_SOURCE([[
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *, size_t);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	return (data != NULL && size > 0 ? 0 : 0);
}
]])],
	    [relic_cv_fuzzers=yes],
	    [relic_cv_fuzzers=no])
	 CFLAGS=$relic_saved_CFLAGS
	 LDFLAGS=$relic_saved_LDFLAGS])
     AS_VAR_IF([relic_cv_fuzzers], [yes],
	[RELIC_SAN_CFLAGS="$RELIC_SAN_CFLAGS -fsanitize=fuzzer-no-link"],
	[AC_MSG_ERROR([--enable-fuzzers was requested but $CC cannot link a libFuzzer target])])])

AM_CONDITIONAL([RELIC_FUZZERS], [test "x$enable_fuzzers" = xyes])

AC_SUBST([RELIC_SAN_CFLAGS])

_RELIC_TRY_SANITIZER([asan_probe], [-fsanitize=address], [-fsanitize=address],
    [RELIC_ASAN_SUPPORTED])
_RELIC_TRY_SANITIZER([lsan_probe], [-fsanitize=leak], [-fsanitize=leak],
    [RELIC_LSAN_SUPPORTED])
_RELIC_TRY_SANITIZER([msan_probe], [-fsanitize=memory], [-fsanitize=memory],
    [RELIC_MSAN_SUPPORTED])
_RELIC_TRY_SANITIZER([ubsan_probe], [-fsanitize=undefined],
    [-fsanitize=undefined], [RELIC_UBSAN_SUPPORTED])
AC_SUBST([RELIC_ASAN_SUPPORTED])
AC_SUBST([RELIC_LSAN_SUPPORTED])
AC_SUBST([RELIC_MSAN_SUPPORTED])
AC_SUBST([RELIC_UBSAN_SUPPORTED])

RELIC_CHECK_ASAN_LEAKS
])
