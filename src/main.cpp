#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <uv.h>
#include "util.h"

#define DEFAULT_PORT        (8080)

static void* run_server(void* data);
static void* run_client(void* data);


static void server_on_read(uv_udp_t *handler, ssize_t nread, const uv_buf_t *buf, const struct sockaddr* addr, unsigned flags)
{
    if (nread == 0) {
        return ;
    }

    if (nread > 0) {
        server_context* context = (server_context*) handler->data;

#if KCP_WITH_FEC
        uint32_t conv = ikcp_getconv(buf->base + fecHeaderSizePlus2);
#else
        uint32_t conv = ikcp_getconv(buf->base);
#endif
        /** find conv */
        connection* conn = connection_find(context, conv);
        if (conn == NULL) {
            conn = connection_create(context, conv);
            memcpy(&conn->addr, addr, sizeof(struct sockaddr_in));
        }

        connnection_input(conn, buf->base, nread);

        free(buf->base);
    }
}


static void* run_server(void* data)
{
    server_context* context = (server_context*) data;
    uv_loop_t* loop = uv_loop_new();
    uv_udp_t* server = (uv_udp_t*) malloc(sizeof(uv_udp_t));
    struct sockaddr_in addr;
    int r = uv_udp_init(loop, server);
    if (r != 0) {
        printf("run_server uv_udp_init error ! \n");
    }
    server->data = context;
    loop->data = context;

    r = uv_ip4_addr("0.0.0.0", DEFAULT_PORT, &addr);
    if (r != 0) {
        printf("run_server uv_ip4_addr error ! \n");
    }
    r = uv_udp_bind(server, (const struct sockaddr*)&addr, 0);
    if (r != 0) {
        printf("run_server uv_udp_bind error ! \n");
    }


    uv_timer_t* timer = (uv_timer_t*) malloc(sizeof(*timer));
    r = uv_timer_init(loop, timer);
    if (r != 0) {
        printf("uv_timer_init error ! \n");
    }

    timer->data = context;
    r = uv_timer_start(timer, server_update_timer, KCP_UPDATE, KCP_UPDATE);
    if (r != 0) {
        printf("uv_timer_start error ! \n");
    }


    context->loop = loop;
    context->sock = server;
    context->update_timer = timer;

    r = uv_udp_recv_start(server, alloc_buffer, server_on_read);
    if (r != 0) {
        printf("uv_udp_recv_start error [%d]! \n", r);
    }

    set_dscp(server);

    uv_update_time(loop);
    uv_run(loop, UV_RUN_DEFAULT);
    return NULL;
}




static void client_on_read(uv_udp_t *handler, ssize_t nread, const uv_buf_t *buf, const struct sockaddr* addr, unsigned flags)
{
    if (nread == 0) {
        return ;
    }
    if (nread > 0) {
        client_context* context = (client_context*) handler->data;
        client_input(context, buf->base, nread);
    }
    free(buf->base);
}

static void* run_client(void* data)
{
    client_context* context = (client_context*) data;
    client_kcp_init(context);

    uv_loop_t* loop = uv_loop_new();
    uv_udp_t* client = (uv_udp_t *)malloc(sizeof(uv_udp_t));
    memset(client, 0, sizeof(uv_udp_t));

    struct sockaddr_in addr;
    int r = uv_udp_init(loop, client);
    if (r != 0) {
        printf("run_client uv_udp_init error ! \n");
    }

    r = uv_ip4_addr("127.0.0.1", DEFAULT_PORT, &addr);
    if (r != 0) {
        printf("run_client uv_ip4_addr error ! \n");
    }
    client->data = context;
    loop->data = context;

    context->update_timer = (uv_timer_t*) malloc(sizeof(uv_timer_t));
    uv_timer_init(loop, context->update_timer);
    context->update_timer->data = context;
    context->loop = loop;
    context->sock = client;
    context->addr = addr;
    context->recv_buf.base = (char*) malloc(MAX_DATA_LEN);
    context->recv_buf.len = MAX_DATA_LEN;

    r = uv_timer_start(context->update_timer, client_update_timer, KCP_UPDATE, KCP_UPDATE);
    if (r != 0) {
        printf("uv_timer_start error ! \n");
    }

    client_kcp_create(context);
    r = uv_udp_recv_start(client, alloc_buffer, client_on_read);
    if (r != 0) {
        printf("uv_udp_recv_start error [%d] \n", r);
    }

    set_dscp(client);

    for (int i = 0; i < 100; i++) {

        uint8_t h[1023] = {0};
        fill_rand_buffer(h, sizeof(h)/ sizeof(h[0]));

        context->input_data_size += sizeof(h);
        context->input_data_frg++;

        r = ikcp_send(context->kcp, (const char*) h, sizeof(h));
        if (r < 0) {
            printf("run_client ikcp_send error ! \n");
        }
    }


    uv_update_time(loop);
    uv_run(loop, UV_RUN_DEFAULT);
    return NULL;
}




int main()
{
    uint32_t  start_timestamp = iclock();
    srand(start_timestamp);

    s_context = (server_context*) malloc(sizeof(*s_context));
    c_context = (client_context*) malloc(sizeof(*c_context));


    pthread_t s, c;

    int r = 0;
    r = pthread_create(&s, NULL, run_server, s_context);
    if (r != 0) {
        printf("pthread_create failed! \n");
    }
    r = pthread_create(&c, NULL, run_client, c_context);
    if (r != 0) {
        printf("pthread_create failed ! \n");
    }

    pthread_join(c, NULL);
    pthread_join(s, NULL);
    return 0;
}



