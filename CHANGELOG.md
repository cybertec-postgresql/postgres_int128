# Changelog

## 1.0 — 2026-11-21

Initial release.

- Native 128-bit signed integer type for PostgreSQL 17, 18 and 19.
- Checked arithmetic, bitwise operators, text and binary I/O.
- Casts to and from `smallint`, `integer`, `bigint`, `numeric`, `real` and
  `double precision`.
- btree, hash, GiST, SP-GiST, GIN and BRIN operator classes.
- GiST and SP-GiST KNN ordering; btree skip scan on PostgreSQL 18 and later.
- `sum`, `avg`, `min`, `max`, `stddev_samp`, `stddev_pop`, `var_samp`,
  `var_pop`, `bit_and`, `bit_or` and `bit_xor` aggregates.
- Mathematical functions, `generate_series`, `width_bucket`, `random` and
  `to_char`.
- `int128range` and `int128multirange` range types.
- pg_regress suite with seven test files.
