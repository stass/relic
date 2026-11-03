/*
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Ordinary publication and canonical counted destinations share one protocol. */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef RELIC_THREADS
#include <pthread.h>
#endif

#include "harness.h"
#include "internal.h"

static void test_pool_for(relic_pool_t *, size_t, relic_pool_part_fn, void *);
static bool test_round_merge(relic_t *, relic_pool_t *, relic_round_t *, size_t,
    relic_dense_t *);

/* Keep test coordination and shared-refusal hooks out of runtime interfaces. */
#define relic_pool_for test_pool_for
#define relic_round_merge test_round_merge
#include "../../lib/round.c"
#undef relic_pool_for
#undef relic_round_merge

typedef struct observed observed_t;

struct observed {
	relic_t			*ctx;
	relic_dom_t		*ids;
	relic_value_view_t	*values;
	size_t			 count;
	size_t			 capacity;
	size_t			 value_capacity;
	size_t			 arity;
	size_t			 counted;
	size_t			 declined;
	size_t			 notes;
	size_t			 errors;
	char			 diagnostics[4096];
	bool			 interrupt_trace;
	bool			 interrupt_delivery;
	bool			 interrupt_error;
	bool			 interrupt_note;
};

static bool require_other_worker;
static bool force_shared_decline;
static size_t forced_declines;
static bool inspect_retained_keys;

#ifdef RELIC_THREADS
typedef struct shared_gate shared_gate_t;

struct shared_gate {
	relic_pool_part_fn	 run;
	void			*argument;
	pthread_mutex_t		 mutex;
	pthread_cond_t		 changed;
	bool			 entered;
};
static void
gated_part(void *argument, size_t part, size_t worker)
{
	shared_gate_t *gate = argument;

	pthread_mutex_lock(&gate->mutex);
	if (worker == 0) {
		while (!gate->entered)
			pthread_cond_wait(&gate->changed, &gate->mutex);
	} else {
		gate->entered = true;
		pthread_cond_signal(&gate->changed);
	}
	pthread_mutex_unlock(&gate->mutex);
	gate->run(gate->argument, part, worker);
}

#endif

static void
test_pool_for(relic_pool_t *pool, size_t parts, relic_pool_part_fn run,
    void *argument)
{
#ifdef RELIC_THREADS
	shared_gate_t gate = {
		.run = run,
		.argument = argument,
		.mutex = PTHREAD_MUTEX_INITIALIZER,
		.changed = PTHREAD_COND_INITIALIZER
	};

	if (require_other_worker && relic_pool_workers(pool) > 1 && parts > 1) {
		relic_pool_for(pool, parts, gated_part, &gate);
		pthread_cond_destroy(&gate.changed);
		pthread_mutex_destroy(&gate.mutex);
		return;
	}
#endif
	relic_pool_for(pool, parts, run, argument);
}


static bool
test_round_merge(relic_t *ctx, relic_pool_t *pool, relic_round_t *round,
    size_t bytes, relic_dense_t *dense)
{
	if (inspect_retained_keys) {
		size_t worker, candidate, retained, key_bound, key_width;
		uint64_t contributions;
		bool repeated;

		key_width = relic_destination_arity(round->targets[0]);
		key_bound = key_width == 0 ? 1 : 17;
		retained = 0;
		contributions = 0;
		repeated = false;
		for (worker = 0; worker < pool->worker_count; worker++) {
			const relic_candidates_t *keys = &pool->workers[worker].candidates;

			check(keys->count <= key_bound && keys->word_count == keys->count * key_width &&
			    keys->value_count <= key_bound * key_width,
			    "shared counted work", "each worker retains keys and distinct pending values");
			retained += keys->count;
			for (candidate = 0; candidate < keys->count; candidate++) {
				contributions += keys->entries[candidate].contribution;
				if (keys->entries[candidate].contribution > 1)
					repeated = true;
			}
		}
		check(retained <= key_bound * pool->worker_count && contributions == 12000 && repeated,
		    "shared counted work", "retained key contributions cover every producer emission");
	}
	if (force_shared_decline) {
		force_shared_decline = false;
		forced_declines++;
		check(round->target_count == 1 &&
		    relic_destination_view(round->targets[0])->kind == RELIC_DESTINATION_COUNT &&
		    relic_relation_count(relic_destination_relation(round->targets[0])) == 0,
		    "shared refusal", "keys and counts remain unpublished before reservation");
		return (false);
	}
	return (relic_round_merge(ctx, pool, round, bytes, dense));
}
static void
diagnostic(void *argument, enum relic_diag_level level,
    const relic_location_t *where, const char *message)
{
	observed_t *observed = argument;
	size_t used;

	used = strlen(observed->diagnostics);
	(void)snprintf(observed->diagnostics + used,
	    sizeof(observed->diagnostics) - used, "%d:%u:%u:%s\n", level,
	    where == NULL ? 0 : where->line, where == NULL ? 0 : where->column, message);
	if (level == RELIC_ERROR) {
		observed->errors++;
		if (observed->interrupt_error)
			relic_interrupt(observed->ctx);
	}
	if (level == RELIC_NOTE) {
		observed->notes++;
		if (observed->interrupt_note)
			relic_interrupt(observed->ctx);
	}
}


static void
trace(void *argument, const char *line)
{
	observed_t *observed = argument;

	if (strstr(line, "counted producer ") != NULL &&
	    strstr(line, "declined") == NULL) {
		observed->counted++;
		if (observed->interrupt_trace)
			relic_interrupt(observed->ctx);
	}
	if (strstr(line, "counted producer declined") != NULL)
		observed->declined++;
}


static int
keep_row(void *argument, const relic_value_t *row, size_t arity)
{
	observed_t *observed = argument;
	size_t i, end;
	relic_dom_t id;

	observed->arity = arity;
	end = observed->count + arity;
	observed->ids = relic_grow_array(observed->ids, &observed->capacity,
	    end, sizeof(*observed->ids));
	observed->values = relic_grow_array(observed->values,
	    &observed->value_capacity, end, sizeof(*observed->values));
	if (observed->ids == NULL || observed->values == NULL)
		abort();
	for (i = 0; i < arity; i++) {
		if (row[i].type == RELIC_INTEGER)
			id = relic_value_find_integer(observed->ctx, row[i].v.integer);
		else
			id = row[i].v.id | (row[i].type == RELIC_STRING ?
			    RELIC_DOM_STRING : RELIC_DOM_SYMBOL);
		observed->ids[observed->count] = id;
		relic_value_view(observed->ctx, id,
		    &observed->values[observed->count]);
		observed->count++;
	}
	if (observed->interrupt_delivery)
		relic_interrupt(observed->ctx);
	return (0);
}


static bool
open_context(observed_t *observed, const char *program, unsigned int flags,
    size_t workers)
{
	relic_config_t config;

	bzero(observed, sizeof(*observed));
	bzero(&config, sizeof(config));
	config.flags = flags;
	config.threads = workers;
	observed->ctx = relic_create_config(&config, sizeof(config));
	if (observed->ctx == NULL)
		return (false);
	relic_diag_set(observed->ctx, diagnostic, observed);
	relic_explain_set(observed->ctx, trace, observed);
	return (relic_load_string(observed->ctx, "grouped.dl", program) == 0);
}


static void
close_context(observed_t *observed)
{
	free(observed->ids);
	free(observed->values);
	relic_destroy(observed->ctx);
}


static int
answer_query(observed_t *observed, const char *text)
{
	relic_query_t *query;
	int status;

	observed->count = 0;
	query = relic_query_prepare(observed->ctx, NULL, text);
	if (query == NULL)
		return (-1);
	status = relic_query_run(observed->ctx, query, keep_row, observed);
	check(relic_relation_count(query->answer) == 0,
	    text, "the transient answer is empty after delivery or failure");
	relic_query_destroy(query);
	return (status);
}


static bool
same_answer(const observed_t *left, const observed_t *right)
{
	size_t i;

	if (left->count != right->count || left->arity != right->arity)
		return (false);
	for (i = 0; i < left->count; i++)
		if (left->ids[i] != right->ids[i] ||
		    relic_value_view_order(&left->values[i], &right->values[i]) !=
		    RELIC_ORDER_EQUAL)
			return (false);
	return (true);
}


static bool
same_counted_work(const relic_stats_t *left, const relic_stats_t *right)
{
	return (left->iterations == right->iterations &&
	    left->derivations == right->derivations &&
	    left->tuples_inserted == right->tuples_inserted &&
	    left->tuples_scanned == right->tuples_scanned &&
	    left->tuples_looked_up == right->tuples_looked_up &&
	    left->tuples == right->tuples &&
	    left->storage_bytes == right->storage_bytes &&
	    left->integer_values == right->integer_values &&
	    left->string_values == right->string_values &&
	    left->symbol_values == right->symbol_values);
}


static void
compare(const char *scenario, const char *program, const char *query,
    bool admitted)
{
	observed_t reference, actual;
	relic_stats_t serial, parallel;
	size_t workers;

	if (!open_context(&reference, program, RELIC_F_NO_MAGIC, 1)) {
		check(false, scenario, "the reference loads");
		return;
	}
	check(answer_query(&reference, query) == 0, scenario,
	    "the ordinary query succeeds");
	bzero(&serial, sizeof(serial));
	for (workers = 1; workers <= 8; workers *= 2) {
		if (!open_context(&actual, program, RELIC_F_PARALLEL_ALWAYS, workers)) {
			check(false, scenario, "the counted context loads");
			break;
		}
		checkf(answer_query(&actual, query) == 0 && same_answer(&reference, &actual),
		    scenario, "%zu workers preserve every ordered value and ID", workers);
		checkf(actual.counted == (admitted ? 1u : 0u) &&
		    (!admitted || actual.declined == 0), scenario,
		    "%zu workers select the certified strategy exactly when admitted", workers);
		relic_stats(actual.ctx, &parallel, sizeof(parallel));
		if (workers == 1)
			serial = parallel;
		else if (admitted)
			check(same_counted_work(&serial, &parallel), scenario,
			    "workers preserve successful counted work and held storage");
		check(actual.ctx->account.arena_bytes == 0 &&
		    relic_atomic_size_load(&actual.ctx->account.round_bytes) == 0,
		    scenario, "the owner releases all temporary memory");
		close_context(&actual);
	}
	close_context(&reference);
}


static const char *rules =
    "p(X,K) :- s(X), K = X mod 17 + 1000.\n"
    "g(K) :- p(_,K).\n"
    "q(K,N) :- g(K), N = count { p(X,K) }.\n";

static char *
numbered_program(size_t count, const char *definitions)
{
	char *program;
	size_t i, used, bytes;

	bytes = count * 32 + strlen(definitions) + 1;
	program = malloc(bytes);
	if (program == NULL)
		abort();
	used = 0;
	for (i = 0; i < count; i++)
		used += (size_t)snprintf(program + used, bytes - used,
		    "s(%zu).\n", i);
	strlcpy(program + used, definitions, bytes - used);
	return (program);
}


static void
feature_shapes(void)
{
	char *program;

	compare("key and count IDs follow producer publication",
	    "s(3). s(1). s(2). s(3).\n"
	    "p(X,K) :- s(X), K = X + 1000.\n"
	    "g(K) :- p(_,K).\n"
	    "q(N,K) :- g(K), N = count { p(X,K) }.\n", "q(N,K)?", true);
	compare("new numeric keys precede all new count integers",
	    "s(10). s(20). s(30). s(40).\n"
	    "p(I,K) :- s(I), K = I / 20 + 1000.\n"
	    "g(K) :- p(_,K). q(N,K) :- g(K), N = count { p(I,K) }.\n",
	    "q(N,K)?", true);
	compare("P, G and Q use different column permutations and arities",
	    "s(1). s(2). s(3). s(4).\n"
	    "p(B,X,A) :- s(X), A = cat(\"a\", str(X mod 2)), B = cat(\"b\", str(X mod 3)).\n"
	    "g(A,B) :- p(B,_,A).\n"
	    "q(N,B,A) :- g(A,B), N = count { p(B,X,A) }.\n", "q(N,B,A)?", true);
	compare("the final answer is wider than the complete producer",
	    "s(10,70). s(20,70). s(30,80). p(I,K) :- s(I,K).\n"
	    "g(I,K) :- p(I,K). q(N,K,I) :- g(I,K), N = count { p(I,K) }.\n",
	    "q(N,K,I)?", true);
	compare("typed homonyms and embedded NUL stay distinct",
	    "s(1,a). s(2,\"a\"). s(3,7). s(4,\"7\").\n"
	    "s(5,\"x\\x00a\"). s(6,\"x\\x00b\"). s(7,a).\n"
	    "p(X,K) :- s(X,K).\n"
	    "g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(K,N)?", true);
	compare("computed strings borrow an existing symbol spelling",
	    "s(1,stem). s(2,stem). s(3,other).\n"
	    "p(X,Y,K) :- s(X,Y), K = str(Y).\n"
	    "g(K) :- p(_,_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,Y,K) }.\n", "q(K,N)?", true);
	compare("computed NUL keys are copied before batch reuse",
	    "s(1). s(2). s(3).\n"
	    "p(X,K) :- s(X), K = cat(\"x\\x00\", str(X mod 2)).\n"
	    "g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(K,N)?", true);
	compare("a filter with no matches publishes no values",
	    "s(1). s(2).\n"
	    "p(X,K) :- s(X), X < 0, K = str(X).\n"
	    "g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(K,N)?", true);
	compare("an empty extensional source has no group",
	    "p(X,K) :- s(X), K = str(X).\n"
	    "g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(K,N)?", true);
	program = numbered_program(12000,
	    "p(I) :- s(I). g() :- p(_). q(N) :- g(), N = count { p(I) }.\n");
	compare("a nullary key combines many source ranges", program, "q(N)?", true);
	free(program);
	program = numbered_program(12000, rules);
	compare("partials persist over batches and ordered source ranges",
	    program, "q(K,N)?", true);
	free(program);
}


static void
near_misses(void)
{
	char *program;

	compare("a full tuple projection must be distinct",
	    "s(1). s(2). p(K) :- s(X), K = X mod 2.\n"
	    "g(K) :- p(K). q(K,N) :- g(K), N = count { p(K) }.\n",
	    "q(K,N)?", false);
	compare("computed omitted producer columns keep ordinary publication",
	    "s(1). s(2). p(X,K,V) :- s(X), K = X mod 2, V = str(X).\n"
	    "g(K) :- p(_,K,_). q(K,N) :- g(K), N = count { p(X,K,V) }.\n",
	    "q(K,N)?", false);
	compare("multiple producer rules stay ordinary",
	    "s(1). p(X,K) :- s(X), K = 1. p(X,2) :- s(X).\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n",
	    "q(K,N)?", false);
	compare("producer base facts stay ordinary",
	    "s(1). p(9,8). p(X,K) :- s(X), K = 1.\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n",
	    "q(K,N)?", false);
	compare("group base facts can create empty groups",
	    "s(1). p(X,K) :- s(X), K = 1.\n"
	    "g(8). g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n",
	    "q(K,N)?", false);
	compare("answer base facts stay ordinary",
	    "s(1). p(X,K) :- s(X), K = 1.\n"
	    "g(K) :- p(_,K). q(9,8). q(K,N) :- g(K), N = count { p(X,K) }.\n",
	    "q(K,N)?", false);
	compare("a bound query stays ordinary", "s(1).\n"
	    "p(X,K) :- s(X), K = 1. g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(1,N)?", false);
	compare("a wildcard fold projection stays ordinary", "s(1).\n"
	    "p(X,K) :- s(X), K = 1. g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(_,K) }.\n", "q(K,N)?", false);
	compare("another aggregate stays ordinary", "s(1).\n"
	    "p(X,K) :- s(X), K = 1. g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = sum(X) { p(X,K) }.\n", "q(K,N)?", false);
	compare("another generator stays ordinary", "s(1). t(2).\n"
	    "p(X,Y,K) :- s(X), t(Y), K = 1. g(K) :- p(_,_,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,Y,K) }.\n", "q(K,N)?", false);
	compare("an independent guard can create empty groups", "s(1). t(1). t(9).\n"
	    "p(X,K) :- s(X), K = 1. g(K) :- t(K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(K,N)?", false);
	compare("a recursive source stays ordinary", "e(1). s(X) :- e(X).\n"
	    "s(Y) :- s(X), X < 3, Y = X + 1. p(X,K) :- s(X), K = X mod 2.\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n",
	    "q(K,N)?", false);
	compare("an additional aggregate stays ordinary", "s(1).\n"
	    "p(X,K) :- s(X), K = 1. g(K) :- p(_,K).\n"
	    "q(K,N,M) :- g(K), N = count { p(X,K) }, M = sum(Y) { p(Y,K) }.\n",
	    "q(K,N,M)?", false);
	compare("a guard selection stays ordinary", "s(1,1). s(2,1).\n"
	    "p(X,K) :- s(X,K). g(K) :- p(K,K).\n"
	    "q(K,N) :- g(K), N = count { p(X,K) }.\n", "q(K,N)?", false);
	compare("a zero-column key counts a nonempty producer", "s(1). s(2).\n"
	    "p(X) :- s(X). g() :- p(_). q(N) :- g(), N = count { p(X) }.\n",
	    "q(N)?", true);
	/*
	 * Every V is a new integer interleaved with the new keys, so a counted
	 * path that projected V away would assign the keys and counts other
	 * IDs than the ordinary evaluation that publishes V.
	 */
	program = numbered_program(12000,
	    "p(X,K,V) :- s(X), K = X mod 17 + 100000, V = X * 3 + 200000.\n"
	    "g(K) :- p(_,K,_). q(K,N) :- g(K), N = count { p(X,K,V) }.\n");
	compare("a computed non-key number declines the counted path",
	    program, "q(K,N)?", false);
	free(program);
}


static void
prepared_queries_keep_definitions(void)
{
	observed_t actual, reference;
	relic_query_t *query;
	char *program;
	const char *later[] = { "p(X,K)?", "g(K)?", "q(K,N)?" };
	size_t i;

	program = numbered_program(513, rules);
	check(open_context(&actual, program, 0, 4) &&
	    open_context(&reference, program, RELIC_F_NO_MAGIC, 1),
	    "prepared counted queries", "both contexts load");
	check(answer_query(&reference, "q(K,N)?") == 0,
	    "prepared counted queries", "both contexts publish the same initial values");
	query = relic_query_prepare(actual.ctx, NULL, "q(K,N)?");
	check(query != NULL, "prepared counted queries", "the query prepares");
	for (i = 0; i < 3; i++) {
		actual.count = 0;
		check(relic_query_run(actual.ctx, query, keep_row, &actual) == 0 &&
		    actual.count == 34 && relic_relation_count(query->answer) == 0,
		    "prepared counted queries", "one prepared query can run repeatedly");
		check(!relic_relations_current(actual.ctx) &&
		    relic_relation_total_tuples(actual.ctx) == 513,
		    "prepared counted queries", "only the complete source is retained");
	}
	check(relic_load_string(actual.ctx, NULL, "s(513).") == 0 &&
	    relic_load_string(reference.ctx, NULL, "s(513).") == 0,
	    "prepared counted queries", "the complete source can gain facts");
	actual.count = 0;
	check(relic_query_run(actual.ctx, query, keep_row, &actual) == 0 &&
	    answer_query(&reference, "q(K,N)?") == 0 && same_answer(&actual, &reference),
	    "prepared counted queries", "the certificate revalidates after loading");
	relic_query_destroy(query);
	for (i = 0; i < sizeof(later) / sizeof(later[0]); i++)
		check(answer_query(&actual, later[i]) == 0 &&
		    answer_query(&reference, later[i]) == 0 && same_answer(&actual, &reference),
		    later[i], "later queries use the original predicate definitions");
	close_context(&actual);
	close_context(&reference);
	free(program);
}


static void
configuration_guards(void)
{
	observed_t observed;
	size_t i;

	for (i = 0; i < 8; i++) {
		check(open_context(&observed, "s(1). p(X,K) :- s(X), K = 2.\n"
		    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n", 0, 1),
		    "configuration guards", "the context loads");
		if (i == 0)
			observed.ctx->config.max_tuples = SIZE_MAX;
		if (i == 1)
			observed.ctx->config.max_memory = SIZE_MAX;
		if (i == 2)
			observed.ctx->config.max_magic = SIZE_MAX;
		if (i == 3)
			observed.ctx->config.flags |= RELIC_F_NO_MAGIC;
		if (i == 4)
			check(relic_materialize(observed.ctx) == 0,
			    "configuration guards", "the program materializes");
		if (i == 5)
			observed.ctx->config.max_tuples = 1;
		if (i == 6) {
			observed.ctx->config.max_memory = 1;
			observed.ctx->account.limit = 1;
		}
		if (i == 7)
			observed.ctx->config.max_magic = 1;
		check(answer_query(&observed, "q(K,N)?") == (i == 5 || i == 6 ? -1 : 0) &&
		    observed.counted == 0 && ((i != 5 && i != 6) || observed.count == 0),
		    "configuration guards", "explicit limits and current programs stay ordinary");
		close_context(&observed);
	}
}

static void
ordinary_publication_preserves_diagnostics(void)
{
	observed_t actual, reference;
	char *program;
	size_t workers, i, test;
	const char *definitions[] = {
	    "s(a). p(X,K) :- s(X), K = X + 10000, X < 200.\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n",
	    "p(X,K) :- s(X), K = 100000 / (130 - X).\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n"
	};
	int expected;

	for (test = 0; test < 2; test++) {
		program = numbered_program(4001, definitions[test]);
		check(open_context(&reference, program,
		    RELIC_F_NO_MAGIC | RELIC_F_NOTE_MIXED, 1),
		    "ordinary diagnostic publication", "the ordinary context loads");
		expected = answer_query(&reference, "q(K,N)?");
		check(expected == (test == 0 ? 0 : -1) && (reference.notes != 0) == (test == 0),
		    "ordinary diagnostic publication", "the fixture produces a note or a late error");
		for (workers = 1; workers <= 4; workers *= 4) {
			check(open_context(&actual, program,
			    RELIC_F_NOTE_MIXED | RELIC_F_PARALLEL_ALWAYS, workers),
			    "ordinary diagnostic publication", "the counted context loads");
			check(answer_query(&actual, "q(K,N)?") == expected &&
			    same_answer(&actual, &reference) &&
			    strcmp(actual.diagnostics, reference.diagnostics) == 0,
			    "ordinary diagnostic publication", "ordinary replay preserves diagnostics and answer order");
			check(actual.ctx->values.integer_count == reference.ctx->values.integer_count,
			    "ordinary diagnostic publication", "the retained integer prefix has the ordinary length");
			for (i = 0; i < actual.ctx->values.integer_count &&
			    i < reference.ctx->values.integer_count; i++)
				check(actual.ctx->values.integers[i] == reference.ctx->values.integers[i],
				    "ordinary diagnostic publication", "the retained integer prefix has the ordinary values");
			check(actual.counted == 1 && actual.declined == 0,
			    "ordinary diagnostic publication", "the same counted strategy handles notes and errors");
			close_context(&actual);
		}
		close_context(&reference);
		free(program);
	}
}

static void
certificate_search_is_bounded(void)
{
	observed_t observed;
	char program[16384];
	size_t used, i;

	used = (size_t)snprintf(program, sizeof(program), "s(1).\n");
	for (i = 0; i < 256u; i++)
		used += (size_t)snprintf(program + used, sizeof(program) - used,
		    "unrelated%zu(X) :- s(X).\n", i);
	strlcpy(program + used, rules, sizeof(program) - used);
	check(open_context(&observed, program, 0, 1) &&
	    answer_query(&observed, "q(K,N)?") == 0 && observed.counted == 0,
	    "bounded certificate", "a distant definition chooses ordinary evaluation");
	close_context(&observed);
}

static void
high_cardinality_stays_counted(void)
{
	observed_t actual, reference;
	char *program;

	program = numbered_program(160000,
	    "p(X,K) :- s(X), K = X + 100000.\n"
	    "g(X,K) :- p(X,K). q(X,K,N) :- g(X,K), N = count { p(X,K) }.\n");
	check(open_context(&actual, program, RELIC_F_PARALLEL_ALWAYS, 8) &&
	    open_context(&reference, program, RELIC_F_NO_MAGIC, 1),
	    "high-cardinality keys", "both contexts load");
	check(answer_query(&actual, "q(X,K,N)?") == 0 &&
	    answer_query(&reference, "q(X,K,N)?") == 0 && same_answer(&actual, &reference),
	    "high-cardinality keys", "high cardinality preserves the complete answer");
	check(actual.counted == 1 && actual.declined == 0, "high-cardinality keys",
	    "the counted destination grows without a private cutoff");
	close_context(&actual);
	close_context(&reference);
	free(program);
}


static void
shared_refusal_retries_the_same_consumer(void)
{
	observed_t actual, reference;
	char *program;

	program = numbered_program(12000,
	    "p(X,K) :- s(X), K = cat(\"key\", str(X mod 17)).\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(X,K) }.\n");
	check(open_context(&reference, program, RELIC_F_NO_MAGIC, 1) &&
	    answer_query(&reference, "q(K,N)?") == 0,
	    "shared refusal", "the ordinary reference answers");
	check(open_context(&actual, program, RELIC_F_PARALLEL_ALWAYS, 4),
	    "shared refusal", "the counted context loads");
	force_shared_decline = true;
	forced_declines = 0;
	check(answer_query(&actual, "q(K,N)?") == 0 && same_answer(&actual, &reference),
	    "shared refusal", "serial retry preserves every answer value and ID");
	check(forced_declines == 1 && actual.counted == 1 && actual.declined == 0 &&
	    actual.ctx->pool->abandoned_rounds == 1 &&
	    relic_relation_total_tuples(actual.ctx) == 12000,
	    "shared refusal", "one producer round retries without ordinary query replay");
	close_context(&actual);
	close_context(&reference);
	free(program);
}

static void
shared_storage_and_actual_work(void)
{
	observed_t actual;
	relic_stats_t stats;
	char *program;

	program = numbered_program(12000, rules);
	check(open_context(&actual, program, RELIC_F_PARALLEL_ALWAYS, 4),
	    "shared counted work", "the context loads");
	require_other_worker = true;
	inspect_retained_keys = true;
	check(answer_query(&actual, "q(K,N)?") == 0,
	    "shared counted work", "the shared producer answers");
	require_other_worker = false;
	inspect_retained_keys = false;
	relic_stats(actual.ctx, &stats, sizeof(stats));
	check(stats.derivations == 12017 && stats.tuples_inserted == 17 &&
	    stats.tuples_scanned == 12017 && stats.tuples_looked_up == 0 &&
	    stats.tuples == 12000 && actual.counted == 1,
	    "shared counted work", "actual work is source EMITs, installed keys and answer rows");
	if (relic_pool_workers(actual.ctx->pool) > 1)
		check(actual.ctx->pool->thread_tasks != 0,
		    "shared counted work", "another existing worker executes a source range");
	close_context(&actual);
	free(program);

	program = numbered_program(12000,
	    "p(I) :- s(I). g() :- p(_). q(N) :- g(), N = count { p(I) }.\n");
	check(open_context(&actual, program, RELIC_F_PARALLEL_ALWAYS, 4),
	    "nullary counted work", "the context loads");
	require_other_worker = inspect_retained_keys = true;
	check(answer_query(&actual, "q(N)?") == 0,
	    "nullary counted work", "several source ranges combine into one empty key");
	require_other_worker = inspect_retained_keys = false;
	relic_stats(actual.ctx, &stats, sizeof(stats));
	check(stats.derivations == 12001 && stats.tuples_inserted == 1 &&
	    stats.tuples_scanned == 12001 && stats.tuples == 12000 &&
	    actual.count == 1 && actual.values[0].integer == 12000,
	    "nullary counted work", "one empty key counts every source emission");
	if (relic_pool_workers(actual.ctx->pool) > 1)
		check(actual.ctx->pool->thread_tasks != 0,
		    "nullary counted work", "another worker contributes without candidate columns");
	close_context(&actual);
	free(program);
}

static void
constant_keys_keep_source_traversal(void)
{
	char *program;

	program = numbered_program(12000,
	    "p(I,7) :- s(I).\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(I,K) }.\n");
	compare("constant keys retain every producer emission", program, "q(K,N)?", true);
	free(program);
}

static void
callbacks_keep_their_interruption_contract(void)
{
	observed_t actual;
	char *program;

	program = numbered_program(12000, rules);
	check(open_context(&actual, program, 0, 1),
	    "callback interrupts", "the context loads");
	actual.interrupt_trace = true;
	check(answer_query(&actual, "q(K,N)?") == -1 && actual.count == 0 &&
	    actual.errors == 1 && relic_error_class(actual.ctx) == RELIC_ERROR_INTERRUPTED,
	    "callback interrupts", "a trace interrupt stops before execution");
	actual.interrupt_trace = false;
	actual.interrupt_delivery = true;
	check(answer_query(&actual, "q(K,N)?") == -1 && actual.count == 2,
	    "callback interrupts", "delivery retains its prefix contract");
	check(actual.ctx->account.arena_bytes == 0 &&
	    relic_atomic_size_load(&actual.ctx->account.round_bytes) == 0,
	    "callback interrupts", "failure releases destination and evaluator memory");
	close_context(&actual);
	free(program);
}

static bool
stage_key_pair(observed_t *observed, const relic_destination_t *destination,
    relic_destination_append_t *append)
{
	relic_dom_t key;
	size_t part;

	bzero(append, sizeof(*append));
	append->relation.count = 2;
	append->relation.first = relic_relation_end(destination->relation);
	if (relic_destination_append_open(observed->ctx, destination, append) != 0)
		return (false);
	key = relic_value_find_integer(observed->ctx, 7);
	relic_destination_stage(destination, append, 0, &key,
	    relic_relation_hash(destination->relation, &key), 2);
	key = relic_value_find_integer(observed->ctx, 8);
	relic_destination_stage(destination, append, 1, &key,
	    relic_relation_hash(destination->relation, &key), 1);
	for (part = 0; part < relic_relation_list_parts(&append->relation); part++)
		relic_relation_list_append(&append->relation, part);
	for (part = 0; part < RELIC_STRIPES; part++)
		append->relation.growth.by_stripe[part] =
		    append->relation.stripe_start[part + 1] - append->relation.stripe_start[part];
	return (true);
}

static void
keys_and_counts_are_installed_together(void)
{
	observed_t observed;
	relic_destination_t destination;
	relic_destination_append_t append;
	relic_arena_t arena;
	relic_plan_t plan;
	const relic_rule_t *producer;
	relic_dom_t keys[3];
	bool added[3];
	size_t completed, part;

	if (!open_context(&observed,
	    "s(1,7). s(2,7). s(3,8). p(I,K) :- s(I,K).\n", 0, 1)) {
		check(false, "paired key/count storage", "the fixture loads");
		return;
	}
	relic_arena_init_charged(&arena, &observed.ctx->account);
	check(relic_check(observed.ctx) == 0, "paired key/count storage", "the rule checks");
	producer = &observed.ctx->program.rules.entries[0];
	check(relic_plan_rule(observed.ctx, &arena, producer, NULL, &plan) == 0,
	    "paired key/count storage", "the complete producer plans");
	{
		const uint16_t key_column = 1;
		const uint16_t invalid[] = { 0, 0 };
		uint16_t output[] = { 1, 0 };

		check(relic_destination_count(observed.ctx, &destination, producer,
		    &plan, &key_column, 1, invalid, 2) == 0 &&
		    destination.relation == NULL && observed.errors == 0,
		    "paired key/count storage", "a repeated result column declines before allocation");
		check(relic_destination_count(observed.ctx, &destination, producer,
		    &plan, &key_column, 1, output, 1) == 0 &&
		    destination.relation == NULL && observed.errors == 0,
		    "paired key/count storage", "result arity must be key arity plus one");

		check(relic_destination_count(observed.ctx, &destination, producer,
		    &plan, &key_column, 1, output, 2) == 1,
		    "paired key/count storage", "the destination derives its source bound");
		output[0] = 0;
	}
	keys[0] = keys[1] = relic_value_find_integer(observed.ctx, 7);
	keys[2] = relic_value_find_integer(observed.ctx, 8);
	check(relic_destination_insert(observed.ctx, &destination, keys, 3,
	    added, &completed) == 0 && completed == 3 && added[0] && !added[1] && added[2] &&
	    relic_destination_count_value(&destination, 1) == 2 &&
	    relic_destination_count_value(&destination, 2) == 1,
	    "paired key/count storage", "serial key IDs and counts stay aligned");
	{
		relic_dom_t result[RELIC_MAX_ARITY];
		const relic_dom_t *tuple;

		tuple = relic_destination_result(observed.ctx, &destination, 1, result);
		check(relic_destination_result_arity(&destination) == 2 && tuple == result &&
		    tuple[0] == relic_value_find_integer(observed.ctx, 2) && tuple[1] == keys[0],
		    "paired key/count storage", "completed rows use the owner's copied result permutation");
	}
	relic_destination_close(observed.ctx, &destination);
	{
		const uint16_t key_column = 1;
		const uint16_t output[] = { 0, 1 };

		check(relic_destination_count(observed.ctx, &destination, producer,
		    &plan, &key_column, 1, output, 2) == 1 &&
		    stage_key_pair(&observed, &destination, &append),
		    "paired key/count storage", "an empty destination stages a paired append");
	}
	observed.ctx->account.limit = 1;
	check(relic_destination_reserve_append(observed.ctx, &destination, &append) == -1 &&
	    observed.errors == 0 && relic_relation_count(destination.relation) == 0,
	    "paired key/count storage", "refused count capacity installs no key and reports nothing");
	observed.ctx->account.limit = 0;
	relic_destination_append_close(observed.ctx, &append);
	check(stage_key_pair(&observed, &destination, &append) &&
	    relic_destination_reserve_append(observed.ctx, &destination, &append) == 0 &&
	    relic_relation_count(destination.relation) == 0,
	    "paired key/count storage", "successful reservation still publishes no key");
	relic_destination_take_append(observed.ctx, &destination, &append);
	for (part = 0; part < relic_relation_apply_parts(&append.relation); part++)
		relic_relation_apply_append(destination.relation, &append.relation, part);
	relic_relation_finish_append(observed.ctx, destination.relation, &append.relation);
	check(relic_relation_count(destination.relation) == 2 &&
	    relic_destination_count_value(&destination, 1) == 2 &&
	    relic_destination_count_value(&destination, 2) == 1,
	    "paired key/count storage", "deferred application installs counts at the matching key IDs");
	relic_destination_append_close(observed.ctx, &append);
	relic_destination_close(observed.ctx, &destination);
	relic_arena_reset(&arena);
	check(relic_atomic_size_load(&observed.ctx->account.round_bytes) == 0 &&
	    observed.ctx->account.arena_bytes == 0 &&
	    relic_relation_total_tuples(observed.ctx) == 3,
	    "paired key/count storage", "cleanup leaves only the complete base source");
	close_context(&observed);
}

typedef struct stopped_result stopped_result_t;

struct stopped_result {
	observed_t	*observed;
	relic_query_t	*query;
	size_t		 rows;
};

static int
stop_after_values_are_published(void *argument, const relic_value_t *row,
    size_t arity)
{
	stopped_result_t *result = argument;
	relic_t *ctx = result->observed->ctx;
	relic_stats_t stats;
	size_t count;

	result->rows++;
	check(arity == 2 && row[1].type == RELIC_INTEGER && row[1].v.integer == 1,
	    "completed counted delivery", "the first row preserves key order");
	for (count = 1; count <= 81; count++)
		check(relic_value_find_integer(ctx, (int64_t)count) != RELIC_ID_NONE,
		    "completed counted delivery", "every count ID is published before the first row");
	check(relic_relation_count(result->query->answer) == 0 &&
	    ctx->eval_arena.total_bytes == 0,
	    "completed counted delivery", "delivery uses no answer copy or producer scratch");
	relic_stats(ctx, &stats, sizeof(stats));
	check(stats.tuples == 3402 && stats.tuples_inserted == 81 &&
	    stats.derivations == 3402 && stats.tuples_scanned == 3402 &&
	    relic_atomic_size_load(&ctx->account.round_bytes) != 0,
	    "completed counted delivery", "the completed key/count owner stays alive through callbacks");
	return (1);
}

/* Symbol-only input ensures that all 81 count integers are new values. */
static void
early_stop_keeps_all_count_values(void)
{
	char program[65536];
	observed_t observed;
	stopped_result_t stopped;
	relic_query_t *query;
	size_t key, item, used, workers;

	used = 0;
	for (key = 1; key <= 81; key++)
		for (item = 1; item <= key; item++)
			used += (size_t)snprintf(program + used, sizeof(program) - used,
			    "s(r%zu,g%zu).\n", item, key);
	strlcpy(program + used,
	    "p(I,K) :- s(I,K). g(K) :- p(_,K).\n"
	    "q(K,N) :- g(K), N = count { p(I,K) }.\n", sizeof(program) - used);
	for (workers = 1; workers <= 4; workers *= 4) {
		check(open_context(&observed, program, RELIC_F_PARALLEL_ALWAYS, workers),
		    "completed counted delivery", "the symbol-only context loads");
		query = relic_query_prepare(observed.ctx, NULL, "q(K,N)?");
		check(query != NULL && observed.ctx->values.integer_count == 0,
		    "completed counted delivery", "the query starts without count integers");
		if (query != NULL) {
			stopped.observed = &observed;
			stopped.query = query;
			stopped.rows = 0;
			check(relic_query_run(observed.ctx, query,
			    stop_after_values_are_published, &stopped) == 0 && stopped.rows == 1 &&
			    observed.counted == 1 && observed.ctx->values.integer_count == 81,
			    "completed counted delivery", "early stop retains the complete value prefix");
			check(relic_relation_count(query->answer) == 0 &&
			    relic_relation_total_tuples(observed.ctx) == 3321 &&
			    observed.ctx->account.arena_bytes == 0 &&
			    relic_atomic_size_load(&observed.ctx->account.round_bytes) == 0,
			    "completed counted delivery", "early stop releases the completed owner");
			check(relic_query_run(observed.ctx, query, keep_row, &observed) == 0 &&
			    observed.count == 162,
			    "completed counted delivery", "the same prepared query remains usable");
			relic_query_destroy(query);
		}
		close_context(&observed);
	}
}

static void
reported_expression_errors_remain_final(void)
{
	observed_t observed;
	char *program;
	size_t workers;

	program = numbered_program(4001,
	    "p(I,K) :- s(I), K = 100000 / (130 - I).\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(I,K) }.\n");
	for (workers = 1; workers <= 4; workers *= 4) {
		check(open_context(&observed, program, RELIC_F_PARALLEL_ALWAYS, workers),
		    "reported error callback", "the context loads");
		observed.interrupt_error = true;
		check(answer_query(&observed, "q(K,N)?") == -1 && observed.errors == 1 &&
		    observed.count == 0 && relic_error_class(observed.ctx) == RELIC_ERROR_EVALUATION,
		    "reported error callback", "an error callback interrupt preserves the original error");
		close_context(&observed);
	}
	free(program);
}

typedef struct set_candidate_shape set_candidate_shape_t;

struct set_candidate_shape {
	size_t offset;
	uint64_t hash;
	uint32_t target;
	bool discarded;
};

static void
ordinary_candidates_keep_their_footprint(void)
{
	check(sizeof(relic_candidate_t) == sizeof(set_candidate_shape_t),
	    "ordinary candidate footprint", "multiplicity fits the set candidate's flag word");
}

static void
note_callbacks_interrupt_without_replay(void)
{
	observed_t observed;
	char *program;
	size_t workers;

	program = numbered_program(4001,
	    "s(a). p(I,K) :- s(I), K = I + 10000, I < 200.\n"
	    "g(K) :- p(_,K). q(K,N) :- g(K), N = count { p(I,K) }.\n");
	for (workers = 1; workers <= 4; workers *= 4) {
		check(open_context(&observed, program,
		    RELIC_F_NOTE_MIXED | RELIC_F_PARALLEL_ALWAYS, workers),
		    "note callback interruption", "the context loads");
		observed.interrupt_note = true;
		check(answer_query(&observed, "q(K,N)?") == -1 && observed.count == 0 &&
		    observed.notes == 1 && observed.errors == 1 && observed.counted == 1 &&
		    relic_error_class(observed.ctx) == RELIC_ERROR_INTERRUPTED,
		    "note callback interruption", "a reported note stops without counted-query replay");
		check(relic_relation_total_tuples(observed.ctx) == 4002 &&
		    observed.ctx->account.arena_bytes == 0 &&
		    relic_atomic_size_load(&observed.ctx->account.round_bytes) == 0,
		    "note callback interruption", "serial or published shared keys are released");
		observed.interrupt_note = false;
		check(answer_query(&observed, "q(K,N)?") == 0 && observed.count == 400,
		    "note callback interruption", "the context remains usable after the callback interrupts");
		close_context(&observed);
	}
	free(program);
}

/*
 * The counted path runs only without a configured limit, so no public
 * configuration lets a limit refuse its storage.  relic_grouped_answer()
 * reads the configuration, though, and a limit set on the account alone, a
 * state only a test can make, lets the path run under one.  Whichever of
 * the path's allocations the limit then refuses, the report names the
 * limit rather than the allocator.
 */
static void
count_storage_reports_its_limit(void)
{
	observed_t observed;
	relic_account_t *account;
	char *program;
	size_t extra, held, refused_counted;
	bool answered;

	program = numbered_program(64, rules);
	refused_counted = 0;
	answered = false;
	for (extra = 0; extra < 65536; extra += 8) {
		if (!open_context(&observed, program, 0, 1)) {
			check(false, "an account limit", "the program loads");
			break;
		}
		account = &observed.ctx->account;
		held = account->store_bytes + account->value_bytes +
		    account->pool_bytes + account->predtab_bytes +
		    account->arena_bytes;
		account->limit = held + extra;
		if (answer_query(&observed, "q(K,N)?") == 0) {
			answered = true;
			close_context(&observed);
			break;
		}
		if (observed.counted != 0)
			refused_counted++;
		checkf(strstr(observed.diagnostics, "limit exceeded: memory=") !=
		    NULL && strstr(observed.diagnostics, "out of memory") == NULL,
		    "an account limit", "a refusal %zu bytes over the held total "
		    "names the limit: %s", extra, observed.diagnostics);
		close_context(&observed);
	}
	check(refused_counted != 0, "an account limit",
	    "the limit refuses the counted path itself");
	check(answered, "an account limit",
	    "the query succeeds once the limit leaves it room");
	free(program);
}


int
main(void)
{
	ordinary_candidates_keep_their_footprint();
	keys_and_counts_are_installed_together();
	early_stop_keeps_all_count_values();
	reported_expression_errors_remain_final();
	note_callbacks_interrupt_without_replay();
	feature_shapes();
	near_misses();
	prepared_queries_keep_definitions();
	configuration_guards();
	ordinary_publication_preserves_diagnostics();
	certificate_search_is_bounded();
	high_cardinality_stays_counted();
	constant_keys_keep_source_traversal();
	shared_refusal_retries_the_same_consumer();
	shared_storage_and_actual_work();
	callbacks_keep_their_interruption_contract();
	count_storage_reports_its_limit();
	return (check_summary("test_grouped"));
}
