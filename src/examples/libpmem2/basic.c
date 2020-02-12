// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2019, Intel Corporation */

/*
 * basic.c -- simple example for the libpmem2
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <io.h>
#endif
#include <libpmem2.h>

int
main(int argc, char *argv[])
{
	int fd;
	struct pmem2_config *cfg;
	struct pmem2_map *map;
	pmem2_persist_fn persist;

	if (argc != 2) {
		fprintf(stderr, "usage: %s file\n", argv[0]);
		exit(1);
	}

	if ((fd = open(argv[1], O_RDWR)) < 0) {
		perror("open");
		exit(1);
	}

	if (pmem2_config_new(&cfg)) {
		fprintf(stderr, "%s\n", pmem2_errormsg());
		exit(1);
	}

	if (pmem2_config_set_fd(cfg, fd)) {
		fprintf(stderr, "%s\n", pmem2_errormsg());
		exit(1);
	}

	if (pmem2_config_set_required_store_granularity(cfg,
			PMEM2_GRANULARITY_PAGE)) {
		fprintf(stderr, "%s\n", pmem2_errormsg());
		exit(1);
	}

	if (pmem2_map(cfg, &map)) {
		fprintf(stderr, "%s\n", pmem2_errormsg());
		exit(1);
	}

	char *addr = pmem2_map_get_address(map);
	size_t size = pmem2_map_get_size(map);

	strcpy(addr, "hello, persistent memory");

	persist = pmem2_get_persist_fn(map);

	/* remove the condition after adding a function implementation */
	if (0)
		persist(addr, size);

	pmem2_unmap(&map);
	pmem2_config_delete(&cfg);
	close(fd);

	return 0;
}
