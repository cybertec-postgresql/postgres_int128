/*-------------------------------------------------------------------------
 *
 * int128.c
 *	  A 128-bit signed integer type for PostgreSQL.
 *
 * The type is a fixed-length, pass-by-reference 16-byte value.  Arithmetic
 * is performed on the compiler's native 128-bit integer type and checked
 * for overflow with the __builtin_*_overflow primitives, which compile to
 * a single flag-setting instruction on all supported platforms.
 *
 * Because PostgreSQL has no 16-byte typalign, values may be only 8-byte
 * aligned in tuples.  Loading and storing through memcpy() keeps the code
 * correct on strict-alignment targets while still compiling to two 64-bit
 * moves; casting the pointer to int128 * would let the compiler emit
 * aligned 16-byte loads.
 *
 * Copyright (c) 2026, CYBERTEC PostgreSQL International GmbH
 *
 * Distributed under the PostgreSQL License; see LICENSE.
 *
 * IDENTIFICATION
 *	  src/int128.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/gist.h"
#include "access/spgist.h"
#include "access/stratnum.h"
#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "fmgr.h"
#include "funcapi.h"
#include "libpq/pqformat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/sortsupport.h"
#if PG_VERSION_NUM >= 180000
#include "utils/skipsupport.h"
#endif
#include "varatt.h"

PG_MODULE_MAGIC;

#if PG_VERSION_NUM < 170000
#error "int128 requires PostgreSQL 17 or later"
#endif

#ifndef HAVE_INT128
#error "int128 requires a compiler with native 128-bit integer support"
#endif

#define PG_INT128_MAX	((int128) ((((uint128) 1) << 127) - 1))
#define PG_INT128_MIN	(-PG_INT128_MAX - 1)

/* 2^127 as a double; the exact upper bound for float8 -> int128 */
#define INT128_FLOAT_LIMIT	0x1p127

#define INT128_OVERFLOW() \
	ereport(ERROR, \
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), \
			 errmsg("int128 out of range")))

/*
 * Load/store helpers.  See the header comment for why memcpy() is used.
 */
static inline int128
int128_get(Datum d)
{
	int128		v;

	memcpy(&v, DatumGetPointer(d), sizeof(v));
	return v;
}

static inline Datum
int128_make(int128 v)
{
	int128	   *p = (int128 *) palloc(sizeof(int128));

	memcpy(p, &v, sizeof(v));
	return PointerGetDatum(p);
}

static inline int128
uint128_as_int128(uint128 u)
{
	int128		v;

	memcpy(&v, &u, sizeof(v));
	return v;
}

static inline int
int128_cmp_internal(int128 a, int128 b)
{
	return (a > b) - (a < b);
}

/*
 * Parse a base-10 integer, accepting the same leading/trailing whitespace
 * and sign as int8in().  Overflow is detected before it happens.
 */
static int128
int128_parse(const char *str)
{
	const char *p = str;
	bool		neg = false;
	bool		any = false;
	uint128		acc = 0;
	uint128		limit;

	while (isspace((unsigned char) *p))
		p++;

	if (*p == '+' || *p == '-')
	{
		neg = (*p == '-');
		p++;
	}

	limit = neg ? ((uint128) PG_INT128_MAX + 1) : (uint128) PG_INT128_MAX;

	while (*p >= '0' && *p <= '9')
	{
		unsigned int digit = (unsigned int) (*p - '0');

		if (acc > (limit - digit) / 10)
			goto overflow;
		acc = acc * 10 + digit;
		any = true;
		p++;
	}

	while (isspace((unsigned char) *p))
		p++;

	if (!any || *p != '\0')
		goto invalid;

	if (neg)
	{
		if (acc == (uint128) PG_INT128_MAX + 1)
			return PG_INT128_MIN;
		return -(int128) acc;
	}
	return (int128) acc;

overflow:
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type int128", str)));
	return 0;					/* keep compiler quiet */

invalid:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type int128: \"%s\"", str)));
	return 0;					/* keep compiler quiet */
}

/*
 * Format a value in base 10.  buf must have room for 41 bytes.
 * Returns the length, excluding the trailing NUL.
 */
static int
int128_format(int128 v, char *buf)
{
	char		tmp[48];
	int			n = 0;
	int			i = 0;
	uint128		u;
	bool		neg = (v < 0);

	if (neg)
		u = (uint128) 0 - (uint128) v;
	else
		u = (uint128) v;

	do
	{
		tmp[i++] = (char) ('0' + (int) (u % 10));
		u /= 10;
	} while (u != 0);

	if (neg)
		buf[n++] = '-';
	while (i > 0)
		buf[n++] = tmp[--i];
	buf[n] = '\0';

	return n;
}

/*----------------------------------------------------------
 * Input/output
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_in);
Datum
int128_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);

	PG_RETURN_DATUM(int128_make(int128_parse(str)));
}

PG_FUNCTION_INFO_V1(int128_out);
Datum
int128_out(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	char		buf[48];
	int			len = int128_format(v, buf);
	char	   *result = (char *) palloc(len + 1);

	memcpy(result, buf, len + 1);
	PG_RETURN_CSTRING(result);
}

PG_FUNCTION_INFO_V1(int128_recv);
Datum
int128_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	uint64		hi = (uint64) pq_getmsgint64(buf);
	uint64		lo = (uint64) pq_getmsgint64(buf);
	uint128		u = (((uint128) hi) << 64) | (uint128) lo;

	PG_RETURN_DATUM(int128_make(uint128_as_int128(u)));
}

PG_FUNCTION_INFO_V1(int128_send);
Datum
int128_send(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, (int64) (((uint128) v) >> 64));
	pq_sendint64(&buf, (int64) (uint64) v);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*----------------------------------------------------------
 * Comparison operators
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_eq);
Datum
int128_eq(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(int128_get(PG_GETARG_DATUM(0)) ==
				   int128_get(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(int128_ne);
Datum
int128_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(int128_get(PG_GETARG_DATUM(0)) !=
				   int128_get(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(int128_lt);
Datum
int128_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(int128_get(PG_GETARG_DATUM(0)) <
				   int128_get(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(int128_le);
Datum
int128_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(int128_get(PG_GETARG_DATUM(0)) <=
				   int128_get(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(int128_gt);
Datum
int128_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(int128_get(PG_GETARG_DATUM(0)) >
				   int128_get(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(int128_ge);
Datum
int128_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(int128_get(PG_GETARG_DATUM(0)) >=
				   int128_get(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(int128_cmp);
Datum
int128_cmp(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(int128_cmp_internal(int128_get(PG_GETARG_DATUM(0)),
										int128_get(PG_GETARG_DATUM(1))));
}

/*----------------------------------------------------------
 * Arithmetic operators
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_pl);
Datum
int128_pl(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));
	int128		r;

	if (unlikely(__builtin_add_overflow(a, b, &r)))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(r));
}

PG_FUNCTION_INFO_V1(int128_mi);
Datum
int128_mi(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));
	int128		r;

	if (unlikely(__builtin_sub_overflow(a, b, &r)))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(r));
}

PG_FUNCTION_INFO_V1(int128_mul);
Datum
int128_mul(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));
	int128		r;

	if (unlikely(__builtin_mul_overflow(a, b, &r)))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(r));
}

PG_FUNCTION_INFO_V1(int128_div);
Datum
int128_div(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));

	if (unlikely(b == 0))
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/*
	 * INT128_MIN / -1 cannot be represented.  Division by -1 is negation,
	 * so handle it without performing the division.
	 */
	if (unlikely(b == -1))
	{
		if (unlikely(a == PG_INT128_MIN))
			INT128_OVERFLOW();
		PG_RETURN_DATUM(int128_make(-a));
	}

	PG_RETURN_DATUM(int128_make(a / b));
}

PG_FUNCTION_INFO_V1(int128_mod);
Datum
int128_mod(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));

	if (unlikely(b == 0))
		ereport(ERROR,
				(errcode(ERRCODE_DIVISION_BY_ZERO),
				 errmsg("division by zero")));

	/* INT128_MIN % -1 is well-defined as zero, but traps on some CPUs */
	if (unlikely(b == -1))
		PG_RETURN_DATUM(int128_make(0));

	PG_RETURN_DATUM(int128_make(a % b));
}

PG_FUNCTION_INFO_V1(int128_um);
Datum
int128_um(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));

	if (unlikely(a == PG_INT128_MIN))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(-a));
}

PG_FUNCTION_INFO_V1(int128_abs);
Datum
int128_abs(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));

	if (unlikely(a == PG_INT128_MIN))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(a < 0 ? -a : a));
}

/*----------------------------------------------------------
 * Greatest common divisor / least common multiple
 *---------------------------------------------------------*/

static int128
int128_gcd_internal(int128 arg1, int128 arg2)
{
	int128		swap;
	int128		a1;
	int128		a2;

	/*
	 * Work in negative space so that INT128_MIN has a representable absolute
	 * value, and put the greater absolute value in arg1.
	 */
	a1 = (arg1 < 0) ? arg1 : -arg1;
	a2 = (arg2 < 0) ? arg2 : -arg2;
	if (a1 > a2)
	{
		swap = arg1;
		arg1 = arg2;
		arg2 = swap;
	}

	if (arg1 == PG_INT128_MIN)
	{
		if (arg2 == 0 || arg2 == PG_INT128_MIN)
			INT128_OVERFLOW();
		if (arg2 == -1)
			return 1;
	}

	while (arg2 != 0)
	{
		swap = arg2;
		arg2 = arg1 % arg2;
		arg1 = swap;
	}

	if (arg1 < 0)
		arg1 = -arg1;

	return arg1;
}

PG_FUNCTION_INFO_V1(int128_gcd);
Datum
int128_gcd(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(int128_make(int128_gcd_internal(a, b)));
}

PG_FUNCTION_INFO_V1(int128_lcm);
Datum
int128_lcm(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));
	int128		gcd;
	int128		result;

	if (a == 0 || b == 0)
		PG_RETURN_DATUM(int128_make(0));

	gcd = int128_gcd_internal(a, b);
	a = a / gcd;

	if (unlikely(__builtin_mul_overflow(a, b, &result)))
		INT128_OVERFLOW();
	if (unlikely(result == PG_INT128_MIN))
		INT128_OVERFLOW();
	if (result < 0)
		result = -result;

	PG_RETURN_DATUM(int128_make(result));
}

/*----------------------------------------------------------
 * Bitwise operators
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_and);
Datum
int128_and(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make(int128_get(PG_GETARG_DATUM(0)) &
								int128_get(PG_GETARG_DATUM(1))));
}

PG_FUNCTION_INFO_V1(int128_or);
Datum
int128_or(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make(int128_get(PG_GETARG_DATUM(0)) |
								int128_get(PG_GETARG_DATUM(1))));
}

PG_FUNCTION_INFO_V1(int128_xor);
Datum
int128_xor(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make(int128_get(PG_GETARG_DATUM(0)) ^
								int128_get(PG_GETARG_DATUM(1))));
}

PG_FUNCTION_INFO_V1(int128_not);
Datum
int128_not(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make(~int128_get(PG_GETARG_DATUM(0))));
}

/*
 * Arithmetic right shift.  Implemented explicitly rather than relying on
 * the implementation-defined behaviour of >> on negative values.
 */
static int128
int128_shr_value(int128 v, int32 shift)
{
	uint128		u;

	if (shift <= 0)
		return v;
	if (shift >= 128)
		return (v < 0) ? -1 : 0;

	u = ((uint128) v) >> shift;
	if (v < 0)
		u |= (~(uint128) 0) << (128 - shift);

	return uint128_as_int128(u);
}

PG_FUNCTION_INFO_V1(int128_shl);
Datum
int128_shl(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	int32		shift = PG_GETARG_INT32(1);

	if (shift < 0)
	{
		if (shift == PG_INT32_MIN)
			PG_RETURN_DATUM(int128_make(v < 0 ? -1 : 0));
		PG_RETURN_DATUM(int128_make(int128_shr_value(v, -shift)));
	}

	if (shift >= 128)
		PG_RETURN_DATUM(int128_make(0));

	PG_RETURN_DATUM(int128_make(uint128_as_int128(((uint128) v) << shift)));
}

PG_FUNCTION_INFO_V1(int128_shr);
Datum
int128_shr(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	int32		shift = PG_GETARG_INT32(1);

	if (shift < 0)
	{
		if (shift == PG_INT32_MIN)
			PG_RETURN_DATUM(int128_make(0));
		shift = -shift;
		if (shift >= 128)
			PG_RETURN_DATUM(int128_make(0));
		PG_RETURN_DATUM(int128_make(uint128_as_int128(((uint128) v) << shift)));
	}

	PG_RETURN_DATUM(int128_make(int128_shr_value(v, shift)));
}

/*----------------------------------------------------------
 * Hashing
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_hash);
Datum
int128_hash(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));

	return hash_any((const unsigned char *) &v, sizeof(v));
}

PG_FUNCTION_INFO_V1(int128_hash_extended);
Datum
int128_hash_extended(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	uint64		seed = (uint64) PG_GETARG_INT64(1);

	return hash_any_extended((const unsigned char *) &v, sizeof(v), seed);
}

/*----------------------------------------------------------
 * Sort support
 *---------------------------------------------------------*/

static int
int128_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	return int128_cmp_internal(int128_get(x), int128_get(y));
}

PG_FUNCTION_INFO_V1(int128_sortsupport);
Datum
int128_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = int128_fast_cmp;
	PG_RETURN_VOID();
}

/*----------------------------------------------------------
 * in_range support for window functions
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_in_range);
Datum
int128_in_range(PG_FUNCTION_ARGS)
{
	int128		val = int128_get(PG_GETARG_DATUM(0));
	int128		base = int128_get(PG_GETARG_DATUM(1));
	int128		offset = int128_get(PG_GETARG_DATUM(2));
	bool		sub = PG_GETARG_BOOL(3);
	bool		less = PG_GETARG_BOOL(4);
	int128		sum;

	if (unlikely(offset < 0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
				 errmsg("invalid preceding or following size in window function")));

	if (sub)
		offset = -offset;		/* offset >= 0, so this cannot overflow */

	if (unlikely(__builtin_add_overflow(base, offset, &sum)))
	{
		/*
		 * If sub is false, the true sum is surely more than val, so the
		 * answer is the same as "less".  If sub is true, the true sum is
		 * surely less than val, so the answer is "!less".
		 */
		PG_RETURN_BOOL(sub ? !less : less);
	}

	if (less)
		PG_RETURN_BOOL(val <= sum);
	else
		PG_RETURN_BOOL(val >= sum);
}

/*----------------------------------------------------------
 * Casts from other types
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_from_int2);
Datum
int128_from_int2(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make((int128) PG_GETARG_INT16(0)));
}

PG_FUNCTION_INFO_V1(int128_from_int4);
Datum
int128_from_int4(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make((int128) PG_GETARG_INT32(0)));
}

PG_FUNCTION_INFO_V1(int128_from_int8);
Datum
int128_from_int8(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make((int128) PG_GETARG_INT64(0)));
}

/*
 * Convert a numeric to int128, rounding half away from zero exactly like
 * numeric -> int8 does.
 */
static int128
numeric_to_int128_internal(Numeric num)
{
	Numeric		rounded;
	char	   *str;
	int128		result;

	if (numeric_is_nan(num))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("cannot convert NaN to type int128")));
	if (numeric_is_inf(num))
		INT128_OVERFLOW();

	rounded = DatumGetNumeric(DirectFunctionCall2(numeric_round,
												  NumericGetDatum(num),
												  Int32GetDatum(0)));
	str = DatumGetCString(DirectFunctionCall1(numeric_out,
											  NumericGetDatum(rounded)));
	result = int128_parse(str);
	pfree(str);

	return result;
}

PG_FUNCTION_INFO_V1(int128_from_numeric);
Datum
int128_from_numeric(PG_FUNCTION_ARGS)
{
	Numeric		num = PG_GETARG_NUMERIC(0);

	PG_RETURN_DATUM(int128_make(numeric_to_int128_internal(num)));
}

static int128
float8_to_int128_internal(float8 f)
{
	if (isnan(f))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("cannot convert NaN to type int128")));

	if (f < -INT128_FLOAT_LIMIT || f >= INT128_FLOAT_LIMIT)
		INT128_OVERFLOW();

	f = rint(f);

	/* rounding may have pushed the value over the limit */
	if (f < -INT128_FLOAT_LIMIT || f >= INT128_FLOAT_LIMIT)
		INT128_OVERFLOW();

	return (int128) f;
}

PG_FUNCTION_INFO_V1(int128_from_float8);
Datum
int128_from_float8(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make(float8_to_int128_internal(PG_GETARG_FLOAT8(0))));
}

PG_FUNCTION_INFO_V1(int128_from_float4);
Datum
int128_from_float4(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(int128_make(float8_to_int128_internal((float8) PG_GETARG_FLOAT4(0))));
}

/*----------------------------------------------------------
 * Casts to other types
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_to_int2);
Datum
int128_to_int2(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));

	if (unlikely(v < PG_INT16_MIN || v > PG_INT16_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("smallint out of range")));

	PG_RETURN_INT16((int16) v);
}

PG_FUNCTION_INFO_V1(int128_to_int4);
Datum
int128_to_int4(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));

	if (unlikely(v < PG_INT32_MIN || v > PG_INT32_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("integer out of range")));

	PG_RETURN_INT32((int32) v);
}

PG_FUNCTION_INFO_V1(int128_to_int8);
Datum
int128_to_int8(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));

	if (unlikely(v < PG_INT64_MIN || v > PG_INT64_MAX))
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) v);
}

static Numeric
int128_to_numeric_internal(int128 v)
{
	if (v >= PG_INT64_MIN && v <= PG_INT64_MAX)
		return DatumGetNumeric(DirectFunctionCall1(int8_numeric,
												   Int64GetDatum((int64) v)));
	else
	{
		char		buf[48];

		int128_format(v, buf);
		return DatumGetNumeric(DirectFunctionCall3(numeric_in,
												   CStringGetDatum(buf),
												   ObjectIdGetDatum(InvalidOid),
												   Int32GetDatum(-1)));
	}
}

PG_FUNCTION_INFO_V1(int128_to_numeric);
Datum
int128_to_numeric(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(NumericGetDatum(int128_to_numeric_internal(
															int128_get(PG_GETARG_DATUM(0)))));
}

PG_FUNCTION_INFO_V1(int128_to_float8);
Datum
int128_to_float8(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) int128_get(PG_GETARG_DATUM(0)));
}

PG_FUNCTION_INFO_V1(int128_to_float4);
Datum
int128_to_float4(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT4((float4) (float8) int128_get(PG_GETARG_DATUM(0)));
}

/*----------------------------------------------------------
 * Aggregate support
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_smaller);
Datum
int128_smaller(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(int128_make(a < b ? a : b));
}

PG_FUNCTION_INFO_V1(int128_larger);
Datum
int128_larger(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(int128_make(a > b ? a : b));
}

/*
 * Transition function for sum(int128).  When called as an aggregate the
 * state lives in the aggregate context and can be updated in place, which
 * avoids one palloc per input row.
 */
PG_FUNCTION_INFO_V1(int128_accum);
Datum
int128_accum(PG_FUNCTION_ARGS)
{
	int128		v;
	int128		cur;
	int128		r;

	if (PG_ARGISNULL(1))
	{
		if (PG_ARGISNULL(0))
			PG_RETURN_NULL();
		PG_RETURN_DATUM(PG_GETARG_DATUM(0));
	}

	v = int128_get(PG_GETARG_DATUM(1));

	if (PG_ARGISNULL(0))
		PG_RETURN_DATUM(int128_make(v));

	cur = int128_get(PG_GETARG_DATUM(0));

	if (unlikely(__builtin_add_overflow(cur, v, &r)))
		INT128_OVERFLOW();

	if (AggCheckCallContext(fcinfo, NULL))
	{
		int128	   *state = (int128 *) PG_GETARG_POINTER(0);

		memcpy(state, &r, sizeof(r));
		PG_RETURN_POINTER(state);
	}

	PG_RETURN_DATUM(int128_make(r));
}

typedef struct Int128AvgState
{
	int128		sum;
	int64		count;
} Int128AvgState;

PG_FUNCTION_INFO_V1(int128_avg_accum);
Datum
int128_avg_accum(PG_FUNCTION_ARGS)
{
	Int128AvgState *state;

	if (PG_ARGISNULL(0))
	{
		MemoryContext agg_context;
		MemoryContext old_context;

		/*
		 * The transition type is internal, so nodeAgg will not copy the
		 * state for us: it has to live in the aggregate context.
		 */
		if (!AggCheckCallContext(fcinfo, &agg_context))
			elog(ERROR, "aggregate function called in non-aggregate context");

		old_context = MemoryContextSwitchTo(agg_context);
		state = (Int128AvgState *) palloc0(sizeof(Int128AvgState));
		MemoryContextSwitchTo(old_context);
	}
	else
		state = (Int128AvgState *) PG_GETARG_POINTER(0);

	if (!PG_ARGISNULL(1))
	{
		int128		v = int128_get(PG_GETARG_DATUM(1));
		int128		r;

		if (unlikely(__builtin_add_overflow(state->sum, v, &r)))
			INT128_OVERFLOW();
		state->sum = r;
		state->count++;
	}

	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(int128_avg_combine);
Datum
int128_avg_combine(PG_FUNCTION_ARGS)
{
	Int128AvgState *state1;
	Int128AvgState *state2;
	int128		r;

	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
		PG_RETURN_NULL();
	if (PG_ARGISNULL(0))
		PG_RETURN_POINTER(PG_GETARG_POINTER(1));
	if (PG_ARGISNULL(1))
		PG_RETURN_POINTER(PG_GETARG_POINTER(0));

	state1 = (Int128AvgState *) PG_GETARG_POINTER(0);
	state2 = (Int128AvgState *) PG_GETARG_POINTER(1);

	if (unlikely(__builtin_add_overflow(state1->sum, state2->sum, &r)))
		INT128_OVERFLOW();
	state1->sum = r;
	state1->count += state2->count;

	PG_RETURN_POINTER(state1);
}

PG_FUNCTION_INFO_V1(int128_avg_serialize);
Datum
int128_avg_serialize(PG_FUNCTION_ARGS)
{
	Int128AvgState *state = (Int128AvgState *) PG_GETARG_POINTER(0);
	StringInfoData buf;

	pq_begintypsend(&buf);
	pq_sendint64(&buf, (int64) (((uint128) state->sum) >> 64));
	pq_sendint64(&buf, (int64) (uint64) state->sum);
	pq_sendint64(&buf, state->count);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(int128_avg_deserialize);
Datum
int128_avg_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate = PG_GETARG_BYTEA_PP(0);
	StringInfoData buf;
	Int128AvgState *state;
	MemoryContext agg_context;
	MemoryContext old_context;
	uint64		hi;
	uint64		lo;
	uint128		u;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, VARDATA_ANY(sstate),
						   VARSIZE_ANY_EXHDR(sstate));

	old_context = MemoryContextSwitchTo(agg_context);
	state = (Int128AvgState *) palloc(sizeof(Int128AvgState));
	MemoryContextSwitchTo(old_context);

	hi = (uint64) pq_getmsgint64(&buf);
	lo = (uint64) pq_getmsgint64(&buf);
	u = (((uint128) hi) << 64) | (uint128) lo;
	memcpy(&state->sum, &u, sizeof(u));
	state->count = pq_getmsgint64(&buf);

	pfree(buf.data);

	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(int128_avg);
Datum
int128_avg(PG_FUNCTION_ARGS)
{
	Int128AvgState *state = (Int128AvgState *) PG_GETARG_POINTER(0);
	Numeric		sum;
	Numeric		count;

	if (state->count == 0)
		PG_RETURN_NULL();

	sum = int128_to_numeric_internal(state->sum);
	count = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
												Int64GetDatum(state->count)));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_div,
										NumericGetDatum(sum),
										NumericGetDatum(count)));
}

/*----------------------------------------------------------
 * BRIN minmax-multi distance support
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128_brin_minmax_multi_distance);
Datum
int128_brin_minmax_multi_distance(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));
	uint128		diff;

	if (a >= b)
		diff = ((uint128) a) - ((uint128) b);
	else
		diff = ((uint128) b) - ((uint128) a);

	PG_RETURN_FLOAT8((float8) diff);
}

/*----------------------------------------------------------
 * Range type support
 *---------------------------------------------------------*/

PG_FUNCTION_INFO_V1(int128range_subdiff);
Datum
int128range_subdiff(PG_FUNCTION_ARGS)
{
	float8		v1 = (float8) int128_get(PG_GETARG_DATUM(0));
	float8		v2 = (float8) int128_get(PG_GETARG_DATUM(1));

	PG_RETURN_FLOAT8(v1 - v2);
}

/*----------------------------------------------------------
 * GiST support
 *
 * The GiST key is a bounding box [lower, upper] of all values in the
 * subtree.  Leaf keys have lower == upper.  The key is stored as four
 * 64-bit words rather than two int128 fields so that every field access
 * is naturally aligned; PostgreSQL only guarantees 8-byte alignment for
 * the key type.
 *---------------------------------------------------------*/

#define INT128_GIST_NE_STRATEGY 6

typedef struct Int128GistKey
{
	int64		lower_hi;
	uint64		lower_lo;
	int64		upper_hi;
	uint64		upper_lo;
} Int128GistKey;

static inline int128
gist_key_lower(const Int128GistKey *k)
{
	uint128		u = (((uint128) (uint64) k->lower_hi) << 64) |
		(uint128) k->lower_lo;
	int128		v;

	memcpy(&v, &u, sizeof(v));
	return v;
}

static inline int128
gist_key_upper(const Int128GistKey *k)
{
	uint128		u = (((uint128) (uint64) k->upper_hi) << 64) |
		(uint128) k->upper_lo;
	int128		v;

	memcpy(&v, &u, sizeof(v));
	return v;
}

static inline void
gist_key_set_lower(Int128GistKey *k, int128 v)
{
	uint128		u;

	memcpy(&u, &v, sizeof(u));
	k->lower_hi = (int64) (uint64) (u >> 64);
	k->lower_lo = (uint64) u;
}

static inline void
gist_key_set_upper(Int128GistKey *k, int128 v)
{
	uint128		u;

	memcpy(&u, &v, sizeof(u));
	k->upper_hi = (int64) (uint64) (u >> 64);
	k->upper_lo = (uint64) u;
}

PG_FUNCTION_INFO_V1(int128_gist_key_in);
Datum
int128_gist_key_in(PG_FUNCTION_ARGS)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot accept a value of type int128_gist_key")));
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(int128_gist_key_out);
Datum
int128_gist_key_out(PG_FUNCTION_ARGS)
{
	Int128GistKey *key = (Int128GistKey *) PG_GETARG_POINTER(0);
	char		lo[48];
	char		hi[48];
	char		buf[128];

	int128_format(gist_key_lower(key), lo);
	int128_format(gist_key_upper(key), hi);
	snprintf(buf, sizeof(buf), "(%s,%s)", lo, hi);

	PG_RETURN_CSTRING(pstrdup(buf));
}

PG_FUNCTION_INFO_V1(int128_gist_compress);
Datum
int128_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		Int128GistKey *key = (Int128GistKey *) palloc(sizeof(Int128GistKey));
		int128		v = int128_get(entry->key);

		gist_key_set_lower(key, v);
		gist_key_set_upper(key, v);

		retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));
		gistentryinit(*retval, PointerGetDatum(key),
					  entry->rel, entry->page, entry->offset, false);
	}
	else
		retval = entry;

	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(int128_gist_decompress);
Datum
int128_gist_decompress(PG_FUNCTION_ARGS)
{
	PG_RETURN_POINTER(PG_GETARG_POINTER(0));
}

PG_FUNCTION_INFO_V1(int128_gist_fetch);
Datum
int128_gist_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	Int128GistKey *key = (Int128GistKey *) DatumGetPointer(entry->key);
	GISTENTRY  *retval = (GISTENTRY *) palloc(sizeof(GISTENTRY));

	gistentryinit(*retval, int128_make(gist_key_lower(key)),
				  entry->rel, entry->page, entry->offset, false);

	PG_RETURN_POINTER(retval);
}

PG_FUNCTION_INFO_V1(int128_gist_union);
Datum
int128_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	int		   *size = (int *) PG_GETARG_POINTER(1);
	Int128GistKey *out = (Int128GistKey *) palloc(sizeof(Int128GistKey));
	Int128GistKey *first = (Int128GistKey *) DatumGetPointer(entryvec->vector[0].key);
	int128		lower = gist_key_lower(first);
	int128		upper = gist_key_upper(first);
	int			i;

	*size = sizeof(Int128GistKey);

	for (i = 1; i < entryvec->n; i++)
	{
		Int128GistKey *cur = (Int128GistKey *) DatumGetPointer(entryvec->vector[i].key);
		int128		cl = gist_key_lower(cur);
		int128		cu = gist_key_upper(cur);

		if (cl < lower)
			lower = cl;
		if (cu > upper)
			upper = cu;
	}

	gist_key_set_lower(out, lower);
	gist_key_set_upper(out, upper);

	PG_RETURN_POINTER(out);
}

PG_FUNCTION_INFO_V1(int128_gist_penalty);
Datum
int128_gist_penalty(PG_FUNCTION_ARGS)
{
	Int128GistKey *orig = (Int128GistKey *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(0))->key);
	Int128GistKey *new = (Int128GistKey *) DatumGetPointer(((GISTENTRY *) PG_GETARG_POINTER(1))->key);
	float	   *result = (float *) PG_GETARG_POINTER(2);
	double		old_size = (double) gist_key_upper(orig) - (double) gist_key_lower(orig);
	double		new_size = (double) gist_key_upper(new) - (double) gist_key_lower(new);
	double		penalty = new_size - old_size;

	*result = (penalty > 0) ? (float) penalty : 0.0f;

	PG_RETURN_POINTER(result);
}

typedef struct Int128GistSortItem
{
	Int128GistKey *key;
	OffsetNumber i;
} Int128GistSortItem;

static int
int128_gist_key_cmp(const void *a, const void *b, void *arg)
{
	const Int128GistSortItem *ia = (const Int128GistSortItem *) a;
	const Int128GistSortItem *ib = (const Int128GistSortItem *) b;
	int128		la = gist_key_lower(ia->key);
	int128		lb = gist_key_lower(ib->key);

	if (la != lb)
		return (la > lb) ? 1 : -1;

	{
		int128		ua = gist_key_upper(ia->key);
		int128		ub = gist_key_upper(ib->key);

		if (ua != ub)
			return (ua > ub) ? 1 : -1;
	}

	return 0;
}

static void
int128_gist_bin_union(Datum *u, Int128GistKey *e)
{
	int128		lower = gist_key_lower(e);
	int128		upper = gist_key_upper(e);

	if (DatumGetPointer(*u) == NULL)
	{
		Int128GistKey *k = (Int128GistKey *) palloc(sizeof(Int128GistKey));

		gist_key_set_lower(k, lower);
		gist_key_set_upper(k, upper);
		*u = PointerGetDatum(k);
	}
	else
	{
		Int128GistKey *k = (Int128GistKey *) DatumGetPointer(*u);
		int128		kl = gist_key_lower(k);
		int128		ku = gist_key_upper(k);

		if (lower < kl)
			gist_key_set_lower(k, lower);
		if (upper > ku)
			gist_key_set_upper(k, upper);
	}
}

PG_FUNCTION_INFO_V1(int128_gist_picksplit);
Datum
int128_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *v = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	OffsetNumber maxoff = entryvec->n - 1;
	Int128GistSortItem *arr;
	int			nbytes;
	OffsetNumber i;
	int			split_at;

	arr = (Int128GistSortItem *) palloc((maxoff + 1) * sizeof(Int128GistSortItem));
	nbytes = (maxoff + 2) * sizeof(OffsetNumber);
	v->spl_left = (OffsetNumber *) palloc(nbytes);
	v->spl_right = (OffsetNumber *) palloc(nbytes);
	v->spl_ldatum = PointerGetDatum(0);
	v->spl_rdatum = PointerGetDatum(0);
	v->spl_nleft = 0;
	v->spl_nright = 0;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		arr[i].key = (Int128GistKey *) DatumGetPointer(entryvec->vector[i].key);
		arr[i].i = i;
	}

	qsort_arg(&arr[FirstOffsetNumber], maxoff - FirstOffsetNumber + 1,
			  sizeof(Int128GistSortItem), int128_gist_key_cmp, NULL);

	split_at = (maxoff - FirstOffsetNumber + 1) / 2;

	for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
	{
		if (i <= split_at)
		{
			int128_gist_bin_union(&v->spl_ldatum, arr[i].key);
			v->spl_left[v->spl_nleft++] = arr[i].i;
		}
		else
		{
			int128_gist_bin_union(&v->spl_rdatum, arr[i].key);
			v->spl_right[v->spl_nright++] = arr[i].i;
		}
	}

	PG_RETURN_POINTER(v);
}

PG_FUNCTION_INFO_V1(int128_gist_same);
Datum
int128_gist_same(PG_FUNCTION_ARGS)
{
	Int128GistKey *a = (Int128GistKey *) PG_GETARG_POINTER(0);
	Int128GistKey *b = (Int128GistKey *) PG_GETARG_POINTER(1);
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = (gist_key_lower(a) == gist_key_lower(b) &&
			   gist_key_upper(a) == gist_key_upper(b));

	PG_RETURN_POINTER(result);
}

PG_FUNCTION_INFO_V1(int128_gist_consistent);
Datum
int128_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int128		query = int128_get(PG_GETARG_DATUM(1));
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	Int128GistKey *key = (Int128GistKey *) DatumGetPointer(entry->key);
	int128		lower = gist_key_lower(key);
	int128		upper = gist_key_upper(key);
	bool		is_leaf = GIST_LEAF(entry);
	bool		retval;

	/* All cases served by this function are exact */
	*recheck = false;

	switch (strategy)
	{
		case BTLessEqualStrategyNumber:
			retval = (query >= lower);
			break;
		case BTLessStrategyNumber:
			retval = is_leaf ? (query > lower) : (query >= lower);
			break;
		case BTEqualStrategyNumber:
			retval = is_leaf ? (query == lower) :
				(lower <= query && query <= upper);
			break;
		case BTGreaterStrategyNumber:
			retval = is_leaf ? (query < upper) : (query <= upper);
			break;
		case BTGreaterEqualStrategyNumber:
			retval = (query <= upper);
			break;
		case INT128_GIST_NE_STRATEGY:
			retval = is_leaf ? (query != lower) :
				!(query == lower && query == upper);
			break;
		default:
			retval = false;
	}

	PG_RETURN_BOOL(retval);
}

/*----------------------------------------------------------
 * SP-GiST support
 *
 * The index is a one-dimensional binary search tree.  Each inner tuple
 * stores a pivot value as its prefix and has up to two nodes:
 *
 *   label 0: values < pivot
 *   label 1: values >= pivot
 *
 * The pivot is the median of the values being split, so the tree stays
 * balanced.  inner_consistent() prunes a node when the query operator
 * cannot match any value in that node's half-open range.
 *---------------------------------------------------------*/

#define INT128_SPGIST_NE_STRATEGY 6

static int
cmp_int128_ptr(const void *a, const void *b)
{
	int128		va;
	int128		vb;

	memcpy(&va, a, sizeof(va));
	memcpy(&vb, b, sizeof(vb));
	return int128_cmp_internal(va, vb);
}

PG_FUNCTION_INFO_V1(int128_spgist_config);
Datum
int128_spgist_config(PG_FUNCTION_ARGS)
{
	spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0);
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = cfgin->attType;
	cfg->labelType = INT2OID;
	cfg->leafType = cfgin->attType;
	cfg->canReturnData = false;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(int128_spgist_choose);
Datum
int128_spgist_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	int128		val = int128_get(in->datum);
	int128		pivot;
	int16		label;
	int			i;

	if (!in->hasPrefix)
		elog(ERROR, "int128_spgist_choose: inner tuple has no prefix");

	pivot = int128_get(in->prefixDatum);
	label = (val < pivot) ? 0 : 1;

	for (i = 0; i < in->nNodes; i++)
	{
		int16		existing = DatumGetInt16(in->nodeLabels[i]);

		if (existing == label)
		{
			out->resultType = spgMatchNode;
			out->result.matchNode.nodeN = i;
			out->result.matchNode.levelAdd = 1;
			out->result.matchNode.restDatum = in->leafDatum;
			PG_RETURN_VOID();
		}
		if (existing > label)
			break;
	}

	if (in->allTheSame)
	{
		/*
		 * Every value under this tuple equals the pivot, so the duplicated
		 * node labels do not contain the label we need.  Split the tuple:
		 * the upper tuple keeps the pivot and gets a dummy node (-2) that
		 * points at the old nodes; the lower tuple has no prefix and keeps
		 * the old nodes.  The next choose() call on the upper tuple can then
		 * add the missing label.
		 */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum = in->prefixDatum;
		out->result.splitTuple.prefixNNodes = 1;
		out->result.splitTuple.prefixNodeLabels =
			(Datum *) palloc(sizeof(Datum));
		out->result.splitTuple.prefixNodeLabels[0] = Int16GetDatum(-2);
		out->result.splitTuple.childNodeN = 0;
		out->result.splitTuple.postfixHasPrefix = false;
		PG_RETURN_VOID();
	}

	out->resultType = spgAddNode;
	out->result.addNode.nodeLabel = Int16GetDatum(label);
	out->result.addNode.nodeN = i;

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(int128_spgist_picksplit);
Datum
int128_spgist_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	int128	   *vals;
	int128		pivot;
	int			i;

	vals = (int128 *) palloc(in->nTuples * sizeof(int128));
	for (i = 0; i < in->nTuples; i++)
		memcpy(&vals[i], DatumGetPointer(in->datums[i]), sizeof(int128));

	qsort(vals, in->nTuples, sizeof(int128), cmp_int128_ptr);
	pivot = vals[in->nTuples / 2];

	out->hasPrefix = true;
	out->prefixDatum = int128_make(pivot);
	out->nNodes = 2;
	out->nodeLabels = (Datum *) palloc(2 * sizeof(Datum));
	out->nodeLabels[0] = Int16GetDatum(0);
	out->nodeLabels[1] = Int16GetDatum(1);
	out->mapTuplesToNodes = (int *) palloc(in->nTuples * sizeof(int));
	out->leafTupleDatums = (Datum *) palloc(in->nTuples * sizeof(Datum));

	for (i = 0; i < in->nTuples; i++)
	{
		int128		v = int128_get(in->datums[i]);

		out->mapTuplesToNodes[i] = (v < pivot) ? 0 : 1;
		out->leafTupleDatums[i] = int128_make(v);
	}

	PG_RETURN_VOID();
}

/*
 * Does the half-open range [lo, hi] (either bound may be infinite) contain
 * a value satisfying "value <strategy> query"?
 */
static bool
int128_spgist_range_matches(bool lo_inf, int128 lo,
							bool hi_inf, int128 hi,
							StrategyNumber strategy, int128 query)
{
	switch (strategy)
	{
		case BTLessStrategyNumber:
			if (query == PG_INT128_MIN)
				return false;
			if (!lo_inf && lo >= query)
				return false;
			return true;
		case BTLessEqualStrategyNumber:
			if (!lo_inf && lo > query)
				return false;
			return true;
		case BTEqualStrategyNumber:
			if (!lo_inf && lo > query)
				return false;
			if (!hi_inf && hi < query)
				return false;
			return true;
		case BTGreaterEqualStrategyNumber:
			if (!hi_inf && hi < query)
				return false;
			return true;
		case BTGreaterStrategyNumber:
			if (query == PG_INT128_MAX)
				return false;
			if (!hi_inf && hi <= query)
				return false;
			return true;
		case INT128_SPGIST_NE_STRATEGY:
			if (lo_inf || hi_inf)
				return true;
			if (lo == hi && lo == query)
				return false;
			return true;
		default:
			return false;
	}
}

static double
int128_spgist_range_distance(bool lo_inf, int128 lo, bool hi_inf, int128 hi,
							 int128 query)
{
	if (!lo_inf && query < lo)
		return (double) lo - (double) query;
	if (!hi_inf && query > hi)
		return (double) query - (double) hi;
	return 0.0;
}

PG_FUNCTION_INFO_V1(int128_spgist_inner_consistent);
Datum
int128_spgist_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int128		pivot = 0;
	bool		has_pivot = in->hasPrefix;
	int			i;

	if (has_pivot)
		pivot = int128_get(in->prefixDatum);

	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	out->levelAdds = (int *) palloc(sizeof(int) * in->nNodes);
	if (in->norderbys > 0)
		out->distances = (double **) palloc(sizeof(double *) * in->nNodes);
	out->nNodes = 0;

	for (i = 0; i < in->nNodes; i++)
	{
		int16		label = DatumGetInt16(in->nodeLabels[i]);
		bool		lo_inf;
		bool		hi_inf;
		int128		lo = 0;
		int128		hi = 0;
		bool		res = true;
		int			j;

		if (!has_pivot)
		{
			lo_inf = true;
			hi_inf = true;
		}
		else if (label == -2)
		{
			/* old all-the-same subtree: every value equals the pivot */
			lo_inf = false;
			hi_inf = false;
			lo = pivot;
			hi = pivot;
		}
		else if (label == 0)
		{
			if (pivot == PG_INT128_MIN)
				continue;		/* empty range */
			lo_inf = true;
			hi_inf = false;
			hi = pivot - 1;
		}
		else
		{
			lo_inf = false;
			hi_inf = true;
			lo = pivot;
		}

		for (j = 0; j < in->nkeys; j++)
		{
			StrategyNumber strategy = in->scankeys[j].sk_strategy;
			int128		query = int128_get(in->scankeys[j].sk_argument);

			if (!int128_spgist_range_matches(lo_inf, lo, hi_inf, hi,
											 strategy, query))
			{
				res = false;
				break;
			}
		}

		if (res)
		{
			out->nodeNumbers[out->nNodes] = i;
			out->levelAdds[out->nNodes] = 1;
			if (in->norderbys > 0)
			{
				double	   *dist = (double *) palloc(sizeof(double) * in->norderbys);
				int			k;

				for (k = 0; k < in->norderbys; k++)
				{
					int128		query = int128_get(in->orderbys[k].sk_argument);

					dist[k] = int128_spgist_range_distance(lo_inf, lo, hi_inf, hi,
														   query);
				}
				out->distances[out->nNodes] = dist;
			}
			out->nNodes++;
		}
	}

	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(int128_spgist_leaf_consistent);
Datum
int128_spgist_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	int128		val = int128_get(in->leafDatum);
	bool		res = true;
	int			j;

	out->recheck = false;

	for (j = 0; j < in->nkeys; j++)
	{
		StrategyNumber strategy = in->scankeys[j].sk_strategy;
		int128		query = int128_get(in->scankeys[j].sk_argument);

		switch (strategy)
		{
			case BTLessStrategyNumber:
				res = (val < query);
				break;
			case BTLessEqualStrategyNumber:
				res = (val <= query);
				break;
			case BTEqualStrategyNumber:
				res = (val == query);
				break;
			case BTGreaterEqualStrategyNumber:
				res = (val >= query);
				break;
			case BTGreaterStrategyNumber:
				res = (val > query);
				break;
			case INT128_SPGIST_NE_STRATEGY:
				res = (val != query);
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d", strategy);
				res = false;
				break;
		}

		if (!res)
			break;
	}

	if (res && in->norderbys > 0)
	{
		double	   *dist = (double *) palloc(sizeof(double) * in->norderbys);
		int			k;

		for (k = 0; k < in->norderbys; k++)
		{
			int128		query = int128_get(in->orderbys[k].sk_argument);

			dist[k] = fabs((double) val - (double) query);
		}
		out->distances = dist;
	}

	PG_RETURN_BOOL(res);
}

/*----------------------------------------------------------
 * GIN support
 *
 * A scalar value is stored as a single GIN key, so equality lookups are
 * exact.  Range operators use a partial-match scan: the scan starts at the
 * leftmost key (for < and <=) or at the query value (for >= and >), and
 * compare_prefix() stops it once the keys leave the requested range.
 *---------------------------------------------------------*/

typedef struct Int128GinQuery
{
	StrategyNumber strategy;
	Datum		datum;			/* pointer to the query value */
} Int128GinQuery;

PG_FUNCTION_INFO_V1(int128_gin_extract_value);
Datum
int128_gin_extract_value(PG_FUNCTION_ARGS)
{
	Datum		datum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));

	entries[0] = datum;
	*nentries = 1;

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(int128_gin_extract_query);
Datum
int128_gin_extract_query(PG_FUNCTION_ARGS)
{
	Datum		datum = PG_GETARG_DATUM(0);
	int32	   *nentries = (int32 *) PG_GETARG_POINTER(1);
	StrategyNumber strategy = PG_GETARG_UINT16(2);
	bool	  **partialmatch = (bool **) PG_GETARG_POINTER(3);
	Pointer   **extra_data = (Pointer **) PG_GETARG_POINTER(4);
	Datum	   *entries = (Datum *) palloc(sizeof(Datum));
	Int128GinQuery *data = (Int128GinQuery *) palloc(sizeof(Int128GinQuery));
	bool	   *ptr_partialmatch;

	*nentries = 1;
	ptr_partialmatch = *partialmatch = (bool *) palloc(sizeof(bool));
	*ptr_partialmatch = false;

	data->strategy = strategy;
	data->datum = datum;
	*extra_data = (Pointer *) palloc(sizeof(Pointer));
	**extra_data = (Pointer) data;

	switch (strategy)
	{
		case BTLessStrategyNumber:
		case BTLessEqualStrategyNumber:
			entries[0] = int128_make(PG_INT128_MIN);
			*ptr_partialmatch = true;
			break;
		case BTGreaterEqualStrategyNumber:
		case BTGreaterStrategyNumber:
			*ptr_partialmatch = true;
			entries[0] = datum;
			break;
		case BTEqualStrategyNumber:
			entries[0] = datum;
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", strategy);
	}

	PG_RETURN_POINTER(entries);
}

PG_FUNCTION_INFO_V1(int128_gin_compare_prefix);
Datum
int128_gin_compare_prefix(PG_FUNCTION_ARGS)
{
	Datum		a = PG_GETARG_DATUM(0);
	Datum		b = PG_GETARG_DATUM(1);
	Int128GinQuery *data = (Int128GinQuery *) PG_GETARG_POINTER(3);
	int32		res;
	int			cmp;

	cmp = int128_cmp_internal(
							  (data->strategy == BTLessStrategyNumber ||
							   data->strategy == BTLessEqualStrategyNumber)
							  ? int128_get(data->datum)
							  : int128_get(a),
							  int128_get(b));

	switch (data->strategy)
	{
		case BTLessStrategyNumber:
			/* stop once the indexed key is >= the query value */
			res = (cmp > 0) ? 0 : 1;
			break;
		case BTLessEqualStrategyNumber:
			res = (cmp >= 0) ? 0 : 1;
			break;
		case BTEqualStrategyNumber:
			res = (cmp != 0) ? 1 : 0;
			break;
		case BTGreaterEqualStrategyNumber:
			res = (cmp <= 0) ? 0 : 1;
			break;
		case BTGreaterStrategyNumber:
			if (cmp < 0)
				res = 0;
			else if (cmp == 0)
				res = -1;		/* skip equal keys, keep scanning */
			else
				res = 1;
			break;
		default:
			elog(ERROR, "unrecognized strategy number: %d", data->strategy);
			res = 0;
	}

	PG_RETURN_INT32(res);
}

PG_FUNCTION_INFO_V1(int128_gin_consistent);
Datum
int128_gin_consistent(PG_FUNCTION_ARGS)
{
	bool	   *recheck = (bool *) PG_GETARG_POINTER(5);

	*recheck = false;
	PG_RETURN_BOOL(true);
}

/*----------------------------------------------------------
 * Mathematical functions
 *---------------------------------------------------------*/

static int128
int128_pow10(int n)
{
	int128		result = 1;
	int128		ten = 10;

	while (n > 0)
	{
		if (n & 1)
		{
			if (__builtin_mul_overflow(result, ten, &result))
				INT128_OVERFLOW();
		}
		n >>= 1;
		if (n > 0)
		{
			if (__builtin_mul_overflow(ten, ten, &ten))
				INT128_OVERFLOW();
		}
	}
	return result;
}

PG_FUNCTION_INFO_V1(int128_identity);
Datum
int128_identity(PG_FUNCTION_ARGS)
{
	PG_RETURN_DATUM(PG_GETARG_DATUM(0));
}

PG_FUNCTION_INFO_V1(int128_round_scale);
Datum
int128_round_scale(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	int32		scale = PG_GETARG_INT32(1);
	int128		p;
	int128		rem;
	int128		base;

	if (scale >= 0)
		PG_RETURN_DATUM(int128_make(v));
	if (scale < -38)
		PG_RETURN_DATUM(int128_make(0));

	p = int128_pow10(-scale);
	rem = v % p;
	base = v - rem;
	if (rem < 0)
		rem = -rem;
	if (rem >= p / 2)
	{
		if (v < 0)
		{
			if (__builtin_sub_overflow(base, p, &base))
				INT128_OVERFLOW();
		}
		else
		{
			if (__builtin_add_overflow(base, p, &base))
				INT128_OVERFLOW();
		}
	}
	PG_RETURN_DATUM(int128_make(base));
}

PG_FUNCTION_INFO_V1(int128_trunc_scale);
Datum
int128_trunc_scale(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	int32		scale = PG_GETARG_INT32(1);
	int128		p;

	if (scale >= 0)
		PG_RETURN_DATUM(int128_make(v));
	if (scale < -38)
		PG_RETURN_DATUM(int128_make(0));

	p = int128_pow10(-scale);
	PG_RETURN_DATUM(int128_make(v - (v % p)));
}

PG_FUNCTION_INFO_V1(int128_sign);
Datum
int128_sign(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));

	PG_RETURN_DATUM(int128_make((v > 0) ? 1 : (v < 0) ? -1 : 0));
}

static Numeric
int128_arg_to_numeric(Datum d)
{
	return int128_to_numeric_internal(int128_get(d));
}

PG_FUNCTION_INFO_V1(int128_power);
Datum
int128_power(PG_FUNCTION_ARGS)
{
	Numeric		a = int128_arg_to_numeric(PG_GETARG_DATUM(0));
	Numeric		b = int128_arg_to_numeric(PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_power,
										NumericGetDatum(a),
										NumericGetDatum(b)));
}

PG_FUNCTION_INFO_V1(int128_sqrt);
Datum
int128_sqrt(PG_FUNCTION_ARGS)
{
	Numeric		a = int128_arg_to_numeric(PG_GETARG_DATUM(0));

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_sqrt, NumericGetDatum(a)));
}

PG_FUNCTION_INFO_V1(int128_cbrt);
Datum
int128_cbrt(PG_FUNCTION_ARGS)
{
	float8		f = (float8) int128_get(PG_GETARG_DATUM(0));

	PG_RETURN_DATUM(DirectFunctionCall1(dcbrt, Float8GetDatum(f)));
}

PG_FUNCTION_INFO_V1(int128_exp);
Datum
int128_exp(PG_FUNCTION_ARGS)
{
	Numeric		a = int128_arg_to_numeric(PG_GETARG_DATUM(0));

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_exp, NumericGetDatum(a)));
}

PG_FUNCTION_INFO_V1(int128_ln);
Datum
int128_ln(PG_FUNCTION_ARGS)
{
	Numeric		a = int128_arg_to_numeric(PG_GETARG_DATUM(0));

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_ln, NumericGetDatum(a)));
}

PG_FUNCTION_INFO_V1(int128_log);
Datum
int128_log(PG_FUNCTION_ARGS)
{
	Numeric		a = int128_arg_to_numeric(PG_GETARG_DATUM(0));
	Numeric		ten = int128_to_numeric_internal(10);

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_log,
										NumericGetDatum(ten),
										NumericGetDatum(a)));
}

PG_FUNCTION_INFO_V1(int128_log_base);
Datum
int128_log_base(PG_FUNCTION_ARGS)
{
	Numeric		base = int128_arg_to_numeric(PG_GETARG_DATUM(0));
	Numeric		value = int128_arg_to_numeric(PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_log,
										NumericGetDatum(base),
										NumericGetDatum(value)));
}

PG_FUNCTION_INFO_V1(int128_factorial);
Datum
int128_factorial(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));

	if (v < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("factorial of a negative number is undefined")));
	if (v > PG_INT64_MAX)
	{
		char		buf[48];

		int128_format(v, buf);
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("factorial of %s is too large", buf)));
	}

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_fac,
										Int64GetDatum((int64) v)));
}

#define INT128_TRIG_1(name, cfunc) \
PG_FUNCTION_INFO_V1(int128_##name); \
Datum \
int128_##name(PG_FUNCTION_ARGS) \
{ \
	float8		f = (float8) int128_get(PG_GETARG_DATUM(0)); \
	PG_RETURN_DATUM(DirectFunctionCall1(cfunc, Float8GetDatum(f))); \
}

INT128_TRIG_1(sin, dsin)
INT128_TRIG_1(cos, dcos)
INT128_TRIG_1(tan, dtan)
INT128_TRIG_1(cot, dcot)
INT128_TRIG_1(asin, dasin)
INT128_TRIG_1(acos, dacos)
INT128_TRIG_1(atan, datan)
INT128_TRIG_1(sinh, dsinh)
INT128_TRIG_1(cosh, dcosh)
INT128_TRIG_1(tanh, dtanh)
INT128_TRIG_1(asinh, dasinh)
INT128_TRIG_1(acosh, dacosh)
INT128_TRIG_1(atanh, datanh)
INT128_TRIG_1(degrees, degrees)
INT128_TRIG_1(radians, radians)

PG_FUNCTION_INFO_V1(int128_atan2);
Datum
int128_atan2(PG_FUNCTION_ARGS)
{
	float8		y = (float8) int128_get(PG_GETARG_DATUM(0));
	float8		x = (float8) int128_get(PG_GETARG_DATUM(1));

	PG_RETURN_DATUM(DirectFunctionCall2(datan2,
										Float8GetDatum(y),
										Float8GetDatum(x)));
}

PG_FUNCTION_INFO_V1(int128_bit_count);
Datum
int128_bit_count(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	uint64		lo = (uint64) v;
	uint64		hi = (uint64) (((uint128) v) >> 64);

	PG_RETURN_INT32(__builtin_popcountll(lo) + __builtin_popcountll(hi));
}

PG_FUNCTION_INFO_V1(int128_width_bucket);
Datum
int128_width_bucket(PG_FUNCTION_ARGS)
{
	int128		operand = int128_get(PG_GETARG_DATUM(0));
	int128		low = int128_get(PG_GETARG_DATUM(1));
	int128		high = int128_get(PG_GETARG_DATUM(2));
	int32		count = PG_GETARG_INT32(3);
	Numeric		n_operand;
	Numeric		n_low;
	Numeric		n_high;
	Numeric		n_count;
	Numeric		n_diff;
	Numeric		n_bucket;

	if (count <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_NTILE),
				 errmsg("count must be greater than zero")));
	if (low >= high)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_ARGUMENT_FOR_NTILE),
				 errmsg("lower bound cannot equal upper bound")));
	if (operand < low)
		PG_RETURN_INT32(0);
	if (operand >= high)
		PG_RETURN_INT32(count + 1);

	n_operand = int128_to_numeric_internal(operand);
	n_low = int128_to_numeric_internal(low);
	n_high = int128_to_numeric_internal(high);
	n_count = DatumGetNumeric(DirectFunctionCall1(int4_numeric,
												  Int32GetDatum(count)));
	n_diff = DatumGetNumeric(DirectFunctionCall2(numeric_sub,
												 NumericGetDatum(n_operand),
												 NumericGetDatum(n_low)));
	n_bucket = DatumGetNumeric(DirectFunctionCall2(numeric_mul,
												   NumericGetDatum(n_diff),
												   NumericGetDatum(n_count)));
	n_bucket = DatumGetNumeric(DirectFunctionCall2(numeric_div,
												   NumericGetDatum(n_bucket),
												   NumericGetDatum(DatumGetNumeric(DirectFunctionCall2(numeric_sub,
																									   NumericGetDatum(n_high),
																									   NumericGetDatum(n_low))))));
	n_bucket = DatumGetNumeric(DirectFunctionCall1(numeric_floor,
												   NumericGetDatum(n_bucket)));
	n_bucket = DatumGetNumeric(DirectFunctionCall2(numeric_add,
												   NumericGetDatum(n_bucket),
												   NumericGetDatum(DatumGetNumeric(DirectFunctionCall1(int4_numeric,
																									   Int32GetDatum(1))))));

	PG_RETURN_DATUM(DirectFunctionCall1(numeric_int4,
										NumericGetDatum(n_bucket)));
}

static inline uint128
int128_random_u128(void)
{
	return (((uint128) pg_prng_uint64(&pg_global_prng_state)) << 64) |
		(uint128) pg_prng_uint64(&pg_global_prng_state);
}

PG_FUNCTION_INFO_V1(int128_random);
Datum
int128_random(PG_FUNCTION_ARGS)
{
	int128		lo = int128_get(PG_GETARG_DATUM(0));
	int128		hi = int128_get(PG_GETARG_DATUM(1));
	uint128		range;
	uint128		r;
	uint128		u;
	int128		result;

	if (lo > hi)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lower bound must be less than or equal to upper bound")));

	range = (uint128) hi - (uint128) lo + 1;
	if (range == 0)
	{
		/* full 128-bit range */
		r = int128_random_u128();
	}
	else
	{
		uint128		rem = ((~(uint128) 0) % range + 1) % range;
		uint128		bound = (~(uint128) 0) - rem + 1;

		do
		{
			r = int128_random_u128();
		} while (rem != 0 && r >= bound);
		r %= range;
	}

	u = (uint128) lo + r;
	memcpy(&result, &u, sizeof(result));
	PG_RETURN_DATUM(int128_make(result));
}

PG_FUNCTION_INFO_V1(int128_to_char);
Datum
int128_to_char(PG_FUNCTION_ARGS)
{
	Numeric		n = int128_to_numeric_internal(int128_get(PG_GETARG_DATUM(0)));
	text	   *fmt = PG_GETARG_TEXT_PP(1);

	PG_RETURN_DATUM(DirectFunctionCall2(numeric_to_char,
										NumericGetDatum(n),
										PointerGetDatum(fmt)));
}

/*----------------------------------------------------------
 * Statistical aggregates
 *---------------------------------------------------------*/

typedef struct Int128StatState
{
	Numeric		sum;
	Numeric		sum2;
	int64		count;
} Int128StatState;

static Int128StatState *
int128_stat_state_new(FunctionCallInfo fcinfo)
{
	MemoryContext agg_context;
	MemoryContext old_context;
	Int128StatState *state;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	old_context = MemoryContextSwitchTo(agg_context);
	state = (Int128StatState *) palloc0(sizeof(Int128StatState));
	state->sum = int128_to_numeric_internal(0);
	state->sum2 = int128_to_numeric_internal(0);
	MemoryContextSwitchTo(old_context);

	return state;
}

PG_FUNCTION_INFO_V1(int128_stat_accum);
Datum
int128_stat_accum(PG_FUNCTION_ARGS)
{
	Int128StatState *state;

	if (PG_ARGISNULL(0))
		state = int128_stat_state_new(fcinfo);
	else
		state = (Int128StatState *) PG_GETARG_POINTER(0);

	if (!PG_ARGISNULL(1))
	{
		MemoryContext agg_context;
		MemoryContext old_context;
		Numeric		v;
		Numeric		v2;

		if (!AggCheckCallContext(fcinfo, &agg_context))
			elog(ERROR, "aggregate function called in non-aggregate context");

		old_context = MemoryContextSwitchTo(agg_context);
		v = int128_to_numeric_internal(int128_get(PG_GETARG_DATUM(1)));
		v2 = DatumGetNumeric(DirectFunctionCall2(numeric_mul,
												 NumericGetDatum(v),
												 NumericGetDatum(v)));
		state->sum = DatumGetNumeric(DirectFunctionCall2(numeric_add,
														 NumericGetDatum(state->sum),
														 NumericGetDatum(v)));
		state->sum2 = DatumGetNumeric(DirectFunctionCall2(numeric_add,
														  NumericGetDatum(state->sum2),
														  NumericGetDatum(v2)));
		state->count++;
		MemoryContextSwitchTo(old_context);
	}

	PG_RETURN_POINTER(state);
}

PG_FUNCTION_INFO_V1(int128_stat_combine);
Datum
int128_stat_combine(PG_FUNCTION_ARGS)
{
	Int128StatState *s1;
	Int128StatState *s2;

	if (PG_ARGISNULL(0) && PG_ARGISNULL(1))
		PG_RETURN_NULL();
	if (PG_ARGISNULL(0))
		PG_RETURN_POINTER(PG_GETARG_POINTER(1));
	if (PG_ARGISNULL(1))
		PG_RETURN_POINTER(PG_GETARG_POINTER(0));

	s1 = (Int128StatState *) PG_GETARG_POINTER(0);
	s2 = (Int128StatState *) PG_GETARG_POINTER(1);

	{
		MemoryContext agg_context;
		MemoryContext old_context;

		if (!AggCheckCallContext(fcinfo, &agg_context))
			elog(ERROR, "aggregate function called in non-aggregate context");

		old_context = MemoryContextSwitchTo(agg_context);
		s1->sum = DatumGetNumeric(DirectFunctionCall2(numeric_add,
													  NumericGetDatum(s1->sum),
													  NumericGetDatum(s2->sum)));
		s1->sum2 = DatumGetNumeric(DirectFunctionCall2(numeric_add,
													   NumericGetDatum(s1->sum2),
													   NumericGetDatum(s2->sum2)));
		s1->count += s2->count;
		MemoryContextSwitchTo(old_context);
	}

	PG_RETURN_POINTER(s1);
}

PG_FUNCTION_INFO_V1(int128_stat_serialize);
Datum
int128_stat_serialize(PG_FUNCTION_ARGS)
{
	Int128StatState *state = (Int128StatState *) PG_GETARG_POINTER(0);
	StringInfoData buf;
	char	   *s1;
	char	   *s2;

	s1 = DatumGetCString(DirectFunctionCall1(numeric_out,
											 NumericGetDatum(state->sum)));
	s2 = DatumGetCString(DirectFunctionCall1(numeric_out,
											 NumericGetDatum(state->sum2)));

	pq_begintypsend(&buf);
	pq_sendint64(&buf, state->count);
	pq_sendstring(&buf, s1);
	pq_sendstring(&buf, s2);

	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

PG_FUNCTION_INFO_V1(int128_stat_deserialize);
Datum
int128_stat_deserialize(PG_FUNCTION_ARGS)
{
	bytea	   *sstate = PG_GETARG_BYTEA_PP(0);
	StringInfoData buf;
	Int128StatState *state;
	MemoryContext agg_context;
	MemoryContext old_context;
	const char *s1;
	const char *s2;

	if (!AggCheckCallContext(fcinfo, &agg_context))
		elog(ERROR, "aggregate function called in non-aggregate context");

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, VARDATA_ANY(sstate),
						   VARSIZE_ANY_EXHDR(sstate));

	old_context = MemoryContextSwitchTo(agg_context);
	state = (Int128StatState *) palloc0(sizeof(Int128StatState));

	state->count = pq_getmsgint64(&buf);
	s1 = pq_getmsgstring(&buf);
	s2 = pq_getmsgstring(&buf);
	state->sum = DatumGetNumeric(DirectFunctionCall3(numeric_in,
													 CStringGetDatum(s1),
													 ObjectIdGetDatum(InvalidOid),
													 Int32GetDatum(-1)));
	state->sum2 = DatumGetNumeric(DirectFunctionCall3(numeric_in,
													  CStringGetDatum(s2),
													  ObjectIdGetDatum(InvalidOid),
													  Int32GetDatum(-1)));
	MemoryContextSwitchTo(old_context);

	pfree(buf.data);

	PG_RETURN_POINTER(state);
}

static Numeric
int128_stat_variance(Int128StatState *state, bool sample)
{
	Numeric		n;
	Numeric		sum_sq;
	Numeric		var;

	if (state->count == 0)
		return NULL;
	if (sample && state->count < 2)
		return NULL;

	n = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
											Int64GetDatum(state->count)));
	sum_sq = DatumGetNumeric(DirectFunctionCall2(numeric_mul,
												 NumericGetDatum(state->sum),
												 NumericGetDatum(state->sum)));
	sum_sq = DatumGetNumeric(DirectFunctionCall2(numeric_div,
												 NumericGetDatum(sum_sq),
												 NumericGetDatum(n)));
	var = DatumGetNumeric(DirectFunctionCall2(numeric_sub,
											  NumericGetDatum(state->sum2),
											  NumericGetDatum(sum_sq)));
	if (sample)
		var = DatumGetNumeric(DirectFunctionCall2(numeric_div,
												  NumericGetDatum(var),
												  NumericGetDatum(DatumGetNumeric(DirectFunctionCall1(int8_numeric,
																									  Int64GetDatum(state->count - 1))))));
	else
		var = DatumGetNumeric(DirectFunctionCall2(numeric_div,
												  NumericGetDatum(var),
												  NumericGetDatum(n)));

	return var;
}

PG_FUNCTION_INFO_V1(int128_var_pop);
Datum
int128_var_pop(PG_FUNCTION_ARGS)
{
	Int128StatState *state = (Int128StatState *) PG_GETARG_POINTER(0);
	Numeric		var = int128_stat_variance(state, false);

	if (var == NULL)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(NumericGetDatum(var));
}

PG_FUNCTION_INFO_V1(int128_var_samp);
Datum
int128_var_samp(PG_FUNCTION_ARGS)
{
	Int128StatState *state = (Int128StatState *) PG_GETARG_POINTER(0);
	Numeric		var = int128_stat_variance(state, true);

	if (var == NULL)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(NumericGetDatum(var));
}

PG_FUNCTION_INFO_V1(int128_stddev_pop);
Datum
int128_stddev_pop(PG_FUNCTION_ARGS)
{
	Int128StatState *state = (Int128StatState *) PG_GETARG_POINTER(0);
	Numeric		var = int128_stat_variance(state, false);

	if (var == NULL)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(DirectFunctionCall1(numeric_sqrt,
										NumericGetDatum(var)));
}

PG_FUNCTION_INFO_V1(int128_stddev_samp);
Datum
int128_stddev_samp(PG_FUNCTION_ARGS)
{
	Int128StatState *state = (Int128StatState *) PG_GETARG_POINTER(0);
	Numeric		var = int128_stat_variance(state, true);

	if (var == NULL)
		PG_RETURN_NULL();
	PG_RETURN_DATUM(DirectFunctionCall1(numeric_sqrt,
										NumericGetDatum(var)));
}

/*----------------------------------------------------------
 * Series and utility functions
 *---------------------------------------------------------*/

typedef struct Int128SeriesCtx
{
	int128		current;
	int128		finish;
	int128		step;
	bool		done;
} Int128SeriesCtx;

PG_FUNCTION_INFO_V1(int128_generate_series);
Datum
int128_generate_series(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	Int128SeriesCtx *ctx;
	int128		result;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
		ctx = (Int128SeriesCtx *) palloc(sizeof(Int128SeriesCtx));
		ctx->current = int128_get(PG_GETARG_DATUM(0));
		ctx->finish = int128_get(PG_GETARG_DATUM(1));
		ctx->step = (PG_NARGS() > 2) ? int128_get(PG_GETARG_DATUM(2)) : 1;
		ctx->done = false;
		if (ctx->step == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("step size cannot equal zero")));
		funcctx->user_fctx = ctx;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (Int128SeriesCtx *) funcctx->user_fctx;

	if (ctx->done)
		SRF_RETURN_DONE(funcctx);
	if (ctx->step > 0 ? ctx->current > ctx->finish : ctx->current < ctx->finish)
		SRF_RETURN_DONE(funcctx);

	result = ctx->current;
	if (__builtin_add_overflow(ctx->current, ctx->step, &ctx->current))
		ctx->done = true;

	SRF_RETURN_NEXT(funcctx, int128_make(result));
}

PG_FUNCTION_INFO_V1(int128_scale);
Datum
int128_scale(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

PG_FUNCTION_INFO_V1(int128_min_scale);
Datum
int128_min_scale(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(0);
}

PG_FUNCTION_INFO_V1(int128_inc);
Datum
int128_inc(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	int128		r;

	if (unlikely(__builtin_add_overflow(v, 1, &r)))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(r));
}

PG_FUNCTION_INFO_V1(int128_dec);
Datum
int128_dec(PG_FUNCTION_ARGS)
{
	int128		v = int128_get(PG_GETARG_DATUM(0));
	int128		r;

	if (unlikely(__builtin_sub_overflow(v, 1, &r)))
		INT128_OVERFLOW();
	PG_RETURN_DATUM(int128_make(r));
}

PG_FUNCTION_INFO_V1(int128_pg_size_pretty);
Datum
int128_pg_size_pretty(PG_FUNCTION_ARGS)
{
	Numeric		n = int128_to_numeric_internal(int128_get(PG_GETARG_DATUM(0)));

	PG_RETURN_DATUM(DirectFunctionCall1(pg_size_pretty_numeric,
										NumericGetDatum(n)));
}

PG_FUNCTION_INFO_V1(int128_dist);
Datum
int128_dist(PG_FUNCTION_ARGS)
{
	int128		a = int128_get(PG_GETARG_DATUM(0));
	int128		b = int128_get(PG_GETARG_DATUM(1));
	int128		r;

	if (unlikely(__builtin_sub_overflow(a, b, &r)))
		INT128_OVERFLOW();
	if (unlikely(r == PG_INT128_MIN))
		INT128_OVERFLOW();
	if (r < 0)
		r = -r;
	PG_RETURN_DATUM(int128_make(r));
}

PG_FUNCTION_INFO_V1(int128_gist_distance);
Datum
int128_gist_distance(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	int128		query = int128_get(PG_GETARG_DATUM(1));
	Int128GistKey *key = (Int128GistKey *) DatumGetPointer(entry->key);
	int128		lower = gist_key_lower(key);
	int128		upper = gist_key_upper(key);
	float8		dist;

	if (query < lower)
		dist = (float8) lower - (float8) query;
	else if (query > upper)
		dist = (float8) query - (float8) upper;
	else
		dist = 0.0;

	PG_RETURN_FLOAT8(dist);
}

PG_FUNCTION_INFO_V1(int128_width_bucket_array);
Datum
int128_width_bucket_array(PG_FUNCTION_ARGS)
{
	int128		operand = int128_get(PG_GETARG_DATUM(0));
	ArrayType  *thresholds = PG_GETARG_ARRAYTYPE_P(1);
	char	   *data;
	int			left;
	int			right;

	if (ARR_NDIM(thresholds) > 1)
		ereport(ERROR,
				(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
				 errmsg("thresholds must be one-dimensional array")));
	if (array_contains_nulls(thresholds))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("thresholds array must not contain NULLs")));

	data = (char *) ARR_DATA_PTR(thresholds);
	left = 0;
	right = ArrayGetNItems(ARR_NDIM(thresholds), ARR_DIMS(thresholds));

	while (left < right)
	{
		int			mid = (left + right) / 2;
		int128		threshold;

		memcpy(&threshold, data + mid * sizeof(int128), sizeof(int128));
		if (operand < threshold)
			right = mid;
		else
			left = mid + 1;
	}

	PG_RETURN_INT32(left);
}

#if PG_VERSION_NUM >= 180000
static Datum
int128_decrement(Relation rel, Datum existing, bool *underflow)
{
	int128		v = int128_get(existing);

	if (v == PG_INT128_MIN)
	{
		*underflow = true;
		return (Datum) 0;
	}
	*underflow = false;
	return int128_make(v - 1);
}

static Datum
int128_increment(Relation rel, Datum existing, bool *overflow)
{
	int128		v = int128_get(existing);

	if (v == PG_INT128_MAX)
	{
		*overflow = true;
		return (Datum) 0;
	}
	*overflow = false;
	return int128_make(v + 1);
}

PG_FUNCTION_INFO_V1(int128_skipsupport);
Datum
int128_skipsupport(PG_FUNCTION_ARGS)
{
	SkipSupport sksup = (SkipSupport) PG_GETARG_POINTER(0);

	sksup->decrement = int128_decrement;
	sksup->increment = int128_increment;
	sksup->low_elem = int128_make(PG_INT128_MIN);
	sksup->high_elem = int128_make(PG_INT128_MAX);

	PG_RETURN_VOID();
}
#endif
