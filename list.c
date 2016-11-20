#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

#include "list.h"

sl * list_pop_head(sll * list) {
	pthread_mutex_lock(&list->head_lock);
	while(list->items == 0 && !list->closed) {
		pthread_cond_wait(&list->cond, &list->head_lock);
	}

	sl * ret = list->head;
	if(ret != NULL) {
		atomic_fetch_add_explicit(&list->items, -1, memory_order_relaxed);
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
	}

	pthread_mutex_unlock(&list->head_lock);
	return ret;
}

void list_push_head(sll * list, sl * item) {
	bool locked_tail = false;

	pthread_mutex_lock(&list->head_lock);
	if(list->head == NULL) {
		pthread_mutex_lock(&list->tail_lock);
		locked_tail = true;
	}


	item->next = list->head;
	list->head = item;
	pthread_mutex_unlock(&list->head_lock);

	if(locked_tail) {
		pthread_mutex_unlock(&list->tail_lock);
	}

	atomic_fetch_add(&list->items, 1);
	pthread_cond_signal(&list->cond);
}

void list_push_tail(sll * list, sl * l) {
	pthread_mutex_lock(&list->tail_lock);

	l->next = NULL;
	*list->tail = l;
	list->tail = &l->next;

	pthread_mutex_unlock(&list->tail_lock);
	atomic_fetch_add(&list->items, 1);
	pthread_cond_signal(&list->cond);
}

void list_init(sll * list) {
	list->items = 0;
	list->head = NULL;
	list->tail = &list->head;
	list->closed = false;
	pthread_mutex_init(&list->head_lock, NULL);
	pthread_mutex_init(&list->tail_lock, NULL);
	pthread_cond_init(&list->cond, NULL);
}

void list_close(sll * list) {
	list->closed = true;
	pthread_cond_broadcast(&list->cond);
}
