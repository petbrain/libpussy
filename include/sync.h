#pragma once

#include <stdatomic.h>
#include <threads.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    cnd_t cond;
    mtx_t mtx;
    atomic_bool flag;
} Event;

Event* create_event();
void delete_event(Event** event_ptr);
void set_event(Event* event);
void clear_event(Event* event);
bool event_is_set(Event* event);
bool wait_event(Event* event, double timeout);

#ifdef __cplusplus
}
#endif
