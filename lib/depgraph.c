/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include <stdint.h>
#include <strings.h>

#include "depgraph.h"
#include "internal.h"

/*
 * The predicate dependency graph, and its strongly connected components by
 * Tarjan's algorithm.
 *
 * The graph is built in two passes over the rules: the first counts the edges
 * leaving each predicate, which fixes where each predicate's edges begin, and
 * the second places every edge.  Placing in rule and literal order keeps each
 * predicate's edges in the order the program wrote them.
 *
 * Tarjan's algorithm is a depth-first search that numbers vertices in the
 * order it discovers them, and tracks for each vertex on the current search
 * the lowest discovery number it can reach without leaving the vertices not
 * yet assigned to a component.  When the search finishes a vertex whose
 * lowest reachable number is its own, nothing below it on the search reaches
 * higher up, so that vertex and everything discovered after it and still
 * unassigned form one component, which is complete.  A component is
 * therefore completed only after every component reachable from it, which is
 * the order evaluation needs: dependencies first.
 *
 * The search is iterative.  A recursive search descends once per vertex of
 * the longest path, which a program controls, and a generated program with a
 * long dependency chain would overflow the C stack.  Here the search path is
 * an array of frames, each a vertex and the position of the next edge it has
 * to follow, so descending and backtracking push and pop an array and the C
 * stack stays flat.
 */

/* A vertex the search has not reached, and one no component holds yet. */
#define	RELIC_UNDISCOVERED	UINT32_MAX
#define	RELIC_UNASSIGNED	UINT32_MAX

typedef struct relic_search relic_search_t;
typedef struct relic_search_frame relic_search_frame_t;

/*
 * A vertex on the search path, and the next of its edges to follow.
 */
struct relic_search_frame {
	size_t		next_edge;
	uint32_t	vertex;
};

/*
 * Tarjan's search in progress.
 *
 * `discovery` numbers each vertex in the order it was reached, and `low` is
 * the lowest discovery number the vertex is known to reach among vertices not
 * yet assigned a component.  `path` is the search path, deepest last.
 * `pending` holds the vertices discovered and not yet assigned, in discovery
 * order; a component is always a suffix of it.
 *
 * A vertex is on `pending` exactly when it has been discovered and not
 * assigned, so no separate on-stack flag is kept: that is what
 * vertex_is_pending() computes.
 */
struct relic_search {
	relic_depgraph_t	*graph;
	uint32_t		*discovery;
	uint32_t		*low;
	relic_search_frame_t	*path;
	uint32_t		*pending;
	size_t			 path_depth;
	size_t			 pending_count;
	uint32_t		 discovered;
};

static void	 assign_component(relic_search_t *search, uint32_t root);
static int	 build_edges(const relic_ruletab_t *rules, relic_arena_t *arena,
		    relic_depgraph_t *graph);
static void	 descend(relic_search_t *search, uint32_t vertex);
static int	 find_components(relic_arena_t *arena, relic_depgraph_t *graph);
static int	 group_members(relic_arena_t *arena, relic_depgraph_t *graph);
static bool	 literal_makes_edge(const relic_literal_t *literal);
static void	 search_from(relic_search_t *search, uint32_t root);
static bool	 vertex_is_pending(const relic_search_t *search,
		    uint32_t vertex);

/*
 * A relational literal, positive or negated, reads a relation and so makes an
 * edge; a comparison reads only values.
 */
static bool
literal_makes_edge(const relic_literal_t *literal)
{
	return (literal->kind == RELIC_LITERAL_POSITIVE ||
	    literal->kind == RELIC_LITERAL_NEGATED);
}

/*
 * Build the edges: count those leaving each vertex, turn the counts into
 * offsets, then place each edge at the next free position of its source.
 *
 * After the counting pass, first_edge[v + 1] holds the number of edges
 * leaving v; the running sum turns that into where v's edges end, which is
 * where v + 1's begin.  The placing pass uses `next` as a cursor per vertex,
 * starting at first_edge[v], and leaves it at first_edge[v + 1].
 */
static int
build_edges(const relic_ruletab_t *rules, relic_arena_t *arena,
    relic_depgraph_t *graph)
{
	const relic_rule_t *rule;
	const relic_literal_t *literal;
	relic_dep_edge_t *edge;
	size_t *next;
	size_t literal_index;
	size_t rule_index;
	size_t v;
	uint32_t source;

	graph->first_edge = relic_arena_allocarray(arena,
	    graph->vertex_count + 1, sizeof(*graph->first_edge));
	next = relic_arena_allocarray(arena, graph->vertex_count,
	    sizeof(*next));
	if (graph->first_edge == NULL || next == NULL)
		return (-1);
	bzero(graph->first_edge,
	    (graph->vertex_count + 1) * sizeof(*graph->first_edge));

	for (rule_index = 0; rule_index < rules->count; rule_index++) {
		rule = &rules->entries[rule_index];
		source = relic_depgraph_vertex(rule->head);

		for (literal_index = 0; literal_index < rule->body_length;
		    literal_index++) {
			if (literal_makes_edge(&rule->body[literal_index]))
				graph->first_edge[source + 1]++;
		}
	}

	for (v = 0; v < graph->vertex_count; v++) {
		graph->first_edge[v + 1] += graph->first_edge[v];
		next[v] = graph->first_edge[v];
	}
	graph->edge_count = graph->first_edge[graph->vertex_count];

	graph->edges = relic_arena_allocarray(arena, graph->edge_count,
	    sizeof(*graph->edges));
	if (graph->edges == NULL)
		return (-1);

	for (rule_index = 0; rule_index < rules->count; rule_index++) {
		rule = &rules->entries[rule_index];
		source = relic_depgraph_vertex(rule->head);

		for (literal_index = 0; literal_index < rule->body_length;
		    literal_index++) {
			literal = &rule->body[literal_index];
			if (!literal_makes_edge(literal))
				continue;

			edge = &graph->edges[next[source]];
			next[source]++;

			edge->rule_index = rule_index;
			edge->literal_index = literal_index;
			edge->source = source;
			edge->target =
			    relic_depgraph_vertex(literal->u.atom.pred);
			edge->negative = literal->kind == RELIC_LITERAL_NEGATED;
		}
	}

	return (0);
}

static bool
vertex_is_pending(const relic_search_t *search, uint32_t vertex)
{
	return (search->discovery[vertex] != RELIC_UNDISCOVERED &&
	    search->graph->component_of[vertex] == RELIC_UNASSIGNED);
}

/*
 * Reach a vertex: number it, put it on the pending list, and push a frame
 * that will follow its edges from the first.
 */
static void
descend(relic_search_t *search, uint32_t vertex)
{
	relic_search_frame_t *frame;

	search->discovery[vertex] = search->discovered;
	search->low[vertex] = search->discovered;
	search->discovered++;

	search->pending[search->pending_count] = vertex;
	search->pending_count++;

	frame = &search->path[search->path_depth];
	search->path_depth++;
	frame->vertex = vertex;
	frame->next_edge = search->graph->first_edge[vertex];
}

/*
 * The search has finished `root` and found that it reaches nothing pending
 * above it: `root` and every vertex pending after it form a component.
 */
static void
assign_component(relic_search_t *search, uint32_t root)
{
	relic_depgraph_t *graph;
	uint32_t component;
	uint32_t vertex;

	graph = search->graph;
	component = (uint32_t)graph->component_count;
	graph->component_count++;

	do {
		search->pending_count--;
		vertex = search->pending[search->pending_count];
		graph->component_of[vertex] = component;
	} while (vertex != root);
}

/*
 * Search from one undiscovered vertex until everything it reaches has been
 * assigned a component.
 *
 * Each round looks at the deepest frame.  While it has an edge left to
 * follow, the edge either reaches a new vertex, which the search descends
 * into, or a pending one, which lowers the frame's low number: the target is
 * part of a component still being gathered, and so is this vertex.  An edge
 * to an assigned vertex leads into a finished component and says nothing
 * about this one.
 *
 * A frame with no edges left is finished.  It closes a component when it
 * reaches nothing pending that was discovered before it, and otherwise
 * passes its low number up to the frame below, which reaches everything it
 * does.
 */
static void
search_from(relic_search_t *search, uint32_t root)
{
	const relic_depgraph_t *graph;
	relic_search_frame_t *frame;
	uint32_t parent;
	uint32_t target;
	uint32_t vertex;

	graph = search->graph;
	descend(search, root);

	while (search->path_depth > 0) {
		frame = &search->path[search->path_depth - 1];
		vertex = frame->vertex;

		if (frame->next_edge < graph->first_edge[vertex + 1]) {
			target = graph->edges[frame->next_edge].target;
			frame->next_edge++;

			if (search->discovery[target] == RELIC_UNDISCOVERED)
				descend(search, target);
			else if (vertex_is_pending(search, target) &&
			    search->discovery[target] < search->low[vertex])
				search->low[vertex] = search->discovery[target];
			continue;
		}

		search->path_depth--;

		if (search->low[vertex] == search->discovery[vertex])
			assign_component(search, vertex);

		if (search->path_depth == 0)
			continue;

		parent = search->path[search->path_depth - 1].vertex;
		if (search->low[vertex] < search->low[parent])
			search->low[parent] = search->low[vertex];
	}
}

/*
 * Assign every vertex a component, starting a search from each vertex not
 * yet reached, in vertex order.
 */
static int
find_components(relic_arena_t *arena, relic_depgraph_t *graph)
{
	relic_search_t search;
	size_t count;
	uint32_t v;

	count = graph->vertex_count;

	bzero(&search, sizeof(search));
	search.graph = graph;
	search.discovery = relic_arena_allocarray(arena, count,
	    sizeof(*search.discovery));
	search.low = relic_arena_allocarray(arena, count, sizeof(*search.low));
	search.path = relic_arena_allocarray(arena, count,
	    sizeof(*search.path));
	search.pending = relic_arena_allocarray(arena, count,
	    sizeof(*search.pending));
	graph->component_of = relic_arena_allocarray(arena, count,
	    sizeof(*graph->component_of));
	if (relic_arena_failed(arena))
		return (-1);

	for (v = 0; v < count; v++) {
		search.discovery[v] = RELIC_UNDISCOVERED;
		graph->component_of[v] = RELIC_UNASSIGNED;
	}

	for (v = 0; v < count; v++) {
		if (search.discovery[v] == RELIC_UNDISCOVERED)
			search_from(&search, v);
	}

	return (0);
}

/*
 * List each component's members, in vertex order: count the members of each
 * component, turn the counts into offsets, and place every vertex at the
 * next free position of its component.
 */
static int
group_members(relic_arena_t *arena, relic_depgraph_t *graph)
{
	size_t *next;
	size_t c;
	uint32_t component;
	uint32_t v;

	graph->first_member = relic_arena_allocarray(arena,
	    graph->component_count + 1, sizeof(*graph->first_member));
	graph->members = relic_arena_allocarray(arena, graph->vertex_count,
	    sizeof(*graph->members));
	next = relic_arena_allocarray(arena, graph->component_count,
	    sizeof(*next));
	if (relic_arena_failed(arena))
		return (-1);
	bzero(graph->first_member,
	    (graph->component_count + 1) * sizeof(*graph->first_member));

	for (v = 0; v < graph->vertex_count; v++)
		graph->first_member[graph->component_of[v] + 1]++;

	for (c = 0; c < graph->component_count; c++) {
		graph->first_member[c + 1] += graph->first_member[c];
		next[c] = graph->first_member[c];
	}

	for (v = 0; v < graph->vertex_count; v++) {
		component = graph->component_of[v];
		graph->members[next[component]] = v;
		next[component]++;
	}

	return (0);
}

int
relic_depgraph_build(const relic_t *ctx, const relic_ruletab_t *rules,
    relic_arena_t *arena, relic_depgraph_t *graph)
{
	bzero(graph, sizeof(*graph));
	graph->vertex_count = relic_pred_count(ctx);

	if (build_edges(rules, arena, graph) != 0)
		return (-1);

	if (find_components(arena, graph) != 0)
		return (-1);

	return (group_members(arena, graph));
}
