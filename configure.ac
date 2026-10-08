AC_PREREQ([2.69])
AC_INIT([relic], [1.0.0], [stas@deglitch.com], [relic])
AC_CONFIG_SRCDIR([lib/api.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_LIBOBJ_DIR([compat])
AM_INIT_AUTOMAKE([1.13 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

AC_USE_SYSTEM_EXTENSIONS
RELIC_PROG_CC_C11
AM_PROG_AR

dnl The C++ compiler is selected before LT_INIT.  libtool hooks its own C++
dnl language setup onto AC_PROG_CXX when it has not already run, and the hook
dnl then asks for the macro it is attached to, which autoconf reports as a
dnl macro required after it was expanded.  Choosing the compiler first leaves
dnl libtool nothing to hook.  Whether it can build the one test that needs it
dnl is decided once the instrumentation is known, by RELIC_CHECK_CXX.
AC_PROG_CXX
LT_PREREQ([2.4.2])
LT_INIT

dnl The shared library interface version, maintained independently of the
dnl release version and edited deliberately: a release that changes nothing
dnl in the library keeps the same version-info.  By libtool's rules a change
dnl inside the library alone is current:revision+1:age, an added entry point
dnl is current+1:0:age+1, and a removed or changed one -- a field added to a
dnl complete type among them -- is current+1:0:0.
RELIC_ABI_CURRENT=1
RELIC_ABI_REVISION=0
RELIC_ABI_AGE=0
AC_SUBST([RELIC_ABI_CURRENT])
AC_SUBST([RELIC_ABI_REVISION])
AC_SUBST([RELIC_ABI_AGE])

dnl getentropy(2) is declared by <unistd.h> on the BSDs, glibc and musl, and
dnl by <sys/random.h> on macOS.  compat.h includes whichever exists, so no
dnl engine source has to know which one it is.
AC_CHECK_HEADERS([sys/random.h])

dnl The instrumentation is settled first, because what follows depends on
dnl it: which replacements are compiled, and whether the C++ compiler can
dnl build the test that needs it.  Neither macro adds to CFLAGS, so the
dnl probes below are compiled as they would be without them.
RELIC_ARG_SANITIZERS
RELIC_CHECK_CXX

dnl A replacement is compiled from compat/ only on a platform whose C library
dnl lacks the interface, so the library and the tool call these
dnl unconditionally and neither tests for a platform.
dnl
dnl MemorySanitizer knows memory to be written only when instrumented code
dnl or a C library function its runtime intercepts wrote it.  It intercepts
dnl no strlcat(3) or strlcpy(3), so a string the C library copied would
dnl read as uninitialized and fail the suite.  Under --enable-msan the
dnl replacements are compiled whatever the C library provides, and are
dnl instrumented with the rest of the tree.
AC_REPLACE_FUNCS([getentropy memmem reallocarray strtonum])
AS_IF([test "x$enable_msan" = xyes],
    [AC_LIBOBJ([strlcat])
     AC_LIBOBJ([strlcpy])],
    [AC_REPLACE_FUNCS([strlcat strlcpy])])

dnl compat.h declares each of those interfaces exactly where no system header
dnl does.  Whether one is declared is a separate question from whether it
dnl links: a C library may provide a function its headers do not declare, or
dnl declare one that is then replaced, and a declaration repeated beside the
dnl system's is refused by -Wredundant-decls.  The probe includes the headers
dnl compat.h includes, so that it sees what compat.h's users see.
AC_CHECK_DECLS([getentropy, memmem, reallocarray, strlcat, strlcpy, strtonum],
    [], [], [[
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif
]])

dnl A stream whose read function is the caller's own code, which the interface
dnl tests hand to relic_load_file().  Neither is used by the library or the
dnl tool, and the test that needs one is skipped where both are missing.
AC_CHECK_FUNCS([fopencookie funopen])

RELIC_CHECK_PREFETCH
RELIC_CHECK_OVERFLOW_BUILTINS

RELIC_ARG_DEBUG
RELIC_ARG_THREADS
RELIC_SET_ASSERTIONS
RELIC_ARG_WITH_LIBEDIT
RELIC_CHECK_CFLAGS([-Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings dnl
 -Wcast-qual -Wmissing-prototypes -Wstrict-prototypes -Wold-style-definition dnl
 -Wredundant-decls -Wnested-externs -Wvla -Wundef -Wbad-function-cast dnl
 -Wdeclaration-after-statement -Wformat=2])
RELIC_ARG_WERROR
RELIC_CHECK_STRICT_CFLAGS
RELIC_CHECK_PREFIX_MAP
RELIC_CHECK_VISIBILITY

AC_CONFIG_FILES([Makefile librelic.pc])
AC_OUTPUT

AC_MSG_NOTICE([

  $PACKAGE_NAME $PACKAGE_VERSION

    compiler          $CC
    warning flags    $RELIC_WARN_CFLAGS
    instrumentation   $RELIC_SAN_CFLAGS
    assertions        $relic_assertions
    libedit           $relic_libedit
    C++ header test   $relic_cxx_works
    install prefix    $prefix
])
