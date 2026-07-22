/**
 * @file basic_example.c
 * @brief InduRTDB C11 basic usage example
 * @version 3.0.0
 * @date 2026-07-21
 * @copyright MIT License
 */

#include <indurtdb/indurtdb.h>
#include <stdio.h>
#include <unistd.h>

/* subscription callback - C function pointer */
static void on_pump_state_change(uint32_t id,
                                 const indurtdb_point_t* data,
                                 void* user_data)
{
    (void)id;
    (void)user_data;
    printf("   [callback] pump state changed: %s\n",
           data->value.b ? "ON" : "OFF");
}

int main(void)
{
    printf("=== InduRTDB Basic Example (C11) ===\n");

    /* 1. initialize */
    printf("1. init ... ");
    if (indurtdb_initialize("example_instance", 4096, 10) != 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("OK\n");

    /* 2. write 4 types */
    printf("2. write ... ");
    if (indurtdb_write_double(1001, 23.5) != 0) { printf("FAIL\n"); return 1; }
    if (indurtdb_write_double(1002, 65.0) != 0) { printf("FAIL\n"); return 1; }
    if (indurtdb_write_bool(2001, true) != 0)     { printf("FAIL\n"); return 1; }
    if (indurtdb_write_string(3001, "HVAC-01") != 0) { printf("FAIL\n"); return 1; }
    printf("OK\n");

    /* 3. read */
    printf("3. read ... ");
    {
        double temp;
        if (indurtdb_read_double(1001, &temp) == 0)
            printf("temp=%.1f ", temp);

        bool pump;
        if (indurtdb_read_bool(2001, &pump) == 0)
            printf("pump=%s ", pump ? "ON" : "OFF");

        char name[64];
        if (indurtdb_read_string(3001, name, sizeof(name)) == 0)
            printf("name=%s ", name);
    }
    printf("OK\n");

    /* 4. subscribe */
    printf("4. subscribe ... ");
    if (indurtdb_subscribe(2001, on_pump_state_change, NULL) != 0) {
        printf("FAIL\n"); return 1;
    }
    printf("OK\n");

    /* 5. mutate (write triggers callback) */
    printf("5. mutate ... ");
    for (int i = 0; i < 3; i++) {
        indurtdb_write_bool(2001, (i % 2 == 0));
        sleep(1);
    }
    printf("OK\n");

    /* 6. peek (zero-copy) */
    printf("6. peek ... ");
    {
        const indurtdb_point_t* p = indurtdb_peek(1001);
        if (p)
            printf("temp=%.1f ", p->value.d);
    }
    printf("OK\n");

    /* 7. heartbeat */
    printf("7. heartbeat ... ");
    indurtdb_update_heartbeat();
    printf("OK\n");

    /* 8. shutdown */
    printf("8. shutdown ... ");
    indurtdb_shutdown();
    printf("OK\n");

    printf("\n=== Example completed successfully ===\n");
    return 0;
}
