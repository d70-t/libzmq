/*
    Copyright (c) 2007-2014 Contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "testutil.hpp"
#include "../include/zmq_utils.h"

// Asynchronous client-to-server (DEALER to ROUTER) - pure libzmq
//
// While this example runs in a single process, that is to make
// it easier to start and stop the example. Each task may have its own
// context and conceptually acts as a separate process. To have this
// behaviour, it is necessary to replace the inproc transport of the
// control socket by a tcp transport.

// This is our client task
// It connects to the server, and then sends a request once per second
// It collects responses as they arrive, and it prints them out. We will
// run several client tasks in parallel, each with a different random ID.

#define CONTENT_SIZE 13
#define CONTENT_SIZE_MAX 32
#define ID_SIZE 10
#define ID_SIZE_MAX 32
#define QT_WORKERS    5
#define QT_CLIENTS    3
#define is_verbose 0

// Our test Hook that uppercase the message from the frontend to the backend and vice versa
struct stats_t {
    void *ctx; // not usefull for the kook itself, but convenient to provide the thread with it without building an additional struct for arguments
    int qt_upper_case;
    int qt_lower_case;
} stats = {NULL, 0, 0};

int
upper_case(void*, void*, void*, zmq_msg_t* msg_, size_t n_, void *stats_)
{
    size_t size = zmq_msg_size(msg_);
    if (!size || n_ == 1) return 0; // skip identity and 0 frames
    char* message = (char*) zmq_msg_data(msg_);
    for (size_t i = 0; i < size; i++)
        if ('a' <= message[i] && message[i] <= 'z')
            message[i] += 'A' - 'a';
    struct stats_t* stats = (struct stats_t*) stats_;
    stats->qt_upper_case++;
    return 0;
}

int
lower_case(void*, void*, void*, zmq_msg_t* msg_, size_t n_, void *stats_)
{
    size_t size = zmq_msg_size(msg_);
    if (!size || n_ == 1) return 0; // skip identity and 0 frames
    char* message = (char*) zmq_msg_data(msg_);
    for (size_t i = 0; i < size; i++)
        if ('A' <= message[i] && message[i] <= 'Z')
            message[i] += 'a' - 'A';
    struct stats_t* stats = (struct stats_t*) stats_;
    stats->qt_lower_case++;
    return 0;
}

zmq_proxy_hook_t hook = {
        &stats, // data used by the hook functions if needed, NULL otherwise
        upper_case, // hook for messages going from frontend to backend
        lower_case // hook for messages going from backend to frontend
};


static void
client_task (void *ctx)
{
    void *client = zmq_socket (ctx, ZMQ_DEALER);
    assert (client);

    // Control socket receives terminate command from main over inproc
    void *control = zmq_socket (ctx, ZMQ_SUB);
    assert (control);
    int rc = zmq_setsockopt (control, ZMQ_SUBSCRIBE, "", 0);
    assert (rc == 0);
    rc = zmq_connect (control, "inproc://control");
    assert (rc == 0);

    char content [CONTENT_SIZE_MAX];
    // Set random identity to make tracing easier
    char identity [ID_SIZE];
    sprintf (identity, "%04X-%04X", rand() % 0xFFFF, rand() % 0xFFFF);
    rc = zmq_setsockopt (client, ZMQ_IDENTITY, identity, ID_SIZE); // includes '\0' as an helper for printf
    assert (rc == 0);
    rc = zmq_connect (client, "tcp://127.0.0.1:9999");
    assert (rc == 0);

    zmq_pollitem_t items [] = { { client, 0, ZMQ_POLLIN, 0 }, { control, 0, ZMQ_POLLIN, 0 } };
    int request_nbr = 0;
    bool run = true;
    while (run) {
        // Tick once per 200 ms, pulling in arriving messages
        int centitick;
        for (centitick = 0; centitick < 20; centitick++) {
            zmq_poll (items, 2, 10);
            if (items [0].revents & ZMQ_POLLIN) {
                int rcvmore;
                size_t sz = sizeof (rcvmore);
                rc = zmq_recv (client, content, CONTENT_SIZE_MAX, 0);
                assert (rc == CONTENT_SIZE);
                if (is_verbose) printf("client receive - identity = %s    content = %s\n", identity, content);
                //  Check that message is still the same
                assert (memcmp (content, "request #", 9) == 0);
                rc = zmq_getsockopt (client, ZMQ_RCVMORE, &rcvmore, &sz);
                assert (rc == 0);
                assert (!rcvmore);
            }
            if (items [1].revents & ZMQ_POLLIN) {
                rc = zmq_recv (control, content, CONTENT_SIZE_MAX, 0);
                if (rc > 0) {
                    if (is_verbose) {
                        if (rc == 9 && memcmp(content, "TERMINATE", 9) == 0)
                            content[9] = '\0'; // required to have a clean output since '\0' is not included in the command
                        printf("client receive - identity = %s    command = %s\n", identity, content);
                    }
                    if (memcmp (content, "STOP", 4) == 0) {
                        run = false;
                        break;
                    }
                }
            }
        }
        sprintf(content, "request #%03d", ++request_nbr); // CONTENT_SIZE
        rc = zmq_send (client, content, CONTENT_SIZE, 0);
        assert (rc == CONTENT_SIZE);
    }

    rc = zmq_close (client);
    assert (rc == 0);
    rc = zmq_close (control);
    assert (rc == 0);
}

// This is our server task.
// It uses the multithreaded server model to deal requests out to a pool
// of workers and route replies back to clients. One worker can handle
// one request at a time but one client can talk to multiple workers at
// once.

static void server_worker (void *ctx);

void
server_task (void *arg)
{
    zmq_proxy_hook_t* hook = (zmq_proxy_hook_t*) arg;
    struct stats_t* stats = (struct stats_t*) hook->data;
    void* ctx = stats->ctx;
    // Frontend socket talks to clients over TCP
    void *frontend = zmq_socket (ctx, ZMQ_ROUTER);
    assert (frontend);
    int rc = zmq_bind (frontend, "tcp://127.0.0.1:9999");
    assert (rc == 0);

    // Backend socket talks to workers over inproc
    void *backend = zmq_socket (ctx, ZMQ_DEALER);
    assert (backend);
    rc = zmq_bind (backend, "inproc://backend");
    assert (rc == 0);

    // Control socket receives terminate command from main over inproc
    void *control = zmq_socket (ctx, ZMQ_SUB);
    assert (control);
    rc = zmq_setsockopt (control, ZMQ_SUBSCRIBE, "", 0);
    assert (rc == 0);
    rc = zmq_connect (control, "inproc://control");
    assert (rc == 0);

    // Launch pool of worker threads, precise number is not critical
    int thread_nbr;
    void* threads [5];
    for (thread_nbr = 0; thread_nbr < QT_WORKERS; thread_nbr++)
        threads[thread_nbr] = zmq_threadstart (&server_worker, ctx);

    // Connect backend to frontend via a proxy
    if (is_verbose)
        printf("---------- standard proxy ----------\n");
    zmq_proxy_steerable (frontend, backend, NULL, control); // until TERMINATE is sent on control
    // Connect backend to frontend via a hooked proxy
    if (is_verbose)
        printf("----------  hooked proxy  ----------\n");
    zmq_proxy_hook (frontend, backend, NULL, hook, control); // until TERMINATE is sent on control

    for (thread_nbr = 0; thread_nbr < QT_WORKERS; thread_nbr++)
        zmq_threadclose (threads[thread_nbr]);

    rc = zmq_close (frontend);
    assert (rc == 0);
    rc = zmq_close (backend);
    assert (rc == 0);
    rc = zmq_close (control);
    assert (rc == 0);
}

// Each worker task works on one request at a time and sends a random number
// of replies back, with random delays between replies:
// The comments in the first column, if suppressed, makes it a poller version

static void
server_worker (void *ctx)
{
    void *worker = zmq_socket (ctx, ZMQ_DEALER);
    assert (worker);
    int rc = zmq_connect (worker, "inproc://backend");
    assert (rc == 0);

    // Control socket receives terminate command from main over inproc
    void *control = zmq_socket (ctx, ZMQ_SUB);
    assert (control);
    rc = zmq_setsockopt (control, ZMQ_SUBSCRIBE, "", 0);
    assert (rc == 0);
    rc = zmq_connect (control, "inproc://control");
    assert (rc == 0);

    char content [CONTENT_SIZE_MAX]; //    bigger than what we need to check that
    char identity [ID_SIZE_MAX];      // the size received is the size sent

    bool run = true;
    while (run) {
        rc = zmq_recv (control, content, CONTENT_SIZE_MAX, ZMQ_DONTWAIT); // usually, rc == -1 (no message)
        if (rc > 0) {
            if (is_verbose) {
                if (rc == 9 && memcmp(content, "TERMINATE", 9) == 0)
                    content[9] = '\0'; // required to have a clean output since '\0' is not included in the command
                printf("server_worker receives command = %s\n", content);
            }
            if (memcmp (content, "STOP", 4) == 0)
                run = false;
        }
        // The DEALER socket gives us the reply envelope and message
        // if we don't poll, we have to use ZMQ_DONTWAIT, if we poll, we can block-receive with 0
        rc = zmq_recv (worker, identity, ID_SIZE_MAX, ZMQ_DONTWAIT);
        if (rc == ID_SIZE) {
            rc = zmq_recv (worker, content, CONTENT_SIZE_MAX, 0);
            assert (rc == CONTENT_SIZE);
            if (is_verbose)
                printf ("server receive - identity = %s    content = %s\n", identity, content);

            // Send 0..4 replies back
            int reply, replies = rand() % 5;
            for (reply = 0; reply < replies; reply++) {
                // Sleep for some fraction of a second
                msleep (rand () % 10 + 1);
                //  Send message from server to client
                rc = zmq_send (worker, identity, ID_SIZE, ZMQ_SNDMORE);
                assert (rc == ID_SIZE);
                rc = zmq_send (worker, content, CONTENT_SIZE, 0);
                assert (rc == CONTENT_SIZE);
            }
        }
    }
    rc = zmq_close (worker);
    assert (rc == 0);
    rc = zmq_close (control);
    assert (rc == 0);
}

// The main thread simply starts several clients and a server, and then
// waits for the server to finish.

int
main (void)
{
    setup_test_environment ();

    void *ctx = zmq_ctx_new ();
    assert (ctx);
    // Control socket receives terminate command from main over inproc
    void *control = zmq_socket (ctx, ZMQ_PUB);
    assert (control);
    int rc = zmq_bind (control, "inproc://control");
    assert (rc == 0);

    void *threads [QT_CLIENTS + 1];
    for (int i = 0; i < QT_CLIENTS; i++)
        threads[i] = zmq_threadstart  (&client_task, ctx);
    stats.ctx = ctx;
    threads[QT_CLIENTS] = zmq_threadstart  (&server_task, &hook);

    msleep (500); // Run for 500 ms the standard proxy
    rc = zmq_send (control, "TERMINATE", 9, 0); // stops the standard proxy
    assert (rc == 9);
    msleep (200); // Run for 200 ms the standard proxy
    rc = zmq_send (control, "TERMINATE", 9, 0); // stops the hooked proxy
    assert (rc == 9);
    rc = zmq_send (control, "STOP", 5, 0); // stops clients and workers (\0 is sent to ease the printf  of the verbose mode)
    assert (rc == 5);

    if (is_verbose) printf("frontend to backend hook hits = %d\nbackend to frontend hook hits = %d\n", stats.qt_upper_case, stats.qt_lower_case);

    rc = zmq_close (control);
    assert (rc == 0);

    for (int i = 0; i < QT_CLIENTS + 1; i++)
        zmq_threadclose (threads[i]);

    rc = zmq_ctx_term (ctx);
    assert (rc == 0);
    return 0;
}
