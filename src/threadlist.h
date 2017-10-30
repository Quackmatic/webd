#ifndef THREADLIST_H
#define THREADLIST_H

struct thread_list;
typedef void (*thread_cleanup)(void *);

struct thread_list {
	pthread_t thread;
	struct thread_list * next;
	void * data;
	thread_cleanup cleanup;
	int socket_fd, thread_running;
};

int thread_list_create(int socket_fd, void * (*start_routine)(void *));
void thread_list_clean();
int thread_list_kill(struct thread_list * list);

#endif
