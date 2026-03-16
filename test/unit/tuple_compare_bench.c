/*
 * Microbenchmark for tuple comparison functions.
 * Measures raw comparison cost with zero Lua overhead.
 *
 * Build: make -j32 (auto-detected via CMakeLists.txt)
 * Run:   test/unit/tuple_compare_bench.test
 *   or:  perf stat test/unit/tuple_compare_bench.test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fiber.h"
#include "key_def.h"
#include "memory.h"
#include "mp_uuid.h"
#include "msgpuck.h"
#include "tt_uuid.h"
#include "tuple.h"

enum {
	/** Number of comparison operations per benchmark round. */
	N_OPS = 5000000,
	/** Number of rounds (report the best). */
	N_ROUNDS = 5,
};

static uint32_t bench_seed = 12345;

static uint32_t
bench_rng(uint32_t limit)
{
	bench_seed = bench_seed * 1103515245u + 12345u;
	return bench_seed % limit;
}

static uint32_t
test_field_name_hash(const char *str, uint32_t len)
{
	return str[0] + len;
}

/**
 * Create a key_def with the given field types.
 * @a types is an array of field_type values, terminated by field_type_MAX.
 */
static struct key_def *
make_key_def(const enum field_type *types, int count)
{
	struct key_part_def *parts = calloc(count, sizeof(*parts));
	for (int i = 0; i < count; i++) {
		parts[i] = key_part_def_default;
		parts[i].fieldno = i;
		parts[i].type = types[i];
	}
	struct key_def *def = key_def_new(parts, count, false);
	key_def_update_optionality(def, 0);
	free(parts);
	return def;
}

/** Encode a tuple [uint, uint, uint] into buf, return size. */
static size_t
encode_tuple_uuu(char *buf, size_t buf_size, uint64_t a, uint64_t b, uint64_t c)
{
	char *p = buf;
	p = mp_encode_array(p, 3);
	p = mp_encode_uint(p, a);
	p = mp_encode_uint(p, b);
	p = mp_encode_uint(p, c);
	(void)buf_size;
	return (size_t)(p - buf);
}

/** Encode a key [uint, uint] into buf, return size. */
static size_t
encode_key_uu(char *buf, size_t buf_size, uint64_t a, uint64_t b)
{
	char *p = buf;
	p = mp_encode_array(p, 2);
	p = mp_encode_uint(p, a);
	p = mp_encode_uint(p, b);
	(void)buf_size;
	return (size_t)(p - buf);
}

static double
now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void
bench_tuple_compare(struct key_def *def, struct tuple **tuples, int n_tuples)
{
	/*
	 * Pre-compute hints for all tuples.
	 */
	hint_t *hints = calloc(n_tuples, sizeof(*hints));
	for (int i = 0; i < n_tuples; i++)
		hints[i] = def->tuple_hint(tuples[i], def);

	/*
	 * Pre-generate random index pairs.
	 */
	uint32_t *idx_a = malloc(N_OPS * sizeof(*idx_a));
	uint32_t *idx_b = malloc(N_OPS * sizeof(*idx_b));
	for (int i = 0; i < N_OPS; i++) {
		idx_a[i] = bench_rng(n_tuples);
		idx_b[i] = bench_rng(n_tuples);
	}

	/* Warmup. */
	volatile int sink = 0;
	for (int i = 0; i < N_OPS; i++) {
		uint32_t a = idx_a[i], b = idx_b[i];
		sink += tuple_compare(tuples[a], hints[a],
				      tuples[b], hints[b], def);
	}

	double best = 1e18;
	for (int r = 0; r < N_ROUNDS; r++) {
		double t0 = now_sec();
		for (int i = 0; i < N_OPS; i++) {
			uint32_t a = idx_a[i], b = idx_b[i];
			sink += tuple_compare(tuples[a], hints[a],
					      tuples[b], hints[b], def);
		}
		double dt = now_sec() - t0;
		if (dt < best)
			best = dt;
	}
	printf("  tuple_compare:          %7.1f ns/op  %10.0f ops/sec\n",
	       best / N_OPS * 1e9, N_OPS / best);

	free(idx_a);
	free(idx_b);
	free(hints);
	(void)sink;
}

static void
bench_tuple_compare_with_key(struct key_def *def, struct tuple **tuples,
			     int n_tuples, char **keys, hint_t *key_hints,
			     int n_keys)
{
	/*
	 * Pre-compute tuple hints.
	 */
	hint_t *tuple_hints = calloc(n_tuples, sizeof(*tuple_hints));
	for (int i = 0; i < n_tuples; i++)
		tuple_hints[i] = def->tuple_hint(tuples[i], def);

	/*
	 * Pre-generate random pairs.
	 */
	uint32_t *t_idx = malloc(N_OPS * sizeof(*t_idx));
	uint32_t *k_idx = malloc(N_OPS * sizeof(*k_idx));
	for (int i = 0; i < N_OPS; i++) {
		t_idx[i] = bench_rng(n_tuples);
		k_idx[i] = bench_rng(n_keys);
	}

	/* Warmup. */
	volatile int sink = 0;
	uint32_t part_count = def->part_count;
	for (int i = 0; i < N_OPS; i++) {
		uint32_t t = t_idx[i], k = k_idx[i];
		sink += tuple_compare_with_key(tuples[t], tuple_hints[t],
					       keys[k], part_count,
					       key_hints[k], def);
	}

	double best = 1e18;
	for (int r = 0; r < N_ROUNDS; r++) {
		double t0 = now_sec();
		for (int i = 0; i < N_OPS; i++) {
			uint32_t t = t_idx[i], k = k_idx[i];
			sink += tuple_compare_with_key(
				tuples[t], tuple_hints[t],
				keys[k], part_count,
				key_hints[k], def);
		}
		double dt = now_sec() - t0;
		if (dt < best)
			best = dt;
	}
	printf("  tuple_compare_with_key: %7.1f ns/op  %10.0f ops/sec\n",
	       best / N_OPS * 1e9, N_OPS / best);

	free(t_idx);
	free(k_idx);
	free(tuple_hints);
	(void)sink;
}

static void
bench_uint_uint(int n_tuples, int card, const char *label,
		enum field_type type)
{
	printf("\n=== %s (N=%d, card=%d) ===\n\n", label, n_tuples, card);

	enum field_type types[] = {type, type};
	struct key_def *def = make_key_def(types, 2);

	int fanout = n_tuples / card;

	/* Create tuples. */
	struct tuple **tuples = calloc(n_tuples, sizeof(*tuples));
	for (int i = 0; i < n_tuples; i++) {
		char buf[64];
		size_t sz = encode_tuple_uuu(buf, sizeof(buf),
					     i % card,
					     i / card,
					     i);
		tuples[i] = tuple_new(tuple_format_runtime, buf, buf + sz);
		tuple_ref(tuples[i]);
	}

	/* Create keys. */
	int n_keys = n_tuples;
	char **keys = calloc(n_keys, sizeof(*keys));
	hint_t *key_hints = calloc(n_keys, sizeof(*key_hints));
	for (int i = 0; i < n_keys; i++) {
		char buf[64];
		uint64_t a = bench_rng(card);
		uint64_t b = bench_rng(fanout);
		size_t sz = encode_key_uu(buf, sizeof(buf), a, b);
		keys[i] = malloc(sz);
		memcpy(keys[i], buf, sz);
		const char *key_data = keys[i];
		uint32_t part_count = mp_decode_array(&key_data);
		key_hints[i] = def->key_hint(key_data, part_count, def);
	}

	bench_tuple_compare(def, tuples, n_tuples);
	bench_tuple_compare_with_key(def, tuples, n_tuples,
				     keys, key_hints, n_keys);

	/* Cleanup. */
	for (int i = 0; i < n_tuples; i++)
		tuple_unref(tuples[i]);
	for (int i = 0; i < n_keys; i++)
		free(keys[i]);
	free(tuples);
	free(keys);
	free(key_hints);
	key_def_delete(def);
}

/**
 * Encode a tuple [str, str, uint] into buf, return size.
 * Strings are 16 bytes with diverse prefixes.
 */
static size_t
encode_tuple_ssu(char *buf, size_t buf_size, uint32_t a, uint32_t b,
		 uint64_t c)
{
	char sa[17], sb[17];
	snprintf(sa, sizeof(sa), "%04x_pad_1234567", a);
	snprintf(sb, sizeof(sb), "%04x_pad_abcdefg", b);
	char *p = buf;
	p = mp_encode_array(p, 3);
	p = mp_encode_str(p, sa, 16);
	p = mp_encode_str(p, sb, 16);
	p = mp_encode_uint(p, c);
	(void)buf_size;
	return (size_t)(p - buf);
}

/** Encode a key [str, str] into buf, return size. */
static size_t
encode_key_ss(char *buf, size_t buf_size, uint32_t a, uint32_t b)
{
	char sa[17], sb[17];
	snprintf(sa, sizeof(sa), "%04x_pad_1234567", a);
	snprintf(sb, sizeof(sb), "%04x_pad_abcdefg", b);
	char *p = buf;
	p = mp_encode_array(p, 2);
	p = mp_encode_str(p, sa, 16);
	p = mp_encode_str(p, sb, 16);
	(void)buf_size;
	return (size_t)(p - buf);
}

static void
bench_str_str(int n_tuples, int card)
{
	printf("\n=== str+str (N=%d, card=%d) ===\n\n", n_tuples, card);

	enum field_type types[] = {FIELD_TYPE_STRING, FIELD_TYPE_STRING};
	struct key_def *def = make_key_def(types, 2);

	int fanout = n_tuples / card;

	struct tuple **tuples = calloc(n_tuples, sizeof(*tuples));
	for (int i = 0; i < n_tuples; i++) {
		char buf[128];
		size_t sz = encode_tuple_ssu(buf, sizeof(buf),
					     i % card, i / card, i);
		tuples[i] = tuple_new(tuple_format_runtime, buf, buf + sz);
		tuple_ref(tuples[i]);
	}

	int n_keys = n_tuples;
	char **keys = calloc(n_keys, sizeof(*keys));
	hint_t *key_hints = calloc(n_keys, sizeof(*key_hints));
	for (int i = 0; i < n_keys; i++) {
		char buf[128];
		size_t sz = encode_key_ss(buf, sizeof(buf),
					  bench_rng(card), bench_rng(fanout));
		keys[i] = malloc(sz);
		memcpy(keys[i], buf, sz);
		const char *key_data = keys[i];
		uint32_t part_count = mp_decode_array(&key_data);
		key_hints[i] = def->key_hint(key_data, part_count, def);
	}

	bench_tuple_compare(def, tuples, n_tuples);
	bench_tuple_compare_with_key(def, tuples, n_tuples,
				     keys, key_hints, n_keys);

	for (int i = 0; i < n_tuples; i++)
		tuple_unref(tuples[i]);
	for (int i = 0; i < n_keys; i++)
		free(keys[i]);
	free(tuples);
	free(keys);
	free(key_hints);
	key_def_delete(def);
}

/**
 * Generate a deterministic UUID from a seed value.
 * Spreads entropy across all bytes for realistic hint behavior.
 */
static struct tt_uuid
make_uuid(uint32_t seed)
{
	struct tt_uuid uu;
	uint32_t s = seed;
	for (int i = 0; i < 4; i++) {
		s = s * 1103515245u + 12345u;
		memcpy((char *)&uu + i * 4, &s, 4);
	}
	/* Mark as UUIDv4 (random). */
	uu.time_hi_and_version = (uu.time_hi_and_version & 0x0fff) | 0x4000;
	uu.clock_seq_hi_and_reserved =
		(uu.clock_seq_hi_and_reserved & 0x3f) | 0x80;
	return uu;
}

/** Encode a tuple [uuid, uint] into buf, return size. */
static size_t
encode_tuple_uuid_u(char *buf, size_t buf_size, struct tt_uuid *uu,
		    uint64_t val)
{
	char *p = buf;
	p = mp_encode_array(p, 2);
	p = mp_encode_uuid(p, uu);
	p = mp_encode_uint(p, val);
	(void)buf_size;
	return (size_t)(p - buf);
}

/** Encode a key [uuid] into buf, return size. */
static size_t
encode_key_uuid(char *buf, size_t buf_size, struct tt_uuid *uu)
{
	char *p = buf;
	p = mp_encode_array(p, 1);
	p = mp_encode_uuid(p, uu);
	(void)buf_size;
	return (size_t)(p - buf);
}

static void
bench_uuid(int n_tuples)
{
	printf("\n=== uuid (N=%d) ===\n\n", n_tuples);

	enum field_type types[] = {FIELD_TYPE_UUID, FIELD_TYPE_UNSIGNED};
	struct key_def *def = make_key_def(types, 2);

	/* Pre-generate UUIDs with diverse bytes. */
	struct tt_uuid *uuids = calloc(n_tuples, sizeof(*uuids));
	for (int i = 0; i < n_tuples; i++)
		uuids[i] = make_uuid(i);

	struct tuple **tuples = calloc(n_tuples, sizeof(*tuples));
	for (int i = 0; i < n_tuples; i++) {
		char buf[64];
		size_t sz = encode_tuple_uuid_u(buf, sizeof(buf),
						&uuids[i], i);
		tuples[i] = tuple_new(tuple_format_runtime, buf, buf + sz);
		tuple_ref(tuples[i]);
	}

	int n_keys = n_tuples;
	char **keys = calloc(n_keys, sizeof(*keys));
	hint_t *key_hints = calloc(n_keys, sizeof(*key_hints));
	for (int i = 0; i < n_keys; i++) {
		struct tt_uuid uu = make_uuid(bench_rng(n_tuples));
		char buf[64];
		size_t sz = encode_key_uuid(buf, sizeof(buf), &uu);
		keys[i] = malloc(sz);
		memcpy(keys[i], buf, sz);
		const char *key_data = keys[i];
		uint32_t part_count = mp_decode_array(&key_data);
		key_hints[i] = def->key_hint(key_data, part_count, def);
	}

	bench_tuple_compare(def, tuples, n_tuples);
	bench_tuple_compare_with_key(def, tuples, n_tuples,
				     keys, key_hints, n_keys);

	for (int i = 0; i < n_tuples; i++)
		tuple_unref(tuples[i]);
	for (int i = 0; i < n_keys; i++)
		free(keys[i]);
	free(uuids);
	free(tuples);
	free(keys);
	free(key_hints);
	key_def_delete(def);
}

int
main(void)
{
	memory_init();
	fiber_init(fiber_c_invoke);
	tuple_init(test_field_name_hash);

	/* Cache-hot: 2K tuples fit in L2, isolates comparison cost. */
	bench_uint_uint(2000, 10, "uint+uint card=10", FIELD_TYPE_UNSIGNED);
	bench_uint_uint(2000, 1, "uint+uint card=1", FIELD_TYPE_UNSIGNED);
	bench_uint_uint(2000, 10, "int+int card=10", FIELD_TYPE_INTEGER);
	bench_uint_uint(2000, 1, "int+int card=1", FIELD_TYPE_INTEGER);
	bench_str_str(2000, 10);
	bench_str_str(2000, 1);
	bench_uuid(2000);

	tuple_free();
	fiber_free();
	memory_free();
	return 0;
}
