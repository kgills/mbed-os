/*
 * Copyright (c) 2013 - 2014, Freescale Semiconductor, Inc.
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

#include "fsl_i2c_slave_driver.h"
#include "fsl_device_registers.h"
#include "fsl_i2c_hal.h"
#include "fsl_i2c_shared_irqs.h"
#include "fsl_clock_manager.h"
#include "fsl_interrupt_manager.h"
#include <assert.h>
#include <string.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/

#define I2C_EMPTY_CHAR    (0x00)        /*!< Empty character.*/

/*******************************************************************************
 * Variables
 ******************************************************************************/

/*! Place to store application callbacks for each of the I2C modules.*/
static i2c_slave_info_t s_applicationInfo[2] = {{0}};

extern IRQn_Type i2c_irq_ids[HW_I2C_INSTANCE_COUNT];

/*******************************************************************************
 * Code
 ******************************************************************************/

/*!
 * @brief I2C Slave Generic IRQ handler.
 *
 * This handler implements the flow chart at the end of the I2C chapter in the Kinetis
 * KL25 Sub-Family Reference Manual. It uses callbacks to get/put data
 * from/to the application as well as alert the application of an error condition.
 *
 * @param instance Instance number of the I2C module.
 */
void i2c_slave_irq_handler(uint32_t instance)
{
    assert(instance < HW_I2C_INSTANCE_COUNT);

    i2c_slave_info_t * appInfo = &s_applicationInfo[instance];
    
    bool doTransmit = false;
    i2c_status_t error = kStatus_I2C_Success;

    /* Clear I2C IRQ.*/
    i2c_hal_clear_interrupt(instance);
    
    bool wasArbLost = i2c_hal_was_arbitration_lost(instance);
    if (wasArbLost)
    {
        i2c_hal_clear_arbitration_lost(instance);
    }
    
    bool addressedAsSlave = i2c_hal_is_addressed_as_slave(instance);

    /* Make sure device is in slave mode, exit if not.*/
    if (i2c_hal_is_master(instance))
    {
        return;
    }
    
    /* Device is in slave mode.*/
    if (wasArbLost && (!addressedAsSlave))
    {
        /* I2C_S_ARBL is already cleared by ClearInterruptFlags()*/
        error = kStatus_I2C_AribtrationLost;
    }
    else if (addressedAsSlave) /* AddressedAsSlave*/
    {
        if (i2c_hal_get_slave_direction(instance) == kI2CTransmit) /* Master read from Slave. Slave transmit.*/
        {
            /* Switch to TX mode*/
            i2c_hal_set_direction(instance, kI2CTransmit);

            doTransmit = true;
        }
        else /* Master write to Slave. Slave receive.*/
        {
            /* Switch to RX mode.*/
            i2c_hal_set_direction(instance, kI2CReceive);

            /* Read dummy character.*/
            i2c_hal_read(instance);
        }
    }
    else /* not AddressedAsSlave*/
    {
        if (i2c_hal_get_direction(instance) == kI2CTransmit)
        {
            if (!(i2c_hal_get_receive_ack(instance)))
            {
                /* Switch to RX mode.*/
                i2c_hal_set_direction(instance, kI2CReceive);

                /* Read dummy character.*/
                i2c_hal_read(instance);
            }
            else /* ACK from receiver.*/
            {
                /* DO TRANSMIT*/
                doTransmit = true;
            }
        }
        else /* Receive.*/
        {
            /* DO RECEIVE*/

            /* Get byte from data register.*/
            uint8_t sink_byte = i2c_hal_read(instance);

            if (appInfo->data_sink)
            {
                if (appInfo->data_sink(sink_byte) != kStatus_I2C_Success)
                {
                    /* Report the Slave RX Overrun error.*/
                    error = kStatus_I2C_SlaveRxOverrun;
                }
            }
        }
    }
    
    /* DO TRANSMIT*/
    if (doTransmit)
    {
        uint8_t source_byte = I2C_EMPTY_CHAR;
        if (appInfo->data_source)
        {
            error = appInfo->data_source(&source_byte);
        }
        
        i2c_hal_write(instance, source_byte);
    }

    if ((error != kStatus_I2C_Success) && (appInfo->on_error))
    {
        appInfo->on_error(error);
    }
}

/*FUNCTION**********************************************************************
 *
 * Function Name : i2c_slave_init
 * Description   : initializes the I2C module.
 * This function will save the application callback info, turn on the clock to the
 * module, enable the device and enable interrupts. Set the I2C to slave mode. 
 *
 *END**************************************************************************/
void i2c_slave_init(uint32_t instance, i2c_slave_info_t * appInfo)
{
    assert(appInfo);
    assert(instance < HW_I2C_INSTANCE_COUNT);

    /* Save the application info.*/
    s_applicationInfo[instance] = *appInfo;

    /* Enable clock for I2C.*/
    clock_manager_set_gate(kClockModuleI2C, instance, true);
    
    /* Enable max rate slave baud.*/
    i2c_hal_set_independent_slave_baud(instance, true);
    
    /* Set slave address.*/
    i2c_hal_set_slave_address_7bit(instance, appInfo->slaveAddress);

    /* Disable and clear the interrupt before enabling it in the NVIC.*/
    i2c_hal_disable_interrupt(instance);
    i2c_hal_clear_interrupt(instance);
    
    /* Indicate to shared I2C IRQ manager that we are using this instance in slave mode.*/
    i2c_set_shared_irq_is_master(instance, false);
    i2c_set_shared_irq_state(instance, appInfo);
    
    /* Enable I2C interrupt.*/
    interrupt_enable(i2c_irq_ids[instance]);
    
    /* Now enable the I2C interrupt in the peripheral.*/
    i2c_hal_enable_interrupt(instance);

    /* Enable the peripheral operation.*/
    i2c_hal_enable(instance);
}

/*FUNCTION**********************************************************************
 *
 * Function Name : i2c_slave_shutdown
 * Description   : Shuts down the I2C slave driver.
 * This function will clear the control register and turn off the clock to the module. 
 *
 *END**************************************************************************/
void i2c_slave_shutdown(uint32_t instance)
{
    assert(instance < HW_I2C_INSTANCE_COUNT);
    
    /* Turn off I2C.*/
    i2c_hal_disable(instance);

    /* Disable the interrupt */
    interrupt_disable(i2c_irq_ids[instance]);
    
    /* Disable clock for I2C.*/
    clock_manager_set_gate(kClockModuleI2C, instance, false);
}

/*******************************************************************************
 * EOF
 ******************************************************************************/

