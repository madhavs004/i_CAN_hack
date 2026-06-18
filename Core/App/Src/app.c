/*
 * app.c
 *
 * CAN logger helpers shared by the FreeRTOS tasks in main.c:
 * mode/filter state, the console menu, CAN TX, and the hardware filter setup.
 */

#include "app.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* Operating mode + console state machine (driven by CliTask). */
LoggerMode_t currentMode = MODE_READ_ALL;
UART_State_t uartState   = UART_STATE_MENU;

/* Speed-test frame counter (per-second window). */
volatile uint32_t speedFrameCount = 0;
uint32_t lastSpeedPrintTick = 0;

/* Active read filter. */
uint32_t userFilterID = 0;
uint8_t  filterConfigured = 0;

/* Console input buffers. */
char    filterInputBuffer[10];
uint8_t filterInputIndex = 0;
char    txInputBuffer[50];
uint8_t txInputIndex = 0;

void PrintMenu(void)
{
    /* Split across two Log_Send calls: the full menu exceeds one LogMsg_t. */
    Log_Send("\r\n==== CAN LOGGER MENU ====\r\n"
             "1 -> Read All Frames\r\n"
             "2 -> Read Filtered Frames\r\n");
    Log_Send("3 -> Speed Test\r\n"
             "4 -> Idle\r\n"
             "5 -> Write CAN Frame\r\n"
             "Enter option:\r\n");
}

uint8_t Logger_ShouldLog(uint32_t id)
{
    return (filterConfigured && id == userFilterID) ? 1 : 0;
}

/* Parse "ID DLC D0 D1 ..." (hex) from the console and transmit one CAN frame. */
void ProcessTxCommand(char *cmd)
{
    CAN_TxHeaderTypeDef TxHeader;
    uint32_t TxMailbox;
    uint8_t data[8];
    char *token;

    token = strtok(cmd, " ");
    if (token == NULL) return;
    uint32_t id = strtoul(token, NULL, 16);

    token = strtok(NULL, " ");
    if (token == NULL) return;
    uint8_t dlc = atoi(token);
    if (dlc > 8) dlc = 8;

    for (uint8_t i = 0; i < dlc; i++)
    {
        token = strtok(NULL, " ");
        data[i] = (token == NULL) ? 0 : (uint8_t)strtoul(token, NULL, 16);
    }

    TxHeader.StdId = id;
    TxHeader.ExtId = 0;
    TxHeader.IDE = CAN_ID_STD;
    TxHeader.RTR = CAN_RTR_DATA;
    TxHeader.DLC = dlc;
    TxHeader.TransmitGlobalTime = DISABLE;

    if (HAL_CAN_AddTxMessage(&hcan1, &TxHeader, data, &TxMailbox) != HAL_OK)
    {
        Log_Send("TX Error\r\n");
        return;
    }
    Log_Send("Frame Sent\r\n");
}

/* Accept-all hardware filter into RX FIFO0; mode logic filters in software. */
void CAN1_FilterConfig(void)
{
    CAN_FilterTypeDef filter;
    filter.FilterActivation = ENABLE;
    filter.FilterBank = 0;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterIdHigh = 0x0000;
    filter.FilterIdLow = 0x0000;
    filter.FilterMaskIdHigh = 0x0000;
    filter.FilterMaskIdLow = 0x0000;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;

    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK)
    {
        Error_Handler();
    }
}
