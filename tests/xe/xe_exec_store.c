/* SPDX-License-Identifier: MIT */
/*
* Copyright © 2023 Intel Corporation
*
* Authors:
*    Sai Gowtham Ch <sai.gowtham.ch@intel.com>
*/

#include "igt.h"
#include "lib/igt_syncobj.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "xe_drm.h"

/**
 * TEST: Tests to verify store dword functionality.
 * Category: Software building block
 * Sub-category: HW
 * Functionality: intel-bb
 * Test category: functionality test
 */

#define MAX_INSTANCE 9

struct data {
	uint32_t batch[16];
	uint64_t pad;
	uint32_t data;
	uint64_t addr;
};

static void store_dword_batch(struct data *data, uint64_t addr, int value)
{
	int b;
	uint64_t batch_offset = (char *)&(data->batch) - (char *)data;
	uint64_t batch_addr = addr + batch_offset;
	uint64_t sdi_offset = (char *)&(data->data) - (char *)data;
	uint64_t sdi_addr = addr + sdi_offset;

	b = 0;
	data->batch[b++] = MI_STORE_DWORD_IMM_GEN4;
	data->batch[b++] = sdi_addr;
	data->batch[b++] = sdi_addr >> 32;
	data->batch[b++] = value;
	data->batch[b++] = MI_BATCH_BUFFER_END;
	igt_assert(b <= ARRAY_SIZE(data->batch));

	data->addr = batch_addr;
}

/**
 * SUBTEST: basic-store
 * Description: Basic test to verify store dword.
 * Run type: BAT
 */
static void store(int fd)
{
	struct drm_xe_sync sync = {
		.flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL,
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 1,
		.syncs = to_user_pointer(&sync),
	};
	struct data *data;
	struct drm_xe_engine_class_instance *hw_engine;
	uint32_t vm;
	uint32_t exec_queue;
	uint32_t syncobj;
	size_t bo_size;
	int value = 0x123456;
	uint64_t addr = 0x100000;
	uint32_t bo = 0;

	syncobj = syncobj_create(fd, 0);
	sync.handle = syncobj;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data);
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	hw_engine = xe_hw_engine(fd, 1);
	bo = xe_bo_create_flags(fd, vm, bo_size,
				visible_vram_if_possible(fd, hw_engine->gt_id));

	xe_vm_bind_async(fd, vm, hw_engine->gt_id, bo, 0, addr, bo_size, &sync, 1);
	data = xe_bo_map(fd, bo, bo_size);
	store_dword_batch(data, addr, value);

	exec_queue = xe_exec_queue_create(fd, vm, hw_engine, 0);
	exec.exec_queue_id = exec_queue;
	exec.address = data->addr;
	sync.flags &= DRM_XE_SYNC_SIGNAL;
	xe_exec(fd, &exec);

	igt_assert(syncobj_wait(fd, &syncobj, 1, INT64_MAX, 0, NULL));
	igt_assert_eq(data->data, value);

	syncobj_destroy(fd, syncobj);
	munmap(data, bo_size);
	gem_close(fd, bo);

	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: basic-all
 * Description: Test to verify store dword on all available engines.
 * Run type: BAT
 */
static void store_all(int fd, int gt, int class)
{
	struct drm_xe_sync sync[2] = {
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, },
		{ .flags = DRM_XE_SYNC_SYNCOBJ | DRM_XE_SYNC_SIGNAL, }
	};
	struct drm_xe_exec exec = {
		.num_batch_buffer = 1,
		.num_syncs = 2,
		.syncs = to_user_pointer(&sync),
	};

	struct data *data;
	uint32_t syncobjs[MAX_INSTANCE];
	uint32_t exec_queues[MAX_INSTANCE];
	uint32_t vm;
	size_t bo_size;
	uint64_t addr = 0x100000;
	uint32_t bo = 0;
	struct drm_xe_engine_class_instance eci[MAX_INSTANCE];
	struct drm_xe_engine_class_instance *hwe;
	int i, num_placements = 0;

	vm = xe_vm_create(fd, DRM_XE_VM_CREATE_ASYNC_BIND_OPS, 0);
	bo_size = sizeof(*data);
	bo_size = ALIGN(bo_size + xe_cs_prefetch_size(fd),
			xe_get_default_alignment(fd));

	bo = xe_bo_create_flags(fd, vm, bo_size,
				visible_vram_if_possible(fd, 0));
	data = xe_bo_map(fd, bo, bo_size);

	xe_for_each_hw_engine(fd, hwe) {
		if (hwe->engine_class != class || hwe->gt_id != gt)
			continue;
		eci[num_placements++] = *hwe;
	}

	igt_require(num_placements);

	for (i = 0; i < num_placements; i++) {
		struct drm_xe_exec_queue_create create = {
			.vm_id = vm,
			.width = 1,
			.num_placements = num_placements,
			.instances = to_user_pointer(eci),
		};

		igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_EXEC_QUEUE_CREATE,
					&create), 0);
		exec_queues[i] = create.exec_queue_id;
		syncobjs[i] = syncobj_create(fd, 0);
	};

	sync[0].handle = syncobj_create(fd, 0);
	xe_vm_bind_async(fd, vm, 0, bo, 0, addr, bo_size, sync, 1);

	for (i = 0; i < num_placements; i++) {

		store_dword_batch(data, addr, i);
		sync[0].flags &= ~DRM_XE_SYNC_SIGNAL;
		sync[1].flags |= DRM_XE_SYNC_SIGNAL;
		sync[1].handle = syncobjs[i];

		exec.exec_queue_id = exec_queues[i];
		exec.address = data->addr;
		xe_exec(fd, &exec);

		igt_assert(syncobj_wait(fd, &syncobjs[i], 1, INT64_MAX, 0, NULL));
		igt_assert_eq(data->data, i);
	}

	xe_vm_unbind_async(fd, vm, 0, 0, addr, bo_size, sync, 1);
	syncobj_destroy(fd, sync[0].handle);
	munmap(data, bo_size);
	gem_close(fd, bo);

	for (i = 0; i < num_placements; i++) {
		syncobj_destroy(fd, syncobjs[i]);
		xe_exec_queue_destroy(fd, exec_queues[i]);
	}
	xe_vm_destroy(fd, vm);
}

igt_main
{
	int fd, class, gt;

	igt_fixture {
		fd = drm_open_driver(DRIVER_XE);
		xe_device_get(fd);
	}

	igt_subtest("basic-store")
		store(fd);

	igt_subtest("basic-all") {
		xe_for_each_gt(fd, gt)
			xe_for_each_hw_engine_class(class)
				store_all(fd, gt, class);
	}

	igt_fixture {
		xe_device_put(fd);
		close(fd);
	}
}
