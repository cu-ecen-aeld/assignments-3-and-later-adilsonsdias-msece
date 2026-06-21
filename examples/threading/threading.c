#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <time.h> 
#include <math.h>
#include <string.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{
	struct thread_data* th_args = (struct thread_data *) thread_param;
	th_args->thread_complete_success = true;    
	
	long ms_obtain = th_args->obtain_mutex;

	struct timespec w_obtain, w_obtain_rem;
	w_obtain.tv_sec = ms_obtain / 1000;
	w_obtain.tv_nsec = (ms_obtain % 1000) * 1000000L;

	while (nanosleep(&w_obtain, &w_obtain_rem) == -1)
	{
		if (errno == EINTR){
			w_obtain = w_obtain_rem;
			continue;
		}
		th_args->thread_complete_success = false;
		break;
	}

	int rc = pthread_mutex_lock(th_args->mutex);

	if (rc != 0){
		th_args->thread_complete_success = false;
		return th_args;
	}

	long ms_release = th_args->release_mutex;

	struct timespec w_release, w_release_rem;
	w_release.tv_sec = ms_release / 1000;
	w_release.tv_nsec = (ms_release % 1000) * 1000000L;

	while (nanosleep(&w_release, &w_release_rem) == -1)
	{	
		if (errno == EINTR){
			w_release = w_release_rem;
			continue;
		}
		th_args->thread_complete_success = false;
		break;
	}
	
	rc = pthread_mutex_unlock(th_args->mutex);
	if (rc != 0){
		th_args->thread_complete_success = false;
	}

    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int obtain_ms, int release_ms)
{
	struct thread_data *th_data = (struct thread_data*) malloc(sizeof(struct thread_data));
	if (th_data == NULL)
	{
		ERROR_LOG("Couldn't allocate memory for thread_data %d: %s", errno, strerror(errno));
		return false;
	}
	
	th_data->mutex = mutex;
	th_data->obtain_mutex = obtain_ms;
	th_data->release_mutex = release_ms;
	th_data->thread_complete_success = false;
		
	int rc = pthread_create(thread, NULL, threadfunc, th_data);
	if (rc != 0){
		ERROR_LOG("Couldn't create thread: %s", strerror(rc));
		free(th_data);
		return false;
	}
    return true;
}

