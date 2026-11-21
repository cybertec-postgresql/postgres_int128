-- int128: SP-GiST index support
CREATE EXTENSION IF NOT EXISTS int128;

CREATE TABLE int128_spgist (v int128);
INSERT INTO int128_spgist
SELECT g::int128 * 1000000000000000000 FROM generate_series(1, 5000) g;
INSERT INTO int128_spgist VALUES (NULL);
CREATE INDEX int128_spgist_idx ON int128_spgist USING spgist (v);
ANALYZE int128_spgist;

SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_spgist WHERE v = '500000000000000000000'::int128;
SELECT * FROM int128_spgist WHERE v = '500000000000000000000'::int128;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_spgist WHERE v > '4990000000000000000000'::int128
ORDER BY v LIMIT 3;
SELECT * FROM int128_spgist WHERE v > '4990000000000000000000'::int128
ORDER BY v LIMIT 3;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM int128_spgist
WHERE v <> '500000000000000000000'::int128;
SELECT count(*) FROM int128_spgist
WHERE v <> '500000000000000000000'::int128;

RESET enable_seqscan;

--
-- Extreme values
--
CREATE TABLE int128_spgist_extreme (v int128);
INSERT INTO int128_spgist_extreme VALUES
    ('-170141183460469231731687303715884105728'),
    ('170141183460469231731687303715884105727'),
    (0), (-1), (1), (NULL);
CREATE INDEX int128_spgist_extreme_idx ON int128_spgist_extreme USING spgist (v);

SET enable_seqscan = off;
SELECT * FROM int128_spgist_extreme WHERE v = 0::int128;
SELECT * FROM int128_spgist_extreme WHERE v < 0::int128 ORDER BY v;
SELECT * FROM int128_spgist_extreme WHERE v > 0::int128 ORDER BY v;
RESET enable_seqscan;

--
-- All-same tuples (exercises the allTheSame split path)
--
CREATE TABLE int128_spgist_same (v int128);
INSERT INTO int128_spgist_same SELECT 5::int128 FROM generate_series(1, 1000);
CREATE INDEX int128_spgist_same_idx ON int128_spgist_same USING spgist (v);

SET enable_seqscan = off;
SELECT count(*) FROM int128_spgist_same WHERE v = 5::int128;
INSERT INTO int128_spgist_same VALUES (4), (6);
SELECT count(*) FROM int128_spgist_same WHERE v < 5::int128;
SELECT count(*) FROM int128_spgist_same WHERE v = 5::int128;
SELECT count(*) FROM int128_spgist_same WHERE v > 5::int128;
RESET enable_seqscan;

DROP TABLE int128_spgist, int128_spgist_extreme, int128_spgist_same;
