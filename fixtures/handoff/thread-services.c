#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_key_t key;
static atomic_int destructed;

static void *worker(void *arg) {
    intptr_t index = (intptr_t)arg;
    pthread_setspecific(key, (void *)(index + 100));
    return (void *)(index * 3);
}

static void key_destructor(void *value) { (void)value; atomic_fetch_add(&destructed, 1); }

static int pthread8(void) {
    pthread_t threads[8];
    pthread_key_create(&key, key_destructor);
    for (intptr_t i = 0; i < 8; ++i)
        if (pthread_create(&threads[i], NULL, worker, (void *)i) != 0) return 1;
    intptr_t sum = 0;
    for (int i = 0; i < 8; ++i) {
        void *result = NULL;
        if (pthread_join(threads[i], &result) != 0) return 2;
        sum += (intptr_t)result;
    }
    printf("pthread8 sum=%ld destructed=%d\n", (long)sum, atomic_load(&destructed));
    return 0;
}

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int go, ready;

static void *waiter(void *arg) {
    (void)arg;
    pthread_mutex_lock(&mutex);
    ++ready;
    while (!go) pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);
    return NULL;
}

static int condvar(void) {
    pthread_t threads[6];
    for (int i = 0; i < 6; ++i) pthread_create(&threads[i], NULL, waiter, NULL);
    for (;;) {
        pthread_mutex_lock(&mutex);
        int r = ready;
        pthread_mutex_unlock(&mutex);
        if (r == 6) break;
        usleep(1000);
    }
    pthread_mutex_lock(&mutex);
    go = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    for (int i = 0; i < 6; ++i) pthread_join(threads[i], NULL);
    printf("condvar broadcast released=6\n");
    return 0;
}

static int tsd(void) {
    pthread_t thread;
    pthread_key_create(&key, NULL);
    pthread_setspecific(key, (void *)7);
    pthread_create(&thread, NULL, worker, (void *)1);
    pthread_join(thread, NULL);
    printf("tsd main=%ld\n", (long)(intptr_t)pthread_getspecific(key));
    return 0;
}

static int async_sema(void) {
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    __block int value = 0;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        value = 42;
        dispatch_semaphore_signal(sema);
    });
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    printf("dispatch async value=%d\n", value);
    return 0;
}

static int group(void) {
    dispatch_group_t g = dispatch_group_create();
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);
    static atomic_int count;
    for (int i = 0; i < 8; ++i) dispatch_group_async(g, q, ^{ atomic_fetch_add(&count, 1); });
    long r = dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    printf("dispatch group r=%ld count=%d\n", r, atomic_load(&count));
    return 0;
}

static int timer(void) {
    dispatch_queue_t q = dispatch_queue_create("t", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t sema = dispatch_semaphore_create(0);
    dispatch_source_t s = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
    __block int ticks = 0;
    dispatch_source_set_timer(s, dispatch_time(DISPATCH_TIME_NOW, 5000000), 5000000, 100000);
    dispatch_source_set_event_handler(s, ^{
        if (++ticks == 3) { dispatch_source_cancel(s); dispatch_semaphore_signal(sema); }
    });
    dispatch_resume(s);
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
    printf("dispatch timer ticks=%d\n", ticks);
    return 0;
}

static int mainq(void) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        dispatch_async(dispatch_get_main_queue(), ^{ printf("main queue ran\n"); fflush(stdout); _exit(0); });
    });
    dispatch_main();
}

int main(int argc, char **argv) {
    const char *m = argc > 1 ? argv[1] : "pthread8";
    int r = 99;
    if (!strcmp(m, "pthread8")) r = pthread8();
    else if (!strcmp(m, "condvar")) r = condvar();
    else if (!strcmp(m, "tsd")) r = tsd();
    else if (!strcmp(m, "async")) r = async_sema();
    else if (!strcmp(m, "group")) r = group();
    else if (!strcmp(m, "timer")) r = timer();
    else if (!strcmp(m, "mainq")) r = mainq();
    fflush(stdout);
    return r;
}
