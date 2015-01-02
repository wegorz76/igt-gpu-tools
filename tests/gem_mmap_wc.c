/*
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "drm.h"
#include "ioctl_wrappers.h"
#include "drmtest.h"
#include "igt_debugfs.h"

static int OBJECT_SIZE = 16*1024*1024;

static void set_domain(int fd, uint32_t handle)
{
	gem_set_domain(fd, handle, I915_GEM_DOMAIN_GTT, I915_GEM_DOMAIN_GTT);
}

static void *
mmap_bo(int fd, uint32_t handle)
{
	void *ptr;

	ptr = gem_mmap__wc(fd, handle, 0, OBJECT_SIZE, PROT_READ | PROT_WRITE);
	igt_assert(ptr && ptr != MAP_FAILED);

	return ptr;
}

static void *
create_pointer(int fd)
{
	uint32_t handle;
	void *ptr;

	handle = gem_create(fd, OBJECT_SIZE);

	ptr = mmap_bo(fd, handle);
	set_domain(fd, handle);

	gem_close(fd, handle);

	return ptr;
}

static void
test_copy(int fd)
{
	void *src, *dst;

	igt_require_mmap_wc(fd);

	/* copy from a fresh src to fresh dst to force pagefault on both */
	src = create_pointer(fd);
	dst = create_pointer(fd);

	memcpy(dst, src, OBJECT_SIZE);
	memcpy(src, dst, OBJECT_SIZE);

	munmap(dst, OBJECT_SIZE);
	munmap(src, OBJECT_SIZE);
}

enum test_read_write {
	READ_BEFORE_WRITE,
	READ_AFTER_WRITE,
};

static void
test_read_write(int fd, enum test_read_write order)
{
	uint32_t handle;
	void *ptr;
	volatile uint32_t val = 0;

	handle = gem_create(fd, OBJECT_SIZE);
	set_domain(fd, handle);

	ptr = mmap_bo(fd, handle);
	igt_assert(ptr != MAP_FAILED);

	if (order == READ_BEFORE_WRITE) {
		val = *(uint32_t *)ptr;
		*(uint32_t *)ptr = val;
	} else {
		*(uint32_t *)ptr = val;
		val = *(uint32_t *)ptr;
	}

	gem_close(fd, handle);
	munmap(ptr, OBJECT_SIZE);
}

static void
test_read_write2(int fd, enum test_read_write order)
{
	uint32_t handle;
	void *r, *w;
	volatile uint32_t val = 0;

	igt_require_mmap_wc(fd);

	handle = gem_create(fd, OBJECT_SIZE);
	set_domain(fd, handle);

	r = gem_mmap__wc(fd, handle, 0, OBJECT_SIZE, PROT_READ);
	igt_assert(r != MAP_FAILED);

	w = gem_mmap__wc(fd, handle, 0, OBJECT_SIZE, PROT_READ | PROT_WRITE);
	igt_assert(w != MAP_FAILED);

	if (order == READ_BEFORE_WRITE) {
		val = *(uint32_t *)r;
		*(uint32_t *)w = val;
	} else {
		*(uint32_t *)w = val;
		val = *(uint32_t *)r;
	}

	gem_close(fd, handle);
	munmap(r, OBJECT_SIZE);
	munmap(w, OBJECT_SIZE);
}

static void
test_write(int fd)
{
	void *src;
	uint32_t dst;

	igt_require_mmap_wc(fd);

	/* copy from a fresh src to fresh dst to force pagefault on both */
	src = create_pointer(fd);
	dst = gem_create(fd, OBJECT_SIZE);

	gem_write(fd, dst, 0, src, OBJECT_SIZE);

	gem_close(fd, dst);
	munmap(src, OBJECT_SIZE);
}

static void
test_write_gtt(int fd)
{
	uint32_t dst;
	char *dst_gtt;
	void *src;

	igt_require_mmap_wc(fd);

	dst = gem_create(fd, OBJECT_SIZE);
	set_domain(fd, dst);

	/* prefault object into gtt */
	dst_gtt = mmap_bo(fd, dst);
	memset(dst_gtt, 0, OBJECT_SIZE);
	munmap(dst_gtt, OBJECT_SIZE);

	src = create_pointer(fd);

	gem_write(fd, dst, 0, src, OBJECT_SIZE);

	gem_close(fd, dst);
	munmap(src, OBJECT_SIZE);
}

static void
test_read(int fd)
{
	void *dst;
	uint32_t src;

	igt_require_mmap_wc(fd);

	/* copy from a fresh src to fresh dst to force pagefault on both */
	dst = create_pointer(fd);
	src = gem_create(fd, OBJECT_SIZE);

	gem_read(fd, src, 0, dst, OBJECT_SIZE);

	gem_close(fd, src);
	munmap(dst, OBJECT_SIZE);
}

static void
test_write_cpu_read_wc(int fd)
{
	uint32_t handle;
	uint32_t *src, *dst;

	igt_require_mmap_wc(fd);

	handle = gem_create(fd, OBJECT_SIZE);

	dst = gem_mmap__wc(fd, handle, 0, OBJECT_SIZE, PROT_READ);
	igt_assert(dst != (uint32_t *)MAP_FAILED);

	src = gem_mmap__cpu(fd, handle, 0, OBJECT_SIZE, PROT_WRITE);
	igt_assert(src != (uint32_t *)MAP_FAILED);

	gem_close(fd, handle);

	memset(src, 0xaa, OBJECT_SIZE);
	set_domain(fd, handle);
	igt_assert(memcmp(dst, src, OBJECT_SIZE) == 0);

	munmap(src, OBJECT_SIZE);
	munmap(dst, OBJECT_SIZE);
}

static void
test_write_gtt_read_wc(int fd)
{
	uint32_t handle;
	uint32_t *src, *dst;

	igt_require_mmap_wc(fd);

	handle = gem_create(fd, OBJECT_SIZE);
	set_domain(fd, handle);

	dst = gem_mmap__wc(fd, handle, 0, OBJECT_SIZE, PROT_READ);
	igt_assert(dst != (uint32_t *)MAP_FAILED);

	src = gem_mmap__gtt(fd, handle, OBJECT_SIZE, PROT_WRITE);
	igt_assert(src != (uint32_t *)MAP_FAILED);

	gem_close(fd, handle);

	memset(src, 0xaa, OBJECT_SIZE);
	igt_assert(memcmp(dst, src, OBJECT_SIZE) == 0);

	munmap(src, OBJECT_SIZE);
	munmap(dst, OBJECT_SIZE);
}

struct thread_fault_concurrent {
	pthread_t thread;
	int id;
	uint32_t **ptr;
};

static void *
thread_fault_concurrent(void *closure)
{
	struct thread_fault_concurrent *t = closure;
	uint32_t val = 0;
	int n;

	for (n = 0; n < 32; n++) {
		if (n & 1)
			*t->ptr[(n + t->id) % 32] = val;
		else
			val = *t->ptr[(n + t->id) % 32];
	}

	return NULL;
}

static void
test_fault_concurrent(int fd)
{
	uint32_t *ptr[32];
	struct thread_fault_concurrent thread[64];
	int n;

	igt_require_mmap_wc(fd);

	for (n = 0; n < 32; n++) {
		ptr[n] = create_pointer(fd);
	}

	for (n = 0; n < 64; n++) {
		thread[n].ptr = ptr;
		thread[n].id = n;
		pthread_create(&thread[n].thread, NULL, thread_fault_concurrent, &thread[n]);
	}

	for (n = 0; n < 64; n++)
		pthread_join(thread[n].thread, NULL);

	for (n = 0; n < 32; n++) {
		munmap(ptr[n], OBJECT_SIZE);
	}
}

static void
run_without_prefault(int fd,
			void (*func)(int fd))
{
	igt_disable_prefault();
	func(fd);
	igt_enable_prefault();
}

int fd;

igt_main
{
	if (igt_run_in_simulation())
		OBJECT_SIZE = 1 * 1024 * 1024;

	igt_fixture
		fd = drm_open_any();

	igt_subtest("copy")
		test_copy(fd);
	igt_subtest("read")
		test_read(fd);
	igt_subtest("write")
		test_write(fd);
	igt_subtest("write-gtt")
		test_write_gtt(fd);
	igt_subtest("read-write")
		test_read_write(fd, READ_BEFORE_WRITE);
	igt_subtest("write-read")
		test_read_write(fd, READ_AFTER_WRITE);
	igt_subtest("read-write-distinct")
		test_read_write2(fd, READ_BEFORE_WRITE);
	igt_subtest("write-read-distinct")
		test_read_write2(fd, READ_AFTER_WRITE);
	igt_subtest("fault-concurrent")
		test_fault_concurrent(fd);
	igt_subtest("read-no-prefault")
		run_without_prefault(fd, test_read);
	igt_subtest("write-no-prefault")
		run_without_prefault(fd, test_write);
	igt_subtest("write-gtt-no-prefault")
		run_without_prefault(fd, test_write_gtt);
	igt_subtest("write-cpu-read-wc")
		test_write_cpu_read_wc(fd);
	igt_subtest("write-gtt-read-wc")
		test_write_gtt_read_wc(fd);

	igt_fixture
		close(fd);
}
