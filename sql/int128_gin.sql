-- int128: GIN index support
CREATE EXTENSION IF NOT EXISTS int128;

CREATE TABLE int128_gin (v int128);
INSERT INTO int128_gin
SELECT g::int128 * 1000000000000000000 FROM generate_series(1, 5000) g;
INSERT INTO int128_gin VALUES (NULL);
CREATE INDEX int128_gin_idx ON int128_gin USING gin (v);
ANALYZE int128_gin;

SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_gin WHERE v = '500000000000000000000'::int128;
SELECT * FROM int128_gin WHERE v = '500000000000000000000'::int128;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM int128_gin WHERE v > '4990000000000000000000'::int128;
SELECT count(*) FROM int128_gin WHERE v > '4990000000000000000000'::int128;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM int128_gin
WHERE v = ANY(ARRAY['1000000000000000000'::int128,
                    '2000000000000000000'::int128]);
SELECT count(*) FROM int128_gin
WHERE v = ANY(ARRAY['1000000000000000000'::int128,
                    '2000000000000000000'::int128]);

RESET enable_seqscan;

--
-- Extreme values
--
CREATE TABLE int128_gin_extreme (v int128);
INSERT INTO int128_gin_extreme VALUES
    ('-170141183460469231731687303715884105728'),
    ('170141183460469231731687303715884105727'),
    (0), (-1), (1), (NULL);
CREATE INDEX int128_gin_extreme_idx ON int128_gin_extreme USING gin (v);

SET enable_seqscan = off;
SELECT * FROM int128_gin_extreme WHERE v = 0::int128;
SELECT * FROM int128_gin_extreme WHERE v < 0::int128 ORDER BY v;
SELECT * FROM int128_gin_extreme WHERE v > 0::int128 ORDER BY v;
RESET enable_seqscan;

--
-- Pending list (fastupdate)
--
CREATE TABLE int128_gin_pending (v int128);
INSERT INTO int128_gin_pending SELECT g::int128 FROM generate_series(1, 1000) g;
CREATE INDEX int128_gin_pending_idx ON int128_gin_pending USING gin (v)
WITH (fastupdate = on);
INSERT INTO int128_gin_pending SELECT g::int128 FROM generate_series(1001, 2000) g;

SET enable_seqscan = off;
SELECT count(*) FROM int128_gin_pending WHERE v = 1500::int128;
SELECT count(*) FROM int128_gin_pending WHERE v > 1990::int128;
RESET enable_seqscan;

SELECT gin_clean_pending_list('int128_gin_pending_idx') > 0 AS cleaned;

SET enable_seqscan = off;
SELECT count(*) FROM int128_gin_pending WHERE v = 1500::int128;
RESET enable_seqscan;

--
-- Multicolumn GIN with an array column
--
CREATE TABLE int128_gin_multi (v int128, tags int[]);
INSERT INTO int128_gin_multi
SELECT g::int128, ARRAY[g % 10] FROM generate_series(1, 1000) g;
CREATE INDEX int128_gin_multi_idx ON int128_gin_multi USING gin (v, tags);

SET enable_seqscan = off;
EXPLAIN (COSTS OFF)
SELECT * FROM int128_gin_multi
WHERE v = 500::int128 AND tags @> ARRAY[0];
SELECT count(*) FROM int128_gin_multi
WHERE v = 500::int128 AND tags @> ARRAY[0];
RESET enable_seqscan;

DROP TABLE int128_gin, int128_gin_extreme, int128_gin_pending,
           int128_gin_multi;
