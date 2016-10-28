#include <dirent.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <zlib.h>

#include "dumping.h"

void dump_file(int fd, char* fullpath, gzFile outfile) {
	long page_size = sysconf(_SC_PAGESIZE);
	struct stat file_stat;
	if(fstat(fd, &file_stat) != 0) {
		return;
	}

	int num_pages = (file_stat.st_size + page_size - 1) / page_size;
	char* file_mmap = mmap(0, file_stat.st_size, PROT_NONE, MAP_SHARED, fd, 0);
	if(file_mmap == MAP_FAILED) {
		return;
	}

	unsigned char *mincore_vec = calloc(1, num_pages);
	if(mincore_vec == NULL) {
		goto out;
	}

	if(mincore(file_mmap, file_stat.st_size, mincore_vec) != 0) {
		goto out;
	}

	int printed_title = 0;
	size_t last = 0;
	for(size_t i = 0; i < num_pages; i++) {
		if(mincore_vec[i] & 0x01) {
			if(!printed_title) {
				gzprintf(outfile, "%s\n", fullpath);
				printed_title = 1;
			}
			gzprintf(outfile, "%lu\n", i - last);
			last = i;
		}
	}

out:
	munmap(file_mmap, file_stat.st_size);
}

void dump_dir(
	struct dir_info * current,
	struct dumper_state * state
) {
	struct dirent *ep;
	while((ep = readdir(current->dir))) {
		if(strcmp(ep->d_name, "..") == 0 ||
			strcmp(ep->d_name, ".") == 0) {
			continue;
		}

		if(!(ep->d_type & (DT_REG|DT_DIR))) {
			continue;
		}

		int basename_len = strlen(ep->d_name);
		size_t concat_len = basename_len + current->len + 2;
		char * fullpath = malloc(concat_len);

		snprintf(fullpath, concat_len, "%s/%s", current->path, ep->d_name);
		int down_fd = openat(dirfd(current->dir), ep->d_name, O_RDONLY);

		if(down_fd != -1 && (ep->d_type & DT_DIR)) {
			//Push the context we were working on to the shared stack
			list_push_head(state->work_list, &current->list);

			//Create a new context
			current = malloc(sizeof(struct dir_info));
			if(current == NULL) {
				perror(NULL);
				exit(1);
			}

			current->path = fullpath;
			current->dir = fdopendir(down_fd);
			current->len = concat_len;
		} else {
			if(down_fd != -1) {
				dump_file(down_fd, fullpath, state->outfile);
				close(down_fd);
			} else {
				fprintf(stderr, "Could not open %s\n", fullpath);
				perror(NULL);
			}
			free(fullpath);
		}
	}

	//We just exhausted the directory
	closedir(current->dir);
	free(current->path);
	free(current);
}

void dump_worker(struct dumper_state * state) {
	struct dir_info * work;
	while(true) {
		fprintf(stderr, "Waiting for work\n");
		work = container_of(
			list_pop_head(state->work_list),
			struct dir_info,
			list
		);
		fprintf(stderr, "Got work\n");
		if(!work) {
			break;
		}
		dump_dir(
			work,
			state
		);
	}
}
