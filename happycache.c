#include <dirent.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

int load_from_map(FILE* map) {
	long page_size = sysconf(_SC_PAGESIZE);
	int page_shift = __builtin_ctz(page_size);

	char line[4096];

	while(!feof(map)) {
		if(fgets(line, sizeof(line), map) == NULL) {
			return 1;
		}

		line[strlen(line) - 1] = 0;

		int fd = open(line, O_RDONLY);
		ssize_t num_pages;
		char* file_mmap = NULL;
		if(-1 == fd) {
			num_pages = -1;
		} else {
			struct stat file_stat;
			if(fstat(fd, &file_stat) != 0) {
				return 1;
			}

			num_pages = (file_stat.st_size + page_size - 1) / page_size;
			file_mmap = mmap(0, file_stat.st_size, PROT_READ, MAP_SHARED, fd, 0);
			if(file_mmap == MAP_FAILED) {
				num_pages = -1;
			}

			if(posix_madvise(file_mmap, file_stat.st_size, POSIX_MADV_RANDOM) != 0) {
				num_pages = -1;
			}
		}

		volatile long val __attribute__((unused)) = 0;
		size_t page = 0;

		while(!feof(map)) {
			size_t skip;
			if(fscanf(map, "%lu\n", &skip) != 1) {
				break;
			}

			page += skip;

			if(page <= num_pages) {
				val = file_mmap[page << page_shift];
			}
		}

		if(-1 != fd) {
			close(fd);
		}
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
	fprintf(stderr, "    recursively dumps a directory, defaulting to cwd\n");
	fprintf(stderr, "  load [filename]\n");
	fprintf(stderr, "    loads a dump file, defaulting to stdin\n");
	exit(1);
}

int main(int argc, char** argv) {
	if(argc < 2 || argc > 3) {
		do_usage(argv[0]);
	}

	if(strcmp(argv[1], "load") == 0) {
		FILE* map = stdin;
		if(argc == 3) {
			map = fopen(argv[2], "r");
			if(map == NULL) {
				perror("Could not open map file");
				exit(1);
			}
		}
		load_from_map(map);
		if(map != stdin) {
			fclose(map);
		}
	} else if(strcmp(argv[1], "dump") == 0) {
		char * filename = argc == 3 ? argv[2] : ".";
		DIR* dir = opendir(filename);
		dump_dir(dir, filename, strlen(filename));
		closedir(dir);
	} else {
		do_usage(argv[0]);
	}
}
