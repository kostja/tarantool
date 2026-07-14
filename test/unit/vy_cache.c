#include "trivia/util.h"
#include "vy_iterators_helper.h"
#include "fiber.h"

const struct vy_stmt_template key_template = STMT_TEMPLATE(0, SELECT, vyend);

static void
test_basic(void)
{
	header();
	plan(6);
	struct vy_cache cache;
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def;
	struct tuple_format *format;
	create_test_cache(fields, types, lengthof(fields), &cache, &key_def,
			  &format);
	struct vy_entry select_all = vy_new_simple_stmt(format, key_def,
							&key_template);

	/*
	 * Fill the cache with 3 chains.
	 */
	const struct vy_stmt_template chain1[] = {
		STMT_TEMPLATE(1, REPLACE, 100),
		STMT_TEMPLATE(2, REPLACE, 200),
		STMT_TEMPLATE(3, REPLACE, 300),
		STMT_TEMPLATE(4, REPLACE, 400),
		STMT_TEMPLATE(5, REPLACE, 500),
		STMT_TEMPLATE(6, REPLACE, 600),
	};
	vy_cache_insert_templates_chain(&cache, format, chain1,
					lengthof(chain1), &key_template,
					ITER_GE);
	is(vy_cache_tree_size(cache.tree), 8,
	   "cache holds 6 statements and 2 edge key entries");

	const struct vy_stmt_template chain2[] = {
		STMT_TEMPLATE(10, REPLACE, 1001),
		STMT_TEMPLATE(11, REPLACE, 1002),
		STMT_TEMPLATE(12, REPLACE, 1003),
		STMT_TEMPLATE(13, REPLACE, 1004),
		STMT_TEMPLATE(14, REPLACE, 1005),
		STMT_TEMPLATE(15, REPLACE, 1006),
	};
	vy_cache_insert_templates_chain(&cache, format, chain2,
					lengthof(chain2), &key_template,
					ITER_GE);
	is(vy_cache_tree_size(cache.tree), 14,
	   "cache holds 12 statements and 2 edge key entries");

	const struct vy_stmt_template chain3[] = {
		STMT_TEMPLATE(16, REPLACE, 1107),
		STMT_TEMPLATE(17, REPLACE, 1108),
		STMT_TEMPLATE(18, REPLACE, 1109),
		STMT_TEMPLATE(19, REPLACE, 1110),
		STMT_TEMPLATE(20, REPLACE, 1111),
		STMT_TEMPLATE(21, REPLACE, 1112),
	};
	vy_cache_insert_templates_chain(&cache, format, chain3,
					lengthof(chain3), &key_template,
					ITER_GE);
	is(vy_cache_tree_size(cache.tree), 20,
	   "cache holds 18 statements and 2 edge key entries");

	/*
	 * Try to restore opened and positioned iterator.
	 * At first, start the iterator and make several iteration
	 * steps.
	 * At second, change cache version be insertion a new
	 * statement.
	 * At third, restore the opened on the first step
	 * iterator on the several statements back.
	 *
	 *    Key1   Key2   NewKey   Key3   Key4   Key5
	 *     ^              ^              ^
	 * restore to      new stmt     current position
	 *     |                             |
	 *     +- - - - < - - - - < - - - - -+
	 */
	struct vy_cache_iterator itr;
	struct vy_read_view rv;
	rv.vlsn = INT64_MAX;
	const struct vy_read_view *rv_p = &rv;
	/*
	 * The iterator borrows its start bound from a builder.
	 * A below-latest builder builds no chain, leaving the
	 * cache under test unchanged.
	 */
	struct vy_read_view gated_rv;
	gated_rv.vlsn = 0;
	const struct vy_read_view *gated_rv_p = &gated_rv;
	struct vy_cache_builder builder;
	vy_cache_builder_create(&builder, &cache, ITER_GE, select_all,
				vy_entry_none(), &gated_rv_p);
	vy_cache_iterator_open(&itr, &cache, ITER_GE, select_all,
			       &rv_p, &builder);

	/* Start iterator and make several steps. */
	struct vy_entry ret;
	bool unused;
	struct vy_history history;
	vy_history_create(&history, &history_node_pool);
	for (int i = 0; i < 4; ++i)
		vy_cache_iterator_next(&itr, &history, &unused);
	ret = vy_history_last_stmt(&history);
	ok(vy_stmt_are_same(ret, &chain1[3], format, key_def),
	   "next_key * 4");

	/*
	 * Emulate new statement insertion: break the first chain
	 * and insert into the cache the new statement.
	 */
	const struct vy_stmt_template to_insert =
		STMT_TEMPLATE(22, REPLACE, 201);
	vy_cache_on_write_template(&cache, format, &to_insert);
	vy_cache_insert_templates_chain(&cache, format, &to_insert, 1,
					&key_template, ITER_GE);

	/*
	 * Restore after the cache had changed. Restoration
	 * makes position of the iterator be one statement after
	 * the last. So restore on chain1[0], but the result
	 * must be chain1[1].
	 */
	struct vy_entry last = vy_new_simple_stmt(format, key_def, &chain1[0]);
	ok(vy_cache_iterator_restore(&itr, last, &history, &unused) >= 0,
	   "restore");
	ret = vy_history_last_stmt(&history);
	ok(vy_stmt_are_same(ret, &chain1[1], format, key_def),
	   "restore on position after last");
	tuple_unref(last.stmt);

	vy_history_cleanup(&history);
	vy_cache_iterator_close(&itr);
	vy_cache_builder_destroy(&builder);
	tuple_unref(select_all.stmt);
	destroy_test_cache(&cache, key_def, format);
	check_plan();
	footer();
}

static const char *
lsn_str(int64_t lsn)
{
	char *buf = tt_static_buf();
	if (lsn == INT64_MAX) {
		return "INT64_MAX";
	} else if (lsn > MAX_LSN) {
		snprintf(buf, TT_STATIC_BUF_LEN, "MAX_LSN+%lld",
			 (long long)(lsn - MAX_LSN));
	} else {
		snprintf(buf, TT_STATIC_BUF_LEN, "%lld", (long long)lsn);
	}
	return buf;
}

static const char *
iterator_type_str(int type)
{
	switch (type) {
	case ITER_EQ: return "EQ";
	case ITER_GE: return "GE";
	case ITER_GT: return "GT";
	case ITER_LE: return "LE";
	case ITER_LT: return "LT";
	default:
		unreachable();
	}
}

struct test_iterator_expected {
	struct vy_stmt_template stmt;
	bool stop;
};

static void
test_iterator_helper(
		struct vy_cache *cache, struct key_def *key_def,
		struct tuple_format *format, enum iterator_type type,
		const struct vy_stmt_template *key_template,
		int64_t vlsn,
		const struct test_iterator_expected *expected,
		int expected_count, bool expected_stop)
{
	struct vy_read_view rv;
	rv.vlsn = vlsn;
	const struct vy_read_view *prv = &rv;
	struct vy_cache_iterator it;
	struct vy_history history;
	vy_history_create(&history, &history_node_pool);
	struct vy_entry key = vy_new_simple_stmt(format, key_def,
						 key_template);
	/*
	 * The iterator borrows its start bound from a builder.
	 * A below-latest builder builds no chain, leaving the
	 * cache under test unchanged.
	 */
	struct vy_read_view gated_rv;
	gated_rv.vlsn = 0;
	const struct vy_read_view *gated_rv_p = &gated_rv;
	struct vy_cache_builder builder;
	vy_cache_builder_create(&builder, cache, type, key,
				vy_entry_none(), &gated_rv_p);
	vy_cache_iterator_open(&it, cache, type, key, &prv, &builder);
	int i;
	bool stop;
	for (i = 0; ; i++) {
		stop = false;
		fail_unless(vy_cache_iterator_next(&it, &history, &stop) == 0);
		struct vy_entry entry = vy_history_last_stmt(&history);
		if (vy_entry_is_equal(entry, vy_entry_none()))
			break;
		ok(i < expected_count && stop == expected[i].stop &&
		   vy_stmt_are_same(entry, &expected[i].stmt, format, key_def),
		   "type=%s key=%s vlsn=%s stmt=%s stop=%s",
		   iterator_type_str(type), tuple_str(key.stmt), lsn_str(vlsn),
		   vy_stmt_str(entry.stmt), stop ? "true" : "false");
	}
	ok(i == expected_count && stop == expected_stop,
	   "type=%s key=%s vlsn=%s eof stop=%s",
	   iterator_type_str(type), tuple_str(key.stmt), lsn_str(vlsn),
	   stop ? "true" : "false");
	vy_cache_iterator_close(&it);
	vy_cache_builder_destroy(&builder);
	vy_history_cleanup(&history);
	tuple_unref(key.stmt);
}

static void
test_prepared_not_cached(void)
{
	header();
	plan(20);
	struct vy_cache cache;
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def;
	struct tuple_format *format;
	create_test_cache(fields, types, lengthof(fields), &cache, &key_def,
			  &format);
	struct vy_stmt_template chain[] = {
		STMT_TEMPLATE(10, REPLACE, 100),
		STMT_TEMPLATE(20, REPLACE, 200),
		STMT_TEMPLATE(MAX_LSN + 10, REPLACE, 300),
		STMT_TEMPLATE(MAX_LSN + 20, REPLACE, 400),
		STMT_TEMPLATE(15, REPLACE, 500),
		STMT_TEMPLATE(25, REPLACE, 600),
		STMT_TEMPLATE(MAX_LSN + 15, REPLACE, 700),
	};
	/*
	 * Prepared statements never enter the cache: the chain must
	 * come out holding only the committed statements, with links
	 * broken where a prepared statement sat, so a reader at any
	 * read view sees only committed data.
	 */
	vy_cache_insert_templates_chain(&cache, format, chain, lengthof(chain),
					&key_template, ITER_GE);
	/* type=GE vlsn=20 */
	{
		struct test_iterator_expected expected[] = {
			{STMT_TEMPLATE(10, REPLACE, 100), true},
			{STMT_TEMPLATE(20, REPLACE, 200), true},
			{STMT_TEMPLATE(15, REPLACE, 500), false},
		};
		test_iterator_helper(&cache, key_def, format, ITER_GE,
				     &key_template, /*vlsn=*/20,
				     expected, lengthof(expected),
				     /*expected_stop=*/false);
	}
	/* type=GE vlsn=MAX_LSN+10 */
	{
		struct test_iterator_expected expected[] = {
			{STMT_TEMPLATE(10, REPLACE, 100), true},
			{STMT_TEMPLATE(20, REPLACE, 200), true},
			{STMT_TEMPLATE(15, REPLACE, 500), false},
			{STMT_TEMPLATE(25, REPLACE, 600), true},
		};
		test_iterator_helper(&cache, key_def, format, ITER_GE,
				     &key_template, /*vlsn=*/MAX_LSN + 10,
				     expected, lengthof(expected),
				     /*expected_stop=*/false);
	}
	/* type=LE vlsn=20 */
	{
		struct test_iterator_expected expected[] = {
			{STMT_TEMPLATE(15, REPLACE, 500), false},
			{STMT_TEMPLATE(20, REPLACE, 200), false},
			{STMT_TEMPLATE(10, REPLACE, 100), true},
		};
		test_iterator_helper(&cache, key_def, format, ITER_LE,
				     &key_template, /*vlsn=*/20,
				     expected, lengthof(expected),
				     /*expected_stop=*/true);
	}
	/* type=LE vlsn=MAX_LSN+10 */
	{
		struct test_iterator_expected expected[] = {
			{STMT_TEMPLATE(25, REPLACE, 600), false},
			{STMT_TEMPLATE(15, REPLACE, 500), true},
			{STMT_TEMPLATE(20, REPLACE, 200), false},
			{STMT_TEMPLATE(10, REPLACE, 100), true},
		};
		test_iterator_helper(&cache, key_def, format, ITER_LE,
				     &key_template, /*vlsn=*/MAX_LSN + 10,
				     expected, lengthof(expected),
				     /*expected_stop=*/true);
	}
	/* type=EQ key=300 vlsn=20 */
	{
		struct vy_stmt_template key = STMT_TEMPLATE(0, SELECT, 300);
		struct test_iterator_expected expected[] = {};
		test_iterator_helper(&cache, key_def, format, ITER_EQ,
				     &key, /*vlsn=*/20,
				     expected, lengthof(expected),
				     /*expected_stop=*/false);
	}
	/* type=EQ key=300 vlsn=MAX_LSN+10 */
	{
		struct vy_stmt_template key = STMT_TEMPLATE(0, SELECT, 300);
		struct test_iterator_expected expected[] = {};
		test_iterator_helper(&cache, key_def, format, ITER_EQ,
				     &key, /*vlsn=*/MAX_LSN + 10,
				     expected, lengthof(expected),
				     /*expected_stop=*/false);
	}
	destroy_test_cache(&cache, key_def, format);
	footer();
	check_plan();
}

/**
 * A link finalizes a pending link opened when the previous result
 * was cached. A write between two results -- while the reader
 * yields between them -- destroys the pending link via
 * invalidation, and the link must not form; the range cannot be
 * trusted. With no interference the pending link survives and the
 * link forms as usual.
 */
static void
test_pending_link(void)
{
	header();
	plan(6);
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	struct key_def *key_def;
	struct tuple_format *format;
	struct vy_cache cache;
	create_test_cache(fields, types, lengthof(fields), &cache, &key_def,
			  &format);
	struct vy_stmt_template a_templ = STMT_TEMPLATE(10, REPLACE, 100);
	struct vy_stmt_template b_templ = STMT_TEMPLATE(10, REPLACE, 300);
	struct vy_entry key = vy_new_simple_stmt(format, key_def,
						 &key_template);
	struct vy_entry a = vy_new_simple_stmt(format, key_def, &a_templ);
	struct vy_entry b = vy_new_simple_stmt(format, key_def, &b_templ);

	/* No interference: the pending link survives, the link forms. */
	struct vy_cache_builder builder;
	vy_cache_builder_create(&builder, &cache, ITER_GE, key,
				/*last=*/vy_entry_none(), &test_read_view);
	fail_unless(builder.last.stmt != NULL);
	vy_cache_builder_add(&builder, a);
	vy_cache_builder_add(&builder, b);
	vy_cache_builder_destroy(&builder);
	struct test_iterator_expected linked[] = {
		{STMT_TEMPLATE(10, REPLACE, 100), true},
		{STMT_TEMPLATE(10, REPLACE, 300), true},
	};
	test_iterator_helper(&cache, key_def, format, ITER_GE, &key_template,
			     /*vlsn=*/20,
			     linked, lengthof(linked),
			     /*expected_stop=*/false);
	tuple_unref(a.stmt);
	tuple_unref(b.stmt);
	tuple_unref(key.stmt);
	destroy_test_cache(&cache, key_def, format);

	/* A write between the results destroys the pending link. */
	create_test_cache(fields, types, lengthof(fields), &cache, &key_def,
			  &format);
	key = vy_new_simple_stmt(format, key_def, &key_template);
	a = vy_new_simple_stmt(format, key_def, &a_templ);
	b = vy_new_simple_stmt(format, key_def, &b_templ);
	vy_cache_builder_create(&builder, &cache, ITER_GE, key,
				/*last=*/vy_entry_none(), &test_read_view);
	fail_unless(builder.last.stmt != NULL);
	vy_cache_builder_add(&builder, a);
	struct vy_stmt_template w_templ = STMT_TEMPLATE(15, REPLACE, 200);
	vy_cache_on_write_template(&cache, format, &w_templ);
	vy_cache_builder_add(&builder, b);
	vy_cache_builder_destroy(&builder);
	/*
	 * The write into (a, b) destroyed a's pending link: no link
	 * forms over that range. The chain's leading pending link
	 * was untouched, so the first result keeps its stop
	 * authority.
	 */
	struct test_iterator_expected unlinked[] = {
		{STMT_TEMPLATE(10, REPLACE, 100), true},
		{STMT_TEMPLATE(10, REPLACE, 300), false},
	};
	test_iterator_helper(&cache, key_def, format, ITER_GE, &key_template,
			     /*vlsn=*/20,
			     unlinked, lengthof(unlinked),
			     /*expected_stop=*/false);
	tuple_unref(a.stmt);
	tuple_unref(b.stmt);
	tuple_unref(key.stmt);
	destroy_test_cache(&cache, key_def, format);
	footer();
	check_plan();
}

static void
test_drop_drain(void)
{
	header();
	plan(8);
	uint32_t fields[] = { 0 };
	uint32_t types[] = { FIELD_TYPE_UNSIGNED };
	/* A small cache that must survive the drain untouched. */
	struct vy_cache live;
	struct key_def *live_def;
	struct tuple_format *live_format;
	create_test_cache(fields, types, lengthof(fields), &live,
			  &live_def, &live_format);
	for (int i = 0; i < 10; i++) {
		const struct vy_stmt_template t =
			STMT_TEMPLATE(i + 1, REPLACE, i);
		struct vy_entry entry = vy_new_simple_stmt(live_format,
							   live_def, &t);
		vy_cache_add(&live, entry);
		tuple_unref(entry.stmt);
	}
	size_t live_used = cache_env.mem_used;
	uint32_t live_count = vy_cache_tree_size(live.tree);

	/* A big cache: much larger than the inline drop batch. */
	struct vy_cache big;
	struct key_def *big_def;
	struct tuple_format *big_format;
	create_test_cache(fields, types, lengthof(fields), &big,
			  &big_def, &big_format);
	for (int i = 0; i < 4000; i++) {
		const struct vy_stmt_template t =
			STMT_TEMPLATE(i + 1, REPLACE, i);
		struct vy_entry entry = vy_new_simple_stmt(big_format,
							   big_def, &t);
		vy_cache_add(&big, entry);
		tuple_unref(entry.stmt);
	}
	size_t all_used = cache_env.mem_used;
	ok(all_used - live_used > 64 * 1024,
	   "the big cache exceeds the inline drop batch");

	vy_cache_destroy(&big);
	ok(cache_env.mem_used < all_used, "drop released an inline batch");
	ok(cache_env.mem_used > live_used,
	   "the remainder is stowed, not drained synchronously");

	/* The quota walk consumes stowed remainders first. */
	vy_cache_env_set_quota(&cache_env, live_used);
	is(cache_env.mem_used, live_used,
	   "the quota walk drained the dropped cache");
	is(vy_cache_tree_size(live.tree), live_count,
	   "the live cache survived the drain untouched");

	vy_cache_env_set_quota(&cache_env,
			       1LLU * 1024LLU * 1024LLU * 1024LLU);
	key_def_delete(big_def);
	tuple_format_unref(big_format);
	destroy_test_cache(&live, live_def, live_format);
	ok(cache_env.mem_used == 0,
	   "a small cache is drained inline at drop");

	/* The last cache dropped: no live ring, only a remainder. */
	struct vy_cache last;
	struct key_def *last_def;
	struct tuple_format *last_format;
	create_test_cache(fields, types, lengthof(fields), &last,
			  &last_def, &last_format);
	for (int i = 0; i < 4000; i++) {
		const struct vy_stmt_template t =
			STMT_TEMPLATE(i + 1, REPLACE, i);
		struct vy_entry entry = vy_new_simple_stmt(last_format,
							   last_def, &t);
		vy_cache_add(&last, entry);
		tuple_unref(entry.stmt);
	}
	destroy_test_cache(&last, last_def, last_format);
	ok(cache_env.mem_used > 0, "the last drop stows a remainder");
	vy_cache_env_set_quota(&cache_env, 0);
	ok(cache_env.mem_used == 0,
	   "the quota walk drains with an empty ring");
	vy_cache_env_set_quota(&cache_env,
			       1LLU * 1024LLU * 1024LLU * 1024LLU);
	footer();
	check_plan();
}

/*
 * The quota walk yields between steps, so the tests run in a
 * true fiber under the event loop: the scheduler fiber itself
 * cannot yield.
 */
static int
main_f(va_list ap)
{
	(void)ap;
	plan(4);

	test_basic();
	test_prepared_not_cached();
	test_pending_link();
	test_drop_drain();

	ev_break(loop(), EVBREAK_ALL);
	return 0;
}

int
main(void)
{
	vy_iterator_C_test_init(1LLU * 1024LLU * 1024LLU * 1024LLU);

	struct fiber *f = fiber_new("main", main_f);
	fiber_wakeup(f);
	ev_run(loop(), 0);

	vy_iterator_C_test_finish();
	return check_plan();
}
