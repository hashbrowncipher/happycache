#include "list.h"

struct dumper_state {
	sll * work_list;
	gzFile outfile;
};

struct dir_info {
	sl list;
	DIR * dir;
	size_t len;
	char * path;
};

void dump_worker(struct dumper_state * state);


