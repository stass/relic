CREATE TABLE "row" (id VARCHAR, value VARCHAR, weight BIGINT);
COPY "row" FROM 'row.tsv' (DELIMITER '\t', HEADER false);

SELECT DISTINCT a.id, b.id
FROM "row" AS a
JOIN "row" AS b ON a.value = b.value
WHERE a.weight = 7 AND b.weight = 13;
