-- A table that is created before the crash has to survive the restart, the
-- data definition and the rows of the table are stored in the catalog and in
-- the pages of the tree.
CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(4));
INSERT INTO t VALUES (1, 'alpha', '[1, 0, 0, 0]');
CREATE TABLE u (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(4));
INSERT INTO u VALUES (7, 'beta', '[0, 1, 0, 0]');
-- !kill
SELECT id, category FROM u ORDER BY id;
SELECT id, category FROM t ORDER BY id;
SHOW TABLES;