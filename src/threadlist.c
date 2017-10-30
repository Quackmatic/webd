#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "threadlist.h"

static struct thread_list * thread_list = NULL;

int thread_list_create(int socket_fd, void * (*start_routine)(void *)) {
	struct thread_list * list = malloc(sizeof(struct thread_list));
	
	list->socket_fd = socket_fd;
	list->thread_running = 1;
	list->next = thread_list;
	list->cleanup = NULL;
	list->data = NULL;
	thread_list = list;
	if(pthread_create(&list->thread, NULL, start_routine, list) < 0) {
		perror("Could not create thread");
		return -1;
	} else {
		return 0;
	}
}

void thread_list_clean() {
	if(thread_list) {
		struct thread_list * list = thread_list, * next;
		
		if(list->socket_fd == -1) {
			return;
			// Clean has already been started on another thread
		} else {
			while(list) {
				next = list->next;
				thread_list_kill(list);
				list->socket_fd = -1;
				if(list->data && list->cleanup) {
					(*list->cleanup)(list->data);
				}
				free(list);
				list = next;
			}
		}
	}
}

int thread_list_kill(struct thread_list * list) {
	int return_value = 0;
	if(list->thread_running) {
		void * thread_return;
		list->thread_running = 0;
		close(list->socket_fd);
		pthread_join(list->thread, &thread_return);
	}
	return return_value;
}
