#ifndef LIST_H_INCLUDED
#define LIST_H_INCLUDED

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>

#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct singly_linked {
	struct singly_linked * next;
};
typedef struct singly_linked sl;

struct singly_linked_list {
	sl * head;				//guarded by head_lock (RW)
	sl ** tail;				//guarded by tail_lock (RW)
	bool closed;			//guarded by head_lock (RW)
	pthread_cond_t cond;	//guarded by head_lock (waits)

	pthread_mutex_t head_lock;
	pthread_mutex_t tail_lock;
};
typedef struct singly_linked_list sll;

void list_init(sll * list);
sl * list_pop_head(sll * list);
void list_push_head(sll * list, sl * item);
void list_push_tail(sll * list, sl * l);
void list_close(sll * list);

#endif
