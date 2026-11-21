-- int128: btree, hash and BRIN index support
CREATE EXTENSION IF NOT EXISTS int128;

--
-- btree
--
CREATE TABLE int128_btree (v int128);
INSERT INTO int128_btree
SELECT g::int128 * 1000000000000000000 FROM generate_series(1, 1000) g;
CREATE INDEX int128_btree_idx ON int128_btree (v);
ANALYZE int128_btree;

SET enable_seqscan = off;
SET enable_bitmapscan = off;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_btree WHERE v = '500000000000000000000'::int128;
SELECT * FROM int128_btree WHERE v = '500000000000000000000'::int128;

RESET enable_bitmapscan;

EXPLAIN (COSTS OFF)
SELECT * FROM int128_btree WHERE v > '999000000000000000000'::int128
ORDER BY v LIMIT 3;
SELECT * FROM int128_btree WHERE v > '999000000000000000000'::int128
ORDER BY v LIMIT 3;

RESET enable_seqscan;

SELECT min(v), max(v) FROM int128_btree;
SELECT v FROM int128_btree ORDER BY v LIMIT 3;
SELECT v FROM int128_btree ORDER BY v DESC LIMIT 3;

-- unique index
CREATE UNIQUE INDEX int128_btree_uniq ON int128_btree (v);
INSERT INTO int128_btree VALUES ('500000000000000000000'::int128);
DROP INDEX int128_btree_uniq;

-- primary key / foreign key
CREATE TABLE int128_pk (k int128 PRIMARY KEY);
CREATE TABLE int128_fk (k int128 REFERENCES int128_pk(k));
INSERT INTO int128_pk VALUES (1), (2);
INSERT INTO int128_fk VALUES (1);
INSERT INTO int128_fk VALUES (99);
SELECT * FROM int128_fk ORDER BY k;
DROP TABLE int128_fk, int128_pk;

--
-- hash
--
CREATE TABLE int128_hash (v int128);
INSERT INTO int128_hash SELECT g::int128 FROM generate_series(1, 1000) g;
CREATE INDEX int128_hash_idx ON int128_hash USING hash (v);

SET enable_seqscan = off;
SET enable_bitmapscan = off;

EXPLAIN (COSTS OFF) SELECT * FROM int128_hash WHERE v = 500::int128;
SELECT * FROM int128_hash WHERE v = 500::int128;

RESET enable_bitmapscan;
RESET enable_seqscan;

--
-- BRIN minmax
--
CREATE TABLE int128_brin (v int128);
INSERT INTO int128_brin
SELECT g::int128 * 1000000000000000000 FROM generate_series(1, 20000) g;
CREATE INDEX int128_brin_idx ON int128_brin USING brin (v)
WITH (pages_per_range = 2);
ANALYZE int128_brin;

SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM int128_brin
WHERE v BETWEEN '100000000000000000000'::int128
            AND '200000000000000000000'::int128;
SELECT count(*) FROM int128_brin
WHERE v BETWEEN '100000000000000000000'::int128
            AND '200000000000000000000'::int128;

RESET enable_seqscan;

--
-- BRIN minmax-multi
--
CREATE TABLE int128_brin_multi (v int128);
INSERT INTO int128_brin_multi
SELECT g::int128 * 1000000000000000000 FROM generate_series(1, 20000) g;
CREATE INDEX int128_brin_multi_idx ON int128_brin_multi
USING brin (v int128_minmax_multi_ops) WITH (pages_per_range = 2);
ANALYZE int128_brin_multi;

SET enable_seqscan = off;

EXPLAIN (COSTS OFF)
SELECT count(*) FROM int128_brin_multi
WHERE v BETWEEN '100000000000000000000'::int128
            AND '200000000000000000000'::int128;
SELECT count(*) FROM int128_brin_multi
WHERE v BETWEEN '100000000000000000000'::int128
            AND '200000000000000000000'::int128;

RESET enable_seqscan;

--
-- Joins
--
CREATE TABLE int128_j1 (v int128);
CREATE TABLE int128_j2 (v int128);
INSERT INTO int128_j1 SELECT g::int128 FROM generate_series(1, 1000) g;
INSERT INTO int128_j2 SELECT (g * 2)::int128 FROM generate_series(1, 1000) g;

SET enable_mergejoin = off;
SET enable_nestloop = off;
EXPLAIN (COSTS OFF) SELECT count(*) FROM int128_j1 JOIN int128_j2 USING (v);
SELECT count(*) FROM int128_j1 JOIN int128_j2 USING (v);

SET enable_hashjoin = off;
SET enable_mergejoin = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM int128_j1 JOIN int128_j2 USING (v);
SELECT count(*) FROM int128_j1 JOIN int128_j2 USING (v);

RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_nestloop;

--
-- Partitioning
--
CREATE TABLE int128_range (k int128, payload text) PARTITION BY RANGE (k);
CREATE TABLE int128_range_lo PARTITION OF int128_range
FOR VALUES FROM ('-170141183460469231731687303715884105728') TO (0);
CREATE TABLE int128_range_hi PARTITION OF int128_range
FOR VALUES FROM (0) TO ('170141183460469231731687303715884105727');
INSERT INTO int128_range VALUES (-5, 'neg'), (0, 'zero'), (5, 'pos');
SELECT tableoid::regclass, k, payload FROM int128_range ORDER BY k;

CREATE TABLE int128_hashpart (k int128, payload text) PARTITION BY HASH (k);
CREATE TABLE int128_hashpart_0 PARTITION OF int128_hashpart
FOR VALUES WITH (MODULUS 2, REMAINDER 0);
CREATE TABLE int128_hashpart_1 PARTITION OF int128_hashpart
FOR VALUES WITH (MODULUS 2, REMAINDER 1);
INSERT INTO int128_hashpart SELECT g::int128, 'x' FROM generate_series(1, 10) g;
SELECT count(*) FROM int128_hashpart;

--
-- Cleanup
--
DROP TABLE int128_btree, int128_hash, int128_brin, int128_brin_multi;
DROP TABLE int128_j1, int128_j2;
DROP TABLE int128_range, int128_hashpart;
