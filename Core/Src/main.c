/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "fatfs.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app.h"
#include <stm32f4xx_hal_can.h>
#include <string.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "stream_buffer.h"
#include <stdlib.h>
#include "ssd1306.h"
#include "ssd1306_fonts.h"
#include "anomaly.h"
#include "sd_spi.h"
#include "sd_logger.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
CAN_HandleTypeDef hcan1;

I2C_HandleTypeDef hi2c1;

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart2;

osThreadId defaultTaskHandle;
/* USER CODE BEGIN PV */
TaskHandle_t canTaskHandle;
TaskHandle_t cliTaskHandle;
TaskHandle_t loggerTaskHandle;
TaskHandle_t oledTaskHandle;
TaskHandle_t statsTaskHandle;
TaskHandle_t sdTaskHandle;

QueueHandle_t canRxQueue;   /* CAN ISR -> CanTask */
QueueHandle_t logQueue;     /* producers -> LoggerTask */

volatile uint32_t statTotalFrames = 0;

/* OLED dashboard: single source of truth for displayed data + its mutex.
 * Producers publish a snapshot under g_oledMutex; OledTask reads a copy. */
OledTelemetry_t   g_oledTel;
SemaphoreHandle_t g_oledMutex;
QueueHandle_t     oledAlertQueue;
volatile int32_t  g_pageRequest = -1;
volatile int32_t  g_pageRequestIsSnap = 0;

StreamBufferHandle_t uartRxStream;   /* UART ISR -> CliTask */
uint8_t uartRxByte;                  /* single-byte HAL_UART_Receive_IT target */

/* Menu/command state owned by CliTask (defined in app.c). */
extern LoggerMode_t currentMode;
extern UART_State_t uartState;
extern uint32_t userFilterID;
extern uint8_t  filterConfigured;
extern char     filterInputBuffer[10];
extern uint8_t  filterInputIndex;
extern char     txInputBuffer[50];
extern uint8_t  txInputIndex;
extern volatile uint32_t speedFrameCount;
extern uint32_t lastSpeedPrintTick;
extern uint8_t  Logger_ShouldLog(uint32_t id);
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_CAN1_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
void StartDefaultTask(void const * argument);

/* USER CODE BEGIN PFP */
void StartCanTask(void *argument);
void StartCliTask(void *argument);
void StartLoggerTask(void *argument);
void StartOledTask(void *argument);
void StartStatsTask(void *argument);
/* StartSdTask is declared in sd_logger.h */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_CAN1_Init();
  MX_USART2_UART_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_FATFS_Init();
  /* USER CODE BEGIN 2 */
  char test[] = "UART OK\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t *)test, sizeof(test)-1, HAL_MAX_DELAY);

  /* Start CAN with RX FIFO0 notification (the ISR feeds canRxQueue). */
  CAN1_FilterConfig();
  if (HAL_CAN_Start(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);

  /* Arm UART RX interrupt (the ISR feeds uartRxStream). */
  HAL_UART_Receive_IT(&huart2, &uartRxByte, 1);
  /* USER CODE END 2 */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  canRxQueue = xQueueCreate(16, sizeof(CanFrame_t));
  if (canRxQueue == NULL)
  {
    Error_Handler();
  }

  uartRxStream = xStreamBufferCreate(64, 1);   /* 64 bytes, trigger level 1 */
  if (uartRxStream == NULL)
  {
    Error_Handler();
  }

  logQueue = xQueueCreate(16, sizeof(LogMsg_t));
  if (logQueue == NULL)
  {
    Error_Handler();
  }

  /* OLED dashboard: mutex for the shared telemetry struct + alert queue. */
  g_oledMutex = xSemaphoreCreateMutex();
  if (g_oledMutex == NULL)
  {
    Error_Handler();
  }

  oledAlertQueue = xQueueCreate(4, sizeof(OledAlert_t));
  if (oledAlertQueue == NULL)
  {
    Error_Handler();
  }

  /* Telemetry starts zeroed; mode reflects the boot default. */
  memset(&g_oledTel, 0, sizeof(g_oledTel));
  g_oledTel.mode = currentMode;

  /* Phase B: start anomaly detection + its auto-learn window. */
  Anomaly_Init(HAL_GetTick());

  /* SD logging: record queue + logger state (file opened lazily by SdTask). */
  sdQueue = xQueueCreate(32, sizeof(SdRecord_t));
  if (sdQueue == NULL)
  {
    Error_Handler();
  }
  SD_Logger_Init();
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  xTaskCreate(StartCanTask,    "CanTask",    256, NULL, tskIDLE_PRIORITY + 3, &canTaskHandle);
  xTaskCreate(StartCliTask,    "CliTask",    256, NULL, tskIDLE_PRIORITY + 2, &cliTaskHandle);
  xTaskCreate(StartLoggerTask, "LoggerTask", 256, NULL, tskIDLE_PRIORITY + 1, &loggerTaskHandle);
  xTaskCreate(StartOledTask,   "OledTask",   256, NULL, tskIDLE_PRIORITY + 1, &oledTaskHandle);
  xTaskCreate(StartStatsTask,  "StatsTask",  128, NULL, tskIDLE_PRIORITY + 1, &statsTaskHandle);
  /* SD logger task: larger stack for FatFs (f_write/f_sync use ~512B sector ops). */
  xTaskCreate(StartSdTask,     "SdTask",     512, NULL, tskIDLE_PRIORITY + 1, &sdTaskHandle);
  /* USER CODE END RTOS_THREADS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 84;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief CAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_CAN1_Init(void)
{

  /* USER CODE BEGIN CAN1_Init 0 */

  /* USER CODE END CAN1_Init 0 */

  /* USER CODE BEGIN CAN1_Init 1 */

  /* USER CODE END CAN1_Init 1 */
  hcan1.Instance = CAN1;
  hcan1.Init.Prescaler = 6;
  hcan1.Init.Mode = CAN_MODE_NORMAL;
  hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
  hcan1.Init.TimeSeg1 = CAN_BS1_11TQ;
  hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
  hcan1.Init.TimeTriggeredMode = DISABLE;
  hcan1.Init.AutoBusOff = ENABLE;
  hcan1.Init.AutoWakeUp = DISABLE;
  hcan1.Init.AutoRetransmission = ENABLE;
  hcan1.Init.ReceiveFifoLocked = DISABLE;
  hcan1.Init.TransmitFifoPriority = DISABLE;
  if (HAL_CAN_Init(&hcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CAN1_Init 2 */
  /* CAN clock = APB1 = 42 MHz; 42MHz / (6 * (1+11+2)TQ) = 500 kbps */
  /* USER CODE END CAN1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);

  /*Configure GPIO pin : PA4 */
  GPIO_InitStruct.Pin = GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* PA5 is SPI1_SCK for the SD card (configured by MX_SPI1_Init), so the old
   * LED GPIO-output setup that used to live here was removed - driving SCK as
   * a GPIO output would conflict with SPI. CS (PA4) is configured above. */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* Lean CAN RX ISR: read the frame and push it onto canRxQueue. All
 * filtering/logging/stats happen in CanTask. */
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
  CAN_RxHeaderTypeDef rxHeader;
  uint8_t rxData[8];

  if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &rxHeader, rxData) != HAL_OK)
  {
    return;
  }

  CanFrame_t frame;
  frame.id = (rxHeader.IDE == CAN_ID_STD) ? rxHeader.StdId : rxHeader.ExtId;
  frame.dlc = rxHeader.DLC;
  frame.timestamp = HAL_GetTick();
  memcpy(frame.data, rxData, 8);

  BaseType_t higherPriorityTaskWoken = pdFALSE;
  xQueueSendFromISR(canRxQueue, &frame, &higherPriorityTaskWoken);
  portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

/* Enqueue a string for LoggerTask to transmit. Safe from any task; drops the
 * message if logQueue is full rather than blocking the producer. */
void Log_Send(const char *text)
{
  LogMsg_t m;
  size_t len = strlen(text);
  if (len >= sizeof(m.text))
  {
    len = sizeof(m.text) - 1;
  }
  memcpy(m.text, text, len);
  m.text[len] = '\0';
  m.len = (uint16_t)len;
  xQueueSend(logQueue, &m, 0);
}

/* CanTask: mode-aware consumer of canRxQueue. Applies READ_ALL / FILTERED /
 * SPEEDTEST / IDLE logic, runs anomaly detection, routes output via logQueue
 * and the SD logger, and publishes the OLED telemetry snapshot. */
void StartCanTask(void *argument)
{
  CanFrame_t frame;
  char line[120];

  /* Local counters - updated cheaply on every frame WITHOUT taking the mutex.
   * A snapshot is published into g_oledTel every OLED_PUBLISH_MS so the shared
   * struct (and thus the display) is the single source of truth, but the
   * mutex is only contended ~10x/sec instead of once per CAN frame. */
  uint32_t locTotal    = 0;
  uint32_t locLogged   = 0;
  uint32_t locFiltered = 0;
  uint32_t locDropped  = 0;
  uint32_t locLastId   = 0;
  uint8_t  locLastDlc  = 0;
  uint8_t  locLastData[8] = {0};
  uint8_t  locHaveFrame = 0;

  const TickType_t OLED_PUBLISH_MS = pdMS_TO_TICKS(100);
  TickType_t lastPublish = xTaskGetTickCount();

  for(;;)
  {
    uint8_t frameArrived = 0;

    /* Wait up to one publish interval for a frame. The short timeout means we
     * still publish telemetry on schedule even when the bus is idle. */
    if (xQueueReceive(canRxQueue, &frame, OLED_PUBLISH_MS) == pdTRUE)
    {
      frameArrived = 1;
      statTotalFrames++;
      locTotal++;
      locLastId   = frame.id;
      locLastDlc  = frame.dlc;
      memcpy(locLastData, frame.data, 8);
      locHaveFrame = 1;

      switch (currentMode)
      {
        case MODE_SPEEDTEST:
          /* Count only; the Logger task prints the per-second rate. */
          speedFrameCount++;
          locLogged++;
          break;

        case MODE_IDLE:
        case MODE_WRITE:
          /* Frames are not logged in these modes; count as dropped. */
          locDropped++;
          break;

        case MODE_READ_FILTERED:
          if (!Logger_ShouldLog(frame.id))
          {
            /* Filtered out by the active filter (non-match). */
            locFiltered++;
            break;
          }
          /* fall-through: matched filter -> log it */

        case MODE_READ_ALL:
        default:
        {
          locLogged++;
          int n = snprintf(line, sizeof(line),
                           "T:%lu ID:0x%lX DLC:%d DATA:",
                           (unsigned long)frame.timestamp,
                           (unsigned long)frame.id,
                           frame.dlc);
          for (int i = 0; i < frame.dlc && n < (int)sizeof(line) - 4; i++)
          {
            n += snprintf(line + n, sizeof(line) - n, "%02X ", frame.data[i]);
          }
          snprintf(line + n, sizeof(line) - n, "\r\n");
          Log_Send(line);
          /* SD: mirror logged frames to the CSV (respects the same mode rules
           * as UART - only frames we actually log reach here). No-op if SD
           * logging is stopped. */
          SD_Log_Frame(frame.id, frame.dlc, frame.data, frame.timestamp);
          break;
        }
      }
    }

    /* Phase B: anomaly detection. ProcessFrame runs only when a frame arrived;
     * Tick runs every loop to evaluate the rolling flood window even when idle.
     * Both call OLED_RaiseAlert(...) internally on detection. */
    {
      uint32_t nowMs = HAL_GetTick();
      if (frameArrived)
      {
        Anomaly_ProcessFrame(frame.id, frame.dlc, frame.data, nowMs);
      }
      Anomaly_Tick(nowMs);
    }

    /* Publish a telemetry snapshot every ~100 ms (single source of truth). */
    TickType_t now = xTaskGetTickCount();
    if ((now - lastPublish) >= OLED_PUBLISH_MS)
    {
      lastPublish = now;
      if (xSemaphoreTake(g_oledMutex, 0) == pdTRUE)
      {
        g_oledTel.mode       = currentMode;
        g_oledTel.total      = locTotal;
        g_oledTel.logged     = locLogged;
        g_oledTel.sd_logged  = SD_Logger_FramesLogged();
        g_oledTel.filtered   = locFiltered;
        g_oledTel.dropped    = locDropped;
        g_oledTel.have_frame = locHaveFrame;
        g_oledTel.last_id    = locLastId;
        g_oledTel.last_dlc   = locLastDlc;
        memcpy(g_oledTel.last_data, locLastData, 8);
        g_oledTel.filter_id  = userFilterID;
        g_oledTel.filter_set = filterConfigured;
        xSemaphoreGive(g_oledMutex);
      }
      /* If the mutex was momentarily held, skip this publish; the next tick
       * (100 ms later) will catch up. No data is lost - counters are local. */
    }
  }
}

/* LoggerTask: the only task that calls HAL_UART_Transmit. Drains logQueue and
 * transmits each message; emits the per-second rate in SPEEDTEST mode. */
void StartLoggerTask(void *argument)
{
  LogMsg_t m;

  for(;;)
  {
    /* Wait up to 100ms for a log message so we can also service speed-test. */
    if (xQueueReceive(logQueue, &m, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      HAL_UART_Transmit(&huart2, (uint8_t *)m.text, m.len, HAL_MAX_DELAY);
    }

    /* Speed-test: print frames/sec once per second. */
    if (currentMode == MODE_SPEEDTEST &&
        (HAL_GetTick() - lastSpeedPrintTick) >= 1000)
    {
      char rate[40];
      uint32_t count;

      taskENTER_CRITICAL();
      count = speedFrameCount;
      speedFrameCount = 0;
      taskEXIT_CRITICAL();

      int n = snprintf(rate, sizeof(rate), "Frames/sec: %lu\r\n", (unsigned long)count);
      HAL_UART_Transmit(&huart2, (uint8_t *)rate, n, HAL_MAX_DELAY);
      lastSpeedPrintTick = HAL_GetTick();
    }
  }
}

/* ===========================================================================
 * OLED dashboard helpers
 * =========================================================================== */

/* Human-readable mode names, indexed by LoggerMode_t. */
static const char *const oledModeNames[] = {
  "READ ALL", "FILTER", "SPEED", "IDLE", "WRITE"
};

/* Rotating activity indicator glyphs. */
static const char oledSpinner[] = { '|', '/', '-', '\\' };

/* Map a logger mode to the page that should follow it automatically. */
static OLED_Page_t OledPageForMode(LoggerMode_t m)
{
  switch (m)
  {
    case MODE_READ_FILTERED: return OLED_PAGE_FILTER;
    case MODE_SPEEDTEST:     return OLED_PAGE_SPEED;
    case MODE_READ_ALL:
    case MODE_IDLE:
    case MODE_WRITE:
    default:                 return OLED_PAGE_MAIN;
  }
}

static const char *OledAlertTypeName(AlertType_t t);   /* fwd decl */

/* Raise an alert (stub hook for future anomaly detectors). Task context only -
 * never call from an ISR. Non-blocking: drops the alert if the queue is full. */
void OLED_RaiseAlert(AlertType_t type, uint32_t id, uint8_t severity)
{
  OledAlert_t a;
  a.type = type;
  a.id = id;
  a.severity = severity;
  if (oledAlertQueue != NULL)
  {
    (void)xQueueSend(oledAlertQueue, &a, 0);
  }
  /* SD: alerts are always logged regardless of mode (no-op if SD off). */
  SD_Log_Alert(type, id, severity, HAL_GetTick());

  /* UART: print every alert to the serial console (via the Logger task). */
  {
    char msg[48];
    snprintf(msg, sizeof(msg), "ALERT: %s id=0x%lX sev=%u\r\n",
             OledAlertTypeName(type), (unsigned long)id, (unsigned)severity);
    Log_Send(msg);
  }
}

static const char *OledAlertTypeName(AlertType_t t)
{
  switch (t)
  {
    case ALERT_UNKNOWN_ID: return "UNKNOWN ID";
    case ALERT_FLOOD:      return "FLOOD";
    case ALERT_TIMING:     return "TIMING";
    case ALERT_PAYLOAD:    return "PAYLOAD";
    case ALERT_NONE:
    default:               return "NONE";
  }
}

/* --- Page 1: MAIN ---------------------------------------------------------- */
void OLED_DrawMainPage(const OledTelemetry_t *t, uint8_t spin)
{
  char line[24];

  ssd1306_SetCursor(0, 0);
  ssd1306_WriteString("CAN IDS", Font_7x10, White);
  /* SD status flag in the title bar: "SD" shown when logging is active,
   * blanked when off (small font, left of the spinner). */
  if (SD_Logger_IsActive())
  {
    ssd1306_SetCursor(96, 1);
    ssd1306_WriteString("SD", Font_6x8, White);
  }
  line[0] = oledSpinner[spin & 0x03];
  line[1] = '\0';
  ssd1306_SetCursor(120, 0);
  ssd1306_WriteString(line, Font_7x10, White);
  ssd1306_Line(0, 12, 127, 12, White);

  snprintf(line, sizeof(line), "MODE: %s",
           (t->mode <= MODE_WRITE) ? oledModeNames[t->mode] : "?");
  ssd1306_SetCursor(0, 16);
  ssd1306_WriteString(line, Font_6x8, White);

  if (t->have_frame)
    snprintf(line, sizeof(line), "FPS:%lu  LAST:%lX",
             (unsigned long)t->fps, (unsigned long)t->last_id);
  else
    snprintf(line, sizeof(line), "FPS:%lu  LAST:---",
             (unsigned long)t->fps);
  ssd1306_SetCursor(0, 28);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "ALRT:%s  DROP:%lu",
           t->alert_active ? "!!" : "OK", (unsigned long)t->dropped);
  ssd1306_SetCursor(0, 40);
  ssd1306_WriteString(line, Font_6x8, White);

  /* Bottom row (y=52): latest CAN frame on a single line, small font:
   * "<id>:<b0><b1>..."  e.g. "1A3:DA7E001F". Truncated to fit 21 chars. */
  if (t->have_frame)
  {
    char *p = line;
    p += snprintf(p, sizeof(line), "%lX:", (unsigned long)t->last_id);
    uint8_t n = (t->last_dlc > 8) ? 8 : t->last_dlc;
    for (uint8_t i = 0; i < n && (p - line) < (int)sizeof(line) - 3; i++)
    {
      p += snprintf(p, sizeof(line) - (p - line), "%02X", t->last_data[i]);
    }
  }
  else
  {
    snprintf(line, sizeof(line), "----");
  }
  ssd1306_SetCursor(0, 52);
  ssd1306_WriteString(line, Font_6x8, White);
}

/* --- Page 2: FILTER -------------------------------------------------------- */
void OLED_DrawFilterPage(const OledTelemetry_t *t, uint8_t spin)
{
  char line[24];
  (void)spin;

  ssd1306_SetCursor(0, 0);
  ssd1306_WriteString("MODE: FILTER", Font_7x10, White);
  ssd1306_Line(0, 12, 127, 12, White);

  if (t->filter_set)
    snprintf(line, sizeof(line), "FILT: 0x%lX", (unsigned long)t->filter_id);
  else
    snprintf(line, sizeof(line), "FILT: ---");
  ssd1306_SetCursor(0, 18);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "RX  : %lu", (unsigned long)t->logged);
  ssd1306_SetCursor(0, 30);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "DROP: %lu", (unsigned long)t->dropped);
  ssd1306_SetCursor(0, 42);
  ssd1306_WriteString(line, Font_6x8, White);
}

/* --- Page 3: SPEED --------------------------------------------------------- */
void OLED_DrawSpeedPage(const OledTelemetry_t *t, uint8_t spin)
{
  char line[24];
  (void)spin;

  ssd1306_SetCursor(0, 0);
  ssd1306_WriteString("MODE: SPEED", Font_7x10, White);
  ssd1306_Line(0, 12, 127, 12, White);

  /* Hero FPS number in the larger font. */
  snprintf(line, sizeof(line), "FPS : %lu", (unsigned long)t->fps);
  ssd1306_SetCursor(0, 16);
  ssd1306_WriteString(line, Font_7x10, White);

  snprintf(line, sizeof(line), "PEAK: %lu", (unsigned long)t->peak_fps);
  ssd1306_SetCursor(0, 32);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "TOTAL: %lu", (unsigned long)t->total);
  ssd1306_SetCursor(0, 44);
  ssd1306_WriteString(line, Font_6x8, White);
}

/* --- Page 4: ALERT (overlay) ----------------------------------------------- */
void OLED_DrawAlertPage(const OledAlert_t *a, uint8_t spin)
{
  char line[24];

  ssd1306_DrawRectangle(0, 0, 127, 63, White);

  /* Blink the banner using the spinner phase. */
  if (spin & 0x01)
  {
    ssd1306_SetCursor(14, 4);
    ssd1306_WriteString("!!! ALERT !!!", Font_7x10, White);
  }

  snprintf(line, sizeof(line), "TYPE: %s", OledAlertTypeName(a->type));
  ssd1306_SetCursor(6, 22);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "ID  : 0x%lX", (unsigned long)a->id);
  ssd1306_SetCursor(6, 34);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "SEV : %u", (unsigned)a->severity);
  ssd1306_SetCursor(6, 46);
  ssd1306_WriteString(line, Font_6x8, White);
}

/* --- Page 5: STATS --------------------------------------------------------- */
void OLED_DrawStatsPage(const OledTelemetry_t *t, uint8_t spin)
{
  char line[24];
  (void)spin;

  ssd1306_SetCursor(0, 0);
  ssd1306_WriteString("STATS", Font_7x10, White);
  /* SD logging status in the title bar. */
  ssd1306_SetCursor(72, 1);
  ssd1306_WriteString(SD_Logger_IsActive() ? "SD:ON"
                      : (SD_Logger_HasError() ? "SD:ERR" : "SD:OFF"),
                      Font_6x8, White);
  ssd1306_Line(0, 12, 127, 12, White);

  snprintf(line, sizeof(line), "TOTAL: %lu", (unsigned long)t->total);
  ssd1306_SetCursor(0, 16);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "UART : %lu", (unsigned long)t->logged);
  ssd1306_SetCursor(0, 28);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "SD   : %lu", (unsigned long)t->sd_logged);
  ssd1306_SetCursor(0, 40);
  ssd1306_WriteString(line, Font_6x8, White);

  snprintf(line, sizeof(line), "DROP : %lu", (unsigned long)t->dropped);
  ssd1306_SetCursor(0, 52);
  ssd1306_WriteString(line, Font_6x8, White);
}

/* Dispatch to the active page renderer. Caller has already cleared the frame. */
void OLED_DrawPage(OLED_Page_t page, const OledTelemetry_t *t,
                   const OledAlert_t *a, uint8_t spin)
{
  switch (page)
  {
    case OLED_PAGE_FILTER: OLED_DrawFilterPage(t, spin); break;
    case OLED_PAGE_SPEED:  OLED_DrawSpeedPage(t, spin);  break;
    case OLED_PAGE_STATS:  OLED_DrawStatsPage(t, spin);  break;
    case OLED_PAGE_ALERT:  OLED_DrawAlertPage(a, spin);  break;
    case OLED_PAGE_MAIN:
    default:               OLED_DrawMainPage(t, spin);   break;
  }
}

/* OledTask: drives the SSD1306 (128x64, I2C1 @ 0x3C, SCL=PB6 / SDA=PB7).
 * Reads the telemetry snapshot at 10 Hz, selects the active page (mode-follow,
 * manual 'p' lock, or alert overlay), renders it, and computes fps/peak. */
void StartOledTask(void *argument)
{
  OledTelemetry_t t;                 /* local snapshot */
  OledAlert_t     curAlert = {0};    /* latched alert being displayed */

  OLED_Page_t activePage = OLED_PAGE_MAIN;
  OLED_Page_t prevPage    = OLED_PAGE_MAIN;  /* page to restore after an alert */
  uint8_t     manualLock  = 0;               /* 'p' lock active */
  uint8_t     inAlert     = 0;
  TickType_t  alertDeadline = 0;

  uint8_t     spin        = 0;
  uint32_t    lastTotal   = 0;       /* total at last fps sample */
  uint32_t    peakFps     = 0;
  TickType_t  lastFpsTick = xTaskGetTickCount();

  /* The panel needs I2C up; MX_I2C1_Init() already ran in main(). */
  ssd1306_Init();

  for(;;)
  {
    TickType_t now = xTaskGetTickCount();

    /* 1) Snapshot telemetry under the mutex (copy only, no rendering). */
    if (xSemaphoreTake(g_oledMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
      t = g_oledTel;
      xSemaphoreGive(g_oledMutex);
    }

    /* 2) Compute fps from the total delta once per second; latch the peak. */
    if ((now - lastFpsTick) >= pdMS_TO_TICKS(1000))
    {
      uint32_t fps = t.total - lastTotal;
      lastTotal = t.total;
      lastFpsTick = now;
      if (fps > peakFps) peakFps = fps;

      /* Publish fps/peak back so other readers see the same numbers. */
      if (xSemaphoreTake(g_oledMutex, pdMS_TO_TICKS(5)) == pdTRUE)
      {
        g_oledTel.fps = fps;
        g_oledTel.peak_fps = peakFps;
        xSemaphoreGive(g_oledMutex);
      }
      t.fps = fps;
      t.peak_fps = peakFps;
    }

    /* 3) Drain alerts (non-blocking); newest wins, refreshes the 3s timer. */
    {
      OledAlert_t a;
      uint8_t gotAlert = 0;
      while (xQueueReceive(oledAlertQueue, &a, 0) == pdTRUE)
      {
        curAlert = a;
        gotAlert = 1;
      }
      if (gotAlert)
      {
        if (!inAlert)
        {
          prevPage = activePage;   /* remember where to return */
          inAlert = 1;
        }
        activePage = OLED_PAGE_ALERT;
        alertDeadline = now + pdMS_TO_TICKS(3000);
        /* latch the status flag into the shared struct */
        if (xSemaphoreTake(g_oledMutex, pdMS_TO_TICKS(5)) == pdTRUE)
        {
          g_oledTel.alert_active = 1;
          xSemaphoreGive(g_oledMutex);
        }
        t.alert_active = 1;
      }
    }

    /* 4) Page selection. */
    if (inAlert)
    {
      if ((int32_t)(now - alertDeadline) >= 0)
      {
        /* Alert expired: return to the previous page. */
        inAlert = 0;
        activePage = prevPage;
        if (xSemaphoreTake(g_oledMutex, pdMS_TO_TICKS(5)) == pdTRUE)
        {
          g_oledTel.alert_active = 0;
          xSemaphoreGive(g_oledMutex);
        }
        t.alert_active = 0;
      }
      /* While in alert, ignore page requests but keep them pending: a manual
       * 'p' or mode snap issued during an alert will apply after it clears. */
    }

    /* Consume any pending page request (applies now if not in alert; if in
     * alert, defer by leaving it - but we read it once so it doesn't stack). */
    {
      int32_t req = g_pageRequest;
      if (req >= 0)
      {
        int32_t isSnap = g_pageRequestIsSnap;
        g_pageRequest = -1;          /* consume */
        if (!inAlert)
        {
          activePage = (OLED_Page_t)req;
          manualLock = isSnap ? 0 : 1;   /* snap clears lock, manual sets it */
        }
        else
        {
          /* Remember the request as the page to restore after the alert. */
          prevPage = (OLED_Page_t)req;
          manualLock = isSnap ? 0 : 1;
        }
      }
    }

    /* Auto-follow the mode when not locked and not in an alert. */
    if (!inAlert && !manualLock)
    {
      activePage = OledPageForMode(t.mode);
    }

    /* 5) Render. */
    ssd1306_Fill(Black);
    OLED_DrawPage(activePage, &t, &curAlert, spin);
    ssd1306_UpdateScreen();

    spin++;
    vTaskDelay(pdMS_TO_TICKS(100));   /* 10 Hz refresh */
  }
}

/* StatsTask: prints a periodic frame-count line on a precise period
 * (vTaskDelayUntil). The LED heartbeat is unavailable (PA5 is SD SCK). */
void StartStatsTask(void *argument)
{
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t   lastTotal = 0;
  uint8_t    tick = 0;

  for(;;)
  {
    /* NOTE: PA5 is now SPI1_SCK for the SD card, so the old LED heartbeat
     * (HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5)) was removed - toggling SCK as
     * GPIO would corrupt SD I/O. Heartbeat is visible via the OLED spinner. */

    /* Every 5 s emit a stats line (only when not in speed-test mode, to
     * avoid clashing with the Frames/sec output). */
    if (++tick >= 5)
    {
      tick = 0;
      if (currentMode != MODE_SPEEDTEST)
      {
        uint32_t total = statTotalFrames;
        char s[64];
        snprintf(s, sizeof(s), "[stats] total=%lu (+%lu/5s)\r\n",
                 (unsigned long)total, (unsigned long)(total - lastTotal));
        Log_Send(s);
        lastTotal = total;
      }
    }

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000));
  }
}

/* Lean UART RX ISR: push the received byte onto uartRxStream and re-arm RX.
 * All menu parsing happens in CliTask. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART2)
  {
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    xStreamBufferSendFromISR(uartRxStream, &uartRxByte, 1, &higherPriorityTaskWoken);
    HAL_UART_Receive_IT(&huart2, &uartRxByte, 1);
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
  }
}

/* CliTask: owns the UART menu / command state machine. Consumes bytes from
 * uartRxStream; handles global single-char commands and the menu/filter/TX
 * input states. strtok/strtoul are safe here (task context). */
void StartCliTask(void *argument)
{
  uint8_t ch;
  char    msg[50];
  uint8_t pageCycle = OLED_PAGE_MAIN;   /* tracks the 'p' cycle position */

  PrintMenu();

  for(;;)
  {
    /* block until at least one byte is available */
    if (xStreamBufferReceive(uartRxStream, &ch, 1, portMAX_DELAY) != 1)
    {
      continue;
    }

    /* Global command: reopen menu anytime */
    if (ch == 'm' || ch == 'M')
    {
      currentMode = MODE_IDLE;
      uartState = UART_STATE_MENU;
      /* Mode snap: IDLE -> MAIN page, clears any manual page lock. */
      g_pageRequest = (int32_t)OLED_PAGE_MAIN;
      g_pageRequestIsSnap = 1;
      PrintMenu();
      continue;
    }

    /* Global command: cycle the OLED page (manual lock). */
    if (ch == 'p' || ch == 'P')
    {
      pageCycle = (uint8_t)((pageCycle + 1) % 4);  /* MAIN/FILTER/SPEED/STATS */
      g_pageRequest = (int32_t)pageCycle;
      g_pageRequestIsSnap = 0;                      /* manual -> sets lock */
      Log_Send("OLED page ->\r\n");
      continue;
    }

    /* Global command: re-learn the known-ID allow-list for anomaly detection. */
    if (ch == 'l' || ch == 'L')
    {
      Anomaly_StartLearn(HAL_GetTick());
      Log_Send("Anomaly: re-learning known IDs...\r\n");
      continue;
    }

    /* Global command: toggle SD-card CSV logging. */
    if (ch == 's' || ch == 'S')
    {
      if (SD_Logger_IsActive())
      {
        SD_Logger_Stop();
        Log_Send("SD: logging stop requested\r\n");
      }
      else
      {
        SD_Logger_Start();
        Log_Send("SD: logging start requested\r\n");
      }
      continue;
    }

    /* Global command: report anomaly-detection status. */
    if (ch == 'd' || ch == 'D')
    {
      char status[64];
      const char *sd = SD_Logger_IsActive() ? "on"
                     : (SD_Logger_HasError() ? "ERR" : "off");
      snprintf(status, sizeof(status),
               "Detect: %s, known IDs=%u, SD=%s\r\n",
               Anomaly_IsLearning() ? "LEARNING" : "ACTIVE",
               (unsigned)Anomaly_KnownCount(),
               sd);
      Log_Send(status);
      continue;
    }

    switch (uartState)
    {
      case UART_STATE_MENU:
        switch (ch)
        {
          case '1':
            currentMode = MODE_READ_ALL;
            uartState = UART_STATE_IDLE;
            pageCycle = OLED_PAGE_MAIN;
            g_pageRequest = (int32_t)OLED_PAGE_MAIN;   /* mode snap */
            g_pageRequestIsSnap = 1;
            Log_Send("Mode: READ ALL\r\n");
            break;

          case '2':
            uartState = UART_STATE_WAIT_FILTER_ID;
            filterInputIndex = 0;
            Log_Send("Enter CAN ID (HEX):\r\n");
            break;

          case '3':
            currentMode = MODE_SPEEDTEST;
            uartState = UART_STATE_IDLE;
            speedFrameCount = 0;
            lastSpeedPrintTick = HAL_GetTick();
            pageCycle = OLED_PAGE_SPEED;
            g_pageRequest = (int32_t)OLED_PAGE_SPEED;  /* mode snap */
            g_pageRequestIsSnap = 1;
            Log_Send("Mode: SPEED TEST\r\n");
            break;

          case '4':
            currentMode = MODE_IDLE;
            uartState = UART_STATE_IDLE;
            pageCycle = OLED_PAGE_MAIN;
            g_pageRequest = (int32_t)OLED_PAGE_MAIN;   /* mode snap */
            g_pageRequestIsSnap = 1;
            Log_Send("Mode: IDLE\r\n");
            break;

          case '5':
            uartState = UART_STATE_WAIT_TX_FRAME;
            txInputIndex = 0;
            Log_Send("Enter: ID DLC DATA...\r\n");
            break;

          default:
            Log_Send("Invalid option\r\n");
            PrintMenu();
            break;
        }
        break;

      case UART_STATE_WAIT_FILTER_ID:
        if (ch == '\r' || ch == '\n')
        {
          filterInputBuffer[filterInputIndex] = '\0';
          userFilterID = strtoul(filterInputBuffer, NULL, 16);
          filterConfigured = 1;
          currentMode = MODE_READ_FILTERED;
          uartState = UART_STATE_IDLE;
          pageCycle = OLED_PAGE_FILTER;
          g_pageRequest = (int32_t)OLED_PAGE_FILTER;   /* mode snap */
          g_pageRequestIsSnap = 1;
          snprintf(msg, sizeof(msg), "Filtering ID: 0x%lX\r\n", (unsigned long)userFilterID);
          Log_Send(msg);
        }
        else
        {
          if (filterInputIndex < sizeof(filterInputBuffer) - 1)
            filterInputBuffer[filterInputIndex++] = ch;
        }
        break;

      case UART_STATE_WAIT_TX_FRAME:
        if (ch == '\r' || ch == '\n')
        {
          txInputBuffer[txInputIndex] = '\0';
          ProcessTxCommand(txInputBuffer);
          currentMode = MODE_WRITE;
          uartState = UART_STATE_IDLE;
        }
        else
        {
          if (txInputIndex < sizeof(txInputBuffer) - 1)
            txInputBuffer[txInputIndex++] = ch;
        }
        break;

      case UART_STATE_IDLE:
      default:
        break;
    }
  }
}

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  if (htim->Instance == TIM6)
  {
    SD_TimerProc();   /* 1 ms timing for the SD-SPI driver */
  }
  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
	//char test[] = "ERROR!\r\n";

  __disable_irq();
  while (1)
  {
	  //HAL_UART_Transmit(&huart2, (uint8_t *)test, sizeof(test)-1, HAL_MAX_DELAY);
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
