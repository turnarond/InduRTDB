/**
 * @file main.c
 * @brief InduRTDB starter —— 最小读写示例
 * @version 3.1.0
 */
#include <indurtdb/indurtdb.h>
#include <stdio.h>

int main(void) {
    if (indurtdb_initialize("starter", 4096, 8) != 0) {
        fprintf(stderr, "init failed: %s\n", indurtdb_get_last_error());
        return 1;
    }

    indurtdb_write_double(1001, 23.5);
    indurtdb_write_bool(2001, true);

    double t = 0.0;
    indurtdb_read_double(1001, &t);
    bool on = false;
    indurtdb_read_bool(2001, &on);

    indurtdb_shutdown();
    printf("starter ok: temp=%.1f, on=%d\n", t, (int)on);
    return (t == 23.5 && on) ? 0 : 2;
}
