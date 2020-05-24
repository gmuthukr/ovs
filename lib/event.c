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
#include "jsonrpc.h"
#include "openvswitch/poll-loop.h"
#include "openvswitch/shash.h"
#include "openvswitch/vlog.h"
#include "ovs-thread.h"
#include "smap.h"
#include "stream.h"
#include "timeval.h"
#include "unixctl.h"
#include "util.h"

VLOG_DEFINE_THIS_MODULE(event);

static bool user_defined_event;
static struct shash events;
static struct shash events_reg;

static pthread_t event_thread_id;
static struct ovs_mutex event_mutex = OVS_MUTEX_INITIALIZER;

static bool
handle_conditional_event(struct event *ev)
    OVS_REQUIRES(event_mutex)
{
    bool ok;

    switch (ev->def.op) {
        case EV_OP_EQ:
            ok = (ev->current == ev->def.value);
            break;
        case EV_OP_NE:
            ok = (ev->current != ev->def.value);
            break;
        case EV_OP_GT:
            ok = (ev->current > ev->def.value);
            break;
        case EV_OP_GE:
            ok = (ev->current >= ev->def.value);
            break;
        case EV_OP_LT:
            ok = (ev->current < ev->def.value);
            break;
        case EV_OP_LE:
            ok = (ev->current <= ev->def.value);
            break;
        case EV_OP_NONE:
            default:
            ok = false;
    }

    if (ok) {
        ev->hit++;
    }

    return ok;
}

static bool
handle_message_event(struct event *ev)
    OVS_REQUIRES(event_mutex)
{
    if (ev->def.resource == EV_RESOURCE_TIMER) {
        if (ev->count >= ev->def.samples) {
            stopwatch_get_stats(ev->name, &ev->stats);
            ev->count = 0;
            ev->hit++;
        }
    }
    return true;
}

static void *
event_thread(void *args OVS_UNUSED)
{
    for (;;) {
        long long int next_refresh;
        struct shash_node *node;
        struct event *ev;
        int error;
        bool ok;

        next_refresh = time_msec() + EVENT_POLL_INTERVAL;
        do {
            ovs_mutex_lock(&event_mutex);
            SHASH_FOR_EACH (node, &events) {
                ev = (struct event *)node->data;

                if (ev->hit && ev->hit > ev->hit_prev) {
                    continue;
                }

                if (ev->type == EV_CONDITIONAL) {
                    ok = handle_conditional_event(ev);
                } else if (ev->type == EV_MESSAGE) {
                    ok = handle_message_event(ev);
                } else {
                    continue;
                }

                if (ok && ev->notify.cb) {
                    ovs_mutex_unlock(&event_mutex);
                    error = ev->notify.cb(ev);
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
    struct jsonrpc_msg *request;
    struct json **str, *data;
    int error;

    str = xmalloc(2 * sizeof(*str));
    ovs_mutex_lock(&event_mutex);
    str[0] = json_string_create(ev->name);
    if (ev->def.resource == EV_RESOURCE_COVERAGE) {
        str[1] = json_integer_create(ev->current);
    } else if (ev->def.resource == EV_RESOURCE_TIMER) {
        str[1] = json_integer_create(ev->stats.pctl_95);
    }
    ovs_mutex_unlock(&event_mutex);

    data = json_array_create(str, 2);
    request = jsonrpc_create_request("ovs_event", data, NULL);
    ovs_mutex_lock(&event_mutex);
    error = jsonrpc_send(ev->notify.rpc, request);
    ovs_mutex_unlock(&event_mutex);
    if (error) {
        return error;
    }
    return 0;
}

static struct shash_node *
event_find(const char *name)
    OVS_REQUIRES(event_mutex)
{
    return shash_find(&events, name);
}

struct event *
event_get(const char *name)
    OVS_REQUIRES(event_mutex)
{
    return shash_find_data(&events, name);
}

uint
event_count(void)
    OVS_REQUIRES(event_mutex)
{
    return shash_count(&events);
}

static void
event_list(struct event **list)
    OVS_EXCLUDED(event_mutex)
{
    struct shash_node *node;
    uint i = 0;

    if (!list) {
        return;
    }

    ovs_mutex_lock(&event_mutex);
    SHASH_FOR_EACH (node, &events) {
        list[i++] = node->data;
    }
    ovs_mutex_unlock(&event_mutex);
}

static bool
event_is_defined(struct json *ev_def)
    OVS_EXCLUDED(event_mutex)
{
    struct shash_node *node;
    struct json *string;

    ovs_assert(ev_def->type == JSON_ARRAY);

    for (int i = 0; i < ev_def->array.n; i++) {
        struct json *json;

        json = ev_def->array.elems[i];
        string = shash_find_data(json_object(json), "name");

        ovs_mutex_lock(&event_mutex);
        node = event_find(json_string(string));
        ovs_mutex_unlock(&event_mutex);

        if (node) {
            return true;
        }
    }
    return false;
}

static struct event *
event_delete(const char *name)
    OVS_REQUIRES(event_mutex)
{
    struct event *ev;
    ev = shash_find_and_delete(&events, name);
    if (ev && ev->def.resource == EV_RESOURCE_TIMER) {
        if (!stopwatch_count()) {
            stopwatch_exit();
        } else {
            stopwatch_delete(ev->name);
        }
    }
    return ev;
}

void
event_register(const char *name, resource_t type)
{
    static resource_t ev_rst[] = {EV_RESOURCE_NONE,
                                  EV_RESOURCE_COVERAGE,
                                  EV_RESOURCE_TIMER};
    static bool events_registry_once = true;
    if (events_registry_once) {
        events_registry_once = false;
        shash_init(&events_reg);
    }

    shash_add(&events_reg, name, (void *)&ev_rst[type]);
}

static int
event_add(struct json *ev_def)
    OVS_EXCLUDED(event_mutex)
{
    struct event *ev;
    struct shash op_map, def_map;
    everr_t everr = EV_ERR_NONE;

    op_t op[] = {EV_OP_NONE,
                 EV_OP_EQ,
                 EV_OP_NE,
                 EV_OP_GT,
                 EV_OP_GE,
                 EV_OP_LT,
                 EV_OP_LE
                };

    char *events_n[EVENT_MAX];
    uint n = 0;

    shash_init(&op_map);
    shash_add(&op_map, "none", (void *)&op[0]);
    shash_add(&op_map, "eq", (void *)&op[1]);
    shash_add(&op_map, "ne", (void *)&op[2]);
    shash_add(&op_map, "gt", (void *)&op[3]);
    shash_add(&op_map, "ge", (void *)&op[4]);
    shash_add(&op_map, "lt", (void *)&op[5]);
    shash_add(&op_map, "le", (void *)&op[6]);

    for (int i = 0; i < ev_def->array.n; i++) {
        struct shash_node *node;
        struct json *string;
        struct json *object;
        struct json *elem;
        char *str;

        shash_init(&def_map);

        elem = ev_def->array.elems[i];
        if (elem->type != JSON_OBJECT) {
            everr = EV_PARSE_OBJ_MISSING;
            goto error;
        }

        string = shash_find_data(json_object(elem), "name");
        if (!string) {
            everr = EV_PARSE_NAME_MISSING;
            goto error;
        }

        ovs_mutex_lock(&event_mutex);
        node = event_find(json_string(string));
        ovs_mutex_unlock(&event_mutex);
        if (node) {
            everr = EV_PARSE_EVENT_EXISTS;
            goto error;
        }

        ev = xmalloc(sizeof(*ev));
        ev->name = string ? xstrdup(json_string(string)) : NULL;

        string = shash_find_data(json_object(elem), "type");
        if (!string) {
            everr = EV_PARSE_TYPE_MISSING;
            goto error;
        }

        if (!strcmp("conditional", json_string(string))) {
            ev->type = EV_CONDITIONAL;
        } else if (!strcmp("message", json_string(string))) {
            ev->type = EV_MESSAGE;
        } else if (!strcmp("none", json_string(string))) {
            ev->type = EV_NONE;
        } else {
            everr = EV_PARSE_INVALID_EVENT;
            goto error;
        }

        ev->current = 0;
        ev->hit = 0;
        ev->hit_prev = 0;
        ev->notify.stream = NULL;
        ev->notify.rpc = NULL;
        ev->notify.cb = NULL;

        object = shash_find_data(json_object(elem), "definition");
        if ((!object) | (object->type != JSON_OBJECT)) {
            everr = EV_PARSE_DEF_MISSING;
            goto error;
        }

        SHASH_FOR_EACH (node, json_object(object)) {
            const struct json *value = node->data;
            unsigned long long *lptr;

            if (value->type == JSON_STRING) {
                shash_add(&def_map, node->name, (void *)json_string(value));
            } else if (value->type == JSON_INTEGER) {
                lptr = xmalloc(sizeof(unsigned long long));
                *lptr = json_integer(value);
                shash_add(&def_map, node->name, (void *)lptr);
            } else {
                everr = EV_PARSE_INVALID_DEF;
                goto error;
            }
        }

        str = xstrdup(shash_find_data(&def_map, "resource"));
        if (!strcmp(str, "coverage_counter")) {
            ev->def.resource = EV_RESOURCE_COVERAGE;
        } else if (!strcmp(str, "timer")) {
            ev->def.resource = EV_RESOURCE_TIMER;
        } else if (!strcmp(str, "none")) {
            ev->def.resource = EV_RESOURCE_NONE;
        } else {
            everr = EV_PARSE_INVALID_DEF;
            goto error;
        }

        if (ev->type == EV_CONDITIONAL) {
            str = shash_find_data(&def_map, "match");
            if (!strcmp(str, "exact")) {
                ev->def.match = EV_MATCH_EXACT;
            } else if (!strcmp(str, "per_min")) {
                ev->def.match = EV_MATCH_RATE_MIN;
            } else if (!strcmp(str, "per_hour")) {
                ev->def.match = EV_MATCH_RATE_HOUR;
            } else {
                everr = EV_PARSE_INVALID_DEF;
                goto error;
            }

            str = shash_find_data(&def_map, "op");
            ev->def.op = *(uint *)shash_find_data(&op_map, (const char *)str);
            ev->def.value = *(unsigned long long *)shash_find_data(
                                                       &def_map, "value");

            if (!ev->def.match || !ev->def.op || !ev->def.value) {
                everr = EV_PARSE_INVALID_DEF;
                goto error;
            }
        }
        if (ev->type == EV_MESSAGE) {
            str = shash_find_data(&def_map, "unit");
            if (!strcmp(str, "ms")) {
                ev->def.unit = EV_UNIT_MS;
            } else if (!strcmp(str, "us")) {
                ev->def.unit = EV_UNIT_US;
            } else {
                everr = EV_PARSE_INVALID_DEF;
                goto error;
            }

            ev->def.samples = *(unsigned long long *)shash_find_data(
                                                       &def_map, "samples");
        }
        shash_destroy(&def_map);

        string = shash_find_data(json_object(elem), "notify");
        if (string) {
            int error;
            char *path;
            struct stream *stream;

            path = xasprintf("unix:%s", json_string(string));
            error = stream_open_block(stream_open(path, &stream, DSCP_DEFAULT),
                                      -1, &stream);
            free(path);
            if (error) {
                everr = EV_STREAM_OPEN_ERROR;
                goto error;
            }

            ev->notify.rpc = jsonrpc_open(stream);
            ev->notify.stream = stream;
            ev->notify.cb = notify_msg;
        }

        ovs_mutex_lock(&event_mutex);
        shash_add(&events, ev->name, (void *)ev);
        ovs_mutex_unlock(&event_mutex);
        events_n[n] = ev->name;
        ++n;

        if (ev->def.resource == EV_RESOURCE_TIMER) {
            unit_t units[4] = {-1, SW_MS, SW_US, SW_NS};
            stopwatch_create(ev->name, units[ev->def.unit]);
        }
    }

    shash_destroy(&op_map);
    if (n) {
        return 0;
    }

    error:
        ovs_mutex_lock(&event_mutex);
        for (int i = 0; i < n; i++) {
            ev = event_delete(events_n[n]);
            if (ev) {
                free(ev->name);
            }
        }
        ovs_mutex_unlock(&event_mutex);
        return everr;
}

static void
event_unixctl_define(struct unixctl_conn *conn, int argc OVS_UNUSED,
                     const char *argv[], void *aux OVS_UNUSED)
{
    struct json *ev_def;
    char *reply;
    bool ok;
    int error;

    ev_def = json_from_file(argv[1]);
    if (!ev_def) {
        reply = xasprintf("Unable to parse json file\n\n");
        goto cleanup;
    }

    if (ev_def->type == JSON_STRING) {
        reply = xstrdup(ev_def->string);
        goto cleanup;
    }

    ok = event_is_defined(ev_def);
    if (ok) {
        reply = xstrdup("One or more events already set\n");
        goto cleanup;
    }

    error = event_add(ev_def);
    switch (error) {
        case EV_ERR_NONE:
            break;
        case EV_PARSE_OBJ_MISSING:
            reply = xstrdup("Unable to add event (array not found)\n");
            goto cleanup;
        case EV_PARSE_NAME_MISSING:
            reply = xstrdup("Unable to add event (missing name)\n");
            goto cleanup;
        case EV_PARSE_TYPE_MISSING:
            reply = xstrdup("Unable to add event (missing type)\n");
            goto cleanup;
        case EV_PARSE_DEF_MISSING:
            reply = xstrdup("Unable to add event (missing definition)\n");
            goto cleanup;
        case EV_PARSE_STREAM_MISSING:
            reply = xstrdup("Unable to add event (missing/invalid stream)\n");
            goto cleanup;
        case EV_PARSE_EVENT_EXISTS:
            reply = xstrdup("Unable to add event (some already exists)\n");
            goto cleanup;
        case EV_PARSE_INVALID_EVENT:
            reply = xstrdup("Unable to add event (invalid event found)\n");
            goto cleanup;
        case EV_PARSE_INVALID_DEF:
            reply = xstrdup("Unable to add event (invalid definition)\n");
            goto cleanup;
        case EV_STREAM_OPEN_ERROR:
            reply = xstrdup("Unable to add event (error in opening stream)\n");
            goto cleanup;
        default:
            reply = xstrdup("Unable to add event (unknown error)\n");
            goto cleanup;
    }
    reply = xasprintf("Added event\n");

    cleanup:
        unixctl_command_reply(conn, reply);
        free(reply);
        json_destroy(ev_def);
}

static void
event_unixctl_undefine(struct unixctl_conn *conn, int argc OVS_UNUSED,
                       const char *argv[] OVS_UNUSED,
                       void *aux OVS_UNUSED)
{
    struct event *ev;
    char *reply;

    ovs_mutex_lock(&event_mutex);
    ev = event_delete(argv[1]);
    ovs_mutex_unlock(&event_mutex);
    if (!ev) {
        unixctl_command_reply(conn, "Unable to clear event\n");
        return;
    }

    free(ev->name);

    reply = xasprintf("Cleared event\n");
    unixctl_command_reply(conn, reply);
    free(reply);
}

static void
event_unixctl_flush(struct unixctl_conn *conn, int argc OVS_UNUSED,
                    const char *argv[] OVS_UNUSED,
                    void *aux OVS_UNUSED)
{
    struct shash_node *node;
    struct event *ev;
    char *reply;

    reply = xasprintf("Deleting all coverage events");
    ovs_mutex_lock(&event_mutex);
    SHASH_FOR_EACH (node, &events) {
        ev = (struct event *)node->data;
        reply = xasprintf("%s\n%s", reply, ev->name);
        ev = event_delete(ev->name);
        if (!ev) {
            reply = xasprintf("%s not_ok!", reply);
        } else {
            reply = xasprintf("%s ok!", reply);
        }

        free(ev->name);
    }
    ovs_mutex_unlock(&event_mutex);
    unixctl_command_reply(conn, reply);
    free(reply);
}

static void
event_unixctl_list(struct unixctl_conn *conn, int argc OVS_UNUSED,
                   const char *argv[],
                   void *aux OVS_UNUSED)
{
    struct event **list;
    struct shash_node *node;
    char *reply;
    char *ev_reg = NULL;
    uint cnt;
    uint i = 0;
    char *op[] = {"none", "==", "!=", ">", ">=", "<", "<="};
    char *units[] = {"none", "ms", "us", "ns"};
    int lv[4] = {0,};

    if (argv[1] && !(
           (!strcmp(argv[1], "--all")) ||
           (!strcmp(argv[1], "--all-timer")) ||
           (!strcmp(argv[1], "--all-coverage"))
        )) {
        reply = xasprintf("Invalid option %s\n", argv[1]);
        unixctl_command_reply(conn, reply);
        free(reply);
        return;
    }

    if (argv[1]) {
        char *rst[] = {"none", "coverage", "timer"};
        resource_t type;

        ev_reg = xasprintf("List of events not yet added:\n");

        SHASH_FOR_EACH (node, &events_reg) {
            if (shash_find(&events, node->name)) {
                continue;
            }
            type = *(resource_t *)node->data;

            if (type == EV_RESOURCE_TIMER && !(
                (!strcmp(argv[1], "--all")) ||
                 !(strcmp(argv[1], "--all-timer"))
                )) {
                continue;
            }

            if (type == EV_RESOURCE_COVERAGE && !(
                (!strcmp(argv[1], "--all")) ||
                 !(strcmp(argv[1], "--all-coverage"))
                )) {
                continue;
            }

            ev_reg = xasprintf("%s\n%s:", ev_reg, node->name);
            ev_reg = xasprintf("%s\n  type: %s", ev_reg, rst[type]);
        }
    }

    ovs_mutex_lock(&event_mutex);
    cnt = event_count();
    ovs_mutex_unlock(&event_mutex);
    if (!cnt) {
        if (!ev_reg) {
            unixctl_command_reply(conn, "No event added\n");
            return;
        }
        unixctl_command_reply(conn, ev_reg);
        free(ev_reg);
        return;
    }

    list = xcalloc(cnt, sizeof(struct event *));
    event_list(list);

    reply = xasprintf("List of events:");
    for (i = 0; i < cnt; i++) {
        if (list[i]->def.resource == EV_RESOURCE_COVERAGE) {
            lv[1] = list[i]->current;
            lv[2] = list[i]->rate_min;
            lv[3] = list[i]->rate_hour;
            reply = xasprintf("%s\n%s:", reply, list[i]->name);
            reply = xasprintf("%s\n  resource      : coverage", reply);
            reply = xasprintf("%s\n  current       : %llu", reply,
                              list[i]->current);
            reply = xasprintf("%s\n  rate_per_min  : %u", reply,
                              list[i]->rate_min);
            reply = xasprintf("%s\n  rate_per_hour : %u", reply,
                              list[i]->rate_hour);
            reply = xasprintf("%s\n  condition     : %d %s %llu", reply,
                              lv[list[i]->def.match],
                              op[list[i]->def.op],
                              list[i]->def.value);
            reply = xasprintf("%s\n  hit count     : %lu\n", reply,
                              list[i]->hit);
        } else if (list[i]->def.resource == EV_RESOURCE_TIMER) {
            reply = xasprintf("%s\n%s:", reply, list[i]->name);
            reply = xasprintf("%s\n  resource       : timer", reply);
            reply = xasprintf("%s\n  no_of_samples  : %llu", reply,
                              list[i]->def.samples);
            reply = xasprintf("%s\n  max duration   : %llu (%s)", reply,
                              list[i]->stats.max, units[list[i]->def.unit]);
            reply = xasprintf("%s\n  min duration   : %llu (%s)",reply,
                              list[i]->stats.min, units[list[i]->def.unit]);
            reply = xasprintf("%s\n  95%% of times   : %f (%s)", reply,
                              list[i]->stats.pctl_95,
                              units[list[i]->def.unit]);
            reply = xasprintf("%s\n  hit count      : %lu\n", reply,
                              list[i]->hit);
        } else {
            reply = xasprintf("%s\n%s: (unknown)", reply, list[i]->name);
        }

    }

    if (ev_reg) {
        reply = xasprintf("%s\n%s", reply, ev_reg);
        free(ev_reg);
    }

    unixctl_command_reply(conn, reply);
    free(reply);
}

bool
user_defined_event_enabled(void)
{
    return user_defined_event;
}

void
event_init(const struct smap *ovs_other_config)
{
    if (smap_get_bool(ovs_other_config, "user_defined_event_enable", false)) {

        static struct ovsthread_once once = OVSTHREAD_ONCE_INITIALIZER;
        if (ovsthread_once_start(&once)) {
            shash_init(&events);

            unixctl_command_register("event/define", "<event_file.json>", 1, 1,
                                     event_unixctl_define, NULL);
            unixctl_command_register("event/undefine", "<event_name>", 1, 1,
                                     event_unixctl_undefine, NULL);
            unixctl_command_register("event/flush", "", 0, 0,
                                     event_unixctl_flush, NULL);
            unixctl_command_register("event/list",
                                     "[--all|-all-coverage|--all-timer]", 0, 1,
                                     event_unixctl_list, NULL);

            event_thread_id = ovs_thread_create("event", event_thread, NULL);
            user_defined_event = true;
            VLOG_INFO("User defined event support enabled");
            ovsthread_once_done(&once);
        }
    } else {
        VLOG_INFO("User defined event support disbled");
    }
}
