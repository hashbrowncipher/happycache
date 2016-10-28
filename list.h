#ifndef LIST_H_INCLUDED
#define LIST_H_INCLUDED

#include <pthread.h>
#include <semaphore.h>

#define container_of(ptr, type, member) ({			\
	const typeof( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct singly_linked {
	struct singly_linked * next;
};
typedef struct singly_linked sl;

struct singly_linked_list {
	sl * head;
	sl ** tail;

	pthread_mutex_t head_lock;
	pthread_mutex_t tail_lock;
	sem_t items;
};
typedef struct singly_linked_list sll;

void list_init(sll * list);
sl * list_pop_head(sll * list);
void list_push_head(sll * list, sl * item);
void list_push_tail(sll * list, sl * l);

#endif
