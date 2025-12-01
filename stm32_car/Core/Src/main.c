/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "adc.h"
#include "i2c.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"
#include "task_mqtt.h"
#include "task_oled.h"
#include "task_misc.h"
#include "OLED.h"
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
void StartDefaultTask(void const * argument);
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];
static TaskHandle_t Mqtt_Task_Handle = NULL;
TaskHandle_t OLED_Task_Handle = NULL;
static TaskHandle_t Misc_Task_Handle = NULL;

SemaphoreHandle_t mqtt_ok;
SemaphoreHandle_t mqtt_resond;

QueueHandle_t Queue_car_recv = NULL;
QueueHandle_t Queue_car_send = NULL;



int main(void)
{
	
  HAL_Init();

  SystemClock_Config();

  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_SPI2_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_USART2_UART_Init();
	USART3_UART_Init();
	printf("app runing\n");

	
	Queue_car_recv = xQueueCreate(2, sizeof(car_status_t));
	if(Queue_car_recv == 0)
	{
		Error_Handler();
	}
	Queue_car_send = xQueueCreate(2, sizeof(car_status_t));
	if(Queue_car_send == 0)
	{
		Error_Handler();
	}
	
	
	
	/*	超声波雷达测距。配合舵机可以扫射前方一定角度的障碍物距离。
	while(1)
	{
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_SET);
		HAL_Delay(1);
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_12, GPIO_PIN_RESET);
		
		HAL_Delay(100);
		//printf("%d\n", __HAL_TIM_GetCompare(&htim3, TIM_CHANNEL_3));
	}
	
	//对射传感器计数，可测小车移动速度
	while(1)
	{
		printf("counter:%d\n", __HAL_TIM_GetCounter(&htim2));
		HAL_Delay(100);
	}*/
	
	
	
	mqtt_ok = xSemaphoreCreateBinary();
	mqtt_resond = xSemaphoreCreateBinary();
	if(xTaskCreate((TaskFunction_t)Task_Mqtt, "Task_Mqtt", 256, NULL, 3, &Mqtt_Task_Handle) != pdPASS)
	{
		Error_Handler();
	}
	printf("free heap: %d\n", xPortGetFreeHeapSize());
	if(xTaskCreate((TaskFunction_t)Task_OLED, "Task_OLED", 256, NULL, 3, &OLED_Task_Handle) != pdPASS)
	{
		Error_Handler();
	} 
	
	if(xTaskCreate((TaskFunction_t)Task_Misc, "Task_Misc", 256, NULL, 3, &Misc_Task_Handle) != pdPASS)
	{
		Error_Handler();
	}

	vTaskStartScheduler();

	Error_Handler();
}

//内存不足钩子
void vApplicationMallocFailedHook(void)
{
    //printf("FreeRTOS malloc failed!\r\n");
    taskDISABLE_INTERRUPTS();
    for(;;);
}
//堆栈溢出检测钩子
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    //printf("Stack overflow in %s\r\n", pcTaskName);
    taskDISABLE_INTERRUPTS();
    for(;;);
}

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
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
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM4 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */


/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
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
