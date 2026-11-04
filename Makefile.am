ACLOCAL_AMFLAGS = -I m4

# The optional browser build uses an isolated copy of this distribution.
WASM_OUTPUT = $(abs_top_builddir)/disposable/wasm-dist
wasm-build:
	$(SHELL) $(abs_top_srcdir)/wasm/build.sh "$(abs_top_builddir)" "$(WASM_OUTPUT)"

wasm-check: relic
	RELIC_WASM_CHECK=yes $(SHELL) $(abs_top_srcdir)/wasm/build.sh "$(abs_top_builddir)" "$(WASM_OUTPUT)"
	node $(abs_top_srcdir)/tests/wasm/node-tests.mjs "$(WASM_OUTPUT)" "$(abs_top_builddir)/relic"

.PHONY: wasm-build wasm-check

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
librelic_internal_la_SOURCES = lib/account.c lib/api.c lib/arena.c lib/arith.c \
	lib/cost.c lib/dense.c lib/depgraph.c lib/destination.c lib/diag.c lib/eval.c lib/exec.c \
	lib/explain.c lib/expr.c lib/facts.c lib/fold.c lib/grouped.c \
	lib/import.c lib/index.c lib/lex.c \
	lib/literal.c lib/magic.c lib/merge.c lib/parse.c lib/plan.c \
	lib/pool.c lib/pred.c lib/regex.c lib/relation.c lib/result.c \
	lib/round.c lib/rule.c lib/stratify.c lib/strfn.c \
	lib/symbol.c lib/task.c lib/util.c lib/value.c lib/window.c

lib_LTLIBRARIES = librelic.la
librelic_la_SOURCES =
librelic_la_LIBADD = librelic_internal.la $(LTLIBOBJS)
librelic_la_LDFLAGS = -no-undefined -export-symbols-regex '^relic_' \
	-version-info $(RELIC_ABI_CURRENT):$(RELIC_ABI_REVISION):$(RELIC_ABI_AGE)

include_HEADERS = include/relic.h
dist_man1_MANS = man/relic.1
dist_man3_MANS = man/relic.3
noinst_HEADERS = cmd/relic/cmd.h compat/compat.h compat/queue.h \
	lib/account.h lib/arena.h lib/arith.h lib/ast.h lib/atomic.h \
	lib/cdefs.h lib/cost.h lib/depgraph.h lib/destination.h \
	lib/exec.h lib/fold.h \
	lib/frozen.h lib/internal.h lib/plan.h lib/program.h lib/regex.h \
	lib/relation.h lib/round.h lib/strfn.h lib/table.h lib/util.h \
	lib/window.h \
	tests/unit/harness.h \
	tests/unit/programs.h

bin_PROGRAMS = relic
relic_SOURCES = cmd/relic/export.c cmd/relic/files.c cmd/relic/input.c \
	cmd/relic/main.c cmd/relic/operand.c cmd/relic/output.c cmd/relic/repl.c

# The tool links the compatibility objects itself.  Hidden visibility and the
# export list together make them unreachable through librelic, so the first
# call to strtonum(3) from the tool would otherwise fail to link on every
# platform whose C library lacks it.  libedit is the tool's alone, for the
# interactive mode, and is empty when configure did not find it or was told
# not to use it.
relic_LDADD = librelic.la $(LTLIBOBJS) $(LIBEDIT_LIBS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = librelic.pc

# The m4 macro files are distributed by automake already, as dependencies of
# configure; listing them here as well would only create a list that a sixth
# macro file could be left out of.
#
# The project's documents are distributed with the source, so that whoever
# audits the code has its design beside it: the architecture, the decisions
# and the design of each subsystem, and what remains open.
# specs/ and docs/ are named whole, as tests/lang is, so that a new design
# document cannot be left out of the tarball.
EXTRA_DIST = autogen.sh EXAMPLES.md LICENSE README.md BENCHMARKS.md bench/baseline.txt \
	TODO.md docs specs website wasm tests/wasm tests/check-website.py \
	bench/datasets.sh bench/run.sh \
	bench/compare/results.tsv bench/compare/suite-summary.tsv \
	bench/compare/tools/plot-performance.py \
	bench/compare/tools/test_plot_performance.py \
	bench/compare/run.sh bench/compare/sweep.sh bench/compare/timing.awk bench/compare/pairset.c \
	tests/fixtures/bench \
	compat/README.md examples/embed.c tests/coverage-report.sh \
	tests/fuzz/corpus tests/fuzz/corpus-import tests/fuzz/corpus-regex \
	tests/fuzz/regex.dict tests/fuzz/relic.dict tests/lang \
	tests/hidden-answers.awk tests/langtest.sh tests/strict.sh tests/object-tools.sh

##
## The example is built and run by make installcheck, against the installed
## header and library as pkg-config describes them, and never against the
## build tree: that is the only arrangement that catches a missing export, a
## public header that needs a private one, or a librelic.pc that points at the
## wrong place.  make distcheck runs it.  The installed library is found at run
## time through the loader's path as well, for a platform whose linker does
## not record where a library was installed.  The configured compiler and
## linker flags apply, so that a cross or multilib build links the example
## the way it linked the library, and an instrumented library links an
## instrumented example.  Without pkg-config there is nothing to
## check the installation against, and the skip says so rather than passing
## quietly, or fails under RELIC_STRICT_CHECK=yes unless RELIC_ALLOWED_SKIPS
## names installcheck; pkg-config is not required to build or install relic.  The
## example is built in a directory of its own and removed with it, together
## with whatever else the compiler leaves beside an executable.
##
installcheck-local:
	@if ! command -v pkg-config >/dev/null 2>&1; then \
		check=installcheck; \
		reason="SKIP: examples/embed.c: pkg-config is not installed,"; \
		reason="$$reason so the installed library was not checked"; \
		$(RELIC_UNAVAILABLE_CHECK); \
	fi; \
	PKG_CONFIG_PATH='$(libdir)/pkgconfig'; export PKG_CONFIG_PATH; \
	work=$$(mktemp -d) || exit 1; \
	echo "building examples/embed.c against the installed librelic"; \
	$(CC) $(RELIC_STRICT_CFLAGS) $(RELIC_WARN_CFLAGS) \
	    $(RELIC_WERROR_CFLAGS) $(RELIC_SAN_CFLAGS) $(CPPFLAGS) $(CFLAGS) \
	    $$(pkg-config --cflags librelic) \
	    -o "$$work/embed$(EXEEXT)" $(srcdir)/examples/embed.c \
	    $(LDFLAGS) $$(pkg-config $(RELIC_PKG_CONFIG_FLAGS) --libs librelic); status=$$?; \
	if [ $$status -eq 0 ]; then \
		LD_LIBRARY_PATH='$(libdir)' "$$work/embed$(EXEEXT)"; \
		status=$$?; \
	fi; \
	rm -rf "$$work"; \
	exit $$status

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
## the sanitizers on the machine it was written on.
##
## Fortification is turned off for that build.  A fortified C library header
## may define these interfaces as function-like macros, which take
## precedence over the renaming and leave the replacement's own definition
## colliding with a compiler builtin, or as inline wrappers that call the C
## library's own function by a name the renaming cannot reach, so that the
## tests would check the C library and pass whatever the replacements did.
## It is turned off by a header forced in with -include, which the compiler
## reads after every -D and -U, so that a packager's hardening flags, which
## come after this target's on the command line, cannot turn it back on.
##

RELIC_COMPAT_RENAMES = \
	-Dgetentropy=relic_replacement_getentropy \
	-Dmemmem=relic_replacement_memmem \
	-Dreallocarray=relic_replacement_reallocarray \
	-Dstrlcat=relic_replacement_strlcat \
	-Dstrlcpy=relic_replacement_strlcpy \
	-Dstrtonum=relic_replacement_strtonum

RELIC_TEST_BINARIES = tests/unit/test_compat \
	tests/unit/test_compat_replacements \
	tests/unit/test_header

tests_unit_test_compat_SOURCES = tests/unit/test_compat.c \
	tests/unit/harness.c
tests_unit_test_compat_LDADD = $(LTLIBOBJS)

tests_unit_test_compat_replacements_SOURCES = tests/unit/test_compat.c \
	tests/unit/harness.c tests/unit/unfortified.h compat/getentropy.c \
	compat/memmem.c compat/reallocarray.c compat/strlcat.c \
	compat/strlcpy.c compat/strtonum.c
tests_unit_test_compat_replacements_CPPFLAGS = $(AM_CPPFLAGS) \
	-include $(top_srcdir)/tests/unit/unfortified.h $(RELIC_COMPAT_RENAMES)

##
## One test binary per module, driving each through the interface its callers
## use.  They link the convenience library rather than librelic.la because
## every module below the public interface is hidden from a program that
## links the installed library, which is the property librelic.la exists to
## have.
##

RELIC_TEST_BINARIES += tests/unit/test_api tests/unit/test_arena \
	tests/unit/test_arith tests/unit/test_compute tests/unit/test_cost \
	tests/unit/test_dense tests/unit/test_diag \
	tests/unit/test_eval tests/unit/test_exec \
	tests/unit/test_facts tests/unit/test_fold tests/unit/test_grouped tests/unit/test_import \
	tests/unit/test_index tests/unit/test_interrupt \
	tests/unit/test_lex tests/unit/test_magic tests/unit/test_merge \
	tests/unit/test_parse \
	tests/unit/test_plan tests/unit/test_pred tests/unit/test_query \
	tests/unit/test_regex tests/unit/test_relation tests/unit/test_round \
	tests/unit/test_rule tests/unit/test_stratify tests/unit/test_strfn \
	tests/unit/test_serial_seen tests/unit/test_symbol \
	tests/unit/test_text tests/unit/test_util \
	tests/unit/test_value

## test_api links the installed form of the library rather than the
## convenience library, so an entry point it calls that the export list or
## hidden visibility left out fails to link.
tests_unit_test_api_SOURCES = tests/unit/test_api.c tests/unit/harness.c
tests_unit_test_api_LDADD = librelic.la

## test_interrupt, too, is written as an embedder writes it.  Its deadline
## scenario interrupts from a thread of its own.
tests_unit_test_interrupt_SOURCES = tests/unit/test_interrupt.c \
	tests/unit/harness.c
tests_unit_test_interrupt_CFLAGS = $(AM_CFLAGS) -pthread
tests_unit_test_interrupt_LDADD = librelic.la

tests_unit_test_arena_SOURCES = tests/unit/test_arena.c tests/unit/harness.c
tests_unit_test_arena_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_arith_SOURCES = tests/unit/test_arith.c tests/unit/harness.c
tests_unit_test_arith_LDADD = librelic_internal.la $(LTLIBOBJS)

## Checked arithmetic uses the compiler's overflow builtins where it has
## them; this program compiles the portable comparisons instead and runs the
## same suite, so that both forms are held to the same answers.
RELIC_TEST_BINARIES += tests/unit/test_arith_portable
tests_unit_test_arith_portable_SOURCES = tests/unit/test_arith.c \
	tests/unit/harness.c lib/arith.c
tests_unit_test_arith_portable_CPPFLAGS = $(AM_CPPFLAGS) \
	-DRELIC_ARITH_PORTABLE

tests_unit_test_cost_SOURCES = tests/unit/test_cost.c tests/unit/harness.c
tests_unit_test_cost_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_diag_SOURCES = tests/unit/test_diag.c tests/unit/harness.c
tests_unit_test_diag_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_eval_SOURCES = tests/unit/test_eval.c tests/unit/harness.c
tests_unit_test_eval_LDADD = librelic_internal.la $(LTLIBOBJS)

## The serial-cache boundary is private to eval.c. This test includes it to
## check compacted refusal prefixes and optional allocation failure without
## exposing a runtime test interface. Suffix substitution excludes that one
## evaluator object; every other module is the ordinary built object.
tests_unit_test_serial_seen_SOURCES = tests/unit/test_serial_seen.c \
	tests/unit/harness.c
tests_unit_test_serial_seen_LDADD = \
	$(librelic_internal_la_OBJECTS:lib/eval.lo=) $(LTLIBOBJS)

## Include the ordinary round driver so shared retry and worker coordination
## are tested without exposing a runtime fault interface.
tests_unit_test_grouped_SOURCES = tests/unit/test_grouped.c tests/unit/harness.c
tests_unit_test_grouped_LDADD = \
	$(librelic_internal_la_OBJECTS:lib/round.lo=) $(LTLIBOBJS)

tests_unit_test_exec_SOURCES = tests/unit/test_exec.c tests/unit/harness.c
tests_unit_test_exec_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_facts_SOURCES = tests/unit/test_facts.c tests/unit/harness.c \
	tests/unit/programs.c
tests_unit_test_facts_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_fold_SOURCES = tests/unit/test_fold.c tests/unit/harness.c
tests_unit_test_fold_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_import_SOURCES = tests/unit/test_import.c tests/unit/harness.c
tests_unit_test_import_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_index_SOURCES = tests/unit/test_index.c tests/unit/harness.c
tests_unit_test_index_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_lex_SOURCES = tests/unit/test_lex.c tests/unit/harness.c
tests_unit_test_lex_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_magic_SOURCES = tests/unit/test_magic.c tests/unit/harness.c \
	tests/unit/programs.c
tests_unit_test_magic_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_merge_SOURCES = tests/unit/test_merge.c tests/unit/harness.c
tests_unit_test_merge_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_dense_SOURCES = tests/unit/test_dense.c tests/unit/harness.c
tests_unit_test_dense_LDADD = librelic_internal.la $(LTLIBOBJS)

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

tests_unit_test_round_SOURCES = tests/unit/test_round.c tests/unit/harness.c
tests_unit_test_round_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_regex_SOURCES = tests/unit/test_regex.c tests/unit/harness.c
tests_unit_test_regex_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_rule_SOURCES = tests/unit/test_rule.c tests/unit/harness.c
tests_unit_test_rule_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_stratify_SOURCES = tests/unit/test_stratify.c \
	tests/unit/harness.c
# The long-chain test runs its analysis on a thread with a small stack.
tests_unit_test_stratify_CFLAGS = $(AM_CFLAGS) -pthread
tests_unit_test_stratify_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_strfn_SOURCES = tests/unit/test_strfn.c tests/unit/harness.c
tests_unit_test_strfn_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_symbol_SOURCES = tests/unit/test_symbol.c tests/unit/harness.c
tests_unit_test_symbol_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_text_SOURCES = tests/unit/test_text.c tests/unit/harness.c
tests_unit_test_text_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_util_SOURCES = tests/unit/test_util.c tests/unit/harness.c
tests_unit_test_util_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_compute_SOURCES = tests/unit/test_compute.c \
	tests/unit/harness.c
tests_unit_test_compute_LDADD = librelic_internal.la $(LTLIBOBJS)
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
## getentropy(2) is renamed the same way, and for the same reason: a context
## that cannot draw its hash seed is not created, and only a replacement that
## fails on demand can show that it is not.  So is fclose(3): a load or an
## import whose file fails to close must add nothing, and no file a test can
## make fails to close on its own.
##
## RELIC_ARENA_FAULT names the function lib/arena.c asks before serving any
## request.  An arena calls the allocator only when a request opens a chunk,
## so a sweep of the allocator's requests reaches only the callers whose
## requests happen to fall on a chunk boundary; the suite sweeps the arena's
## requests through this point as well, which reaches every caller.
##
## The renaming follows the pattern the compat suite already uses.  It is
## confined to this program; nothing that ships is built this way.
## tests/allocators.sh checks the objects this program links, so that an
## allocator the renaming misses fails the build's checks rather than
## quietly narrowing what the suite refuses.
##

RELIC_EXHAUSTION_RENAMES = \
	-Dcalloc=relic_test_calloc \
	-Dfclose=relic_test_fclose \
	-Dgetentropy=relic_test_getentropy \
	-Dmalloc=relic_test_malloc \
	-Drealloc=relic_test_realloc \
	-Dreallocarray=relic_test_reallocarray \
	-DRELIC_ARENA_FAULT=relic_test_arena_refuses

RELIC_TEST_BINARIES += tests/unit/test_exhaustion
tests_unit_test_exhaustion_SOURCES = tests/unit/test_exhaustion.c \
	tests/unit/harness.c tests/unit/programs.c \
	$(librelic_internal_la_SOURCES)
tests_unit_test_exhaustion_CPPFLAGS = $(AM_CPPFLAGS) \
	$(RELIC_EXHAUSTION_RENAMES)
tests_unit_test_exhaustion_LDADD = $(LTLIBOBJS)

##
## The symbol pool and the integer table each number at most 2^30 - 1
## entries, below the type bits of a domain identifier, far more than a test
## can intern, and a string value holds at most 2^32 - 2 bytes, more than a
## test can concatenate.  This program compiles the library from source with
## all three ceilings lowered, as the allocation-failure suite compiles it
## with its allocators renamed, so that the refusal at a ceiling is actually
## taken.
##

RELIC_CEILING_DEFINES = -DRELIC_SYMBOL_MAX=64 -DRELIC_INTEGER_MAX=64 \
	-DRELIC_STRING_LENGTH_MAX=64

RELIC_TEST_BINARIES += tests/unit/test_ceiling
tests_unit_test_ceiling_SOURCES = tests/unit/test_ceiling.c \
	tests/unit/harness.c $(librelic_internal_la_SOURCES)
tests_unit_test_ceiling_CPPFLAGS = $(AM_CPPFLAGS) $(RELIC_CEILING_DEFINES)
tests_unit_test_ceiling_LDADD = $(LTLIBOBJS)

##
## A batch import reads a table in windows of records, cut into pieces at
## record ends, out of reads of a fixed chunk, all large enough that most
## tables the import suite reads fit in one window.  This program compiles
## the library from source with all three made small, as the ceiling suite
## lowers its ceilings, and runs the import suite again, so that its tables
## span many windows and pieces and the reads cut them at every byte.
##
## Only a regular file is batched, and no regular file a test can make fails
## to read.  RELIC_IMPORT_REGULAR_FILE names the function lib/import.c asks
## whether to take a stream for a regular file as well, so that this program
## batches the failing streams of the suite's own and reaches the read
## errors a batch import meets.
##

RELIC_SMALL_WINDOWS = -DRELIC_IMPORT_READ_CHUNK=16u \
	-DRELIC_IMPORT_WINDOW_BYTES=64u -DRELIC_IMPORT_PIECE_BYTES=8u \
	-DRELIC_IMPORT_REGULAR_FILE=relic_test_regular_file

RELIC_TEST_BINARIES += tests/unit/test_import_small_windows
tests_unit_test_import_small_windows_SOURCES = tests/unit/test_import.c \
	tests/unit/harness.c $(librelic_internal_la_SOURCES)
tests_unit_test_import_small_windows_CPPFLAGS = $(AM_CPPFLAGS) \
	$(RELIC_SMALL_WINDOWS)
tests_unit_test_import_small_windows_LDADD = $(LTLIBOBJS)

##
## The canonical index and every secondary index's key table are striped,
## RELIC_STRIPES tables a key is spread over by its hash.  These programs
## compile the library from source with one stripe, as the allocation-failure
## suite compiles it with its allocators renamed, and run the relation and
## index suites again, so that membership, lookup, growth and accounting are
## held to the same answers whatever the stripe count.
##

RELIC_ONE_STRIPE = -DRELIC_STRIPE_BITS=0

RELIC_TEST_BINARIES += tests/unit/test_relation_one_stripe
tests_unit_test_relation_one_stripe_SOURCES = tests/unit/test_relation.c \
	tests/unit/harness.c $(librelic_internal_la_SOURCES)
tests_unit_test_relation_one_stripe_CPPFLAGS = $(AM_CPPFLAGS) \
	$(RELIC_ONE_STRIPE)
tests_unit_test_relation_one_stripe_LDADD = $(LTLIBOBJS)

RELIC_TEST_BINARIES += tests/unit/test_index_one_stripe
tests_unit_test_index_one_stripe_SOURCES = tests/unit/test_index.c \
	tests/unit/harness.c $(librelic_internal_la_SOURCES)
tests_unit_test_index_one_stripe_CPPFLAGS = $(AM_CPPFLAGS) \
	$(RELIC_ONE_STRIPE)
tests_unit_test_index_one_stripe_LDADD = $(LTLIBOBJS)

##
## A context's worker pool starts and joins threads the operating system
## does not count portably.  This program compiles the library from source
## with pthread_create() and pthread_join() renamed, as the allocation-failure
## suite renames its allocators, so that the test counts every thread started
## and joined and can refuse a start.
##

RELIC_POOL_RENAMES = \
	-Dpthread_create=relic_test_pthread_create \
	-Dpthread_join=relic_test_pthread_join

RELIC_TEST_BINARIES += tests/unit/test_pool
tests_unit_test_pool_SOURCES = tests/unit/test_pool.c \
	tests/unit/harness.c $(librelic_internal_la_SOURCES)
tests_unit_test_pool_CPPFLAGS = $(AM_CPPFLAGS) $(RELIC_POOL_RENAMES)
tests_unit_test_pool_LDADD = $(LTLIBOBJS)

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
RELIC_TEST_BINARIES += tests/unit/test_header_cxx
tests_unit_test_header_cxx_SOURCES = tests/unit/test_header_cxx.cc
tests_unit_test_header_cxx_CPPFLAGS = -I$(top_srcdir)/include
tests_unit_test_header_cxx_CXXFLAGS = $(RELIC_SAN_CFLAGS)
tests_unit_test_header_cxx_LDADD = librelic.la
endif

dist_check_SCRIPTS = tests/allocators.sh tests/ascii.sh tests/bench-output.sh tests/bench-pairset.sh tests/bench-sweep.sh tests/bench-timing.sh tests/cli.sh \
	tests/globals.sh tests/hidden-answers.sh tests/langfiles.sh \
	tests/magic.sh tests/man.sh tests/object-audits.sh tests/output.sh tests/perf.sh \
	tests/pkgconfig.sh tests/reentry.sh tests/repl.sh tests/roundtrip.sh \
	tests/skips.sh tests/symbols.sh tests/version.sh

if RELIC_THINLTO
dist_check_SCRIPTS += tests/thinlto.sh
endif

##
## Every example the manual pages, README.md and EXAMPLES.md show is run by
## tests/docs.sh against its entry in tests/examples/expected, in a copy of
## tests/examples, which holds the files they name.  The directory is not
## named tests/docs, as make's built-in suffix rule would build a file of that
## name from tests/docs.sh when make dist asks for it.  The C example of
## relic(3) is examples/query.c, built here as an embedder builds it, with
## only the public header on its include path.
##

dist_check_SCRIPTS += tests/docs.sh
EXTRA_DIST += tests/examples

RELIC_EXAMPLE_BINARIES = examples/query
examples_query_SOURCES = examples/query.c
examples_query_CPPFLAGS = -I$(top_srcdir)/include
examples_query_LDADD = librelic.la

##
## What the tool does at a terminal is checked by tests/terminal.sh, and its
## line editing by tests/editing.sh, through tests/pty, which runs a command
## on a pseudo-terminal and types its input a line at a time, since a
## pseudo-terminal cannot be driven portably from sh.
##

dist_check_SCRIPTS += tests/terminal.sh tests/editing.sh

RELIC_DRIVER_BINARIES = tests/pty
tests_pty_SOURCES = tests/pty.c

##
## The equivalence suite runs every language test under each alternative
## evaluation strategy -X selects and compares it with the default, and
## again with every answer gathered before its rows are written, which must
## write the same rows in the same order.  Its corpus is RELIC_LANG_TESTS
## below, so it needs no list of its own.
##

dist_check_SCRIPTS += tests/equiv/strategies.sh tests/equiv/streaming.sh

##
## The randomized differential suite generates programs from seeds, runs each
## under every strategy the public configuration selects, and shrinks a
## program that makes two disagree.  It links the installed form of the
## library, as an embedder would, because every equivalence it checks is
## reachable through <relic.h>.  make check runs it over fifty fixed seeds,
## and the self-test shows it can fail: a fault injected into one strategy
## must be found and shrunk.  make check-random runs a range of any size.
##

RELIC_TEST_BINARIES += tests/random/random
tests_random_random_SOURCES = tests/random/answer.c tests/random/answer.h \
	tests/random/compare.c tests/random/generate.c \
	tests/random/main.c tests/random/program.c tests/random/shrink.c \
	tests/random/random.h
tests_random_random_LDADD = librelic.la $(LTLIBOBJS)

dist_check_SCRIPTS += tests/random/selftest.sh

RELIC_SEEDS = 1000
RELIC_SEED_START = 1

## A build configured with a sanitizer runs the driver, and the fuzz targets
## below, under the options the sanitizer targets use, so that a long run
## holds a program to what make check-asan and check-ubsan hold the suite
## to; a build without one ignores them.
RELIC_RUN_SANITIZED = ASAN_OPTIONS='$(RELIC_ASAN_OPTIONS)' \
	UBSAN_OPTIONS='$(RELIC_UBSAN_OPTIONS)'

check-random: tests/random/random$(EXEEXT)
	$(RELIC_RUN_SANITIZED) $(builddir)/tests/random/random \
	    -n $(RELIC_SEEDS) -s $(RELIC_SEED_START)

##
## Fuzz targets
##
## Each target is an LLVMFuzzerTestOneInput(), over the public interface but
## for the regex target, which feeds the matcher behind `match` directly, and
## each is linked twice.  The replay driver, built always, passes every file of a
## corpus to the target once, so make check replays the corpora on every
## compiler and under every sanitizer build.  With --enable-fuzzers the
## target is also linked with libFuzzer, and make fuzz runs each for
## FUZZ_SECONDS over a working corpus in the build tree seeded from the
## language tests and tests/fuzz/corpus, or for the import and regex
## targets, whose inputs are tables and patterns, from
## tests/fuzz/corpus-import and tests/fuzz/corpus-regex; the source tree's
## corpora change only when someone copies a finding into them.  configure
## then compiles the whole tree with the coverage instrumentation libFuzzer
## steers by, and the targets alone with its main().  No clean target removes
## fuzz-corpus or fuzz-artifacts: an input not yet looked at is the one thing
## a fuzzing run produces that cannot be made again.
##

RELIC_FUZZ_COMMON = tests/fuzz/fuzz.c tests/fuzz/fuzz.h
RELIC_FUZZ_EVAL_SOURCES = tests/fuzz/fuzz_eval.c tests/random/answer.c \
	tests/random/answer.h $(RELIC_FUZZ_COMMON)

RELIC_REPLAY_BINARIES = tests/fuzz/replay_eval tests/fuzz/replay_import \
	tests/fuzz/replay_load tests/fuzz/replay_regex
tests_fuzz_replay_eval_SOURCES = tests/fuzz/replay.c $(RELIC_FUZZ_EVAL_SOURCES)
tests_fuzz_replay_eval_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_replay_import_SOURCES = tests/fuzz/replay.c \
	tests/fuzz/fuzz_import.c $(RELIC_FUZZ_COMMON)
tests_fuzz_replay_import_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_replay_load_SOURCES = tests/fuzz/replay.c tests/fuzz/fuzz_load.c \
	$(RELIC_FUZZ_COMMON)
tests_fuzz_replay_load_LDADD = librelic.la $(LTLIBOBJS)
## The regex target reaches a module the library does not export, so it
## links the convenience library as the unit tests do.
tests_fuzz_replay_regex_SOURCES = tests/fuzz/replay.c \
	tests/fuzz/fuzz_regex.c tests/fuzz/fuzz.h
tests_fuzz_replay_regex_LDADD = librelic_internal.la $(LTLIBOBJS)

dist_check_SCRIPTS += tests/fuzz/replay.sh

FUZZ_SECONDS = 60

if RELIC_FUZZERS
noinst_PROGRAMS = tests/fuzz/fuzz_eval tests/fuzz/fuzz_import \
	tests/fuzz/fuzz_load tests/fuzz/fuzz_regex
tests_fuzz_fuzz_eval_SOURCES = $(RELIC_FUZZ_EVAL_SOURCES)
tests_fuzz_fuzz_eval_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_eval_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_fuzz_import_SOURCES = tests/fuzz/fuzz_import.c $(RELIC_FUZZ_COMMON)
tests_fuzz_fuzz_import_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_import_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_fuzz_load_SOURCES = tests/fuzz/fuzz_load.c $(RELIC_FUZZ_COMMON)
tests_fuzz_fuzz_load_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_load_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_fuzz_regex_SOURCES = tests/fuzz/fuzz_regex.c tests/fuzz/fuzz.h
tests_fuzz_fuzz_regex_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_regex_LDADD = librelic_internal.la $(LTLIBOBJS)

# -max_len bounds an input's text, and with it how long a program can take
# to load and check; -timeout reports an input that runs longer anyway.  An
# evaluation can stay inside every limit and still run long -- a cross
# product whose tuples are all duplicates -- so a timeout from the eval
# target needs reading before it is called a bug.  The import target reads
# tables rather than programs, so it takes neither the language's dictionary
# nor its seeds; the regex target takes a dictionary of the pattern
# dialect's metacharacters.
fuzz: $(noinst_PROGRAMS)
	@for target in load eval; do \
		corpus=fuzz-corpus/$$target; \
		artifacts=fuzz-artifacts/$$target/; \
		$(MKDIR_P) $$corpus $$artifacts || exit 1; \
		echo "fuzzing $$target for $(FUZZ_SECONDS) seconds"; \
		$(RELIC_RUN_SANITIZED) ./tests/fuzz/fuzz_$$target \
		    -max_total_time=$(FUZZ_SECONDS) \
		    -max_len=4096 -timeout=10 \
		    -dict=$(srcdir)/tests/fuzz/relic.dict \
		    -artifact_prefix=$$artifacts $$corpus \
		    $(srcdir)/tests/lang $(srcdir)/tests/fuzz/corpus || exit 1; \
	done
	@$(MKDIR_P) fuzz-corpus/import fuzz-artifacts/import/
	@echo "fuzzing import for $(FUZZ_SECONDS) seconds"
	@$(RELIC_RUN_SANITIZED) ./tests/fuzz/fuzz_import \
	    -max_total_time=$(FUZZ_SECONDS) \
	    -max_len=4096 -timeout=10 \
	    -artifact_prefix=fuzz-artifacts/import/ fuzz-corpus/import \
	    $(srcdir)/tests/fuzz/corpus-import
	@$(MKDIR_P) fuzz-corpus/regex fuzz-artifacts/regex/
	@echo "fuzzing regex for $(FUZZ_SECONDS) seconds"
	@$(RELIC_RUN_SANITIZED) ./tests/fuzz/fuzz_regex \
	    -max_total_time=$(FUZZ_SECONDS) \
	    -max_len=4096 -timeout=10 \
	    -dict=$(srcdir)/tests/fuzz/regex.dict \
	    -artifact_prefix=fuzz-artifacts/regex/ fuzz-corpus/regex \
	    $(srcdir)/tests/fuzz/corpus-regex
else
fuzz:
	@echo "the fuzz targets need configure --enable-fuzzers and a" \
	    "compiler with libFuzzer; skipping."
endif

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
## tests/lang/LANGUAGE names the tests whose expectation the language alone
## fixes, which another implementation of the language can run, and
## langfiles.sh holds it to the tests in both directions as well.
##

RELIC_LANG_TESTS = \
	tests/lang/aggregate-anonymous.dl \
	tests/lang/aggregate-assign.dl tests/lang/aggregate-columns.dl \
	tests/lang/aggregate-count.dl \
	tests/lang/aggregate-explain.dl tests/lang/aggregate-limit.dl \
	tests/lang/aggregate-limit-long-name.dl \
	tests/lang/aggregate-limit-query.dl \
	tests/lang/aggregate-magic.dl tests/lang/aggregate-minmax.dl \
	tests/lang/aggregate-mixed.dl tests/lang/aggregate-nested.dl \
	tests/lang/aggregate-outcome.dl \
	tests/lang/aggregate-overflow.dl tests/lang/aggregate-parallel.dl \
	tests/lang/aggregate-project.dl tests/lang/aggregate-query.dl \
	tests/lang/aggregate-relation.dl \
	tests/lang/aggregate-shared.dl tests/lang/aggregate-shortest.dl \
	tests/lang/aggregate-stratify.dl tests/lang/aggregate-sum.dl \
	tests/lang/aggregate-syntax.dl tests/lang/aggregate-unsafe.dl \
	tests/lang/aggregate-variable-limit.dl \
	tests/lang/arithmetic-binding.dl \
	tests/lang/arithmetic-depth.dl tests/lang/arithmetic-explain.dl \
	tests/lang/arithmetic-gathered.dl tests/lang/arithmetic-height.dl \
	tests/lang/arithmetic-magic.dl tests/lang/arithmetic-magic-drafts.dl \
	tests/lang/arithmetic-magic-plan.dl \
	tests/lang/arithmetic-magic-wide.dl tests/lang/arithmetic-memory.dl \
	tests/lang/arithmetic-mixed.dl tests/lang/arithmetic-negation.dl \
	tests/lang/arithmetic-negative.dl tests/lang/arithmetic-order.dl \
	tests/lang/arithmetic-overflow.dl tests/lang/arithmetic-prefix.dl \
	tests/lang/arithmetic-probe.dl \
	tests/lang/arithmetic-recursion.dl \
	tests/lang/arithmetic-strategy.dl \
	tests/lang/arithmetic-strategy-full.dl \
	tests/lang/arithmetic-syntax.dl tests/lang/arithmetic-unsafe.dl \
	tests/lang/arithmetic-zero.dl tests/lang/arithmetic-zero-query.dl \
	tests/lang/arity-32.dl tests/lang/arity-limit.dl \
	tests/lang/arity-limit-name.dl tests/lang/bad-backslash.dl \
	tests/lang/bad-byte.dl tests/lang/bad-escape-at-end.dl \
	tests/lang/bad-escape.dl tests/lang/check-queries.dl \
	tests/lang/closure-double.dl tests/lang/closure-graphs.dl \
	tests/lang/comments-only.dl tests/lang/components.dl \
	tests/lang/constructs.dl tests/lang/empty.dl \
	tests/lang/error-after-recovery.dl tests/lang/error-first-token.dl \
	tests/lang/exists.dl tests/lang/exists-gathered.dl \
	tests/lang/exists-streamed.dl \
	tests/lang/explain-abandoned.dl tests/lang/explain-check.dl \
	tests/lang/explain-exists.dl tests/lang/explain-exists-no-index.dl \
	tests/lang/explain-facts.dl \
	tests/lang/explain-free.dl \
	tests/lang/explain-graph.dl tests/lang/explain-long.dl \
	tests/lang/explain-magic.dl tests/lang/explain-materialized.dl \
	tests/lang/explain-ops.dl tests/lang/explain-restating.dl \
	tests/lang/explain-retained.dl \
	tests/lang/explain-subsumed.dl tests/lang/explain-supplementary.dl \
	tests/lang/explain-tautology.dl tests/lang/explain-views.dl \
	tests/lang/export-arities.dl tests/lang/export-arities-head.dl \
	tests/lang/export-arity.dl tests/lang/export-rows.dl \
	tests/lang/export-undefined.dl tests/lang/export-unknown.dl \
	tests/lang/export-usage-arity.dl \
	tests/lang/export-usage-extension.dl \
	tests/lang/export-usage-format.dl tests/lang/export-usage-head.dl \
	tests/lang/export-usage-none.dl tests/lang/export-usage-output.dl \
	tests/lang/export-usage-path.dl tests/lang/export-usage-predicate.dl \
	tests/lang/export-usage-stdout.dl tests/lang/export-usage-stem.dl \
	tests/lang/export-usage-twice.dl tests/lang/export-usage-wide.dl \
	tests/lang/float-exponent.dl tests/lang/float-literal.dl \
	tests/lang/float-period.dl \
	tests/lang/format-csv.dl tests/lang/format-integers.dl \
	tests/lang/format-json.dl tests/lang/format-nul.dl \
	tests/lang/format-text.dl tests/lang/format-tsv.dl \
	tests/lang/head-not-atom.dl tests/lang/import-check.dl \
	tests/lang/import-csv.dl tests/lang/import-declared.dl \
	tests/lang/import-errors-csv.dl tests/lang/import-errors-tsv.dl \
	tests/lang/import-errors-typed.dl tests/lang/import-order.dl \
	tests/lang/import-stdin.dl tests/lang/import-tsv.dl \
	tests/lang/import-usage-arity.dl tests/lang/import-usage-column.dl \
	tests/lang/import-usage-declare-column.dl \
	tests/lang/import-usage-declare.dl tests/lang/import-usage-differ.dl \
	tests/lang/import-usage-disagree.dl \
	tests/lang/import-usage-equals.dl \
	tests/lang/import-usage-extension.dl \
	tests/lang/import-usage-format.dl tests/lang/import-usage-not.dl \
	tests/lang/import-usage-option.dl tests/lang/import-usage-parts.dl \
	tests/lang/import-usage-path.dl tests/lang/import-usage-predicate.dl \
	tests/lang/import-usage-stdin.dl tests/lang/import-usage-stem.dl \
	tests/lang/import-usage-twice.dl tests/lang/import-usage-types.dl \
	tests/lang/import-wide.dl tests/lang/import_path.dl \
	tests/lang/incomplete-operator.dl tests/lang/integer-range.dl \
	tests/lang/integer-trailing.dl tests/lang/magic-bindings.dl \
	tests/lang/magic-cspa.dl tests/lang/magic-default.dl \
	tests/lang/magic-default-adornments.dl \
	tests/lang/magic-default-search.dl \
	tests/lang/magic-duplicate-rules.dl tests/lang/magic-limit.dl \
	tests/lang/magic-limit-adorned.dl tests/lang/magic-limit-demand.dl \
	tests/lang/magic-limit-dropped.dl tests/lang/magic-limit-query.dl \
	tests/lang/magic-limit-raised.dl tests/lang/magic-limit-subsumed.dl \
	tests/lang/magic-limit-supplementary.dl \
	tests/lang/magic-limit-wide-cut.dl tests/lang/magic-mixed.dl \
	tests/lang/magic-mutual.dl tests/lang/magic-negation.dl \
	tests/lang/magic-same-generation.dl tests/lang/magic-subsumed.dl \
	tests/lang/magic-supplementary-wide.dl \
	tests/lang/magic-supplementary.dl tests/lang/materialize.dl \
	tests/lang/missing-terminator.dl \
	tests/lang/negation-anonymous.dl \
	tests/lang/negation-anonymous-explain.dl \
	tests/lang/negation-forms.dl \
	tests/lang/negation-recursive.dl tests/lang/negation-strata.dl \
	tests/lang/negation-tasks.dl tests/lang/not-as-predicate.dl \
	tests/lang/not-symbol.dl tests/lang/predicate-identity.dl \
	tests/lang/prefix-quantum.dl \
	tests/lang/query-bindings.dl tests/lang/query-columns-limit.dl \
	tests/lang/query-columns.dl tests/lang/query-comparisons.dl \
	tests/lang/query-duplicates.dl tests/lang/query-graph.dl \
	tests/lang/query-none.dl tests/lang/query-not-a-query.dl \
	tests/lang/query-option-unsafe.dl tests/lang/query-option.dl \
	tests/lang/query-recursion.dl tests/lang/query-undefined.dl \
	tests/lang/query-unsafe.dl tests/lang/query-zero-arity.dl \
	tests/lang/recovery.dl tests/lang/recursion-late-index.dl \
	tests/lang/repl-check.dl \
	tests/lang/repl-discard.dl tests/lang/repl-errors.dl \
	tests/lang/repl-json.dl tests/lang/repl-limit.dl \
	tests/lang/repl-plan-materialized.dl tests/lang/repl-plan.dl \
	tests/lang/repl-session.dl tests/lang/repl-verbose.dl \
	tests/lang/reserved-bytes.dl \
	tests/lang/safe-rules.dl tests/lang/singleton.dl \
	tests/lang/sort-mixed.dl tests/lang/stats-exists.dl \
	tests/lang/stats-indexed.dl \
	tests/lang/stats-magic.dl tests/lang/stats-naive.dl \
	tests/lang/stats-no-exists.dl \
	tests/lang/stats-semi-naive.dl tests/lang/string-escapes.dl \
	tests/lang/strings-aggregate.dl tests/lang/strings-atom-or-call.dl \
	tests/lang/strings-binding.dl tests/lang/strings-bytes.dl \
	tests/lang/strings-explain.dl tests/lang/strings-gathered.dl \
	tests/lang/strings-index.dl tests/lang/strings-magic.dl \
	tests/lang/strings-match.dl tests/lang/strings-match-bad.dl \
	tests/lang/strings-match-fails.dl \
	tests/lang/strings-memory.dl tests/lang/strings-mixed.dl \
	tests/lang/strings-parallel.dl tests/lang/strings-probe.dl \
	tests/lang/strings-recursion.dl tests/lang/strings-str.dl \
	tests/lang/strings-substr.dl tests/lang/strings-syntax.dl \
	tests/lang/success-criteria.dl tests/lang/trailing-comma.dl \
	tests/lang/undefined-predicate.dl tests/lang/unexpected-token.dl \
	tests/lang/unsafe-anonymous.dl tests/lang/unsafe-comparison.dl \
	tests/lang/unsafe-fact.dl tests/lang/unsafe-head.dl \
	tests/lang/unsafe-negation.dl tests/lang/unstratified-cycles.dl \
	tests/lang/unstratified-policy.dl tests/lang/unstratified-query.dl \
	tests/lang/unstratified-unsafe.dl tests/lang/unstratified.dl \
	tests/lang/unterminated-escape.dl tests/lang/unterminated-string.dl \
	tests/lang/variable-limit-max.dl tests/lang/variable-limit.dl \
	tests/lang/verbose-counts.dl tests/lang/verbose-discard.dl \
	tests/lang/verbose-mixed.dl tests/lang/verbose-one.dl \
	tests/lang/verbose-prefix.dl tests/lang/zero-arity.dl

# Every test runs through tests/strict.sh, whatever its kind, so that
# RELIC_STRICT_CHECK=yes in the environment turns a skipped test into a
# failed one for make check, make distcheck and the sanitizer suites alike.
RELIC_STRICT = $(SHELL) $(abs_top_srcdir)/tests/strict.sh

# A whole check that cannot run, such as a sanitizer suite the compiler cannot
# build, ends its recipe here with $$check naming it and $$reason saying why.
# It is put to tests/strict.sh as a stand-in test that skips, so the one
# policy decides: the recipe succeeds where a skip is allowed and fails where
# strict mode forbids it, and RELIC_ALLOWED_SKIPS can name the check.
RELIC_UNAVAILABLE_CHECK = \
	status=0; \
	$(RELIC_STRICT) $(SHELL) -c 'echo "$$0"; exit 77' \
	    "$$reason" "$$check" || status=$$?; \
	if test $$status -eq 77; then exit 0; fi; \
	exit $$status

TEST_EXTENSIONS = .dl .sh
LOG_COMPILER = $(RELIC_STRICT)
DL_LOG_COMPILER = $(RELIC_STRICT) $(SHELL) $(abs_top_srcdir)/tests/langtest.sh
SH_LOG_COMPILER = $(RELIC_STRICT) $(SHELL)

AM_TESTS_ENVIRONMENT = \
	RELIC_MAKE='$(MAKE)'; export RELIC_MAKE; \
	RELIC_CC='$(CC)'; export RELIC_CC; \
	RELIC_CXX='$(CXX)'; export RELIC_CXX; \
	RELIC_AR='$(AR)'; export RELIC_AR; \
	RELIC_RANLIB='$(RANLIB)'; export RELIC_RANLIB; \
	RELIC_CXXFLAGS='$(CXXFLAGS)'; export RELIC_CXXFLAGS; \
	RELIC_CPPFLAGS='$(CPPFLAGS)'; export RELIC_CPPFLAGS; \
	RELIC_LDFLAGS='$(LDFLAGS)'; export RELIC_LDFLAGS; \
	RELIC_LIBS='$(LIBS)'; export RELIC_LIBS; \
	RELIC_CFLAGS='$(CFLAGS)'; export RELIC_CFLAGS; \
	RELIC_NM='$(NM)'; export RELIC_NM; \
	RELIC_LTO_CFLAGS='$(RELIC_LTO_CFLAGS)'; export RELIC_LTO_CFLAGS; \
	RELIC='$(abs_top_builddir)/relic$(EXEEXT)'; export RELIC; \
	RELIC_BUILDDIR='$(abs_top_builddir)'; export RELIC_BUILDDIR; \
	RELIC_SRCDIR='$(abs_top_srcdir)'; export RELIC_SRCDIR; \
	RELIC_VERSION='$(PACKAGE_VERSION)'; export RELIC_VERSION; \
	RELIC_LANG_TESTS='$(RELIC_LANG_TESTS)'; export RELIC_LANG_TESTS;

# A test program runs as a test, with no arguments.  A replay driver, an
# example and the terminal driver are built for make check too, but are run
# by the script that tests them.
check_PROGRAMS = $(RELIC_TEST_BINARIES) $(RELIC_REPLAY_BINARIES) \
	$(RELIC_EXAMPLE_BINARIES) $(RELIC_DRIVER_BINARIES) tests/bench-pairset
tests_bench_pairset_SOURCES = bench/compare/pairset.c
tests_bench_pairset_CFLAGS = $(AM_CFLAGS) -pthread
TESTS = $(RELIC_TEST_BINARIES) $(dist_check_SCRIPTS) $(RELIC_LANG_TESTS)

##
## Sanitizer suites
##
## Each suite configures a disposable build tree under disposable/ in the
## current one, from a copy of the distributed sources.  The sanitizers need compiler and link flags that the working tree does not
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
## Of the suites here, every one but UndefinedBehaviorSanitizer replaces the
## allocator, and by default the replacement aborts the process on a request
## it will not satisfy, such as reallocarray(3) of an overflowing product or
## one above its own maximum size, where the C library would return NULL.
## relic reports a failed allocation as an error, and the tests hold it to
## that, so allocator_may_return_null=1 makes the replacement return NULL
## too, for the suite to test what relic does with it.
##
## On macOS the sanitizer's reservations leave the system allocator no room
## for its nano zone, and the allocator says so on standard error, which a
## test reading the output of a terminal sees among the answers.
## MallocNanoZone=0 turns the zone off, so there is nothing to say; other
## systems ignore the variable.
##

RELIC_ASAN_OPTIONS = halt_on_error=1:detect_leaks=$(RELIC_ASAN_DETECT_LEAKS):detect_stack_use_after_return=1:check_initialization_order=1:strict_string_checks=1:detect_invalid_pointer_pairs=2:abort_on_error=1:allocator_may_return_null=1
RELIC_LSAN_OPTIONS = exitcode=1:allocator_may_return_null=1
RELIC_MSAN_OPTIONS = halt_on_error=1:abort_on_error=1:allocator_may_return_null=1
RELIC_UBSAN_OPTIONS = halt_on_error=1:print_stacktrace=1
RELIC_TSAN_OPTIONS = halt_on_error=1:abort_on_error=1:second_deadlock_stack=1:allocator_may_return_null=1

# tests/perf.sh sizes its CPU time budget for AddressSanitizer on a slow
# machine.
# Unless a budget is set, check-msan and check-tsan give it more: three
# times the default for MemorySanitizer, which runs a program about twice as
# long as AddressSanitizer does, and six times for ThreadSanitizer, which
# runs it about six times as long.
RELIC_MSAN_PERF_BUDGET = 1800
RELIC_TSAN_PERF_BUDGET = 3600

# Create the disposable tree disposable/$$tree of this build directory
# afresh, removing any earlier one, and change to its build directory:
#
#	disposable/$$tree/source	a copy of the distributed sources,
#					RELIC_DISPOSABLE_SOURCE
#	disposable/$$tree/build		empty; the caller configures here by
#					running the copy's configure
#
# Both macros read the recipe's shell variable tree, a literal every target
# sets.  Every disposable tree lies under the one directory disposable, so
# that ignoring, cleaning and skipping them all takes its name alone.  The
# value must be a plain name, without a slash and neither . nor .., so that
# it names nothing outside disposable; an empty value stops the recipe rather
# than naming disposable itself.
#
# The tree is built from a copy rather than from the source directory
# itself, because configure refuses to build out of a source directory that
# has been configured in place, which is how many developers build.  The
# copy is the distribution's, as make distcheck's is, so a file a test
# needs but the tarball lacks fails here too.  It keeps the permissions of
# the files it copies rather than opening them to every user, as a tarball's
# are.  The copy and the build directory are siblings rather than one inside
# the other, so that the source paths recorded in the objects, which gcov
# reads back, are the ones the file prefix map gives.
RELIC_DISPOSABLE_TREE = $(abs_top_builddir)/disposable/$${tree:?}
RELIC_DISPOSABLE_SOURCE = $(RELIC_DISPOSABLE_TREE)/source
RELIC_ENTER_DISPOSABLE_TREE = \
	rm -rf "$(RELIC_DISPOSABLE_TREE)" && \
	$(MKDIR_P) "$(RELIC_DISPOSABLE_TREE)/build" && \
	$(MAKE) $(AM_MAKEFLAGS) distdir am__skip_mode_fix=yes \
	    distdir="$(RELIC_DISPOSABLE_SOURCE)" && \
	cd "$(RELIC_DISPOSABLE_TREE)/build"

# Configure, build and test in the tree disposable/$$tree using the configure
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
# Under RELIC_STRICT_CHECK=yes it fails, unless RELIC_ALLOWED_SKIPS names the
# target: a machine set up to run every check must not report one that ran
# nothing as passed.
#
# The tree is configured afresh with nothing carried over but the compilers,
# so that a sanitizer run means the same thing whatever options the
# surrounding build was configured with.
RELIC_SANITIZER_CHECK = \
	{ test -n "$$tree" && test -n "$$flags" && test -n "$$name" && \
	  test -n "$$supported"; } || \
	    { echo "sanitizer target is incompletely defined" >&2; exit 1; }; \
	if test "x$$supported" != xyes; then \
		check='$@'; \
		reason="$$name is not available with $(CC); skipping."; \
		$(RELIC_UNAVAILABLE_CHECK); \
	fi; \
	if test -n "$$note"; then echo "note: $$note"; fi; \
	$(RELIC_ENTER_DISPOSABLE_TREE) && \
	"$(RELIC_DISPOSABLE_SOURCE)/configure" $$flags CC="$(CC)" \
	    CXX="$(CXX)" && \
	$(MAKE) $(AM_MAKEFLAGS) && \
	$(MAKE) $(AM_MAKEFLAGS) check

check-asan:
	@tree=asan; flags=--enable-asan; name=AddressSanitizer; \
	supported='$(RELIC_ASAN_SUPPORTED)'; note=; \
	test x'$(RELIC_ASAN_DETECT_LEAKS)' = x1 || \
	    note='this platform cannot detect leaks; memory errors only'; \
	ASAN_OPTIONS='$(RELIC_ASAN_OPTIONS)'; export ASAN_OPTIONS; \
	MallocNanoZone=0; export MallocNanoZone; \
	$(RELIC_SANITIZER_CHECK)

check-lsan:
	@tree=lsan; flags=--enable-lsan; name=LeakSanitizer; \
	supported='$(RELIC_LSAN_SUPPORTED)'; note=; \
	LSAN_OPTIONS='$(RELIC_LSAN_OPTIONS)'; export LSAN_OPTIONS; \
	$(RELIC_SANITIZER_CHECK)

check-msan:
	@tree=msan; flags=--enable-msan; name=MemorySanitizer; \
	supported='$(RELIC_MSAN_SUPPORTED)'; note=; \
	MSAN_OPTIONS='$(RELIC_MSAN_OPTIONS)'; export MSAN_OPTIONS; \
	RELIC_PERF_BUDGET=$${RELIC_PERF_BUDGET:-$(RELIC_MSAN_PERF_BUDGET)}; \
	export RELIC_PERF_BUDGET; \
	$(RELIC_SANITIZER_CHECK)

check-ubsan:
	@tree=ubsan; flags=--enable-ubsan; \
	name=UndefinedBehaviorSanitizer; \
	supported='$(RELIC_UBSAN_SUPPORTED)'; note=; \
	UBSAN_OPTIONS='$(RELIC_UBSAN_OPTIONS)'; export UBSAN_OPTIONS; \
	$(RELIC_SANITIZER_CHECK)

check-tsan:
	@tree=tsan; flags='--enable-tsan --enable-threads'; \
	name=ThreadSanitizer; \
	supported='$(RELIC_TSAN_SUPPORTED)'; note=; \
	TSAN_OPTIONS='$(RELIC_TSAN_OPTIONS)'; export TSAN_OPTIONS; \
	MallocNanoZone=0; export MallocNanoZone; \
	RELIC_PERF_BUDGET=$${RELIC_PERF_BUDGET:-$(RELIC_TSAN_PERF_BUDGET)}; \
	export RELIC_PERF_BUDGET; \
	$(RELIC_SANITIZER_CHECK)

##
## Coverage
##
## make coverage configures a disposable tree inside this one, as the
## sanitizer suites do, builds it with gcov instrumentation, runs the suite
## and writes the report to disposable/coverage/build/coverage.txt as well
## as to the terminal.
## The tree is built without shared libraries, so that every object and its
## counts sit beside its source's directory rather than in libtool's hidden
## one.  A failing suite stops the target: the coverage of a suite that
## fails describes nothing worth reading.
##

coverage:
	@tree=coverage; \
	$(RELIC_ENTER_DISPOSABLE_TREE) && \
	"$(RELIC_DISPOSABLE_SOURCE)/configure" --enable-coverage \
	    --disable-shared CC="$(CC)" CXX="$(CXX)" && \
	$(MAKE) $(AM_MAKEFLAGS) && \
	$(MAKE) $(AM_MAKEFLAGS) check && \
	$(SHELL) "$(RELIC_DISPOSABLE_SOURCE)/tests/coverage-report.sh" . \
	    "$(RELIC_DISPOSABLE_SOURCE)" > coverage.txt && \
	cat coverage.txt

# Two build products automake does not generate a rule for.
#
# The disposable trees of the sanitizer and coverage suites belong to this
# build directory rather than to the source directory, so they are removed
# relative to it, all at once by their common parent.
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
	-rm -f "$(distdir)"/tests/lang/*.log "$(distdir)"/tests/lang/*.trs
	-rm -rf "$(distdir)/website/assets/wasm"

clean-local:
	-cd $(top_builddir) && rm -rf disposable
	-rm -rf $(LIBOBJDIR).libs $(LIBOBJDIR)_libs

.PHONY: check-asan check-lsan check-msan check-random check-tsan check-ubsan \
	coverage \
	dist-hook fuzz
