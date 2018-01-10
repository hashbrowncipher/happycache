#include <dirent.h>
#include <errno.h>
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

#define CHUNK_SIZE (1024 * 1024)

void dumper_init(
	struct dumper_state *state,
	gzFile outfile,
	sll * work_list
)
{
	state->work_list = work_list;
	state->outfile = outfile;
	state->open_directories = 1;
	pthread_mutex_init(&state->outfile_lock, NULL);
}

void dump_file(int fd,
	char* fullpath,
	gzFile outfile,
	pthread_mutex_t * outfile_lock
) {
	long page_size = sysconf(_SC_PAGESIZE);
	struct stat file_stat;
	if(fstat(fd, &file_stat) != 0) {
		fprintf(stderr, "%s: %s\n", strerror(errno), fullpath);
		return;
	}

	uint64_t file_pages = (file_stat.st_size + page_size - 1) / page_size;
	uint64_t processed_pages = 0;
	uint32_t chunk_pages = file_pages > CHUNK_SIZE ? CHUNK_SIZE : file_pages;

	unsigned char *mincore_vec = calloc(1, chunk_pages);
	if(mincore_vec == NULL) {
		fprintf(stderr, "%s: %s\n", strerror(errno), fullpath);
		return;
	}

	while(file_pages > 0) {
		uint32_t chunk_pages = file_pages > CHUNK_SIZE ? CHUNK_SIZE : file_pages;
		uint64_t chunk_bytes = chunk_pages * page_size;

		char* file_mmap = mmap(0, chunk_bytes, PROT_NONE, MAP_SHARED, fd, processed_pages * page_size);
		if(file_mmap == MAP_FAILED) {
			fprintf(stderr, "%s: %s\n", strerror(errno), fullpath);
			goto out;
		}
		file_pages -= chunk_pages;

		int ret = mincore(file_mmap, chunk_bytes, mincore_vec);
		munmap(file_mmap, chunk_bytes);

		if(ret != 0) {
			fprintf(stderr, "mincore() %s: %s\n", strerror(errno), fullpath);
			goto out;
		}

		size_t i = 0;
		for(; i < chunk_pages; i++) {
			if(mincore_vec[i] & 0x01) {
				break;
			}
		}

		ssize_t last = -processed_pages;
		if(i < chunk_pages) {
			pthread_mutex_lock(outfile_lock);

			gzprintf(outfile, "%s\n", fullpath);
			for(; i < chunk_pages; i++) {
				if(mincore_vec[i] & 0x01) {
					gzprintf(outfile, "%lu\n", i - last);
					last = i;
				}
			}
			pthread_mutex_unlock(outfile_lock);
		}

		processed_pages += chunk_pages;
	}


out:
	free(mincore_vec);
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

		int basename_len = strlen(ep->d_name);
		size_t concat_len = basename_len + current->len + 2;
		char * fullpath = malloc(concat_len);
		snprintf(fullpath, concat_len, "%s/%s", current->path, ep->d_name);

		int down_fd = openat(dirfd(current->dir), ep->d_name, O_RDONLY);
		if(down_fd == -1) {
			fprintf(stderr, "%s: %s\n", strerror(errno), fullpath);
			free(fullpath);
			continue;
		}

		unsigned char type = ep->d_type;

		struct stat buf;
		if(type == 0) {
			fstat(down_fd, &buf);
			switch(buf.st_mode & S_IFMT) {
				case S_IFDIR: type = DT_DIR; break;
				case S_IFREG: type = DT_REG; break;
			}
		}

		if(type & DT_DIR) {
			atomic_fetch_add(&state->open_directories, 1);
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
			if(type & DT_REG) {
				dump_file(
					down_fd,
					fullpath,
					state->outfile,
					&state->outfile_lock
				);
			}
			free(fullpath);
			close(down_fd);
		}
	}

	uint32_t ret = atomic_fetch_add(&state->open_directories, -1);
	if(ret == 1) {
		list_close(state->work_list);
	}

	//We just exhausted the directory
	closedir(current->dir);
	free(current->path);
	free(current);
}

void dump_worker(struct dumper_state * state) {
	struct dir_info * work;
	while(true) {
		work = container_of(
			list_pop_head(state->work_list),
			struct dir_info,
			list
		);
		if(work == NULL) {
			break;
		}
		dump_dir(
			work,
			state
		);
	}
}
