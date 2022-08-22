#include "igt.h"
/**
 * TEST: gem mgpu
 * Description: Playing with device selection
 *
 * SUBTEST: mgpu
 * Category: Infrastructure
 * Description: Select device API check
 * Feature: igt_core
 * Functionality: device selecton
 * Run type: BAT
 * Sub-category: i915
 * Test category: GEM_Legacy
 */


IGT_TEST_DESCRIPTION("Multi gpu test");

static void test_mcard(void)
{
	int fd1 = __drm_open_driver_another(0, DRIVER_INTEL);
	int fd2 = __drm_open_driver_another(1, DRIVER_INTEL);

	igt_info("fd1: %d, fd2: %d\n", fd1, fd2);

	close(fd1);
	close(fd2);
}

igt_main
{
	igt_subtest("mgpu")
		test_mcard();
}
