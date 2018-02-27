#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

#include "list.h"

sl * list_pop_head(sll * list) {
	sl * ret = NULL;

	pthread_mutex_lock(&list->head_lock);
	while(true) {
		ret = list->head;

		if(ret != NULL) {
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
			}
			break;
		} else if(list->closed) {
			break;
		} else {
			pthread_cond_wait(&list->cond, &list->head_lock);
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

	pthread_cond_signal(&list->cond);
}

void list_push_tail(sll * list, sl * l) {
	//Clear any existing value out
	l->next = NULL;

	pthread_mutex_lock(&list->tail_lock);
	*list->tail = l;
	list->tail = &l->next;
	pthread_mutex_unlock(&list->tail_lock);

	pthread_cond_signal(&list->cond);
}

void list_init(sll * list) {
	list->head = NULL;
	list->tail = &list->head;
	list->closed = false;
	pthread_mutex_init(&list->head_lock, NULL);
	pthread_mutex_init(&list->tail_lock, NULL);
	pthread_cond_init(&list->cond, NULL);
}

void list_close(sll * list) {
	pthread_mutex_lock(&list->head_lock);
	list->closed = true;
	pthread_mutex_unlock(&list->head_lock);

	pthread_cond_broadcast(&list->cond);
}
