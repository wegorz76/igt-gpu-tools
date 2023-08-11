/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 *
 * Authors:
 *    Francois Dugast <francois.dugast@intel.com>
 */

#ifndef INTEL_COMPUTE_H
#define INTEL_COMPUTE_H

/*
 * OpenCL Kernels are generated using:
 *
 * GPU=tgllp &&                                                         \
 *      ocloc -file opencl/compute_square_kernel.cl -device $GPU &&     \
 *      xxd -i compute_square_kernel_Gen12LPlp.bin
 *
 * For each GPU model desired. A list of supported models can be obtained with: ocloc compile --help
 */

struct compute_kernels {
	int ip_ver;
	unsigned int size;
	const unsigned char *kernel;
};

extern const struct compute_kernels compute_square_kernels[];

bool run_compute_kernel(int fd);

#endif	/* INTEL_COMPUTE_H */
