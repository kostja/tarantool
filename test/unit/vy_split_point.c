/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "vy_iterators_helper.h"

#include "box/key_def.h"
#include "trivia/util.h"
#include "vy_range.h"
#include "vy_run.h"
#include <inttypes.h>
#include <limits.h>
#include <msgpuck.h>
#include <stdio.h>
#include <stdint.h>

static struct key_def *cmp_def;
static struct vy_run_env run_env;
const int MSGPACK_KEY_MAX = 1 + 9;
const int64_t NEG_INF = INT64_MIN;
const int64_t POS_INF = INT64_MAX;
static inline char *
mp_encode_i64(char *pos, int64_t value)
{
	if (value < 0)
		return mp_encode_int(pos, value);
	return mp_encode_uint(pos, (uint64_t)value);
}

const int64_t KEY_NOT_FOUND = INT64_MAX - 1;

static struct vy_entry
vy_key_new_i64(int64_t value)
{
	char buf[MSGPACK_KEY_MAX];
	char *pos = buf;
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_i64(pos, value);
	return vy_entry_key_from_msgpack(stmt_env.key_format, cmp_def, buf);
}

static struct vy_entry
vy_bound_new_i64(int64_t value, bool exclusive)
{
	struct vy_entry e = vy_key_new_i64(value);
	if (exclusive)
		vy_stmt_set_flags(e.stmt, VY_STMT_EXCLUSIVE_BOUND);
	return e;
}

enum slice_end_type {
	END_EX,
	END_IN,
};

struct vy_slice_i64 {
	int64_t begin;
	int64_t end;
	enum slice_end_type end_type;
	uint64_t bytes;
	/**
	 * If non-zero, used as run->info.max_key instead of @a end.
	 * Models slices whose boundaries were widened by a prior
	 * range split (range end > run->info.max_key).
	 */
	int64_t max_key;
};

const struct vy_slice_i64 VY_SLICE_LAST =
{ .begin = KEY_NOT_FOUND };

static struct vy_run *
vy_run_new_i64(int64_t min_key, int64_t max_key)
{
	struct vy_run *run = vy_run_new(&run_env, 1, NULL);
	fail_if(run == NULL);
	run->info.min_key = xmalloc(MSGPACK_KEY_MAX);
	run->info.max_key = xmalloc(MSGPACK_KEY_MAX);
	char *pos = run->info.min_key;
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_i64(pos, min_key);
	pos = run->info.max_key;
	pos = mp_encode_array(pos, 1);
	pos = mp_encode_i64(pos, max_key);
	return run;
}

static bool
raw_key_equal_i64(const char *raw_key, int64_t expected)
{
	if (raw_key == NULL)
		return false;
	const char *pos = raw_key;
	if (mp_decode_array(&pos) != 1)
		return false;
	if (mp_typeof(*pos) == MP_UINT)
		return (int64_t)mp_decode_uint(&pos) == expected;
	return mp_decode_int(&pos) == expected;
}

static bool
raw_key_to_i64(const char *raw_key, int64_t *value)
{
	if (raw_key == NULL)
		return false;
	const char *pos = raw_key;
	if (mp_decode_array(&pos) != 1)
		return false;
	if (mp_typeof(*pos) == MP_UINT) {
		*value = (int64_t)mp_decode_uint(&pos);
		return true;
	}
	*value = mp_decode_int(&pos);
	return true;
}

static const char *
format_key_bound(int64_t value, char *buf, size_t size)
{
	if (value == NEG_INF)
		return "-inf";
	if (value == POS_INF)
		return "+inf";
	snprintf(buf, size, "%" PRId64, value);
	return buf;
}

static void
log_split_case(const struct vy_slice_i64 *specs, int n,
	       const char *best_key)
{
	uint64_t total = 0;
	uint64_t left = 0;
	uint64_t right = 0;
	uint64_t active = 0;
	int64_t split_key = KEY_NOT_FOUND;
	bool has_key = raw_key_to_i64(best_key, &split_key);

	if (has_key) {
		for (int i = 0; i < n; i++) {
			total += specs[i].bytes;
			if (split_key <= specs[i].begin) {
				right += specs[i].bytes;
				continue;
			}
			bool to_left = specs[i].end_type == END_EX ?
				       split_key >= specs[i].end :
				       split_key > specs[i].end;
			if (to_left)
				left += specs[i].bytes;
			else
				active += specs[i].bytes;
		}
	} else {
		for (int i = 0; i < n; i++)
			total += specs[i].bytes;
	}

	fprintf(stderr, "# n=%d slices=", n);
	for (int i = 0; i < n; i++) {
		char begin_buf[32];
		char end_buf[32];
		const char *begin_label =
			format_key_bound(specs[i].begin, begin_buf,
					 sizeof(begin_buf));
		const char *end_label =
			format_key_bound(specs[i].end, end_buf,
					 sizeof(end_buf));
		fprintf(stderr,
			"%s[%s, %s%s %" PRIu64,
			i == 0 ? "" : ", ",
			begin_label, end_label,
			specs[i].end_type == END_IN ? "]" : ")",
			specs[i].bytes);
	}
	fprintf(stderr, "\n");
	if (has_key) {
		fprintf(stderr,
			"# key=%" PRId64 " active=%" PRIu64 " left=%" PRIu64
			" right=%" PRIu64 " total=%" PRIu64 "\n",
			split_key, active, left, right, total);
	} else {
		fprintf(stderr, "# key=NULL total=%" PRIu64 "\n", total);
	}
}

/** Build a range with slices from @a specs (VY_SLICE_LAST-terminated). */
static struct vy_range *
vy_test_range_new(const struct vy_slice_i64 *specs,
		  struct vy_slice ***p_slices, int *n)
{
	*n = 0;
	while (specs[*n].begin != VY_SLICE_LAST.begin)
		(*n)++;
	struct vy_slice **slices = xmalloc(sizeof(*slices) * *n);
	*p_slices = slices;
	struct vy_range *range =
		vy_range_new(1, vy_entry_none(), vy_entry_none(), cmp_def);
	fail_if(range == NULL);

	for (int i = 0; i < *n; i++) {
		int64_t mk = specs[i].max_key != 0 ?
			     specs[i].max_key : specs[i].end;
		struct vy_run *run = vy_run_new_i64(specs[i].begin, mk);
		slices[i] = vy_slice_new(1, run);
		fail_if(slices[i] == NULL);
		slices[i]->count.bytes = specs[i].bytes;
		/* Avoid the compaction randomization path. */
		slices[i]->seed = RAND_MAX;
		vy_range_add_slice(range, slices[i]);
		/*
		 * Set begin/end_bound after vy_range_add_slice,
		 * because vy_range_init_slice clears them first
		 * (and then returns early for runs with no pages).
		 *
		 * When range end is set and mk >= end (normal case),
		 * the boundary clips: EXCLUSIVE(range end).
		 * Otherwise (widened or unbounded): INCLUSIVE(max_key).
		 */
		if (specs[i].begin != NEG_INF)
			slices[i]->begin = vy_key_new_i64(specs[i].begin);
		if (specs[i].end_type == END_EX &&
		    specs[i].end != POS_INF && mk >= specs[i].end) {
			slices[i]->end_bound =
				vy_bound_new_i64(specs[i].end, true);
		} else {
			slices[i]->end_bound = vy_bound_new_i64(mk, false);
		}
	}
	return range;
}

/** Free slices and range created by vy_test_range_new. */
static void
vy_test_range_delete(struct vy_range *range, struct vy_slice **slices,
		     int n)
{
	for (int i = 0; i < n; i++)
		vy_run_unref(slices[i]->run);
	vy_range_delete(range);
	free(slices);
}

static void
run_split_case(const char *name, const struct vy_slice_i64 *specs,
	       int64_t expected)
{
	uint64_t range_size = 1024;
	int n;
	struct vy_slice **slices;
	struct vy_range *range = vy_test_range_new(specs, &slices, &n);

	const char *best_key = vy_range_find_best_split(range, range_size);
	if (expected == KEY_NOT_FOUND) {
		is(best_key, NULL, "%s", name);
	} else {
		ok(raw_key_equal_i64(best_key, expected), "%s", name);
	}
	log_split_case(specs, n, best_key);

	vy_test_range_delete(range, slices, n);
}

static void
test_vy_split_point_cmp(void)
{
	header();
	plan(3);

	struct vy_run *run = vy_run_new_i64(1, 10);

	struct vy_slice_i64 slice_spec_a = (struct vy_slice_i64)
	{ .begin = 1, .end = POS_INF, .end_type = END_IN, .bytes = 0, };
	struct vy_slice *slice_a = vy_slice_new(1, run);
	fail_if(slice_a == NULL);
	slice_a->count.bytes = slice_spec_a.bytes;
	slice_a->begin = vy_key_new_i64(slice_spec_a.begin);
	slice_a->end_bound = vy_bound_new_i64(10, false);
	struct vy_split_point p_key_a = {
		.slice = slice_a,
		.type = VY_SPLIT_POINT_BEGIN,
	};

	struct vy_slice_i64 slice_spec_b = (struct vy_slice_i64)
	{ .begin = 2, .end = POS_INF, .end_type = END_IN, .bytes = 0, };
	struct vy_slice *slice_b = vy_slice_new(1, run);
	fail_if(slice_b == NULL);
	slice_b->count.bytes = slice_spec_b.bytes;
	slice_b->begin = vy_key_new_i64(slice_spec_b.begin);
	slice_b->end_bound = vy_bound_new_i64(10, false);
	struct vy_split_point p_key_b = {
		.slice = slice_b,
		.type = VY_SPLIT_POINT_BEGIN,
	};

	ok(vy_split_point_cmp(&p_key_a, &p_key_b, cmp_def) < 0,
	   "different keys use key comparison");

	struct vy_slice_i64 slice_spec_same = (struct vy_slice_i64)
	{ .begin = 1, .end = 1, .end_type = END_EX, .bytes = 0, };
	struct vy_slice *slice_same = vy_slice_new(1, run);
	fail_if(slice_same == NULL);
	slice_same->count.bytes = slice_spec_same.bytes;
	slice_same->begin = vy_key_new_i64(slice_spec_same.begin);
	/* range_end(1) <= max_key(10) -> EXCLUSIVE */
	slice_same->end_bound = vy_bound_new_i64(slice_spec_same.end, true);

	struct vy_split_point p_begin = {
		.slice = slice_same,
		.type = VY_SPLIT_POINT_BEGIN,
	};
	struct vy_split_point p_excl_end = {
		.slice = slice_same,
		.type = VY_SPLIT_POINT_END,
	};

	ok(vy_split_point_cmp(&p_begin, &p_excl_end, cmp_def) < 0,
	   "equal keys order begin < end");

	/* Inclusive end at key 1: use slice_a which has INCLUSIVE end_bound. */
	struct vy_split_point p_incl_end = {
		.slice = slice_a,
		.type = VY_SPLIT_POINT_END,
	};
	/*
	 * slice_a->end_bound is INCLUSIVE(10), but for this test we need
	 * an inclusive end at key 1.  Use a temporary override.
	 */
	struct vy_entry saved = slice_a->end_bound;
	slice_a->end_bound = vy_bound_new_i64(1, false);
	ok(vy_split_point_cmp(&p_excl_end, &p_incl_end, cmp_def) < 0,
	   "equal keys order exclusive end < inclusive end");
	tuple_unref(slice_a->end_bound.stmt);
	slice_a->end_bound = saved;

	vy_slice_delete(slice_a);
	vy_slice_delete(slice_b);
	vy_slice_delete(slice_same);
	vy_run_unref(run);

	check_plan();
	footer();
}

static void
test_vy_range_find_best_split(void)
{
	header();

	plan(30);

	/*
	 * Split tests are expressed declaratively as slices with
	 * begin/end keys, explicit end types, and weights, plus the
	 * expected split key.
	 */
	struct vy_slice_i64 case_empty[] = {
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("empty range", case_empty, KEY_NOT_FOUND);

	struct vy_slice_i64 case_single_excl[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("single slice exclusive",
		       case_single_excl, KEY_NOT_FOUND);

	struct vy_slice_i64 case_single_incl[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = POS_INF, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("single slice inclusive",
		       case_single_incl, KEY_NOT_FOUND);

	struct vy_slice_i64 case_two_disjoint[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("two disjoint slices",
		       case_two_disjoint, 20);

	struct vy_slice_i64 case_two_identical[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("two identical slices",
		       case_two_identical, KEY_NOT_FOUND);

	struct vy_slice_i64 case_three_identical[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("three identical slices",
		       case_three_identical, KEY_NOT_FOUND);

	struct vy_slice_i64 case_duplicate_begins[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 30, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("duplicate begin keys",
		       case_duplicate_begins, KEY_NOT_FOUND);

	struct vy_slice_i64 case_duplicate_ends[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 30, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 30, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("duplicate end keys",
		       case_duplicate_ends, KEY_NOT_FOUND);

	struct vy_slice_i64 case_duplicate_ends_balanced[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 15, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 200, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("duplicate end keys still balance",
		       case_duplicate_ends_balanced, 20);

	struct vy_slice_i64 case_duplicate_inclusive_ends[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = POS_INF, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = POS_INF, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("duplicate inclusive end keys",
		       case_duplicate_inclusive_ends, KEY_NOT_FOUND);

	struct vy_slice_i64 case_incl_excl_same_end[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 50, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 50, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("inclusive end equals exclusive end",
		       case_incl_excl_same_end, KEY_NOT_FOUND);

	struct vy_slice_i64 case_incl_end_equals_begin[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 50, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 70, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("inclusive end equals another begin",
		       case_incl_end_equals_begin, KEY_NOT_FOUND);

	struct vy_slice_i64 case_excl_end_equals_begin[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 50, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 70, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("exclusive end equals another begin",
		       case_excl_end_equals_begin, KEY_NOT_FOUND);

	struct vy_slice_i64 case_three_duplicate_begins[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 30, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("three equal begin keys",
		       case_three_duplicate_begins, KEY_NOT_FOUND);

	struct vy_slice_i64 case_three_inclusive_ends[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 50, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 50, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 50, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("three inclusive end keys",
		       case_three_inclusive_ends, KEY_NOT_FOUND);

	struct vy_slice_i64 case_three_weighted[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 60, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 60, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("three slices, split after first",
		       case_three_weighted, 20);

	struct vy_slice_i64 case_too_small_range[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 5, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 30, .end_type = END_EX, .bytes = 5, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 20, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("reject split below one-third",
		       case_too_small_range, KEY_NOT_FOUND);

	struct vy_slice_i64 case_four_equal[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 60, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 70, .end = 80, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("four equal slices, split after second",
		       case_four_equal, 40);

	struct vy_slice_i64 case_five_equal[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 60, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 70, .end = 80, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 90, .end = 100, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("five equal slices, split after second",
		       case_five_equal, 40);

	struct vy_slice_i64 case_six_equal[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 60, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 70, .end = 80, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 90, .end = 100, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 110, .end = 120, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("six equal slices, split after third",
		       case_six_equal, 60);

	struct vy_slice_i64 case_seven_equal[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 60, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 70, .end = 80, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 90, .end = 100, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 110, .end = 120, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 130, .end = 140, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("seven equal slices, split after third",
		       case_seven_equal, 60);

	struct vy_slice_i64 case_with_inclusive_end[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = POS_INF, .end_type = END_IN, .bytes = 180, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("inclusive end does not affect candidate keys",
		       case_with_inclusive_end, 20);

	struct vy_slice_i64 case_weighted_disjoint[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 200, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 50, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = 60, .end_type = END_EX, .bytes = 200, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("weighted disjoint chooses earliest balance",
		       case_weighted_disjoint, 20);

	struct vy_slice_i64 case_inclusive_between_candidates[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_IN, .bytes = 90, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 30, .end_type = END_IN, .bytes = 80, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 40, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 40, .end = 50, .end_type = END_EX, .bytes = 200, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("inclusive ends shift weight between candidates",
		       case_inclusive_between_candidates, 40);

	struct vy_slice_i64 case_equal_point_shift[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 30, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 0, .end = 20, .end_type = END_IN, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 200, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("equal points shift weight correctly",
		       case_equal_point_shift, 30);

	struct vy_slice_i64 case_active_preference[] = {
		(struct vy_slice_i64)
		{ .begin = 0, .end = 10, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 30, .end_type = END_EX, .bytes = 30, },
		(struct vy_slice_i64)
		{ .begin = 0, .end = 20, .end_type = END_IN, .bytes = 20, },
		(struct vy_slice_i64)
		{ .begin = 0, .end = 25, .end_type = END_IN, .bytes = 20, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("equal balance prefers lower active",
		       case_active_preference, 30);

	struct vy_slice_i64 case_nested_overlap[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 50, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 30, .end_type = END_EX, .bytes = 60, },
		(struct vy_slice_i64)
		{ .begin = 60, .end = 70, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("nested overlap splits after outer end",
		       case_nested_overlap, 50);

	struct vy_slice_i64 case_overlap_balanced[] = {
		(struct vy_slice_i64)
		{ .begin = 10, .end = 20, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 40, .end_type = END_EX, .bytes = 120, },
		(struct vy_slice_i64)
		{ .begin = 15, .end = 35, .end_type = END_EX, .bytes = 60, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("overlap still chooses balanced candidate",
		       case_overlap_balanced, 20);

	struct vy_slice_i64 case_inf_bounds[] = {
		(struct vy_slice_i64)
		{ .begin = NEG_INF, .end = 50, .end_type = END_EX, .bytes = 150, },
		(struct vy_slice_i64)
		{ .begin = 50, .end = POS_INF, .end_type = END_IN, .bytes = 150, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("range split between -inf and +inf",
		       case_inf_bounds, KEY_NOT_FOUND);

	/*
	 * Pyramid-like overlaps keep the balance heuristic from selecting
	 * a split, so it falls back to the mid-key strategy.
	 */
	struct vy_slice_i64 case_pyramid[] = {
		(struct vy_slice_i64)
		{ .begin = NEG_INF, .end = POS_INF, .end_type = END_IN, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 10, .end = 90, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 20, .end = 80, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		{ .begin = 30, .end = 70, .end_type = END_EX, .bytes = 100, },
		(struct vy_slice_i64)
		VY_SLICE_LAST,
	};
	run_split_case("pyramid overlaps force mid-key fallback",
		       case_pyramid, KEY_NOT_FOUND);

	check_plan();
	footer();
}

/**
 * Test that the sweep heuristic correctly handles slices whose
 * boundaries were widened by a prior range split.
 *
 * After vy_slice_cut, a child slice gets its range end set to the
 * split key, which may be far beyond run->info.max_key.  The fix
 * emits an INCLUSIVE_END at run->info.max_key (where the data
 * really ends) in addition to an EXCLUSIVE_END at the range end.
 * Without this, the sweep overestimates "active" weight and fails
 * to find valid split points.
 */
static void
test_split_with_widened_slice_end(void)
{
	header();
	plan(2);

	/*
	 * Two disjoint data slices whose range end was widened
	 * to 50 by a prior split.  Actual data lives at [1,10]
	 * and [20,30].  Without the fix both slices appear to
	 * span the full [begin, 50) range and no split is found.
	 * With the fix, the sweep sees INCLUSIVE_END at 10 and 30,
	 * finds a balanced split at key 20.
	 */
	struct vy_slice_i64 case_widened_end[] = {
		{ .begin = 1, .end = 50, .end_type = END_EX,
		  .bytes = 100, .max_key = 10, },
		{ .begin = 20, .end = 50, .end_type = END_EX,
		  .bytes = 100, .max_key = 30, },
		VY_SLICE_LAST,
	};
	run_split_case("widened range end: split at data boundary",
		       case_widened_end, 20);

	/*
	 * Three slices: two data slices with widened ends plus an
	 * anchor at the far end (the realistic scenario after
	 * splitting a range with a disjoint anchor run).
	 */
	struct vy_slice_i64 case_widened_with_anchor[] = {
		{ .begin = 1, .end = 5000, .end_type = END_EX,
		  .bytes = 100, .max_key = 10, },
		{ .begin = 20, .end = 5000, .end_type = END_EX,
		  .bytes = 100, .max_key = 30, },
		{ .begin = 5000, .end = POS_INF, .end_type = END_IN,
		  .bytes = 50, },
		VY_SLICE_LAST,
	};
	run_split_case("widened ends + anchor: split at data boundary",
		       case_widened_with_anchor, 20);

	check_plan();
	footer();
}

/**
 * Helper: create a slice over a run with keys [run_min, run_max],
 * set slice boundaries to [slice_begin, slice_end), call vy_slice_cut
 * with target range [cut_begin, cut_end), and check the result.
 *
 * Pass NEG_INF/POS_INF for cut_begin/cut_end to use a NULL entry
 * (unbounded).
 */
static void
slice_cut_check(const char *name,
		int64_t cut_begin, int64_t cut_end,
		int64_t slice_begin, int64_t slice_end,
		int64_t run_min, int64_t run_max, bool expect_hit)
{
	struct vy_run *run = vy_run_new_i64(run_min, run_max);
	struct vy_slice *slice = vy_slice_new(1, run);
	fail_if(slice == NULL);
	slice->begin = vy_key_new_i64(slice_begin);
	/*
	 * Compute end_bound like vy_range_init_slice would:
	 * slice_end <= max_key -> EXCLUSIVE(slice_end),
	 * otherwise -> INCLUSIVE(max_key).
	 */
	if (slice_end <= run_max)
		slice->end_bound = vy_bound_new_i64(slice_end, true);
	else
		slice->end_bound = vy_bound_new_i64(run_max, false);

	struct vy_entry begin = { .stmt = NULL };
	struct vy_entry end = { .stmt = NULL };
	if (cut_begin != NEG_INF)
		begin = vy_key_new_i64(cut_begin);
	if (cut_end != POS_INF)
		end = vy_key_new_i64(cut_end);

	struct vy_slice *result = NULL;
	int rc = vy_slice_cut(slice, 99, begin, end, cmp_def, &result);
	is(rc, 0, "%s: no error", name);
	if (expect_hit)
		ok(result != NULL, "%s: intersection", name);
	else
		is(result, NULL, "%s: no intersection", name);

	if (result != NULL)
		vy_slice_delete(result);
	if (begin.stmt != NULL)
		tuple_unref(begin.stmt);
	if (end.stmt != NULL)
		tuple_unref(end.stmt);
	vy_slice_delete(slice);
	vy_run_unref(run);
}

/**
 * Test vy_slice_cut boundary conditions.
 *
 * Conventions (matching vinyl range semantics):
 *   slice->begin, run->min_key  — inclusive
 *   range end                   — exclusive
 *   run->max_key                — inclusive
 *   vy_slice_cut begin          — inclusive
 *   vy_slice_cut end            — exclusive
 *
 * After a range split, slice boundaries (begin/range end) may be far
 * wider than the run's actual key range [min_key, max_key].
 * vy_slice_cut must check against BOTH the slice boundaries and the
 * run's actual key range to avoid creating phantom slices.
 */
static void
test_vy_slice_cut_boundaries(void)
{
	header();
	plan(14);

	/* Begin-side: run [1,10], vary range end and cut begin. */

	slice_cut_check("begin(11) > max_key(10)",
			11, POS_INF,    /* cut */
			1, 5001,        /* slice */
			1, 10,          /* run */
			false);
	slice_cut_check("begin(10) == max_key(10)",
			10, POS_INF,
			1, 5001,
			1, 10,
			true);
	slice_cut_check("begin(10) == range_end(10) excl",
			10, POS_INF,
			1, 10,
			1, 10,
			false);
	slice_cut_check("begin(10) == max_key, range_end(11)",
			10, POS_INF,
			1, 11,
			1, 10,
			true);
	slice_cut_check("begin(11) == range_end(11) excl",
			11, POS_INF,
			1, 11,
			1, 10,
			false);

	/* End-side: run [10,100], vary slice->begin and cut end. */

	slice_cut_check("end(10) == slice->begin(10) incl",
			NEG_INF, 10,
			10, 200,
			10, 100,
			false);
	slice_cut_check("end(11) > slice->begin(10)",
			NEG_INF, 11,
			10, 200,
			10, 100,
			true);

	check_plan();
	footer();
}

/**
 * Helper for trim tests.  Builds a range with slices described by
 * @specs, calls vy_range_update_compaction_priority (which runs
 * vy_compaction_plan_check_shape), then checks
 * that the resulting plan count matches @expected_count.
 *
 * @specs must end with a VY_SLICE_LAST sentinel.
 * All slices must have the same bytes so they land on the same LSM
 * level and trigger compaction with run_count_per_level = 1.
 */
static void
run_trim_case(const char *name, const struct vy_slice_i64 *specs,
	      int expected_count)
{
	int n;
	struct vy_slice **slices;
	struct vy_range *range = vy_test_range_new(specs, &slices, &n);

	struct index_opts opts;
	index_opts_create(&opts);
	opts.run_count_per_level = 1;
	opts.run_size_ratio = 2;
	/*
	 * Use a large range_size to prevent the split heuristic
	 * from firing — we only want to test trim here.
	 */
	opts.range_size = INT64_MAX / 2;

	vy_range_update_compaction_priority(range, &opts, opts.range_size);
	is(range->compaction_plan.count, expected_count, "%s", name);

	vy_test_range_delete(range, slices, n);
}

static void
test_vy_compaction_plan_trim(void)
{
	header();
	plan(4);

	/*
	 * Case 1: all slices overlap.
	 * Three slices with overlapping [begin, max_key] ranges.
	 * Expect all 3 kept.
	 */
	struct vy_slice_i64 case_all_overlap[] = {
		{ .begin = 1, .end = 50, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 20, .end = 70, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 40, .end = 90, .end_type = END_IN, .bytes = 8192 },
		VY_SLICE_LAST,
	};
	run_trim_case("all slices overlap: keep all 3",
		      case_all_overlap, 3);

	/*
	 * Case 2: no slices overlap.
	 * Trim finds no cluster larger than 1, so the plan
	 * is trimmed to 0 slices (a single slice has nothing
	 * to merge with).
	 */
	struct vy_slice_i64 case_no_overlap[] = {
		{ .begin = 1, .end = 10, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 20, .end = 30, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 40, .end = 50, .end_type = END_IN, .bytes = 8192 },
		VY_SLICE_LAST,
	};
	run_trim_case("no overlap: trimmed to 0",
		      case_no_overlap, 0);

	/*
	 * Case 3: two overlap, one disjoint.
	 * Slices A [1,50] and B [20,70] overlap; C [200,300]
	 * is disjoint.  Largest cluster is {A, B} with size 2.
	 */
	struct vy_slice_i64 case_mixed[] = {
		{ .begin = 1, .end = 50, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 20, .end = 70, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 200, .end = 300, .end_type = END_IN, .bytes = 8192 },
		VY_SLICE_LAST,
	};
	run_trim_case("two overlap, one disjoint: keep 2",
		      case_mixed, 2);

	/*
	 * Case 4: two disjoint clusters, pick larger.
	 * Cluster 1: A [1,30] + B [20,50] (size 2).
	 * Cluster 2: C [100,150] + D [120,170] + E [140,200]
	 * (size 3).
	 * F [500,600] is alone.
	 * Expected: cluster 2 wins with 3 slices.
	 */
	struct vy_slice_i64 case_two_clusters[] = {
		{ .begin = 1, .end = 30, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 20, .end = 50, .end_type = END_IN, .bytes = 8192 },
		{ .begin = 100, .end = 150, .end_type = END_IN,
		  .bytes = 8192 },
		{ .begin = 120, .end = 170, .end_type = END_IN,
		  .bytes = 8192 },
		{ .begin = 140, .end = 200, .end_type = END_IN,
		  .bytes = 8192 },
		{ .begin = 500, .end = 600, .end_type = END_IN,
		  .bytes = 8192 },
		VY_SLICE_LAST,
	};
	run_trim_case("two clusters, pick larger (3 slices)",
		      case_two_clusters, 3);

	check_plan();
	footer();
}

/**
 * Test that vy_range_update_compaction_priority does not run the
 * bloat check when a split is already scheduled.
 *
 * Without the guard, vy_compaction_plan_check_bloat adds a slice
 * to the plan (count > 0) while split_key is also set.
 * vy_compaction_plan_seal then hits assert(plan->count == 0)
 * in the split_key branch.
 */
static void
test_bloat_guard_with_split(void)
{
	header();
	plan(3);

	/*
	 * Two disjoint slices large enough to trigger a split.
	 * One of the runs is bloated (unreferenced pages).
	 * range_size is set so that total bytes > range_size * 3/2
	 * and the sweep finds a split point.
	 */
	struct vy_slice_i64 specs[] = {
		{ .begin = 10, .end = 20, .end_type = END_EX,
		  .bytes = 1000, },
		{ .begin = 30, .end = 40, .end_type = END_EX,
		  .bytes = 1000, },
		VY_SLICE_LAST,
	};
	int n;
	struct vy_slice **slices;
	struct vy_range *range = vy_test_range_new(specs, &slices, &n);

	/*
	 * Make the first slice's run bloated: 100 pages in the run
	 * file but only 10 referenced by live slices.  The bloat
	 * threshold is MAX(2, slice_pages / 10).  With 20 slice
	 * pages, threshold = 2, waste = (100 - 10) / 1 = 90 > 2.
	 */
	struct vy_run *run = slices[0]->run;
	run->info.page_count = 100;
	run->referenced_pages = 10;
	slices[0]->count.pages = 20;

	struct index_opts opts;
	index_opts_create(&opts);
	opts.run_count_per_level = 100; /* prevent shape compaction */

	/*
	 * range_size must be small enough that range->count.bytes
	 * (2000) >= range_size * 3/2, triggering vy_range_needs_split.
	 * With range_size = 1024, 2000 >= 1536.
	 */
	int64_t range_size = 1024;

	/*
	 * Without the fix this would crash with:
	 * assert(plan->count == 0) in vy_compaction_plan_seal.
	 */
	vy_range_update_compaction_priority(range, &opts, range_size);

	ok(range->compaction_plan.split_key != NULL,
	   "split is scheduled");
	is(range->compaction_plan.count, 0,
	   "bloat check skipped (count == 0)");
	ok(!range->compaction_plan.is_bloat,
	   "bloat flag not set");

	vy_test_range_delete(range, slices, n);

	check_plan();
	footer();
}

int
main(void)
{
	plan(6);
	header();

	vy_iterator_C_test_init(128 * 1024);
	vy_run_env_create(&run_env, stmt_env.key_format, 0);
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_INTEGER };
	cmp_def = box_key_def_new(fields, types, 1);
	fail_if(cmp_def == NULL);

	test_vy_split_point_cmp();
	test_vy_range_find_best_split();
	test_split_with_widened_slice_end();
	test_vy_slice_cut_boundaries();
	test_vy_compaction_plan_trim();
	test_bloat_guard_with_split();

	key_def_delete(cmp_def);
	vy_run_env_destroy(&run_env);
	vy_iterator_C_test_finish();
	footer();
	return check_plan();
}
