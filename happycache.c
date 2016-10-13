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
#include <unistd.h>

#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

long page_size;
int page_shift;

struct singly_linked {
	struct singly_linked * next;
};
typedef struct singly_linked sl;

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
};
typedef struct fd_info fd_info;

struct singly_linked_list {
	struct singly_linked * head;
	struct singly_linked ** tail;

	pthread_mutex_t head_lock;
	pthread_mutex_t tail_lock;
	sem_t items;
};
typedef struct singly_linked_list sll;

sll work_list;
sll free_list;

sl * list_pop_head(sll * list) {
	sem_wait(&list->items);

	pthread_mutex_lock(&list->head_lock);
	sl * ret = list->head;
	list->head = ret->next;
	if(list->head == NULL) {
		// If we have emptied the list, we need to reset the tail
		// pointer to equal head.
		pthread_mutex_lock(&list->tail_lock);
		// Double check. This could have changed.
		if(ret->next == NULL) {
			list->tail = &list->head;
		} else {
			list->head = ret->next;
		}
		pthread_mutex_unlock(&list->tail_lock);
	}
	pthread_mutex_unlock(&list->head_lock);

	return ret;
}

void list_push_tail(sll * list, sl * l) {
	pthread_mutex_lock(&list->tail_lock);

	l->next = NULL;
	*list->tail = l;
	list->tail = &l->next;

	pthread_mutex_unlock(&list->tail_lock);
	sem_post(&list->items);
}

void finished_file_op(fd_info * fdi, int fd) {
	if(--fdi->refcount == 0) {
		if(fdi->base_addr != MAP_FAILED) {
			munmap(fdi->base_addr, fdi->num_pages << page_shift);
		}
		close(fd);
	}
}

void list_init(sll * list) {
	sem_init(&list->items, 0, 0);
	list->head = NULL;
	list->tail = &list->head;
	pthread_mutex_init(&list->head_lock, NULL);
	pthread_mutex_init(&list->tail_lock, NULL);
}

struct loader_state {
	sll * work_list;
	sll * free_list;
};

void read_worker(struct loader_state * state) {
	volatile long val = 0;
	while(true) {
		read_work * my_work = container_of(
			list_pop_head(&work_list),
			read_work,
			list
		);
		val ^= *(long *)(my_work->addr);
		list_push_tail(&free_list, &my_work->list);
	}
}

int prepare_file(char * line, fd_info * fds) {
	// Trim newline
	line[strlen(line) - 1] = 0;

	int fd = open(line, O_RDONLY);
	if(-1 == fd) {
		fprintf(stderr, "Could not open %s: ", line);
		perror(NULL);
		return -1;
	}

	fd_info * fdi = &fds[fd];

	fdi->base_addr = MAP_FAILED;
	fdi->refcount = 1;
	fdi->num_pages = -1;

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

	if(posix_madvise(fdi->base_addr, file_stat.st_size, POSIX_MADV_RANDOM) != 0) {
		fprintf(stderr, "Could not posix_madvise %s: ", line);
		perror(NULL);
		return fd;
	}

	//Only update the struct's num_pages once we're confident everything's
	//tip-top
	fdi->num_pages = num_pages;

	return fd;
}

void load_pages(
	FILE * map,
	fd_info * fds,
	int fd,
	sll * free_list,
	sll * work_list
) {
	int64_t num_pages = -1;
	int64_t page = 0;

	char * file_mmap = NULL;

	if(fd != -1) {
		file_mmap = fds[fd].base_addr;
		num_pages = fds[fd].num_pages;
	}

	while(!feof(map)) {
		uint64_t skip;
		if(fscanf(map, "%lu\n", &skip) != 1) {
			break;
		}

		page += skip;

		if(page > num_pages) {
			continue;
		}

		read_work * rw = container_of(
			list_pop_head(free_list),
			read_work,
			list
		);
		finished_file_op(&fds[rw->fd], rw->fd);

		fds[fd].refcount++;
		rw->fd = fd;
		rw->addr = (void *)&file_mmap[page << page_shift];
		rw->base_addr = file_mmap;
		list_push_tail(work_list, &rw->list);
	}

	if(fd != -1) {
		finished_file_op(&fds[fd], fd);
	}
}


int load_from_map(FILE* map, int num_threads) {
	page_size = sysconf(_SC_PAGESIZE);
	page_shift = __builtin_ctz(page_size);

	list_init(&work_list);
	list_init(&free_list);

	uint32_t item_count = num_threads * 2;
	read_work * items = calloc(item_count, sizeof(read_work));
	for(uint32_t i = 0; i < item_count; i++) {
		list_push_tail(&free_list, &items[i].list);
	}

	fd_info * fds = calloc(item_count * 2, sizeof(fd_info));

	struct loader_state args;
	args.work_list = &work_list;
	args.free_list = &free_list;

	pthread_t * threads = calloc(num_threads, sizeof(pthread_t));
	for(uint32_t i = 0; i < num_threads; i++) {
		pthread_create(
			&threads[i],
			NULL,
			(void * (*)(void *)) read_worker,
			(void *) &args
		);
	}

	char line[4096];

	while(!feof(map)) {
		if(fgets(line, sizeof(line), map) == NULL) {
			return 1;
		}
		int fd = prepare_file(line, fds);
		load_pages(map, fds, fd, &free_list, &work_list);
	}

	return 0;
}

void dump_file(int fd, char* fullpath) {
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
				printf("%s\n", fullpath);
				printed_title = 1;
			}
			printf("%lu\n", i - last);
			last = i;
		}
	}

out:
	munmap(file_mmap, file_stat.st_size);
}

void dump_dir(DIR* dir, char* dirname, int dirname_len) {
	struct dirent *ep;
	while((ep = readdir(dir))) {
		if(strcmp(ep->d_name, "..") == 0 ||
			strcmp(ep->d_name, ".") == 0) {
			continue;
		}

		if(!(ep->d_type & (DT_REG|DT_DIR))) {
			continue;
		}

		int basename_len = strlen(ep->d_name);
		int concat_len = basename_len + dirname_len + 2;
		char * fullpath = malloc(concat_len);
		strncpy(fullpath, dirname, dirname_len);
		snprintf(fullpath, concat_len, "%s/%s", dirname, ep->d_name);
		int down_fd = openat(dirfd(dir), ep->d_name, O_RDONLY);

		if(down_fd != -1) {
			if(ep->d_type & DT_REG) {
				dump_file(down_fd, fullpath);
				close(down_fd);
			} else if(ep->d_type & DT_DIR) {
				DIR* dir = fdopendir(down_fd);
				dump_dir(dir, fullpath, concat_len);
				closedir(dir);
			}
		} else {
			fprintf(stderr, "Could not open %s\n", fullpath);
		}

		free(fullpath);
	}
}

void do_usage(char* name) {
	fprintf(stderr, "Usage: %s (dump|load) args...\n", name);
	fprintf(stderr, "  dump [directory]\n");
	fprintf(stderr, "    print out a map of pages that are currently in the page cache. happycache\n");
	fprintf(stderr, "    recursively walks the files in the directory given by <directory>. Only\n");
	fprintf(stderr, "    files in that directory are mapped. If <directory> is not specified, the\n");
	fprintf(stderr, "    current working directory is assumed by default.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  load [filename] [threads]\n");
	fprintf(stderr, "    load pages into the cache using a happycache dump. If no <filename> is\n");
	fprintf(stderr, "    specified, happycache reads from stdin.\n");

	exit(1);
}

void do_load(int argc, char** argv, char * progname) {
	if(argc > 2) {
		do_usage(progname);
	}

	FILE* map = stdin;
	int num_threads = 32;

	if(argc >= 1) {
		map = fopen(argv[0], "r");
		if(map == NULL) {
			perror("Could not open map file");
			exit(1);
		}
	}

	if(argc >= 2) {
		num_threads = strtoul(argv[1], NULL, 10);
		if(num_threads < 0) {
			fputs("Invalid number of threads\n", stderr);
			exit(1);
		}
	}

	load_from_map(map, num_threads);
	if(map != stdin) {
		fclose(map);
	}
}

void do_dump(int argc, char ** argv, char * progname) {
	if(argc > 1) {
		do_usage(progname);
	}

	char * filename = argc >= 1 ? argv[0] : ".";
	DIR* dir = opendir(filename);
	if(NULL == dir) {
		fprintf(stderr, "Could not open %s: ", filename);
		perror(NULL);
		exit(1);
	}
	dump_dir(dir, ".", 1);
	closedir(dir);
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
