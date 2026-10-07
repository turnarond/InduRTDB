/**
 * @file demo.c
 * @brief InduRTDB 外部消费 Demo（经 pkg-config 消费已安装库）
 *
 * v3.7 (主题B B5)：迁移到 **v2 句柄 API** (`indurtdb_h_*`)。
 * v1 全局 `indurtdb_*` 函数自 v3.7 起标注弃用（仍可用，v4.0 移除）。
 */

#include <indurtdb/indurtdb.h>
#include <stdio.h>
#include <unistd.h>

static void on_change(uint32_t id, const indurtdb_point_t* data, void* user_data) {
    (void)id; (void)user_data;
    printf("  [notify] point %u = %.1f\n", id, data->value.d);
}

int main(void) {
    printf("=== InduRTDB 外部库 Demo (API v2 句柄) ===\n\n");

    indurtdb_t* db = NULL;
    indurtdb_cfg_t cfg = { .max_points = 1024, .max_subscribers = 8 };

    /* 1. 打开实例 */
    if (indurtdb_h_open(&db, "demo", &cfg) != 0) {
        printf("FAIL: open: %s\n", indurtdb_get_last_error());
        return 1;
    }
    printf("[OK] 打开实例成功\n");

    /* 2. 写入 */
    indurtdb_h_write_double(db, 1, 25.5);
    indurtdb_h_write_int32(db, 2, 42);
    indurtdb_h_write_bool(db, 3, true);
    indurtdb_h_write_string(db, 4, "pump_running");
    printf("[OK] 写入 4 个点位\n");

    /* 3. 读取 */
    double temp; int32_t cnt; bool state; char name[64];
    indurtdb_h_read_double(db, 1, &temp);
    indurtdb_h_read_int32(db, 2, &cnt);
    indurtdb_h_read_bool(db, 3, &state);
    indurtdb_h_read_string(db, 4, name, sizeof(name));
    printf("[OK] 读取: temp=%.1f  cnt=%d  state=%s  name=%s\n",
           temp, cnt, state ? "ON" : "OFF", name);

    /* 4. 订阅 */
    indurtdb_h_subscribe(db, 1, on_change, NULL);
    printf("[OK] 订阅 point 1\n");

    /* 5. 触发回调 */
    indurtdb_h_write_double(db, 1, 26.0);
    sleep(1);

    /* 6. 单拷贝 peek (线程本地缓冲, 下次 peek 覆盖) */
    const indurtdb_point_t* p = indurtdb_h_peek(db, 1);
    printf("[OK] peek point 1 = %.1f (单拷贝)\n", p ? p->value.d : 0.0);

    /* 7. 批量读 */
    indurtdb_point_t buf[2];
    int n = indurtdb_h_read_range(db, 0, 2, buf, 2);
    printf("[OK] batch read %d points\n", n);

    /* 8. 统计 */
    printf("[OK] write_count = %lu\n", (unsigned long)indurtdb_h_get_write_count(db));

    /* 9. 清理 */
    indurtdb_h_close(db);
    printf("[OK] close\n");

    printf("\n=== Demo 完成 ===\n");
    return 0;
}
