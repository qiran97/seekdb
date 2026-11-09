CREATE TABLE docs (
    id BIGINT PRIMARY KEY,
    category VARCHAR(32),
    content VARCHAR(1024),
    embedding VECTOR(4)
);
INSERT INTO docs VALUES
    (1, 'database', 'transaction', '[1, 0, 0, 0]'),
    (2, 'database', 'index', '[0, 1, 0, 0]'),
    (3, 'travel', 'guide', '[0, 0, 1, 0]'),
    (4, 'database', 'recovery', '[0, 0, 0, 1]');
SELECT id, category, content FROM docs ORDER BY id;
SELECT * FROM docs WHERE category = 'database' ORDER BY id;
SELECT id FROM docs WHERE id >= 3 ORDER BY id;
SELECT id, content FROM docs WHERE category = 'travel' OR id = 1 ORDER BY id;
UPDATE docs SET content = 'changed' WHERE id = 2;
SELECT id, content FROM docs ORDER BY id;
DELETE FROM docs WHERE id = 4;
SELECT id FROM docs ORDER BY id;