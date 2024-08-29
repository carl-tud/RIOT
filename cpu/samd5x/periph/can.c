/*
 * Copyright (C) 2023 ML!PA Consulting GmbH
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @ingroup     cpu_samd5x
 * @{
 *
 * @file
 * @brief       Implementation of the CAN controller driver
 *
 * @author      Firas Hamdi <firas.hamdi@ml-pa.com>
 * @}
 */

#include <assert.h>
#include <string.h>

#include "periph/can.h"
#include "periph/gpio.h"
#include "can/device.h"

/* #define ENABLE_DEBUG 0 */
#include "debug.h"

/**
 * @brief Value from SAMD5x/E5x Family datasheet, Tables 39-13 and 39-17
 */
#define CANDEV_SAMD5X_CLASSIC_FILTER 0x02
/**
 * @brief Set to 1 to access the internal loopback mode.
 * Check SAMD5x/E5x Family datasheet, Figure 39-4
 */
#define CANDEV_SAMD5X_INTERNAL_LOOPBACK 0

/**
 * @brief Specific configuration of the CAN filter
 */
enum {
    CANDEV_SAMD5X_FILTER_DISABLE = 0x00,
    CANDEV_SAMD5X_FILTER_RX_FIFO_0,
    CANDEV_SAMD5X_FILTER_RX_FIFO_1
};

/**
 * @brief Configuration of how to handle frames not matching the CAN filters
 */
enum {
    /* Direct frames not matching any CAN filters applied to Rx FIFO 0 */
    CAN_ACCEPT_RX_FIFO_0 = 0x00,
    /* Direct frames not matching any CAN filters applied to Rx FIFO 1 */
    CAN_ACCEPT_RX_FIFO_1,
    /* Reject all frames not matching any CAN filters applied */
    CAN_REJECT
};

typedef enum {
    MODE_INIT,
    MODE_TEST,
} can_mode_t;

typedef struct {
    /* Message Marker Put index */
    uint8_t put:4;
    /* Message Marker Get index */
    uint8_t get:4;
} can_mm_t;

/* Used to handle interrupts generated by the CAN controller 0 */
static can_t *_can_0;
/* Used to handle interrupts generated by the CAN controller 1 */
static can_t *_can_1;

static int _init(candev_t *candev);
static int _send(candev_t *candev, const struct can_frame *frame);
static int _set_filter(candev_t *candev, const struct can_filter *filter);
static int _remove_filter(candev_t *candev, const struct can_filter *filter);
static int _set(candev_t *candev, canopt_t opt, void *value, size_t value_len);
static void _isr(candev_t *candev);

static int _set_mode(Can *can, can_mode_t mode);

static const candev_driver_t candev_samd5x_driver = {
    .init = _init,
    .send = _send,
    .set_filter = _set_filter,
    .remove_filter = _remove_filter,
    .set = _set,
    .isr = _isr,
};

/* Values taken from SAMD5x/E5x datasheet section 39.8.8 */
static const struct can_bittiming_const bittiming_const = {
    .tseg1_min = 1,
    .tseg1_max = 256,
    .tseg2_min = 1,
    .tseg2_max = 128,
    .sjw_max = 128,
    .brp_min = 1,
    .brp_max = 512,
    .brp_inc = 1,
};

static int _power_on(can_t *dev)
{
    if (dev->conf->can == CAN0) {
        DEBUG_PUTS("CAN0 controller is used");
        MCLK->AHBMASK.reg |= MCLK_AHBMASK_CAN0;
    }
    else if (dev->conf->can == CAN1) {
        DEBUG_PUTS("CAN1 controller is used");
        MCLK->AHBMASK.reg |= MCLK_AHBMASK_CAN1;
    }
    else {
        DEBUG_PUTS("Unsupported CAN channel");
        assert(0);
    }

    return 0;
}

static int _power_off(can_t *dev)
{
    if (dev->conf->can == CAN0) {
        DEBUG_PUTS("CAN0 controller is used");
        MCLK->AHBMASK.reg &= ~MCLK_AHBMASK_CAN0;
    }
    else if (dev->conf->can == CAN1) {
        DEBUG_PUTS("CAN1 controller is used");
        MCLK->AHBMASK.reg &= ~MCLK_AHBMASK_CAN1;
    }
    else {
        DEBUG_PUTS("Unsupported CAN channel");
        return -1;
    }

    return 0;
}

static void _enter_init_mode(Can *can)
{
    can->CCCR.reg |= CAN_CCCR_INIT;
    while (!(can->CCCR.reg & CAN_CCCR_INIT)) {}
    DEBUG_PUTS("Device in init mode");
}

static void _exit_init_mode(Can *can)
{
    if (can->CCCR.reg & CAN_CCCR_INIT) {
        can->CCCR.reg &= ~CAN_CCCR_INIT;
    }

    while (can->CCCR.reg & CAN_CCCR_INIT) {}
    DEBUG_PUTS("Device out of init mode");
}

static int _set_mode(Can *can, can_mode_t can_mode)
{
    switch (can_mode) {
        case MODE_INIT:
            _enter_init_mode(can);
            can->CCCR.reg |= CAN_CCCR_CCE;
            break;
        case MODE_TEST:
            DEBUG_PUTS("test mode");
            _enter_init_mode(can);
            /* CCCR.TEST and CCCR.MON can be set only when CCCR.INIT and CCCR.CCE are set */
            can->CCCR.reg |= CAN_CCCR_CCE;
            can->CCCR.reg |= CAN_CCCR_TEST;
            can->TEST.reg |= CAN_TEST_LBCK;
#if IS_ACTIVE(CANDEV_SAMD5X_INTERNAL_LOOPBACK)
            can->CCCR.reg |= CAN_CCCR_MON;
#endif
            _exit_init_mode(can);
            break;
        default:
            DEBUG_PUTS("Unsupported mode");
            return -1;
    }

    return 0;
}

static void _setup_clock(can_t *dev)
{
    if (dev->conf->can == CAN0) {
        GCLK->PCHCTRL[CAN0_GCLK_ID].reg = GCLK_PCHCTRL_CHEN | GCLK_PCHCTRL_GEN(dev->conf->gclk_src);
    }
    else if (dev->conf->can == CAN1) {
        GCLK->PCHCTRL[CAN1_GCLK_ID].reg = GCLK_PCHCTRL_CHEN | GCLK_PCHCTRL_GEN(dev->conf->gclk_src);
    }
    else {
        DEBUG_PUTS("CAN channel not supported");
    }
}

static void _set_bit_timing(can_t *dev)
{
    assert(dev->candev.bittiming.sjw >= 1);
    assert(dev->candev.bittiming.phase_seg2 >= 1);
    assert(dev->candev.bittiming.phase_seg1 + dev->candev.bittiming.prop_seg >= 1);
    assert(dev->candev.bittiming.brp >= 1);

    DEBUG("bitrate=%" PRIu32 ", sample_point=%" PRIu32 ", brp=%" PRIu32 ", prop_seg=%" PRIu32
          ", phase_seg1=%" PRIu32 ", phase_seg2=%" PRIu32 ", sjw=%" PRIu32 "\n",
          dev->candev.bittiming.bitrate, dev->candev.bittiming.sample_point,
          dev->candev.bittiming.brp, dev->candev.bittiming.prop_seg,
          dev->candev.bittiming.phase_seg1, dev->candev.bittiming.phase_seg2,
          dev->candev.bittiming.sjw);

    /* Set bit timing */
    dev->conf->can->NBTP.reg = (uint32_t)((CAN_NBTP_NTSEG2(dev->candev.bittiming.phase_seg2 - 1))
                            | (CAN_NBTP_NTSEG1(dev->candev.bittiming.phase_seg1 + dev->candev.bittiming.prop_seg - 1))
                            | (CAN_NBTP_NBRP(dev->candev.bittiming.brp - 1))
                            | (CAN_NBTP_NSJW(dev->candev.bittiming.sjw - 1)));
}

static void _set_tx_fifo_data_size(can_t *dev, uint8_t size) {
    assert(size < 0x8);
    dev->conf->can->TXESC.reg |= CAN_TXESC_TBDS(size);
}

static void _set_rx_buffer_data_size(can_t *dev, uint8_t size) {
    assert(size < 0x8);
    dev->conf->can->RXESC.reg |= CAN_RXESC_RBDS(size);
}

static void _set_rx_fifo_0_data_size(can_t *dev, uint8_t size) {
    assert(size < 0x8);
    dev->conf->can->RXESC.reg |= CAN_RXESC_F0DS(size);
}

static void _set_rx_fifo_1_data_size(can_t *dev, uint8_t size) {
    assert(size < 0x8);
    dev->conf->can->RXESC.reg |= CAN_RXESC_F1DS(size);
}

static void _set_can_pins(can_t *dev)
{
    assert(dev->conf->tx_pin != GPIO_UNDEF);
    assert(dev->conf->rx_pin != GPIO_UNDEF);

    gpio_init(dev->conf->tx_pin, GPIO_OUT);
    gpio_init(dev->conf->rx_pin, GPIO_IN);

    if (dev->conf->can == CAN0) {
        gpio_init_mux(dev->conf->tx_pin, GPIO_MUX_I);
        gpio_init_mux(dev->conf->rx_pin, GPIO_MUX_I);
    }
    else if (dev->conf->can == CAN1) {
        gpio_init_mux(dev->conf->tx_pin, GPIO_MUX_H);
        gpio_init_mux(dev->conf->rx_pin, GPIO_MUX_H);
    }
    else {
        DEBUG_PUTS("Unsupported can channel");
    }
}

void candev_samd5x_enter_sleep_mode(candev_t *candev)
{
    can_t *dev = container_of(candev, can_t, candev);

    dev->conf->can->CCCR.reg |= CAN_CCCR_CSR;
    while (!(dev->conf->can->CCCR.reg & CAN_CCCR_CSA)) {}
    DEBUG_PUTS("Device in sleep mode");
}

void candev_samd5x_exit_sleep_mode(candev_t *candev)
{
    can_t *dev = container_of(candev, can_t, candev);

    dev->conf->can->CCCR.reg &= ~CAN_CCCR_CSR;
    while (dev->conf->can->CCCR.reg & CAN_CCCR_CSA) {}
    DEBUG_PUTS("Device out of sleep mode");
}

void candev_samd5x_tdc_control(can_t *dev)
{
    if (dev->tdc_ctrl) {
        DEBUG_PUTS("Enable Transceiver Delay Compensation");
        dev->conf->can->DBTP.reg |= CAN_DBTP_TDC;
    }
    else {
        DEBUG_PUTS("Disable Transceiver Delay Compensation");
        dev->conf->can->DBTP.reg &= ~(CAN_DBTP_TDC);
    }
}

void can_init(can_t *dev, const can_conf_t *conf)
{
    dev->candev.driver = &candev_samd5x_driver;

    struct can_bittiming timing = { .bitrate = CANDEV_SAMD5X_DEFAULT_BITRATE,
                                    .sample_point = CANDEV_SAMD5X_DEFAULT_SPT };

    uint32_t clk_freq = sam0_gclk_freq(conf->gclk_src);
    can_device_calc_bittiming(clk_freq, &bittiming_const, &timing);

    memcpy(&dev->candev.bittiming, &timing, sizeof(timing));
    dev->conf = conf;
}

static void _dump_msg_ram_section(can_t *dev)
{
    puts("start address|\tsize of section");
    printf("Standard filters|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.std_filter),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.std_filter)));
    printf("Extended filters|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.ext_filter),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.ext_filter)));
    printf("Rx FIFO 0|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.rx_fifo_0),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.rx_fifo_0)));
    printf("Rx FIFO 1|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.rx_fifo_1),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.rx_fifo_1)));
    printf("Rx buffer|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.rx_buffer),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.rx_buffer)));
    printf("Tx event FIFO|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.tx_event_fifo),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.tx_event_fifo)));
    printf("Tx buffer|\t0x%08lx|\t%lu\n", (uint32_t)(dev->msg_ram_conf.tx_fifo_queue),
                                        (uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.tx_fifo_queue)));
}

static int _init(candev_t *candev)
{
    can_t *dev = container_of(candev, can_t, candev);
    int res = 0;

    sam0_gclk_enable(dev->conf->gclk_src);

    _setup_clock(dev);
    _power_on(dev);

    _set_can_pins(dev);

    res = _set_mode(dev->conf->can, MODE_INIT);
    if (res != 0) {
        return -1;
    }

    _set_bit_timing(dev);

    candev_samd5x_tdc_control(dev);

    /*Configure the start addresses of the RAM message sections */
    dev->conf->can->SIDFC.reg = CAN_SIDFC_FLSSA((uint32_t)(dev->msg_ram_conf.std_filter))
                            | CAN_SIDFC_LSS((uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.std_filter)));
    dev->conf->can->XIDFC.reg = CAN_XIDFC_FLESA((uint32_t)(dev->msg_ram_conf.ext_filter))
                            | CAN_XIDFC_LSE((uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.ext_filter)));
    dev->conf->can->RXF0C.reg = CAN_RXF0C_F0SA((uint32_t)(dev->msg_ram_conf.rx_fifo_0))
                            | CAN_RXF0C_F0S((uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.rx_fifo_0)));
    dev->conf->can->RXF1C.reg = CAN_RXF1C_F1SA((uint32_t)(dev->msg_ram_conf.rx_fifo_1))
                            | CAN_RXF1C_F1S((uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.rx_fifo_1)));
    dev->conf->can->RXBC.reg = CAN_RXBC_RBSA((uint32_t)(dev->msg_ram_conf.rx_buffer));
    dev->conf->can->TXEFC.reg = CAN_TXEFC_EFSA((uint32_t)(dev->msg_ram_conf.tx_event_fifo))
                            | CAN_TXEFC_EFS((uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.tx_event_fifo)));
    dev->conf->can->TXBC.reg = CAN_TXBC_TBSA((uint32_t)(dev->msg_ram_conf.tx_fifo_queue))
                            | CAN_TXBC_TFQS((uint32_t)(ARRAY_SIZE(dev->msg_ram_conf.tx_fifo_queue)));

    /* In the vendor file, the data field size in CanMramTxbe is set to 64 bytes
        although it can be configurable. That's why 64 bytes is used here by default */
    _set_tx_fifo_data_size(dev, CAN_RXESC_F1DS_DATA64_Val);
    /* In the vendor file, the data field size in CanMramRxbe is set to 64 bytes
        although it can be configurable. That's why 64 bytes is used here by default */
    _set_rx_buffer_data_size(dev, CAN_RXESC_RBDS_DATA64_Val);
    /* In the vendor file, the data field size in CanMramRxf0e is set to 64 bytes
        although it can be configurable. That's why 64 bytes is used here by default */
    _set_rx_fifo_0_data_size(dev, CAN_RXESC_F0DS_DATA64_Val);
    /* In the vendor file, the data field size in CanMramRxf1e is set to 64 bytes
        although it can be configurable. That's why 64 bytes is used here by default */
    _set_rx_fifo_1_data_size(dev, CAN_RXESC_F1DS_DATA64_Val);

    if (IS_ACTIVE(ENABLE_DEBUG)) {
        _dump_msg_ram_section(dev);
    }
    /* Disable automatic retransmission by default */
    /* This can be added as a configuration parameter for the CAN controller */
    dev->conf->can->CCCR.reg |= CAN_CCCR_DAR;

    /* Reject all remote frames */
    dev->conf->can->GFC.reg |= CAN_GFC_RRFE | CAN_GFC_RRFS;

    /* Enable reception interrupts: reception on FIFO0 and FIFO1 */
    dev->conf->can->IE.reg |= CAN_IE_RF0NE | CAN_IE_RF1NE;
    /* Enable transmission events interrupts */
    dev->conf->can->IE.reg |= CAN_IE_TEFNE;
    /* Enable errors interrupts */
    dev->conf->can->IE.reg |= CAN_IE_PEDE | CAN_IE_PEAE | CAN_IE_BOE | CAN_IE_EWE | CAN_IE_EPE;
    /* Enable the interrupt lines */
    dev->conf->can->ILE.reg = CAN_ILE_EINT0 | CAN_ILE_EINT1;

    /* Enable the peripheral's interrupt */
    if (dev->conf->can == CAN0) {
        NVIC_EnableIRQ(CAN0_IRQn);
        _can_0 = dev;
    }
    else {
        NVIC_EnableIRQ(CAN1_IRQn);
        _can_1 = dev;
    }

    /* Exit initialization mode */
    _exit_init_mode(dev->conf->can);

    return res;
}

static uint8_t _form_message_marker(can_mm_t *can_mm)
{
    return can_mm->put | (can_mm->get << 4);
}

static int _send(candev_t *candev, const struct can_frame *frame)
{
    /* this assertion ensures the EFF-FLAG is set or the id does not exceed the CAN_SFF_MASK*/
    assert( (frame->can_id & CAN_EFF_FLAG)
            || ((frame->can_id & CAN_SFF_MASK) == (frame->can_id & CAN_EFF_MASK)) );
    can_t *dev = container_of(candev, can_t, candev);

    if (frame->can_dlc > CAN_MAX_DLEN) {
        DEBUG_PUTS("CAN frame payload not supported");
        return -1;
    }

    /* Check if the Tx FIFO is full */
    if (dev->conf->can->TXFQS.reg & CAN_TXFQS_TFQF) {
        DEBUG_PUTS("Tx FIFO is full");
        return -1;
    }

    can_mm_t can_mm = {0};
    uint8_t fifo_queue_put_idx = (dev->conf->can->TXFQS.reg >> CAN_TXFQS_TFQPI_Pos) & 0x1F;
    DEBUG("Tx FIFO put index = %u\n", fifo_queue_put_idx);
    uint8_t fifo_queue_get_idx = (dev->conf->can->TXFQS.reg >> CAN_TXFQS_TFGI_Pos) & 0x1F;
    DEBUG("Tx FIFO get index = %u\n", fifo_queue_get_idx);
    can_mm.put = fifo_queue_put_idx;
    can_mm.get = fifo_queue_get_idx;
    uint32_t txbe0 = 0;
    uint32_t txbe1 = 0;
    if (frame->can_id & CAN_EFF_FLAG) {
        DEBUG_PUTS("Extended ID");
        txbe0 = CAN_TXBE_0_ID(frame->can_id & CAN_EFF_MASK) | CAN_TXBE_0_XTD;
    }
    else {
        DEBUG_PUTS("Standard identifier");
        txbe0 = CAN_TXBE_0_ID((frame->can_id & CAN_SFF_MASK) << 18);
    }
    txbe0 |= (((frame->can_id & CAN_RTR_FLAG) >> 30) << CAN_TXBE_0_RTR_Pos);
    txbe0 |= CAN_TXBE_0_ESI;
    /* Write the prepared word */
    dev->msg_ram_conf.tx_fifo_queue[can_mm.put].TXBE_0.reg = txbe0;

    /* Prepare second word */
    txbe1 = CAN_TXBE_1_DLC(frame->can_dlc) | CAN_TXBE_1_EFC |
                CAN_TXBE_1_MM(_form_message_marker(&can_mm));
    /* Write the second word */
    dev->msg_ram_conf.tx_fifo_queue[can_mm.put].TXBE_1.reg = txbe1;

    memcpy((void *)dev->msg_ram_conf.tx_fifo_queue[can_mm.put].TXBE_DATA, frame->data, frame->can_dlc);

    /* Request transmission */
    dev->conf->can->TXBAR.reg |= (1 << can_mm.put);

    return 0;
}

static int16_t _find_filter(can_t *can, const struct can_filter *filter, bool is_std_filter)
{
    int16_t idx = -1;
    uint32_t msg_id;
    /* Standard filter */
    if (is_std_filter) {
        /* Search for the standard filter in the CAN controller message RAM */
        for (uint8_t i = 0; i < ARRAY_SIZE(can->msg_ram_conf.std_filter); i++) {
            msg_id = ((can->msg_ram_conf.std_filter[i].SIDFE_0.reg &
                       CAN_SIDFE_0_SFID1_Msk) >> CAN_SIDFE_0_SFID1_Pos);
            if ((filter->can_id & CAN_SFF_MASK) == msg_id ) {
                idx = i;
                break;
            }
        }
    }
    /* Extended filter */
    else {
        /* Search for the extended filter in the CAN controller message RAM */
        for (uint8_t i = 0; i < ARRAY_SIZE(can->msg_ram_conf.ext_filter); i++) {
            msg_id = ((can->msg_ram_conf.ext_filter[i].XIDFE_0.reg &
                       CAN_XIDFE_0_EFID1_Msk) >> CAN_XIDFE_0_EFID1_Pos);
            if ((filter->can_id & CAN_EFF_MASK) == msg_id) {
                idx = i;
                break;
            }
        }
    }

    return idx;
}

static int _set_filter(candev_t *candev, const struct can_filter *filter)
{
    can_t *dev = container_of(candev, can_t, candev);

    int16_t idx = 0;
    uint32_t tmp = 0;
    uint8_t filter_conf = 0;
    switch (filter->target_mailbox) {
        case 0:
            filter_conf = CANDEV_SAMD5X_FILTER_RX_FIFO_0;
            break;
        case 1 :
            filter_conf = CANDEV_SAMD5X_FILTER_RX_FIFO_1;
            break;
        default:
            puts("Invalid target mailbox --> Do not apply filter");
            return -1;
    }
    if (filter->can_id & CAN_EFF_FLAG) {
        DEBUG_PUTS("Extended filter to add in the extended filter section of the message RAM");
        /* Check if the filter already exists */
        idx = _find_filter(dev, filter, false);
        if (idx != -1) {
            DEBUG_PUTS("Extended filter already exists --> Update it");
        }
        else {
            /* Find a free slot where to save the filter */
            for (idx = 0; (uint16_t)idx < ARRAY_SIZE(dev->msg_ram_conf.ext_filter); idx++) {
                tmp = dev->msg_ram_conf.ext_filter[idx].XIDFE_0.reg;
                if (((tmp & CAN_XIDFE_0_EFEC_Msk) >> CAN_XIDFE_0_EFEC_Pos) ==
                    CANDEV_SAMD5X_FILTER_DISABLE)
                {
                    DEBUG_PUTS("empty slot");
                    break;
                }
            }
        }

        if (idx == ARRAY_SIZE(dev->msg_ram_conf.ext_filter)) {
            DEBUG_PUTS("Reached maximum capacity of extended filters --> Could not add filter");
            return -1;
        }

        DEBUG("Extended Filter to add at idx = %d\n", idx);
        dev->msg_ram_conf.ext_filter[idx].XIDFE_0.reg |= CAN_XIDFE_0_EFEC(filter_conf);
        /* For now, only CLASSIC filters are supported */
        dev->msg_ram_conf.ext_filter[idx].XIDFE_1.reg |= CAN_XIDFE_1_EFT(CANDEV_SAMD5X_CLASSIC_FILTER);
        dev->msg_ram_conf.ext_filter[idx].XIDFE_0.reg |= CAN_XIDFE_0_EFID1(filter->can_id);
        dev->msg_ram_conf.ext_filter[idx].XIDFE_1.reg |= CAN_XIDFE_1_EFID2(filter->can_mask & CAN_EFF_MASK);
        DEBUG("Extended filter element N°%d: F0 = 0x%08lx, F1 = 0x%08lx\n", idx,
                                    (uint32_t)(dev->msg_ram_conf.ext_filter[idx].XIDFE_0.reg),
                                    (uint32_t)(dev->msg_ram_conf.ext_filter[idx].XIDFE_1.reg));
        _set_mode(dev->conf->can, MODE_INIT);
        /* Reject all extended frames that are not matching the filters applied */
        dev->conf->can->GFC.reg |= CAN_GFC_ANFE((uint32_t)CAN_REJECT);
        _exit_init_mode(dev->conf->can);
    }
    else {
        DEBUG_PUTS("Standard filter to add in the standard filter section of the message RAM");
        /* Check if the filter already exists */
        idx = _find_filter(dev, filter, true);
        if (idx != -1) {
            DEBUG_PUTS("Standard filter already exists --> Update it");
        }
        else {
            /* Find a free slot where to save the filter */
            for (idx = 0; (uint16_t)idx < ARRAY_SIZE(dev->msg_ram_conf.std_filter); idx++) {
                tmp = dev->msg_ram_conf.std_filter[idx].SIDFE_0.reg;
                if (((tmp & CAN_SIDFE_0_SFEC_Msk) >> CAN_SIDFE_0_SFEC_Pos) ==
                    CANDEV_SAMD5X_FILTER_DISABLE)
                {
                    DEBUG_PUTS("empty slot");
                    break;
                }
            }
        }

        if (idx == ARRAY_SIZE(dev->msg_ram_conf.std_filter)) {
            DEBUG_PUTS("Reached maximum capacity of standard filters --> Could not add filter");
            return -1;
        }

        DEBUG("Standard Filter to add at idx = %d\n", idx);
        /* For now, only CLASSIC filters are supported */
        dev->msg_ram_conf.std_filter[idx].SIDFE_0.reg = CAN_SIDFE_0_SFEC(filter_conf)
                                                      | CAN_SIDFE_0_SFT(CANDEV_SAMD5X_CLASSIC_FILTER)
                                                      | CAN_SIDFE_0_SFID1(filter->can_id & CAN_SFF_MASK)
                                                      | CAN_SIDFE_0_SFID2(filter->can_mask & CAN_SFF_MASK);

        DEBUG("Standard filter element N°%d: S0 = 0x%08lx\n", idx,
                                    (uint32_t)(dev->msg_ram_conf.std_filter[idx].SIDFE_0.reg));
        _set_mode(dev->conf->can, MODE_INIT);
        /* Reject all standard frames that are not matching the filters applied */
        dev->conf->can->GFC.reg |= CAN_GFC_ANFS((uint32_t)CAN_REJECT);
        _exit_init_mode(dev->conf->can);
    }

    return idx;
}

static int _remove_filter(candev_t *candev, const struct can_filter *filter)
{
    can_t *dev = container_of(candev, can_t, candev);

    int16_t idx = 0;
    if (filter->can_id & CAN_EFF_FLAG) {
        idx = _find_filter(dev, filter, false);
        if (idx != -1) {
            DEBUG("Extended filter to disable at idx = %d\n", idx);
            dev->msg_ram_conf.ext_filter[idx].XIDFE_0.reg &= ~CAN_XIDFE_0_EFEC_Msk;
        }
        else {
            DEBUG_PUTS("Filter not found");
            return -1;
        }
    }
    else {
        idx = _find_filter(dev, filter, true);
        if (idx != -1) {
            DEBUG("Standard filter to disable at idx = %d\n", idx);
            dev->msg_ram_conf.std_filter[idx].SIDFE_0.reg &= ~CAN_SIDFE_0_SFEC_Msk;
        }
        else {
            DEBUG_PUTS("Filter not found");
            return -1;
        }
    }

    return idx;
}

static int _set(candev_t *candev, canopt_t opt, void *value, size_t value_len)
{
    can_t *dev = container_of(candev, can_t, candev);
    int res = 0;

    switch (opt) {
        case CANOPT_BITTIMING:
            if (value_len < sizeof(struct can_bittiming)) {
                return -1;
            }
            else {
                memcpy(&candev->bittiming, value, sizeof(struct can_bittiming));
                uint32_t clk_freq = sam0_gclk_freq(dev->conf->gclk_src);
                can_device_calc_bittiming(clk_freq, &bittiming_const, &candev->bittiming);
                res = _init(candev);
                if (res == 0) {
                    res = sizeof(candev->bittiming);
                }
                else {
                    return -1;
                }
            }
            break;
        case CANOPT_RX_FILTERS:
            if (value_len < sizeof(struct can_filter)) {
                return -1;
            }
            else {
                res = _set_filter(candev, value);
                if (res >= 0) {
                    res = sizeof(struct can_filter);
                }
                else {
                    return -1;
                }
            }
            break;
        case CANOPT_STATE:
            if (value_len < sizeof(canopt_state_t)) {
                return -1;
            }
            else {
                switch (*((canopt_state_t *)value)) {
                    case CANOPT_STATE_OFF:
                        res = _power_off(dev);
                        if (res == 0) {
                            res = sizeof(canopt_state_t);
                        }
                        else {
                            return -1;
                        }
                        break;
                    case CANOPT_STATE_ON:
                        res = _power_on(dev);
                        if (res == 0) {
                            res = sizeof(canopt_state_t);
                        }
                        else {
                            return -1;
                        }
                        break;
                    case CANOPT_STATE_LOOPBACK:
                        res = _set_mode(dev->conf->can, MODE_TEST);
                        if (res == 0) {
                            res = sizeof(canopt_state_t);
                        }
                        else {
                            return -1;
                        }
                        break;
                    default:
                        break;
                }
            }
        default:
            break;
    }

    return res;
}

static void _isr(candev_t *candev)
{
    can_t *dev = container_of(candev, can_t, candev);

    uint32_t irq_reg = dev->conf->can->IR.reg;
    DEBUG("isr: IR reg = 0x%08lx\n", irq_reg);

    /* Interrupt triggered due to reception of CAN frame on Rx_FIFO_0 */
    if (irq_reg & CAN_IR_RF0N) {
        DEBUG_PUTS("New message in Rx FIFO 0");
        /* Clear the interrupt source flag */
        dev->conf->can->IR.reg |= CAN_IR_RF0N;

        uint16_t rx_get_idx = 0;
        uint16_t rx_put_idx = 0;
        uint32_t reg = dev->conf->can->RXF0S.reg;
        rx_get_idx = (reg >> CAN_RXF0S_F0GI_Pos) & 0x3F;
        DEBUG("rx get index = %u\n", rx_get_idx);
        rx_put_idx = (reg >> CAN_RXF0S_F0PI_Pos) & 0x3F;
        DEBUG("rx put index = %u\n", rx_put_idx);

        struct can_frame frame_received = {0};
        /* Reuse variable to avoid multiple read of the same register */
        reg = dev->msg_ram_conf.rx_fifo_0[rx_get_idx].RXF0E_0.reg;
        if (!(reg & CAN_RXF0E_0_XTD)) {
            DEBUG_PUTS("Received standard CAN frame");
            frame_received.can_id = (reg & CAN_RXF0E_0_ID_Msk) >> 18;
        }
        else {
            DEBUG_PUTS("Received extended CAN frame");
            frame_received.can_id = (reg & CAN_RXF0E_0_ID_Msk) | CAN_EFF_FLAG;
        }
        if (reg & CAN_RXF0E_0_RTR) {
            frame_received.can_id |= CAN_RTR_FLAG;
        }

        frame_received.can_dlc =
            (dev->msg_ram_conf.rx_fifo_0[rx_get_idx].RXF0E_1.reg & CAN_RXF0E_1_DLC_Msk) >>
            CAN_RXF0E_1_DLC_Pos;
        memcpy(frame_received.data, (uint32_t *)dev->msg_ram_conf.rx_fifo_0[rx_get_idx].RXF0E_DATA, frame_received.can_dlc);

        dev->conf->can->RXF0A.reg = CAN_RXF0A_F0AI(rx_get_idx);
        if (dev->candev.event_callback) {
            dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_INDICATION, &frame_received);
        }
    }

    /* Interrupt triggered due to reception of CAN frame on Rx_FIFO_1 */
    if (irq_reg & CAN_IR_RF1N) {
        DEBUG_PUTS("New message in Rx FIFO 1");
        /* Clear the interrupt source flag */
        dev->conf->can->IR.reg |= CAN_IR_RF1N;

        uint16_t rx_get_idx = 0;
        uint16_t rx_put_idx = 0;
        uint32_t reg = dev->conf->can->RXF1S.reg;
        rx_get_idx = (reg >> CAN_RXF1S_F1GI_Pos) & 0x3F;
        DEBUG("rx get index = %u\n", rx_get_idx);
        rx_put_idx = (reg >> CAN_RXF1S_F1PI_Pos) & 0x3F;
        DEBUG("rx put index = %u\n", rx_put_idx);

        struct can_frame frame_received = {0};
        /* Reuse variable to avoid multiple read of the same register */
        reg = dev->msg_ram_conf.rx_fifo_1[rx_get_idx].RXF1E_0.reg;
        if (!(reg & CAN_RXF1E_0_XTD)) {
            DEBUG_PUTS("Received standard CAN frame");
            frame_received.can_id = (reg & CAN_RXF1E_0_ID_Msk) >> 18;
        }
        else {
            DEBUG_PUTS("Received extended CAN frame");
            frame_received.can_id = ((reg & CAN_RXF1E_0_ID_Msk) | CAN_EFF_FLAG);
        }
        if (reg & CAN_RXF1E_0_RTR) {
            frame_received.can_id |= CAN_RTR_FLAG;
        }

        frame_received.can_dlc =
            (dev->msg_ram_conf.rx_fifo_1[rx_get_idx].RXF1E_1.reg & CAN_RXF1E_1_DLC_Msk) >>
            CAN_RXF1E_1_DLC_Pos;
        memcpy(frame_received.data, (uint32_t *)dev->msg_ram_conf.rx_fifo_1[rx_get_idx].RXF1E_DATA, frame_received.can_dlc);

        dev->conf->can->RXF1A.reg = CAN_RXF1A_F1AI(rx_get_idx);
        if (dev->candev.event_callback) {
            dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_INDICATION, &frame_received);
        }
    }

    /* Interrupt triggered due to new transmission event */
    if (irq_reg & CAN_IR_TEFN) {
        DEBUG_PUTS("New Tx event FIFO entry");
        dev->conf->can->IR.reg |= CAN_IR_TEFN;
        if (dev->candev.event_callback) {
            dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_TX_CONFIRMATION, NULL);
        }
    }

    static uint8_t last_error_code = 0;
    /* Interrupt triggered due to protocol error in data phase */
    if (irq_reg & CAN_IR_PED) {
        DEBUG_PUTS("protocol error in data phase");
        dev->conf->can->IR.reg |= CAN_IR_PED;
        /* Extract the Tx and Rx error counters */
        uint32_t reg = dev->conf->can->ECR.reg;
        uint8_t tx_err_cnt = (uint8_t) (reg & CAN_ECR_TEC_Msk);
        DEBUG("tx error counter = %u\n", tx_err_cnt);
        uint8_t rx_err_cnt = (uint8_t)((reg & CAN_ECR_REC_Msk) >> CAN_ECR_REC_Pos);
        DEBUG("rx error counter = %u\n", rx_err_cnt);
        /* Check the CAN error type */
        uint8_t error_code = (uint8_t)(dev->conf->can->PSR.reg & CAN_PSR_LEC_Msk);
        DEBUG("error code = %u\n", error_code);
        if (error_code == CANDEV_SAMD5X_NO_CHANGE_ERROR) {
            error_code = last_error_code;
        }

        switch (error_code) {
            case CANDEV_SAMD5X_STUFF_ERROR:
                DEBUG_PUTS("STUFF error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_FORM_ERROR:
                DEBUG_PUTS("FORM error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_ACK_ERROR:
                DEBUG_PUTS("ACK error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_TX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_BIT1_ERROR:
                DEBUG_PUTS("BIT1 error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_TX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_BIT0_ERROR:
                DEBUG_PUTS("BIT0 error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_TX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_CRC_ERROR:
                DEBUG_PUTS("CRC error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_ERROR, NULL);
                }
                break;
            default:
                break;
        }

        last_error_code = error_code;
    }

    /* Interrupt triggered due to protocol error in arbitration phase */
    if (irq_reg & CAN_IR_PEA) {
        DEBUG_PUTS("protocol error in arbitration phase");
        dev->conf->can->IR.reg |= CAN_IR_PEA;
        /* Extract the Tx and Rx error counters */
        uint32_t reg = dev->conf->can->ECR.reg;
        uint8_t tx_err_cnt = (uint8_t) (reg & CAN_ECR_TEC_Msk);
        DEBUG("tx error counter = %u\n", tx_err_cnt);
        uint8_t rx_err_cnt = (uint8_t)((reg & CAN_ECR_REC_Msk) >> CAN_ECR_REC_Pos);
        DEBUG("rx error counter = %u\n", rx_err_cnt);
        /* Check the CAN error type */
        uint8_t error_code = (uint8_t)(dev->conf->can->PSR.reg & CAN_PSR_LEC_Msk);
        DEBUG("error code = %u\n", error_code);
        if (error_code == CANDEV_SAMD5X_NO_CHANGE_ERROR) {
            error_code = last_error_code;
        }

        switch (error_code) {
            case CANDEV_SAMD5X_STUFF_ERROR:
                DEBUG_PUTS("STUFF error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_FORM_ERROR:
                DEBUG_PUTS("FORM error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_ACK_ERROR:
                DEBUG_PUTS("ACK error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_TX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_BIT0_ERROR:
                DEBUG_PUTS("BIT0 error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_TX_ERROR, NULL);
                }
                break;
            case CANDEV_SAMD5X_CRC_ERROR:
                DEBUG_PUTS("CRC error");
                if (dev->candev.event_callback) {
                    dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_RX_ERROR, NULL);
                }
                break;
            default:
                break;
        }

        last_error_code = error_code;
    }

    /* Interrupt triggered due to Bus_Off status */
    if (irq_reg & CAN_IR_BO) {
        DEBUG_PUTS("Bus off");
        dev->conf->can->IR.reg |= CAN_IR_BO;
        if (dev->candev.event_callback) {
            dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_BUS_OFF, NULL);
        }
    }

    /* Interrupt triggered due to Error warning status */
    if (irq_reg & CAN_IR_EW) {
        DEBUG_PUTS("Error warning");
        dev->conf->can->IR.reg |= CAN_IR_EW;
        if (dev->candev.event_callback) {
            dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_ERROR_WARNING, NULL);
        }
    }

    /* Interrupt triggered due to Error passive status */
    if (irq_reg & CAN_IR_EP) {
        DEBUG_PUTS("Error Passive");
        dev->conf->can->IR.reg |= CAN_IR_EP;
        if (dev->candev.event_callback) {
            dev->candev.event_callback(&(dev->candev), CANDEV_EVENT_ERROR_PASSIVE, NULL);
        }
    }

    /* Enable the peripheral's interrupt */
    if (dev->conf->can == CAN0) {
        NVIC_EnableIRQ(CAN0_IRQn);
    }
    else {
        NVIC_EnableIRQ(CAN1_IRQn);
    }
}

#ifdef ISR_CAN0
void ISR_CAN0(void)
{
    DEBUG_PUTS("ISR CAN0");

    /* Disable the peripheral's interrupt to avoid potential 'interrupt bouncing' */
    NVIC_DisableIRQ(CAN0_IRQn);
    if (_can_0->candev.event_callback) {
        _can_0->candev.event_callback(&(_can_0->candev), CANDEV_EVENT_ISR, NULL);
    }
}
#endif

#ifdef ISR_CAN1
void ISR_CAN1(void)
{
    DEBUG_PUTS("ISR CAN1");

    /* Disable the peripheral's interrupt to avoid potential 'interrupt bouncing' */
    NVIC_DisableIRQ(CAN1_IRQn);
    if (_can_1->candev.event_callback) {
        _can_1->candev.event_callback(&(_can_1->candev), CANDEV_EVENT_ISR, NULL);
    }

}
#endif
