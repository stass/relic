ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/lib \
	      -I$(top_srcdir)/compat -I$(top_builddir) \
	      $(RELIC_ASSERT_CPPFLAGS)

# Automake passes AM_CFLAGS to the link command as well as to the compile
# command, which is what the sanitizer and coverage switches need.
AM_CFLAGS = $(RELIC_WARN_CFLAGS) $(RELIC_WERROR_CFLAGS) \
	    $(RELIC_VISIBILITY_CFLAGS) $(RELIC_PREFIX_MAP_CFLAGS) \
	    $(RELIC_SAN_CFLAGS)

# AC_REPLACE_FUNCS objects live in compat/.  A non-recursive build with
# subdir-objects needs the trailing slash here for them to be found.
#
# Everything links the libtool form, $(LTLIBOBJS), and nothing the plain
# $(LIBOBJS); libtool links whichever of the PIC and non-PIC objects was
# built, so a program can name the .lo form as a library does.  A replacement
# named in both forms is compiled by two rules, which write the same
# dependency file and, when static objects are built, the same compat/foo.o,
# and under make -j the two compilations race over them.
LIBOBJDIR = compat/

##
## The library is built once, into a convenience library, and linked from two
## places.  librelic.la applies hidden visibility and the export list, so the
## installed library offers only the documented interface; the unit tests link
## the same objects directly, because a test of the arena or the value table
## calls symbols that are deliberately unreachable through librelic.
##
## Compiling lib/ a second time for the tests would work and would be worse:
## the tests would exercise objects built with different flags from the ones
## that ship.
##

noinst_LTLIBRARIES = librelic_internal.la
librelic_internal_la_SOURCES = lib/api.c lib/arena.c lib/depgraph.c \
	lib/diag.c lib/eval.c lib/explain.c lib/index.c lib/lex.c \
	lib/magic.c lib/parse.c lib/plan.c lib/pred.c lib/relation.c \
	lib/rule.c lib/stratify.c lib/symbol.c lib/util.c lib/value.c

lib_LTLIBRARIES = librelic.la
librelic_la_SOURCES =
librelic_la_LIBADD = librelic_internal.la $(LTLIBOBJS)
librelic_la_LDFLAGS = -no-undefined -export-symbols-regex '^relic_' \
	-version-info $(RELIC_ABI_CURRENT):$(RELIC_ABI_REVISION):$(RELIC_ABI_AGE)

include_HEADERS = include/relic.h
noinst_HEADERS = cmd/relic/cmd.h compat/compat.h compat/queue.h lib/arena.h \
	lib/ast.h lib/depgraph.h lib/internal.h lib/plan.h lib/program.h \
	lib/relation.h lib/util.h tests/unit/harness.h tests/unit/programs.h

bin_PROGRAMS = relic
relic_SOURCES = cmd/relic/main.c cmd/relic/output.c

# The tool links the compatibility objects itself.  Hidden visibility and the
# export list together make them unreachable through librelic, so the first
# call to strtonum(3) from the tool would otherwise fail to link on every
# platform whose C library lacks it.
relic_LDADD = librelic.la $(LTLIBOBJS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = librelic.pc

# The m4 macro files are distributed by automake already, as dependencies of
# configure; listing them here as well would only create a list that a sixth
# macro file could be left out of.
EXTRA_DIST = autogen.sh LICENSE README.md compat/README.md \
	tests/lang tests/langtest.sh

##
## Tests
##
## Shell tests receive the paths and the expected version through the
## environment, so that a test is identical in an in-tree build, an
## out-of-tree build and the unpacked tarball that distcheck builds.
##

##
## The compatibility replacements are exercised twice.
##
## test_compat calls each interface by its ordinary name, so it tests whatever
## this platform will actually link: the C library's implementation where one
## exists, and the replacement from compat/ where it does not.
##
## test_compat_replacements compiles every replacement from source under a
## private name and runs the same scenarios against those.  Without it, a
## replacement would be compiled only on the platforms whose C library lacks
## it, so relic-owned code could go unread by the compiler and unvisited by
## the sanitizers on the machine it was written on.  Fortification is turned
## off for that build because a C library may define these interfaces as
## function-like macros, which take precedence over the renaming and leave
## the replacement's own definition colliding with a compiler builtin.
##

RELIC_COMPAT_RENAMES = \
	-Dreallocarray=relic_replacement_reallocarray \
	-Dstrlcat=relic_replacement_strlcat \
	-Dstrlcpy=relic_replacement_strlcpy \
	-Dstrtonum=relic_replacement_strtonum

check_PROGRAMS = tests/unit/test_compat tests/unit/test_compat_replacements \
	tests/unit/test_header

tests_unit_test_compat_SOURCES = tests/unit/test_compat.c \
	tests/unit/harness.c
tests_unit_test_compat_LDADD = $(LTLIBOBJS)

tests_unit_test_compat_replacements_SOURCES = tests/unit/test_compat.c \
	tests/unit/harness.c compat/reallocarray.c compat/strlcat.c \
	compat/strlcpy.c compat/strtonum.c
tests_unit_test_compat_replacements_CPPFLAGS = $(AM_CPPFLAGS) \
	-DRELIC_COMPAT_DECLARE_ALL -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	$(RELIC_COMPAT_RENAMES)

##
## One test binary per module, driving each through the interface its callers
## use.  They link the convenience library rather than librelic.la because
## every module below the public interface is hidden from a program that
## links the installed library, which is the property librelic.la exists to
## have.
##

check_PROGRAMS += tests/unit/test_arena tests/unit/test_diag \
	tests/unit/test_eval tests/unit/test_index tests/unit/test_lex \
	tests/unit/test_magic tests/unit/test_parse tests/unit/test_plan \
	tests/unit/test_pred tests/unit/test_query tests/unit/test_relation \
	tests/unit/test_rule tests/unit/test_stratify tests/unit/test_symbol \
	tests/unit/test_util tests/unit/test_value

tests_unit_test_arena_SOURCES = tests/unit/test_arena.c tests/unit/harness.c
tests_unit_test_arena_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_diag_SOURCES = tests/unit/test_diag.c tests/unit/harness.c
tests_unit_test_diag_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_eval_SOURCES = tests/unit/test_eval.c tests/unit/harness.c
tests_unit_test_eval_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_index_SOURCES = tests/unit/test_index.c tests/unit/harness.c
tests_unit_test_index_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_lex_SOURCES = tests/unit/test_lex.c tests/unit/harness.c
tests_unit_test_lex_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_magic_SOURCES = tests/unit/test_magic.c tests/unit/harness.c \
	tests/unit/programs.c
tests_unit_test_magic_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_parse_SOURCES = tests/unit/test_parse.c tests/unit/harness.c
tests_unit_test_parse_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_plan_SOURCES = tests/unit/test_plan.c tests/unit/harness.c
tests_unit_test_plan_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_pred_SOURCES = tests/unit/test_pred.c tests/unit/harness.c
tests_unit_test_pred_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_query_SOURCES = tests/unit/test_query.c tests/unit/harness.c
tests_unit_test_query_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_relation_SOURCES = tests/unit/test_relation.c \
	tests/unit/harness.c
tests_unit_test_relation_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_rule_SOURCES = tests/unit/test_rule.c tests/unit/harness.c
tests_unit_test_rule_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_stratify_SOURCES = tests/unit/test_stratify.c \
	tests/unit/harness.c
# The long-chain test runs its analysis on a thread with a small stack.
tests_unit_test_stratify_CFLAGS = $(AM_CFLAGS) -pthread
tests_unit_test_stratify_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_symbol_SOURCES = tests/unit/test_symbol.c tests/unit/harness.c
tests_unit_test_symbol_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_util_SOURCES = tests/unit/test_util.c tests/unit/harness.c
tests_unit_test_util_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_value_SOURCES = tests/unit/test_value.c tests/unit/harness.c
tests_unit_test_value_LDADD = librelic_internal.la $(LTLIBOBJS)

##
## The allocation-failure suite is the one test program that cannot link the
## convenience library.  It compiles the library from source with every
## allocator call renamed, and supplies replacements that refuse the nth
## request, so that the failure paths the arena and interning contracts are
## built around are actually taken.  Without it those paths are dead code in
## every other suite, and a failure that left a table inconsistent would go
## unnoticed until a query returned a wrong answer.
##
## The renaming follows the pattern the compat suite already uses.  It is
## confined to this program; nothing that ships is built this way.
##

RELIC_ALLOCATOR_RENAMES = \
	-Dcalloc=relic_test_calloc \
	-Dmalloc=relic_test_malloc \
	-Dreallocarray=relic_test_reallocarray

check_PROGRAMS += tests/unit/test_exhaustion
tests_unit_test_exhaustion_SOURCES = tests/unit/test_exhaustion.c \
	tests/unit/harness.c tests/unit/programs.c \
	$(librelic_internal_la_SOURCES)
tests_unit_test_exhaustion_CPPFLAGS = $(AM_CPPFLAGS) \
	$(RELIC_ALLOCATOR_RENAMES)
tests_unit_test_exhaustion_LDADD = $(LTLIBOBJS)

##
## The public header carries the longest-lived contract in the tree: it must
## stay strict C11, must compile without config.h, and must be includable
## from C++.  Each property is checked by a translation unit that includes
## <relic.h> and nothing else, compiled with only the include directory on
## its search path.  The C++ program is built where a C++ compiler exists and
## skipped where one does not, since relic itself needs none.
##

tests_unit_test_header_SOURCES = tests/unit/test_header.c
tests_unit_test_header_CPPFLAGS = -I$(top_srcdir)/include
tests_unit_test_header_CFLAGS = $(RELIC_STRICT_CFLAGS) $(RELIC_SAN_CFLAGS)
tests_unit_test_header_LDADD = librelic.la

if RELIC_HAVE_CXX
check_PROGRAMS += tests/unit/test_header_cxx
tests_unit_test_header_cxx_SOURCES = tests/unit/test_header_cxx.cc
tests_unit_test_header_cxx_CPPFLAGS = -I$(top_srcdir)/include
tests_unit_test_header_cxx_CXXFLAGS = $(RELIC_SAN_CFLAGS)
tests_unit_test_header_cxx_LDADD = librelic.la
endif

dist_check_SCRIPTS = tests/ascii.sh tests/cli.sh tests/globals.sh \
	tests/langfiles.sh tests/magic.sh tests/perf.sh tests/pkgconfig.sh \
	tests/version.sh

##
## The equivalence suite runs every language test under each alternative
## evaluation strategy -X selects and compares it with the default.  Its
## corpus is RELIC_LANG_TESTS below, so it needs no list of its own.
##

dist_check_SCRIPTS += tests/equiv/strategies.sh

##
## Language tests
##
## A language test is a .dl program and the output it must produce, with no
## code: tests/langtest.sh runs it as automake's DL_LOG_COMPILER, so adding
## one means adding files rather than writing a test.
##
## The .dl files have to be listed for automake to run them, and a list is
## only as good as the last edit to it -- a test left out of it never runs,
## and a test that never runs passes forever.  tests/langfiles.sh compares
## this list against the directory in both directions and fails if they
## differ, so the omission is loud rather than silent.
##
## The expectation files beside each program are distributed by naming the
## directory in EXTRA_DIST rather than by listing them, because that list
## would be four times as long and could go stale in a way only make dist
## notices.  What a directory cannot do is tell a golden file from something
## a debugging session left behind, so langfiles.sh checks that too.
##

RELIC_LANG_TESTS = tests/lang/arity-limit.dl tests/lang/bad-byte.dl \
	tests/lang/bad-escape.dl tests/lang/check-queries.dl \
	tests/lang/closure-double.dl tests/lang/closure-graphs.dl \
	tests/lang/comments-only.dl tests/lang/components.dl \
	tests/lang/constructs.dl tests/lang/empty.dl \
	tests/lang/error-after-recovery.dl tests/lang/error-first-token.dl \
	tests/lang/explain-check.dl tests/lang/explain-graph.dl \
	tests/lang/explain-long.dl tests/lang/explain-magic.dl \
	tests/lang/explain-materialized.dl tests/lang/explain-ops.dl \
	tests/lang/format-csv.dl tests/lang/format-json.dl \
	tests/lang/format-text.dl \
	tests/lang/format-tsv.dl tests/lang/head-not-atom.dl \
	tests/lang/incomplete-operator.dl tests/lang/integer-range.dl \
	tests/lang/integer-trailing.dl tests/lang/magic-bindings.dl \
	tests/lang/magic-budget.dl \
	tests/lang/magic-limit-preds.dl tests/lang/magic-limit-rules.dl \
	tests/lang/magic-mixed.dl tests/lang/magic-mutual.dl \
	tests/lang/magic-negation.dl tests/lang/magic-same-generation.dl \
	tests/lang/materialize.dl tests/lang/missing-terminator.dl \
	tests/lang/negation-forms.dl tests/lang/negation-recursive.dl \
	tests/lang/negation-strata.dl tests/lang/negation-tasks.dl \
	tests/lang/not-as-predicate.dl tests/lang/not-symbol.dl \
	tests/lang/predicate-identity.dl tests/lang/query-bindings.dl \
	tests/lang/query-columns-limit.dl tests/lang/query-columns.dl \
	tests/lang/query-comparisons.dl tests/lang/query-duplicates.dl \
	tests/lang/query-graph.dl tests/lang/query-none.dl \
	tests/lang/query-not-a-query.dl tests/lang/query-option-unsafe.dl \
	tests/lang/query-option.dl tests/lang/query-recursion.dl \
	tests/lang/query-undefined.dl tests/lang/query-unsafe.dl \
	tests/lang/query-zero-arity.dl tests/lang/recovery.dl \
	tests/lang/safe-rules.dl tests/lang/singleton.dl \
	tests/lang/sort-mixed.dl tests/lang/stats-indexed.dl \
	tests/lang/stats-magic.dl tests/lang/stats-naive.dl \
	tests/lang/stats-semi-naive.dl tests/lang/string-escapes.dl \
	tests/lang/success-criteria.dl tests/lang/trailing-comma.dl \
	tests/lang/undefined-predicate.dl tests/lang/unsafe-anonymous.dl \
	tests/lang/unsafe-comparison.dl tests/lang/unsafe-fact.dl \
	tests/lang/unsafe-head.dl tests/lang/unsafe-negation.dl \
	tests/lang/unstratified-cycles.dl tests/lang/unstratified-policy.dl \
	tests/lang/unstratified-query.dl tests/lang/unstratified-unsafe.dl \
	tests/lang/unstratified.dl tests/lang/unterminated-string.dl \
	tests/lang/variable-limit-max.dl tests/lang/variable-limit.dl \
	tests/lang/zero-arity.dl

TEST_EXTENSIONS = .dl .sh
DL_LOG_COMPILER = $(SHELL) $(abs_top_srcdir)/tests/langtest.sh
SH_LOG_COMPILER = $(SHELL)

AM_TESTS_ENVIRONMENT = \
	RELIC='$(abs_top_builddir)/relic$(EXEEXT)'; export RELIC; \
	RELIC_BUILDDIR='$(abs_top_builddir)'; export RELIC_BUILDDIR; \
	RELIC_SRCDIR='$(abs_top_srcdir)'; export RELIC_SRCDIR; \
	RELIC_VERSION='$(PACKAGE_VERSION)'; export RELIC_VERSION; \
	RELIC_LANG_TESTS='$(RELIC_LANG_TESTS)'; export RELIC_LANG_TESTS;

TESTS = $(check_PROGRAMS) $(dist_check_SCRIPTS) $(RELIC_LANG_TESTS)

##
## Sanitizer suites
##
## Each suite configures a disposable build tree beside the current one.  The
## sanitizers need compiler and link flags that the working tree does not
## have, and rebuilding in place would leave instrumented objects behind for
## the next ordinary build to link against.
##
## The options below matter as much as the flags.  AddressSanitizer defaults
## to halt_on_error=0 and detect_leaks=0 on macOS, so a run that found a
## poisoned read or a leak would report it and still exit successfully,
## turning a failing test into a passing one with a message in its log.
##
## Whether leak detection is asked for at all is decided by configure, which
## runs an instrumented program to find out: a platform without it treats the
## request as an error and aborts every test before it starts.
##

RELIC_ASAN_OPTIONS = halt_on_error=1:detect_leaks=$(RELIC_ASAN_DETECT_LEAKS):detect_stack_use_after_return=1:check_initialization_order=1:strict_string_checks=1:detect_invalid_pointer_pairs=2:abort_on_error=1
RELIC_LSAN_OPTIONS = exitcode=1
RELIC_MSAN_OPTIONS = halt_on_error=1:abort_on_error=1
RELIC_UBSAN_OPTIONS = halt_on_error=1:print_stacktrace=1

# Configure, build and test in the tree named by $$tree using the configure
# options in $$flags.  The calling target sets those, $$name for messages,
# $$supported to say whether this toolchain has the sanitizer at all, and
# optionally $$note for a limitation worth mentioning.
#
# The variables are checked rather than assumed.  A target that forgot to set
# $$flags would otherwise configure an uninstrumented tree, run the suite in
# it and report a pass, which is the one outcome worse than a failure.
#
# A sanitizer the toolchain cannot provide makes the target say so and
# succeed.  Failing instead would leave no way to run the suite on a platform
# that has only some of them, and would make the sanitizer runs that the
# working agreement asks for impossible rather than merely unavailable.
#
# The tree is configured afresh with nothing carried over but the compiler,
# so that a sanitizer run means the same thing whatever options the
# surrounding build was configured with.
RELIC_SANITIZER_CHECK = \
	{ test -n "$$tree" && test -n "$$flags" && test -n "$$name" && \
	  test -n "$$supported"; } || \
	    { echo "sanitizer target is incompletely defined" >&2; exit 1; }; \
	if test "x$$supported" != xyes; then \
		echo "$$name is not available with $(CC); skipping."; \
		exit 0; \
	fi; \
	if test -n "$$note"; then echo "note: $$note"; fi; \
	rm -rf $(abs_top_builddir)/$$tree && \
	$(MKDIR_P) $(abs_top_builddir)/$$tree && \
	cd $(abs_top_builddir)/$$tree && \
	$(abs_top_srcdir)/configure $$flags CC="$(CC)" && \
	$(MAKE) $(AM_MAKEFLAGS) && \
	$(MAKE) $(AM_MAKEFLAGS) check

check-asan:
	@tree=build-asan; flags=--enable-asan; name=AddressSanitizer; \
	supported='$(RELIC_ASAN_SUPPORTED)'; note=; \
	test x'$(RELIC_ASAN_DETECT_LEAKS)' = x1 || \
	    note='this platform cannot detect leaks; memory errors only'; \
	ASAN_OPTIONS='$(RELIC_ASAN_OPTIONS)'; export ASAN_OPTIONS; \
	$(RELIC_SANITIZER_CHECK)

check-lsan:
	@tree=build-lsan; flags=--enable-lsan; name=LeakSanitizer; \
	supported='$(RELIC_LSAN_SUPPORTED)'; note=; \
	LSAN_OPTIONS='$(RELIC_LSAN_OPTIONS)'; export LSAN_OPTIONS; \
	$(RELIC_SANITIZER_CHECK)

check-msan:
	@tree=build-msan; flags=--enable-msan; name=MemorySanitizer; \
	supported='$(RELIC_MSAN_SUPPORTED)'; note=; \
	MSAN_OPTIONS='$(RELIC_MSAN_OPTIONS)'; export MSAN_OPTIONS; \
	$(RELIC_SANITIZER_CHECK)

check-ubsan:
	@tree=build-ubsan; flags=--enable-ubsan; \
	name=UndefinedBehaviorSanitizer; \
	supported='$(RELIC_UBSAN_SUPPORTED)'; note=; \
	UBSAN_OPTIONS='$(RELIC_UBSAN_OPTIONS)'; export UBSAN_OPTIONS; \
	$(RELIC_SANITIZER_CHECK)

# Two build products automake does not generate a rule for.
#
# The sanitizer trees belong to this build directory rather than to the
# source directory, so they are removed relative to it.  They are matched by
# pattern rather than listed, so that a suite added later is cleaned without
# a second place to remember.
#
# The compatibility objects come from AC_REPLACE_FUNCS rather than from a
# _SOURCES variable, so automake never learns that libtool builds them in a
# directory of their own and emits no rule to remove it.  Left behind, that
# directory fails the distclean check distcheck ends with.
# The language tests are distributed by naming their directory, and in an
# in-tree build automake's record of each test run lands in that same
# directory.  Those records are build output and have no place in a release
# tarball, so they are removed from the distribution after it is assembled.
dist-hook:
	-rm -f $(distdir)/tests/lang/*.log $(distdir)/tests/lang/*.trs

clean-local:
	-cd $(top_builddir) && rm -rf build-*san
	-rm -rf $(LIBOBJDIR).libs $(LIBOBJDIR)_libs

.PHONY: check-asan check-lsan check-msan check-ubsan dist-hook
