#pragma once

#define FRAMEWORK_ARDUINO 1
#define FRAMEWORK_ESP_IDF 2
#define FRAMEWORK_USED FRAMEWORK_ESP_IDF

//####  MAIN CONFIG  ####
#define BOOT_WAIT 2000
#define MAIN_WAIT 100 /** Time in ms between every main loop cycle */

//----------------------------------

//#### CANOPEN CONFIG ####
#define CAN_BITRATE 500    /* kbit */
#define NODE_ID_SELF 0x42
#define NODE_ID_LINMOT 0x3F
//----------------------------------

#define UART_PORT UART_NUM_0
#define UART_TX_PIN GPIO_NUM_33 /** Serial TX Pinnummber */
#define UART_RX_PIN GPIO_NUM_32 /** Serial RX Pinnummber */