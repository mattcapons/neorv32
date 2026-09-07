// ================================================================================ //
// The NEORV32 RISC-V Processor - https://github.com/stnolting/neorv32              //
// Copyright (c) NEORV32 contributors.                                              //
// Copyright (c) 2020 - 2025 Stephan Nolting. All rights reserved.                  //
// Licensed under the BSD-3-Clause license, see LICENSE for details.                //
// SPDX-License-Identifier: BSD-3-Clause                                            //
// ================================================================================ //


/**********************************************************************//**
 * @file my_accelerator_test/main.c
 * @author Matteo Capone
 * @test program for my custom AI accelerator
 **************************************************************************/

#include <neorv32.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>


/**********************************************************************//**
 * @name User configuration
 **************************************************************************/
/**@{*/
/** UART BAUD rate */
#define BAUD_RATE 19200
/**@}*/


// ACCELERATOR CONSTRAINTS
#define TILE_SIZE 4
#define MAX_MAT_SIZE 256


typedef struct {
    int32_t *result;
    uint32_t mat_size;
    uint32_t acc_num;
    uint8_t has_padding;

    volatile uint32_t rx_count;

    uint32_t tile_row;
    uint32_t tile_col;
    uint32_t local_row;
    uint32_t local_col;
    uint32_t tile_row_base;
    uint32_t row_base;

    uint32_t expected_results;
} acc_context_t;

static acc_context_t acc_ctx;

static volatile uint64_t irq_cycles_total = 0;
static volatile uint32_t irq_count = 0;
static volatile uint64_t send_cycles  = 0;
static volatile uint64_t wait_cycles  = 0;
static volatile uint64_t pack_cycles = 0;

// ACCELERATOR COMMUNICATION THROUGH CFS
static inline int acc_ready(void) {
    return (NEORV32_CFS->REG[2] & 1u) != 0;
}

static inline void acc_set_num(uint32_t acc_num) {
    NEORV32_CFS->REG[1] = acc_num;
}

static inline void acc_set_start(int start) {
    NEORV32_CFS->REG[0] = start ? 1u : 0u;
}

static uint32_t pack_a_word(
    const int8_t *a,
    uint32_t mat_size,
    uint32_t tile_row,
    uint32_t tile_col,
    uint32_t local_col
) {
    uint32_t packed = 0;

    for (uint32_t local_row = 0; local_row < TILE_SIZE; local_row++) {
        uint32_t row = tile_row * TILE_SIZE + local_row;
        uint32_t col = tile_col * TILE_SIZE + local_col;

        uint8_t value = 0;

        if ((row < mat_size) && (col < mat_size)) {
            value = (uint8_t)a[row * mat_size + col];
        }

        packed |= ((uint32_t)value) << (24 - 8 * local_row);
    }

    return packed;
}

static uint32_t pack_w_word(
    const int8_t *w,
    uint32_t mat_size,
    uint32_t tile_row,
    uint32_t tile_col,
    uint32_t local_row
) {
    uint32_t packed = 0;

    for (uint32_t local_col = 0; local_col < TILE_SIZE; local_col++) {
        uint32_t row = tile_row * TILE_SIZE + local_row;
        uint32_t col = tile_col * TILE_SIZE + local_col;

        uint8_t value = 0;

        if ((row < mat_size) && (col < mat_size)) {
            value = (uint8_t)w[row * mat_size + col];
        }

        packed |= ((uint32_t)value) << (24 - 8 * local_col);
    }

    return packed;
}

static void send_packed_tile(const uint32_t *pack) {
    for (uint32_t i = 0; i < TILE_SIZE; i++) {
        while (neorv32_slink_tx_full()) {
        }

        neorv32_slink_put(pack[i]);
    }
}

static void acc_matmul(int8_t *a, int8_t *w, int32_t *c, uint32_t mat_size) {
    if ((mat_size <= 0) || (mat_size > MAX_MAT_SIZE)) {
        return;
    }

    // checks if the accelerator is available
    while (!acc_ready()) {
    }

    // sets context variable for the interrupt
    uint32_t acc_num = (mat_size + TILE_SIZE - 1) / TILE_SIZE;

    acc_ctx.result = c;
    acc_ctx.mat_size = mat_size;
    acc_ctx.acc_num = acc_num;
    acc_ctx.has_padding = (mat_size % TILE_SIZE) !=0 ;
    acc_ctx.rx_count = 0;
    acc_ctx.tile_row  = 0;
    acc_ctx.tile_col  = 0;
    acc_ctx.local_row = 0;
    acc_ctx.local_col = 0;
    acc_ctx.tile_row_base = 0;
    acc_ctx.row_base = 0;
    acc_ctx.expected_results = acc_num * acc_num * 16;

    // sets number of accumulations and start
    acc_set_num(acc_num);
    acc_set_start(1);

    // after the accelerator starts reset start
    while (acc_ready()) {
    }
    acc_set_start(0);


    //uint64_t pack_start = neorv32_cpu_get_cycle();

    uint32_t num_tiles = acc_num * acc_num;

    uint32_t packed_a[num_tiles][TILE_SIZE];
    uint32_t packed_w[num_tiles][TILE_SIZE];

    for (uint32_t i = 0; i < acc_num; i++) {
        for (uint32_t j = 0; j < acc_num; j++) {
            for (uint32_t word = 0; word < TILE_SIZE; word++){
                packed_a[i*acc_num + j][word] = pack_a_word(a, mat_size, i, j, word);
                packed_w[i*acc_num + j][word] = pack_w_word(w, mat_size, i, j, word);
            }
        }
    }

    //pack_cycles = neorv32_cpu_get_cycle() - pack_start;

    //uint64_t send_start = neorv32_cpu_get_cycle();

    for (uint32_t i = 0; i < acc_num; i++) {
        for (uint32_t j = 0; j < acc_num; j++) {
            for (uint32_t k = 0; k < acc_num; k++){
                send_packed_tile(packed_a[i*acc_num + k]);
                send_packed_tile(packed_w[k*acc_num + j]);
            }
        }
    }

    //send_cycles = neorv32_cpu_get_cycle() - send_start;

    //uint64_t wait_start = neorv32_cpu_get_cycle();

    while (acc_ctx.rx_count < acc_ctx.expected_results){}
    while (!acc_ready()) {}

    //wait_cycles += neorv32_cpu_get_cycle() - wait_start;
}

static void slink_irq_handler(void) {
    //uint64_t start = neorv32_cpu_get_cycle();

    while (!neorv32_slink_rx_empty()) {

        uint32_t raw = neorv32_slink_get();

        uint32_t row =
            acc_ctx.tile_row * TILE_SIZE + acc_ctx.local_row;

        uint32_t col =
            acc_ctx.tile_col * TILE_SIZE + acc_ctx.local_col;

        if ((row < acc_ctx.mat_size) && (col < acc_ctx.mat_size)) {
            acc_ctx.result[acc_ctx.row_base + col] = (int32_t)raw;
        }

        acc_ctx.rx_count++;
        acc_ctx.local_col++;

        if (acc_ctx.local_col == TILE_SIZE) {
            acc_ctx.local_col = 0;
            acc_ctx.local_row++;

            acc_ctx.row_base += acc_ctx.mat_size;

            if (acc_ctx.local_row == TILE_SIZE) {
                acc_ctx.local_row = 0;
                acc_ctx.tile_col++;

                if (acc_ctx.tile_col == acc_ctx.acc_num) {
                    acc_ctx.tile_col = 0;
                    acc_ctx.tile_row++;

                    acc_ctx.tile_row_base += TILE_SIZE * acc_ctx.mat_size;
                }

                acc_ctx.row_base = acc_ctx.tile_row_base;
            }
        }
    }

    //irq_cycles_total += neorv32_cpu_get_cycle() - start;
    irq_count++;
}

static void normal_matmul(int8_t *a, int8_t *w, int32_t *c, uint32_t mat_size) {
        for (uint32_t i = 0; i < mat_size; i++) {
            for (uint32_t j = 0; j < mat_size; j++) {
                int32_t sum = 0;
                for (uint32_t k = 0; k < mat_size; k++) {
                    sum += a[i*mat_size + k] * w[k*mat_size + j];
                }
                c[i*mat_size + j] = sum;
            }
        }
}

static void print_mat(int32_t *c, uint32_t mat_size) {
    for (uint32_t i = 0; i < mat_size; i++) {
        for (uint32_t j = 0; j < mat_size; j++) {
            neorv32_uart0_printf("%d\t", c[i*mat_size + j]);
        }
        neorv32_uart0_puts("\n");
    }
}

static void test_normal(int8_t *a, int8_t *w, int32_t *c, uint32_t mat_size) {
    uint64_t clock_start, clock_end, total;

    clock_start = neorv32_cpu_get_cycle();
    normal_matmul(a, w, c, mat_size);
    clock_end = neorv32_cpu_get_cycle();

    total = clock_end - clock_start;
    neorv32_uart0_puts("CPU:\n");
    //print_mat(c, mat_size);
    neorv32_uart0_printf("Clock count: %d\n", total);
}

static void test_acc(int8_t *a, int8_t *w, int32_t *c, uint32_t mat_size) {
    uint64_t clock_start, clock_end, total;

    clock_start = neorv32_cpu_get_cycle();
    acc_matmul(a, w, c, mat_size);
    clock_end = neorv32_cpu_get_cycle();

    total = clock_end - clock_start;
    neorv32_uart0_puts("ACC:\n");
    //print_mat(c, mat_size);
    neorv32_uart0_printf("Clock count: %d\n", total);
}

static void test_normal_average(int8_t *a, int8_t *w, int32_t *c, uint32_t mat_size) {
    uint64_t clock_start, clock_end, total = 0;
    for (int i = 0; i < 10; i++) {


        clock_start = neorv32_cpu_get_cycle();
        normal_matmul(a, w, c, mat_size);
        clock_end = neorv32_cpu_get_cycle();

        total += clock_end - clock_start;
    }
    uint64_t average = total/10;
    neorv32_uart0_puts("CPU:\n");
    //print_mat(c, mat_size);
    neorv32_uart0_printf("Average clock count: %d\n", average);
}

static void test_acc_average(int8_t *a, int8_t *w, int32_t *c, uint32_t mat_size) {
    uint64_t clock_start, clock_end, total = 0;
    for (int i = 0; i < 10; i++) {

        clock_start = neorv32_cpu_get_cycle();
        acc_matmul(a, w, c, mat_size);
        clock_end = neorv32_cpu_get_cycle();

        total += clock_end - clock_start;
    }
    uint64_t average = total/10;
    neorv32_uart0_puts("ACC:\n");
    //print_mat(c, mat_size);
    neorv32_uart0_printf("Average clock count: %d\n", average);
}


void check_matrices(const int32_t *a, const int32_t *b, uint32_t size) {
    uint32_t elements = size * size;

    for (uint32_t i = 0; i < elements; i++) {
        if (a[i] != b[i]) {
            neorv32_uart0_printf("Matrix comparison failed at index %u: %ld != %ld\n",
                   i, (long)a[i], (long)b[i]);
            return;
        }
    }

    neorv32_uart0_printf("Matrix comparison successful: all elements match.\n");
}


static void make_test_matrix(int8_t *a, uint32_t size) {
    for (uint32_t i = 0; i < size * size; i++) {
        a[i] = (int8_t)((i % 20) - 10);
    }
}

static void make_identity(int8_t *w, uint32_t size) {
    for (uint32_t row = 0; row < size; row++) {
        for (uint32_t col = 0; col < size; col++) {
            w[row * size + col] = (row == col) ? 1 : 0;
        }
    }
}




/**********************************************************************//**
* Main function; prints some fancy stuff via UART.
*
* @note This program requires the UART interface to be synthesized.
*
* @return 0 if execution was successful
**************************************************************************/
int main() {

    // capture all exceptions and give debug info via UART
    // this is not required, but keeps us safe
    neorv32_rte_setup();
    // setup UART at default baud rate, no interrupts
    neorv32_uart0_setup(BAUD_RATE, 0);


    /* Check CFS is actually present */
    if (neorv32_cfs_available() == 0)
    {
    neorv32_uart0_puts("[ERROR] CFS not synthesized. Aborting.\n");
    return -1;
    }

    /* Check SLINK is actually present */
    if (neorv32_slink_available() == 0)
    {
    neorv32_uart0_puts("[ERROR] SLINK not synthesized. Aborting.\n");
    return -1;
    }

    // setup slink interrupt
    neorv32_rte_handler_install(SLINK_TRAP_CODE, slink_irq_handler);
    neorv32_slink_setup(1u << SLINK_CTRL_IRQ_RX_NEMPTY);
    neorv32_cpu_csr_set(CSR_MIE, 1u << SLINK_FIRQ_ENABLE);
    neorv32_cpu_csr_set(CSR_MSTATUS, 1u << CSR_MSTATUS_MIE);

    #define N 8

    int8_t a[N * N];
    int8_t w[N * N];
    int32_t cpu_result[N * N];
    int32_t acc_result[N * N];

    make_test_matrix(a, N);
    make_identity(w, N);

    test_normal(a, w, cpu_result, N);
    test_acc(a, w, acc_result, N);
    check_matrices(cpu_result, acc_result, N);


    test_normal_average(a, w, cpu_result, N);
    test_acc_average(a, w, acc_result, N);

    // neorv32_uart0_printf(
    //     "Send cycles: %u\n",
    //     (uint32_t)send_cycles
    // );

    // neorv32_uart0_printf(
    //     "Pack cycles: %u\n",
    //     (uint32_t)pack_cycles
    // );

    // neorv32_uart0_printf(
    //     "Wait cycles: %u\n",
    //     (uint32_t)wait_cycles
    // );

    // neorv32_uart0_printf(
    //     "IRQ count: %u\n",
    //     irq_count
    // );

    // neorv32_uart0_printf(
    //     "IRQ cycles: %u\n",
    //     (uint32_t)irq_cycles_total
    // );
    return 0;
}
