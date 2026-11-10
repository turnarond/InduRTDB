#include "rtdbd_defer_global.h"

static rtdbd_defer_queue_t g_defer_global;

rtdbd_defer_queue_t *rtdbd_defer_global(void)
{
    return &g_defer_global;
}
