/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <luaconf.h>

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <h2o.h>
#include <h2o/http1.h>
#include <h2o/http2.h>
#include <h2o/memcached.h>
#include <uv.h>
#include <stdatomic.h>

#include "module.h"
#include "libev/ev.h"
#include "queue.h"

struct route;

static int create_listener(void);
static void on_accept(uv_stream_t *listener, int status);
static void alloc_buffer_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void uv_read_wpipe(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);
static void uv_read_stop_pipe(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);
static int h2o_on_req(h2o_handler_t *self, h2o_req_t *req);
static struct route *find_h2o_route(const char *path);
static void destroy_h2o_route(struct route *route);
static h2o_pathconf_t *register_h2o_handler(h2o_hostconf_t *hostconf, const struct route *route,
        int (*on_req)(h2o_handler_t *, h2o_req_t *));

struct request {
        /* Pointer to request data */
        char* data;
        /* Request status, 1 means success */
        int status;
};

typedef struct parameter_handler {
        h2o_handler_t handler;
        void *parameter;
} parameter_handler;

struct route {
        /* Link for route queue */
        LIST_ENTRY(route) entry;
        /* Route name */
        char *path;
        /* Route method */
        char method[7];
        /* A reference to Lua trigger function. */
        int ref;
};

STAILQ_HEAD(responce_queue, request);
LIST_HEAD(route_list, route);

struct module_context {
        /* Specific h2o structs */
        h2o_globalconf_t config;
        h2o_context_t ctx;
        h2o_accept_ctx_t accept_ctx;
        uv_tcp_t listener;
        /* Pipe for fast uv_loop stop */
        int stop_pipe[2];
        uv_pipe_t stop_pipe_read;
        uv_pipe_t stop_pipe_write;
        /* ev loop for tx thread message */
        struct ev_loop *ev_loop;
        /* ev loop internal pipe */
        int wpipe;
        /* Ip address */
        char addr[INET_ADDRSTRLEN];
        /* Pointer to tarantool module itself*/
        void *module;
        /* Listening port */
        unsigned short port;
        /* Current lua state */
        lua_State *L;
        /* Queue of active routes */
        struct route_list route_list;
        /* Flag set when module work */
        _Atomic int work;
};

static int tarantool_module_init(void **ctx, void *module);
static void tarantool_module_start(void *ctx, void *loop, int loopfd);
static void tarantool_module_stop(void *ctx);
static void tarantool_module_destroy(void *ctx);

static struct tarantool_module_vtab vtab  = {
        .init = tarantool_module_init,
        .start = tarantool_module_start,
        .stop = tarantool_module_stop,
        .destroy = tarantool_module_destroy
};

static struct module_context context;

static int
cfg(lua_State *L) 
{
        const char *error;
        if (LIST_EMPTY(&context.route_list)) {
                lua_pushstring(L, "error: need some active routes before start http server");
                lua_error(L);
        }      
        if (lua_gettop(L) != 1 || !lua_istable(L, 1)) {
                lua_pushfstring(L, "error: invalid module params");
                lua_error(L);
        }
        lua_pushstring(L, "port");
        lua_gettable(L, -2);
        if (!lua_isnumber(L, -1)) {
                lua_pushfstring(L, "error: invalid module params, need port number");
                lua_error(L);
        }
        context.port = lua_tointeger(L, -1);        
        lua_pop(L, 1);
        lua_pushstring(L, "ip");
        lua_gettable(L, -2);
        if (!lua_isstring(L, -1)) {
                lua_pushfstring(L, "error: invalid module params, need ip addr");
                lua_error(L);
        }
        strncpy(context.addr, lua_tostring(L, -1), INET_ADDRSTRLEN - 1);
        context.addr[INET_ADDRSTRLEN - 1] = '\0';
        lua_pop(L, -1);

        if (tarantool_start_module(context.module, &error) < 0) {
                lua_pushfstring(L, error);
                lua_error(L);
        }
        return 0;
}

static int
stop(lua_State *L)
{
        const char *error;
        if (tarantool_stop_module(context.module, &error, 0) < 0) {
                lua_pushfstring(L, error);
                lua_error(L);
        }
        atomic_store(&context.work, 0);
        return 0;
}

static int
route(lua_State *L)
{       
        const char *error_msg;
        int pop, top = lua_gettop(L);
        if (atomic_load(&context.work) != 0) {
                error_msg = "error: unbale to set new route when module work";
                goto error;
        }
        if (lua_gettop(L) != 2 || !lua_istable(L, 1) || !lua_isfunction(L, 2)) {
                error_msg = "error: invalid module route params";
                goto error;
        }
        lua_pushstring(L, "path");
        lua_gettable(L, -3);
        if (!lua_isstring(L, -1)) {
                error_msg = "error: invalid module route params, need path";
                goto error;
        }
        const char *path = lua_tostring(L, -1);
        lua_pushstring(L, "method");
        lua_gettable(L, -4);
        if (!lua_isstring(L, -1)) {
                error_msg = "error: invalid module route params, need method";
                goto error;
        }
        const char *method = lua_tostring(L, -1);
        if (strcmp(method, "POST") && strcmp(method, "GET") && 
            strcmp(method, "PUT") && strcmp(method, "DELETE")) {
                error_msg = "error: invalid module route params, method must be POST, GET, PUT or DELETE";
                goto error;
        }
        struct route *route = find_h2o_route(path);
        if (!route) {
                route = (struct route *)malloc(sizeof(struct route));
                if (!route) {
                        error_msg = "error: failed to allocate memory for route";
                        goto error;
                }
                memset(route, 0, sizeof(struct route));
                route->ref = -1;
                route->path = (char *)malloc(strlen(path) + 1);
                if (!route->path) {
                        free(route);
                        error_msg = "error: failed to allocate memory for route path";
                        goto error;
                }
                strcpy(route->method, method);
                strcpy(route->path, path);
                LIST_INSERT_HEAD(&context.route_list, route, entry);
        } else {
                luaL_unref(L, LUA_REGISTRYINDEX, route->ref);
                route->ref = -1;
                strcpy(route->method, method);
        }
        lua_pop(L, 2);
        if (!lua_isfunction(L, -1)) {
                LIST_REMOVE(route, entry);
                destroy_h2o_route(route);
                error_msg = "error: invalid module route params, need function to route path";
                goto error;
        }
        context.L = L;
        route->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        return 0;

error:
        pop = lua_gettop(L) - top;
        lua_pop(L, pop);
        lua_pushfstring(L, error_msg);
        lua_error(L);
        return 0;
}

static const struct luaL_Reg h2o_module_example_lib[] = {
        {"cfg", cfg},
        {"stop", stop},
        {"route", route},
        {NULL, NULL}
}; 

LUALIB_API int
luaopen_h2o_module_example(lua_State *L) 
{
        const char *error;
        luaL_openlib(L, "h2o_module_example", h2o_module_example_lib, 0);
        if (tarantool_register_module("h2o_module_example", &vtab, &error) < 0) {
                lua_pushfstring(L, error);
                lua_error(L);
        }
        return 0;
}

static struct route *
find_h2o_route(const char *path)
{
        struct route *route = NULL;
        LIST_FOREACH(route, &context.route_list, entry) {
                if(!strcmp(route->path, path))
                        break;
        }
        return route;
}

static void 
destroy_h2o_route(struct route *route)
{
        if (route->ref >= 0)
                luaL_unref(context.L, LUA_REGISTRYINDEX, route->ref);
        free(route->path);
        free(route);
}

static void
tx_process_request(void *data)
{
        struct request *request = (struct request *)data;
        struct route *route = (struct route *)request->data;
        lua_rawgeti(context.L, LUA_REGISTRYINDEX, route->ref);
        if (lua_pcall(context.L, 0, LUA_MULTRET, 0) != LUA_OK)
                return;
        /* TODO implement logic */
}


static void
m_process_request(void *data, int failed)
{
        struct request *request = (struct request *)data;
        if (!failed && request->status) {
                /* TODO implement logic */
        } else {
                free(request);
        }
}

static int
tarantool_module_init(void **ctx, void *module)
{
        context.module = module;
        *ctx = &context;
        return 0;
}

static void
tarantool_module_start(void *ctx, void *loop, int wpipe)
{
        struct module_context *context = (struct module_context *)(ctx);
        context->ev_loop = (struct ev_loop *)loop;
        context->wpipe = wpipe;
        uv_loop_t uvloop;
        uv_pipe_t pipew;
        struct route *route;

        memset(&context->config, 0, sizeof(context->config));
        memset(&context->ctx, 0, sizeof(context->ctx));
        memset(&context->accept_ctx, 0, sizeof(context->accept_ctx));
        h2o_config_init(&context->config);
        h2o_hostconf_t *hostconf = h2o_config_register_host(&context->config, 
                h2o_iovec_init(H2O_STRLIT("default")), 65535);    
        LIST_FOREACH(route, &context->route_list, entry)
                register_h2o_handler(hostconf, route, h2o_on_req);
        uv_loop_init(&uvloop);
        h2o_context_init(&context->ctx, &uvloop, &context->config);
        context->accept_ctx.ctx = &context->ctx;
        context->accept_ctx.hosts = context->config.hosts;
        if (create_listener() != 0)
                goto finish;
        uv_pipe_init(&uvloop, &pipew, 0);
        uv_pipe_open(&pipew, wpipe);
        if(pipe(context->stop_pipe) < 0)
                goto close_pipew; 
        uv_pipe_init(&uvloop, &context->stop_pipe_read, 0);
        uv_pipe_open(&context->stop_pipe_read, context->stop_pipe[0]);
        uv_pipe_init(&uvloop, &context->stop_pipe_write, 0);
        uv_pipe_open(&context->stop_pipe_write, context->stop_pipe[1]);
        uv_read_start((uv_stream_t*)&pipew, alloc_buffer_cb, uv_read_wpipe);
        uv_read_start((uv_stream_t*)&context->stop_pipe_read, alloc_buffer_cb, uv_read_stop_pipe);

        while(!ev_prepare_extern_loop_wait(context->ev_loop))
                ev_process_events(context->ev_loop, wpipe, EV_READ);

        atomic_store(&context->work, 1);
        uv_run(context->ctx.loop, UV_RUN_DEFAULT);
        atomic_store(&context->work, 0);

        uv_read_stop((uv_stream_t*)&context->stop_pipe_read);
        uv_read_stop((uv_stream_t*)&pipew);
        uv_close((uv_handle_t *)&context->stop_pipe_write, NULL);
        uv_close((uv_handle_t *)&context->stop_pipe_read, NULL);
close_pipew:
        uv_close((uv_handle_t *)&pipew, NULL);
        uv_close((uv_handle_t *)&context->listener, NULL);
finish:
        uv_loop_close(&uvloop);
}

static void
tarantool_module_stop(void *ctx)
{
        struct module_context *context = (struct module_context *)(ctx);
        uv_stop(context->ctx.loop);
        unsigned int magic = 0xAABBCCDD;
        uv_buf_t buf[] = {
                { .base = (char*)&magic, .len = 1 }
        };
        uv_write_t req;
        uv_write(&req, (uv_stream_t*)&context->stop_pipe_write, buf, 1, NULL);      
}

static void
tarantool_module_destroy(void *ctx)
{
        struct module_context *context = (struct module_context *)(ctx);
        struct route *route, *tmp;
        LIST_FOREACH_SAFE(route, &context->route_list, entry, tmp)
                destroy_h2o_route(route);
}

static void 
alloc_buffer_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
        buf->base = (char*)malloc(suggested_size);
        buf->len = suggested_size;
}

static void 
uv_read_stop_pipe(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf)
{
        if (buf->base)
                free(buf->base);
}

static void
uv_read_wpipe(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) 
{
        if (nread < 0) {
                tarantool_module_stop(&context);
        } else if (nread > 0) {
                do {
                        ev_process_events(context.ev_loop, context.wpipe, EV_READ);
                } while (!ev_prepare_extern_loop_wait(context.ev_loop));
        }
        if (buf->base)
                free(buf->base);
}

static int
create_listener(void)
{
        struct sockaddr_in addr;
    
        uv_tcp_init(context.ctx.loop, &context.listener);
        uv_ip4_addr(context.addr, context.port, &addr);
        if (uv_tcp_bind(&context.listener, (struct sockaddr *)&addr, 0) != 0)
                goto error;
        if (uv_listen((uv_stream_t *)&context.listener, 128, on_accept) != 0)
                goto error;
        return 0;
error:
        uv_close((uv_handle_t *)&context.listener, NULL);
        return -1;
}

static void
on_accept(uv_stream_t *listener, int status)
{
    uv_tcp_t *conn;
    h2o_socket_t *sock;

    if (status != 0)
        return;

    conn = h2o_mem_alloc(sizeof(*conn));
    if (!conn)
        return;

    uv_tcp_init(listener->loop, conn);
    if (uv_accept(listener, (uv_stream_t *)conn) != 0) {
        uv_close((uv_handle_t *)conn, (uv_close_cb)free);
        return;
    }

    sock = h2o_uv_socket_create((uv_stream_t *)conn, (uv_close_cb)free);
    if (!sock) {
        uv_close((uv_handle_t *)conn, (uv_close_cb)free);
        return;
    }
    h2o_accept(&context.accept_ctx, sock);
}

static int 
h2o_on_req(h2o_handler_t *handler, h2o_req_t *req)
{
        parameter_handler *phandler = (parameter_handler *)handler;
        struct route *route = (struct route *)phandler->parameter;
        if (!h2o_memis(req->method.base, req->method.len, route->method, strlen(route->method)) ||
            !h2o_memis(req->path_normalized.base, req->path_normalized.len, route->path, strlen(route->path)))
                return -1;

        struct request *request = (struct request *)malloc(sizeof(struct request));
        if (!request)
                return -1;
        request->data = (void *)route;
        request->status = 0;
        tx_msg_send(context.module, request, tx_process_request, m_process_request);
        return 0;
}

static h2o_pathconf_t *register_h2o_handler(h2o_hostconf_t *hostconf, const struct route *route, 
        int (*on_req)(h2o_handler_t *, h2o_req_t *))
{
        h2o_pathconf_t *pathconf = h2o_config_register_path(hostconf, route->path, 0);
        parameter_handler *handler = (struct parameter_handler*)h2o_create_handler(pathconf, sizeof(*handler));
        handler->handler.on_req = on_req;
        handler->parameter = (void *)route;
        return pathconf;
}