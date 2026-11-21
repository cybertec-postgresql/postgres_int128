# int128 — a 128-bit integer type for PostgreSQL

`int128` adds a native 128-bit signed integer type to PostgreSQL 17, 18 and 19.
It stores values from `-170141183460469231731687303715884105728` to
`170141183460469231731687303715884105727`. It is implemented in C on top of
the compiler's `__int128` type, with checked arithmetic, full text and binary
I/O, casts to and from the standard numeric types, and operator classes for
the btree, hash, GiST, SP-GiST, GIN and BRIN access methods.

The type is intended for identifiers, counters, monetary amounts in minor
units, hashes, and any other value that outgrows `bigint` but does not need
the arbitrary precision (and per-value overhead) of `numeric`.

```sql
CREATE EXTENSION int128;

CREATE TABLE ledger (
    id      int128 PRIMARY KEY,
    amount  int128 NOT NULL
);

INSERT INTO ledger VALUES
    (1, 170141183460469231731687303715884105727),
    (2, -170141183460469231731687303715884105728);

SELECT sum(amount) FROM ledger;
```

## Contents

- [Features](#features)
- [Requirements](#requirements)
- [Installation](#installation)
- [The type](#the-type)
- [Value range](#value-range)
- [Operators](#operators)
- [Functions](#functions)
- [Mathematical functions](#mathematical-functions)
- [Casts](#casts)
- [Index support](#index-support)
- [Aggregates](#aggregates)
- [Window functions](#window-functions)
- [Range types](#range-types)
- [Performance notes](#performance-notes)
- [Limitations](#limitations)
- [Regression tests](#regression-tests)
- [License](#license)

## Features

- Signed 128-bit integer with the range
  `-170141183460469231731687303715884105728` to
  `170141183460469231731687303715884105727`.
- Overflow-checked arithmetic: `+`, `-`, `*`, `/`, `%`, unary `-`, `abs()`,
  `gcd()` and `lcm()` raise `int128 out of range` instead of wrapping.
- Bitwise operators `&`, `|`, `#` (XOR), `~`, `<<` and `>>`.
- Text and binary I/O, so `COPY ... WITH (FORMAT binary)` and client
  binary protocols work.
- Implicit casts from `smallint`, `integer` and `bigint`; assignment casts
  from `numeric`, `real` and `double precision`; implicit casts to `numeric`,
  `real` and `double precision`; range-checked assignment casts to the
  smaller integer types.
- btree, hash, GiST, SP-GiST, GIN and BRIN operator classes, including BRIN
  `minmax` and `minmax-multi`.
- GiST support for exclusion constraints and index-only scans.
- SP-GiST support using a balanced binary search tree over the 128-bit
  value.
- GIN support for equality, `= ANY(array)` and multicolumn GIN indexes.
- `sum()`, `avg()`, `min()` and `max()` aggregates, all parallel safe.
- Mathematical functions: rounding, powers, roots, logarithms,
  trigonometry, `bit_count()`, `width_bucket()`, `random()` and `to_char()`.
- Statistical aggregates: `stddev_samp()`, `stddev_pop()`, `var_samp()` and
  `var_pop()`.
- `generate_series()`, `scale()`, `min_scale()`, `trim_scale()`,
  `int128_inc()`, `int128_dec()` and `pg_size_pretty()`.
- `int128range` and `int128multirange` range types, including GiST-indexed
  range columns.
- GiST KNN ordering through the `<->` operator.
- Sort support, btree deduplication and `in_range` support for window
  functions.
- Works as a primary key, unique key, foreign key, hash/range partition key,
  hash join key and merge join key.

## Requirements

- PostgreSQL 17, 18 or 19.
- A 64-bit platform with a compiler that provides native 128-bit integers
  (`__int128`). GCC and Clang on x86-64 and AArch64 qualify. The build fails
  with a clear error if the compiler does not support the type.
- PostgreSQL server development headers (`postgresql-server-dev-*` or a
  source/`pg_config` installation).

## Installation

Build and install with PGXS:

```sh
make PG_CONFIG=/path/to/pg_config
make install PG_CONFIG=/path/to/pg_config
```

Then, in each database that needs the type:

```sql
CREATE EXTENSION int128;
```

The extension is relocatable, so it can be installed into any schema:

```sql
CREATE SCHEMA types;
CREATE EXTENSION int128 SCHEMA types;
```

## The type

| Property | Value |
| --- | --- |
| SQL name | `int128` |
| Range | `-170141183460469231731687303715884105728` to `170141183460469231731687303715884105727` |
| Internal length | 16 bytes, fixed |
| Passed by value | no |
| Alignment | `double` (8 bytes) |
| Storage | `plain` (never toasted) |
| Category | `N` (numeric) |
| Typmod | none |

## Value range

| | Value |
| --- | --- |
| **Minimum** | `-170141183460469231731687303715884105728` |
| **Maximum** | `170141183460469231731687303715884105727` |
| Bits | 128, signed two's complement |
| Decimal digits | 39 (maximum) |

The minimum is `-2^127`; the maximum is `2^127 - 1`. Values outside this
range are rejected on input, and arithmetic that would leave the range raises
`int128 out of range`:

```sql
SELECT '170141183460469231731687303715884105727'::int128;   -- maximum
SELECT '-170141183460469231731687303715884105728'::int128;  -- minimum
SELECT '170141183460469231731687303715884105728'::int128;   -- error
```

Values are written as ordinary base-10 integers with an optional sign.
Leading and trailing whitespace is accepted, matching `bigint` input:

```sql
SELECT '42'::int128, '  -42  '::int128, '+7'::int128;
```

Out-of-range input is rejected:

```sql
SELECT '170141183460469231731687303715884105728'::int128;
ERROR:  value "170141183460469231731687303715884105728" is out of range for type int128
```

Because the type is 16 bytes wide, PostgreSQL stores a pointer to the value
in the tuple rather than the value itself. This is inherent: `Datum` is one
machine word, and PostgreSQL only allows `typlen` 1, 2, 4 or 8 for
pass-by-value types.

## Operators

| Operator | Left | Right | Result | Description |
| --- | --- | --- | --- | --- |
| `=` | `int128` | `int128` | `boolean` | equal |
| `<>` | `int128` | `int128` | `boolean` | not equal |
| `<` | `int128` | `int128` | `boolean` | less than |
| `<=` | `int128` | `int128` | `boolean` | less than or equal |
| `>` | `int128` | `int128` | `boolean` | greater than |
| `>=` | `int128` | `int128` | `boolean` | greater than or equal |
| `+` | `int128` | `int128` | `int128` | addition |
| `-` | `int128` | `int128` | `int128` | subtraction |
| `*` | `int128` | `int128` | `int128` | multiplication |
| `/` | `int128` | `int128` | `int128` | division, truncating toward zero |
| `%` | `int128` | `int128` | `int128` | remainder |
| `-` | — | `int128` | `int128` | negation |
| `@` | — | `int128` | `int128` | absolute value |
| `&` | `int128` | `int128` | `int128` | bitwise AND |
| `\|` | `int128` | `int128` | `int128` | bitwise OR |
| `#` | `int128` | `int128` | `int128` | bitwise XOR |
| `~` | — | `int128` | `int128` | bitwise NOT |
| `<<` | `int128` | `integer` | `int128` | left shift |
| `>>` | `int128` | `integer` | `int128` | arithmetic right shift |

The comparison operators carry `HASHES` and `MERGES`, so hash joins and merge
joins are available. Selectivity is estimated with the standard scalar
selectivity functions.

Shift semantics are fully defined, unlike the platform-dependent behaviour of
out-of-range shifts on `bigint`:

- A negative shift count reverses the direction (`x << -3` is `x >> 3`).
- A left shift of 128 or more bits yields `0`.
- A right shift of 128 or more bits yields `0` for non-negative values and
  `-1` for negative values.

## Functions

| Function | Result | Description |
| --- | --- | --- |
| `abs(int128)` | `int128` | absolute value |
| `div(int128, int128)` | `int128` | truncating division |
| `mod(int128, int128)` | `int128` | remainder |
| `gcd(int128, int128)` | `int128` | greatest common divisor |
| `lcm(int128, int128)` | `int128` | least common multiple |
| `int128_cmp(int128, int128)` | `integer` | btree comparator (-1, 0, 1) |
| `int128_hash(int128)` | `integer` | hash support function |
| `int128_hash_extended(int128, bigint)` | `bigint` | seeded hash support function |
| `int128_send(int128)` | `bytea` | binary output |
| `int128_recv(internal)` | `int128` | binary input |
| `int128_in(cstring)` | `int128` | text input |
| `int128_out(int128)` | `cstring` | text output |
| `int128_sortsupport(internal)` | `void` | btree sort support |
| `int128_in_range(int128, int128, int128, boolean, boolean)` | `boolean` | window frame support |
| `int128range_subdiff(int128, int128)` | `double precision` | `subtype_diff` for range types |
| `generate_series(int128, int128)` | `setof int128` | inclusive series |
| `generate_series(int128, int128, int128)` | `setof int128` | inclusive series with step |
| `scale(int128)` | `integer` | always 0 |
| `min_scale(int128)` | `integer` | always 0 |
| `trim_scale(int128)` | `int128` | identity |
| `int128_inc(int128)` | `int128` | add one |
| `int128_dec(int128)` | `int128` | subtract one |
| `pg_size_pretty(int128)` | `text` | human-readable size |
| `int128_dist(int128, int128)` | `int128` | absolute difference, used by `<->` |

`gcd()` and `lcm()` follow the same conventions as their `bigint`
counterparts: `gcd(x, 0) = abs(x)`, `gcd(0, 0) = 0`, and `lcm(x, 0) = 0`.
Results that cannot be represented raise `int128 out of range`.

## Mathematical functions

All functions are `IMMUTABLE` and `PARALLEL SAFE` except `random()`, which is
`VOLATILE PARALLEL RESTRICTED`. Algebraic functions return `numeric` for
exactness; transcendental functions return `double precision`, matching
PostgreSQL's `numeric` and `float8` conventions.

| Function | Returns | Notes |
| --- | --- | --- |
| `round(int128)` | `int128` | identity |
| `round(int128, int)` | `int128` | rounds to `10^-n` for negative `n`; half away from zero |
| `trunc(int128)` | `int128` | identity |
| `trunc(int128, int)` | `int128` | truncates to `10^-n` for negative `n` |
| `ceil(int128)` | `int128` | identity |
| `ceiling(int128)` | `int128` | identity |
| `floor(int128)` | `int128` | identity |
| `sign(int128)` | `int128` | -1, 0 or 1 |
| `power(int128, int128)` | `numeric` | |
| `pow(int128, int128)` | `numeric` | alias for `power` |
| `sqrt(int128)` | `numeric` | |
| `cbrt(int128)` | `float8` | |
| `exp(int128)` | `numeric` | |
| `ln(int128)` | `numeric` | |
| `log(int128)` | `numeric` | base 10 |
| `log10(int128)` | `numeric` | base 10 |
| `log(int128, int128)` | `numeric` | `log(base, value)` |
| `factorial(int128)` | `numeric` | errors for negative input |
| `sin(int128)` | `float8` | |
| `cos(int128)` | `float8` | |
| `tan(int128)` | `float8` | |
| `cot(int128)` | `float8` | |
| `asin(int128)` | `float8` | |
| `acos(int128)` | `float8` | |
| `atan(int128)` | `float8` | |
| `atan2(int128, int128)` | `float8` | `atan2(y, x)` |
| `sinh(int128)` | `float8` | |
| `cosh(int128)` | `float8` | |
| `tanh(int128)` | `float8` | |
| `asinh(int128)` | `float8` | |
| `acosh(int128)` | `float8` | |
| `atanh(int128)` | `float8` | |
| `degrees(int128)` | `float8` | |
| `radians(int128)` | `float8` | |
| `bit_count(int128)` | `int4` | population count |
| `width_bucket(int128, int128, int128, int)` | `int4` | |
| `random(int128, int128)` | `int128` | uniform in `[min, max]` |
| `to_char(int128, text)` | `text` | delegates to `to_char(numeric, text)` |

```sql
SELECT round(12345::int128, -2), sqrt(144::int128), cos(0::int128);
 round | sqrt | cos
-------+------+-----
 12300 |   12 |   1
```

## Casts

| From | To | Context | Notes |
| --- | --- | --- | --- |
| `smallint` | `int128` | implicit | lossless |
| `integer` | `int128` | implicit | lossless |
| `bigint` | `int128` | implicit | lossless |
| `numeric` | `int128` | assignment | rounds half away from zero; range checked |
| `real` | `int128` | assignment | uses `rint()`; range checked |
| `double precision` | `int128` | assignment | uses `rint()`; range checked |
| `int128` | `smallint` | assignment | range checked |
| `int128` | `integer` | assignment | range checked |
| `int128` | `bigint` | assignment | range checked |
| `int128` | `numeric` | implicit | exact |
| `int128` | `real` | implicit | may lose precision |
| `int128` | `double precision` | implicit | may lose precision |
| `text` | `int128` | explicit (I/O) | parses the text representation |
| `int128` | `text` | explicit (I/O) | formats the value |

The rounding rules match PostgreSQL's existing `bigint` casts:
`numeric` rounds half away from zero (`2.5` becomes `3`), while floating
point uses `rint()` and therefore round-half-to-even (`2.5` becomes `2`).

```sql
SELECT 2.5::numeric::int128, 2.5::float8::int128;
 int128 | int128
--------+--------
      3 |      2
```

## Index support

### btree — `int128_ops` (default)

Supports `=`, `<>`, `<`, `<=`, `>`, `>=`, `ORDER BY`, `DISTINCT`, `GROUP BY`,
`IN`, `BETWEEN`, unique constraints, primary keys, foreign keys, range
partitioning, merge joins and the `min()`/`max()` index optimisation.

```sql
CREATE INDEX ledger_id_idx ON ledger (id);
SELECT * FROM ledger WHERE id = '500000000000000000000'::int128;
```

The operator class provides sort support (avoiding per-comparison fmgr
overhead), `in_range` support for `RANGE` window frames, and
`btequalimage` so btree deduplication can be used.

### hash — `int128_ops` (default)

Supports equality lookups, hash joins, hash aggregation and hash
partitioning.

```sql
CREATE INDEX ledger_id_hash ON ledger USING hash (id);
```

### GiST — `int128_gist_ops` (default)

The GiST operator class stores a bounding box `[lower, upper]` per subtree,
so it supports the same comparison operators as btree, plus exclusion
constraints and index-only scans (through the `fetch` support function).

```sql
CREATE INDEX ledger_id_gist ON ledger USING gist (id);

-- exclusion constraint: no two rows may share an id
ALTER TABLE ledger ADD CONSTRAINT ledger_id_excl
    EXCLUDE USING gist (id WITH =);
```

GiST is useful when you need an exclusion constraint over `int128`, or when
you want a single index type that can be combined with other GiST-indexed
columns in a multicolumn exclusion constraint. For plain lookups and range
scans, btree is smaller and faster.

KNN ordering (`ORDER BY id <-> value`) is supported through the `<->`
operator and the `int128_gist_distance()` support function. The distance is
returned as `double precision`, so ordering is approximate for values beyond
2^53.

### SP-GiST — `int128_spgist_ops` (default)

The SP-GiST operator class is a one-dimensional binary search tree. Each
inner tuple stores a pivot (the median of the values being split) and has up
to two nodes: values below the pivot and values at or above it. This gives
logarithmic search for equality and range predicates, and avoids the page
splits of a btree when values arrive in random order.

```sql
CREATE INDEX ledger_id_spgist ON ledger USING spgist (id);
```

SP-GiST is a good choice for read-mostly tables with random insert order.
It does not support exclusion constraints or index-only scans.

### GIN — `int128_gin_ops` (default)

The GIN operator class stores each value as a single key, so equality
lookups and `= ANY(array)` are exact. Range operators use a partial-match
scan over the ordered key space; this is correct but usually slower than
btree, so GIN is mainly useful for multicolumn GIN indexes that mix
`int128` with array or `jsonb` columns.

```sql
CREATE INDEX ledger_id_gin ON ledger USING gin (id);

-- multicolumn GIN: int128 plus an array column
CREATE INDEX ledger_gin ON ledger USING gin (id, tags);
```

GIN does not support `<>`; use btree, GiST or SP-GiST for that.

### BRIN — `int128_minmax_ops` (default) and `int128_minmax_multi_ops`

Both BRIN operator classes store 16-byte values directly. `minmax` keeps one
minimum and one maximum per range; `minmax-multi` keeps a bounded set of
values per range and is more robust when the data is not physically sorted.

```sql
CREATE INDEX ledger_id_brin ON ledger USING brin (id)
    WITH (pages_per_range = 32);

CREATE INDEX ledger_id_brin_multi ON ledger
    USING brin (id int128_minmax_multi_ops)
    WITH (pages_per_range = 32);
```

BRIN is a good fit for append-only tables with naturally increasing keys,
where it is orders of magnitude smaller than a btree index.

### Not provided

KNN ordering is supported only by GiST. SP-GiST does not provide a `<->`
operator or distance support function.

## Aggregates

| Aggregate | Result | Notes |
| --- | --- | --- |
| `sum(int128)` | `int128` | overflow checked; parallel safe |
| `avg(int128)` | `numeric` | exact; parallel safe |
| `min(int128)` | `int128` | uses the btree opclass; parallel safe |
| `max(int128)` | `int128` | uses the btree opclass; parallel safe |
| `stddev_samp(int128)` | `numeric` | sample standard deviation; parallel safe |
| `stddev_pop(int128)` | `numeric` | population standard deviation; parallel safe |
| `var_samp(int128)` | `numeric` | sample variance; parallel safe |
| `var_pop(int128)` | `numeric` | population variance; parallel safe |

`sum()` updates its transition value in place when running as an aggregate,
so it does not allocate one value per input row. `avg()` accumulates an exact
128-bit sum and a row count, then divides as `numeric`, so the result is
exact even for values beyond the range of `double precision`.

```sql
SELECT sum(amount), avg(amount), min(amount), max(amount) FROM ledger;
```

## Window functions

The btree operator class provides `in_range` support, so `RANGE` frames with
`int128` offsets work:

```sql
SELECT id,
       count(*) OVER (ORDER BY id RANGE BETWEEN 100 PRECEDING AND CURRENT ROW)
FROM ledger;
```

## Range types

The extension provides `int128range` and `int128multirange`, built on
`int128range_subdiff()`:

```sql
SELECT '[1,10)'::int128range @> 5::int128;
SELECT '{[1,10)}'::int128multirange;

CREATE TABLE windows (span int128range);
CREATE INDEX windows_span_idx ON windows USING gist (span);

SELECT * FROM windows WHERE span && '[100,200)'::int128range;
```

## Performance notes

- Arithmetic uses native `__int128` operations. Overflow is detected with
  `__builtin_add_overflow`, `__builtin_sub_overflow` and
  `__builtin_mul_overflow`, which compile to a single flag-setting
  instruction rather than a branchy pre-check.
- Values are loaded and stored with `memcpy()`. PostgreSQL has no 16-byte
  `typalign`, so a value in a tuple may be only 8-byte aligned; casting the
  pointer to `int128 *` would let the compiler emit aligned 16-byte loads
  that fault on strict-alignment targets. The `memcpy()` compiles to two
  64-bit moves.
- The btree comparator and sort support function avoid fmgr call overhead
  during sorting.
- `sum()` mutates its transition value in place.
- Text output formats directly into a stack buffer; no `snprintf` with
  `long long` pairs is involved.
- The type is pass-by-reference, so each value costs a pointer indirection
  and 16 bytes of storage. If your values fit in `bigint`, `bigint` remains
  faster and smaller.

## Limitations

- No typmod support (`int128(10)` is not a thing).
- GiST KNN ordering uses `double precision` distances, so it is approximate
  for values beyond 2^53.
- GIN range scans are correct but not selective; prefer btree for range
  predicates. GIN does not support `<>`.
- Integer literals larger than `bigint` are typed as `numeric` by the
  parser. A comparison such as `WHERE id = 500000000000000000000` therefore
  becomes a `numeric` comparison and cannot use an `int128` index. Cast the
  literal explicitly:

  ```sql
  SELECT * FROM ledger WHERE id = '500000000000000000000'::int128;
  ```

  This is the same behaviour PostgreSQL has for `bigint` columns compared
  with out-of-range literals.
- Other `numeric` formatting functions (for example `to_number()`) require
  an explicit cast to `numeric`.
- `to_jsonb()` and `row_to_json()` render `int128` values as JSON strings,
  because PostgreSQL's JSON categorisation only recognises the built-in
  numeric types. Cast to `numeric` for a JSON number:
  `to_jsonb(v::numeric)`.

## Regression tests

The extension ships with a pg_regress suite covering I/O, arithmetic,
overflow, bitwise operations, comparisons, hashing, binary output, casts,
aggregates, window frames, range types, type integration, opclass
validation, mathematical functions, and the btree, hash, GiST, SP-GiST, GIN
and BRIN operator classes. The suite is split into seven files:

| File | Coverage |
| --- | --- |
| `int128` | I/O, arithmetic, bitwise, comparisons, hashing, binary output, aggregates, window frames, type integration, opclass validation, range types |
| `int128_casts` | Every cast in both directions, rounding and range errors |
| `int128_index` | btree, hash, BRIN minmax and minmax-multi, joins, primary/foreign keys, partitioning |
| `int128_gist` | GiST scans, index-only scans, KNN ordering, exclusion constraints, extreme values |
| `int128_spgist` | SP-GiST scans, extreme values, all-same split path |
| `int128_gin` | GIN equality, ranges, `= ANY`, pending list, multicolumn GIN |
| `int128_math` | Rounding, powers, roots, logs, trigonometry, `bit_count`, `width_bucket`, `random`, `to_char`, `generate_series`, `scale`, `int128_inc`/`dec`, `pg_size_pretty`, `int128_dist`, statistical aggregates |

With a running server and the extension installed:

```sh
make installcheck PG_CONFIG=/path/to/pg_config
```

The suite creates and drops its own tables in the `contrib_regression`
database.

## License

This extension is distributed under the **PostgreSQL License**, the same
license as PostgreSQL itself. The full text is in the `LICENSE` file.

---

Implemented by **Hans-Jürgen Schönig**, <hs@cybertec.at>

**CYBERTEC PostgreSQL International GmbH** — <https://www.cybertec-postgresql.com>
