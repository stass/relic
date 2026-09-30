CREATE TABLE file (p VARCHAR);
COPY file FROM 'file.tsv' (DELIMITER '\t', HEADER false, QUOTE '');

SELECT substr(p, 1, 7), substr(p, strpos(p, '.') + 1), count(*)
FROM file
WHERE NOT regexp_matches(p, '/test_[0-9]+[.]')
GROUP BY 1, 2;
