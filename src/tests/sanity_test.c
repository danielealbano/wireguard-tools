// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2026 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include <stdbool.h>
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void test_sanity_harness_wired(void)
{
	TEST_ASSERT_TRUE(true);
	TEST_ASSERT_EQUAL_INT(2, 1 + 1);
}

int main(void)
{
	UNITY_BEGIN();
	RUN_TEST(test_sanity_harness_wired);
	return UNITY_END();
}
