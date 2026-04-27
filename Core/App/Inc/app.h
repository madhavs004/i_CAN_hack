/*
 * app.h
 *
 *  Created on: Feb 1, 2026
 *      Author: madha
 */

#ifndef APP_INC_APP_H_
#define APP_INC_APP_H_

#include <stdint.h>
#include "main.h"

extern CAN_HandleTypeDef hcan1;
extern UART_HandleTypeDef huart2;

//Function Prototypes
void app_main(void);
void CAN1_tx(void);
void CAN1_FilterConfig(void);
void HAL_CAN_RxFifo0MsgPendingCallback(CAN_HandleTypeDef *hcan);
void PrintMenu(void);
void ProcessTxCommand(char *cmd);
void HandleUartEvent(void);
uint8_t Logger_ShouldLog(uint32_t id);

// Flags set in UART RX ISR, handled in main loop
typedef enum {
    UART_EVT_NONE = 0,
    UART_EVT_MENU_SELECT,
    UART_EVT_FILTER_DONE,
    UART_EVT_TX_DONE,
    UART_EVT_INVALID,
    UART_EVT_SHOW_MENU
} UART_Event_t;


//State machine states
typedef enum
{
    MODE_READ_ALL = 0,     // log all frames
    MODE_READ_FILTERED,    // log only selected IDs
    MODE_SPEEDTEST,        // count frames per second
    MODE_IDLE,              // do nothing
	MODE_WRITE
} LoggerMode_t;

// UART state machine
typedef enum {
    UART_STATE_IDLE = 0,
    UART_STATE_MENU,
    UART_STATE_WAIT_FILTER_ID,
    UART_STATE_WAIT_TX_FRAME
} UART_State_t;

//CAN log struct
typedef struct{
	uint32_t id;
	uint8_t  dlc;
	uint8_t  data[8];
	uint32_t timestamp;
}CAN_LogFrame_t;



#endif /* APP_INC_APP_H_ */
