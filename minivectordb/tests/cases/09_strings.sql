CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(64), v VECTOR(2));
INSERT INTO t VALUES (1, 'zhongwen', '[1, 0]');
INSERT INTO t VALUES (2, 'ascii', '[0, 1]');
INSERT INTO t VALUES (3, '', '[1, 1]');
SELECT id, name FROM t ORDER BY name;
SELECT id FROM t WHERE name = 'zhongwen';
SELECT id FROM t WHERE name > 'a' ORDER BY id;
INSERT INTO t VALUES (4, 'this string is definitely longer than sixty four characters', '[0, 0]');
SELECT id FROM t ORDER BY id;