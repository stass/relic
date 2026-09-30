CREATE TABLE edge (x VARCHAR, y VARCHAR);
COPY edge FROM 'edge.tsv' (DELIMITER '\t', HEADER false);

WITH node(x) AS (
	SELECT x FROM edge UNION SELECT y FROM edge
),
outs(x, n) AS (
	SELECT x, count(*) FROM edge GROUP BY x
),
ins(x, n) AS (
	SELECT y, count(*) FROM edge GROUP BY y
)
SELECT node.x, coalesce(outs.n, 0), coalesce(ins.n, 0)
FROM node LEFT JOIN outs ON outs.x = node.x LEFT JOIN ins ON ins.x = node.x;
