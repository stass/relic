/*
 * Copyright (c) 2026 Stanislav Sedov
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RELIC_DEPGRAPH_H
#define RELIC_DEPGRAPH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "arena.h"
#include "program.h"

/*
 * The predicate dependency graph and its strongly connected components.
 *
 * A vertex is a predicate, numbered by its position in the predicate table:
 * its identifier less one.  relic_depgraph_vertex() and relic_depgraph_pred()
 * are the only places that translation is written.
 *
 * An edge runs from a rule's head to the predicate of one of its relational
 * body literals, and says the head depends on it.  Edges are grouped by
 * source: the edges leaving vertex v are `edges[first_edge[v]]` up to but not
 * including `edges[first_edge[v + 1]]`, in rule order and then in literal
 * order, which is the order the program was written in.
 *
 * Components are numbered in the order Tarjan's algorithm completes them.  A
 * component completes only after every component it can reach, so that
 * numbering puts every component after everything it depends on, and an
 * edge that leaves a component always reaches a lower-numbered one.  The
 * members of component c are `members[first_member[c]]` up to but not
 * including `members[first_member[c + 1]]`, in vertex order.
 *
 * Everything is allocated from the arena the graph was built in.
 */

typedef struct relic_dep_edge relic_dep_edge_t;
typedef struct relic_depgraph relic_depgraph_t;

/*
 * One dependency, and the literal that created it: body literal
 * `literal_index` of program rule `rule_index`.  The strata and components do
 * not depend on them; they say where the program wrote the edge, which is
 * what a diagnostic points at and what orders its reports.
 */
struct relic_dep_edge {
	size_t		rule_index;
	size_t		literal_index;
	uint32_t	source;
	uint32_t	target;
	bool		negative;
};

struct relic_depgraph {
	relic_dep_edge_t	*edges;
	size_t			*first_edge;	/* vertex_count + 1 */
	uint32_t		*component_of;	/* by vertex */
	uint32_t		*members;	/* vertices, by component */
	size_t			*first_member;	/* component_count + 1 */
	size_t			 vertex_count;
	size_t			 edge_count;
	size_t			 component_count;
};

static inline uint32_t
relic_depgraph_vertex(relic_pred_id_t pred)
{
	return (pred - 1);
}

static inline relic_pred_id_t
relic_depgraph_pred(uint32_t vertex)
{
	return (vertex + 1);
}

/*
 * Build the graph of the program's rules over every predicate in the table,
 * and find its components.  Returns 0, or -1 when `arena` refused an
 * allocation; the arena is then marked failed and the caller reports it.
 */
int	 relic_depgraph_build(const relic_t *ctx, relic_arena_t *arena,
	    relic_depgraph_t *graph);

#endif /* RELIC_DEPGRAPH_H */
