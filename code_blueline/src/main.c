#include "stdint.h"
#include "main.h"
#include "main_fsm.h"
#include "tones.h"
#include "slic.h"
#include "bt.h"
#include "gpio_i2c.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/smf.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(main, LOG_LEVEL_NONE);

/* ------------------------------------------------------------------ */
/* Tone instances — one per subscriber line                           */
/* tone_run initialised to 0 (blocked), melody = gb_ring              */
/* periph_addr identifies which SLIC to ring                          */
/* ------------------------------------------------------------------ */
static tone_instance_t tone1 = {
    .melody = gb_ring,
    .periph_addr = PERIPH_ADDR_20,
};

static tone_instance_t tone2 = {
    .melody = de_ring,
    .periph_addr = PERIPH_ADDR_21,
};

static tone_instance_t tone3 = {
    .melody = de_ring,
    .periph_addr = PERIPH_ADDR_21, /* update when hw address known  */
};


/* ------------------------------------------------------------------ */
/* FSM instances                                                       */
/* ------------------------------------------------------------------ */

static struct fsm_instance fsm1 = {
    .button = GPIO_DT_SPEC_GET(DT_ALIAS(detint), gpios),
    .engaged_bit = ENGAGED_FSM1_BIT,
    .int_call_bit = INT_CALL_FSM1_BIT,
    .tone = &tone1,
    .status_bit = 6,
    .slic_addr = PERIPH_ADDR_20,
};

static struct fsm_instance fsm2 = {
    .button = GPIO_DT_SPEC_GET(DT_ALIAS(detint), gpios),
    .engaged_bit = ENGAGED_FSM2_BIT,
    .int_call_bit = INT_CALL_FSM2_BIT,
    .tone = &tone2,
    .status_bit = 7,
    .slic_addr = PERIPH_ADDR_21,
};

/* NOTE: sw2 must be defined in the devicetree overlay               */
static struct fsm_instance fsm3 = {
    .button = GPIO_DT_SPEC_GET(DT_ALIAS(detint), gpios),
    .engaged_bit = ENGAGED_FSM3_BIT,
    .int_call_bit = INT_CALL_FSM3_BIT,
    .tone = &tone3,
    .status_bit = 5,
    .slic_addr = PERIPH_ADDR_23,
};

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

#define FSM_STACK_SIZE  1024
#define FSM_PRIORITY    5
#define TONE_STACK_SIZE  1024
#define TONE_PRIORITY    6

static K_SEM_DEFINE(fsm_ready, 0, 3);

static void fsm1_thread(void* a, void* b, void* c)
{
    k_sem_take(&fsm_ready, K_FOREVER);
    fsm_run(&fsm1);
}

static void fsm2_thread(void* a, void* b, void* c)
{
    k_sem_take(&fsm_ready, K_FOREVER);
    fsm_run(&fsm2);
}

static void fsm3_thread(void* a, void* b, void* c)
{
    k_sem_take(&fsm_ready, K_FOREVER);
    fsm_run(&fsm3);
}

K_THREAD_DEFINE(fsm1_tid, FSM_STACK_SIZE, fsm1_thread,
                NULL, NULL, NULL, FSM_PRIORITY, 0, 0);
K_THREAD_DEFINE(fsm2_tid, FSM_STACK_SIZE, fsm2_thread,
                NULL, NULL, NULL, FSM_PRIORITY, 0, 0);
K_THREAD_DEFINE(fsm3_tid, FSM_STACK_SIZE, fsm3_thread,
                NULL, NULL, NULL, FSM_PRIORITY, 0, 0);

/* Tone threads — one per subscriber line, pass tone_instance as arg */
K_THREAD_DEFINE(tone1_tid, TONE_STACK_SIZE, tone_task,
                &tone1, NULL, NULL, TONE_PRIORITY, 0, 0);
K_THREAD_DEFINE(tone2_tid, TONE_STACK_SIZE, tone_task,
                &tone2, NULL, NULL, TONE_PRIORITY, 0, 0);
K_THREAD_DEFINE(tone3_tid, TONE_STACK_SIZE, tone_task,
                &tone3, NULL, NULL, TONE_PRIORITY, 0, 0);

/* ----------------------------------------------------------- */
/* main                                                                */
/* ----------------------------------------------------------- */

int main(void)
{
    int ret;


/* Initialise tone_run semaphores — blocked at start              */
    k_sem_init(&tone1.tone_run, 0, 1);
    k_sem_init(&tone2.tone_run, 0, 1);
    k_sem_init(&tone3.tone_run, 0, 1);

    init_gpios();
    ret = init_slic();
    init_bt();

    if (ret) LOG_ERR("init_slic failed: %d", ret);


    // set spio cs pin on address decoder for spi
    set_slic(i2c_bus0,
             PERIPH_ADDR_24,
             MCPREG_GPIO_B,
             0x00, //Set Address decoder to 0 for CMX no 1
             MASK_HIGH);
    initCMX865();

    set_slic(i2c_bus0,
             PERIPH_ADDR_24,
             MCPREG_GPIO_B,
             0x01, //Set Address decoder to 1 for CMX no 1
             MASK_HIGH);
    initCMX865();

    ret = fsm_init(&fsm1);
    if (ret) LOG_ERR("fsm1 init failed: %d", ret);

    ret = fsm_init(&fsm2);
    if (ret) LOG_ERR("fsm2 init failed: %d", ret);

    ret = fsm_init(&fsm3);
    if (ret) LOG_ERR("fsm3 init failed: %d", ret);

    /* Register instances so update_int_call can wake them            */
    fsm_register(&fsm1);
    fsm_register(&fsm2);
    fsm_register(&fsm3);

    k_sem_give(&fsm_ready);
    k_sem_give(&fsm_ready);
    k_sem_give(&fsm_ready);

    return 0;
}
