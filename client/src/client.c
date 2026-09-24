/**
 * @file client.c
 * @brief indurtdb-client —— T3 红阶段桩：声明与生命周期已就位，写路径待实现
 */
#include <irtcli/client.h>

#include <stdlib.h>
#include <string.h>

int irtcli_init(irtcli_t* c, const char* sock_path, uint32_t cap,
                irtcli_alert_fn alert, void* user_data)
{
    if (!c || !sock_path) return IRTCLI_ERR_ARG;

    memset(c, 0, sizeof(*c));
    strncpy(c->sock_path, sock_path, IRTCLI_SOCK_PATH_MAX - 1);
    c->cap        = (cap == 0) ? IRTCLI_QUEUE_CAP_DEFAULT : cap;
    c->async      = true;
    c->alert      = alert;
    c->user_data  = user_data;
    c->fd         = -1;
    c->connected  = false;
    c->queue      = (irtcli_entry_t*)calloc(c->cap, sizeof(irtcli_entry_t));
    if (!c->queue) return IRTCLI_ERR_IO;
    return IRTCLI_OK;
}

void irtcli_close(irtcli_t* c)
{
    if (!c) return;
    free(c->queue);
    c->queue = NULL;
    memset(c, 0, sizeof(*c));
}

int irtcli_connect(irtcli_t* c)
{
    (void)c;
    return IRTCLI_ERR_IO; /* 待实现 */
}

void irtcli_set_async(irtcli_t* c, bool async)
{
    if (c) c->async = async;
}

int irtcli_write_bool(irtcli_t* c, uint32_t id, bool value, uint64_t source_ts_ns)
{
    (void)c; (void)id; (void)value; (void)source_ts_ns;
    return IRTCLI_ERR_IO; /* 待实现 */
}
int irtcli_write_int32(irtcli_t* c, uint32_t id, int32_t value, uint64_t source_ts_ns)
{
    (void)c; (void)id; (void)value; (void)source_ts_ns;
    return IRTCLI_ERR_IO; /* 待实现 */
}
int irtcli_write_double(irtcli_t* c, uint32_t id, double value, uint64_t source_ts_ns)
{
    (void)c; (void)id; (void)value; (void)source_ts_ns;
    return IRTCLI_ERR_IO; /* 待实现 */
}

int irtcli_flush(irtcli_t* c)
{
    (void)c;
    return 0; /* 待实现 */
}

uint32_t irtcli_queue_count(const irtcli_t* c)
{
    return c ? c->count : 0u;
}
