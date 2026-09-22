# RELIC_ARG_WITH_LIBEDIT
# ----------------------
# Decide whether the interactive mode is built against libedit.
#
# libedit is the project's only optional dependency and is never required:
# without it the interactive mode reads standard input normally and offers no
# history.  --with-libedit=yes therefore means "fail if it is missing", =no
# means "do not look", and the default =auto means "use it where it is
# installed", which is the behaviour a packager expects and the one that keeps
# a build from acquiring a dependency the builder did not ask for.
#
# Both the header and the library are probed.  A platform that installs
# histedit.h without libedit, or the reverse, is common enough that trusting
# either alone would produce a build that fails at link time.
AC_DEFUN([RELIC_ARG_WITH_LIBEDIT], [
AC_ARG_WITH([libedit],
    [AS_HELP_STRING([--with-libedit@<:@=auto|yes|no@:>@],
	[line editing and history in interactive mode (default: auto)])],
    [], [with_libedit=auto])

LIBEDIT_LIBS=
relic_libedit=no

AS_CASE([$with_libedit],
    [auto|yes|no], [],
    [AC_MSG_ERROR([--with-libedit accepts auto, yes or no])])

AS_IF([test "x$with_libedit" != xno],
    [AC_CHECK_HEADER([histedit.h],
	[AC_CHECK_LIB([edit], [el_init],
	    [relic_libedit=yes
	     LIBEDIT_LIBS=-ledit])])])

AS_IF([test "x$with_libedit" = xyes && test "x$relic_libedit" = xno],
    [AC_MSG_ERROR([--with-libedit=yes was given but libedit was not found])])

AS_VAR_IF([relic_libedit], [yes],
    [AC_DEFINE([HAVE_LIBEDIT], [1],
	[Define to 1 if libedit and histedit.h are available.])])

AC_SUBST([LIBEDIT_LIBS])
])
