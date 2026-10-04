/*
 * Copyright (c) 2025-2026 Stanislav Sedov <stas@deglitch.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <stdlib.h>
#include <strings.h>

#include "depgraph.h"
#include "internal.h"

/*
 * Stratification: reject recursion through negation, and hand the evaluator
 * the order a stratified program runs in.
 *
 * The dependency graph's components come out of Tarjan's algorithm with
 * every component after everything it depends on.  A strict edge -- one
 * from a negated literal or from an aggregate -- between two members of one
 * component is recursion through negation or through an aggregate, and the
 * program is rejected with the cycle named.  Otherwise each component gets
 * the least stratum that puts every strict edge strictly below it, which one
 * pass in completion order computes, because an edge leaving a component
 * always reaches one that completed earlier.
 *
 * An aggregate reaches what its body reads through its group predicate: an
 * aggregate edge to the group predicate, and an edge of the group rule from
 * there.  The writer never wrote the group predicate, so a diagnostic
 * reports such a pair of edges as one dependency through an aggregate.
 *
 * The schedule is the components that have rules, sorted by stratum.  The
 * sort is stable, so each stratum keeps completion order, and the result is
 * still an order in which every component follows what it depends on: a
 * dependency's stratum is never above its dependant's, so it is either in an
 * earlier stratum or earlier within the same one.
 *
 * The analysis is rebuilt from nothing whenever the program has gained rules
 * since the schedule was made.  It is linear in the size of the program, and
 * it happens once per load.  The same analysis schedules the rules a query's
 * Magic rewrite generates, which is how they are evaluated by the engine that
 * evaluates the program, and is what would catch a rewrite that recursed
 * through negation.
 */

/* No edge: a component through which no strict edge recurses. */
#define	RELIC_NO_EDGE	SIZE_MAX

typedef struct relic_analysis relic_analysis_t;
typedef struct relic_cycle_search relic_cycle_search_t;

/*
 * The state of the search for the cycle a diagnostic names, indexed by
 * vertex.  `arrival[v]` is the edge the search first reached v by, `queue`
 * is the breadth-first frontier, and `path` receives the edges of the path
 * found, first edge first.
 */
struct relic_cycle_search {
	bool		*reached;
	size_t		*arrival;
	size_t		*path;
	uint32_t	*queue;
};

/*
 * One analysis of a table of rules: the graph, and what is derived from it
 * per component, all in the scratch arena that is released when the analysis
 * ends.  `strata`, `recursive` and `rule_counts` -- the number of rules whose
 * head each component holds -- are indexed by component number.
 *
 * `search` is allocated only when a cycle is found, since a stratified
 * program never needs it.
 */
struct relic_analysis {
	relic_t			*ctx;
	const relic_ruletab_t	*rules;
	relic_arena_t		 scratch;
	relic_depgraph_t	 graph;
	relic_cycle_search_t	 search;
	uint32_t		*strata;
	bool			*recursive;
	size_t			*rule_counts;
};

static enum relic_analysis_result analyse_program(relic_analysis_t *analysis,
		    relic_schedule_t *schedule);
static int	 allocate_cycle_search(relic_analysis_t *analysis);
static int	 assign_strata(relic_analysis_t *analysis);
static int	 compare_written_order(const void *left, const void *right);
static bool	 edge_written_before(const relic_dep_edge_t *a,
		    const relic_dep_edge_t *b);
static int	 find_strict_cycles(relic_analysis_t *analysis,
		    relic_dep_edge_t **cyclesp, size_t *countp);
static int	 build_schedule(relic_analysis_t *analysis,
		    relic_schedule_t *schedule);
static size_t	 count_members(const relic_depgraph_t *graph,
		    const uint32_t *order, size_t count);
static int	 count_rules(relic_analysis_t *analysis);
static void	 fill_schedule(const relic_analysis_t *analysis,
		    const uint32_t *order, size_t count, size_t *next_rule,
		    relic_schedule_t *schedule);
static void	 install_schedule(relic_program_t *program,
		    relic_schedule_t *schedule);
static bool	*mark_group_vertices(relic_analysis_t *analysis);
static void	 note_aggregate(relic_analysis_t *analysis,
		    const relic_dep_edge_t *aggregate,
		    const relic_dep_edge_t *body);
static void	 note_edge(relic_analysis_t *analysis,
		    const relic_dep_edge_t *edge);
static void	 note_path(relic_analysis_t *analysis,
		    const relic_dep_edge_t *closing, size_t length);
static int	 order_by_stratum(relic_analysis_t *analysis, uint32_t *order,
		    size_t *countp);
static void	 report_cycle(relic_analysis_t *analysis,
		    const relic_dep_edge_t *closing);
static enum relic_analysis_result report_strict_cycles(
		    relic_analysis_t *analysis);
static void	 settle_component(relic_analysis_t *analysis,
		    uint32_t component);
static const relic_dep_edge_t *path_edge(const relic_analysis_t *analysis,
		    size_t length, size_t *takenp);
static size_t	 shortest_path(relic_analysis_t *analysis, uint32_t from,
		    uint32_t to);

void
relic_schedule_init(relic_schedule_t *schedule)
{
	bzero(schedule, sizeof(*schedule));
	relic_arena_init(&schedule->arena, RELIC_ARENA_ANY_OBJECT);
}

void
relic_schedule_reset(relic_schedule_t *schedule)
{
	relic_arena_reset(&schedule->arena);
	relic_schedule_init(schedule);
}

/*
 * One note of the unstratified diagnostic: the edge, pointing at the literal
 * that makes it.
 */
static void
note_edge(relic_analysis_t *analysis, const relic_dep_edge_t *edge)
{
	const relic_rule_t *rule;
	char source[RELIC_PRED_TEXT_MAX];
	char target[RELIC_PRED_TEXT_MAX];

	rule = &analysis->rules->entries[edge->rule_index];

	relic_pred_describe(analysis->ctx, relic_depgraph_pred(edge->source),
	    source, sizeof(source));
	relic_pred_describe(analysis->ctx, relic_depgraph_pred(edge->target),
	    target, sizeof(target));

	relic_diag_note(analysis->ctx,
	    &rule->body[edge->literal_index].where, "%s depends %son %s",
	    source, edge->kind == RELIC_DEP_NEGATIVE ? "negatively " : "",
	    target);
}

/*
 * One note for an aggregate edge and the edge of its group rule that
 * follows it on a cycle: the rule holding the aggregate depends on what the
 * aggregate's body reads, positively or negatively as the body reads it.
 * The note points at the literal inside the braces.
 */
static void
note_aggregate(relic_analysis_t *analysis, const relic_dep_edge_t *aggregate,
    const relic_dep_edge_t *body)
{
	const relic_rule_t *group_rule;
	char source[RELIC_PRED_TEXT_MAX];
	char target[RELIC_PRED_TEXT_MAX];

	group_rule = &analysis->rules->entries[body->rule_index];

	relic_pred_describe(analysis->ctx,
	    relic_depgraph_pred(aggregate->source), source, sizeof(source));
	relic_pred_describe(analysis->ctx, relic_depgraph_pred(body->target),
	    target, sizeof(target));

	relic_diag_note(analysis->ctx,
	    &group_rule->body[body->literal_index].where,
	    "%s depends %son %s through an aggregate", source,
	    body->kind == RELIC_DEP_NEGATIVE ? "negatively " : "", target);
}

/*
 * The notes for a cycle: the closing edge, then the `length` edges of the
 * path the search stored, which leads from the closing edge's target back
 * to its source.
 *
 * An aggregate edge leads to a group predicate, and only an edge of its
 * group rule leads out of one, so on the cycle every aggregate edge is
 * followed by an edge of its group rule, and the two make one note.  The
 * cycle ends at the closing edge's source, which heads a rule the writer
 * wrote, so it never ends on an aggregate edge; one that did would have
 * that edge noted alone.
 */
static void
note_path(relic_analysis_t *analysis, const relic_dep_edge_t *closing,
    size_t length)
{
	const relic_dep_edge_t *edge;
	const relic_dep_edge_t *next;
	size_t taken;

	edge = closing;
	taken = 0;
	while (edge != NULL) {
		next = path_edge(analysis, length, &taken);

		if (edge->kind == RELIC_DEP_AGGREGATE && next != NULL) {
			note_aggregate(analysis, edge, next);
			next = path_edge(analysis, length, &taken);
		} else {
			note_edge(analysis, edge);
		}

		edge = next;
	}
}

/*
 * The next of the `length` edges of the stored path, counting those taken
 * in *takenp, or NULL once every one has been.
 */
static const relic_dep_edge_t *
path_edge(const relic_analysis_t *analysis, size_t length, size_t *takenp)
{
	const relic_dep_edge_t *edge;

	if (*takenp == length)
		return (NULL);

	edge = &analysis->graph.edges[analysis->search.path[*takenp]];
	(*takenp)++;

	return (edge);
}

/*
 * Room for the cycle search, shared by every cycle reported.  Returns -1 when
 * the scratch arena refused.
 */
static int
allocate_cycle_search(relic_analysis_t *analysis)
{
	relic_cycle_search_t *search;
	relic_arena_t *scratch;
	size_t count;

	search = &analysis->search;
	scratch = &analysis->scratch;
	count = analysis->graph.vertex_count;

	search->reached = relic_arena_allocarray(scratch, count,
	    sizeof(*search->reached));
	search->arrival = relic_arena_allocarray(scratch, count,
	    sizeof(*search->arrival));
	search->path = relic_arena_allocarray(scratch, count,
	    sizeof(*search->path));
	search->queue = relic_arena_allocarray(scratch, count,
	    sizeof(*search->queue));
	if (relic_arena_failed(scratch))
		return (-1);

	return (0);
}

/*
 * Find the shortest path of edges from vertex `from` to vertex `to`, two
 * members of one component, and store it in the search's `path`, first edge
 * first.  Returns the number of edges, which is zero when `from` is `to`.
 *
 * The search is breadth-first and follows each vertex's edges in the order
 * the program wrote them, so the path it finds is the same on every run.  It
 * stays inside the component, because a path that left it could never come
 * back.  `arrival` records how each vertex was first reached, so following
 * it back from `to` walks the path in reverse; the walk is done twice, once
 * to measure the path and once to write it from its far end, so that `path`
 * reads forward.
 *
 * Only the component's members are cleared before the search, because it
 * reaches nothing else.  Two members of a component reach each other by
 * definition, so `to` is always reached and the walk back always arrives at
 * `from`.
 */
static size_t
shortest_path(relic_analysis_t *analysis, uint32_t from, uint32_t to)
{
	const relic_depgraph_t *graph;
	const relic_dep_edge_t *edge;
	relic_cycle_search_t *search;
	size_t queue_head;
	size_t queue_tail;
	size_t length;
	size_t e;
	size_t m;
	uint32_t component;
	uint32_t vertex;

	graph = &analysis->graph;
	search = &analysis->search;
	component = graph->component_of[from];

	for (m = graph->first_member[component];
	    m < graph->first_member[component + 1]; m++)
		search->reached[graph->members[m]] = false;

	search->reached[from] = true;
	search->queue[0] = from;
	queue_head = 0;
	queue_tail = 1;

	while (queue_head < queue_tail && !search->reached[to]) {
		vertex = search->queue[queue_head];
		queue_head++;

		for (e = graph->first_edge[vertex];
		    e < graph->first_edge[vertex + 1]; e++) {
			edge = &graph->edges[e];
			if (graph->component_of[edge->target] != component ||
			    search->reached[edge->target])
				continue;

			search->reached[edge->target] = true;
			search->arrival[edge->target] = e;
			search->queue[queue_tail] = edge->target;
			queue_tail++;
		}
	}

	length = 0;
	for (vertex = to; vertex != from;
	    vertex = graph->edges[search->arrival[vertex]].source)
		length++;

	e = length;
	for (vertex = to; vertex != from;
	    vertex = graph->edges[search->arrival[vertex]].source) {
		e--;
		search->path[e] = search->arrival[vertex];
	}

	return (length);
}

/*
 * Report one cycle through a strict edge: an error at the rule holding the
 * negated literal or the aggregate, and the notes for that edge and for the
 * shortest path from its target back to its source, which closes the cycle.
 */
static void
report_cycle(relic_analysis_t *analysis, const relic_dep_edge_t *closing)
{
	const relic_rule_t *rule;
	size_t length;

	rule = &analysis->rules->entries[closing->rule_index];
	length = shortest_path(analysis, closing->target, closing->source);

	relic_diag_error(analysis->ctx, RELIC_ERROR_REJECTED, &rule->where,
	    "program is not stratified");
	note_path(analysis, closing, length);
}

/*
 * Whether edge `a` was written before edge `b`: in an earlier rule, or
 * earlier in the body of the same one.
 */
static bool
edge_written_before(const relic_dep_edge_t *a, const relic_dep_edge_t *b)
{
	if (a->rule_index != b->rule_index)
		return (a->rule_index < b->rule_index);

	return (a->literal_index < b->literal_index);
}

/* The qsort(3) form of edge_written_before(). */
static int
compare_written_order(const void *left, const void *right)
{
	if (edge_written_before(left, right))
		return (-1);
	if (edge_written_before(right, left))
		return (1);

	return (0);
}

/*
 * Mark the group predicates among the graph's vertices: the targets of
 * aggregate edges.  Returns NULL when the scratch arena refused.
 */
static bool *
mark_group_vertices(relic_analysis_t *analysis)
{
	const relic_depgraph_t *graph;
	bool *group;
	size_t e;

	graph = &analysis->graph;

	group = relic_arena_allocarray(&analysis->scratch, graph->vertex_count,
	    sizeof(*group));
	if (group == NULL)
		return (NULL);
	bzero(group, graph->vertex_count * sizeof(*group));

	for (e = 0; e < graph->edge_count; e++) {
		if (graph->edges[e].kind == RELIC_DEP_AGGREGATE)
			group[graph->edges[e].target] = true;
	}

	return (group);
}

/*
 * List the components that recursion through a strict edge passes through,
 * each by its closing edge, in the order the program wrote those edges.
 * Stores the edges in *cyclesp and their number in *countp.  Returns -1
 * when the scratch arena refused.
 *
 * The closing edge is the first strict edge inside the component of a rule
 * the writer wrote.  A group rule's own negative edge is never chosen:
 * every cycle through a group predicate enters it by the aggregate edge
 * that reaches it, which is strict and belongs to a written rule, so a
 * component with a strict edge inside it always holds one of those.
 *
 * A strict edge whose source and target share a component lies on a cycle,
 * since the target reaches the source.  The edges are read from the graph,
 * the one definition of what a dependency is and which are strict, so the
 * rejection and the strata cannot disagree about an edge.  The graph groups
 * edges by source rather than by where they were written, so the first edge
 * of each component is found by comparing positions, and the handful found
 * are then sorted into the order of the text.  No two edges share a
 * position, so the order is total and the same on every run.
 */
static int
find_strict_cycles(relic_analysis_t *analysis, relic_dep_edge_t **cyclesp,
    size_t *countp)
{
	const relic_depgraph_t *graph;
	const relic_dep_edge_t *edge;
	relic_dep_edge_t *cycles;
	size_t *first_strict;
	bool *group;
	size_t count;
	size_t e;
	uint32_t c;
	uint32_t component;

	graph = &analysis->graph;

	first_strict = relic_arena_allocarray(&analysis->scratch,
	    graph->component_count, sizeof(*first_strict));
	cycles = relic_arena_allocarray(&analysis->scratch,
	    graph->component_count, sizeof(*cycles));
	group = mark_group_vertices(analysis);
	if (first_strict == NULL || cycles == NULL || group == NULL)
		return (-1);

	for (c = 0; c < graph->component_count; c++)
		first_strict[c] = RELIC_NO_EDGE;

	for (e = 0; e < graph->edge_count; e++) {
		edge = &graph->edges[e];
		component = graph->component_of[edge->source];
		if (!relic_dep_strict(edge) || group[edge->source] ||
		    graph->component_of[edge->target] != component)
			continue;

		if (first_strict[component] == RELIC_NO_EDGE ||
		    edge_written_before(edge,
		    &graph->edges[first_strict[component]]))
			first_strict[component] = e;
	}

	count = 0;
	for (c = 0; c < graph->component_count; c++) {
		if (first_strict[c] == RELIC_NO_EDGE)
			continue;

		cycles[count] = graph->edges[first_strict[c]];
		count++;
	}

	qsort(cycles, count, sizeof(*cycles), compare_written_order);

	*cyclesp = cycles;
	*countp = count;

	return (0);
}

/*
 * Report every component that recursion through a strict edge passes through,
 * once each, in the order the program wrote the edges that show it.  Every
 * allocation happens before the first report, so an analysis that runs out
 * of memory has said nothing about the program.
 */
static enum relic_analysis_result
report_strict_cycles(relic_analysis_t *analysis)
{
	relic_dep_edge_t *cycles;
	size_t count;
	size_t i;

	if (find_strict_cycles(analysis, &cycles, &count) != 0)
		return (RELIC_ANALYSIS_INCOMPLETE);

	if (count == 0)
		return (RELIC_ANALYSIS_STRATIFIED);

	if (allocate_cycle_search(analysis) != 0)
		return (RELIC_ANALYSIS_INCOMPLETE);

	for (i = 0; i < count; i++)
		report_cycle(analysis, &cycles[i]);

	return (RELIC_ANALYSIS_UNSTRATIFIED);
}

/*
 * Settle one component's stratum and whether it is recursive, from the edges
 * leaving its members.
 *
 * An edge that stays inside the component makes it recursive: some rule of
 * the component reads a member.  An edge that leaves it reaches a component
 * that completed earlier and is already settled, and asks for a stratum at
 * least that one's, or one above it when the edge is strict.  The stratum
 * is the least that satisfies every edge.  Edges inside the component are
 * all positive, because a program with a strict one was rejected before
 * strata were assigned.
 */
static void
settle_component(relic_analysis_t *analysis, uint32_t component)
{
	const relic_depgraph_t *graph;
	const relic_dep_edge_t *edge;
	size_t e;
	size_t m;
	uint32_t required;
	uint32_t stratum;
	uint32_t target_component;
	uint32_t vertex;
	bool recursive;

	graph = &analysis->graph;
	stratum = 0;
	recursive = false;

	for (m = graph->first_member[component];
	    m < graph->first_member[component + 1]; m++) {
		vertex = graph->members[m];

		for (e = graph->first_edge[vertex];
		    e < graph->first_edge[vertex + 1]; e++) {
			edge = &graph->edges[e];
			target_component = graph->component_of[edge->target];

			if (target_component == component) {
				recursive = true;
				continue;
			}

			required = analysis->strata[target_component];
			if (relic_dep_strict(edge))
				required++;

			if (required > stratum)
				stratum = required;
		}
	}

	analysis->strata[component] = stratum;
	analysis->recursive[component] = recursive;
}

/*
 * Settle every component, in completion order, so that each finds the
 * components its edges reach already settled.
 */
static int
assign_strata(relic_analysis_t *analysis)
{
	size_t count;
	uint32_t c;

	count = analysis->graph.component_count;

	analysis->strata = relic_arena_allocarray(&analysis->scratch, count,
	    sizeof(*analysis->strata));
	analysis->recursive = relic_arena_allocarray(&analysis->scratch, count,
	    sizeof(*analysis->recursive));
	if (analysis->strata == NULL || analysis->recursive == NULL)
		return (-1);

	for (c = 0; c < count; c++)
		settle_component(analysis, c);

	return (0);
}

/*
 * Count the rules each component holds the head of.
 */
static int
count_rules(relic_analysis_t *analysis)
{
	const relic_ruletab_t *rules;
	const relic_depgraph_t *graph;
	size_t r;
	uint32_t component;

	rules = analysis->rules;
	graph = &analysis->graph;

	analysis->rule_counts = relic_arena_allocarray(&analysis->scratch,
	    graph->component_count, sizeof(*analysis->rule_counts));
	if (analysis->rule_counts == NULL)
		return (-1);
	bzero(analysis->rule_counts,
	    graph->component_count * sizeof(*analysis->rule_counts));

	for (r = 0; r < rules->count; r++) {
		component = graph->component_of[
		    relic_depgraph_vertex(rules->entries[r].head)];
		analysis->rule_counts[component]++;
	}

	return (0);
}

/*
 * Put the components that hold rules in evaluation order: by stratum, and in
 * completion order within a stratum.  Stores the component numbers in
 * `order`, which has room for every component, and how many there are in
 * *countp.
 *
 * This is a counting sort.  `start[s + 1]` first counts the components in
 * stratum s; the running sum then makes `start[s]` where stratum s begins.
 * Placing the components in completion order, and advancing `start[s]` as
 * each one of stratum s is placed, is what makes the sort stable.
 */
static int
order_by_stratum(relic_analysis_t *analysis, uint32_t *order, size_t *countp)
{
	size_t *start;
	size_t count;
	size_t highest;
	size_t kept;
	size_t s;
	uint32_t c;

	count = analysis->graph.component_count;

	highest = 0;
	kept = 0;
	for (c = 0; c < count; c++) {
		if (analysis->rule_counts[c] == 0)
			continue;

		kept++;
		if (analysis->strata[c] > highest)
			highest = analysis->strata[c];
	}

	start = relic_arena_allocarray(&analysis->scratch, highest + 2,
	    sizeof(*start));
	if (start == NULL)
		return (-1);
	bzero(start, (highest + 2) * sizeof(*start));

	for (c = 0; c < count; c++) {
		if (analysis->rule_counts[c] > 0)
			start[analysis->strata[c] + 1]++;
	}

	for (s = 0; s <= highest; s++)
		start[s + 1] += start[s];

	for (c = 0; c < count; c++) {
		if (analysis->rule_counts[c] == 0)
			continue;

		order[start[analysis->strata[c]]] = c;
		start[analysis->strata[c]]++;
	}

	*countp = kept;

	return (0);
}

/*
 * Write the schedule's contents, which build_schedule() has allocated: the
 * components in `order`, their members, and every rule grouped by the
 * component holding its head.
 *
 * Laying out the components fixes where each one's rules begin.  The rules
 * are then placed by walking the table in order and appending each to its
 * component's stretch, which keeps table order within a component;
 * `next_rule`, indexed by component number, is where the next rule of each
 * component goes.
 */
static void
fill_schedule(const relic_analysis_t *analysis, const uint32_t *order,
    size_t count, size_t *next_rule, relic_schedule_t *schedule)
{
	const relic_depgraph_t *graph;
	const relic_ruletab_t *rules;
	relic_component_t *component;
	size_t member_cursor;
	size_t rule_cursor;
	size_t k;
	size_t m;
	size_t r;
	uint32_t c;

	graph = &analysis->graph;
	rules = analysis->rules;

	member_cursor = 0;
	rule_cursor = 0;

	for (k = 0; k < count; k++) {
		c = order[k];
		component = &schedule->components[k];

		component->stratum = analysis->strata[c];
		component->recursive = analysis->recursive[c];

		component->first_member = member_cursor;
		for (m = graph->first_member[c]; m < graph->first_member[c + 1];
		    m++) {
			schedule->members[member_cursor] =
			    relic_depgraph_pred(graph->members[m]);
			member_cursor++;
		}
		component->member_count = member_cursor -
		    component->first_member;

		component->first_rule = rule_cursor;
		component->rule_count = analysis->rule_counts[c];
		rule_cursor += component->rule_count;
		next_rule[c] = component->first_rule;
	}

	for (r = 0; r < rules->count; r++) {
		c = graph->component_of[relic_depgraph_vertex(
		    rules->entries[r].head)];
		schedule->rule_indexes[next_rule[c]] = r;
		next_rule[c]++;
	}

	schedule->component_count = count;
	schedule->member_count = member_cursor;
	schedule->rule_count = rules->count;
}

/*
 * The number of members of the `count` components listed in `order`.
 */
static size_t
count_members(const relic_depgraph_t *graph, const uint32_t *order,
    size_t count)
{
	size_t members;
	size_t k;

	members = 0;
	for (k = 0; k < count; k++)
		members += graph->first_member[order[k] + 1] -
		    graph->first_member[order[k]];

	return (members);
}

/*
 * Build the schedule into its own arena.  Returns 0, or -1 when an arena
 * refused; the caller reports it.
 */
static int
build_schedule(relic_analysis_t *analysis, relic_schedule_t *schedule)
{
	const relic_depgraph_t *graph;
	uint32_t *order;
	size_t *next_rule;
	size_t count;
	size_t members;

	graph = &analysis->graph;

	order = relic_arena_allocarray(&analysis->scratch,
	    graph->component_count, sizeof(*order));
	next_rule = relic_arena_allocarray(&analysis->scratch,
	    graph->component_count, sizeof(*next_rule));
	if (order == NULL || next_rule == NULL)
		return (-1);

	if (order_by_stratum(analysis, order, &count) != 0)
		return (-1);

	members = count_members(graph, order, count);

	schedule->components = relic_arena_allocarray(&schedule->arena, count,
	    sizeof(*schedule->components));
	schedule->members = relic_arena_allocarray(&schedule->arena, members,
	    sizeof(*schedule->members));
	schedule->rule_indexes = relic_arena_allocarray(&schedule->arena,
	    analysis->rules->count, sizeof(*schedule->rule_indexes));
	if (relic_arena_failed(&schedule->arena))
		return (-1);

	fill_schedule(analysis, order, count, next_rule, schedule);

	return (0);
}

/*
 * The whole analysis: the graph, the rejection, the strata and the schedule,
 * each stage run only when the one before it succeeded.
 */
static enum relic_analysis_result
analyse_program(relic_analysis_t *analysis, relic_schedule_t *schedule)
{
	enum relic_analysis_result result;

	if (relic_depgraph_build(analysis->ctx, analysis->rules,
	    &analysis->scratch, &analysis->graph) != 0)
		return (RELIC_ANALYSIS_INCOMPLETE);

	result = report_strict_cycles(analysis);
	if (result != RELIC_ANALYSIS_STRATIFIED)
		return (result);

	if (assign_strata(analysis) != 0)
		return (RELIC_ANALYSIS_INCOMPLETE);

	if (count_rules(analysis) != 0)
		return (RELIC_ANALYSIS_INCOMPLETE);

	if (build_schedule(analysis, schedule) != 0)
		return (RELIC_ANALYSIS_INCOMPLETE);

	return (RELIC_ANALYSIS_STRATIFIED);
}

/*
 * Replace the program's schedule with a finished one, taking over its arena.
 * Assigning the structure moves the arena, as lib/arena.h allows, and
 * *schedule is reinitialized rather than reset because its chunks now belong
 * to the program.
 */
static void
install_schedule(relic_program_t *program, relic_schedule_t *schedule)
{
	relic_schedule_reset(&program->schedule);
	program->schedule = *schedule;
	relic_schedule_init(schedule);
}

/*
 * Analyse a table of rules into a schedule, which must be freshly
 * initialized.  Returns RELIC_ANALYSIS_STRATIFIED with the schedule filled
 * in; RELIC_ANALYSIS_UNSTRATIFIED having reported every cycle through
 * negation; or RELIC_ANALYSIS_INCOMPLETE when memory ran out, which the caller
 * reports.  On either failure the caller resets the schedule, which may hold
 * part of what was being built.
 */
enum relic_analysis_result
relic_schedule_build(relic_t *ctx, const relic_ruletab_t *rules,
    relic_schedule_t *schedule)
{
	relic_analysis_t analysis;
	enum relic_analysis_result result;

	bzero(&analysis, sizeof(analysis));
	analysis.ctx = ctx;
	analysis.rules = rules;
	relic_arena_init(&analysis.scratch, RELIC_ARENA_ANY_OBJECT);

	result = analyse_program(&analysis, schedule);
	relic_arena_reset(&analysis.scratch);

	return (result);
}

/*
 * Bring the program's schedule up to date with its rules, rejecting the
 * program when it is not stratified.
 *
 * Returns 0 when the analysis finished, whatever it found: a program found
 * unstratified has been reported and marked invalid, and the check that
 * called this refuses it on that account.  Returns -1 when memory ran out;
 * that has been reported, the previous schedule is untouched and still not
 * current, and the program is not marked, so the next check tries again.
 *
 * A program once found unstratified is not analysed again.  Adding rules
 * only adds edges, so the cycle is still there, and reporting it a second
 * time would say nothing new.
 */
int
relic_stratify(relic_t *ctx)
{
	relic_program_t *program;
	relic_schedule_t schedule;
	enum relic_analysis_result result;

	program = &ctx->program;
	if (program->unstratified || relic_schedule_is_current(program))
		return (0);

	relic_schedule_init(&schedule);
	result = relic_schedule_build(ctx, &program->rules, &schedule);

	if (result == RELIC_ANALYSIS_STRATIFIED) {
		install_schedule(program, &schedule);
		return (0);
	}

	relic_schedule_reset(&schedule);

	if (result == RELIC_ANALYSIS_UNSTRATIFIED) {
		program->invalid = true;
		program->unstratified = true;
		return (0);
	}

	relic_diag_no_memory(ctx);

	return (-1);
}
