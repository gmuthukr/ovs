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

#include "openvswitch/hmap.h"
#include "openvswitch/json.h"
#include "stream.h"
#include "jsonrpc.h"

#define EVENT_MAX 256
#define EVENT_POLL_INTERVAL 1000

enum cond_type {
   type_none,
   exact,
   rate,
};

enum op_type {
   op_none,
   eq,
   ne,
   gt,
   ge,
   lt,
   le,
};

typedef struct condition event_cond_t;

struct condition {
    enum cond_type type;
    enum op_type op;
    unsigned long long value;
};

struct event;

struct notify {
    struct stream *stream;
    struct jsonrpc *rpc;
    int(*cb)(struct event *ev);
};

struct event {
    char *name;
    struct notify *notify;
    event_cond_t cond;
    unsigned long long current;
    uint64_t hit;
    uint64_t hit_prev;
};

struct event_node {
    struct hmap_node node;
    char *name;
    struct event *ev;
};

int event_try_lock(void);
void event_lock(void) OVS_ACQUIRES(event_mutex);
void event_unlock(void) OVS_RELEASES(event_mutex);
void event_init(void);
int event_add(struct json *ev_def) OVS_EXCLUDED(event_mutex);
bool event_is_added(struct json *ev_def) OVS_EXCLUDED(event_mutex);
int event_delete(const char *name) OVS_REQUIRES(event_mutex);
void event_list(struct event **list) OVS_EXCLUDED(event_mutex);
uint event_count(void) OVS_REQUIRES(event_mutex);
struct event *event_get(const char *name) OVS_REQUIRES(event_mutex);

enum event_error {
    EVENT_ERR_NONE,
    EVENT_NOT_FOUND,
    EVENT_OBJ_MISMATCH,
    EVENT_OBJ_INVALID,
    EVENT_OBJ_NOSTREAM,
};

#endif /* event.h */
