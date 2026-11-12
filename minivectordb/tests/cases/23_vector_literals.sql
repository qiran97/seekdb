-- The vector literal is parsed for every row that is written, the parser
-- has to reject the malformed literals with the same messages as before.
CREATE TABLE v (id BIGINT PRIMARY KEY, vec VECTOR(2));
INSERT INTO v VALUES (1, '[1, 2]');
INSERT INTO v VALUES (2, '[ 1 , 2 ]');
INSERT INTO v VALUES (3, '[]');
INSERT INTO v VALUES (4, '[1, ]');
INSERT INTO v VALUES (5, '[1,2,]');
INSERT INTO v VALUES (6, '[1,,2]');
INSERT INTO v VALUES (7, '[1,2');
INSERT INTO v VALUES (8, '[a, 1]');
INSERT INTO v VALUES (9, '[1.5x, 2]');
INSERT INTO v VALUES (10, '[1.0 2.0]');
INSERT INTO v VALUES (11, '[1e400, 0]');
INSERT INTO v VALUES (12, '[-1.5, 2.5]');
INSERT INTO v VALUES (13, '[1.5, -2.5]');
INSERT INTO v VALUES (14, '[0x10, 1]');
SELECT id, vec FROM v ORDER BY id;