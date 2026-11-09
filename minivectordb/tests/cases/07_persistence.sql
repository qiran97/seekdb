CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(16), v VECTOR(2));
INSERT INTO t VALUES (1, 'one', '[1, 0]');
-- !stop
SELECT id, name FROM t ORDER BY id;
INSERT INTO t VALUES (2, 'two', '[0, 1]');
SELECT id FROM t ORDER BY id;