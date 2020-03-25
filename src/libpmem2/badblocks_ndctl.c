// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2017-2020, Intel Corporation */

/*
 * badblocks_ndctl.c -- implementation of DIMMs API based on the ndctl library
 */
#define _GNU_SOURCE

#include <sys/types.h>
#include <libgen.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <ndctl/libndctl.h>
#include <ndctl/libdaxctl.h>

#include "libpmem2.h"
#include "pmem2_utils.h"
#include "ndctl_region_namespace.h"

#include "file.h"
#include "out.h"
#include "os.h"
#include "badblocks.h"
#include "os_badblock.h"
#include "set_badblocks.h"
#include "vec.h"
#include "extent.h"

/*
 * badblocks_get_namespace_bounds -- (internal) returns the bounds
 *                                 (offset and size) of the given namespace
 *                                 relative to the beginning of its region
 */
static int
badblocks_get_namespace_bounds(struct ndctl_region *region,
				struct ndctl_namespace *ndns,
				unsigned long long *ns_offset,
				unsigned long long *ns_size)
{
	LOG(3, "region %p namespace %p ns_offset %p ns_size %p",
		region, ndns, ns_offset, ns_size);

	struct ndctl_pfn *pfn = ndctl_namespace_get_pfn(ndns);
	struct ndctl_dax *dax = ndctl_namespace_get_dax(ndns);

	ASSERTne(ns_offset, NULL);
	ASSERTne(ns_size, NULL);

	if (pfn) {
		*ns_offset = ndctl_pfn_get_resource(pfn);
		if (*ns_offset == ULLONG_MAX) {
			ERR("!(pfn) cannot read offset of the namespace");
			return -1;
		}

		*ns_size = ndctl_pfn_get_size(pfn);
		if (*ns_size == ULLONG_MAX) {
			ERR("!(pfn) cannot read size of the namespace");
			return -1;
		}

		LOG(10, "(pfn) ns_offset 0x%llx ns_size %llu",
			*ns_offset, *ns_size);
	} else if (dax) {
		*ns_offset = ndctl_dax_get_resource(dax);
		if (*ns_offset == ULLONG_MAX) {
			ERR("!(dax) cannot read offset of the namespace");
			return -1;
		}

		*ns_size = ndctl_dax_get_size(dax);
		if (*ns_size == ULLONG_MAX) {
			ERR("!(dax) cannot read size of the namespace");
			return -1;
		}

		LOG(10, "(dax) ns_offset 0x%llx ns_size %llu",
			*ns_offset, *ns_size);
	} else { /* raw or btt */
		*ns_offset = ndctl_namespace_get_resource(ndns);
		if (*ns_offset == ULLONG_MAX) {
			ERR("!(raw/btt) cannot read offset of the namespace");
			return -1;
		}

		*ns_size = ndctl_namespace_get_size(ndns);
		if (*ns_size == ULLONG_MAX) {
			ERR("!(raw/btt) cannot read size of the namespace");
			return -1;
		}

		LOG(10, "(raw/btt) ns_offset 0x%llx ns_size %llu",
			*ns_offset, *ns_size);
	}

	unsigned long long region_offset = ndctl_region_get_resource(region);
	if (region_offset == ULLONG_MAX) {
		ERR("!cannot read offset of the region");
		return -1;
	}

	LOG(10, "region_offset 0x%llx", region_offset);
	*ns_offset -= region_offset;

	return 0;
}

/*
 * badblocks_get_badblocks_by_region -- (internal) returns bad blocks
 *                                    in the given namespace using the
 *                                    universal region interface.
 *
 * This function works for all types of namespaces, but requires read access to
 * privileged device information.
 */
static int
badblocks_get_badblocks_by_region(struct ndctl_region *region,
				struct ndctl_namespace *ndns,
				struct badblocks *bbs)
{
	LOG(3, "region %p, namespace %p", region, ndns);

	ASSERTne(bbs, NULL);

	unsigned long long ns_beg, ns_size, ns_end;
	unsigned long long bb_beg, bb_end;
	unsigned long long beg, end;

	VEC(bbsvec, struct bad_block) bbv = VEC_INITIALIZER;

	bbs->ns_resource = 0;
	bbs->bb_cnt = 0;
	bbs->bbv = NULL;

	if (badblocks_get_namespace_bounds(region, ndns, &ns_beg, &ns_size)) {
		LOG(1, "cannot read namespace's bounds");
		return -1;
	}

	ns_end = ns_beg + ns_size - 1;

	LOG(10, "namespace: begin %llu, end %llu size %llu (in 512B sectors)",
		B2SEC(ns_beg), B2SEC(ns_end + 1) - 1, B2SEC(ns_size));

	struct badblock *bb;
	ndctl_region_badblock_foreach(region, bb) {
		/*
		 * libndctl returns offset and length of a bad block
		 * both expressed in 512B sectors and offset is relative
		 * to the beginning of the region.
		 */
		bb_beg = SEC2B(bb->offset);
		bb_end = bb_beg + SEC2B(bb->len) - 1;

		LOG(10,
			"region bad block: begin %llu end %llu length %u (in 512B sectors)",
			bb->offset, bb->offset + bb->len - 1, bb->len);

		if (bb_beg > ns_end || ns_beg > bb_end)
			continue;

		beg = (bb_beg > ns_beg) ? bb_beg : ns_beg;
		end = (bb_end < ns_end) ? bb_end : ns_end;

		/*
		 * Form a new bad block structure with offset and length
		 * expressed in bytes and offset relative to the beginning
		 * of the namespace.
		 */
		struct bad_block bbn;
		bbn.offset = beg - ns_beg;
		bbn.length = (unsigned)(end - beg + 1);
		bbn.nhealthy = NO_HEALTHY_REPLICA; /* unknown healthy replica */

		/* add the new bad block to the vector */
		if (VEC_PUSH_BACK(&bbv, bbn)) {
			VEC_DELETE(&bbv);
			return -1;
		}

		LOG(4,
			"namespace bad block: begin %llu end %llu length %llu (in 512B sectors)",
			B2SEC(beg - ns_beg), B2SEC(end - ns_beg),
			B2SEC(end - beg) + 1);
	}

	bbs->bb_cnt = (unsigned)VEC_SIZE(&bbv);
	bbs->bbv = VEC_ARR(&bbv);
	bbs->ns_resource = ns_beg + ndctl_region_get_resource(region);

	LOG(4, "number of bad blocks detected: %u", bbs->bb_cnt);

	return 0;
}

/*
 * badblocks_get_badblocks_by_namespace -- (internal) returns bad blocks
 *                                    in the given namespace using the
 *                                    block device badblocks interface.
 *
 * This function works only for fsdax, but does not require any special
 * permissions.
 */
static int
badblocks_get_badblocks_by_namespace(struct ndctl_namespace *ndns,
					struct badblocks *bbs)
{
	ASSERTeq(ndctl_namespace_get_mode(ndns), NDCTL_NS_MODE_FSDAX);

	VEC(bbsvec, struct bad_block) bbv = VEC_INITIALIZER;
	struct badblock *bb;
	ndctl_namespace_badblock_foreach(ndns, bb) {
		struct bad_block bbn;
		bbn.offset = SEC2B(bb->offset);
		bbn.length = (unsigned)SEC2B(bb->len);
		bbn.nhealthy = NO_HEALTHY_REPLICA; /* unknown healthy replica */
		if (VEC_PUSH_BACK(&bbv, bbn)) {
			VEC_DELETE(&bbv);
			return -1;
		}
	}

	bbs->bb_cnt = (unsigned)VEC_SIZE(&bbv);
	bbs->bbv = VEC_ARR(&bbv);
	bbs->ns_resource = 0;

	return 0;
}

/*
 * badblocks_get_badblocks -- (internal) returns bad blocks in the given
 *                            namespace, using the least privileged path.
 */
static int
badblocks_get_badblocks(struct ndctl_region *region,
			struct ndctl_namespace *ndns,
			struct badblocks *bbs)
{
	if (ndctl_namespace_get_mode(ndns) == NDCTL_NS_MODE_FSDAX)
		return badblocks_get_badblocks_by_namespace(ndns, bbs);

	return badblocks_get_badblocks_by_region(region, ndns, bbs);
}

/*
 * badblocks_files_namespace_bus -- (internal) returns bus where the given
 *                                file is located
 */
static int
badblocks_files_namespace_bus(struct ndctl_ctx *ctx,
				const char *path,
				struct ndctl_bus **pbus)
{
	LOG(3, "ctx %p path %s pbus %p", ctx, path, pbus);

	ASSERTne(pbus, NULL);

	struct ndctl_region *region;
	struct ndctl_namespace *ndns;

	os_stat_t st;

	if (os_stat(path, &st)) {
		ERR("!stat %s", path);
		return -1;
	}

	int rv = ndctl_region_namespace(ctx, &st, &region, &ndns);
	if (rv) {
		LOG(1, "getting region and namespace failed");
		return -1;
	}

	if (!region) {
		ERR("region unknown");
		return -1;
	}

	*pbus = ndctl_region_get_bus(region);

	return 0;
}

/*
 * badblocks_files_namespace_badblocks_bus -- (internal) returns badblocks
 *                                          in the namespace where the given
 *                                          file is located
 *                                          (optionally returns also the bus)
 */
static int
badblocks_files_namespace_badblocks_bus(struct ndctl_ctx *ctx,
					const char *path,
					struct ndctl_bus **pbus,
					struct badblocks *bbs)
{
	LOG(3, "ctx %p path %s pbus %p badblocks %p", ctx, path, pbus, bbs);

	struct ndctl_region *region;
	struct ndctl_namespace *ndns;

	os_stat_t st;

	if (os_stat(path, &st)) {
		ERR("!stat %s", path);
		return -1;
	}

	int rv = ndctl_region_namespace(ctx, &st, &region, &ndns);
	if (rv) {
		LOG(1, "getting region and namespace failed");
		return -1;
	}

	memset(bbs, 0, sizeof(*bbs));

	if (region == NULL || ndns == NULL)
		return 0;

	if (pbus)
		*pbus = ndctl_region_get_bus(region);

	return badblocks_get_badblocks(region, ndns, bbs);
}

/*
 * badblocks_files_namespace_badblocks -- returns badblocks in the namespace
 *                                      where the given file is located
 */
int
badblocks_files_namespace_badblocks(const char *path, struct badblocks *bbs)
{
	LOG(3, "path %s", path);

	struct ndctl_ctx *ctx;

	if (ndctl_new(&ctx)) {
		ERR("!ndctl_new");
		return -1;
	}

	int ret = badblocks_files_namespace_badblocks_bus(ctx, path, NULL, bbs);

	ndctl_unref(ctx);

	return ret;
}

/*
 * badblocks_devdax_clear_one_badblock -- (internal) clear one bad block
 *                                      in the dax device
 */
static int
badblocks_devdax_clear_one_badblock(struct ndctl_bus *bus,
				unsigned long long address,
				unsigned long long length)
{
	LOG(3, "bus %p address 0x%llx length %llu (bytes)",
		bus, address, length);

	int ret = 0;

	struct ndctl_cmd *cmd_ars_cap = ndctl_bus_cmd_new_ars_cap(bus,
							address, length);
	if (cmd_ars_cap == NULL) {
		ERR("failed to create cmd (bus '%s')",
			ndctl_bus_get_provider(bus));
		return -1;
	}

	if ((ret = ndctl_cmd_submit(cmd_ars_cap)) < 0) {
		ERR("failed to submit cmd (bus '%s')",
			ndctl_bus_get_provider(bus));
		goto out_ars_cap;
	}

	struct ndctl_range range;
	if (ndctl_cmd_ars_cap_get_range(cmd_ars_cap, &range)) {
		ERR("failed to get ars_cap range\n");
		goto out_ars_cap;
	}

	struct ndctl_cmd *cmd_clear_error = ndctl_bus_cmd_new_clear_error(
		range.address, range.length, cmd_ars_cap);

	if ((ret = ndctl_cmd_submit(cmd_clear_error)) < 0) {
		ERR("failed to submit cmd (bus '%s')",
			ndctl_bus_get_provider(bus));
		goto out_clear_error;
	}

	size_t cleared = ndctl_cmd_clear_error_get_cleared(cmd_clear_error);

	LOG(4, "cleared %zu out of %llu bad blocks", cleared, length);

	ret = cleared == length ? 0 : -1;

out_clear_error:
	ndctl_cmd_unref(cmd_clear_error);
out_ars_cap:
	ndctl_cmd_unref(cmd_ars_cap);

	return ret;
}

/*
 * badblocks_devdax_clear_badblocks -- clear the given bad blocks in the dax
 *                                  device (or all of them if 'pbbs' is not set)
 */
int
badblocks_devdax_clear_badblocks(const char *path, struct badblocks *pbbs)
{
	LOG(3, "path %s badblocks %p", path, pbbs);

	struct ndctl_ctx *ctx;
	struct ndctl_bus *bus;
	int ret = -1;

	if (ndctl_new(&ctx)) {
		ERR("!ndctl_new");
		return -1;
	}

	struct badblocks *bbs = badblocks_new();
	if (bbs == NULL)
		goto exit_free_all;

	if (pbbs) {
		ret = badblocks_files_namespace_bus(ctx, path, &bus);
		if (ret) {
			LOG(1, "getting bad blocks' bus failed -- %s", path);
			goto exit_free_all;
		}
		badblocks_delete(bbs);
		bbs = pbbs;
	} else {
		ret = badblocks_files_namespace_badblocks_bus(ctx, path, &bus,
									bbs);
		if (ret) {
			LOG(1, "getting bad blocks for the file failed -- %s",
				path);
			goto exit_free_all;
		}
	}

	if (bbs->bb_cnt == 0 || bbs->bbv == NULL) /* OK - no bad blocks found */
		goto exit_free_all;

	LOG(4, "clearing %u bad block(s)...", bbs->bb_cnt);

	unsigned b;
	for (b = 0; b < bbs->bb_cnt; b++) {
		LOG(4,
			"clearing bad block: offset %llu length %u (in 512B sectors)",
			B2SEC(bbs->bbv[b].offset), B2SEC(bbs->bbv[b].length));

		ret = badblocks_devdax_clear_one_badblock(bus,
					bbs->bbv[b].offset + bbs->ns_resource,
					bbs->bbv[b].length);
		if (ret) {
			LOG(1,
				"failed to clear bad block: offset %llu length %u (in 512B sectors)",
				B2SEC(bbs->bbv[b].offset),
				B2SEC(bbs->bbv[b].length));
			goto exit_free_all;
		}
	}

exit_free_all:
	if (!pbbs)
		badblocks_delete(bbs);

	ndctl_unref(ctx);

	return ret;
}

/*
 * badblocks_devdax_clear_badblocks_all -- clear all bad blocks
 *                                         in the dax device
 */
int
badblocks_devdax_clear_badblocks_all(const char *path)
{
	LOG(3, "path %s", path);

	return badblocks_devdax_clear_badblocks(path, NULL);
}

/*
 * badblocks_get -- returns 0 and bad blocks in the 'bbs' array
 *                     (that has to be pre-allocated)
 *                     or -1 in case of an error
 */
int
badblocks_get(const char *file, struct badblocks *bbs)
{
	LOG(3, "file %s badblocks %p", file, bbs);

	ASSERTne(bbs, NULL);

	VEC(bbsvec, struct bad_block) bbv = VEC_INITIALIZER;
	struct extents *exts = NULL;
	long extents = 0;

	unsigned long long bb_beg;
	unsigned long long bb_end;
	unsigned long long bb_len;
	unsigned long long bb_off;
	unsigned long long ext_beg;
	unsigned long long ext_end;
	unsigned long long not_block_aligned;

	int bb_found = -1; /* -1 means an error */

	memset(bbs, 0, sizeof(*bbs));

	if (badblocks_files_namespace_badblocks(file, bbs)) {
		LOG(1, "checking the file for bad blocks failed -- '%s'", file);
		goto error_free_all;
	}

	if (bbs->bb_cnt == 0) {
		bb_found = 0;
		goto exit_free_all;
	}

	exts = Zalloc(sizeof(struct extents));
	if (exts == NULL) {
		ERR("!Zalloc");
		goto error_free_all;
	}

	extents = os_extents_count(file, exts);
	if (extents < 0) {
		LOG(1, "counting file's extents failed -- '%s'", file);
		goto error_free_all;
	}

	if (extents == 0) {
		/* dax device has no extents */
		bb_found = (int)bbs->bb_cnt;

		for (unsigned b = 0; b < bbs->bb_cnt; b++) {
			LOG(4, "bad block found: offset: %llu, length: %u",
				bbs->bbv[b].offset,
				bbs->bbv[b].length);
		}

		goto exit_free_all;
	}

	exts->extents = Zalloc(exts->extents_count * sizeof(struct extent));
	if (exts->extents == NULL) {
		ERR("!Zalloc");
		goto error_free_all;
	}

	if (os_extents_get(file, exts)) {
		LOG(1, "getting file's extents failed -- '%s'", file);
		goto error_free_all;
	}

	bb_found = 0;

	for (unsigned b = 0; b < bbs->bb_cnt; b++) {

		bb_beg = bbs->bbv[b].offset;
		bb_end = bb_beg + bbs->bbv[b].length - 1;

		for (unsigned e = 0; e < exts->extents_count; e++) {

			ext_beg = exts->extents[e].offset_physical;
			ext_end = ext_beg + exts->extents[e].length - 1;

			/* check if the bad block overlaps with file's extent */
			if (bb_beg > ext_end || ext_beg > bb_end)
				continue;

			bb_found++;

			bb_beg = (bb_beg > ext_beg) ? bb_beg : ext_beg;
			bb_end = (bb_end < ext_end) ? bb_end : ext_end;
			bb_len = bb_end - bb_beg + 1;
			bb_off = bb_beg + exts->extents[e].offset_logical
					- exts->extents[e].offset_physical;

			LOG(10,
				"bad block found: physical offset: %llu, length: %llu",
				bb_beg, bb_len);

			/* make sure offset is block-aligned */
			not_block_aligned = bb_off & (exts->blksize - 1);
			if (not_block_aligned) {
				bb_off -= not_block_aligned;
				bb_len += not_block_aligned;
			}

			/* make sure length is block-aligned */
			bb_len = ALIGN_UP(bb_len, exts->blksize);

			LOG(4,
				"bad block found: logical offset: %llu, length: %llu",
				bb_off, bb_len);

			/*
			 * Form a new bad block structure with offset and length
			 * expressed in bytes and offset relative
			 * to the beginning of the file.
			 */
			struct bad_block bb;
			bb.offset = bb_off;
			bb.length = (unsigned)(bb_len);
			/* unknown healthy replica */
			bb.nhealthy = NO_HEALTHY_REPLICA;

			/* add the new bad block to the vector */
			if (VEC_PUSH_BACK(&bbv, bb)) {
				VEC_DELETE(&bbv);
				bb_found = -1;
				goto error_free_all;
			}
		}
	}

error_free_all:
	Free(bbs->bbv);
	bbs->bbv = NULL;
	bbs->bb_cnt = 0;

exit_free_all:
	if (exts) {
		Free(exts->extents);
		Free(exts);
	}

	if (extents > 0 && bb_found > 0) {
		bbs->bbv = VEC_ARR(&bbv);
		bbs->bb_cnt = (unsigned)VEC_SIZE(&bbv);

		LOG(10, "number of bad blocks detected: %u", bbs->bb_cnt);

		/* sanity check */
		ASSERTeq((unsigned)bb_found, bbs->bb_cnt);
	}

	return (bb_found >= 0) ? 0 : -1;
}

/*
 * badblocks_count -- returns number of bad blocks in the file
 *                       or -1 in case of an error
 */
long
badblocks_count(const char *file)
{
	LOG(3, "file %s", file);

	struct badblocks *bbs = badblocks_new();
	if (bbs == NULL)
		return -1;

	int ret = badblocks_get(file, bbs);

	long count = (ret == 0) ? (long)bbs->bb_cnt : -1;

	badblocks_delete(bbs);

	return count;
}

/*
 * badblocks_clear_file -- clear the given bad blocks in the regular file
 *                            (not in a dax device)
 */
static int
badblocks_clear_file(const char *file, struct badblocks *bbs)
{
	LOG(3, "file %s badblocks %p", file, bbs);

	ASSERTne(bbs, NULL);

	int ret = 0;
	int fd;

	if ((fd = os_open(file, O_RDWR)) < 0) {
		ERR("!open: %s", file);
		return -1;
	}

	for (unsigned b = 0; b < bbs->bb_cnt; b++) {
		off_t offset = (off_t)bbs->bbv[b].offset;
		off_t length = (off_t)bbs->bbv[b].length;

		LOG(10,
			"clearing bad block: logical offset %li length %li (in 512B sectors) -- '%s'",
			B2SEC(offset), B2SEC(length), file);

		/* deallocate bad blocks */
		if (fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				offset, length)) {
			ERR("!fallocate");
			ret = -1;
			break;
		}

		/* allocate new blocks */
		if (fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, length)) {
			ERR("!fallocate");
			ret = -1;
			break;
		}
	}

	os_close(fd);

	return ret;
}

/*
 * badblocks_clear -- clears the given bad blocks in a file
 *                       (regular file or dax device)
 */
int
badblocks_clear(const char *file, struct badblocks *bbs)
{
	LOG(3, "file %s badblocks %p", file, bbs);

	ASSERTne(bbs, NULL);

	os_stat_t st;
	if (os_stat(file, &st) < 0) {
		ERR("!stat %s", file);
		return -1;
	}

	enum pmem2_file_type pmem2_type;

	int ret = pmem2_get_type_from_stat(&st, &pmem2_type);
	if (ret) {
		errno = pmem2_err_to_errno(ret);
		return -1;
	}

	if (pmem2_type == PMEM2_FTYPE_DEVDAX)
		return badblocks_devdax_clear_badblocks(file, bbs);

	return badblocks_clear_file(file, bbs);
}

/*
 * badblocks_clear_all -- clears all bad blocks in a file
 *                           (regular file or dax device)
 */
int
badblocks_clear_all(const char *file)
{
	LOG(3, "file %s", file);

	os_stat_t st;
	if (os_stat(file, &st) < 0) {
		ERR("!stat %s", file);
		return -1;
	}

	enum pmem2_file_type pmem2_type;

	int ret = pmem2_get_type_from_stat(&st, &pmem2_type);
	if (ret) {
		errno = pmem2_err_to_errno(ret);
		return -1;
	}

	if (pmem2_type == PMEM2_FTYPE_DEVDAX)
		return badblocks_devdax_clear_badblocks_all(file);

	struct badblocks *bbs = badblocks_new();
	if (bbs == NULL)
		return -1;

	ret = badblocks_get(file, bbs);
	if (ret) {
		LOG(1, "checking bad blocks in the file failed -- '%s'", file);
		goto error_free_all;
	}

	if (bbs->bb_cnt > 0) {
		ret = badblocks_clear_file(file, bbs);
		if (ret < 0) {
			LOG(1, "clearing bad blocks in the file failed -- '%s'",
				file);
			goto error_free_all;
		}
	}

error_free_all:
	badblocks_delete(bbs);

	return ret;
}
