#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#include <unistd.h>
#include <zlib.h>

#define CHUNK_SIZE (1024 * 1024)
#define MAP_FILENAME ".happycache.gz"

#include "dumping.h"
#include "list.h"

long page_size;
int page_shift;

struct read_work {
	struct singly_linked list;
	int fd;
	void * addr;
	void * base_addr;
};
typedef struct read_work read_work;

struct fd_info {
	void * base_addr;
	size_t num_pages;
	int64_t refcount;
	unsigned char * mincore;
	size_t mincore_start;
};
typedef struct fd_info fd_info;

void finished_file_op(fd_info * fdi, int fd) {
	if(--fdi->refcount == 0) {
		if(fdi->base_addr != MAP_FAILED) {
			munmap(fdi->base_addr, fdi->num_pages << page_shift);
		}
		close(fd);
	}
}

struct loader_state {
	sll * work_list;
	sll * free_list;
};

uint16_t get_concurrency() {
	/*
	 * Ideally this would map directly to the maximum queue depth of the
	 * underlying block device. But that's impossible. So instead we multiply
	 * the number of CPUs by 8. Fun!
	 */
	return sysconf(_SC_NPROCESSORS_ONLN) * 8;
}

void read_worker(struct loader_state * state) {
	volatile long val = 0;
	while(true) {
		sl * item = list_pop_head(state->work_list);
		if(item == NULL) {
			break;
		}
		read_work * my_work = container_of(
			item,
			read_work,
			list
		);
		val ^= *(long *)(my_work->addr);
		list_push_tail(state->free_list, &my_work->list);
	}
}

int prepare_file(char * line, fd_info * fdi) {
	int fd = open(line, O_RDONLY | O_CLOEXEC);
	if(-1 == fd) {
		fprintf(stderr, "Could not open %s: ", line);
		perror(NULL);
		return -1;
	}

	fdi->base_addr = MAP_FAILED;
	fdi->refcount = 1;
	fdi->num_pages = -1;
	fdi->mincore_start = 0;

	struct stat file_stat;
	if(fstat(fd, &file_stat) != 0) {
		fprintf(stderr, "Could not fstat %s: ", line);
		perror(NULL);
		return fd;
	}

	uint64_t num_pages = (file_stat.st_size + page_size - 1) / page_size;

	fdi->base_addr = mmap(0, num_pages << page_shift, PROT_READ, MAP_SHARED, fd, 0);
	if(fdi->base_addr == MAP_FAILED) {
		fprintf(stderr, "Could not mmap %s: ", line);
		perror(NULL);
		return fd;
	}

	//Only update the struct's num_pages once we're confident everything's
	//tip-top
	fdi->num_pages = num_pages;

	uint32_t chunk_pages = num_pages > CHUNK_SIZE ? CHUNK_SIZE : num_pages;
	fdi->mincore = malloc(chunk_pages);
	mincore(fdi->base_addr, chunk_pages * page_size, fdi->mincore);
	return fd;
}

void load_pages(
	fd_info * fdi
	, int fd
	, uint64_t start
	, uint64_t count
) {
	if(fd == -1) {
		return;
	}

	uint64_t num_pages = fdi->num_pages;

	if(num_pages - start < count) {
		count = num_pages - start;
	}

	while(count > 0) {
		uint64_t mincore_offset = start - fdi->mincore_start;
		if(mincore_offset > CHUNK_SIZE) {
			fdi->mincore_start = start;
			mincore_offset = 0;
			uint64_t remaining = num_pages - start;
			uint64_t chunk_pages = remaining > CHUNK_SIZE ? CHUNK_SIZE : remaining;

			mincore(
				fdi->base_addr + fdi->mincore_start * page_size,
				chunk_pages * page_size,
				fdi->mincore
			);
		}

		if((fdi->mincore[mincore_offset] & 0x01) == 0) {
			// 8 pages (32KB) is much smaller than the 128KB typical max_sectors_kb
			uint8_t limit = 8;
			if(count < limit) {
				limit = count;
			}

			posix_fadvise(fd, start << page_shift, limit << page_shift, POSIX_FADV_WILLNEED);
			count -= limit;
			start += limit;
		} else {
			start += 1;
			count -= 1;
		}
	}
}

int load_from_map(gzFile map) {
	page_size = sysconf(_SC_PAGESIZE);
	page_shift = __builtin_ctz(page_size);

#ifdef __linux__
	struct sysinfo info;
	sysinfo(&info);
	uint64_t pages_left = info.totalram >> page_shift;
#else
	uint64_t pages_left = -1;
#endif


	fd_info fdi;
	bzero(&fdi, sizeof(fd_info));

	char line[4096];
	int fd = -1;
	//page == -1 signifies that we are between files
	int64_t page = -1;
	uint64_t count = 0;

	while(pages_left--) {
		if(gzgets(map, line, sizeof(line)) == NULL) {
			if(!gzeof(map)) {
				//The file is corrupt?
				fputs(gzerror(map, NULL), stderr);

				return 1;
			}

			load_pages(&fdi, fd, page - count, count);
			break;
		}

		size_t len = strlen(line) - 1;
		//Trim the trailing newline.
		line[len] = 0;

		if(page >= 0) {
			char * endptr;
			unsigned long skip = strtoul(line, &endptr, 10);
			if(endptr - line != len) {
				//We can't parse this as a number, so it must be a filename.
				//Filenames start either with ./ or /, so this works.
				if(fd != -1) {
					load_pages(&fdi, fd, page - count, count);
					finished_file_op(&fdi, fd);
				}
				page = -1;
			} else if(skip <= 1) {
				page += skip;
				count += 1;
			} else {
				load_pages(&fdi, fd, page - count, count);
				page += skip;
				count = 1;
			}
		}

		if(page < 0) {
			fd = prepare_file(line, &fdi);

			// This is awful. It makes way more sense as page = 0
			page = 1;
			count = 0;
		}
	}

	return 0;
}

void do_usage(char* name) {
	fprintf(stderr, "Usage: %s (dump|load) args...\n", name);
	fprintf(stderr, "  dump\n");
	fprintf(stderr, "    save a map of pages that are currently in the page cache by recursively\n");
	fprintf(stderr, "    walking the current working directory.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  load [threads] [filename]\n");
	fprintf(stderr, "    load pages into the cache using a happycache dump file\n");
	fprintf(stderr, "      threads: the number of threads to read with (default=32)\n");
	fprintf(stderr, "      filename: the happycache dump file to read (default=.happycache.gz)\n");

	exit(1);
}

void do_load(int argc, char** argv, char * progname) {
	if(argc > 2) {
		do_usage(progname);
	}

	char * to_read = MAP_FILENAME;
	if(argc >= 2) {
		to_read = argv[1];
	}

	gzFile map = NULL;
	map = gzopen(to_read, "rb");
	if(map == NULL) {
		perror("Could not open map file");
		exit(1);
	}

	int ret = load_from_map(map);
	gzclose(map);

	if(ret) {
		exit(ret);
	}

	exit(0);
}

void do_dump(int argc, char ** argv, char * progname) {
	if(argc > 1) {
		do_usage(progname);
	}

	char * outfile_name = alloca(32);
	strncpy(outfile_name, ".happycache.gz.XXXXXX", 32);
	int outfile_fd = mkostemp(outfile_name, O_CLOEXEC);
	if(-1 == outfile_fd) {
		fprintf(stderr, "Could not create temporary output file: ");
		perror(NULL);
		exit(1);
	}

	gzFile outfile = gzdopen(outfile_fd, "wb1");

	struct dir_info * work = malloc(sizeof(struct dir_info));
	char * filename = ".";
	work->dir = opendir(filename);
	if(NULL == work->dir) {
		fprintf(stderr, "Could not open directory %s: ", filename);
		perror(NULL);
		exit(1);
	}

	sll work_list;
	list_init(&work_list);

	struct dumper_state state;
	dumper_init(
		&state,
		outfile,
		&work_list
	);

	work->len = 1;
	work->path = malloc(work->len + 1);
	strncpy(work->path, filename, work->len + 1);

	int num_threads = get_concurrency();
	pthread_t * threads = calloc(num_threads, sizeof(pthread_t));
	for(uint32_t i = 0; i < num_threads; i++) {
		pthread_create(
			&threads[i],
			NULL,
			(void * (*)(void *)) dump_worker,
			(void *) &state
		);
	}

	list_push_head(&work_list, &work->list);

	for(uint32_t i = 0; i < num_threads; i++) {
		pthread_join(threads[i], NULL);
	}

	gzclose(outfile);
	rename(outfile_name, MAP_FILENAME);
}

int main(int argc, char** argv) {
	if(argc < 2) {
		do_usage(argv[0]);
	}

	struct sched_param param;
	param.sched_priority = 0;
	sched_setscheduler(0, SCHED_IDLE, &param);

	if(strcmp(argv[1], "load") == 0) {
		do_load(argc - 2, &argv[2], argv[0]);
	} else if(strcmp(argv[1], "dump") == 0) {
		do_dump(argc - 2, &argv[2], argv[0]);
	} else {
		do_usage(argv[0]);
	}
}
