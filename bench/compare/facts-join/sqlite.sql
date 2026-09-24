CREATE TABLE "row" (id TEXT, value TEXT, weight INTEGER);
.import row.tsv row

SELECT DISTINCT a.id, b.id
FROM "row" AS a
JOIN "row" AS b ON a.value = b.value
WHERE a.weight = 7 AND b.weight = 13;
