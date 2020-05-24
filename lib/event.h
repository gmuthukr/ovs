/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef EVENT_H
#define EVENT_H 1

#include "jsonrpc.h"
#include "openvswitch/json.h"
#include "smap.h"
#include "stopwatch.h"
#include "stream.h"

#define EVENT_MAX 256
#define EVENT_POLL_INTERVAL 1000

typedef enum {
    EV_NONE,
    EV_CONDITIONAL,
    EV_MESSAGE
} event_t;

typedef enum {
    EV_RESOURCE_NONE,
    EV_RESOURCE_COVERAGE,
    EV_RESOURCE_TIMER
} resource_t;

typedef enum {
    EV_MATCH_NONE,
    EV_MATCH_EXACT,
    EV_MATCH_RATE_MIN,
    EV_MATCH_RATE_HOUR
} match_t;

typedef enum {
    EV_OP_NONE,
    EV_OP_EQ,
    EV_OP_NE,
    EV_OP_GT,
    EV_OP_GE,
    EV_OP_LT,
    EV_OP_LE
} op_t;

typedef enum {
    EV_UNIT_NONE,
    EV_UNIT_MS,
    EV_UNIT_US
} unit_t;

#define EV_DEF_COMMON struct {\
    resource_t resource;      \
}

#define EV_DEF_COND struct { \
    match_t match;           \
    op_t op;                 \
    unsigned long long value;\
}

#define EV_DEF_MSG struct {    \
    unsigned long long samples;\
    unit_t unit;               \
}

typedef struct {
    EV_DEF_COMMON;
    union {
        EV_DEF_COND;
        EV_DEF_MSG;
    };
} definition_t;

struct event;

struct notify {
    struct stream *stream;
    struct jsonrpc *rpc;
    int(*cb)(struct event *ev);
};

typedef struct notify notify_t;

# define EV_RESOURCE struct {     \
    unsigned long long current;   \
    unsigned int rate_min;        \
    unsigned int rate_hour;       \
    struct stopwatch_stats stats; \
}

struct event {
    EV_RESOURCE;

    char *name;
    event_t type;
    definition_t def;
    notify_t notify;

    uint64_t count;
    uint64_t hit;
    uint64_t hit_prev;
};

bool user_defined_event_enabled(void);
void event_init(const struct smap *ovs_other_config);
int event_try_lock(void);
void event_lock(void) OVS_ACQUIRES(event_mutex);
void event_unlock(void) OVS_RELEASES(event_mutex);
void event_register(const char *name, resource_t type);
uint event_count(void) OVS_REQUIRES(event_mutex);
struct event *event_get(const char *name) OVS_REQUIRES(event_mutex);

typedef enum {
    EV_ERR_NONE,
    EV_PARSE_OBJ_MISSING,
    EV_PARSE_NAME_MISSING,
    EV_PARSE_TYPE_MISSING,
    EV_PARSE_DEF_MISSING,
    EV_PARSE_STREAM_MISSING,
    EV_PARSE_EVENT_EXISTS,
    EV_PARSE_INVALID_EVENT,
    EV_PARSE_INVALID_DEF,
    EV_STREAM_OPEN_ERROR
} everr_t;

#define EVENT_REGISTER(EVENT, TYPE)             \
    static bool EVENT##_once = true;            \
    if (EVENT##_once) {                         \
        EVENT##_once = false;                   \
        event_register(#EVENT, TYPE);           \
    }

#define EVENT_TIMER_START(EVENT)                \
    struct event *ev;                           \
    long long int tsec;                         \
    event_lock();                               \
    ev = event_get(#EVENT);                     \
    event_unlock();                             \
    if (ev != NULL) {                           \
        if (ev->def.unit == EV_UNIT_US) {       \
            tsec = time_usec();                 \
        } else {                                \
            tsec = time_msec();                 \
        }                                       \
        stopwatch_start(#EVENT, tsec);          \
    }

#define EVENT_TIMER_STOP(EVENT)                 \
    if (ev != NULL) {                           \
        if (ev->def.unit == EV_UNIT_US) {       \
            tsec = time_usec();                 \
        } else {                                \
            tsec = time_msec();                 \
        }                                       \
        stopwatch_stop(#EVENT, tsec);           \
        event_lock();                           \
        ev->count++;                            \
        event_unlock();                         \
    }

#define EVENT_TIMER_START_TRY(EVENT)            \
    struct event *ev = NULL;                    \
    long long int tsec;                         \
    if (!event_try_lock()) {                    \
        ev = event_get(#EVENT);                 \
        event_unlock();                         \
    }                                           \
    if (ev != NULL) {                           \
        if (ev->def.unit == EV_UNIT_US) {       \
            tsec = time_usec();                 \
        } else {                                \
            tsec = time_msec();                 \
        }                                       \
        stopwatch_start(#EVENT, tsec);          \
    }

#define EVENT_TIMER_STOP_TRY(EVENT)             \
    if (!event_try_lock()) {                    \
        if (ev != NULL) {                       \
            if (ev->def.unit == EV_UNIT_US) {   \
                tsec = time_usec();             \
            } else {                            \
                tsec = time_msec();             \
            }                                   \
            stopwatch_stop(#EVENT, tsec);       \
            ev->count++;                        \
        }                                       \
        event_unlock();                         \
    }

#define EVENT_FUNC_TIMER(FUNC, ...)             \
    if (user_defined_event_enabled()) {         \
        EVENT_REGISTER(FUNC, EV_RESOURCE_TIMER);\
        EVENT_TIMER_START(FUNC)                 \
        FUNC(__VA_ARGS__);                      \
        EVENT_TIMER_STOP(FUNC)                  \
    } else {                                    \
        FUNC(__VA_ARGS__);                      \
    }

#define EVENT_FUNC_TIMER_TRY(FUNC, ...)         \
    if (user_defined_event_enabled()) {         \
        EVENT_REGISTER(FUNC, EV_RESOURCE_TIMER);\
        EVENT_TIMER_START_TRY(FUNC)             \
        FUNC(__VA_ARGS__);                      \
        EVENT_TIMER_STOP_TRY(FUNC)              \
    } else {                                    \
        FUNC(__VA_ARGS__);                      \
    }

#define EVENT_RETFUNC_TIMER(RET, FUNC, ...)     \
    if (user_defined_event_enabled()) {         \
        EVENT_REGISTER(FUNC, EV_RESOURCE_TIMER);\
        EVENT_TIMER_START(FUNC)                 \
        RET = FUNC(__VA_ARGS__);                \
        EVENT_TIMER_STOP(FUNC)                  \
    } else {                                    \
        RET = FUNC(__VA_ARGS__);                \
    }

#define EVENT_RETFUNC_TIMER_TRY(RET, FUNC, ...) \
    if (user_defined_event_enabled()) {         \
        EVENT_REGISTER(FUNC, EV_RESOURCE_TIMER);\
        EVENT_TIMER_START_TRY(FUNC)             \
        RET = FUNC(__VA_ARGS__);                \
        EVENT_TIMER_STOP_TRY(FUNC)              \
    } else {                                    \
        RET = FUNC(__VA_ARGS__);                \
    }

#define EVENT_COUNTER(NAME, VAR, VALUE)         \
    if (user_defined_event_enabled()) {         \
        struct event *ev;                       \
        event_lock();                           \
        ev = event_get(#NAME);                  \
        if (ev != NULL) {                       \
            ev->VAR = VALUE;                    \
        }                                       \
        event_unlock();                         \
    }

#define EVENT_COUNTER_TRY(NAME, VAR, VALUE)     \
    if (user_defined_event_enabled()) {         \
        struct event *ev;                       \
        const char *name;                       \
        name = NAME;                            \
        if (!event_try_lock()) {                \
            ev = event_get(name);               \
            if (ev != NULL) {                   \
                ev->VAR = VALUE;                \
            }                                   \
            event_unlock();                     \
        }                                       \
    }

#endif /* event.h */
