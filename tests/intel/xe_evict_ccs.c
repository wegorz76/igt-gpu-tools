// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Check flat-ccs eviction
 * Category: Software building block
 * Sub-category: Flat-CCS
 * Functionality: evict
 * GPU requirements: GPU needs to have dedicated VRAM
 */

#include "igt.h"
#include "igt_list.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include <math.h>
#include <string.h>

#define OVERCOMMIT_VRAM_PERCENT 110
#define MIN_OBJ_KB 64
#define MAX_OBJ_KB (256 * 1024)

static struct param {
	bool print_bb;
	int num_objs;
	int vram_percent;
	int min_size_kb;
	int max_size_kb;
} params = {
	.num_objs = 0,
	.vram_percent = OVERCOMMIT_VRAM_PERCENT,
	.min_size_kb = MIN_OBJ_KB,
	.max_size_kb = MAX_OBJ_KB,
};

struct object {
	uint64_t size;
	uint32_t start_value;
	struct blt_copy_object *blt_obj;
	struct igt_list_head link;
};

#define TEST_PARALLEL		(1 << 0)
#define TEST_INSTANTFREE	(1 << 2)
#define TEST_REOPEN		(1 << 3)
#define TEST_OBJ_64M		(1 << 16)
#define TEST_OBJ_128M		(1 << 17)
#define TEST_OBJ_256M		(1 << 18)

#define MAX_NPROC 8
struct config {
	uint32_t flags;
	int nproc;
	int free_mb, total_mb;
	int test_mb, mb_per_proc;
	const struct param *param;
};

static void copy_obj(struct blt_copy_data *blt,
		     struct blt_copy_object *src_obj,
		     struct blt_copy_object *dst_obj,
		     uint64_t ahnd, uint32_t vm)
{
	struct blt_block_copy_data_ext ext = {};
	int fd = blt->fd;
	uint64_t bb_size = xe_get_default_alignment(fd);
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	intel_ctx_t *ctx;
	uint32_t bb, exec_queue;
	uint32_t w, h;

	w = src_obj->x2;
	h = src_obj->y2;
	exec_queue = xe_exec_queue_create(fd, vm, &inst, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);

	bb = xe_bo_create_flags(fd, 0, bb_size,
				vram_memory(fd, 0) | XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	blt->color_depth = CD_32bit;
	blt->print_bb = params.print_bb;
	blt_set_copy_object(&blt->src, src_obj);
	blt_set_copy_object(&blt->dst, dst_obj);
	blt_set_object_ext(&ext.src, 0, w, h, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, 0, w, h, SURFACE_TYPE_2D);
	blt_set_batch(&blt->bb, bb, bb_size, vram_if_possible(fd, 0));
	blt_block_copy(fd, ctx, NULL, ahnd, blt, &ext);
	intel_ctx_xe_sync(ctx, true);

	gem_close(fd, bb);
	put_offset(ahnd, bb);
	put_offset(ahnd, blt->src.handle);
	put_offset(ahnd, blt->dst.handle);
	intel_allocator_bind(ahnd, 0, 0);
}

static uint32_t rand_and_update(uint32_t *left, uint32_t min, uint32_t max)
{
	int left_bit, min_bit, max_bit, rand_id, rand_kb;

	left_bit = igt_fls(*left) - 1;
	min_bit = igt_fls(min) - 1;
	max_bit = max_t(int, min_t(int, igt_fls(max) - 1, left_bit), igt_fls(max));
	rand_id = rand() % (max_bit - min_bit);
	rand_kb = 1 << (rand_id + min_bit);

	if (*left >= rand_kb)
		*left -= rand_kb;
	else
		*left = 0;

	return rand_kb;
}

static struct object *create_obj(struct blt_copy_data *blt,
				 struct blt_copy_object *src_obj,
				 uint64_t ahnd, uint32_t vm,
				 uint64_t size, int start_value)
{
	int fd = blt->fd;
	struct object *obj;
	uint32_t w, h;
	uint8_t uc_mocs = intel_get_uc_mocs_index(fd);
	int i;

	obj = calloc(1, sizeof(*obj));
	igt_assert(obj);
	obj->size = size;
	obj->start_value = start_value;

	w = max_t(int, 1024, roundup_power_of_two(sqrt(size/4)));
	h = size / w / 4; /* /4 - 32bpp */

	obj->blt_obj = blt_create_object(blt,
					 vram_memory(fd, 0) | XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM,
					 w, h, 32, uc_mocs,
					 T_LINEAR, COMPRESSION_ENABLED,
					 COMPRESSION_TYPE_3D, true);

	for (i = 0; i < size / sizeof(uint32_t); i++)
		src_obj->ptr[i] = start_value++;

	copy_obj(blt, src_obj, obj->blt_obj, ahnd, vm);

	return obj;
}

static void check_obj(const struct blt_copy_object *obj, uint64_t size,
		      int start_value, int num_obj)
{
	int i, idx;

	igt_assert_eq(obj->ptr[0], start_value);
	igt_assert_eq(obj->ptr[size/4 - 1], start_value + size/4 - 1);

	/* Couple of checks of random indices */
	for (i = 0; i < 16; i++) {
		idx = rand() % (size/4);
		igt_assert_f(obj->ptr[idx] == start_value + idx,
			     "Object number %d doesn't contain valid data",
			     num_obj);
	}
}

static void evict_single(int fd, int child, const struct config *config)
{
	struct blt_copy_data blt = {};
	struct blt_copy_object *orig_obj;
	uint32_t kb_left = config->mb_per_proc * SZ_1K;
	uint32_t min_alloc_kb = config->param->min_size_kb;
	uint32_t max_alloc_kb = config->param->max_size_kb;
	uint32_t vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	uint64_t ahnd = intel_allocator_open(fd, vm, INTEL_ALLOCATOR_RELOC);
	uint8_t uc_mocs = intel_get_uc_mocs_index(fd);
	struct object *obj, *tmp;
	struct igt_list_head list;
	uint32_t w, h;
	int num_obj = 0;

	if (config->flags & TEST_OBJ_64M)
		min_alloc_kb = max_alloc_kb = 64 * 1024;
	else if (config->flags & TEST_OBJ_128M)
		min_alloc_kb = max_alloc_kb = 128 * 1024;
	else if (config->flags & TEST_OBJ_256M)
		min_alloc_kb = max_alloc_kb = 256 * 1024;

	srandom(time(NULL));
	IGT_INIT_LIST_HEAD(&list);
	igt_debug("[%2d] child : to allocate: %uMiB\n", child, kb_left/SZ_1K);

	blt_copy_init(fd, &blt);
	w = max_t(int, 1024, roundup_power_of_two(sqrt(max_alloc_kb * SZ_1K / 4)));
	h = max_alloc_kb * SZ_1K / w / 4;
	orig_obj = blt_create_object(&blt, system_memory(fd),
				     w, h,  32, uc_mocs,
				     T_LINEAR, COMPRESSION_DISABLED,
				     0, true);

	while (kb_left) {
		uint64_t obj_size = rand_and_update(&kb_left, min_alloc_kb, max_alloc_kb) * SZ_1K;
		int start_value = rand();

		igt_debug("[%2d] obj_size: %ldKiB (%ldMiB)\n", child,
			  obj_size / SZ_1K, obj_size / SZ_1M);
		h = obj_size / w / 4;
		blt_set_geom(orig_obj, w * 4, 0, 0, w, h, 0, 0);
		obj = create_obj(&blt, orig_obj, ahnd, vm, obj_size, start_value);
		igt_list_add(&obj->link, &list);

		if (config->param->num_objs && ++num_obj == config->param->num_objs)
			break;
	}

	num_obj = 0;
	igt_list_for_each_entry_safe(obj, tmp, &list, link) {
		h = obj->size / w / 4;
		blt_set_geom(orig_obj, w * 4, 0, 0, w, h, 0, 0);
		copy_obj(&blt, obj->blt_obj, orig_obj, ahnd, vm);
		check_obj(orig_obj, obj->blt_obj->size, obj->start_value, num_obj++);
		if (config->flags & TEST_INSTANTFREE) {
			igt_list_del(&obj->link);
			blt_destroy_object_and_alloc_free(fd, ahnd, obj->blt_obj);
			free(obj);
		}
	}

	if (!(config->flags & TEST_INSTANTFREE))
		igt_list_for_each_entry_safe(obj, tmp, &list, link) {
			igt_list_del(&obj->link);
			blt_destroy_object_and_alloc_free(fd, ahnd, obj->blt_obj);
			free(obj);
		}
	blt_destroy_object_and_alloc_free(fd, ahnd, orig_obj);
}

static void set_config(int fd, uint32_t flags, const struct param *param,
		       struct config *config)
{
	int nproc = 1;

	config->param = param;
	config->flags = flags;
	config->free_mb = xe_vram_available(fd, 0) / SZ_1M;
	config->total_mb = xe_visible_vram_size(fd, 0) / SZ_1M;
	config->test_mb = min_t(int, config->free_mb * config->param->vram_percent / 100,
				config->total_mb * config->param->vram_percent / 100);

	igt_debug("VRAM memory size: %dMB/%dMB (use %dMB), overcommit perc: %d\n",
		  config->free_mb, config->total_mb,
		  config->test_mb, config->param->vram_percent);

	if (flags & TEST_PARALLEL)
		nproc = min_t(int, sysconf(_SC_NPROCESSORS_ONLN), MAX_NPROC);
	config->nproc = nproc;
	config->mb_per_proc = config->test_mb / nproc;

	igt_debug("nproc: %d, mem per proc: %dMB\n", nproc, config->mb_per_proc);
}

static void evict_ccs(int fd, uint32_t flags, const struct param *param)
{
	struct config config;
	char numstr[32];

	igt_info("Test mode <parallel: %d, instant free: %d, reopen: %d>\n",
		 !!(flags & TEST_PARALLEL),
		 !!(flags & TEST_INSTANTFREE),
		 !!(flags & TEST_REOPEN));
	if (param->num_objs)
		snprintf(numstr, sizeof(numstr), "%d", param->num_objs);
	else
		strncpy(numstr, "limited to vram", sizeof(numstr));
	igt_info("Params: num objects: %s, vram percent: %d, kb <min: %d, max: %d>\n",
		 numstr, param->vram_percent,
		 param->min_size_kb, param->max_size_kb);

	set_config(fd, flags, param, &config);

	if (flags & TEST_PARALLEL) {
		igt_fork(n, config.nproc) {
			if (flags & TEST_REOPEN) {
				fd = drm_reopen_driver(fd);
				intel_allocator_init();
			}
			evict_single(fd, n, &config);
		}
		igt_waitchildren();
	} else {
		if (flags & TEST_REOPEN)
			fd = drm_reopen_driver(fd);
		evict_single(fd, 0, &config);
	}
}

/**
 *
 * SUBTEST: evict-ccs-overcommit-%s-%s-%s
 * Description: FlatCCS eviction test.
 * Feature: flatccs
 * Test category: stress test
 *
 * arg[1]:
 *
 * @standalone:			single process
 * @parallel:			multiple processes
 *
 * arg[2]:
 *
 * @nofree:			keep objects till the end of the test
 * @instantfree:		free object after it was verified and it won't
 *				be used anymore
 *
 * arg[3]:
 *
 * @samefd:			operate on same opened drm fd
 * @reopen:			use separately opened drm fds
 *
 */
/**
 * SUBTEST: evict-ccs-overcommit-%s-%s
 * Description: FlatCCS eviction test.
 * Feature: flatccs
 * Test category: stress test
 *
 * arg[1]:
 *
 * @single-object:		limit to one object
 * @two-objects:		limit to two objects
 *
 * arg[2]:
 *
 * @64M:			alloc 64M object (single ctrl-surf copy)
 * @128M:			alloc 128M object (two ctrl-surf copies)
 * @256M:			alloc 256M object (four ctrl-surf copies)
*/
/**
 * SUBTEST: evict-ccs-overcommit-standalone-%s
 * Description: FlatCCS eviction test.
 * Feature: flatccs
 * Test category: stress test
 *
 * arg[1]:
 *
 * @64M:			fill vram with 64M objects
 * @128M:			fill vram with 128M objects
 * @256M:			fill vram with 256M objects
*/

static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'b':
		params.print_bb = true;
		igt_debug("Print bb: %d\n", params.print_bb);
		break;
	case 'n':
		params.num_objs = atoi(optarg);
		igt_debug("Number objects: %d\n", params.num_objs);
		break;
	case 'p':
		params.vram_percent = atoi(optarg);
		igt_debug("Percent vram: %d\n", params.vram_percent);
		break;
	case 's':
		params.min_size_kb = atoi(optarg);
		igt_debug("Min size kb: %d\n", params.min_size_kb);
		break;
	case 'S':
		params.max_size_kb = atoi(optarg);
		igt_debug("Max size kb: %d\n", params.max_size_kb);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -b\tPrint bb\n"
	"  -e\tAdd temporary object which enforce eviction\n"
	"  -n\tNumber of objects to create (0 - 31)\n"
	"  -p\tPercent of VRAM to alloc\n"
	"  -s\tMinimum size of object in kb\n"
	"  -S\tMaximum size of object in kb\n"
	;

igt_main_args("ben:p:s:S:", NULL, help_str, opt_handler, NULL)
{
	struct drm_xe_engine_class_instance *hwe;

	const struct ccs {
		const char *name;
		uint32_t flags;
	} ccs[] = {
		{ "standalone-nofree-samefd",
			0 },
		{ "standalone-nofree-reopen",
			TEST_REOPEN },
		{ "standalone-instantfree-samefd",
			TEST_INSTANTFREE },
		{ "standalone-instantfree-reopen",
			TEST_INSTANTFREE | TEST_REOPEN },
		{ "standalone-64M",
			TEST_OBJ_64M },
		{ "standalone-128M",
			TEST_OBJ_128M },
		{ "standalone-256M",
			TEST_OBJ_256M },
		{ "single-object-64M",
			TEST_OBJ_64M },
		{ "single-object-128M",
			TEST_OBJ_128M },
		{ "single-object-256M",
			TEST_OBJ_256M },
		{ "two-objects-64M",
			TEST_OBJ_64M },
		{ "two-objects-128M",
			TEST_OBJ_128M },
		{ "two-objects-256M",
			TEST_OBJ_256M },
		{ "parallel-nofree-samefd",
			TEST_PARALLEL },
		{ "parallel-nofree-reopen",
			TEST_PARALLEL | TEST_REOPEN },
		{ "parallel-instantfree-samefd",
			TEST_PARALLEL | TEST_INSTANTFREE },
		{ "parallel-instantfree-reopen",
			TEST_PARALLEL | TEST_INSTANTFREE | TEST_REOPEN },
		{ },
	};
	uint64_t vram_size;
	int fd;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(xe_has_vram(fd));
		vram_size = xe_visible_vram_size(fd, 0);
		igt_assert(vram_size);

		xe_for_each_hw_engine(fd, hwe)
			if (hwe->engine_class != DRM_XE_ENGINE_CLASS_COPY)
				break;
	}

	igt_fixture
		intel_allocator_multiprocess_start();

	for (const struct ccs *s = ccs; s->name; s++) {
		igt_subtest_f("evict-ccs-overcommit-%s", s->name)
			evict_ccs(fd, s->flags, &params);
	}

	igt_fixture {
		intel_allocator_multiprocess_stop();
		drm_close_driver(fd);
	}
}
