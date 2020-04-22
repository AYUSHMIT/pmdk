// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2018-2020, Intel Corporation */

/*
 * extent.h -- fs extent query API
 */

#ifndef PMDK_EXTENT_H
#define PMDK_EXTENT_H 1

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct extent {
	uint64_t offset_physical;
	uint64_t offset_logical;
	uint64_t length;
};

struct extents {
	uint64_t blksize;
	uint32_t extents_count;
	struct extent *extents;
};

long pmem2_extents_count(int fd, struct extents *exts);
int pmem2_extents_get(int fd, struct extents *exts);

#ifdef __cplusplus
}
#endif

#endif /* PMDK_EXTENT_H */
