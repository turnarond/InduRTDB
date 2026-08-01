#include <indurtdb/indurtdb.h>
#include <stdio.h>
#include <unistd.h>

static void on_change(uint32_t id, const indurtdb_point_t* data, void* user_data) {
    (void)id; (void)user_data;
    printf("  [notify] point %u = %.1f\n", id, data->value.d);
}

int main(void) {
    printf("=== InduRTDB 外部库 Demo ===\n\n");

    /* 1. 初始化 */
    if (indurtdb_initialize("demo", 1024, 8) != 0) {
        printf("FAIL: init\n"); return 1;
    }
    printf("[OK] 初始化成功\n");

    /* 2. 写入 */
    indurtdb_write_double(1, 25.5);
    indurtdb_write_int32(2, 42);
    indurtdb_write_bool(3, true);
    indurtdb_write_string(4, "pump_running");
    printf("[OK] 写入 4 个点位\n");

    /* 3. 读取 */
    double temp; int32_t cnt; bool state; char name[64];
    indurtdb_read_double(1, &temp);
    indurtdb_read_int32(2, &cnt);
    indurtdb_read_bool(3, &state);
    indurtdb_read_string(4, name, sizeof(name));
    printf("[OK] 读取: temp=%.1f  cnt=%d  state=%s  name=%s\n",
           temp, cnt, state ? "ON" : "OFF", name);

    /* 4. 订阅 */
    indurtdb_subscribe(1, on_change, NULL);
    printf("[OK] 订阅 point 1\n");

    /* 5. 触发回调 */
    indurtdb_write_double(1, 26.0);
    sleep(1);

    /* 6. 零拷贝 peek */
    const indurtdb_point_t* p = indurtdb_peek(1);
    printf("[OK] peek point 1 = %.1f (零拷贝)\n", p->value.d);

    /* 7. 批量读 */
    indurtdb_point_t buf[2];
    int n = indurtdb_read_range(0, 2, buf, 2);
    printf("[OK] batch read %d points\n", n);

    /* 8. 统计 */
    printf("[OK] write_count = %lu\n", indurtdb_get_write_count());

    /* 9. 清理 */
    indurtdb_shutdown();
    printf("[OK] shutdown\n");

    printf("\n=== Demo 完成 ===\n");
    return 0;
}
