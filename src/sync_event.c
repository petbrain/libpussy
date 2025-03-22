#include <errno.h>

#include "allocator.h"
#include "sync.h"
#include "timespec.h"

Event* create_event()
{
    int err = 0;
    Event* event = allocate(sizeof(Event), true);
    if (!event) {
        errno = ENOMEM;
        return nullptr;
    }
    switch (cnd_init(&event->cond)) {
        case thrd_success:
            break;
        case thrd_nomem:
            err = ENOMEM;
            [[fallthrough]];
        default:
            goto err;
    }
    switch (mtx_init(&event->mtx, mtx_timed | mtx_recursive)) {
        case thrd_success:
            break;
        case thrd_nomem:
            err = ENOMEM;
            [[fallthrough]];
        default:
            goto err;
    }
    return event;

err:
    release((void**) &event, sizeof(Event));
    errno = err;
    return nullptr;
}

void delete_event(Event** event_ptr)
{
    if (!event_ptr) {
        return;
    }
    Event* event = *event_ptr;
    if (event) {
        mtx_destroy(&event->mtx);
        cnd_destroy(&event->cond);
        release((void**) &event, sizeof(Event));
    }
}

void set_event(Event* event)
{
    event->flag = true;
    cnd_broadcast(&event->cond);
}

void clear_event(Event* event)
{
    event->flag = false;
}

bool event_is_set(Event* event)
{
    return event->flag;
}

bool wait_event(Event* event, double timeout)
{
    mtx_lock(&event->mtx);
    bool signalled = event->flag;
    if (signalled) {
        mtx_unlock(&event->mtx);
        return true;
    }
    if (timeout >= 0.0) {
        struct timespec time_point;
        timespec_get(&time_point, TIME_UTC);
        timespec_add(&time_point, timeout);

        if (cnd_timedwait(&event->cond, &event->mtx, &time_point) == thrd_timedout) {
            signalled = false;
        } else {
            signalled = true;
        }
    } else {
        cnd_wait(&event->cond, &event->mtx);
    }
    mtx_unlock(&event->mtx);
    return signalled;
}
