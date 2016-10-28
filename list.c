#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>

#include "list.h"

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

void list_push_head(sll * list, sl * item) {
	bool locked_tail = false;
	if(list->head == NULL) {
		pthread_mutex_lock(&list->tail_lock);
		locked_tail = true;
	}

	pthread_mutex_lock(&list->head_lock);
	list->head = item;
	pthread_mutex_unlock(&list->tail_lock);

	if(locked_tail) {
		pthread_mutex_unlock(&list->tail_lock);
	}
	sem_post(&list->items);
}

void list_push_tail(sll * list, sl * l) {
	pthread_mutex_lock(&list->tail_lock);

	l->next = NULL;
	*list->tail = l;
	list->tail = &l->next;

	pthread_mutex_unlock(&list->tail_lock);
	sem_post(&list->items);
}

void list_init(sll * list) {
	sem_init(&list->items, 0, 0);
	list->head = NULL;
	list->tail = &list->head;
	pthread_mutex_init(&list->head_lock, NULL);
	pthread_mutex_init(&list->tail_lock, NULL);
}


