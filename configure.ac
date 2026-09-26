AC_PREREQ([2.69])
AC_INIT([relic], [0.1.0], [stas@deglitch.com], [relic])
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
dnl libtool nothing to hook.
RELIC_CHECK_CXX
LT_INIT

dnl The shared library interface version, maintained independently of the
dnl release version: a release that changes no interface keeps the same
dnl version-info, and an interface change is a deliberate edit here.  By
dnl libtool's rules an added entry point is current+1:0:age+1, a removed or
dnl changed one -- a field added to a complete type among them -- is
dnl current+1:0:0, and a change inside the library alone is
dnl current:revision+1:age.
RELIC_ABI_CURRENT=3
RELIC_ABI_REVISION=0
RELIC_ABI_AGE=2
AC_SUBST([RELIC_ABI_CURRENT])
AC_SUBST([RELIC_ABI_REVISION])
AC_SUBST([RELIC_ABI_AGE])

AC_CHECK_HEADERS([err.h sysexits.h])

dnl getentropy(2) is declared by <unistd.h> on the BSDs, glibc and musl, and
dnl by <sys/random.h> on macOS.  compat.h includes whichever exists, so no
dnl engine source has to know which one it is.
AC_CHECK_HEADERS([sys/random.h])

dnl A replacement is compiled from compat/ only on a platform whose C library
dnl lacks the interface, so the library and the tool call these
dnl unconditionally and neither tests for a platform.
AC_REPLACE_FUNCS([getentropy reallocarray strlcat strlcpy strtonum])

dnl A stream whose read function is the caller's own code, which the interface
dnl tests hand to relic_load_file().  Neither is used by the library or the
dnl tool, and the test that needs one is skipped where both are missing.
AC_CHECK_FUNCS([fopencookie funopen])

RELIC_CHECK_PREFETCH

RELIC_ARG_DEBUG
RELIC_ARG_SANITIZERS
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
    C++ header test   $relic_cv_cxx_works
    install prefix    $prefix
])
