#define OS_OBJECT_USE_OBJC 0
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <string.h>

__attribute__((objc_root_class))
@interface TimerProbe
+ (int)value;
@end

@implementation TimerProbe
+ (int)value {
    return 42;
}
@end

static void marker(const char *message) {
    puts(message);
    fflush(stdout);
}

int main(int argc, char **argv) {
    int startup_only = argc == 2 && strcmp(argv[1], "--startup-only") == 0;
    if (argc != 1 && !startup_only) return 64;

    marker("objc-timer: main");
    @autoreleasepool {
        if ([TimerProbe value] != 42) return 1;
    }
    marker("objc-timer: objc ready");
    if (startup_only) return 0;

    dispatch_queue_t queue = dispatch_queue_create("objc-timer", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t fired = dispatch_semaphore_create(0);
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    if (!timer || !fired) return 2;

    // Keep the semaphore alive if the callback and wait finish concurrently.
    dispatch_retain(fired);
    dispatch_source_set_event_handler(timer, ^{
        marker("objc-timer: callback");
        dispatch_semaphore_signal(fired);
        dispatch_release(fired);
    });
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_MSEC),
                              DISPATCH_TIME_FOREVER, 0);
    dispatch_resume(timer);

    long timed_out = dispatch_semaphore_wait(
        fired, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    dispatch_source_cancel(timer);
    dispatch_release(timer);
    dispatch_release(fired);
    dispatch_release(queue);
    if (timed_out) return 3;

    marker("objc-timer: clean exit");
    return 0;
}
