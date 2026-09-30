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
librelic_internal_la_SOURCES = lib/account.c lib/api.c lib/arena.c lib/arith.c \
	lib/depgraph.c lib/diag.c lib/eval.c lib/exec.c lib/explain.c \
	lib/expr.c lib/facts.c lib/import.c lib/index.c lib/lex.c \
	lib/literal.c lib/magic.c lib/merge.c lib/parse.c lib/plan.c \
	lib/pool.c lib/pred.c lib/relation.c lib/result.c lib/round.c \
	lib/rule.c lib/stratify.c lib/strfn.c lib/symbol.c lib/task.c \
	lib/util.c lib/value.c

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
	lib/depgraph.h \
	lib/exec.h \
	lib/frozen.h lib/internal.h lib/plan.h lib/program.h lib/relation.h \
	lib/round.h lib/strfn.h lib/util.h tests/unit/harness.h \
	tests/unit/programs.h

bin_PROGRAMS = relic
relic_SOURCES = cmd/relic/import.c cmd/relic/input.c cmd/relic/main.c \
	cmd/relic/output.c cmd/relic/repl.c

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
EXTRA_DIST = autogen.sh LICENSE README.md bench/baseline.txt \
	bench/datasets.sh bench/run.sh \
	compat/README.md examples/embed.c tests/coverage-report.sh \
	tests/fuzz/corpus tests/fuzz/corpus-import \
	tests/fuzz/relic.dict tests/lang tests/langtest.sh

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
## quietly; pkg-config is not required to build or install relic.  The
## example is built in a directory of its own and removed with it, together
## with whatever else the compiler leaves beside an executable.
##
installcheck-local:
	@if ! command -v pkg-config >/dev/null 2>&1; then \
		echo "SKIP: examples/embed.c: pkg-config is not installed," \
		    "so the installed library was not checked"; \
		exit 0; \
	fi; \
	PKG_CONFIG_PATH='$(libdir)/pkgconfig'; export PKG_CONFIG_PATH; \
	work=$$(mktemp -d) || exit 1; \
	echo "building examples/embed.c against the installed librelic"; \
	$(CC) $(RELIC_STRICT_CFLAGS) $(RELIC_WARN_CFLAGS) \
	    $(RELIC_WERROR_CFLAGS) $(RELIC_SAN_CFLAGS) $(CPPFLAGS) $(CFLAGS) \
	    $$(pkg-config --cflags librelic) \
	    -o "$$work/embed$(EXEEXT)" $(srcdir)/examples/embed.c \
	    $(LDFLAGS) $$(pkg-config --libs librelic); status=$$?; \
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
## the sanitizers on the machine it was written on.  Fortification is turned
## off for that build because a C library may define these interfaces as
## function-like macros, which take precedence over the renaming and leave
## the replacement's own definition colliding with a compiler builtin.
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
	tests/unit/harness.c compat/getentropy.c compat/memmem.c \
	compat/reallocarray.c compat/strlcat.c compat/strlcpy.c \
	compat/strtonum.c
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

RELIC_TEST_BINARIES += tests/unit/test_api tests/unit/test_arena \
	tests/unit/test_arith tests/unit/test_compute tests/unit/test_diag \
	tests/unit/test_eval tests/unit/test_exec \
	tests/unit/test_facts tests/unit/test_import tests/unit/test_index \
	tests/unit/test_lex tests/unit/test_magic tests/unit/test_parse \
	tests/unit/test_plan tests/unit/test_pred tests/unit/test_query \
	tests/unit/test_relation tests/unit/test_round tests/unit/test_rule \
	tests/unit/test_stratify tests/unit/test_strfn tests/unit/test_symbol \
	tests/unit/test_text tests/unit/test_util tests/unit/test_value

## test_api links the installed form of the library rather than the
## convenience library, so an entry point it calls that the export list or
## hidden visibility left out fails to link.
tests_unit_test_api_SOURCES = tests/unit/test_api.c tests/unit/harness.c
tests_unit_test_api_LDADD = librelic.la

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

tests_unit_test_diag_SOURCES = tests/unit/test_diag.c tests/unit/harness.c
tests_unit_test_diag_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_eval_SOURCES = tests/unit/test_eval.c tests/unit/harness.c
tests_unit_test_eval_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_exec_SOURCES = tests/unit/test_exec.c tests/unit/harness.c
tests_unit_test_exec_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_facts_SOURCES = tests/unit/test_facts.c tests/unit/harness.c \
	tests/unit/programs.c
tests_unit_test_facts_LDADD = librelic_internal.la $(LTLIBOBJS)

tests_unit_test_import_SOURCES = tests/unit/test_import.c tests/unit/harness.c
tests_unit_test_import_LDADD = librelic_internal.la $(LTLIBOBJS)

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

tests_unit_test_round_SOURCES = tests/unit/test_round.c tests/unit/harness.c
tests_unit_test_round_LDADD = librelic_internal.la $(LTLIBOBJS)

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
## fails on demand can show that it is not.
##
## The renaming follows the pattern the compat suite already uses.  It is
## confined to this program; nothing that ships is built this way.  The
## threshold from which a growable array is mapped is lowered with it, so
## that the small programs the suite sweeps take the mapped paths as well.
## tests/allocators.sh checks the objects this program links, so that an
## allocator the renaming misses fails the build's checks rather than
## quietly narrowing what the suite refuses.
##

RELIC_EXHAUSTION_RENAMES = \
	-Dcalloc=relic_test_calloc \
	-Dgetentropy=relic_test_getentropy \
	-Dmalloc=relic_test_malloc \
	-Dmmap=relic_test_mmap \
	-Drealloc=relic_test_realloc \
	-Dreallocarray=relic_test_reallocarray \
	-DRELIC_ARRAY_MAP_BYTES=4096

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

dist_check_SCRIPTS = tests/allocators.sh tests/ascii.sh tests/cli.sh \
	tests/globals.sh tests/langfiles.sh tests/magic.sh tests/man.sh \
	tests/output.sh tests/perf.sh tests/pkgconfig.sh tests/reentry.sh \
	tests/repl.sh tests/roundtrip.sh tests/symbols.sh tests/version.sh

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

check-random: tests/random/random$(EXEEXT)
	$(builddir)/tests/random/random -n $(RELIC_SEEDS) \
	    -s $(RELIC_SEED_START)

##
## Fuzz targets
##
## Each target is an LLVMFuzzerTestOneInput() over the public interface, and
## is linked twice.  The replay driver, built always, passes every file of a
## corpus to the target once, so make check replays the corpora on every
## compiler and under every sanitizer build.  With --enable-fuzzers the
## target is also linked with libFuzzer, and make fuzz runs each for
## FUZZ_SECONDS over a working corpus in the build tree seeded from the
## language tests and tests/fuzz/corpus, or for the import target, whose
## inputs are tables, from tests/fuzz/corpus-import; the source tree's
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
	tests/fuzz/replay_load
tests_fuzz_replay_eval_SOURCES = tests/fuzz/replay.c $(RELIC_FUZZ_EVAL_SOURCES)
tests_fuzz_replay_eval_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_replay_import_SOURCES = tests/fuzz/replay.c \
	tests/fuzz/fuzz_import.c $(RELIC_FUZZ_COMMON)
tests_fuzz_replay_import_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_replay_load_SOURCES = tests/fuzz/replay.c tests/fuzz/fuzz_load.c \
	$(RELIC_FUZZ_COMMON)
tests_fuzz_replay_load_LDADD = librelic.la $(LTLIBOBJS)

dist_check_SCRIPTS += tests/fuzz/replay.sh

FUZZ_SECONDS = 60

if RELIC_FUZZERS
noinst_PROGRAMS = tests/fuzz/fuzz_eval tests/fuzz/fuzz_import \
	tests/fuzz/fuzz_load
tests_fuzz_fuzz_eval_SOURCES = $(RELIC_FUZZ_EVAL_SOURCES)
tests_fuzz_fuzz_eval_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_eval_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_fuzz_import_SOURCES = tests/fuzz/fuzz_import.c $(RELIC_FUZZ_COMMON)
tests_fuzz_fuzz_import_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_import_LDADD = librelic.la $(LTLIBOBJS)
tests_fuzz_fuzz_load_SOURCES = tests/fuzz/fuzz_load.c $(RELIC_FUZZ_COMMON)
tests_fuzz_fuzz_load_CFLAGS = $(AM_CFLAGS) -fsanitize=fuzzer
tests_fuzz_fuzz_load_LDADD = librelic.la $(LTLIBOBJS)

# -max_len bounds an input's text, and with it how long a program can take
# to load and check; -timeout reports an input that runs longer anyway.  An
# evaluation can stay inside every limit and still run long -- a cross
# product whose tuples are all duplicates -- so a timeout from the eval
# target needs reading before it is called a bug.  The import target reads
# tables rather than programs, so it takes neither the language's dictionary
# nor its seeds.
fuzz: $(noinst_PROGRAMS)
	@for target in load eval; do \
		corpus=fuzz-corpus/$$target; \
		artifacts=fuzz-artifacts/$$target/; \
		$(MKDIR_P) $$corpus $$artifacts || exit 1; \
		echo "fuzzing $$target for $(FUZZ_SECONDS) seconds"; \
		./tests/fuzz/fuzz_$$target -max_total_time=$(FUZZ_SECONDS) \
		    -max_len=4096 -timeout=10 \
		    -dict=$(srcdir)/tests/fuzz/relic.dict \
		    -artifact_prefix=$$artifacts $$corpus \
		    $(srcdir)/tests/lang $(srcdir)/tests/fuzz/corpus || exit 1; \
	done
	@$(MKDIR_P) fuzz-corpus/import fuzz-artifacts/import/
	@echo "fuzzing import for $(FUZZ_SECONDS) seconds"
	@./tests/fuzz/fuzz_import -max_total_time=$(FUZZ_SECONDS) \
	    -max_len=4096 -timeout=10 \
	    -artifact_prefix=fuzz-artifacts/import/ fuzz-corpus/import \
	    $(srcdir)/tests/fuzz/corpus-import
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

RELIC_LANG_TESTS = \
	tests/lang/aggregate-assign.dl tests/lang/aggregate-count.dl \
	tests/lang/aggregate-explain.dl tests/lang/aggregate-limit.dl \
	tests/lang/aggregate-magic.dl tests/lang/aggregate-minmax.dl \
	tests/lang/aggregate-mixed.dl tests/lang/aggregate-nested.dl \
	tests/lang/aggregate-overflow.dl tests/lang/aggregate-parallel.dl \
	tests/lang/aggregate-project.dl tests/lang/aggregate-query.dl \
	tests/lang/aggregate-shared.dl tests/lang/aggregate-shortest.dl \
	tests/lang/aggregate-stratify.dl tests/lang/aggregate-sum.dl \
	tests/lang/aggregate-syntax.dl tests/lang/aggregate-unsafe.dl \
	tests/lang/arithmetic-binding.dl \
	tests/lang/arithmetic-depth.dl tests/lang/arithmetic-explain.dl \
	tests/lang/arithmetic-gathered.dl tests/lang/arithmetic-height.dl \
	tests/lang/arithmetic-magic.dl tests/lang/arithmetic-magic-drafts.dl \
	tests/lang/arithmetic-magic-plan.dl \
	tests/lang/arithmetic-magic-wide.dl tests/lang/arithmetic-memory.dl \
	tests/lang/arithmetic-mixed.dl tests/lang/arithmetic-negation.dl \
	tests/lang/arithmetic-negative.dl tests/lang/arithmetic-order.dl \
	tests/lang/arithmetic-overflow.dl tests/lang/arithmetic-probe.dl \
	tests/lang/arithmetic-recursion.dl \
	tests/lang/arithmetic-strategy.dl \
	tests/lang/arithmetic-strategy-full.dl \
	tests/lang/arithmetic-syntax.dl tests/lang/arithmetic-unsafe.dl \
	tests/lang/arithmetic-zero.dl tests/lang/arithmetic-zero-query.dl \
	tests/lang/arity-32.dl tests/lang/arity-limit.dl \
	tests/lang/bad-backslash.dl \
	tests/lang/bad-byte.dl tests/lang/bad-escape-at-end.dl \
	tests/lang/bad-escape.dl tests/lang/check-queries.dl \
	tests/lang/closure-double.dl tests/lang/closure-graphs.dl \
	tests/lang/comments-only.dl tests/lang/components.dl \
	tests/lang/constructs.dl tests/lang/empty.dl \
	tests/lang/error-after-recovery.dl tests/lang/error-first-token.dl \
	tests/lang/explain-check.dl tests/lang/explain-facts.dl \
	tests/lang/explain-free.dl \
	tests/lang/explain-graph.dl tests/lang/explain-long.dl \
	tests/lang/explain-magic.dl tests/lang/explain-materialized.dl \
	tests/lang/explain-ops.dl tests/lang/explain-subsumed.dl \
	tests/lang/explain-supplementary.dl \
	tests/lang/explain-thresholds.dl tests/lang/explain-views.dl \
	tests/lang/format-csv.dl tests/lang/format-integers.dl \
	tests/lang/format-json.dl tests/lang/format-nul.dl \
	tests/lang/format-text.dl tests/lang/format-tsv.dl \
	tests/lang/head-not-atom.dl tests/lang/import-check.dl \
	tests/lang/import-csv.dl tests/lang/import-errors-csv.dl \
	tests/lang/import-errors-tsv.dl \
	tests/lang/import-errors-typed.dl tests/lang/import-order.dl \
	tests/lang/import-stdin.dl tests/lang/import-tsv.dl \
	tests/lang/import-usage-arity.dl tests/lang/import-usage-column.dl \
	tests/lang/import-usage-equals.dl tests/lang/import-usage-format.dl \
	tests/lang/import-usage-not.dl tests/lang/import-usage-option.dl \
	tests/lang/import-usage-parts.dl tests/lang/import-usage-path.dl \
	tests/lang/import-usage-predicate.dl tests/lang/import-wide.dl \
	tests/lang/incomplete-operator.dl tests/lang/integer-range.dl \
	tests/lang/integer-trailing.dl tests/lang/magic-bindings.dl \
	tests/lang/magic-budget-adornments.dl \
	tests/lang/magic-budget-literals.dl \
	tests/lang/magic-budget-subsumed.dl \
	tests/lang/magic-budget-wide-cut.dl tests/lang/magic-budget.dl \
	tests/lang/magic-cspa.dl \
	tests/lang/magic-duplicate-rules.dl tests/lang/magic-limit-preds.dl \
	tests/lang/magic-limit-rules.dl \
	tests/lang/magic-limit-supplementary.dl tests/lang/magic-mixed.dl \
	tests/lang/magic-mutual.dl tests/lang/magic-negation.dl \
	tests/lang/magic-rules-read.dl \
	tests/lang/magic-same-generation.dl tests/lang/magic-subsumed.dl \
	tests/lang/magic-supplementary-wide.dl \
	tests/lang/magic-supplementary.dl tests/lang/materialize.dl \
	tests/lang/missing-terminator.dl tests/lang/negation-forms.dl \
	tests/lang/negation-recursive.dl tests/lang/negation-strata.dl \
	tests/lang/negation-tasks.dl tests/lang/not-as-predicate.dl \
	tests/lang/not-symbol.dl tests/lang/predicate-identity.dl \
	tests/lang/query-bindings.dl tests/lang/query-columns-limit.dl \
	tests/lang/query-columns.dl tests/lang/query-comparisons.dl \
	tests/lang/query-duplicates.dl tests/lang/query-graph.dl \
	tests/lang/query-none.dl tests/lang/query-not-a-query.dl \
	tests/lang/query-option-unsafe.dl tests/lang/query-option.dl \
	tests/lang/query-recursion.dl tests/lang/query-undefined.dl \
	tests/lang/query-unsafe.dl tests/lang/query-zero-arity.dl \
	tests/lang/recovery.dl tests/lang/repl-check.dl \
	tests/lang/repl-discard.dl tests/lang/repl-errors.dl \
	tests/lang/repl-json.dl tests/lang/repl-limit.dl \
	tests/lang/repl-plan-materialized.dl tests/lang/repl-plan.dl \
	tests/lang/repl-session.dl tests/lang/repl-verbose.dl \
	tests/lang/safe-rules.dl tests/lang/singleton.dl \
	tests/lang/sort-mixed.dl tests/lang/stats-indexed.dl \
	tests/lang/stats-magic.dl tests/lang/stats-naive.dl \
	tests/lang/stats-semi-naive.dl tests/lang/string-escapes.dl \
	tests/lang/strings-aggregate.dl tests/lang/strings-atom-or-call.dl \
	tests/lang/strings-binding.dl tests/lang/strings-bytes.dl \
	tests/lang/strings-explain.dl tests/lang/strings-gathered.dl \
	tests/lang/strings-index.dl tests/lang/strings-magic.dl \
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

# A test program runs as a test, with no arguments.  A replay driver is
# built for make check too, but needs its corpus named, which the script that
# runs it does.
check_PROGRAMS = $(RELIC_TEST_BINARIES) $(RELIC_REPLAY_BINARIES)
TESTS = $(RELIC_TEST_BINARIES) $(dist_check_SCRIPTS) $(RELIC_LANG_TESTS)

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
RELIC_TSAN_OPTIONS = halt_on_error=1:abort_on_error=1:second_deadlock_stack=1

# ThreadSanitizer runs a program about six times slower than the other
# sanitizers do, and tests/perf.sh sizes its clock for those, so its
# check-tsan budget is six times the default unless one is set.
RELIC_TSAN_PERF_BUDGET = 3600

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

check-tsan:
	@tree=build-tsan; flags='--enable-tsan --enable-threads'; \
	name=ThreadSanitizer; \
	supported='$(RELIC_TSAN_SUPPORTED)'; note=; \
	TSAN_OPTIONS='$(RELIC_TSAN_OPTIONS)'; export TSAN_OPTIONS; \
	RELIC_PERF_BUDGET=$${RELIC_PERF_BUDGET:-$(RELIC_TSAN_PERF_BUDGET)}; \
	export RELIC_PERF_BUDGET; \
	$(RELIC_SANITIZER_CHECK)

##
## Coverage
##
## make coverage configures a disposable tree beside this one, as the
## sanitizer suites do, builds it with gcov instrumentation, runs the suite
## and writes the report to coverage.txt there as well as to the terminal.
## The tree is built without shared libraries, so that every object and its
## counts sit beside its source's directory rather than in libtool's hidden
## one.  A failing suite stops the target: the coverage of a suite that
## fails describes nothing worth reading.
##

coverage:
	@tree=build-coverage; \
	rm -rf $(abs_top_builddir)/$$tree && \
	$(MKDIR_P) $(abs_top_builddir)/$$tree && \
	cd $(abs_top_builddir)/$$tree && \
	$(abs_top_srcdir)/configure --enable-coverage --disable-shared \
	    CC="$(CC)" && \
	$(MAKE) $(AM_MAKEFLAGS) && \
	$(MAKE) $(AM_MAKEFLAGS) check && \
	$(SHELL) $(abs_top_srcdir)/tests/coverage-report.sh . \
	    $(abs_top_srcdir) > coverage.txt && \
	cat coverage.txt

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
	-cd $(top_builddir) && rm -rf build-*san build-coverage
	-rm -rf $(LIBOBJDIR).libs $(LIBOBJDIR)_libs

.PHONY: check-asan check-lsan check-msan check-random check-tsan check-ubsan \
	coverage \
	dist-hook fuzz
