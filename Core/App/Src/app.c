/*
 * app.c
 *
 *  Created on: Feb 1, 2026
 *      Author: madha
 */


#include "app.h"
#include<stdio.h>
#include<string.h>
#include <stdint.h>
#include <stdlib.h>


#define LOG_BUFFER_SIZE 32



LoggerMode_t currentMode = MODE_READ_ALL;
UART_State_t uartState = UART_STATE_MENU;


CAN_LogFrame_t logBuffer[LOG_BUFFER_SIZE];

//~~~~~~~~~~~~Global Variables~~~~~~~~~~~~~~~~~~

// Circular buffer vars 
volatile uint16_t logWriteIndex = 0;
volatile uint16_t logReadIndex = 0;

//Counters 
volatile uint32_t logOverflowCount = 0;
volatile uint32_t totalFrames = 0;
volatile uint32_t loggedFrames = 0;
volatile uint32_t filteredFrames = 0;
volatile uint32_t speedFrameCount = 0;

uint32_t lastSpeedPrintTick = 0;
uint8_t uartRxChar;
uint32_t userFilterID = 0;
uint8_t filterConfigured = 0;
char filterInputBuffer[10];
uint8_t filterInputIndex = 0;
char txInputBuffer[50];
uint8_t txInputIndex = 0;

volatile UART_Event_t uartEvent = UART_EVT_NONE;
volatile uint8_t uartMenuChoice = 0;





//-----------------------------------------------------------------------------------

void app_main(void){
	//to record application start time
	//start_tick = HAL_GetTick();
	char msg[] = "Entered app_main\r\n";
	HAL_UART_Transmit(&huart2, (uint8_t *)msg, sizeof(msg)-1, HAL_MAX_DELAY);

	CAN1_FilterConfig();
	// Start CAN module
	if(HAL_CAN_Start(&hcan1) != HAL_OK ){
		Error_Handler();
	}
	HAL_CAN_ActivateNotification(&hcan1, CAN_IT_RX_FIFO0_MSG_PENDING);
	HAL_UART_Receive_IT(&huart2, &uartRxChar, 1);
	PrintMenu();

	//CAN1_tx();


	while(1){
		HandleUartEvent();

		switch (currentMode)
		    {
		        case MODE_READ_ALL:
		        	/* fall-through intentional: filtering is done in the ISR */
		        case MODE_READ_FILTERED:
		        {
		            uint16_t writeSnap;
		            __disable_irq();
		            writeSnap = logWriteIndex;
		            __enable_irq();

		            if (logReadIndex != writeSnap)
		            {
		                CAN_LogFrame_t *frame = &logBuffer[logReadIndex];

		                char msg[120];

		                sprintf(msg,
		                        "T:%lu ID:0x%lX DLC:%d DATA:",
		                        frame->timestamp,
		                        frame->id,
		                        frame->dlc);

		                HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);

		                for (int i = 0; i < frame->dlc; i++)
		                {
		                    sprintf(msg, "%02X ", frame->data[i]);
		                    HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
		                }

		                sprintf(msg, "\r\n");
		                HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);

		                logReadIndex++;
		                if (logReadIndex >= LOG_BUFFER_SIZE)
		                    logReadIndex = 0;
		            }

		            break;
		        }

		        case MODE_SPEEDTEST:
		        	if (HAL_GetTick() - lastSpeedPrintTick >= 1000)
		        	    {
		        	        char msg[120];
		        	        uint32_t count;

		        	        __disable_irq();
		        	        count = speedFrameCount;
		        	        speedFrameCount = 0;
		        	        __enable_irq();

		        	        uint32_t esr = hcan1.Instance->ESR;
		        	        uint32_t btr = hcan1.Instance->BTR;

		        	        sprintf(msg,
		        	            "F/s:%lu ESR:0x%08lX (REC=%lu TEC=%lu LEC=%lu BOFF=%lu) BTR:0x%08lX\r\n",
		        	            count,
		        	            esr,
		        	            (esr >> 24) & 0xFF,
		        	            (esr >> 16) & 0xFF,
		        	            (esr >> 4) & 0x07,
		        	            (esr >> 2) & 0x01,
		        	            btr);
		        	        HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);

		        	        lastSpeedPrintTick = HAL_GetTick();
		        	    }
		        	break ;

		        case MODE_WRITE:
		        	 currentMode = MODE_IDLE;
		        	 break ;

		        case MODE_IDLE:
		        default:
		            break;
		    }

            __disable_irq();
            uint32_t overflow = logOverflowCount;
            logOverflowCount = 0;
            __enable_irq();

		 // Report overflow
		 if(overflow > 0){
			 char warning[60];
			 sprintf(warning, "WARNING: LOG OVERFLOW COUNT = %lu\r\n", overflow);
			 HAL_UART_Transmit(&huart2, (uint8_t*)warning, strlen(warning), HAL_MAX_DELAY);
		 }
	}

}


void CAN1_tx(void){

	char msg[50];

	CAN_TxHeaderTypeDef TxHeader;
	uint32_t TxMailbox;
	uint8_t our_message[] = "HELLO";

	TxHeader.DLC = 5; 
	TxHeader.IDE = CAN_ID_STD; 
	TxHeader.RTR = CAN_RTR_DATA;
	TxHeader.StdId 	= 0x65D; 
	

	if (HAL_CAN_AddTxMessage(&hcan1, &TxHeader, our_message, &TxMailbox) != HAL_OK) {
		Error_Handler();
	}

	//while(HAL_CAN_IsTxMessagePending(&hcan1, TxMailbox));

	sprintf(msg, "Message Transmitted :%s\r\n", our_message);
	HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), HAL_MAX_DELAY);
}

// Polling Rx function
//void CAN1_rx(void){
//	CAN_RxHeaderTypeDef RxHeader;
//	uint8_t rcvd_msg[6];
//	char msg[50];
//
//	if(HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &RxHeader, rcvd_msg) != HAL_OK ) {
//		Error_Handler();
//	}
//	sprintf(msg, "Message Received : %s\r\n", rcvd_msg);
//	HAL_UART_Transmit(&huart2, (uint8_t *) msg, strlen(msg), HAL_MAX_DELAY) ;
//}

//Rx Interrupt Callback
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan)
{
    CAN_RxHeaderTypeDef RxHeader;
    uint8_t rcvd_msg[8];

    if (HAL_CAN_GetRxMessage(hcan, CAN_RX_FIFO0, &RxHeader, rcvd_msg) != HAL_OK)
        return;

    /* blink LED so we know ISR is executing */
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);

    totalFrames++;

    uint32_t id = (RxHeader.IDE == CAN_ID_STD) ? RxHeader.StdId : RxHeader.ExtId;

    // /* debug print (remove when finished testing) */
    // {
    //     char dbg[80];
    //     int n = sprintf(dbg, "ISR frame id=0x%lX dlc=%d\r\n", id, RxHeader.DLC);
    //     HAL_UART_Transmit(&huart2, (uint8_t*)dbg, n, HAL_MAX_DELAY);
    // }

    if (currentMode == MODE_IDLE) return;

    if (currentMode == MODE_SPEEDTEST)
    {
        speedFrameCount++;
        return;
    }

    if (currentMode == MODE_READ_FILTERED)
    {
        if (!Logger_ShouldLog(id))
        {
            filteredFrames++;
            return;
        }
    }

    // MODE: READ ALL → skip filter

    // Check buffer overflow BEFORE writing
    uint16_t nextIndex = logWriteIndex + 1;
    if (nextIndex >= LOG_BUFFER_SIZE)
        nextIndex = 0;

    if (nextIndex == logReadIndex)
    {
        logOverflowCount++;   // buffer full → drop frame
        return;
    }

    // Store frame in ring buffer
    CAN_LogFrame_t *frame = &logBuffer[logWriteIndex];

    frame->id = id;
    frame->dlc = RxHeader.DLC;

    memcpy(frame->data, rcvd_msg, RxHeader.DLC);

    frame->timestamp = HAL_GetTick();

    logWriteIndex = nextIndex;

    loggedFrames++;
}


void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2)
    {
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);

        if (uartRxChar == 'm' || uartRxChar == 'M')
        {
            currentMode = MODE_IDLE;
            uartState = UART_STATE_MENU;
            uartEvent = UART_EVT_SHOW_MENU;
            HAL_UART_Receive_IT(&huart2, &uartRxChar, 1);
            return;
        }

        switch (uartState)
        {
            case UART_STATE_MENU:
                uartMenuChoice = uartRxChar;
                uartEvent = UART_EVT_MENU_SELECT;
                break;

            case UART_STATE_WAIT_FILTER_ID:
                if (uartRxChar == '\r' || uartRxChar == '\n')
                {
                    filterInputBuffer[filterInputIndex] = '\0';
                    uartEvent = UART_EVT_FILTER_DONE;
                }
                else
                {
                    if (filterInputIndex < sizeof(filterInputBuffer) - 1)
                        filterInputBuffer[filterInputIndex++] = uartRxChar;
                }
                break;

            case UART_STATE_WAIT_TX_FRAME:
                if (uartRxChar == '\r' || uartRxChar == '\n')
                {
                    txInputBuffer[txInputIndex] = '\0';
                    uartEvent = UART_EVT_TX_DONE;
                }
                else
                {
                    if (txInputIndex < sizeof(txInputBuffer) - 1)
                        txInputBuffer[txInputIndex++] = uartRxChar;
                }
                break;

            case UART_STATE_IDLE:
            default:
                break;
        }

        HAL_UART_Receive_IT(&huart2, &uartRxChar, 1);
    }
}



void HandleUartEvent(void)
{
    UART_Event_t evt;

    __disable_irq();
    evt = uartEvent;
    uartEvent = UART_EVT_NONE;
    __enable_irq();

    if (evt == UART_EVT_NONE)
        return;

    switch (evt)
    {
        case UART_EVT_SHOW_MENU:
            PrintMenu();
            break;

        case UART_EVT_MENU_SELECT:
            switch (uartMenuChoice)
            {
                case '1':
                    currentMode = MODE_READ_ALL;
                    uartState = UART_STATE_IDLE;
                    HAL_UART_Transmit(&huart2,
                                      (uint8_t*)"Mode: READ ALL\r\n",
                                      16, HAL_MAX_DELAY);
                    break;

                case '2':
                    uartState = UART_STATE_WAIT_FILTER_ID;
                    filterInputIndex = 0;
                    HAL_UART_Transmit(&huart2,
                                      (uint8_t*)"Enter CAN ID (HEX):\r\n",
                                      22, HAL_MAX_DELAY);
                    break;

                case '3':
                    currentMode = MODE_SPEEDTEST;
                    uartState = UART_STATE_IDLE;
                    speedFrameCount = 0;
                    lastSpeedPrintTick = HAL_GetTick();
                    HAL_UART_Transmit(&huart2,
                                      (uint8_t*)"Mode: SPEED TEST\r\n",
                                      18, HAL_MAX_DELAY);
                    break;

                case '4':
                    currentMode = MODE_IDLE;
                    uartState = UART_STATE_IDLE;
                    HAL_UART_Transmit(&huart2,
                                      (uint8_t*)"Mode: IDLE\r\n",
                                      12, HAL_MAX_DELAY);
                    break;

                case '5':
                    uartState = UART_STATE_WAIT_TX_FRAME;
                    txInputIndex = 0;
                    HAL_UART_Transmit(&huart2,
                                      (uint8_t*)"Enter: ID DLC DATA...\r\n",
                                      24, HAL_MAX_DELAY);
                    break;

                default:
                    HAL_UART_Transmit(&huart2,
                                      (uint8_t*)"Invalid option\r\n",
                                      16, HAL_MAX_DELAY);
                    PrintMenu();
                    break;
            }
            break;

        case UART_EVT_FILTER_DONE:
        {
            userFilterID = strtoul(filterInputBuffer, NULL, 16);
            filterConfigured = 1;
            currentMode = MODE_READ_FILTERED;
            uartState = UART_STATE_IDLE;
            char msg[50];
            sprintf(msg, "Filtering ID: 0x%lX\r\n", userFilterID);
            HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), HAL_MAX_DELAY);
            break;
        }

        case UART_EVT_TX_DONE:
            ProcessTxCommand(txInputBuffer);
            currentMode = MODE_WRITE;
            uartState = UART_STATE_IDLE;
            break;

        default:
            break;
    }
}

void PrintMenu(void)
{
    char menu[] =
        "\r\n==== CAN LOGGER MENU ====\r\n"
        "1 -> Read All Frames\r\n"
        "2 -> Read Filtered Frames\r\n"
        "3 -> Speed Test\r\n"
        "4 -> Idle\r\n"
    	"5 -> Write CAN Frame\r\n"
        "Enter option:\r\n";

    HAL_UART_Transmit(&huart2, (uint8_t*)menu, sizeof(menu) - 1, HAL_MAX_DELAY);
}

uint8_t Logger_ShouldLog(uint32_t id)
{
    if (!filterConfigured)
        return 0;

    if (id == userFilterID)
        return 1;

    return 0;
}

void ProcessTxCommand(char *cmd)
{
    CAN_TxHeaderTypeDef TxHeader;
    uint32_t TxMailbox;
    uint8_t data[8];

    char *token;

    // Parse ID 
    token = strtok(cmd, " ");
    if (token == NULL) return;
    uint32_t id = strtoul(token, NULL, 16);

    // Parse DLC 
    token = strtok(NULL, " ");
    if (token == NULL) return;
    uint8_t dlc = atoi(token);

    if (dlc > 8) dlc = 8;

    // Parse DATA bytes 
    for (uint8_t i = 0; i < dlc; i++)
    {
        token = strtok(NULL, " ");
        if (token == NULL)
            data[i] = 0;
        else
            data[i] = strtoul(token, NULL, 16);
    }

    TxHeader.StdId = id;
    TxHeader.ExtId = 0;
    TxHeader.IDE = CAN_ID_STD;
    TxHeader.RTR = CAN_RTR_DATA;
    TxHeader.DLC = dlc;
    TxHeader.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(&hcan1, &TxHeader, data, &TxMailbox) != HAL_OK)
    {
        HAL_UART_Transmit(&huart2,
                          (uint8_t*)"TX Error\r\n",
                          10,
                          HAL_MAX_DELAY);
        return;
    }

    HAL_UART_Transmit(&huart2,
                      (uint8_t*)"Frame Sent\r\n",
                      12,
                      HAL_MAX_DELAY);
}


void CAN1_FilterConfig(void){
	CAN_FilterTypeDef filter ; 
	filter.FilterActivation = ENABLE ;
	filter.FilterBank = 0;
	filter.FilterFIFOAssignment = CAN_RX_FIFO0;
	filter.FilterIdHigh = 0x0000;
	filter.FilterIdLow = 0x0000;
	filter.FilterMaskIdHigh = 0x0000;
	filter.FilterMaskIdLow = 0x0000;
	filter.FilterMode = CAN_FILTERMODE_IDMASK;
	filter.FilterScale = CAN_FILTERSCALE_32BIT;

	if(HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK ) {
		Error_Handler();
	}
}
