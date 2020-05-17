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
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "stream.h"
#include "jsonrpc.h"
#include "openvswitch/json.h"
#include "openvswitch/poll-loop.h"
#include "util.h"

static int
handle_rpc(struct jsonrpc *rpc, struct jsonrpc_msg *msg, bool *done)
{
    if (msg->type == JSONRPC_REQUEST) {
        struct jsonrpc_msg *reply = NULL;

        if (!strcmp(msg->method, "echo")) {
            reply = jsonrpc_create_reply(json_clone(msg->params), msg->id);
        } else if (!strcmp(msg->method, "ovs_event")) {
            char *params_s = json_to_string(msg->params, 0);
            char *id_s = json_to_string(msg->id, 0);
            printf("received msg %s(%s), id=%s\n",
                 msg->method, params_s, id_s);

            struct json *object = json_object_create();
            json_object_put_string(object, "status", "ok");
            reply = jsonrpc_create_reply(object, msg->id);
            free(params_s);
            free(id_s);
        } else {
            struct json *error = json_object_create();
            json_object_put_string(error, "error", "unknown method");
            reply = jsonrpc_create_error(error, msg->id);
            ovs_error(0, "unknown msg %s", msg->method);
        }

        jsonrpc_send(rpc, reply);
        return 0;
    } else if (msg->type == JSONRPC_NOTIFY) {
        if (!strcmp(msg->method, "shutdown")) {
            *done = true;
            return 0;
        } else {
            ovs_error(0, "unknown notification %s", msg->method);
            return ENOTTY;
        }
    } else {
        ovs_error(0, "unsolicited JSON-RPC reply or error");
        return EPROTO;
    }
}

int main(int argc, char *argv[])
{
    char *path;
    struct pstream *listen;
    struct jsonrpc **rpcs;
    size_t n_rpcs, allocated_rpcs;
    int error;
    bool done;

    if (argc != 2) {
        printf("USAGE: %s socket_file_to_create\n", argv[0]);
        return -1;
    }

    path = xasprintf("punix:%s", argv[1]);
    error = jsonrpc_pstream_open(path, &listen, 0);
    free(path);

    if (error) {
        printf("unable to open listening socket");
        return -1;
    }

    rpcs = NULL;
    done = false;
    n_rpcs = allocated_rpcs = 0;
    for (;;) {
        struct stream *stream;
        size_t i;

        error = pstream_accept(listen, &stream);
        if (!error) {
            if (n_rpcs >= allocated_rpcs) {
                rpcs = x2nrealloc(rpcs, &allocated_rpcs, sizeof *rpcs);
            }
            rpcs[n_rpcs++] = jsonrpc_open(stream);
        } else if (error != EAGAIN) {
            printf("pstream_accept failed");
        }

        for (i = 0; i < n_rpcs; ) {
            struct jsonrpc *rpc = rpcs[i];
            struct jsonrpc_msg *msg;

            jsonrpc_run(rpc);
            if (!jsonrpc_get_backlog(rpc)) {
                error = jsonrpc_recv(rpc, &msg);
                if (!error) {
                    error = handle_rpc(rpc, msg, &done);
                    jsonrpc_msg_destroy(msg);
                } else if (error == EAGAIN) {
                    error = 0;
                }
            }

            if (!error) {
                error = jsonrpc_get_status(rpc);
            }
            if (error) {
                jsonrpc_close(rpc);
                ovs_error(error, "connection closed");
                memmove(&rpcs[i], &rpcs[i + 1],
                        (n_rpcs - i - 1) * sizeof *rpcs);
                n_rpcs--;
            } else {
                i++;
            }
        }

        /* Wait for something to do. */
        if (done && !n_rpcs) {
            break;
        }
        pstream_wait(listen);
        for (i = 0; i < n_rpcs; i++) {
            struct jsonrpc *rpc = rpcs[i];

            jsonrpc_wait(rpc);
            if (!jsonrpc_get_backlog(rpc)) {
                jsonrpc_recv_wait(rpc);
            }
        }
        poll_block();
    }
    free(rpcs);
    pstream_close(listen);
    return 0;
}
