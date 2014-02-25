#include "stm32f30x.h"
#include "stm32f3_discovery.h"


//sum of TIM_IT_CC1 .. TIM_IT_CC4
#define ALL_TIM_CHANNELS  ((uint16_t)0x1E)


#pragma mark - Private variables 

RCC_ClocksTypeDef RCC_Clocks;
__IO uint32_t __timingDelay = 0;
__IO uint32_t __userButtonPressed = 0;
__IO uint32_t i = 0;
__IO uint32_t __dataAvailable = 0;

__IO uint32_t __captureCounters[4];
__IO uint32_t __risingEdges[4];
__IO uint32_t __fallingEdges[4];
__IO uint32_t __pulseWidths[4];


#pragma mark - Private function prototypes

void TimingDelay_Decrement(void);
void Delay(__IO uint32_t nTime);

static void DAC_Config(void);
static void COMP_Config(void);
static void TIM_Config(void);

static void clear_leds(void);
static void spin_leds(int gap);
static void flash_leds(int gap);
static void illuminate_led_count(int count);
static void illuminate_four_way_leds(int index);
static void watch_input_captures(void);
static void config_one_comparator(GPIO_TypeDef* gpioPort, uint32_t gpioPin, uint32_t compSelection, uint32_t compOutput);
static void config_one_timer_channel(TIM_TypeDef* timer, uint16_t channel);
static void handle_one_capture_channel(TIM_TypeDef* timer, uint16_t channel);

#pragma mark - IRQ handlers

/**
  * @brief  This function handles SysTick Handler.
  * @param  None
  * @retval None
  */
void SysTick_Handler(void)
{
  TimingDelay_Decrement();
}

void EXTI0_IRQHandler(void)
{
    if ((EXTI_GetITStatus(USER_BUTTON_EXTI_LINE) == SET)&&(STM_EVAL_PBGetState(BUTTON_USER) != RESET))
    {
        /* Delay */
        for(i=0; i<0x7FFFF; i++);
        
        /* Wait for SEL button to be pressed  */
        while(STM_EVAL_PBGetState(BUTTON_USER) != RESET);
        /* Delay */
        for(i=0; i<0x7FFFF; i++);
        __userButtonPressed++;
        
        if (__userButtonPressed > 0x02) {
            __userButtonPressed = 0x00;
        }
        
        /* Clear the EXTI line pending bit */
        EXTI_ClearITPendingBit(USER_BUTTON_EXTI_LINE);
    }
}


/**
 If the interrupt status flag is set for this timer and channel,
 record the value of the channel
 
 @param timer eg TIM2
 @param channel TIM_IT pending bit ie TIM_IT_CC1 - TIM_IT_CC4
 */
void handle_one_capture_channel(TIM_TypeDef* timer, uint16_t channel)
{
    uint32_t captureVal;
    uint16_t idx;
    
    if (SET != TIM_GetITStatus(timer, channel) ) {
        //the interrupt for this channel hasn't happened yet
        return;
    }
    
    switch(channel)
    {
        case TIM_IT_CC4:
            captureVal = TIM_GetCapture4(timer);
            idx = 3;
            break;
            
        case TIM_IT_CC3:
            captureVal = TIM_GetCapture3(timer);
            idx = 2;
            break;
            
        case TIM_IT_CC2:
            captureVal = TIM_GetCapture2(timer);
            idx = 1;
            break;
            
        case TIM_IT_CC1:
        default:
            captureVal = TIM_GetCapture1(timer);
            idx = 0;
            break;
    }
    
    
    if (0 == __captureCounters[idx]) {
        //we haven't seen a rising edge yet
        __captureCounters[idx] = 1; //now we have
        __risingEdges[idx] = captureVal;
    }
    else {
        //we've seen rising edge already: this is the falling edge
        uint32_t pulseWidth = 0;
        __captureCounters[idx] = 0;
        __fallingEdges[idx] = captureVal;
        
        if (__fallingEdges[idx] > __risingEdges[idx])  {
            // the rising and falling edges were detected before timer wrapped
            pulseWidth = __fallingEdges[idx] - __risingEdges[idx] - 1 ;
        }
        else {
            //the timer wrapped before falling edge was detected
            pulseWidth = ((0xFFFF - __risingEdges[idx]) + __fallingEdges[idx]) - 1;
        }
        
        //TODO check the pulseWidth to ensure it's something reasonable
        __pulseWidths[idx] = pulseWidth;
        
        //this channel now has a valid pulsewidth read
        __dataAvailable += channel;
    }
    
    TIM_ClearITPendingBit(timer, channel);

}

/**
 * @brief  This function handles TIM2 global interrupt.
 * @param  None
 * @retval None
 */
void TIM2_IRQHandler(void)
{
//    if (TIM_GetITStatus(TIM2, TIM_IT_CC4) == SET) {
//        
//        uint16_t icVal = TIM_GetCapture4(TIM2);
//
//        if (0 == __captureCounter)  {
//            IC4Value1 = icVal;
//            __captureCounter = 1;
//        }
//        else if (1 == __captureCounter) {
//            __captureCounter = 0;
//            IC4Value2 = icVal;
//            
//            if (IC4Value2 > IC4Value1) {
//                __captureVal = (IC4Value2 - IC4Value1) - 1;
//            }
//            else {
//                __captureVal = ((0xFFFF - IC4Value1) + IC4Value2) - 1;
//            }
//            __dataAvailable = 1;
//        }
//        TIM_ClearITPendingBit(TIM2, TIM_IT_CC4);
//    }


    handle_one_capture_channel(TIM2, TIM_IT_CC1);
    handle_one_capture_channel(TIM2, TIM_IT_CC2);
    handle_one_capture_channel(TIM2, TIM_IT_CC3);
    handle_one_capture_channel(TIM2, TIM_IT_CC4);

}



#pragma mark - Configure Peripherals


/*
 Configure a comparator to take input from a pin and output to a specific TIM channel
 
 @param gpioPin The pin number to assign, ie GPIO_Pin_1 - n
 @param compSelection The comparator to select, ie COMP_Selection_COMP1 - COMP_Selection_COMP7
 @param compOutput The redirect ie COMP_Output_TIM2IC1 - COMP_Output_TIM2IC4
 */
void config_one_comparator(GPIO_TypeDef* gpioPort, uint32_t gpioPin, uint32_t compSelection, uint32_t compOutput)
{
    COMP_InitTypeDef compInit;
    GPIO_InitTypeDef gpioInit;
    
    /* Init GPIO Init Structure */
    GPIO_StructInit(&gpioInit);
    /* Configure GPIO pin to be used as comparator non-inverting input */
    gpioInit.GPIO_Pin = gpioPin;
    gpioInit.GPIO_Mode = GPIO_Mode_AN; /*!< GPIO Analog Mode */
    gpioInit.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOA, &gpioInit);
    
    /* Clear compInit struct */
    COMP_StructInit(&compInit);
    /* GPIO pin is used as selected comparator non-inverting input */
    compInit.COMP_NonInvertingInput = COMP_NonInvertingInput_IO1;
    /* DAC1 output is as used COMP1 inverting input, providing threshold */
    compInit.COMP_InvertingInput = COMP_InvertingInput_DAC1;
    /* Redirect selected comparator output */
    compInit.COMP_Output = compOutput;
    compInit.COMP_OutputPol = COMP_OutputPol_NonInverted;
    compInit.COMP_BlankingSrce = COMP_BlankingSrce_None;
    compInit.COMP_Hysteresis = COMP_Hysteresis_High;
    compInit.COMP_Mode = COMP_Mode_UltraLowPower;
    COMP_Init(compSelection, &compInit);
    
    /* Enable selected comparator */
    COMP_Cmd(compSelection, ENABLE);

}

/**
 * @brief  Configures the DAC channel 1 with output buffer enabled.
 * @param  None
 * @retval None
 */
static void DAC_Config(void)
{
    /* Init Structure definition */
    DAC_InitTypeDef  dacInit;
    
    /* DAC clock enable */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_DAC, ENABLE);
    
    /* Fill DAC init struct */
    dacInit.DAC_Trigger = DAC_Trigger_None;
    dacInit.DAC_WaveGeneration = DAC_WaveGeneration_None;
    dacInit.DAC_LFSRUnmask_TriangleAmplitude = DAC_LFSRUnmask_Bit0;
    dacInit.DAC_OutputBuffer = DAC_OutputBuffer_Enable;
    DAC_Init(DAC_Channel_1, &dacInit);
    
    /* Enable DAC Channel1 */
    DAC_Cmd(DAC_Channel_1, ENABLE);
    
    /*
    Set DAC Channel1 DHR register: DAC_OUT1
    n = (Vref / 3.3 V) * 4095
    eg   n = (2V V / 3.3 V) * 4095 = 2482
    */
    DAC_SetChannel1Data(DAC_Align_12b_R, 2482);
}


/**
 * @brief  Configures COMP1: DAC channel 1 to COMP1 inverting input
 *                           and COMP1 output to TIM2 IC4.
 * @param  None
 * @retval None
 */
static void COMP_Config(void)
{
    /* Init Structure definition */
    COMP_InitTypeDef compInit;
    GPIO_InitTypeDef gpioInit;
    
    /* GPIOA Peripheral clock enable */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOA, ENABLE);
    /* GPIOB Peripheral clock enable */
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOB, ENABLE);
    /* COMP Peripheral clock enable */
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SYSCFG, ENABLE);

//    /* Init GPIO Init Structure */
//    GPIO_StructInit(&gpioInit);
//    /* Configure PA1: PA1 is used as COMP1 non-inverting input */
//    gpioInit.GPIO_Pin = GPIO_Pin_1;
//    gpioInit.GPIO_Mode = GPIO_Mode_AN; /*!< GPIO Analog Mode */
//    gpioInit.GPIO_PuPd = GPIO_PuPd_NOPULL;
//    GPIO_Init(GPIOA, &gpioInit);
//    
//    /* Clear compInit struct */
//    COMP_StructInit(&compInit);
//    /* COMP1 Init: PA1 is used as COMP1 non-inverting input */
//    compInit.COMP_NonInvertingInput = COMP_NonInvertingInput_IO1;
//    /* DAC1 output is as used COMP1 inverting input */
//    compInit.COMP_InvertingInput = COMP_InvertingInput_DAC1;
//    /* Redirect COMP1 output to TIM2 Input capture 4 */
//    compInit.COMP_Output = COMP_Output_TIM2IC4;
//    compInit.COMP_OutputPol = COMP_OutputPol_NonInverted;
//    compInit.COMP_BlankingSrce = COMP_BlankingSrce_None;
//    compInit.COMP_Hysteresis = COMP_Hysteresis_High;
//    compInit.COMP_Mode = COMP_Mode_UltraLowPower;
//    COMP_Init(COMP_Selection_COMP1, &compInit);
//    
//    /* Enable COMP1 */
//    COMP_Cmd(COMP_Selection_COMP1, ENABLE);
    
    
    
    //(PA1 for COMP1, PA7 for COMP2, PB14 for COMP3, PB0 for COMP4)
//    config_one_comparator(uint32_t gpioPort, uint32_t gpioPin, uint32_t compSelection, uint32_t compOutput)
    config_one_comparator(GPIOA, GPIO_Pin_1, COMP_Selection_COMP1, COMP_Output_TIM2IC1);
    config_one_comparator(GPIOA, GPIO_Pin_7, COMP_Selection_COMP2, COMP_Output_TIM2IC2);
    config_one_comparator(GPIOB, GPIO_Pin_14, COMP_Selection_COMP3, COMP_Output_TIM2IC3);
    config_one_comparator(GPIOB, GPIO_Pin_0, COMP_Selection_COMP4, COMP_Output_TIM2IC4);


}


/**
 @param timer TIM1-n
 @param channel eg TIM_Channel_4
 */
void config_one_timer_channel(TIM_TypeDef* timer, uint16_t channel)
{
    TIM_ICInitTypeDef tim_ic_init;

    /* TIM2 Channel4 Input capture Mode configuration */
    TIM_ICStructInit(&tim_ic_init);
    tim_ic_init.TIM_Channel = channel;
    /* TIM2 counter is captured at each transition detection: rising or falling edges (both edges) */
    tim_ic_init.TIM_ICPolarity = TIM_ICPolarity_BothEdge;
    tim_ic_init.TIM_ICSelection = TIM_ICSelection_DirectTI;
    tim_ic_init.TIM_ICPrescaler = TIM_ICPSC_DIV1;
    tim_ic_init.TIM_ICFilter = 0;
    TIM_ICInit(timer, &tim_ic_init);
}

/**
 * @brief  Configures TIM2 channel 4 in input capture (IC) mode (TIM2IC4)
 * @param  None
 * @retval None
 */
static void TIM_Config(void)
{
    /* Init Structure definition */
    TIM_ICInitTypeDef tim_ic_init;
    TIM_TimeBaseInitTypeDef tim_timebase;
    NVIC_InitTypeDef nvicInit;
    
    /* TIM2 clock enable */
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    
    /* TIM2 Time base configuration */
    TIM_TimeBaseStructInit(&tim_timebase);
    tim_timebase.TIM_Prescaler = 0;
    tim_timebase.TIM_CounterMode = TIM_CounterMode_Up;
    tim_timebase.TIM_Period = 65535;
    tim_timebase.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(TIM2, &tim_timebase);
    
    TIM_ClearFlag(TIM2, TIM_FLAG_Update);
    
//    /* TIM2 Channel4 Input capture Mode configuration */
//    TIM_ICStructInit(&tim_ic_init);
//    tim_ic_init.TIM_Channel = TIM_Channel_4;
//    /* TIM2 counter is captured at each transition detection: rising or falling edges (both edges) */
//    tim_ic_init.TIM_ICPolarity = TIM_ICPolarity_BothEdge;
//    tim_ic_init.TIM_ICSelection = TIM_ICSelection_DirectTI;
//    tim_ic_init.TIM_ICPrescaler = TIM_ICPSC_DIV1;
//    tim_ic_init.TIM_ICFilter = 0;
//    TIM_ICInit(TIM2, &tim_ic_init);
    
    //config_one_timer_channel(uint32_t timer, uint32_t channel)
    config_one_timer_channel(TIM2, TIM_Channel_1);
    config_one_timer_channel(TIM2, TIM_Channel_2);
    config_one_timer_channel(TIM2, TIM_Channel_3);
    config_one_timer_channel(TIM2, TIM_Channel_4);
    
    /* TIM2 IRQChannel enable */
    nvicInit.NVIC_IRQChannel = TIM2_IRQn;
    nvicInit.NVIC_IRQChannelPreemptionPriority = 0;
    nvicInit.NVIC_IRQChannelSubPriority = 0;
    nvicInit.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvicInit);
    
    /* Enable capture interrupt */
    TIM_ITConfig(TIM2, TIM_IT_CC1, ENABLE);
    TIM_ITConfig(TIM2, TIM_IT_CC2, ENABLE);
    TIM_ITConfig(TIM2, TIM_IT_CC3, ENABLE);
    TIM_ITConfig(TIM2, TIM_IT_CC4, ENABLE);
    
    /* Enable the TIM2 counter */
    TIM_Cmd(TIM2, ENABLE);
    
    /* Reset the flags */
    TIM2->SR = 0;
}



/**
 * @brief  Configure the TIM IRQ Handler.
 * @param  None
 * @retval None
 */
//static void TIM_Config(void)
//{
//    GPIO_InitTypeDef gpioInit;
//
//    /* TIM2 clock enable */
//    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
//
//    /* GPIOA clock enable */
//    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_GPIOA, ENABLE);
//
//    /* TIM2_CH1 pin (PA.00) and TIM2_CH2 pin (PA.01) configuration */
//    gpioInit.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1;
//    gpioInit.GPIO_Mode = GPIO_Mode_AF;
//    gpioInit.GPIO_Speed = GPIO_Speed_50MHz;
//    gpioInit.GPIO_OType = GPIO_OType_PP;
//    gpioInit.GPIO_PuPd = GPIO_PuPd_NOPULL;
//    GPIO_Init(GPIOA, &gpioInit); //assign to PA00 + PA01
//
//    /* Connect TIM pins to AF1 */
//    GPIO_PinAFConfig(GPIOA, GPIO_PinSource0, GPIO_AF_1);
//    GPIO_PinAFConfig(GPIOA, GPIO_PinSource1, GPIO_AF_1);
//}


/* Private functions ---------------------------------------------------------*/
#pragma mark - Private functions

void clear_leds()
{
    STM_EVAL_LEDOff(LED3);
    STM_EVAL_LEDOff(LED6);
    STM_EVAL_LEDOff(LED7);
    STM_EVAL_LEDOff(LED4);
    STM_EVAL_LEDOff(LED10);
    STM_EVAL_LEDOff(LED8);
    STM_EVAL_LEDOff(LED9);
    STM_EVAL_LEDOff(LED5);
}


void leds_on()
{
    STM_EVAL_LEDOn(LED3);
    STM_EVAL_LEDOn(LED6);
    STM_EVAL_LEDOn(LED7);
    STM_EVAL_LEDOn(LED4);
    STM_EVAL_LEDOn(LED10);
    STM_EVAL_LEDOn(LED8);
    STM_EVAL_LEDOn(LED9);
    STM_EVAL_LEDOn(LED5);
}


void illuminate_led_count(int count)
{
    clear_leds();
    
    switch (count) {
        case 8:
            STM_EVAL_LEDOn(LED4);
        case 7:
            STM_EVAL_LEDOn(LED6);
        case 6:
            STM_EVAL_LEDOn(LED8);
        case 5:
            STM_EVAL_LEDOn(LED10);
        case 4:
            STM_EVAL_LEDOn(LED9);
        case 3:
            STM_EVAL_LEDOn(LED7);
        case 2:
            STM_EVAL_LEDOn(LED5);
        case 1:
            STM_EVAL_LEDOn(LED3);
            break;
    }
    
    Delay(50);
}

void illuminate_four_way_leds(int index)
{
    clear_leds();
    
    switch (index) {
        case 3:
            STM_EVAL_LEDOn(LED6);
            break;
        case 2:
            STM_EVAL_LEDOn(LED10);
            break;
        case 1:
            STM_EVAL_LEDOn(LED7);
            break;
        case 0:
            STM_EVAL_LEDOn(LED3);
            break;
    }
    
    Delay(50);
}


void spin_leds(int gap)
{
    /* Toggle LD3 */
    STM_EVAL_LEDToggle(LED3);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD5 */
    STM_EVAL_LEDToggle(LED5);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD7 */
    STM_EVAL_LEDToggle(LED7);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD9 */
    STM_EVAL_LEDToggle(LED9);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD10 */
    STM_EVAL_LEDToggle(LED10);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD8 */
    STM_EVAL_LEDToggle(LED8);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD6 */
    STM_EVAL_LEDToggle(LED6);
    /* Insert 50 ms delay */
    Delay(gap);
    /* Toggle LD4 */
    STM_EVAL_LEDToggle(LED4);
    /* Insert 50 ms delay */
    Delay(gap);
}

void flash_leds(int gap)
{
    clear_leds();
    Delay(gap); /*500ms - half second*/
    
    leds_on();
    Delay(gap); /*500ms - half second*/
}

void watch_input_captures()
{
    //wait for data available on all channels
    if (ALL_TIM_CHANNELS == __dataAvailable) {
        uint16_t maxIdx;
        uint32_t maxPulseWidth = 0;
        for (i = 0; i < 4; i++) {
            if (__pulseWidths[i] > maxPulseWidth) {
                maxPulseWidth = __pulseWidths[i];
                maxIdx = i;
            }
        }
        
        /* Compute the pulse width in us */
//        __measuredPulse = (uint32_t)(((uint64_t) minPulse * 1000000) / ((uint32_t)SystemCoreClock));
        
        //show which comparator has the max delay time
        illuminate_four_way_leds(maxIdx);
        
        __dataAvailable = 0;
    }
    
}


void setup(void)
{
    for (i = 0; i < 4; i++) {
        __captureCounters[i] = 0;
        __risingEdges[i] = 0;
        __fallingEdges[i] = 0;
        __pulseWidths[i] = 0;
    }
    
    
    /* SysTick end of count event each 10ms */
    RCC_GetClocksFreq(&RCC_Clocks);
    SysTick_Config(RCC_Clocks.HCLK_Frequency / 200); // div 100 normally?
    
    /* Initialize LEDs and User Button available on STM32F3-Discovery board */
    STM_EVAL_LEDInit(LED3);
    STM_EVAL_LEDInit(LED4);
    STM_EVAL_LEDInit(LED5);
    STM_EVAL_LEDInit(LED6);
    STM_EVAL_LEDInit(LED7);
    STM_EVAL_LEDInit(LED8);
    STM_EVAL_LEDInit(LED9);
    STM_EVAL_LEDInit(LED10);
    
    
    /* DAC Ch1 configuration */
    DAC_Config();
    
    /* COMP Configuration */
    COMP_Config();
    
    /* TIM2 Configuration in input capture mode */
    TIM_Config();
    
    
    STM_EVAL_PBInit(BUTTON_USER, BUTTON_MODE_EXTI);
    
    /* Configure the USB */
    //    Demo_USB();
}
/**
  * @brief  Main program.
  * @param  None
  * @retval None
  */
int main(void)
{  

    setup();
    
    //user button selects one of multiple modes that run forever
    while (1) {
        __userButtonPressed = 0x00;
        
        while (0x00 == __userButtonPressed) {
            spin_leds(5);
        }
        
        while (0x01 == __userButtonPressed) {
            watch_input_captures();
        }
        
        while (0x02 == __userButtonPressed) {
            flash_leds(5);
        }
    }
    
}
/**
  * @brief  Inserts a delay time.
  * @param  nTime: specifies the delay time length, in 10 ms.
  * @retval None
  */
void Delay(__IO uint32_t nTime)
{
  __timingDelay = nTime;

  while(__timingDelay != 0);
}

/**
  * @brief  Decrements the __timingDelay variable.
  * @param  None
  * @retval None
  */
void TimingDelay_Decrement(void)
{
  if (__timingDelay != 0x00)
  { 
    __timingDelay--;
  }
}

#ifdef  USE_FULL_ASSERT

/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t* file, uint32_t line)
{ 
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */

  /* Infinite loop */
  while (1)
  {
  }
}
#endif
