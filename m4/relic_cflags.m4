# RELIC_CHECK_CFLAGS(FLAGS)
# -------------------------
# Append every flag in FLAGS that the compiler accepts to RELIC_WARN_CFLAGS.
#
# Each flag is probed on its own, so one unsupported flag does not cost the
# rest.  The probe compiles a translation unit with -Werror added rather than
# merely running the driver, because clang accepts an unknown -W option and
# reports it as a warning instead of failing: without -Werror every flag would
# appear to be supported.
AC_DEFUN([RELIC_CHECK_CFLAGS], [
AC_REQUIRE([AC_PROG_CC])
for relic_flag in $1; do
	AS_VAR_PUSHDEF([relic_cache], [relic_cv_cflags_$relic_flag])
	AC_CACHE_CHECK([whether $CC accepts $relic_flag], [relic_cache],
	    [relic_saved_CFLAGS=$CFLAGS
	     CFLAGS="$CFLAGS $relic_flag -Werror"
	     AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
		[AS_VAR_SET([relic_cache], [yes])],
		[AS_VAR_SET([relic_cache], [no])])
	     CFLAGS=$relic_saved_CFLAGS])
	AS_VAR_IF([relic_cache], [yes],
	    [RELIC_WARN_CFLAGS="$RELIC_WARN_CFLAGS $relic_flag"])
	AS_VAR_POPDEF([relic_cache])
done
AC_SUBST([RELIC_WARN_CFLAGS])
])

# RELIC_ARG_WERROR
# ----------------
# --enable-werror puts -Werror in RELIC_WERROR_CFLAGS, which the makefile
# appends after the warning flags.
#
# It is deliberately kept out of RELIC_WARN_CFLAGS.  RELIC_CHECK_CFLAGS probes
# each warning flag by compiling with -Werror added, so a -Werror already
# present in the flags under test would make every probe fail and leave the
# build with no warning flags at all.  A separate variable makes that
# impossible regardless of the order the two macros are called in.
AC_DEFUN([RELIC_ARG_WERROR], [
AC_ARG_ENABLE([werror],
    [AS_HELP_STRING([--enable-werror],
	[treat compiler warnings as errors (default: no)])],
    [], [enable_werror=no])

_RELIC_REQUIRE_YES_NO([enable-werror], [$enable_werror])

RELIC_WERROR_CFLAGS=
AS_VAR_IF([enable_werror], [yes], [RELIC_WERROR_CFLAGS=-Werror])
AC_SUBST([RELIC_WERROR_CFLAGS])
])

# RELIC_CHECK_VISIBILITY
# ----------------------
# Set RELIC_VISIBILITY_CFLAGS to -fvisibility=hidden where the compiler
# supports it, so that the shared library exports only the symbols the public
# header marks with RELIC_EXPORT.  The export regular expression passed to
# libtool covers the same ground on its own; hiding by default as well means
# an internal symbol is unreachable rather than merely unlisted, and costs
# nothing on a platform whose compiler does not support the switch.
AC_DEFUN([RELIC_CHECK_VISIBILITY], [
AC_REQUIRE([AC_PROG_CC])
AC_CACHE_CHECK([whether $CC supports -fvisibility=hidden],
    [relic_cv_visibility_hidden],
    [relic_saved_CFLAGS=$CFLAGS
     CFLAGS="$CFLAGS -fvisibility=hidden -Werror"
     AC_COMPILE_IFELSE(
	[AC_LANG_PROGRAM(
	    [[__attribute__((__visibility__("default"))) int relic_probe(void);
	      int relic_probe(void) { return (0); }]],
	    [[]])],
	[relic_cv_visibility_hidden=yes],
	[relic_cv_visibility_hidden=no])
     CFLAGS=$relic_saved_CFLAGS])

AS_VAR_IF([relic_cv_visibility_hidden], [yes],
    [RELIC_VISIBILITY_CFLAGS=-fvisibility=hidden])
AC_SUBST([RELIC_VISIBILITY_CFLAGS])
])

# RELIC_CHECK_STRICT_CFLAGS
# -------------------------
# Set RELIC_STRICT_CFLAGS to the switches that compile a translation unit as
# strictly conforming C11, or leave it empty on a compiler that offers no such
# mode.
#
# The public header is compiled with these.  A consumer may build with
# conformance demanded and warnings fatal, and a header that reaches for a
# compiler extension fails in that consumer's build rather than in this one,
# where nothing would have reported it.
AC_DEFUN([RELIC_CHECK_STRICT_CFLAGS], [
AC_REQUIRE([AC_PROG_CC])
AC_CACHE_CHECK([for switches that demand strict C11 conformance],
    [relic_cv_strict_cflags],
    [relic_saved_CFLAGS=$CFLAGS
     CFLAGS="$CFLAGS -std=c11 -pedantic-errors"
     AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
	[relic_cv_strict_cflags="-std=c11 -pedantic-errors"],
	[relic_cv_strict_cflags=none])
     CFLAGS=$relic_saved_CFLAGS])

AS_VAR_IF([relic_cv_strict_cflags], [none],
    [RELIC_STRICT_CFLAGS=],
    [RELIC_STRICT_CFLAGS=$relic_cv_strict_cflags])
AC_SUBST([RELIC_STRICT_CFLAGS])
])

# _RELIC_COMPILER_FAMILY(LANGUAGE, RESULT-VAR)
# --------------------------------------------
# Set RESULT-VAR to clang, gnu or other, after the compiler of LANGUAGE.
AC_DEFUN([_RELIC_COMPILER_FAMILY], [
AC_LANG_PUSH([$1])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef __clang__
#error not clang
#endif
]], [[]])],
    [$2=clang],
    [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#ifndef __GNUC__
#error not GNU
#endif
]], [[]])],
	[$2=gnu],
	[$2=other])])
AC_LANG_POP([$1])
])

# _RELIC_CXX_LINKS_LIBRARY_OBJECT(RESULT-VAR)
# -----------------------------------------
# A header consumer must link what the C compiler produces, including its
# bitcode format or instrumentation runtime. Linking an empty C++ program
# tests only the C++ compiler against itself and cannot establish that.
AC_DEFUN([_RELIC_CXX_LINKS_LIBRARY_OBJECT], [
relic_saved_CFLAGS=$CFLAGS
relic_saved_CXXFLAGS=$CXXFLAGS
relic_saved_LIBS=$LIBS
CFLAGS="$RELIC_SAN_CFLAGS $CFLAGS"
CXXFLAGS="$RELIC_SAN_CFLAGS $CXXFLAGS"
$1=no
AC_LANG_PUSH([C])
AC_COMPILE_IFELSE([AC_LANG_SOURCE([[
int relic_cxx_probe(void);
int relic_cxx_probe(void) { return (7); }
]])], [
    mv conftest.$ac_objext conftest-relic.$ac_objext
    LIBS="conftest-relic.$ac_objext $LIBS"
    AC_LANG_PUSH([C++])
    AC_LINK_IFELSE([AC_LANG_PROGRAM(
	[[extern "C" int relic_cxx_probe(void);]],
	[[return (relic_cxx_probe() != 7);]])], [$1=yes])
    AC_LANG_POP([C++])
    rm -f conftest-relic.$ac_objext
])
AC_LANG_POP([C])
CFLAGS=$relic_saved_CFLAGS
CXXFLAGS=$relic_saved_CXXFLAGS
LIBS=$relic_saved_LIBS
])

# RELIC_CHECK_CXX
# ---------------
# Decide whether the C++ compiler can build the C++ header test, and set the
# RELIC_HAVE_CXX conditional and relic_cxx_works accordingly.
#
# relic contains no C++ and must build without it; the compiler is wanted
# only to compile the test that proves the public header is includable from
# C++.  A compiler that cannot build that test drops it instead of failing
# the build, which is why the result is a conditional rather than an error.
#
# The test is compiled with the instrumentation of the rest of the build and
# linked against the library the C compiler built.  So the probe links with
# the same flags, and in an instrumented build it requires the C++ compiler
# to be of the C compiler's family: a sanitizer's or gcov's runtime belongs
# to its compiler, and objects instrumented by GCC linked by clang++, or the
# other way round, fail to link or bring two runtimes into one process.
#
# The answer depends on the instrumentation options as well as on CXX, and
# a cached answer is keyed on CXX alone, so it is worked out afresh every
# time rather than cached.
AC_DEFUN([RELIC_CHECK_CXX], [
AC_REQUIRE([AC_PROG_CXX])
AC_REQUIRE([RELIC_ARG_SANITIZERS])
relic_cxx_works=yes
AS_IF([test -n "$RELIC_SAN_CFLAGS"],
    [_RELIC_COMPILER_FAMILY([C], [relic_cc_family])
     _RELIC_COMPILER_FAMILY([C++], [relic_cxx_family])
     AS_IF([test "x$relic_cc_family" != "x$relic_cxx_family"],
	[AC_MSG_WARN([omitting the C++ header test: $CXX is not of the family of $CC])
	 relic_cxx_works=no])])
AS_VAR_IF([relic_cxx_works], [yes],
    [_RELIC_CXX_LINKS_LIBRARY_OBJECT([relic_cxx_works])])
AC_MSG_CHECKING([whether $CXX can build the C++ header test])
AC_MSG_RESULT([$relic_cxx_works])
AM_CONDITIONAL([RELIC_HAVE_CXX], [test "x$relic_cxx_works" = xyes])
])

# RELIC_CHECK_PREFIX_MAP
# ----------------------
# Set RELIC_PREFIX_MAP_CFLAGS to the switches that rewrite the source and
# build directories recorded in compiled output to relative paths, or leave it
# empty where the compiler offers no such switch.
#
# Debugging information records the path of every source file and the
# directory the compiler ran in.  Those survive into an installed static
# library, where they disclose the directory layout of the machine that built
# it and make two builds of identical sources differ.  Rewriting them costs
# nothing and removes both.
AC_DEFUN([RELIC_CHECK_PREFIX_MAP], [
AC_REQUIRE([AC_PROG_CC])
AC_CACHE_CHECK([for a switch that rewrites recorded source paths],
    [relic_cv_prefix_map_flag],
    [relic_cv_prefix_map_flag=none
     for relic_flag in -ffile-prefix-map -fdebug-prefix-map; do
	relic_saved_CFLAGS=$CFLAGS
	CFLAGS="$CFLAGS $relic_flag=/relic-probe=. -Werror"
	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[]])],
	    [relic_cv_prefix_map_flag=$relic_flag])
	CFLAGS=$relic_saved_CFLAGS
	AS_IF([test "x$relic_cv_prefix_map_flag" != xnone], [break])
     done])

RELIC_PREFIX_MAP_CFLAGS=
AS_VAR_IF([relic_cv_prefix_map_flag], [none], [],
    [relic_abs_srcdir=`cd "$srcdir" && pwd`
     relic_abs_builddir=`pwd`
     RELIC_PREFIX_MAP_CFLAGS="$relic_cv_prefix_map_flag=$relic_abs_srcdir=."
     AS_IF([test "x$relic_abs_builddir" != "x$relic_abs_srcdir"],
	[RELIC_PREFIX_MAP_CFLAGS="$RELIC_PREFIX_MAP_CFLAGS $relic_cv_prefix_map_flag=$relic_abs_builddir=."])])
AC_SUBST([RELIC_PREFIX_MAP_CFLAGS])
])
