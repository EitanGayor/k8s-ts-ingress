/* vim:set sw=8 ts=8 noet: */
/*
 * Copyright (c) 2016-2017 Torchbox Ltd.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely. This software is provided 'as-is', without any express or implied
 * warranty.
 */

/*
 * Tests for hash.c: simple FNV-1a-based hash table.
 */

#include	<string>
#include	<vector>
#include	<map>

#include	"hash.h"

#include	"gtest/gtest.h"

using std::vector;
using std::map;
using std::string;

namespace {
	/*
	 * Repeat each test with various different hash sizes, to exercise both
	 * the hashing and the bucket handling code.
	 */
	vector<int> test_sizes{ 1, 2, 7, 127, 15601 };

	/*
	 * Populate a hash with data.  This inserts and deletes data in a
	 * specific order, to exercise the single linked list deletion code.
	 */
	void
	populate_hash(hash_t hs)
	{
#define	VOID_P(s)	const_cast<void *>(static_cast<void const *>(s))
		hash_set(hs, "delete 1", VOID_P("x"));
		hash_set(hs, "foo", VOID_P("foo key"));
		hash_set(hs, "delete 2", VOID_P("x"));
		hash_set(hs, "bar", VOID_P("bar key"));
		hash_del(hs, "delete 2");
		hash_set(hs, "quux", VOID_P("quux key"));
		hash_set(hs, "delete 3", VOID_P("x"));
		hash_del(hs, "delete 1");
		hash_del(hs, "delete 3");
#undef VOID_P
	}

} // anonymous namespace

TEST(Hash, SetGet)
{
	for (int size: test_sizes) {
	hash_t	hs;
	char	*s;

		hs = hash_new(size, NULL);
		populate_hash(hs);

		s = static_cast<char *>(hash_get(hs, "foo"));
		ASSERT_NE(s, static_cast<char *>(NULL));
		EXPECT_STREQ("foo key", s);

		s = static_cast<char *>(hash_get(hs, "bar"));
		ASSERT_NE(s, static_cast<char *>(NULL));
		EXPECT_STREQ("bar key", s);

		s = static_cast<char *>(hash_get(hs, "quux"));
		ASSERT_NE(s, static_cast<char *>(NULL));
		EXPECT_STREQ("quux key", s);

		hash_free(hs);
	}
}

TEST(Hash, Foreach)
{
	for (int size: test_sizes) {
	hash_t			 hs;
	map<string, string>	 actual, expected{
			{ "foo", "foo key" },
			{ "bar", "bar key" },
			{ "quux", "quux key" },
		};

		hs = hash_new(size, NULL);
		populate_hash(hs);

		const char *key, *value;
		size_t keylen;
		hash_foreach(hs, &key, &keylen, &value)
			actual[string(key, keylen)] = value;

		EXPECT_EQ(size_t(3), actual.size());
		EXPECT_EQ(expected, actual);

		hash_free(hs);
	}
}

TEST(Hash, Iterate)
{
	for (int size: test_sizes) {
	struct hash_iter_state	 iterstate;
	hash_t			 hs = NULL;
	const char		*k = NULL;
	size_t			 klen;
	void			*v = NULL;
	int			 i = 0;
	map<string, string>	 actual, expected{
			{ "foo", "foo key" },
			{ "bar", "bar key" },
			{ "quux", "quux key" },
		};

		hs = hash_new(size, NULL);
		populate_hash(hs);

		memset(&iterstate, 0, sizeof(iterstate));

		for (int c = 0; c < 3; c++) {
			i = hash_iterate(hs, &iterstate, &k, &klen, &v);
			ASSERT_EQ(i, 1);
			actual.emplace(string(k, klen), static_cast<char const *>(v));
		}

		i = hash_iterate(hs, &iterstate, &k, &klen, &v);
		EXPECT_EQ(i, 0);

		EXPECT_EQ(expected, actual);

		hash_free(hs);
	}
}

TEST(Hash, IterateEmpty)
{
struct hash_iter_state	 iterstate;
hash_t			 hs = NULL;
const char		*k = NULL;
size_t			 klen;
void			*v = NULL;
int			 i = 0;

	hs = hash_new(10, NULL);
	hash_setn(hs, "", 0, HASH_PRESENT);

	memset(&iterstate, 0, sizeof(iterstate));
	i = hash_iterate(hs, &iterstate, &k, &klen, &v);
	ASSERT_EQ(i, 1);
	EXPECT_EQ(klen, 0u);
	EXPECT_EQ(v, HASH_PRESENT);

	hash_free(hs);
}
