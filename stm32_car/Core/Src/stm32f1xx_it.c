/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32f1xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "stm32f1xx_it.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "jsmn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define UART_RX_BUF_SIZE 256

extern TIM_HandleTypeDef htim4;
extern UART_HandleTypeDef huart3;
extern SemaphoreHandle_t mqtt_ok;

uint8_t UART_dma_rx_buff[UART_RX_BUF_SIZE];    // DMA接收缓冲区
uint8_t UART_FrameBuf[UART_RX_BUF_SIZE];      // 提取后的帧数据
uint16_t UART_FrameLen = 0;
car_status_t car_status;
uint8_t json_buf[256];

void UART_IdleCallback(UART_HandleTypeDef *huart);


/******************************************************************************/
/*           Cortex-M3 Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */

  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */

  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */

  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32F1xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32f1xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM4 global interrupt.
  */
void TIM4_IRQHandler(void)
{
  /* USER CODE BEGIN TIM4_IRQn 0 */

  /* USER CODE END TIM4_IRQn 0 */
  HAL_TIM_IRQHandler(&htim4);
  /* USER CODE BEGIN TIM4_IRQn 1 */

  /* USER CODE END TIM4_IRQn 1 */
}

/* USER CODE BEGIN 1 */
void USART3_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart3);
		if (__HAL_UART_GET_FLAG(&huart3, UART_FLAG_IDLE)) {
			__HAL_UART_CLEAR_IDLEFLAG(&huart3);

			UART_IdleCallback(&huart3);
    }
}

static int jsoneq(const char *json, jsmntok_t *tok, const char *s)
{
    if (tok->type == JSMN_STRING &&
        (int)strlen(s) == (tok->end - tok->start) &&
        strncmp(json + tok->start, s, tok->end - tok->start) == 0)
    {
        return 0; // match
    }
    return -1;
}


int json_to_car_status(const char *json, car_status_t *status)
{
    jsmn_parser parser;
    jsmntok_t tokens[32];  // 足够解析你的 JSON
    int ret, i;

    jsmn_init(&parser);
    ret = jsmn_parse(&parser, json, strlen(json), tokens, 32);

    if (ret < 0)
        return -1; // JSON 格式错误

    if (ret < 1 || tokens[0].type != JSMN_OBJECT)
        return -2; // 根节点不是对象

    for (i = 1; i < ret; i++)
    {
        if (jsoneq(json, &tokens[i], "current_speed") == 0)
        {
            status->current_speed = atoi(json + tokens[i + 1].start);
            i++;
        }
        else if (jsoneq(json, &tokens[i], "speed_max") == 0)
        {
            status->speed_max = atoi(json + tokens[i + 1].start);
            i++;
        }
        else if (jsoneq(json, &tokens[i], "direction") == 0)
        {
            status->direction = atoi(json + tokens[i + 1].start);
            i++;
        }
    }

    return 0; // success
}


void UART_FrameReceived(uint8_t *data, uint16_t len)
{
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		
		printf("mqtt data:%s\n", data);
		if(strstr((char *)data, "OK") != NULL)
		{
			xSemaphoreGiveFromISR(mqtt_ok, &xHigherPriorityTaskWoken);
		}
		else if(strstr((char *)data, "WIFI GOT IP") != NULL)
		{
			xSemaphoreGiveFromISR(mqtt_ok, &xHigherPriorityTaskWoken);
		}
		else if(strstr((char *)data, "ERROR") != NULL)
		{
			printf("mqtt err\n");
		}
		else if(strstr((char *)data, "MQTTSUBRECV") != NULL)
		{
			printf("mqtt receive!!!\n");
			
			char *json_start = strchr((char *)data, '{');   // 查找 JSON 起点
			char *json_end   = strrchr((char *)data, '}');  // 查找 JSON 终点

			if(json_start && json_end)
			{
					int json_len = json_end - json_start + 1;
					memcpy(json_buf, json_start, json_len);
					json_buf[json_len] = '\0';   // 加字符串结束符
			}

			
			json_to_car_status((char *)json_buf, &car_status);
			printf("car_status.current_speed = %d\n", car_status.current_speed);
			printf("car_status.direction = %d\n", car_status.direction);
			printf("car_status.speed_max = %d\n", car_status.speed_max);
		}
		memset(UART_FrameBuf, 0, UART_RX_BUF_SIZE);
		
}


void UART_IdleCallback(UART_HandleTypeDef *huart)
{
    // 停止DMA
    HAL_UART_DMAStop(huart);

    // 获取当前接收的字节数
    UART_FrameLen = UART_RX_BUF_SIZE - __HAL_DMA_GET_COUNTER(huart->hdmarx);

    // 复制数据到FrameBuf
    memcpy(UART_FrameBuf, UART_dma_rx_buff, UART_FrameLen);

    // 重新开启DMA接收
    HAL_UART_Receive_DMA(huart, UART_dma_rx_buff, UART_RX_BUF_SIZE);

    // 处理帧数据
    UART_FrameReceived(UART_FrameBuf, UART_FrameLen);
}



