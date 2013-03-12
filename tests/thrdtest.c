#define THREAD 100
#define MAX_TASKS  1000

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "threadpool.h"

int tasks = 0, done = 0;
pthread_mutex_t lock;

void dummy_task(void *arg) {
    usleep(10);
    pthread_mutex_lock(&lock);
    done++;
    pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv)
{
    threadpool_t *pool;
    int i = 0;

    pthread_mutex_init(&lock, NULL);

    pool = threadpool_create(THREAD, &dummy_task, 0);
    fprintf(stderr, "Pool started with %d threads\n", THREAD );

    while( i++ < MAX_TASKS )
    {
        threadpool_add(pool, NULL, 0);
        pthread_mutex_lock(&lock);
        tasks++;
        pthread_mutex_unlock(&lock);
    }
    fprintf(stderr, "Added %d tasks\n", tasks);

    while(tasks / 2 > done) {
        sleep(1);
    }
    fprintf(stderr, "Did %d tasks before shutdown\n", done);
    threadpool_destroy(pool, 0);
    fprintf(stderr, "Did %d tasks\n", done);

    return 0;
}
