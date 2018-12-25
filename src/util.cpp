


#include "util.h"
#include "encoding.h"
server_context* s_context = NULL;
client_context* c_context = NULL;


static const int dataShards = 6;
static const int parityShards = 2;


/* get clock in millisecond 64 */
IINT64 iclock64(void)
{
    struct timeval time;
    gettimeofday(&time, NULL);
    return time.tv_sec * 1000 + time.tv_usec / 1000;
}

IUINT32 iclock(void)
{
    return (IUINT32)(iclock64() & 0xfffffffful);
}


void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = (char*) malloc(suggested_size);
    buf->len = suggested_size;
}

void free_write_req(void* req) {
    write_req_t *wr = (write_req_t*) req;
    free(wr->buf.base);
    free(wr);
}

void on_close(uv_handle_t* handle) {
    free(handle);
}



void on_write(uv_udp_send_t *req, int status) {
    if (status) {
        fprintf(stderr, "Write error %s\n", uv_strerror(status));
    }

    free_write_req(req);
}

void set_dscp(uv_udp_t* handler) {
    int iptos = 46;
    iptos = (iptos << 2) & 0xFF;
    int fd = handler->io_watcher.fd;
    setsockopt(fd, IPPROTO_IP, IP_TOS, &iptos, sizeof(iptos));
}



uint32_t gen_conv_id(uint32_t room_id, uint8_t line_index) {
    uint32_t rand = 0;
    struct timeval val;
    gettimeofday(&val, NULL);
    uint64_t cts = val.tv_sec * 1000 + val.tv_usec / 1000;
    uint32_t cts32 = cts & 0xffffffff;

    rand = ((cts32 & 0x3FFFF) << 14) |  (line_index & 0xf) << 10 | (room_id & 0x3f);
    return rand;
}

void fill_rand_buffer(uint8_t* buff, int len) {

    for (int i = 0; i < len; i++) {
        buff[i] = (uint8_t) (rand() % 255);
    }
}


int client_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user)
{
    client_context* context = (client_context*) user;

    context->output_data_frg++;
    context->output_data_size += len;

    write_req_t* req = (write_req_t*) malloc(sizeof(write_req_t));
    req->buf.base = (char*) malloc(len * sizeof(char));
    req->buf.len = len;
    memcpy(req->buf.base, buf, len);
    int r = uv_udp_send((uv_udp_send_t*) req, context->sock, &req->buf, 1, (const struct sockaddr *)&context->addr, on_write);
    if (r != 0) {
        printf("client_kcp_output uv_udp_send error ! \n");
    }
    return 0;
}


int client_kcp_output_wrapper(const char *buf, int len, struct IKCPCB *kcp, void *user)
{
    client_context* context = (client_context*) user;

#if KCP_WITH_FEC
    // extend to len + fecHeaderSizePlus2
    // i.e. 4B seqid + 2B flag + 2B size
    memcpy(context->m_buf + fecHeaderSizePlus2, buf, static_cast<size_t>(len));
    context->fec.MarkData(context->m_buf, static_cast<uint16_t>(len));
    client_kcp_output((const char *)context->m_buf, len + fecHeaderSizePlus2, kcp, context);

    // FEC calculation
    // copy "2B size + data" to shards
    auto slen = len + 2;
    context->shards[context->pkt_idx] =
            std::make_shared<std::vector<byte>>(&context->m_buf[fecHeaderSize], &context->m_buf[fecHeaderSize + slen]);

    // count number of data shards
    context->pkt_idx++;
    if (context->pkt_idx == context->dataShards) { // we've collected enough data shards
        context->fec.Encode(context->shards);
        // send parity shards
        for (size_t i = context->dataShards; i < context->dataShards + context->parityShards; i++) {
            // append header to parity shards
            // i.e. fecHeaderSize + data(2B size included)
            memcpy(context->m_buf + fecHeaderSize, context->shards[i]->data(), context->shards[i]->size());
            context->fec.MarkFEC(context->m_buf);
            client_kcp_output((const char *)context->m_buf, context->shards[i]->size() + fecHeaderSize, kcp, context);
        }

        // reset indexing
        context->pkt_idx = 0;
    }
#else
    client_kcp_output(buf, len, kcp, user);
#endif
    return 0;
}

void client_update_timer(uv_timer_t* handler)
{
    client_context* context = (client_context*) handler->data;
    ikcp_update(context->kcp, iclock());
}


void client_input(client_context* context, char* data, int len)
{

#if KCP_WITH_FEC

    // decode FEC packet
    auto pkt = context->fec.Decode((byte*)data, static_cast<size_t>(len));
    if (pkt.flag == typeData) {
        auto ptr = pkt.data->data();
        // we have 2B size, ignore for typeData
        ikcp_input(context->kcp, (char *) (ptr + 2), pkt.data->size() - 2);
    }

    // allow FEC packet processing with correct flags.
    if (pkt.flag == typeData || pkt.flag == typeFEC) {
        // input to FEC, and see if we can recover data.
        auto recovered = context->fec.Input(pkt);

        // we have some data recovered.
        for (auto &r : recovered) {
            // recovered data has at least 2B size.
            if (r->size() > 2) {
                auto ptr = r->data();
                // decode packet size, which is also recovered.
                uint16_t sz;
                decode16u(ptr, &sz);

                // the recovered packet size must be in the correct range.
                if (sz >= 2 && sz <= r->size()) {
                    // input proper data to kcp
                    ikcp_input(context->kcp, (char *) (ptr + 2), sz - 2);
                    // std::cout << "sz:" << sz << std::endl;
                }
            }
        }
    }

    int r = 0;
    while (1) {
        r = ikcp_recv(context->kcp, context->recv_buf.base, context->recv_buf.len);
        if (r < 0) {
            break;
        }

        printf("client_input -- data [%s] len [%d] \n", context->recv_buf.base, r);

        r = ikcp_send(context->kcp, context->recv_buf.base, r);
        if (r < 0) {
            printf("client_input - ikcp_send error \n");
        }

        context->recv_buf.base[0] = '\0';
    }

#else
    int r = ikcp_input(context->kcp, data, len);
    if (r < 0) {
        printf("client_input ikcp_input error ! \n");
    }

    while (1) {

        r = ikcp_recv(context->kcp, context->recv_buf.base, context->recv_buf.len);
        if (r < 0) {
            break;
        }

        printf("client_input -- data [%s] len [%d] \n", context->recv_buf.base, r);

        r = ikcp_send(context->kcp, context->recv_buf.base, r);
        if (r < 0) {
            printf("client_input - ikcp_send error \n");
        }

        context->recv_buf.base[0] = '\0';
    }
#endif
}


void client_kcp_init(client_context* context)
{
    if (context == NULL) {
        return ;
    }
    context->loop = NULL;
    context->sock = NULL;
    context->recv_buf.base = NULL;
    context->recv_buf.len = 0;
    context->update_timer = NULL;
    context->kcp = NULL;
    context->pkt_idx = 0;
    context->dataShards = 0;
    context->parityShards = 0;

    /** stat data*/
    context->input_data_size = 0;
    context->input_data_frg = 0;
    context->output_data_size = 0;
    context->output_data_frg = 0;

    memset(context->m_buf, 0, sizeof(context->m_buf));
    memset(&context->addr, 0, sizeof(context->addr));
}

void client_kcp_create(client_context* context)
{
    uint32_t conv = gen_conv_id(100, 1);
    context->kcp = ikcp_create(conv, context);
    context->kcp->output = client_kcp_output_wrapper;
    ikcp_wndsize(context->kcp, KCP_SEND_WND, KCP_RECV_WND);
    ikcp_nodelay(context->kcp, KCP_NO_DELAY, KCP_INTERVAL, KCP_RESEND, KCP_NC);
    ikcp_setmtu(context->kcp, KCP_MTU);

#if KCP_WITH_FEC
    context->fec = FEC::New(3 * (dataShards + parityShards), dataShards, parityShards);
    context->shards.resize(dataShards + parityShards, NULL);
    context->dataShards = dataShards;
    context->parityShards = parityShards;
    context->pkt_idx = 0;
#endif
}

int server_kcp_output_wrapper(const char* buf, int len, struct IKCPCB *kcp, void *user)
{
#if KCP_WITH_FEC
    connection* conn = (connection*) user;


    // extend to len + fecHeaderSizePlus2
    // i.e. 4B seqid + 2B flag + 2B size
    memcpy(conn->m_buf + fecHeaderSizePlus2, buf, static_cast<size_t>(len));
    conn->fec.MarkData(conn->m_buf, static_cast<uint16_t>(len));
    server_kcp_output((const char *)conn->m_buf, len + fecHeaderSizePlus2, kcp, conn);

    // FEC calculation
    // copy "2B size + data" to shards
    auto slen = len + 2;
    conn->shards[conn->pkt_idx] =
            std::make_shared<std::vector<byte>>(&conn->m_buf[fecHeaderSize], &conn->m_buf[fecHeaderSize + slen]);

    // count number of data shards
    conn->pkt_idx++;
    if (conn->pkt_idx == conn->dataShards) { // we've collected enough data shards
        conn->fec.Encode(conn->shards);
        // send parity shards
        for (size_t i = conn->dataShards; i < conn->dataShards + conn->parityShards; i++) {
            // append header to parity shards
            // i.e. fecHeaderSize + data(2B size included)
            memcpy(conn->m_buf + fecHeaderSize, conn->shards[i]->data(), conn->shards[i]->size());
            conn->fec.MarkFEC(conn->m_buf);
            server_kcp_output((const char *)conn->m_buf, conn->shards[i]->size() + fecHeaderSize, kcp, conn);
        }

        // reset indexing
        conn->pkt_idx = 0;
    }

#else
    server_kcp_output(buf, len, kcp, user);
#endif
    return 0;
}

int server_kcp_output(const char *buf, int len, struct IKCPCB *kcp, void *user)
{

    connection* conn = (connection*) user;

    write_req_t* req = (write_req_t*) malloc(sizeof(write_req_t));
    req->buf.base = (char*) malloc(len * sizeof(char));
    req->buf.len = len;
    memcpy(req->buf.base, buf, len);
    int r = uv_udp_send((uv_udp_send_t*) req, conn->context->sock, &req->buf, 1, (const struct sockaddr *)&conn->addr, on_write);
    if (r != 0) {
        printf("server_kcp_output uv_udp_send error ! \n");
    }

    return 0;
}

void server_update_timer(uv_timer_t* handler)
{
    server_context* context = (server_context*) handler->data;
    if (context) {
        for (int i = 0; i < context->conn_count; i++) {
            if (context->conns[i]) {
                connection* conn =  context->conns[i];
                ikcp_update(conn->kcp, iclock());
            }
        }
    }
}


connection* connection_find(server_context* context, uint32_t conv)
{
    if (context == NULL || conv == 0) {
        return NULL;
    }

    connection* conn = NULL;
    if (context->conn_count > 0) {
        for (int i = 0; i < context->conn_count; ++i) {
            if (context->conns[i] && context->conns[i]->conv == conv) {
                conn = context->conns[i];
                break;
            }
        }
    }
    return conn;
}


connection* connection_create(server_context* context, uint32_t conv)
{
    connection* conn = (connection*) malloc(sizeof(connection));

    conn->recv_buf.len = 0;
    conn->recv_buf.base = NULL;
    conn->kcp = NULL;
    conn->dataShards = 0;
    conn->parityShards = 0;
    conn->pkt_idx = 0;

    conn->conv = conv;
    conn->context = context;
    conn->kcp = ikcp_create(conv, conn);
    conn->kcp->output = server_kcp_output_wrapper;
    ikcp_wndsize(conn->kcp, KCP_SEND_WND, KCP_RECV_WND);
    ikcp_nodelay(conn->kcp, KCP_NO_DELAY, KCP_INTERVAL, KCP_RESEND, KCP_NC);
    ikcp_setmtu(conn->kcp, KCP_MTU);

    conn->recv_buf.base = (char*) malloc(MAX_DATA_LEN);
    conn->recv_buf.len = MAX_DATA_LEN;

#if KCP_WITH_FEC
    conn->fec = FEC::New(3 * (dataShards + parityShards), dataShards, parityShards);
    conn->shards.resize(dataShards + parityShards, NULL);
    conn->dataShards = dataShards;
    conn->parityShards = parityShards;
    conn->pkt_idx = 0;
#endif



    for (int i = context->conn_count; i < MAX_KCP_CONN; i++) {
        if (context->conns[i] == NULL) {
            context->conns[i] = conn;
            context->conn_count++;
            break;
        }
    }

    return conn;
}


void connnection_input(connection* conn, char* data, int len)
{

#if KCP_WITH_FEC

    // decode FEC packet
    auto pkt = conn->fec.Decode((byte*)data, static_cast<size_t>(len));
    if (pkt.flag == typeData) {
        auto ptr = pkt.data->data();
        // we have 2B size, ignore for typeData
        ikcp_input(conn->kcp, (char *) (ptr + 2), pkt.data->size() - 2);
    }

    // allow FEC packet processing with correct flags.
    if (pkt.flag == typeData || pkt.flag == typeFEC) {
        // input to FEC, and see if we can recover data.
        auto recovered = conn->fec.Input(pkt);

        // we have some data recovered.
        for (auto &r : recovered) {
            // recovered data has at least 2B size.
            if (r->size() > 2) {
                auto ptr = r->data();
                // decode packet size, which is also recovered.
                uint16_t sz;
                decode16u(ptr, &sz);

                // the recovered packet size must be in the correct range.
                if (sz >= 2 && sz <= r->size()) {
                    // input proper data to kcp
                    ikcp_input(conn->kcp, (char *) (ptr + 2), sz - 2);
                    // std::cout << "sz:" << sz << std::endl;
                }
            }
        }
    }

    int r = 0;
    while (1) {

        r = ikcp_recv(conn->kcp, conn->recv_buf.base, conn->recv_buf.len);
        if (r < 0) {
            break;
        }

        printf("connnection_input -- data [%s] len [%d] \n", conn->recv_buf.base, r);

    }


#else

    int r = ikcp_input(conn->kcp, data, len);
    if (r < 0) {
        printf("connnection_input ikcp_input error ! \n");
    }

    while (1) {

        r = ikcp_recv(conn->kcp, conn->recv_buf.base, conn->recv_buf.len);
        if (r < 0) {
            break;
        }
        printf("connnection_input -- data [%s] len [%d] \n", conn->recv_buf.base, r);

    }


#endif
}











