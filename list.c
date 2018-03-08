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
						// Nobody has come along and modified the tail, so just
						// update the tail pointer and let the next enqueue
						// handle it.
						list->tail = &list->head;
					} else {
						// A concurrent writer has modified the tail (which is
						// ret->next). Just take what they did and put it in
						// list->head.
						list->head = ret->next;
					}

					pthread_mutex_unlock(&list->tail_lock);
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
	bool lock_head = false;
	//Clear any existing value out
	l->next = NULL;

	pthread_mutex_lock(&list->tail_lock);
	if(list->tail == &list->head) {
		lock_head = true;
	}
	*list->tail = l;
	list->tail = &l->next;
	pthread_mutex_unlock(&list->tail_lock);

	if(lock_head) {
		// If we modified list->head, we need to lock head_lock to allow all
		// threads in the list_pop_head critical section to exit or begin
		// waiting on the condition variable.
		pthread_mutex_lock(&list->head_lock);
	}

	// We must do this unconditionally, because even if we didn't modify head,
	// there may be waiters stacked up from when the list was last empty.
	pthread_cond_signal(&list->cond);

	if(lock_head) {
		pthread_mutex_unlock(&list->head_lock);
	}
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
