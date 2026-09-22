# RELIC_PROG_CC_C11
# -----------------
# Select the dialect switch that gives a C11 compiler while keeping the BSD
# interfaces of the C library visible.
#
# -std=gnu11 is preferred over -std=c11 because a strict-conformance switch
# hides functions such as strlcpy(3) and reallocarray(3) behind feature test
# macros on several platforms, which would make configure decide that the
# platform lacks an interface it in fact has.  A compiler that offers no
# dialect switch at all is accepted as it stands.
#
# The dialect is pinned rather than merely required.  AC_PROG_CC selects the
# newest standard the compiler knows, which is a moving target across compiler
# releases, and building against a later standard on one platform would let
# code that no longer compiles as C11 reach the platforms that only have C11.
# Any -std already in CC is therefore discarded, whether AC_PROG_CC put it
# there or the builder did, so that exactly one dialect switch reaches the
# compiler and every platform sees the same language.  The compiler that
# results is printed in the configure summary, so the substitution is visible
# rather than silent.
#
# A -std switch in CFLAGS is a different matter and is left alone: automake
# places CFLAGS after the compiler's own flags, so the builder's choice wins.
# The probe below compiles with CFLAGS in place, so a dialect older than C11
# selected that way fails configure rather than surfacing later.
AC_DEFUN([RELIC_PROG_CC_C11], [
AC_REQUIRE([AC_PROG_CC])

dnl The compiler driver with every dialect switch removed.  The probe below
dnl and the selection that follows it both build on this one value, and it is
dnl computed outside the cached check because a cached run skips that check
dnl entirely and still has to choose a compiler.
relic_cc_without_std=`echo "$CC" | sed 's/ -std=[[^ ]]*//g'`

AC_CACHE_CHECK([for a C11 dialect switch], [relic_cv_prog_cc_c11],
    [relic_cv_prog_cc_c11=unsupported
     relic_saved_CC=$CC
     for relic_switch in -std=gnu11 -std=c11 none; do
	AS_IF([test "x$relic_switch" = xnone],
	    [CC=$relic_cc_without_std],
	    [CC="$relic_cc_without_std $relic_switch"])
	AC_COMPILE_IFELSE(
	    [AC_LANG_PROGRAM(
		[[_Static_assert(__STDC_VERSION__ >= 201112L, "C11 required");]],
		[[]])],
	    [relic_cv_prog_cc_c11=$relic_switch])
	AS_IF([test "x$relic_cv_prog_cc_c11" != xunsupported], [break])
     done
     CC=$relic_saved_CC])

AS_CASE([$relic_cv_prog_cc_c11],
    [unsupported],
	[AC_MSG_ERROR([a C11 compiler is required; if CFLAGS names a -std
	switch, note that it is applied after the one chosen here and
	decides the dialect])],
    [none], [CC=$relic_cc_without_std],
    [CC="$relic_cc_without_std $relic_cv_prog_cc_c11"])
])
