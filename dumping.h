#include <stdatomic.h>

#include "list.h"

struct dumper_state {
	sll * work_list;
	gzFile outfile;
	uint32_t open_directories;
	pthread_mutex_t outfile_lock;
	sem_t dumping_sem;
};

struct dir_info {
	sl list;
	DIR * dir;
	size_t len;
	char * path;
};

void dump_worker(struct dumper_state * state);

void dumper_init(
	struct dumper_state *state,
	gzFile outfile,
	sll * work_list
);
