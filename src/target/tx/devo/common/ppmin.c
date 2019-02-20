/*
 This project is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Deviation is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Deviation.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/exti.h>

#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/iwdg.h>
 
#include "common.h"
#include "devo.h"
//#include "interface.h"
#include "mixer.h"
#include "config/model.h"

#define PPMIn_prescaler 35    // 72MHz /(35+1) = 2MHz = 0.5uSecond
#define PPMIn_period 65535  // max value of u16

/*
(1) use TIM1 : set the unit same as "ppmout.c" for count the ppm-input signal, "uSecond (72MHz / 36) = 2MHz = 0.5uSecond"
(2) set GPIO PA10 input mode as external trigger 
(3) set GPIO PA10 into source(EXTI10) interrupt(NVIC_EXTI15_10_IRQ) to trigger-call "exti15_10_isr()" function
(4) transfer value for mixer.c   
    (4.1) "ppmin_num_channels"
    (4.2) "Channels[i]" or "raw[i+1]" : each channel value (volatile s32 Channels[NUM_OUT_CHANNELS];)
          Channels[i]
                      = ((ppmChannels[i]*2MHz:uSecond - 1.5mSecond)/(1.0mSecond))*(CHAN_MAX_VALUE-CHAN_MIN_VALUE)
                      = (ppmChannels[i]-3000)*10
    (4.3) "ppmSync"
*/

void PPMin_TIM_Init()
{
    /* Enable TIM1 clock. */
    rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_TIM1EN);
  
    /* No Enable TIM1 interrupt. */
    // nvic_enable_irq(NVIC_TIM1_IRQ);
    // nvic_set_priority(NVIC_TIM1_IRQ, 16); //High priority

    /* Reset TIM1 peripheral. */
    timer_disable_counter(TIM1);
    rcc_periph_reset_pulse(RST_TIM1);

    /* Timer global mode:
     * - No divider
     * - Alignment edge
     * - Direction up
     */
    timer_set_mode(TIM1, TIM_CR1_CKD_CK_INT,
               TIM_CR1_CMS_EDGE, TIM_CR1_DIR_UP);

       /* Reset prescaler value.  timer updates each uSecond */
    timer_set_prescaler(TIM1, PPMIn_prescaler);     // uSecond (72MHz / (35+1) = 2MHz = 0.5uSecond
    timer_set_period(TIM1, PPMIn_period);           // 3300uSecond= 2MHz*6600times,  TIM1_prescaler=0.5uSecond

    /* Disable preload. */
    timer_disable_preload(TIM1);

    /* Continous mode. */
    timer_continuous_mode(TIM1);

    /* Disable outputs. */
    timer_disable_oc_output(TIM1, TIM_OC1);
    timer_disable_oc_output(TIM1, TIM_OC2);
    timer_disable_oc_output(TIM1, TIM_OC3);
    timer_disable_oc_output(TIM1, TIM_OC4);

    /* -- OC1 configuration -- */
    /* Configure global mode of line 1. */
    /* Enable CCP1 */
    timer_disable_oc_clear(TIM1, TIM_OC1);
    timer_disable_oc_preload(TIM1, TIM_OC1);
    timer_set_oc_slow_mode(TIM1, TIM_OC1);
    timer_set_oc_mode(TIM1, TIM_OC1, TIM_OCM_FROZEN);

    /* Enable commutation interrupt. */
    //  timer_enable_irq(TIM1, TIM_DIER_CC1IE);
    /* Disable CCP1 interrupt. */
    timer_disable_irq(TIM1, TIM_DIER_CC1IE);

    /* Counter enable. */
    timer_disable_counter(TIM1);
}

void PPMin_Init()
{
#if _PWM_PIN == GPIO_USART1_TX
    UART_Stop();  // disable USART1 for GPIO PA9 & PA10 (Trainer Tx(PA9) & Rx(PA10))
#endif
    /* Enable GPIOA clock. */
    rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_IOPAEN);

    /* Enable AFIO clock. */
    rcc_peripheral_enable_clock(&RCC_APB2ENR, RCC_APB2ENR_AFIOEN);

    /* Enable EXTI interrupt. */
    nvic_enable_irq(NVIC_EXTI9_5_IRQ);

    /* Set GPIO0 (in GPIO port A) to 'input float'. */
    gpio_set_mode(GPIOA, GPIO_MODE_INPUT, GPIO_CNF_INPUT_FLOAT, _PWM_PIN);
    
    /* Configure the EXTI subsystem. */
    exti_select_source(_PWM_EXTI, GPIOA);
    exti_set_trigger(_PWM_EXTI, EXTI_TRIGGER_RISING);
    exti_disable_request(_PWM_EXTI);
}
/* ===get PPM===
(1) capture  ppmSync (for ppm-timing > MIN_PPMin_Sync : 3300uSecond)
(2) count channels and set to  "ppmin_num_channels"  (include Syn-singal)
(3) get  each channel value and set to  "Channels[i]" or "raw[i+1]"
            "Channels[i]" or "raw[i+1]"
                    = ((ppmChannels[i]*2MHz:uSecond - 1.5mSecond)/(1.0mSecond))*(CHAN_MAX_VALUE-CHAN_MIN_VALUE)
                    = (ppmChannels[i]-3000)*10
(4) continue count channels and compare  "num_channels",  if not equal => disconnect(no-Sync) and re-connect (re-Sync)
===get PPM===*/

volatile u8 ppmSync = 0;     //  the ppmSync for mixer.c,  0:ppm-Not-Sync , 1:ppm-Got-Sync
volatile s32 ppmChannels[MAX_PPM_IN_CHANNELS];    //  [0...ppmin_num_channels-1] for each channels width, [ppmin_num_channels] for sync-signal width
volatile u8 ppmin_num_channels;     //  the ppmin_num_channels for mixer.c

void PPMin_Stop()
{
    nvic_disable_irq(NVIC_EXTI9_5_IRQ);
    exti_disable_request(_PWM_EXTI);
    timer_disable_counter(TIM1);
    ppmSync = 0;
}

void PPMin_Start()
{
    if (Model.protocol == PROTOCOL_PPM)
        CLOCK_StopTimer();
    PPMin_Init();
    ppmSync = 0;
    timer_enable_counter(TIM1);
    nvic_enable_irq(NVIC_EXTI9_5_IRQ);
    exti_enable_request(_PWM_EXTI);
    ppmSync = 0;
}