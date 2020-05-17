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

#include <config.h>
#include <stdlib.h>
#include "event.h"
#include "openvswitch/hmap.h"
#include "openvswitch/shash.h"
#include "openvswitch/poll-loop.h"
#include "hash.h"
#include "util.h"
#include "ovs-thread.h"
#include "stream.h"
#include "jsonrpc.h"
#include "unixctl.h"
#include "timeval.h"

static struct hmap events;
static pthread_t event_thread_id;
static struct ovs_mutex event_mutex = OVS_MUTEX_INITIALIZER;

static void *
event_thread(void *args OVS_UNUSED)
{
    for (;;) {
        long long int next_refresh;
        struct event_node *enode;
        struct event *ev;
        event_cond_t cond;
        struct notify *notify;
        unsigned long long current;
        int error;
        bool ok;

        next_refresh = time_msec() + EVENT_POLL_INTERVAL;
        do {
            ovs_mutex_lock(&event_mutex);
            HMAP_FOR_EACH(enode, node, &events) {
                ev = enode->ev;
                if (ev->hit && ev->hit > ev->hit_prev) {
                    continue;
                }

                notify = ev->notify;
                cond = ev->cond;
                current = ev->current;

                /* rate type of cond is not yet implemented */
                /* only exact type is checked */
                switch (cond.op) {
                    case eq:
                        ok = (current == cond.value);
                        break;
                    case ne:
                        ok = (current != cond.value);
                        break;
                    case gt:
                        ok = (current > cond.value);
                        break;
                    case ge:
                        ok = (current >= cond.value);
                        break;
                    case lt:
                        ok = (current < cond.value);
                        break;
                    case le:
                        ok = (current <= cond.value);
                        break;
                    case op_none:
                    default:
                        ok = false;
                }

                if (ok) {
                    ev->hit++;
                }

                if (ok && notify) {
                    ovs_mutex_unlock(&event_mutex);
                    error = notify->cb(ev);
                    ev->hit_prev = (!error) ? ev->hit: ev->hit_prev;
                    ovs_mutex_lock(&event_mutex);
                }

            }
            ovs_mutex_unlock(&event_mutex);

            poll_timer_wait_until(next_refresh);
            poll_block();
        } while (time_msec() < next_refresh);
    }

    return NULL;
}

int
event_try_lock(void)
{
    return ovs_mutex_trylock(&event_mutex);
}

void
event_lock(void)
    OVS_ACQUIRES(event_mutex)
{
    return ovs_mutex_lock(&event_mutex);
}

void
event_unlock(void)
    OVS_RELEASES(event_mutex)
{
    ovs_mutex_unlock(&event_mutex);
}

static int
notify_msg(struct event *ev)
    OVS_EXCLUDED(event_mutex)
{
    struct notify *notify;
    struct jsonrpc_msg *request;
    struct json **str, *data;
    int error;

    str = xmalloc(2 * sizeof(*str));
    ovs_mutex_lock(&event_mutex);
    str[0] = json_string_create(ev->name);
    str[1] = json_integer_create(ev->current);
    notify = ev->notify;
    ovs_mutex_unlock(&event_mutex);

    data = json_array_create(str, 2);
    request = jsonrpc_create_request("ovs_event", data, NULL);
    error = jsonrpc_send(notify->rpc, request);
    if (error) {
        return error;
    }
    return 0;
}

static struct event_node *
event_find(const char *name)
    OVS_REQUIRES(event_mutex)
{
    struct event_node *enode;

    HMAP_FOR_EACH(enode, node, &events) {
        if (!strcmp(enode->name,name)) {
            return enode;
        }
    }

    return NULL;
}

struct event *
event_get(const char *name)
    OVS_REQUIRES(event_mutex)
{
    struct event_node *enode;
    enode = event_find(name);
    return enode ? enode->ev : NULL;
}

uint
event_count(void)
    OVS_REQUIRES(event_mutex)
{
    return hmap_count(&events);
}

void
event_list(struct event **list)
    OVS_EXCLUDED(event_mutex)
{
    struct event_node *enode;
    uint i=0;

    if (!list) {
        return;
    }

    ovs_mutex_lock(&event_mutex);
    HMAP_FOR_EACH(enode, node, &events) {
        list[i++] = enode->ev;
    }
    ovs_mutex_unlock(&event_mutex);
}

bool
event_is_added(struct json *ev_def)
    OVS_EXCLUDED(event_mutex)
{
    struct json *string;
    struct event_node *enode;

    ovs_assert(ev_def->type == JSON_ARRAY);

    for (int i=0; i < ev_def->array.n; i++) {
        struct json *json;

        json = ev_def->array.elems[i];
        if (json->type != JSON_OBJECT) {
            return false;
        }

        string = shash_find_data(json_object(json), "name");
        
        ovs_mutex_lock(&event_mutex);
        enode = event_find(json_string(string));
        ovs_mutex_unlock(&event_mutex);

        if (!enode) {
            return false;
        }
    }
    return true;
}

int
event_add(struct json *ev_def)
    OVS_EXCLUDED(event_mutex)
{
    struct event_node *enode;
    struct event *ev;
    struct shash type_map, op_map, cond_map;
    struct shash_node *snode;
    enum cond_type ct[] = {exact, rate};
    enum op_type ot[] = {eq, ne, gt, ge, lt, le};
    char *events_n[EVENT_MAX];
    uint n=0, error=0;

    if (ev_def->type != JSON_ARRAY) {
        return EVENT_OBJ_MISMATCH;
    }

    shash_init(&type_map);
    shash_add(&type_map, "exact", (void *)&ct[0]);
    shash_add(&type_map, "rate", (void *)&ct[1]);
 
    shash_init(&op_map);
    shash_add(&op_map, "eq", (void *)&ot[0]);
    shash_add(&op_map, "ne", (void *)&ot[1]);
    shash_add(&op_map, "gt", (void *)&ot[2]);
    shash_add(&op_map, "ge", (void *)&ot[3]);
    shash_add(&op_map, "lt", (void *)&ot[4]);
    shash_add(&op_map, "le", (void *)&ot[5]);

    for (int i=0; i < ev_def->array.n; i++) {
        shash_init(&cond_map);
        struct json *string;
        struct json *object;
        struct json *elem;
        char *str;

        elem = ev_def->array.elems[i];
        if (elem->type != JSON_OBJECT) {
            error = EVENT_OBJ_MISMATCH;
            goto error;
        }

        string = shash_find_data(json_object(elem), "name");
        if (!string) {
            error = EVENT_OBJ_INVALID;
            goto error;
        }

        ovs_mutex_lock(&event_mutex);
        enode = event_find(json_string(string));
        ovs_mutex_unlock(&event_mutex);
        if (enode) {
            continue;
        }

        enode = xmalloc(sizeof(*enode));
        ev = xmalloc(sizeof(*ev));

        ev->name = string ? xstrdup(json_string(string)) : NULL;
        enode->name = ev->name;
        enode->ev = ev;
        ev->current = 0;
        ev->hit = 0;
        ev->hit_prev = 0;
        ev->notify = NULL;

        object = shash_find_data(json_object(elem), "condition");
        if (!object || object->type != JSON_OBJECT) {
            error = EVENT_OBJ_INVALID;
            goto error;
        }

        SHASH_FOR_EACH (snode, json_object(object)) {
            const struct json *value = snode->data;
            unsigned long long *lptr;
            if (value->type == JSON_STRING) {
                shash_add(&cond_map, snode->name, (void *)json_string(value));
            } else if (value->type == JSON_INTEGER) {
                lptr = xmalloc(sizeof(unsigned long long));
                *lptr = json_integer(value); 
                shash_add(&cond_map, snode->name, (void *)lptr);
            } else {
                error = EVENT_OBJ_INVALID;
                goto error;
            }
        }

        str = shash_find_data(&cond_map, "type");
        ev->cond.type = *(uint *)shash_find_data(&type_map, (const char *)str);
        str = shash_find_data(&cond_map, "op");
        ev->cond.op = *(uint *)shash_find_data(&op_map, (const char *)str);
        ev->cond.value = *(unsigned long long *)shash_find_data(&cond_map, "value");

        if (!ev->cond.type || !ev->cond.op || !ev->cond.value) {
            error = EVENT_OBJ_INVALID;
            goto error;
        }
        shash_destroy(&cond_map);

        string = shash_find_data(json_object(elem), "socket");
        if (string) {
            char *path;
            struct stream *stream;
            struct notify *notify;

            path = xasprintf("unix:%s", json_string(string));
            error = stream_open_block(stream_open(path, &stream, DSCP_DEFAULT),
                                      -1, &stream);
            free(path);
            if (error) {
               error = EVENT_OBJ_NOSTREAM;
                goto error;
            }

            notify = xmalloc(sizeof(*notify));
            notify->rpc = jsonrpc_open(stream);
            notify->stream = stream;
            notify->cb = notify_msg;
            ev->notify = notify;
        }

        ovs_mutex_lock(&event_mutex);
        hmap_insert(&events, &enode->node, hash_string(ev->name, 0));
        ovs_mutex_unlock(&event_mutex);
        events_n[n] = ev->name;
        ++n;
    }

    shash_destroy(&type_map);
    shash_destroy(&op_map);
    if (n) {
        return 0;
    }

    error:
        ovs_mutex_lock(&event_mutex);
        for (int i=0; i<n; i++) {
            event_delete(events_n[n]);
        }
        ovs_mutex_unlock(&event_mutex);
        return error;
}

int
event_delete(const char *name)
    OVS_REQUIRES(event_mutex)
{
    struct event_node *enode;
    struct event *ev;
    struct notify *notify;

    enode = event_find(name);
    if (enode) {
        hmap_remove(&events, &enode->node);
    }

    if (!enode) {
        return EVENT_NOT_FOUND;
    }

    ev = enode->ev;
    notify = ev->notify;
    if (!event_count()) {
        jsonrpc_close(notify->rpc);
    }
    free(ev->notify);
    free(ev->name);
    free(enode->ev);
    free(enode);
    return 0;
}

static void
event_unixctl_coverage_add(struct unixctl_conn *conn, int argc OVS_UNUSED,
                              const char *argv[], void *aux OVS_UNUSED)
{
    struct json *ev_def;
    char *reply;
    bool ok;
    int error;

    ev_def = json_from_file(argv[1]);
    if (!ev_def) {
        unixctl_command_reply(conn, "Unable to parse json file\n");
        return;
    }

    if (ev_def->type == JSON_STRING) {
        unixctl_command_reply(conn, ev_def->string);
        return;
    }

    ok = event_is_added(ev_def);
    if (ok) {
        unixctl_command_reply(conn, "One or more events already set\n");
        return;
    }

    error = event_add(ev_def);
    switch (error) {
        case EVENT_ERR_NONE:
            break;
        case EVENT_OBJ_MISMATCH:
            unixctl_command_reply(conn, "Unable to add event (array not found)\n");
            goto cleanup;
        case EVENT_OBJ_INVALID:
            unixctl_command_reply(conn, "Unable to add event (invalid entry)\n");
            goto cleanup;
        case EVENT_OBJ_NOSTREAM:
            unixctl_command_reply(conn, "Unable to add event (unable to stream)\n");
            goto cleanup;
        default:
            unixctl_command_reply(conn, "Unable to add event (unknown error)\n");
            goto cleanup;
    }

    reply = xasprintf("Added event\n");
    unixctl_command_reply(conn, reply);
    free(reply);

    cleanup:
        json_destroy(ev_def);
}

static void
event_unixctl_coverage_del(struct unixctl_conn *conn, int argc OVS_UNUSED,
                              const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    char *reply;
    int error;

    ovs_mutex_lock(&event_mutex);
    error = event_delete(argv[1]);
    ovs_mutex_unlock(&event_mutex);
    if (error) {
        unixctl_command_reply(conn, "Unable to clear event\n");
        return;
    }

    reply = xasprintf("Cleared event\n");
    unixctl_command_reply(conn, reply);
    free(reply);
}

static void
event_unixctl_coverage_flush(struct unixctl_conn *conn, int argc OVS_UNUSED,
                              const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    struct event_node *enode;
    struct event *ev;
    char *reply;
    int error;

    reply = xasprintf("Deleting all coverage events");
    ovs_mutex_lock(&event_mutex);
    HMAP_FOR_EACH(enode, node, &events) {
        ev = enode->ev;
        reply = xasprintf("%s\n%s", reply, ev->name);
        error = event_delete(ev->name);
        if (error) {
            reply = xasprintf("%s not_ok!", reply);
        } else {
            reply = xasprintf("%s ok!", reply);
        }
    }
    ovs_mutex_unlock(&event_mutex);
    unixctl_command_reply(conn, reply);
    free(reply);
}

static void
event_unixctl_coverage_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
                              const char *argv[] OVS_UNUSED, void *aux OVS_UNUSED)
{
    struct event **list;
    char *reply;
    uint cnt;
    uint i=0;

    ovs_mutex_lock(&event_mutex);
    cnt = event_count();
    ovs_mutex_unlock(&event_mutex);
    if (!cnt) {
        unixctl_command_reply(conn, "No event added\n");
        return;
    }

    list = xcalloc(cnt, sizeof(struct event *));
    event_list(list);

    reply = xasprintf("%s (%llu)", list[i]->name, list[i]->current);
    for (i = 1; i < cnt; i++) {
        reply = xasprintf("%s\n%s (%llu)", reply, list[i]->name, list[i]->current);
    }

    unixctl_command_reply(conn, reply);
    free(reply);
}

static void
event_unixctl_coverage_show(struct unixctl_conn *conn, int argc OVS_UNUSED,
                              const char *argv[], void *aux OVS_UNUSED)
{
    char *reply;
    struct event *ev;

    ovs_mutex_lock(&event_mutex);
    ev = event_get(argv[1]);
    ovs_mutex_unlock(&event_mutex);
    if (!ev) {
        reply = xasprintf("event %s not found\n", argv[1]);
    } else {
        reply = xasprintf("event %s (hit %ld)\n", ev->name, ev->hit);
    }
    unixctl_command_reply(conn, reply);
    free(reply);
}

void
event_init(void)
{
    hmap_init(&events);

    unixctl_command_register("event/coverage-add", "EVENT_JSON", 1, 1,
                             event_unixctl_coverage_add, NULL);
    unixctl_command_register("event/coverage-del", "EVENT", 1, 1,
                             event_unixctl_coverage_del, NULL);
    unixctl_command_register("event/coverage-flush", "", 0, 0,
                             event_unixctl_coverage_flush, NULL);
    unixctl_command_register("event/coverage-list", "", 0, 0,
                             event_unixctl_coverage_list, NULL);
    unixctl_command_register("event/coverage-show", "EVENT", 1, 1,
                             event_unixctl_coverage_show, NULL);

    static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;
    if (ovsthread_once_start(&once)) {
        event_thread_id = ovs_thread_create("event", event_thread, NULL);
        ovsthread_once_done(&once);
    }
}
