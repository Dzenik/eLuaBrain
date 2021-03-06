// Platform-dependent functions

#include "platform.h"
#include "type.h"
#include "devman.h"
#include "genstd.h"
#include <reent.h>
#include <errno.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include "uip_arp.h"
#include "elua_uip.h"
#include "elua_adc.h"
#include "uip-conf.h"
#include "platform_conf.h"
#include "diskio.h"
#include "common.h"
#include "buf.h"
#include "utils.h"
#include "vram.h"
#include "fsmc_sram.h"
#include "enc28j60.h"

// Platform specific includes
#include "stm32f10x.h"
#include "stm32f10x_exti.h"
#include "stm32f10x_dma.h"
#include "stm32f10x_spi.h"
#include "lua.h"
#include "lauxlib.h"
#include "lrotable.h"

// Clock data
// IMPORTANT: if you change these, make sure to modify RCC_Configuration() too!
#define HCLK        ( HSE_Value * 9 )
#define PCLK1_DIV   2
#define PCLK2_DIV   1

// SysTick Config Data
// NOTE: when using virtual timers, SYSTICKHZ and VTMR_FREQ_HZ should have the
// same value, as they're served by the same timer (the systick)
// Max SysTick preload value is 16777215, for STM32F103RET6 @ 72 MHz, lowest acceptable rate would be about 5 Hz
#define SYSTICKHZ               10
#define SYSTICKMS               (1000 / SYSTICKHZ)
// ****************************************************************************
// Platform initialization

// forward dcls
static void RCC_Configuration(void);
static void NVIC_Configuration(void);

static void timers_init();
static void pwms_init();
static void uarts_init();
static void spis_init();
static void pios_init();
static void adcs_init();
static void cans_init();
static void vram_transfer_init();
static void i2cs_init();
static void eth_init();

static u8 eth_timer_fired;
static u8 eth_initialized;

int platform_init()
{
  // Set the clocking to run from PLL
  RCC_Configuration();

  // Setup IRQ's
  NVIC_Configuration();

  // Setup PIO
  pios_init();

  // Setup UARTs
  uarts_init();
  
  // Setup SPIs
  spis_init();
  
  // Setup timers
  timers_init();
  
  // Setup PWMs
  pwms_init();

#ifdef BUILD_ADC
  // Setup ADCs
  adcs_init();
#endif

  // Setup CANs
  cans_init();

  // Setup I2Cs
  i2cs_init();
  
  // Enable SysTick
  if ( SysTick_Config( HCLK / SYSTICKHZ ) )
  { 
    /* Capture error */ 
    while (1);
  }
  
  FSMC_SRAM_Init();

  cmn_platform_init();

  // Setup the nRF UART
  // NOTE: this should happen BEFORE vram_transfer_init
  platform_uart_setup( NRF24L01_UART_ID, 115200, 8, PLATFORM_UART_PARITY_NONE, PLATFORM_UART_STOPBITS_1 );
  platform_uart_set_flow_control( NRF24L01_UART_ID, PLATFORM_UART_FLOW_RTS | PLATFORM_UART_FLOW_CTS );
  platform_uart_set_buffer( NRF24L01_UART_ID, NRF24L01_BUF_SIZE );

#ifdef BUILD_VRAM
  vram_transfer_init();
#endif  

  eth_init();

  // All done
  return PLATFORM_OK;
}

// ****************************************************************************
// Clocks
// Shared by all STM32 devices.
// TODO: Fix to handle different crystal frequencies and CPU frequencies.

/*******************************************************************************
* Function Name  : RCC_Configuration
* Description    : Configures the different system clocks.
* Input          : None
* Output         : None
* Return         : None
*******************************************************************************/
static void RCC_Configuration(void)
{
  SystemInit();
  
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_AFIO, ENABLE);
}

// ****************************************************************************
// NVIC
// Shared by all STM32 devices.

/*******************************************************************************
* Function Name  : NVIC_Configuration
* Description    : Configures the nested vectored interrupt controller.
* Input          : None
* Output         : None
* Return         : None
*******************************************************************************/
/* This struct is used for later reconfiguration of ADC interrupt */
NVIC_InitTypeDef nvic_init_structure_adc;

static void NVIC_Configuration(void)
{
  NVIC_InitTypeDef nvic_init_structure;
  
#ifdef  VECT_TAB_RAM
  /* Set the Vector Table base location at 0x20000000 */
  NVIC_SetVectorTable(NVIC_VectTab_RAM, 0x0);
#else  /* VECT_TAB_FLASH  */
  /* Set the Vector Table base location at 0x08000000 */
  NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0x0);
#endif

  /* Configure the NVIC Preemption Priority Bits */
  NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);

#ifdef BUILD_ADC  
  nvic_init_structure_adc.NVIC_IRQChannel = DMA1_Channel1_IRQn; 
  nvic_init_structure_adc.NVIC_IRQChannelPreemptionPriority = 0; 
  nvic_init_structure_adc.NVIC_IRQChannelSubPriority = 2; 
  nvic_init_structure_adc.NVIC_IRQChannelCmd = DISABLE; 
  NVIC_Init(&nvic_init_structure_adc);
#endif
}

// ****************************************************************************
// PIO
// This is pretty much common code to all STM32 devices.
// todo: Needs updates to support different processor lines.
static GPIO_TypeDef * const pio_port[] = { GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG };
static const u32 pio_port_clk[]        = { RCC_APB2Periph_GPIOA, RCC_APB2Periph_GPIOB, RCC_APB2Periph_GPIOC, RCC_APB2Periph_GPIOD, RCC_APB2Periph_GPIOE, RCC_APB2Periph_GPIOF, RCC_APB2Periph_GPIOG };
const u32 exti_line[] = { EXTI_Line0, EXTI_Line1,  EXTI_Line2,  EXTI_Line3,  EXTI_Line4,  EXTI_Line5,  EXTI_Line6,  EXTI_Line7, 
                          EXTI_Line8, EXTI_Line9, EXTI_Line10, EXTI_Line11, EXTI_Line12, EXTI_Line13, EXTI_Line14, EXTI_Line15 };

static void pios_init()
{
  GPIO_InitTypeDef GPIO_InitStructure;
  int port;

  for( port = 0; port < NUM_PIO; port++ )
  {
    // Enable clock to port.
    RCC_APB2PeriphClockCmd(pio_port_clk[port], ENABLE);

    // Default all port pins to input and enable port.
    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_All;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;

    GPIO_Init(pio_port[port], &GPIO_InitStructure);
  }

  // Get back lines PA13 and PA15
  GPIO_PinRemapConfig( GPIO_Remap_SWJ_Disable, ENABLE );
}

pio_type platform_pio_op( unsigned port, pio_type pinmask, int op )
{
  pio_type retval = 1;
  GPIO_InitTypeDef GPIO_InitStructure;
  GPIO_TypeDef * base = pio_port[ port ];

  switch( op )
  {
    case PLATFORM_IO_PORT_SET_VALUE:
      GPIO_Write(base, pinmask);
      break;

    case PLATFORM_IO_PIN_SET:
      GPIO_SetBits(base, pinmask);
      break;

    case PLATFORM_IO_PIN_CLEAR:
      GPIO_ResetBits(base, pinmask);
      break;

    case PLATFORM_IO_PORT_DIR_INPUT:
      pinmask = GPIO_Pin_All;
    case PLATFORM_IO_PIN_DIR_INPUT:
      GPIO_InitStructure.GPIO_Pin  = pinmask;
      GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;

      GPIO_Init(base, &GPIO_InitStructure);
      break;

    case PLATFORM_IO_PORT_DIR_OUTPUT:
      pinmask = GPIO_Pin_All;
    case PLATFORM_IO_PIN_DIR_OUTPUT:
      GPIO_InitStructure.GPIO_Pin   = pinmask;
      GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;
      GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;

      GPIO_Init(base, &GPIO_InitStructure);
      break;

    case PLATFORM_IO_PORT_GET_VALUE:
      retval = pinmask == PLATFORM_IO_READ_IN_MASK ? GPIO_ReadInputData(base) : GPIO_ReadOutputData(base);
      break;

    case PLATFORM_IO_PIN_GET:
      retval = GPIO_ReadInputDataBit(base, pinmask);
      break;

    case PLATFORM_IO_PIN_PULLUP:
      GPIO_InitStructure.GPIO_Pin   = pinmask;
      GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPU;

      GPIO_Init(base, &GPIO_InitStructure);
      break;

    case PLATFORM_IO_PIN_PULLDOWN:
      GPIO_InitStructure.GPIO_Pin   = pinmask;
      GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IPD;

      GPIO_Init(base, &GPIO_InitStructure);
      break;

    case PLATFORM_IO_PIN_NOPULL:
      GPIO_InitStructure.GPIO_Pin   = pinmask;
      GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_IN_FLOATING;

      GPIO_Init(base, &GPIO_InitStructure);
      break;

    default:
      retval = 0;
      break;
  }
  return retval;
}

// ****************************************************************************
// CAN
// TODO: Many things

void cans_init( void )
{
  // Remap CAN to PB8/9
  GPIO_PinRemapConfig( GPIO_Remap1_CAN1, ENABLE );

  // CAN Periph clock enable
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_CAN1, ENABLE);
}

/*       BS1 BS2 SJW Pre
1M:      5   3   1   4
500k:    7   4   1   6
250k:    9   8   1   8
125k:    9   8   1   16
100k:    9   8   1   20 */

#define CAN_BAUD_COUNT 5
static const u8 can_baud_bs1[]    = { CAN_BS1_9tq, CAN_BS1_9tq, CAN_BS1_9tq, CAN_BS1_7tq, CAN_BS1_5tq };
static const u8 can_baud_bs2[]    = { CAN_BS1_8tq, CAN_BS1_8tq, CAN_BS1_8tq, CAN_BS1_4tq, CAN_BS1_3tq };
static const u8 can_baud_sjw[]    = { CAN_SJW_1tq, CAN_SJW_1tq, CAN_SJW_1tq, CAN_SJW_1tq, CAN_SJW_1tq };
static const u8 can_baud_pre[]    = { 20, 16, 8, 6, 4 };
static const u32 can_baud_rate[]  = { 100000, 125000, 250000, 500000, 1000000 };

u32 platform_can_setup( unsigned id, u32 clock )
{
  CAN_InitTypeDef        CAN_InitStructure;
  CAN_FilterInitTypeDef  CAN_FilterInitStructure;
  GPIO_InitTypeDef GPIO_InitStructure;
  int cbaudidx = -1;

  // Configure IO Pins -- This is for STM32F103RE
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
  GPIO_Init( GPIOB, &GPIO_InitStructure );
  
  GPIO_InitStructure.GPIO_Pin = GPIO_Pin_9;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_Init( GPIOB, &GPIO_InitStructure );

  // Select baud rate up to requested rate, except for below min, where min is selected
  if ( clock >= can_baud_rate[ CAN_BAUD_COUNT - 1 ] ) // round down to peak rate if >= peak rate
    cbaudidx = CAN_BAUD_COUNT - 1;
  else
  {
    for( cbaudidx = 0; cbaudidx < CAN_BAUD_COUNT - 1; cbaudidx ++ )
    {
      if( clock < can_baud_rate[ cbaudidx + 1 ] ) // take current idx if next is too large
        break;
    }
  }

  /* Deinitialize CAN Peripheral */
  CAN_DeInit( CAN1 );
  CAN_StructInit( &CAN_InitStructure );

  /* CAN cell init */
  CAN_InitStructure.CAN_TTCM=DISABLE;
  CAN_InitStructure.CAN_ABOM=DISABLE;
  CAN_InitStructure.CAN_AWUM=DISABLE;
  CAN_InitStructure.CAN_NART=DISABLE;
  CAN_InitStructure.CAN_RFLM=DISABLE;
  CAN_InitStructure.CAN_TXFP=DISABLE;
  CAN_InitStructure.CAN_Mode=CAN_Mode_Normal;
  CAN_InitStructure.CAN_SJW=can_baud_sjw[ cbaudidx ];
  CAN_InitStructure.CAN_BS1=can_baud_bs1[ cbaudidx ];
  CAN_InitStructure.CAN_BS2=can_baud_bs2[ cbaudidx ];
  CAN_InitStructure.CAN_Prescaler=can_baud_pre[ cbaudidx ];
  CAN_Init( CAN1, &CAN_InitStructure );

  /* CAN filter init */
  CAN_FilterInitStructure.CAN_FilterNumber=0;
  CAN_FilterInitStructure.CAN_FilterMode=CAN_FilterMode_IdMask;
  CAN_FilterInitStructure.CAN_FilterScale=CAN_FilterScale_32bit;
  CAN_FilterInitStructure.CAN_FilterIdHigh=0x0000;
  CAN_FilterInitStructure.CAN_FilterIdLow=0x0000;
  CAN_FilterInitStructure.CAN_FilterMaskIdHigh=0x0000;
  CAN_FilterInitStructure.CAN_FilterMaskIdLow=0x0000;
  CAN_FilterInitStructure.CAN_FilterFIFOAssignment=CAN_FIFO0;
  CAN_FilterInitStructure.CAN_FilterActivation=ENABLE;
  CAN_FilterInit(&CAN_FilterInitStructure);
  
  return can_baud_rate[ cbaudidx ];
}
/*
u32 platform_can_op( unsigned id, int op, u32 data )
{
  u32 res = 0;
  TIM_TypeDef *ptimer = timer[ id ];
  volatile unsigned dummy;

  data = data;
  switch( op )
  {
    case PLATFORM_TIMER_OP_READ:
      res = TIM_GetCounter( ptimer );
      break;
  }
  return res;
}
*/

void platform_can_send( unsigned id, u32 canid, u8 idtype, u8 len, const u8 *data )
{
  CanTxMsg TxMessage;
  const char *s = ( char * )data;
  char *d;
  
  switch( idtype )
  {
    case ELUA_CAN_ID_STD:
      TxMessage.IDE = CAN_ID_STD;
      TxMessage.StdId = canid;
      break;
    case ELUA_CAN_ID_EXT:
      TxMessage.IDE = CAN_ID_EXT;
      TxMessage.ExtId = canid;
      break;
  }
  
  TxMessage.RTR=CAN_RTR_DATA;
  TxMessage.DLC=len;
  
  d = ( char * )TxMessage.Data;
  DUFF_DEVICE_8( len,  *d++ = *s++ );
  
  CAN_Transmit( CAN1, &TxMessage );
}

void USB_LP_CAN_RX0_IRQHandler(void)
{
/*
  CanRxMsg RxMessage;

  RxMessage.StdId=0x00;
  RxMessage.ExtId=0x00;
  RxMessage.IDE=0;
  RxMessage.DLC=0;
  RxMessage.FMI=0;
  RxMessage.Data[0]=0x00;
  RxMessage.Data[1]=0x00;

  CAN_Receive(CAN_FIFO0, &RxMessage);

  if((RxMessage.ExtId==0x1234) && (RxMessage.IDE==CAN_ID_EXT)
     && (RxMessage.DLC==2) && ((RxMessage.Data[1]|RxMessage.Data[0]<<8)==0xDECA))
  {
    ret = 1; 
  }
  else
  {
    ret = 0; 
  }*/
}

int platform_can_recv( unsigned id, u32 *canid, u8 *idtype, u8 *len, u8 *data )
{
  CanRxMsg RxMessage;
  const char *s;
  char *d;

  if( CAN_MessagePending( CAN1, CAN_FIFO0 ) > 0 )
  {
    CAN_Receive(CAN1, CAN_FIFO0, &RxMessage);

    if( RxMessage.IDE == CAN_ID_STD )
    {
      *canid = ( u32 )RxMessage.StdId;
      *idtype = ELUA_CAN_ID_STD;
    }
    else
    {
      *canid = ( u32 )RxMessage.ExtId;
      *idtype = ELUA_CAN_ID_EXT;
    }

    *len = RxMessage.DLC;

    s = ( const char * )RxMessage.Data;
    d = ( char* )data;
    DUFF_DEVICE_8( RxMessage.DLC,  *d++ = *s++ );
    return PLATFORM_OK;
  }
  else
    return PLATFORM_UNDERFLOW;
}

// ****************************************************************************
// SPI

static SPI_TypeDef *const spi[]  = { SPI1, SPI2, SPI3 };
static const u16 spi_prescaler[] = { 0, SPI_BaudRatePrescaler_2, SPI_BaudRatePrescaler_4, SPI_BaudRatePrescaler_8, 
                                     SPI_BaudRatePrescaler_16, SPI_BaudRatePrescaler_32, SPI_BaudRatePrescaler_64,
                                     SPI_BaudRatePrescaler_128, SPI_BaudRatePrescaler_256 };
static GPIO_TypeDef *const spi_gpio_port[] = { GPIOA, GPIOB, GPIOB };
static const u16 spi_sck_mosi_pins[] = { GPIO_Pin_5 | GPIO_Pin_7, GPIO_Pin_13 | GPIO_Pin_15, GPIO_Pin_3 | GPIO_Pin_5 };
static const u16 spi_miso_pins[] = { GPIO_Pin_6, GPIO_Pin_14, GPIO_Pin_4 };

static void spis_init()
{
  // Enable Clocks
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI3, ENABLE);
}

#define SPI_GET_BASE_CLK( id ) ( ( id ) == 0 ? ( HCLK / PCLK2_DIV ) : ( HCLK / PCLK1_DIV ) )

u32 platform_spi_setup( unsigned id, int mode, u32 clock, unsigned cpol, unsigned cpha, unsigned databits )
{
  SPI_InitTypeDef SPI_InitStructure;
  GPIO_InitTypeDef GPIO_InitStructure;
  u8 prescaler_idx = intlog2( ( unsigned ) ( SPI_GET_BASE_CLK( id ) / clock ) );
  if( prescaler_idx == 0 )
    prescaler_idx ++;
  else if( prescaler_idx > 8 )
    prescaler_idx = 8;
  
  /* Configure SPI pins */
  GPIO_InitStructure.GPIO_Pin = spi_sck_mosi_pins[ id ];
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_Init(spi_gpio_port[ id ], &GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin = spi_miso_pins[ id ];
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_Init(spi_gpio_port[id], &GPIO_InitStructure);
  
  /* Take down, then reconfigure SPI peripheral */
  SPI_Cmd( spi[ id ], DISABLE );
  SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
  SPI_InitStructure.SPI_Mode = mode ? SPI_Mode_Master : SPI_Mode_Slave;
  SPI_InitStructure.SPI_DataSize = ( databits == 16 ) ? SPI_DataSize_16b : SPI_DataSize_8b; // not ideal, but defaults to sane 8-bits
  SPI_InitStructure.SPI_CPOL = cpol ? SPI_CPOL_High : SPI_CPOL_Low;
  SPI_InitStructure.SPI_CPHA = cpha ? SPI_CPHA_2Edge : SPI_CPHA_1Edge;
  SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
  SPI_InitStructure.SPI_BaudRatePrescaler = spi_prescaler[ prescaler_idx ];
  SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
  SPI_InitStructure.SPI_CRCPolynomial = 7;
  SPI_Init( spi[ id ], &SPI_InitStructure );
  SPI_Cmd( spi[ id ], ENABLE );
  SPI_NSSInternalSoftwareConfig( spi[ id ], SPI_NSSInternalSoft_Set );
  
  return ( SPI_GET_BASE_CLK( id ) / ( 1 << prescaler_idx ) );
}

spi_data_type platform_spi_send_recv( unsigned id, spi_data_type data )
{
  SPI_I2S_SendData( spi[ id ], data );
  while( SPI_I2S_GetFlagStatus( spi[ id ], SPI_I2S_FLAG_TXE ) == RESET );
  while( SPI_I2S_GetFlagStatus( spi[ id ], SPI_I2S_FLAG_RXNE ) == RESET );
  return SPI_I2S_ReceiveData( spi[ id ] );
}

void platform_spi_select( unsigned id, int is_select )
{
  // This platform doesn't have a hardware SS pin, so there's nothing to do here
  id = id;
  is_select = is_select;
}

// ****************************************************************************
// UART
// TODO: Support timeouts.

// All possible STM32 uarts defs
USART_TypeDef *const stm32_usart[] =          { USART1, USART2, USART3, UART4, UART5 };
static GPIO_TypeDef *const usart_gpio_rx_port[] = { GPIOA, GPIOA, GPIOB, GPIOC, GPIOD };
static GPIO_TypeDef *const usart_gpio_tx_port[] = { GPIOA, GPIOA, GPIOB, GPIOC, GPIOC };
static const u16 usart_gpio_rx_pin[] = { GPIO_Pin_10, GPIO_Pin_3, GPIO_Pin_11, GPIO_Pin_11, GPIO_Pin_2 };
static const u16 usart_gpio_tx_pin[] = { GPIO_Pin_9, GPIO_Pin_2, GPIO_Pin_10, GPIO_Pin_10, GPIO_Pin_12 };
static GPIO_TypeDef *const usart_gpio_hwflow_port[] = { GPIOA, GPIOA, GPIOB };
static const u16 usart_gpio_cts_pin[] = { GPIO_Pin_11, GPIO_Pin_0, GPIO_Pin_13 };
static const u16 usart_gpio_rts_pin[] = { GPIO_Pin_12, GPIO_Pin_1, GPIO_Pin_14 };

static void usart_init(u32 id, USART_InitTypeDef * initVals)
{
  /* Configure USART IO */
  GPIO_InitTypeDef GPIO_InitStructure;

  /* Configure USART Tx Pin as alternate function push-pull */
  GPIO_InitStructure.GPIO_Pin = usart_gpio_tx_pin[id];
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_Init(usart_gpio_tx_port[id], &GPIO_InitStructure);

  /* Configure USART Rx Pin as input floating */
  GPIO_InitStructure.GPIO_Pin = usart_gpio_rx_pin[id];
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_Init(usart_gpio_rx_port[id], &GPIO_InitStructure);

  /* Configure USART */
  USART_Init(stm32_usart[id], initVals);

  /* Enable USART */
  USART_Cmd(stm32_usart[id], ENABLE);
}

static void uarts_init()
{
  // Enable clocks.
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_UART4, ENABLE);
}

u32 platform_uart_setup( unsigned id, u32 baud, int databits, int parity, int stopbits )
{
  USART_InitTypeDef USART_InitStructure;

  USART_InitStructure.USART_BaudRate = baud;

  USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;

  switch( databits )
  {
    case 5:
    case 6:
    case 7:
    case 8:
      USART_InitStructure.USART_WordLength = USART_WordLength_8b;
      break;
    case 9:
      USART_InitStructure.USART_WordLength = USART_WordLength_9b;
      break;
    default:
      USART_InitStructure.USART_WordLength = USART_WordLength_8b;
      break;
  }

  switch (stopbits)
  {
    case PLATFORM_UART_STOPBITS_1:
      USART_InitStructure.USART_StopBits = USART_StopBits_1;
      break;
    case PLATFORM_UART_STOPBITS_2:
      USART_InitStructure.USART_StopBits = USART_StopBits_2;
      break;
    default:
      USART_InitStructure.USART_StopBits = USART_StopBits_2;
      break;
  }

  switch (parity)
  {
    case PLATFORM_UART_PARITY_EVEN:
      USART_InitStructure.USART_Parity = USART_Parity_Even;
      break;
    case PLATFORM_UART_PARITY_ODD:
      USART_InitStructure.USART_Parity = USART_Parity_Odd;
      break;
    default:
      USART_InitStructure.USART_Parity = USART_Parity_No;
      break;
  }

  usart_init(id, &USART_InitStructure);

  return TRUE;
}

void platform_s_uart_send( unsigned id, u8 data )
{
  while(USART_GetFlagStatus(stm32_usart[id], USART_FLAG_TXE) == RESET)
  {
  }
  USART_SendData(stm32_usart[id], data);
}

int platform_s_uart_recv( unsigned id, s32 timeout )
{
  if( timeout == 0 )
  {
    if (USART_GetFlagStatus(stm32_usart[id], USART_FLAG_RXNE) == RESET)
      return -1;
    else
      return USART_ReceiveData(stm32_usart[id]);
  }
  // Receive char blocking
  while(USART_GetFlagStatus(stm32_usart[id], USART_FLAG_RXNE) == RESET);
  return USART_ReceiveData(stm32_usart[id]);
}

int platform_s_uart_set_flow_control( unsigned id, int type )
{
  USART_TypeDef *usart = stm32_usart[ id ]; 
  int temp = 0;
  GPIO_InitTypeDef GPIO_InitStructure;

  if( id >= 3 ) // on STM32 only USART1 through USART3 have hardware flow control ([TODO] but only on high density devices?)
    return PLATFORM_ERR;

  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;

  if( type == PLATFORM_UART_FLOW_NONE )
  {
    usart->CR3 &= ~USART_HardwareFlowControl_RTS_CTS;
    GPIO_InitStructure.GPIO_Pin = usart_gpio_rts_pin[ id ] | usart_gpio_cts_pin[ id ];
    GPIO_Init( usart_gpio_hwflow_port[ id ], &GPIO_InitStructure );      
    return PLATFORM_OK;
  }
  if( type & PLATFORM_UART_FLOW_CTS )
  {
    temp |= USART_HardwareFlowControl_CTS;
    GPIO_InitStructure.GPIO_Pin = usart_gpio_cts_pin[ id ];
    GPIO_Init( usart_gpio_hwflow_port[ id ], &GPIO_InitStructure );
  }
  if( type & PLATFORM_UART_FLOW_RTS )
  {
    temp |= USART_HardwareFlowControl_RTS;
    GPIO_InitStructure.GPIO_Pin = usart_gpio_rts_pin[ id ];
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
    GPIO_Init( usart_gpio_hwflow_port[ id ], &GPIO_InitStructure );
  }
  usart->CR3 |= temp;
  return PLATFORM_OK;
}

// ****************************************************************************
// Timers

// We leave out TIM6/TIM for now, as they are dedicated
static TIM_TypeDef * const timer[] = { TIM1, TIM2, TIM3, TIM4, TIM5 };
#define TIM_GET_PRESCALE( id ) ( ( id ) == 0 || ( id ) == 5 ? ( PCLK2_DIV ) : ( PCLK1_DIV ) )
#define TIM_GET_BASE_CLK( id ) ( TIM_GET_PRESCALE( id ) == 1 ? ( HCLK / TIM_GET_PRESCALE( id ) ) : ( HCLK / ( TIM_GET_PRESCALE( id ) / 2 ) ) )
#define TIM_STARTUP_CLOCK       50000

static u32 timer_set_clock( unsigned id, u32 clock );

static u16 systick_eth_counter;
#define SYSTICK_ETH_LIMIT_MS    500       

void SysTick_Handler( void )
{
  // Handle virtual timers
  cmn_virtual_timer_cb();

#ifdef BUILD_MMCFS
  disk_timerproc();
#endif

  if( eth_initialized )
  {  
    systick_eth_counter += SYSTICKMS;
    if( systick_eth_counter == SYSTICK_ETH_LIMIT_MS )
    {
      systick_eth_counter = 0;

      // Indicate that a SysTick interrupt has occurred.
      eth_timer_fired = 1;

      // Generate a fake Ethernet interrupt.  This will perform the actual work
      // of incrementing the timers and taking the appropriate actions.
      platform_eth_force_interrupt();
    }
  }
}

static void timers_init()
{
  unsigned i;

  // Enable clocks.
  RCC_APB2PeriphClockCmd( RCC_APB2Periph_TIM1, ENABLE );  
  RCC_APB1PeriphClockCmd( RCC_APB1Periph_TIM2, ENABLE );
  RCC_APB1PeriphClockCmd( RCC_APB1Periph_TIM3, ENABLE );
  RCC_APB1PeriphClockCmd( RCC_APB1Periph_TIM4, ENABLE );
  RCC_APB1PeriphClockCmd( RCC_APB1Periph_TIM5, ENABLE );

  // Configure timers
  for( i = 0; i < NUM_TIMER; i ++ )
    timer_set_clock( i, TIM_STARTUP_CLOCK );
}

static u32 timer_get_clock( unsigned id )
{
  TIM_TypeDef* ptimer = timer[ id ];

  return TIM_GET_BASE_CLK( id ) / ( TIM_GetPrescaler( ptimer ) + 1 );
}

static u32 timer_set_clock( unsigned id, u32 clock )
{
  TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
  TIM_TypeDef *ptimer = timer[ id ];
  u16 pre = ( TIM_GET_BASE_CLK( id ) / clock ) - 1;
  
  TIM_TimeBaseStructure.TIM_Period = 0xFFFF;
  TIM_TimeBaseStructure.TIM_Prescaler = pre;
  TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
  TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
  TIM_TimeBaseStructure.TIM_RepetitionCounter = 0x0000;
  TIM_TimeBaseInit( timer[ id ], &TIM_TimeBaseStructure );
  TIM_Cmd( ptimer, ENABLE );
  
  return TIM_GET_BASE_CLK( id ) / ( pre + 1 );
}

void platform_s_timer_delay( unsigned id, u32 delay_us )
{
  TIM_TypeDef *ptimer = timer[ id ];
  volatile unsigned dummy;
  timer_data_type final;

  final = ( ( u64 )delay_us * timer_get_clock( id ) ) / 1000000;
  TIM_SetCounter( ptimer, 0 );
  for( dummy = 0; dummy < 200; dummy ++ );
  while( TIM_GetCounter( ptimer ) < final );
}

u32 platform_s_timer_op( unsigned id, int op, u32 data )
{
  u32 res = 0;
  TIM_TypeDef *ptimer = timer[ id ];
  volatile unsigned dummy;

  data = data;
  switch( op )
  {
    case PLATFORM_TIMER_OP_START:
      TIM_SetCounter( ptimer, 0 );
      for( dummy = 0; dummy < 200; dummy ++ );
      break;

    case PLATFORM_TIMER_OP_READ:
      res = TIM_GetCounter( ptimer );
      break;

    case PLATFORM_TIMER_OP_GET_MAX_DELAY:
      res = platform_timer_get_diff_us( id, 0, 0xFFFF );
      break;

    case PLATFORM_TIMER_OP_GET_MIN_DELAY:
      res = platform_timer_get_diff_us( id, 0, 1 );
      break;

    case PLATFORM_TIMER_OP_SET_CLOCK:
      res = timer_set_clock( id, data );
      break;

    case PLATFORM_TIMER_OP_GET_CLOCK:
      res = timer_get_clock( id );
      break;

  }
  return res;
}

int platform_s_timer_set_match_int( unsigned id, u32 period_us, int type )
{
  return PLATFORM_TIMER_INT_INVALID_ID;
}

// ****************************************************************************
// Quadrature Encoder Support (uses timers)
// No pin configuration, many of the timers should work with default config if
// pins aren't reconfigured for another peripheral

void stm32_enc_init( unsigned id )
{
  TIM_TypeDef *ptimer = timer[ id ];

  TIM_Cmd( ptimer, DISABLE );
  TIM_DeInit( ptimer );
  TIM_SetCounter( ptimer, 0 );
  TIM_EncoderInterfaceConfig( ptimer, TIM_EncoderMode_TI12, TIM_ICPolarity_Rising, TIM_ICPolarity_Rising);
  TIM_Cmd( ptimer, ENABLE );
}

void stm32_enc_set_counter( unsigned id, unsigned count )
{
  TIM_TypeDef *ptimer = timer[ id ];
  
  TIM_SetCounter( ptimer, ( u16 )count );
}

// ****************************************************************************
// PWMs
// Using Timer 8 (5 in eLua)

#define PWM_TIMER_ID 5
#define PWM_TIMER_NAME TIM8

static const u16 pwm_gpio_pins[] = { GPIO_Pin_6, GPIO_Pin_7, GPIO_Pin_8, GPIO_Pin_9 };

static void pwms_init()
{
  RCC_APB2PeriphClockCmd( RCC_APB2Periph_TIM8, ENABLE );  
  // 
}

// Helper function: return the PWM clock
// NOTE: Can't find a function to query for the period set for the timer, therefore using the struct.
//       This may require adjustment if driver libraries are updated.
static u32 platform_pwm_get_clock()
{
  return ( ( TIM_GET_BASE_CLK( PWM_TIMER_ID ) / ( TIM_GetPrescaler( PWM_TIMER_NAME ) + 1 ) ) / ( PWM_TIMER_NAME->ARR + 1 ) );
}

// Helper function: set the PWM clock
static u32 platform_pwm_set_clock( u32 clock )
{
  TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;
  TIM_TypeDef* ptimer = PWM_TIMER_NAME;
  unsigned period, prescaler;
  
  /* Time base configuration */
  period = TIM_GET_BASE_CLK( PWM_TIMER_ID ) / clock;
    
  prescaler = (period / 0x10000) + 1;
  period /= prescaler;
  
  TIM_TimeBaseStructure.TIM_Period = period - 1;
  TIM_TimeBaseStructure.TIM_Prescaler = prescaler - 1;
  TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
  TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
  TIM_TimeBaseStructure.TIM_RepetitionCounter = 0x0000;
  TIM_TimeBaseInit( ptimer, &TIM_TimeBaseStructure );
    
  return platform_pwm_get_clock();
}

u32 platform_pwm_setup( unsigned id, u32 frequency, unsigned duty )
{
  TIM_OCInitTypeDef  TIM_OCInitStructure;
  TIM_TypeDef* ptimer = TIM8;
  GPIO_InitTypeDef GPIO_InitStructure;
  u32 clock;
  
  TIM_Cmd( ptimer, DISABLE);
  TIM_SetCounter( ptimer, 0 );
  
  /* Configure USART Tx Pin as alternate function push-pull */
  GPIO_InitStructure.GPIO_Pin = pwm_gpio_pins[ id ];
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_Init(GPIOC, &GPIO_InitStructure);
  
  clock = platform_pwm_set_clock( frequency );
  TIM_ARRPreloadConfig( ptimer, ENABLE );
  
  /* PWM Mode configuration */  
  TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
  TIM_OCInitStructure.TIM_OutputState = ( PWM_TIMER_NAME->CCER & ( ( u16 )1 << 4 * id ) ) ? TIM_OutputState_Enable : TIM_OutputState_Disable;
  TIM_OCInitStructure.TIM_OutputNState = TIM_OutputNState_Disable;
  TIM_OCInitStructure.TIM_Pulse = ( u16 )( duty * ( PWM_TIMER_NAME->ARR + 1 ) / 100 );
  TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
  TIM_OCInitStructure.TIM_OCIdleState = TIM_OCIdleState_Set;
  
  switch ( id )
  {
    case 0:
      TIM_OC1Init( ptimer, &TIM_OCInitStructure );
      TIM_OC1PreloadConfig( ptimer, TIM_OCPreload_Enable );
      break;
    case 1:
      TIM_OC2Init( ptimer, &TIM_OCInitStructure );
      TIM_OC2PreloadConfig( ptimer, TIM_OCPreload_Enable );
      break;
    case 2:
      TIM_OC3Init( ptimer, &TIM_OCInitStructure );
      TIM_OC3PreloadConfig( ptimer, TIM_OCPreload_Enable );
      break;
    case 3:
      TIM_OC4Init( ptimer, &TIM_OCInitStructure );
      TIM_OC4PreloadConfig( ptimer, TIM_OCPreload_Enable ) ;
      break;
    default:
      return 0;
  }
  
  TIM_CtrlPWMOutputs(ptimer, ENABLE);  
  
  TIM_Cmd( ptimer, ENABLE );
  
  return clock;
}

u32 platform_pwm_op( unsigned id, int op, u32 data )
{
  u32 res = 0;

  switch( op )
  {
    case PLATFORM_PWM_OP_SET_CLOCK:
      res = platform_pwm_set_clock( data );
      break;

    case PLATFORM_PWM_OP_GET_CLOCK:
      res = platform_pwm_get_clock();
      break;

    case PLATFORM_PWM_OP_START:
      PWM_TIMER_NAME->CCER |= ( ( u16 )1 << 4 * id );
      break;

    case PLATFORM_PWM_OP_STOP:
      PWM_TIMER_NAME->CCER &= ~( ( u16 )1 << 4 * id );
      break;
  }

  return res;
}

// *****************************************************************************
// CPU specific functions
 
u32 platform_s_cpu_get_frequency()
{
  return HCLK;
}

// *****************************************************************************
// ADC specific functions and variables

#ifdef BUILD_ADC

#define ADC1_DR_Address ((u32)ADC1_BASE + 0x4C)

static ADC_TypeDef *const adc[] = { ADC1, ADC2, ADC3 };
static const u32 adc_timer[] = { ADC_ExternalTrigConv_T1_CC1, ADC_ExternalTrigConv_T2_CC2, ADC_ExternalTrigConv_T3_TRGO, ADC_ExternalTrigConv_T4_CC4 };

ADC_InitTypeDef adc_init_struct;
DMA_InitTypeDef dma_init_struct;

int platform_adc_check_timer_id( unsigned id, unsigned timer_id )
{
  // NOTE: We only allow timer 2 at the moment, for the sake of implementation simplicity
  return ( timer_id == 2 );
}

void platform_adc_stop( unsigned id )
{
  elua_adc_ch_state *s = adc_get_ch_state( id );
  elua_adc_dev_state *d = adc_get_dev_state( 0 );
  
  s->op_pending = 0;
  INACTIVATE_CHANNEL( d, id );

  // If there are no more active channels, stop the sequencer
  if( d->ch_active == 0 )
  {
    // Ensure that no external triggers are firing
    ADC_ExternalTrigConvCmd( adc[ d->seq_id ], DISABLE );
    
    // Also ensure that DMA interrupt won't fire ( this shouldn't really be necessary )
    nvic_init_structure_adc.NVIC_IRQChannelCmd = DISABLE; 
    NVIC_Init(&nvic_init_structure_adc);
    
    d->running = 0;
  }
}

int platform_adc_update_sequence( )
{  
  elua_adc_dev_state *d = adc_get_dev_state( 0 );
  
  // NOTE: this shutdown/startup stuff may or may not be absolutely necessary
  //       it is here to deal with the situation that a dma conversion has
  //       already started and should be reset.
  ADC_ExternalTrigConvCmd( adc[ d->seq_id ], DISABLE );
  
  // Stop in-progress adc dma transfers
  // Later de/reinitialization should flush out synchronization problems
  ADC_DMACmd( adc[ d->seq_id ], DISABLE );
  
  // Bring down adc, update setup, bring back up
  ADC_Cmd( adc[ d->seq_id ], DISABLE );
  ADC_DeInit( adc[ d->seq_id ] );
  
  d->seq_ctr = 0; 
  while( d->seq_ctr < d->seq_len )
  {
    ADC_RegularChannelConfig( adc[ d->seq_id ], d->ch_state[ d->seq_ctr ]->id, d->seq_ctr+1, ADC_SampleTime_1Cycles5 );
    d->seq_ctr++;
  }
  d->seq_ctr = 0;
  
  adc_init_struct.ADC_NbrOfChannel = d->seq_len;
  ADC_Init( adc[ d->seq_id ], &adc_init_struct );
  ADC_Cmd( adc[ d->seq_id ], ENABLE );
  
  // Bring down adc dma, update setup, bring back up
  DMA_Cmd( DMA1_Channel1, DISABLE );
  DMA_DeInit( DMA1_Channel1 );
  dma_init_struct.DMA_BufferSize = d->seq_len;
  dma_init_struct.DMA_MemoryBaseAddr = (u32)d->sample_buf;
  DMA_Init( DMA1_Channel1, &dma_init_struct );
  DMA_Cmd( DMA1_Channel1, ENABLE );
  
  ADC_DMACmd( adc[ d->seq_id ], ENABLE );
  DMA_ITConfig( DMA1_Channel1, DMA1_IT_TC1 , ENABLE ); 
  
  if ( d->clocked == 1 && d->running == 1 )
    ADC_ExternalTrigConvCmd( adc[ d->seq_id ], ENABLE );
  
  return PLATFORM_OK;
}

void DMA1_Channel1_IRQHandler(void) 
{
  elua_adc_dev_state *d = adc_get_dev_state( 0 );
  elua_adc_ch_state *s;
  
  DMA_ClearITPendingBit( DMA1_IT_TC1 );
  
  d->seq_ctr = 0;
  while( d->seq_ctr < d->seq_len )
  {
    s = d->ch_state[ d->seq_ctr ];
    s->value_fresh = 1;
    
    // Fill in smoothing buffer until warmed up
    if ( s->logsmoothlen > 0 && s->smooth_ready == 0)
      adc_smooth_data( s->id );
#if defined( BUF_ENABLE_ADC )
    else if ( s->reqsamples > 1 )
    {
      buf_write( BUF_ID_ADC, s->id, ( t_buf_data* )s->value_ptr );
      s->value_fresh = 0;
    }
#endif

    // If we have the number of requested samples, stop sampling
    if ( adc_samples_available( s->id ) >= s->reqsamples && s->freerunning == 0 )
      platform_adc_stop( s->id );

    d->seq_ctr++;
  }
  d->seq_ctr = 0;

  if( d->running == 1 )
    adc_update_dev_sequence( 0 );
  
  if ( d->clocked == 0 && d->running == 1 )
    ADC_SoftwareStartConvCmd( adc[ d->seq_id ], ENABLE );
}

static void adcs_init()
{
  unsigned id;
  elua_adc_dev_state *d = adc_get_dev_state( 0 );
  
  for( id = 0; id < NUM_ADC; id ++ )
    adc_init_ch_state( id );

  RCC_APB2PeriphClockCmd( RCC_APB2Periph_ADC1, ENABLE );
  RCC_ADCCLKConfig( RCC_PCLK2_Div8 );
  
  ADC_DeInit( adc[ d->seq_id ] );
  ADC_StructInit( &adc_init_struct );
  
  // Universal Converter Setup
  adc_init_struct.ADC_Mode = ADC_Mode_Independent;
  adc_init_struct.ADC_ScanConvMode = ENABLE;
  adc_init_struct.ADC_ContinuousConvMode = DISABLE;
  adc_init_struct.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
  adc_init_struct.ADC_DataAlign = ADC_DataAlign_Right;
  adc_init_struct.ADC_NbrOfChannel = 1;
  
  // Apply default config
  ADC_Init( adc[ d->seq_id ], &adc_init_struct );
  ADC_ExternalTrigConvCmd( adc[ d->seq_id ], DISABLE );
    
  // Enable ADC
  ADC_Cmd( adc[ d->seq_id ], ENABLE );  
  
  // Reset/Perform ADC Calibration
  ADC_ResetCalibration( adc[ d->seq_id ] );
  while( ADC_GetResetCalibrationStatus( adc[ d->seq_id ] ) );
  ADC_StartCalibration( adc[ d->seq_id ] );
  while( ADC_GetCalibrationStatus( adc[ d->seq_id ] ) );
  
  // Set up DMA to handle samples
  RCC_AHBPeriphClockCmd( RCC_AHBPeriph_DMA1, ENABLE );
  
  DMA_DeInit( DMA1_Channel1 );
  dma_init_struct.DMA_PeripheralBaseAddr = ADC1_DR_Address;
  dma_init_struct.DMA_MemoryBaseAddr = (u32)d->sample_buf;
  dma_init_struct.DMA_DIR = DMA_DIR_PeripheralSRC;
  dma_init_struct.DMA_BufferSize = 1;
  dma_init_struct.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
  dma_init_struct.DMA_MemoryInc = DMA_MemoryInc_Enable;
  dma_init_struct.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
  dma_init_struct.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
  dma_init_struct.DMA_Mode = DMA_Mode_Circular;
  dma_init_struct.DMA_Priority = DMA_Priority_Low;
  dma_init_struct.DMA_M2M = DMA_M2M_Disable;
  DMA_Init( DMA1_Channel1, &dma_init_struct );
  
  ADC_DMACmd(ADC1, ENABLE );
  
  DMA_Cmd( DMA1_Channel1, ENABLE );
  DMA_ITConfig( DMA1_Channel1, DMA1_IT_TC1 , ENABLE ); 
  
  platform_adc_set_clock( 0, 0 );
}

u32 platform_adc_set_clock( unsigned id, u32 frequency )
{
  TIM_TimeBaseInitTypeDef timer_base_struct;
  elua_adc_dev_state *d = adc_get_dev_state( 0 );
  
  unsigned period, prescaler;
  
  // Make sure sequencer is disabled before making changes
  ADC_ExternalTrigConvCmd( adc[ d->seq_id ], DISABLE );
  
  if ( frequency > 0 )
  {
    d->clocked = 1;
    // Attach timer to converter
    adc_init_struct.ADC_ExternalTrigConv = adc_timer[ d->timer_id ];
    
    period = TIM_GET_BASE_CLK( id ) / frequency;
    
    prescaler = (period / 0x10000) + 1;
    period /= prescaler;

    timer_base_struct.TIM_Period = period - 1;
    timer_base_struct.TIM_Prescaler = prescaler - 1;
    timer_base_struct.TIM_ClockDivision = TIM_CKD_DIV1;
    timer_base_struct.TIM_CounterMode = TIM_CounterMode_Down;
    TIM_TimeBaseInit( timer[ d->timer_id ], &timer_base_struct );
    
    frequency = ( TIM_GET_BASE_CLK( id ) / ( TIM_GetPrescaler( timer[ d->timer_id ] ) + 1 ) ) / period;
    
    // Set up output compare for timer
    TIM_SelectOutputTrigger(timer[ d->timer_id ], TIM_TRGOSource_Update);
  }
  else
  {
    d->clocked = 0;
    
    // Switch to Software-only Trigger
    adc_init_struct.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;   
  }
  
  // Apply config
  ADC_Init( adc[ d->seq_id ], &adc_init_struct );
  
  return frequency;
}

int platform_adc_start_sequence( )
{ 
  elua_adc_dev_state *d = adc_get_dev_state( 0 );
  
  // Only force update and initiate if we weren't already running
  // changes will get picked up during next interrupt cycle
  if ( d->running != 1 )
  {
    adc_update_dev_sequence( 0 );
    
    d->running = 1;
    
    DMA_ClearITPendingBit( DMA1_IT_TC1 );

    nvic_init_structure_adc.NVIC_IRQChannelCmd = ENABLE; 
    NVIC_Init(&nvic_init_structure_adc);

    if( d->clocked == 1 )
      ADC_ExternalTrigConvCmd( adc[ d->seq_id ], ENABLE );
    else
      ADC_SoftwareStartConvCmd( adc[ d->seq_id ], ENABLE );
  }

  return PLATFORM_OK;
}

#endif // ifdef BUILD_ADC

// ****************************************************************************
// I2C support

static I2C_TypeDef *const i2c[]  = { I2C1, I2C2 };
static const u16 i2c_gpio_pins[] = { GPIO_Pin_6  | GPIO_Pin_7,
                                     GPIO_Pin_10 | GPIO_Pin_11 };
//                                   SCL           SDA

static void i2cs_init()
{
  RCC_APB1PeriphClockCmd( RCC_APB1Periph_I2C1, ENABLE );
  RCC_APB1PeriphClockCmd( RCC_APB1Periph_I2C2, ENABLE );
}

u32 platform_i2c_setup( unsigned id, u32 speed )
{
  I2C_InitTypeDef I2C_InitStructure;
  GPIO_InitTypeDef GPIO_InitStructure;
  
  /* Configure I2C pins */
  GPIO_InitStructure.GPIO_Pin = i2c_gpio_pins[ id ];
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_OD;
  GPIO_Init( GPIOB, &GPIO_InitStructure );

  /* Configure I2C peripheral */
  I2C_Cmd( i2c[ id ], DISABLE );
  I2C_StructInit( &I2C_InitStructure );
  I2C_InitStructure.I2C_Mode = I2C_Mode_I2C;
  I2C_InitStructure.I2C_DutyCycle = I2C_DutyCycle_2;
  I2C_InitStructure.I2C_Ack = I2C_Ack_Enable;
  I2C_InitStructure.I2C_ClockSpeed = speed;
  I2C_InitStructure.I2C_OwnAddress1 = 0; // dummy, shouldn't matter
  I2C_InitStructure.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
  I2C_Init( i2c[ id ], &I2C_InitStructure );
  I2C_Cmd( i2c[ id ], ENABLE );

  return speed;
}

void platform_i2c_send_start( unsigned id )
{
  I2C_TypeDef *pi2c = ( I2C_TypeDef* )i2c[ id ];

  //while( I2C_GetFlagStatus( pi2c, I2C_FLAG_BUSY ) );
  I2C_GenerateSTART( pi2c, ENABLE );
  while( I2C_CheckEvent( pi2c, I2C_EVENT_MASTER_MODE_SELECT ) != SUCCESS );
}

void platform_i2c_send_stop( unsigned id )
{
  I2C_TypeDef *pi2c = ( I2C_TypeDef* )i2c[ id ];

  I2C_GenerateSTOP( pi2c, ENABLE );
  while( I2C_GetFlagStatus( pi2c, I2C_FLAG_BUSY ) );
  ( void )pi2c->SR1;
  ( void )pi2c->SR2;
}

int platform_i2c_send_address( unsigned id, u16 address, int direction )
{
  I2C_TypeDef *pi2c = ( I2C_TypeDef* )i2c[ id ];
  u32 flags;
  u32 match = direction == PLATFORM_I2C_DIRECTION_TRANSMITTER ? I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED : I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED;
  int res = 1;

  I2C_Send7bitAddress( pi2c, address, direction == PLATFORM_I2C_DIRECTION_TRANSMITTER ? I2C_Direction_Transmitter : I2C_Direction_Receiver );
  while( 1 )
  {
    flags = I2C_GetLastEvent( pi2c );
    if( flags & I2C_FLAG_AF )
    {
      I2C_ClearFlag( pi2c, I2C_FLAG_AF );
      res = 0;
      break;
    }
    if( flags == match )
      break;
  }
  ( void )pi2c->SR1;
  ( void )pi2c->SR2;
  return res;
}

int platform_i2c_send_byte( unsigned id, u8 data )
{
  I2C_TypeDef *pi2c = ( I2C_TypeDef* )i2c[ id ];
  u32 flags;
  int res = 1;

  I2C_SendData( pi2c, data ); 
  while( 1 )
  {
    flags = I2C_GetLastEvent( pi2c );
    if( flags & I2C_FLAG_AF )
    {
      I2C_ClearFlag( pi2c, I2C_FLAG_AF );
      res = 0;
      break;
    }
    if( flags == I2C_EVENT_MASTER_BYTE_TRANSMITTED )
      break;
  }
  return res;
}

int platform_i2c_recv_byte( unsigned id, int ack )
{
  I2C_TypeDef *pi2c = ( I2C_TypeDef* )i2c[ id ];
  u8 data;

  I2C_AcknowledgeConfig( pi2c, ack ? ENABLE : DISABLE );
  if( !ack )
    I2C_GenerateSTOP( pi2c, ENABLE );
  while( I2C_GetFlagStatus( pi2c, I2C_FLAG_RXNE ) == RESET );
  data = I2C_ReceiveData( pi2c );
  if( !ack )
    while( pi2c->CR1 & I2C_CR1_STOP );
  return data;
}

// *****************************************************************************
// VRAM subsystem

extern u32 vram_data[ VRAM_SIZE_TOTAL >> 2 ]; 

#define SPI2_DR_Address       0x4000380C
#define SPI_VRAM_PIN_MOSI     GPIO_Pin_15
#define SPI_VRAM_PIN_MISO     GPIO_Pin_14
#define SPI_VRAM_PIN_CLK      GPIO_Pin_13
#define SPI_VRAM_PORT         GPIOB
#define SPI_VRAM_PERIPH       SPI2
#define PROP_RESET_PIN        12
#define PROP_RESET_PORT       1

static void vram_transfer_init()
{
  // NEW CODE
  DMA_InitTypeDef DMA_InitStructure;
  SPI_InitTypeDef SPI_InitStructure;
  GPIO_InitTypeDef GPIO_InitStructure;

  // Setup SPI interface in slave mode
   /* Configure SPI pins */
  GPIO_InitStructure.GPIO_Pin = SPI_VRAM_PIN_CLK | SPI_VRAM_PIN_MOSI;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_Init(SPI_VRAM_PORT, &GPIO_InitStructure);
  GPIO_InitStructure.GPIO_Pin = SPI_VRAM_PIN_MISO;
  GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
  GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
  GPIO_Init(SPI_VRAM_PORT, &GPIO_InitStructure);
 
  /* Take down, then reconfigure SPI peripheral */
  SPI_Cmd( SPI_VRAM_PERIPH, DISABLE );
  SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
  SPI_InitStructure.SPI_Mode = SPI_Mode_Slave;
  SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
  SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
  SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
  SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
  SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
  SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
  SPI_InitStructure.SPI_CRCPolynomial = 7;
  SPI_Init( SPI_VRAM_PERIPH, &SPI_InitStructure );
  SPI_Cmd( SPI_VRAM_PERIPH, ENABLE );
  SPI_NSSInternalSoftwareConfig( SPI_VRAM_PERIPH, SPI_NSSInternalSoft_Reset );
  
  // Setup DMA
  RCC_AHBPeriphClockCmd( RCC_AHBPeriph_DMA1, ENABLE );  
  DMA_DeInit( DMA1_Channel5 );
  DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)SPI2_DR_Address;
  DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)vram_data;
  DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralDST;
  DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;
  DMA_InitStructure.DMA_BufferSize = VRAM_SIZE_TOTAL;
  DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
  DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
  DMA_InitStructure.DMA_PeripheralDataSize = DMA_MemoryDataSize_Byte;
  DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
  DMA_InitStructure.DMA_Mode = DMA_Mode_Circular;
  DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
  DMA_Init( DMA1_Channel5, &DMA_InitStructure );     
  
  // Start DMA transfer now
  // It will automatically cycle through the data each time a request is made
	SPI_I2S_DMACmd( SPI_VRAM_PERIPH, SPI_I2S_DMAReq_Tx, ENABLE );	
	DMA_Cmd( DMA1_Channel5, ENABLE );  

  // Now it's a good time to get the Propeller out of reset
  platform_pio_op( PROP_RESET_PORT, 1 << PROP_RESET_PIN, PLATFORM_IO_PIN_SET );  
  platform_pio_op( PROP_RESET_PORT, 1 << PROP_RESET_PIN, PLATFORM_IO_PIN_DIR_OUTPUT );
}

// ****************************************************************************
// Ethernet support with ENC28J60

#ifdef BUILD_ENC28J60

#define ETH_INT_RESNUM        PLATFORM_IO_ENCODE( ENC28J60_INT_PORT, ENC28J60_INT_PIN, PLATFORM_IO_ENC_PIN )
#define MAX_SEND_RETRY        5

volatile static u8 eth_forced = 0;

// Ethernet interrupt handler
void eth_int_handler()
{
  if( !eth_initialized )
    return;
  if( eth_forced )
  {
    elua_uip_mainloop();
    eth_forced = 0;
    return;
  }
  SetGlobalInterrupt( 0 );
  if( isRxIntActive() )
    elua_uip_mainloop();
  else if( isLinkIntActive() )
    elua_net_link_changed();
  SetGlobalInterrupt( 1 );
}

static void eth_init()
{
  static struct uip_eth_addr sTempAddr;
  unsigned i;

  // Initialize the MAC first
  static const u8 macaddr[] = ENC28J60_MAC_ADDRESS;
  initMAC( macaddr );

  // Setup Ethernet timer
  platform_s_timer_op( ELUA_DHCP_TIMER_ID, PLATFORM_TIMER_OP_SET_CLOCK, 1200 );

  // Then the Ethernet interrupt
  platform_pio_op( ENC28J60_INT_PORT, 1 << ENC28J60_INT_PIN, PLATFORM_IO_PIN_DIR_INPUT );
  platform_pio_op( ENC28J60_INT_PORT, 1 << ENC28J60_INT_PIN, PLATFORM_IO_PIN_PULLUP );
  platform_cpu_set_interrupt( INT_GPIO_NEGEDGE, ETH_INT_RESNUM, PLATFORM_CPU_ENABLE );
  // Note: the handler will be called automatically from platform_int.c
  SetRXInterrupt( 1 );
  SetLinkInterrupt( 1 );

  // Let uIP run now
  for( i = 0; i < 6; i ++ )
    sTempAddr.addr[ i ] = macaddr[ i ];
  elua_net_init( &sTempAddr );  
  eth_initialized = 1;  
}

int platform_eth_get_link_status()
{
  return isLinkUp() ? PLATFORM_ETH_LINK_UP : PLATFORM_ETH_LINK_DOWN;
}

void platform_eth_send_packet( const void* src, u32 size )
{
  int retrcount = 0;
  //printf( "send %d bytes\n", ( int )size );
  for( retrcount = 0; retrcount < MAX_SEND_RETRY; retrcount ++ )
    if( MACWrite( ( u8* )src, ( u16 )size ) == TRUE )
      return;
  //MACWrite( ( u8* )src, ( u16 )size );
}

u32 platform_eth_get_packet_nb( void* buf, u32 maxlen )
{
  u16 res = MACRead( buf, maxlen );
//  if( res > 0 )
//    printf( "got %d bytes\n", res );
  return res;
}

void platform_eth_force_interrupt()
{
  eth_forced = 1;
  EXTI_GenerateSWInterrupt( exti_line[ ENC28J60_INT_PIN ]  ); 
}

u32 platform_eth_get_elapsed_time()
{
  if( eth_timer_fired )
  {
    eth_timer_fired = 0;
    return SYSTICK_ETH_LIMIT_MS;
  }
  else
    return 0;
}

void platform_eth_set_interrupt( int state )
{
  platform_cpu_set_interrupt( INT_GPIO_NEGEDGE, ETH_INT_RESNUM, state == PLATFORM_ETH_INT_ENABLE ? PLATFORM_CPU_ENABLE : PLATFORM_CPU_DISABLE );
}

#else // #ifdef BUILD_ENC28J60

static void eth_init()
{
}

#endif // #ifdef BUILD_ENC28J60

// ****************************************************************************
// Platform specific modules go here

#define MIN_OPT_LEVEL 2
#include "lrodefs.h"
extern const LUA_REG_TYPE snd_map[];

const LUA_REG_TYPE platform_map[] =
{
#if LUA_OPTIMIZE_MEMORY > 0
  { LSTRKEY( "snd" ), LROVAL( snd_map ) },
#endif
  { LNILKEY, LNILVAL }
};

LUALIB_API int luaopen_platform( lua_State *L )
{
#if LUA_OPTIMIZE_MEMORY > 0
  return 0;
#else // #if LUA_OPTIMIZE_MEMORY > 0
#error "the stm32 platform module doesn't work at LUA_OPTIMIZE_MEMORY == 0"
#endif // #if LUA_OPTIMIZE_MEMORY > 0
}

