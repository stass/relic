CREATE TABLE edge (x VARCHAR, y VARCHAR);
COPY edge FROM 'edge.tsv' (DELIMITER '\t', HEADER false);

SELECT x, y FROM edge;
