/*
 * Copyright (c) 2014, Freescale Semiconductor, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of Freescale Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "utilities/fsl_assert.h"
#include "bootloader/bl_context.h"
#include "bootloader_common.h"
#include "bootloader/bl_peripheral_interface.h"
#include "packet/command_packet.h"
#include "lpspi/fsl_lpspi.h"
#include "fsl_device_registers.h"
#include "peripherals_pinmux.h"
#include "packet/serial_packet.h"

#if BL_CONFIG_LPSPI

//! @addtogroup lpspi_peripheral
//! @{

////////////////////////////////////////////////////////////////////////////////
// Definitions
////////////////////////////////////////////////////////////////////////////////

#define SPI_EMPTY_CHAR (0x00) //!< Empty character.

//! @brief Synchronization state between LPSPI ISR and read/write functions.
typedef struct _lpspi_transfer_info
{
    const uint8_t *writeData;                                //!< The applications data to write
    volatile uint32_t bytesToTransfer;                       //!< The total number of bytes to be transmitted
    void (*data_source)(uint8_t *source_byte);               // !< Callback used to get byte to transmit.
    void (*data_sink)(uint8_t sink_byte, uint16_t instance); // !< Callback used to put received byte.
} lpspi_transfer_info_t;

////////////////////////////////////////////////////////////////////////////////
// Prototypes
////////////////////////////////////////////////////////////////////////////////

static bool lpspi_poll_for_activity(const peripheral_descriptor_t *self);
static status_t lpspi_full_init(const peripheral_descriptor_t *self, serial_byte_receive_func_t function);
static void lpspi_full_shutdown(const peripheral_descriptor_t *self);

static void lpspi_data_source(uint8_t *source_byte);

static void lpspi_initial_data_sink(uint8_t sink_byte, uint16_t instance);
static void lpspi_data_sink(uint8_t sink_byte, uint16_t instance);

static status_t lpspi_write(const peripheral_descriptor_t *self, const uint8_t *buffer, uint32_t byteCount);

static void lpspi_slave_irq_handler(uint32_t instance);

extern void LPSPI_SetSystemIRQ(uint32_t instance, PeripheralSystemIRQSetting set);

////////////////////////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////////////////////////

const peripheral_control_interface_t g_lpspiControlInterface = {
    .pollForActivity = lpspi_poll_for_activity, .init = lpspi_full_init, .shutdown = lpspi_full_shutdown, .pump = 0
};

const peripheral_byte_inteface_t g_lpspiByteInterface = {.init = NULL, .write = lpspi_write };

static lpspi_transfer_info_t s_lpspiInfo = {.writeData = 0, .bytesToTransfer = 0, .data_source = lpspi_data_source };

//! @brief Flag for detecting device activity
static bool s_lpspiActivity[FSL_FEATURE_SOC_LPSPI_COUNT] = { false };
static bool s_lpspiIntialized[FSL_FEATURE_SOC_LPSPI_COUNT] = { false };

const static uint32_t g_lpspiBaseAddr[] = LPSPI_BASE_ADDRS;

static serial_byte_receive_func_t s_lpspi_app_data_sink_callback;

////////////////////////////////////////////////////////////////////////////////
// Code
////////////////////////////////////////////////////////////////////////////////

#if !defined(BL_LPSPI_SIZE_OPTIMIZE) || USE_ONLY_SPI(0)
//! @brief Implementation of LPSPI0 handler named in startup code.
//!
//! Passes instance to generic LPSPI IRQ handler.
void LPSPI0_IRQHandler()
{
    lpspi_slave_irq_handler(0);
}
#endif // !defined(BL_LPSPI_SIZE_OPTIMIZE) || USE_ONLY_SPI(0)

#if (!defined(BL_LPSPI_SIZE_OPTIMIZE) || USE_ONLY_SPI(1)) && (FSL_FEATURE_SOC_LPSPI_COUNT > 1)
//! @brief Implementation of LPSPI1 handler named in startup code.
//!
//! Passes instance to generic LPSPI IRQ handler.
void LPSPI1_IRQHandler()
{
    lpspi_slave_irq_handler(1);
}
#endif // (!defined(BL_LPSPI_SIZE_OPTIMIZE) || USE_ONLY_SPI(1)) && (HW_LPSPI_INSTANCE_COUNT > 1)

#if (!defined(BL_LPSPI_SIZE_OPTIMIZE) || USE_ONLY_SPI(2)) && (FSL_FEATURE_SOC_LPSPI_COUNT > 2)
//! @brief Implementation of LPSPI2 handler named in startup code.
//!
//! Passes instance to generic LPSPI IRQ handler.
void LPSPI2_IRQHandler()
{
    lpspi_slave_irq_handler(2);
}
#endif // (!defined(BL_LPSPI_SIZE_OPTIMIZE) || USE_ONLY_SPI(2)) && (HW_LPSPI_INSTANCE_COUNT > 2)

//! @brief LPSPI Slave Generic IRQ handler.
//!
//! @param instance Instance number of the LPSPI module.
static void lpspi_slave_irq_handler(uint32_t instance)
{
    assert(instance < FSL_FEATURE_SOC_LPSPI_COUNT);

    LPSPI_Type *baseAddr = (LPSPI_Type *)g_lpspiBaseAddr[instance];

    uint32_t status = LPSPI_GetStatusFlags(baseAddr);

    if (status & kLPSPI_RxDataReadyFlag)
    {
        // Get byte from receive data register.
        uint8_t sink_byte = (uint8_t)LPSPI_ReadDataNonBlocking(baseAddr);
        s_lpspiInfo.data_sink(sink_byte, instance);
    }

    if (status & kLPSPI_TxDataRequestFlag)
    {
        uint8_t source_byte = SPI_EMPTY_CHAR;
        s_lpspiInfo.data_source(&source_byte);

        LPSPI_WriteDataNonBlocking(baseAddr, source_byte);
    }
}

bool lpspi_poll_for_activity(const peripheral_descriptor_t *self)
{
    return s_lpspiActivity[self->instance];
}

void lpspi_data_source(uint8_t *source_byte)
{
    assert(source_byte);

    if (s_lpspiInfo.bytesToTransfer)
    {
        *source_byte = *s_lpspiInfo.writeData++;
        s_lpspiInfo.bytesToTransfer--;
    }
    else
    {
        *source_byte = 0;
    }
}

void lpspi_initial_data_sink(uint8_t sink_byte, uint16_t instance)
{
    if (sink_byte == kFramingPacketStartByte)
    {
        s_lpspiActivity[instance] = true;
        s_lpspiInfo.data_sink = lpspi_data_sink;
        s_lpspi_app_data_sink_callback(sink_byte);
    }
}

void lpspi_data_sink(uint8_t sink_byte, uint16_t instance)
{
    s_lpspi_app_data_sink_callback(sink_byte);
}

status_t lpspi_full_init(const peripheral_descriptor_t *self, serial_byte_receive_func_t function)
{
    s_lpspi_app_data_sink_callback = function;
    uint32_t baseAddr = g_lpspiBaseAddr[self->instance];

    s_lpspiInfo.data_sink = lpspi_initial_data_sink;
    // Init the driver.
    lpspi_slave_config_t config;

    LPSPI_SlaveGetDefaultConfig(&config);

    lpspi_which_pcs_t selectedChip = kLPSPI_Pcs0;

    switch (self->instance)
    {
#if defined(SPI0_SELECTED_PCS)
        case 0:
            selectedChip = kLPSPI_Pcs0;
            break;
#endif // #if defined(SPI0_SELECTED_PCS)
#if defined(SPI1_SELECTED_PCS)
        case 1:
            selectedChip = kLPSPI_Pcs1;
            break;
#endif // #if defined(SPI1_SELECTED_PCS)
#if defined(SPI2_SELECTED_PCS)
        case 2:
            selectedChip = kLPSPI_Pcs2;
            break;
#endif // #if defined(SPI2_SELECTED_PCS)
        default:
            break;
    }

    config.cpha = kLPSPI_ClockPhaseSecondEdge;
    config.cpol = kLPSPI_ClockPolarityActiveLow;
    config.whichPcs = selectedChip;

    // Configure selected pin as lpspi peripheral interface
    self->pinmuxConfig(self->instance, kPinmuxType_Peripheral);

    // Ungate the LPSPI clock.
    LPSPI_SlaveInit((LPSPI_Type *)baseAddr, &config);

    // Enable LPSPI interrupt
    LPSPI_SetSystemIRQ(self->instance, kPeripheralEnableIRQ);
    LPSPI_EnableInterrupts((LPSPI_Type *)baseAddr, kLPSPI_TxInterruptEnable);
    LPSPI_EnableInterrupts((LPSPI_Type *)baseAddr, kLPSPI_RxInterruptEnable);

    s_lpspiIntialized[self->instance] = true;

    return kStatus_Success;
}

void lpspi_full_shutdown(const peripheral_descriptor_t *self)
{
    if (s_lpspiIntialized[self->instance])
    {
        uint32_t baseAddr = g_lpspiBaseAddr[self->instance];

        LPSPI_Deinit((LPSPI_Type *)baseAddr);

        // Restore selected pin to default state to reduce IDD.
        self->pinmuxConfig(self->instance, kPinmuxType_Default);

        s_lpspiIntialized[self->instance] = false;
    }
}

status_t lpspi_write(const peripheral_descriptor_t *self, const uint8_t *buffer, uint32_t byteCount)
{
    s_lpspiInfo.writeData = buffer;
    s_lpspiInfo.bytesToTransfer = byteCount;

    while (s_lpspiInfo.bytesToTransfer)
        ;

    return kStatus_Success;
}

//! @}

#endif // BL_CONFIG_LPSPI

////////////////////////////////////////////////////////////////////////////////
// EOF
////////////////////////////////////////////////////////////////////////////////
