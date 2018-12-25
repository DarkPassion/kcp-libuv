#ifndef _UTIL_H_
#define _UTIL_H_


#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
#include <uv.h>
#include "ikcp.h"
#include "fec.h"


#define KCP_SEND_WND    (128)
#define KCP_RECV_WND    (128)
#define KCP_NO_DELAY    (1)
#define KCP_INTERVAL    (20)
#define KCP_RESEND      (2)
#define KCP_NC          (1)
#define KCP_MTU         (1300)
#define KCP_UPDATE      (30)

#define MAX_KCP_CONN    (1024)
#define MAX_DATA_LEN    (4*1024)

#define KCP_WITH_FEC    1


struct connection_s;
typedef struct connection_s connection;
typedef struct {
    uv_loop_t* loop;
    uv_timer_t* update_timer;
    uv_udp_t*   sock;

    connection* conns[MAX_KCP_CONN];
    uint32_t conn_count;
} server_context;


struct connection_s {
    uint32_t            conv;
    ikcpcb*             kcp;
    uv_buf_t            recv_buf;
    struct sockaddr_in  addr;

    /** fec */
    FEC                 fec;
    uint32_t            pkt_idx;
    std::vector<row_type> shards;
    int                 dataShards;
    int                 parityShards;
    byte                m_buf[MAX_DATA_LEN];


    server_context*     context;
};


typedef struct {
    uv_loop_t*  loop;
    uv_udp_t*   sock;
    uv_buf_t    recv_buf;
    struct sockaddr_in addr;
    uv_timer_t* update_timer;
    ikcpcb*     kcp;

    uint32_t    input_data_size;
    uint32_t    input_data_frg;
    uint32_t    output_data_size;
    uint32_t    output_data_frg;

    FEC         fec;
    uint32_t    pkt_idx;
    std::vector<row_type> shards;
    int         dataShards;
    int         parityShards;

    byte        m_buf[MAX_DATA_LEN];

} client_context;

typedef struct {
    uv_udp_send_t   req;
    uv_buf_t        buf;
} write_req_t;


extern server_context* s_context;
extern client_context* c_context;


#ifdef __cplusplus
extern "C"
{
#endif

/* get clock in millisecond 64 */
IINT64 iclock64(void);
IUINT32 iclock(void);


void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);

void free_write_req(void* req);

void on_close(uv_handle_t* handle);

void on_write(uv_udp_send_t *req, int status);

uint32_t gen_conv_id(uint32_t room_id, uint8_t line_index);

void fill_rand_buffer(uint8_t* buff, int len);

void set_dscp(uv_udp_t* handler);

void client_kcp_init(client_context* context);
void client_kcp_create(client_context* context);
int client_kcp_output_wrapper(const char *buf, int len, struct IKCPCB *kcp, void *user);
int client_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user);

void client_update_timer(uv_timer_t* handler);
void client_input(client_context* context, char* data, int len);


int server_kcp_output_wrapper(const char* buf, int len, struct IKCPCB *kcp, void *user);
int server_kcp_output(const char* buf, int len, struct IKCPCB *kcp, void *user);
void server_update_timer(uv_timer_t* handler);

connection* connection_find(server_context* context, uint32_t conv);

connection* connection_create(server_context* context, uint32_t conv);

void connnection_input(connection* conn, char* data, int len);


#ifdef __cplusplus
}
#endif

#endif
