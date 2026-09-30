CREATE TABLE file (p TEXT);
.import file.tsv file

-- SQLite has no regular expressions without an extension; on this data
-- set the glob holds exactly the paths the other engines' pattern matches.
SELECT substr(p, 1, 7), substr(p, instr(p, '.') + 1), count(*)
FROM file
WHERE p NOT GLOB '*/test_[0-9]*.*'
GROUP BY 1, 2;
