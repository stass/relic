# RELIC_ARG_THINLTO
# ------------------
# Enable ThinLTO only when the complete configured toolchain can carry a
# bitcode definition through an archive to a separate caller. Merely accepting
# a compiler flag does not establish archive indexing or linker support.
# CFLAGS and CXXFLAGS reach every compile and link recipe, including targets
# with their own AM flags and the installed-library check.
AC_DEFUN_ONCE([RELIC_ARG_THINLTO], [
AC_ARG_ENABLE([thinlto],
    [AS_HELP_STRING([--enable-thinlto],
	[enable LLVM ThinLTO (default: no)])],
    [], [enable_thinlto=no])

_RELIC_REQUIRE_YES_NO([enable-thinlto], [$enable_thinlto])
RELIC_LTO_CFLAGS=
AS_VAR_IF([enable_thinlto], [yes], [
    RELIC_LTO_CFLAGS=-flto=thin
    CFLAGS="$CFLAGS $RELIC_LTO_CFLAGS"
    CXXFLAGS="$CXXFLAGS $RELIC_LTO_CFLAGS"
    AC_MSG_CHECKING([whether the toolchain builds and links ThinLTO archives])
    cat > conftest-lto-library.c <<'_RELIC_EOF'
int relic_lto_probe(void);
int relic_lto_probe(void) { return (7); }
_RELIC_EOF
    cat > conftest-lto-main.c <<'_RELIC_EOF'
int relic_lto_probe(void);
int main(void) { return (relic_lto_probe() != 7); }
_RELIC_EOF
    relic_lto_error=
    AS_IF([$CC $CPPFLAGS $RELIC_SAN_CFLAGS $CFLAGS -c \
	conftest-lto-library.c -o conftest-lto.o >&AS_MESSAGE_LOG_FD 2>&1], [
	relic_lto_magic=`od -An -tx1 -N4 conftest-lto.o | tr -d ' \n'`
	AS_CASE([$relic_lto_magic],
	    [4243c0de|dec0170b], [],
	    [relic_lto_error="the compiler did not emit LLVM bitcode"])
    ], [relic_lto_error="the C compiler cannot emit ThinLTO bitcode"])
    AS_IF([test -z "$relic_lto_error"], [
	AS_IF([$AR cr conftest-lto.a conftest-lto.o >&AS_MESSAGE_LOG_FD 2>&1],
	    [], [relic_lto_error="AR cannot archive LLVM bitcode"])
    ])
    AS_IF([test -z "$relic_lto_error"], [
	AS_IF([$RANLIB conftest-lto.a >&AS_MESSAGE_LOG_FD 2>&1],
	    [], [relic_lto_error="RANLIB cannot index LLVM bitcode"])
    ])
    AS_IF([test -z "$relic_lto_error"], [
	AS_IF([$NM conftest-lto.o > conftest-lto.nm 2>&AS_MESSAGE_LOG_FD], [
	    AS_IF([grep relic_lto_probe conftest-lto.nm >/dev/null],
		[], [relic_lto_error="NM cannot identify bitcode definitions"])
	], [relic_lto_error="NM cannot read LLVM bitcode"])
    ])
    AS_IF([test -z "$relic_lto_error"], [
	AS_IF([$CC $CPPFLAGS $RELIC_SAN_CFLAGS $CFLAGS $LDFLAGS \
	    conftest-lto-main.c conftest-lto.a $LIBS -o conftest-lto \
	    >&AS_MESSAGE_LOG_FD 2>&1],
	    [], [relic_lto_error="the linker cannot consume ThinLTO archives"])
    ])
    rm -f conftest-lto-library.c conftest-lto-main.c conftest-lto.o \
	conftest-lto.a conftest-lto.nm conftest-lto conftest-lto.exe
    AS_IF([test -n "$relic_lto_error"], [
	AC_MSG_RESULT([no])
	AC_MSG_ERROR([--enable-thinlto: $relic_lto_error; select a compatible CC, AR, RANLIB, NM and linker])
    ])
    AC_MSG_RESULT([yes])
])
AC_SUBST([RELIC_LTO_CFLAGS])
AM_CONDITIONAL([RELIC_THINLTO], [test "x$enable_thinlto" = xyes])
])
