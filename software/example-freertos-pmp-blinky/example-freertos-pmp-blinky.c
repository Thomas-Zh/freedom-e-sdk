/* Copyright 2019 SiFive, Inc */
/* SPDX-License-Identifier: Apache-2.0 */



/******************************************************************************
 *
 * main() creates one queue, and two tasks.  It then starts the
 * scheduler.
 *
 * The Queue Send Task:
 * The queue send task is implemented by the prvQueueSendTask() function in
 * this file.  prvQueueSendTask() sits in a loop that causes it to repeatedly
 * block for 1000 milliseconds, before sending the value 100 to the queue that
 * was created within main().  Once the value is sent, the task loops
 * back around to block for another 1000 milliseconds...and so on.
 *
 * The Queue Receive Task:
 * The queue receive task is implemented by the prvQueueReceiveTask() function
 * in this file.  prvQueueReceiveTask() sits in a loop where it repeatedly
 * blocks on attempts to read data from the queue that was created within
 * blinky().  When data is received, the task checks the value of the
 * data, and if the value equals the expected 100, writes 'Blink' to the UART
 * (the UART is used in place of the LED to allow easy execution in QEMU).  The
 * 'block time' parameter passed to the queue receive function specifies that
 * the task should be held in the Blocked state indefinitely to wait for data to
 * be available on the queue.  The queue receive task will only leave the
 * Blocked state when the queue send task writes to the queue.  As the queue
 * send task writes to the queue every 1000 milliseconds, the queue receive
 * task leaves the Blocked state every 1000 milliseconds, and therefore toggles
 * the LED every 1 second.
 */

/* Standard includes. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Freedom metal includes. */
#include <metal/machine.h>
#include <metal/machine/platform.h>

#include <metal/lock.h>
#include <metal/uart.h>
#include <metal/interrupt.h>
#include <metal/clock.h>
#include <metal/led.h>
#include <metal/gpio.h>

extern struct metal_led *led0_red, *led0_green, *led0_blue;

extern pmp_info_t xPmpInfo;

/* Priorities used by the tasks. */
#define mainQUEUE_RECEIVE_TASK_PRIORITY		( tskIDLE_PRIORITY + 2 )
#define	mainQUEUE_SEND_TASK_PRIORITY		( tskIDLE_PRIORITY + 1 )

#ifndef _RTL_
/* The 1s value is converted to ticks using the pdMS_TO_TICKS() macro. */
#define mainQUEUE_TICK_COUNT_FOR_1S			pdMS_TO_TICKS( 1000 )
#else
/* For RTL Simulation we reduce the waiting timing, otherwise the simulation 
 * will take too more time */
/* The 10ms value is converted to ticks using the pdMS_TO_TICKS() macro. */
#define mainQUEUE_TICK_COUNT_FOR_1S			pdMS_TO_TICKS( 10 )
#endif
/* The maximum number items the queue can hold.  The priority of the receiving
task is above the priority of the sending task, so the receiving task will
preempt the sending task and remove the queue items each time the sending task
writes to the queue.  Therefore the queue will never have more than one item in
it at any time, and even with a queue length of 1, the sending task will never
find the queue full. */
#define mainQUEUE_LENGTH					( 1 )

#if( portUSING_MPU_WRAPPERS == 1 )
/**
 * @brief Calls the port specific code to raise the privilege.
 *
 * @return pdFALSE if privilege was raised, pdTRUE otherwise.
 */
BaseType_t xPortRaisePrivilege( void ) FREERTOS_SYSTEM_CALL;
extern pmp_info_t xPmpInfo;
#endif

/*-----------------------------------------------------------*/
/*
 * Functions:
 * 		- prvSetupHardware: Setup Hardware according CPU and Board.
 */
static void prvSetupHardware( void );

/*
 * The tasks as described in the comments at the top of this file.
 */
static void prvQueueReceiveTask( void *pvParameters );
static void prvQueueSendTask( void *pvParameters );
/*-----------------------------------------------------------*/

/* The queue used by both tasks. */
static QueueHandle_t xQueue = NULL;

struct metal_cpu *cpu0;
struct metal_interrupt *cpu_intr, *tmr_intr;
struct metal_led *led0_red, *led0_green, *led0_blue;

#define LED_ERROR ((led0_red == NULL) || (led0_green == NULL) || (led0_blue == NULL))

/*-----------------------------------------------------------*/
int main( void )
{
	TaskHandle_t xHandle_ReceiveTask, xHandle_SendTask;
	const char * const pcMessage = "FreeRTOS-PMP Demo start\r\n";
	const char * const pcMessageEnd = "FreeRTOS-PMP Demo end\r\n";
	const char * const pcMessageEndError = "FreeRTOS-PMP Demo end - Error no enough PMP entry\r\n";
    const char * const pcMessageGranularityError = "FreeRTOS-PMP Demo end - Error platform granularity no supported\r\n";

	prvSetupHardware();
	write( STDOUT_FILENO, pcMessage, strlen( pcMessage ) );
	
#if( portUSING_MPU_WRAPPERS == 1 )
	if (xPmpInfo.nb_pmp < 8)
	{
		write( STDOUT_FILENO, pcMessageEndError, strlen( pcMessageEndError ) );
		_exit(0);
	} else if (xPmpInfo.granularity > 4) {
        /* 
         * platfrom granularity > 4 bytes is not supported yet, some
         * modifications are needed on FreeRTOS port to do so.
         */
        write( STDOUT_FILENO, pcMessageGranularityError, strlen( pcMessageGranularityError ) );
		_exit(0);
    }
	/* Create the queue. */
	xQueue = xQueueCreate( mainQUEUE_LENGTH, sizeof( uint32_t ) );

	if( xQueue != NULL )
	{
		extern uint32_t __unprivileged_data_section_start__[];
		extern uint32_t __unprivileged_data_section_end__[];

		static TaskParameters_t xTaskRXDefinition =
		{
			.pvTaskCode = prvQueueReceiveTask,
			.pcName = "Rx",
			.usStackDepth = 0x200,
			.pvParameters = NULL,
			.uxPriority = mainQUEUE_RECEIVE_TASK_PRIORITY,
			.puxStackBuffer = NULL,
		};

		static TaskParameters_t xTaskTXDefinition =
		{
			.pvTaskCode = prvQueueSendTask,
			.pcName = "Tx",
			.usStackDepth = 0x200,
			.pvParameters = NULL,
			.uxPriority = mainQUEUE_SEND_TASK_PRIORITY,
			.puxStackBuffer = NULL,
		};

        if(0 == xPmpInfo.granularity) 
		{
		    init_pmp (&xPmpInfo);
        }

		/* 
		 * Prepare xRegions for Receive Task 
		 */
        memset(&xTaskRXDefinition.xRegions, 0, sizeof(xTaskRXDefinition.xRegions));

        // authorize access to data and bss
		// 		Low address 
        xTaskRXDefinition.xRegions[0].ulLengthInBytes = 4;
        xTaskRXDefinition.xRegions[0].ulParameters = ((portPMP_REGION_READ_WRITE) |
                                                    (portPMP_REGION_ADDR_MATCH_NA4));
        addr_modifier ( xPmpInfo.granularity,
                        ( size_t ) __unprivileged_data_section_start__,
                        (size_t *) &xTaskRXDefinition.xRegions[0].pvBaseAddress);

		// 		High address 
        xTaskRXDefinition.xRegions[1].ulLengthInBytes = 4;
        xTaskRXDefinition.xRegions[1].ulParameters = ((portPMP_REGION_READ_WRITE) |
                                                    (portPMP_REGION_ADDR_MATCH_TOR));

        addr_modifier ( xPmpInfo.granularity,
                        ( size_t ) __unprivileged_data_section_end__,
                        (size_t *) &xTaskRXDefinition.xRegions[1].pvBaseAddress);

#ifdef METAL_SIFIVE_UART0
    // allow access to UART peripheral
        xTaskRXDefinition.xRegions[2].ulLengthInBytes = METAL_SIFIVE_UART0_0_SIZE;
        xTaskRXDefinition.xRegions[2].ulParameters = ((portPMP_REGION_READ_WRITE) |
                                                     (portPMP_REGION_ADDR_MATCH_NAPOT));

        napot_addr_modifier (	xPmpInfo.granularity,
                                (size_t) METAL_SIFIVE_UART0_0_BASE_ADDRESS,
                                (size_t *) &xTaskRXDefinition.xRegions[2].pvBaseAddress,
                                xTaskRXDefinition.xRegions[2].ulLengthInBytes);
#endif /* METAL_SIFIVE_UART0 */

        // allocate stack (It will take 2 PMP Slot - So it is not needed to put align the StackBuffer)
        xTaskRXDefinition.puxStackBuffer = ( StackType_t * ) pvPortMalloc( xTaskRXDefinition.usStackDepth * sizeof( StackType_t ) );

        xTaskCreateRestricted(  &xTaskRXDefinition,
                                &xHandle_ReceiveTask);


		/*
		 * Prepare xRegions for Send Task 
		 */
        memset(&xTaskTXDefinition.xRegions, 0, sizeof(xTaskTXDefinition.xRegions));

        // authorize access to data and bss
		// 		Low address 
        xTaskTXDefinition.xRegions[0].ulLengthInBytes = 4;
        xTaskTXDefinition.xRegions[0].ulParameters = ((portPMP_REGION_READ_WRITE) |
                                                    (portPMP_REGION_ADDR_MATCH_NA4));
        addr_modifier ( xPmpInfo.granularity,
                        ( size_t ) __unprivileged_data_section_start__,
                        (size_t *) &xTaskTXDefinition.xRegions[0].pvBaseAddress);

		// 		High address 
        xTaskTXDefinition.xRegions[1].ulLengthInBytes = 4;
        xTaskTXDefinition.xRegions[1].ulParameters = ((portPMP_REGION_READ_WRITE) |
                                                    (portPMP_REGION_ADDR_MATCH_TOR));

        addr_modifier ( xPmpInfo.granularity,
                        ( size_t ) __unprivileged_data_section_end__,
                        (size_t *) &xTaskTXDefinition.xRegions[1].pvBaseAddress);

#ifdef METAL_SIFIVE_GPIO0
        // allow access to GPIO (Each peripheral are on 4Kb mapping area)
        xTaskTXDefinition.xRegions[2].ulLengthInBytes = METAL_SIFIVE_GPIO0_0_SIZE;
        xTaskTXDefinition.xRegions[2].ulParameters = ((portPMP_REGION_READ_WRITE) |
                                                     (portPMP_REGION_ADDR_MATCH_NAPOT));

        napot_addr_modifier (	xPmpInfo.granularity,
                                (size_t) METAL_SIFIVE_GPIO0_0_BASE_ADDRESS,
                                (size_t *) &xTaskTXDefinition.xRegions[2].pvBaseAddress,
                                xTaskTXDefinition.xRegions[2].ulLengthInBytes);
#endif /* METAL_SIFIVE_GPIO0 */
        // allocate stack (It will take 2 PMP Slot - So it is not needed to put align the StackBuffer)
        xTaskTXDefinition.puxStackBuffer = ( StackType_t * ) pvPortMalloc( xTaskTXDefinition.usStackDepth * sizeof( StackType_t ) );

        xTaskCreateRestricted(  &xTaskTXDefinition,
                                &xHandle_SendTask);

		/* Start the tasks and timer running. */
		vTaskStartScheduler();

	/* If all is well, the scheduler will now be running, and the following
	line will never be reached.  If the following line does execute, then
	there was insufficient FreeRTOS heap memory available for the Idle and/or
	timer tasks to be created. 
	or task have stoppped the Scheduler */

		vTaskDelete( xHandle_SendTask );
		vTaskDelete( xHandle_ReceiveTask );
	}
#endif /* ( portUSING_MPU_WRAPPERS == 1 ) */
	write( STDOUT_FILENO, pcMessageEnd, strlen( pcMessageEnd ) );

}
/*-----------------------------------------------------------*/

static void prvQueueSendTask( void *pvParameters )
{
	TickType_t xNextWakeTime;
	const unsigned long ulValueToSend = 100UL;
	BaseType_t xReturned;
	unsigned int i;

	/* Remove compiler warning about unused parameter. */
	( void ) pvParameters;
	( void ) xReturned;

	/* Initialise xNextWakeTime - this only needs to be done once. */
	xNextWakeTime = xTaskGetTickCount();

	/* For automation test process we limite the number of message to send to 5 then we exit the program */
	for( i=0 ; i<5 ; i++)
	{
		if ( led0_green != NULL ) 
		{
			/* Switch off the Green led */
			metal_led_toggle(led0_green);
		}

		/* Place this task in the blocked state until it is time to run again. */
		vTaskDelayUntil( &xNextWakeTime, mainQUEUE_TICK_COUNT_FOR_1S );

		/* Send to the queue - causing the queue receive task to unblock and
		toggle the LED.  0 is used as the block time so the sending operation
		will not block - it shouldn't need to block as the queue should always
		be empty at this point in the code. */
		xReturned = xQueueSend( xQueue, &ulValueToSend, 0U );
		configASSERT( xReturned == pdPASS );
	}

	/** 
	 * SiFive CI/CD need to have a exit(0) status to pass
	 */
#if( portUSING_MPU_WRAPPERS == 1 )
	/* We run into user mode, so need to be machine mode before to call vTaskEndScheduler */
	xPortRaisePrivilege();
#endif /* ( portUSING_MPU_WRAPPERS == 1 ) */
	vTaskEndScheduler();
}
/*-----------------------------------------------------------*/

static void prvQueueReceiveTask( void *pvParameters )
{
	unsigned long ulReceivedValue;
	const unsigned long ulExpectedValue = 100UL;
	const char * const pcPassMessage = "Blink\r\n";
	const char * const pcFailMessage = "Unexpected value received\r\n";

	/* Remove compiler warning about unused parameter. */
	( void ) pvParameters;

	for( ;; )
	{
		/* Wait until something arrives in the queue - this task will block
		indefinitely provided INCLUDE_vTaskSuspend is set to 1 in
		FreeRTOSConfig.h. */
		xQueueReceive( xQueue, &ulReceivedValue, portMAX_DELAY );

		/*  To get here something must have been received from the queue, but
		is it the expected value?  If it is, toggle the LED. */
		if( ulReceivedValue == ulExpectedValue )
		{
			write( STDOUT_FILENO, pcPassMessage, strlen( pcPassMessage ) );
			ulReceivedValue = 0U;
		}
		else
		{
			write( STDOUT_FILENO, pcFailMessage, strlen( pcFailMessage ) );
		}
	}
}
/*-----------------------------------------------------------*/

static void prvSetupHardware( void )
{
	const char * const pcWarningMsg = "At least one of LEDs is null.\r\n";

	// This demo will toggle LEDs colors so we define them here
	led0_red = metal_led_get_rgb("LD0", "red");
	led0_green = metal_led_get_rgb("LD0", "green");
	led0_blue = metal_led_get_rgb("LD0", "blue");
	if ((led0_red == NULL) || (led0_green == NULL) || (led0_blue == NULL))
	{
		write( STDOUT_FILENO, pcWarningMsg, strlen( pcWarningMsg ) );
	}
	else
	{
		// Enable each LED
		metal_led_enable(led0_red);
		metal_led_enable(led0_green);
		metal_led_enable(led0_blue);

		// All Off
		metal_led_on(led0_red);
		metal_led_on(led0_green);
		metal_led_on(led0_blue);
	}
}
/*-----------------------------------------------------------*/


void vApplicationMallocFailedHook( void )
{
	/* vApplicationMallocFailedHook() will only be called if
	configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h.  It is a hook
	function that will get called if a call to pvPortMalloc() fails.
	pvPortMalloc() is called internally by the kernel whenever a task, queue,
	timer or semaphore is created.  It is also called by various parts of the
	demo application.  If heap_1.c or heap_2.c are used, then the size of the
	heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
	FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
	to query the size of free heap space that remains (although it does not
	provide information on how the remaining heap might be fragmented). */

	const char * const pcErrorMsg = "ERROR malloc \r\n";

	taskDISABLE_INTERRUPTS();

#if( portUSING_MPU_WRAPPERS == 1 )
	/* need to be machine mode */
	xPortRaisePrivilege();
#endif /* ( portUSING_MPU_WRAPPERS == 1 ) */
	write( STDOUT_FILENO, pcErrorMsg, strlen(pcErrorMsg) );

	if ( led0_red != NULL )
	{
		// Red light on
		metal_led_off(led0_red);
	}

	_exit(1);
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
	/* vApplicationIdleHook() will only be called if configUSE_IDLE_HOOK is set
	to 1 in FreeRTOSConfig.h.  It will be called on each iteration of the idle
	task.  It is essential that code added to this hook function never attempts
	to block in any way (for example, call xQueueReceive() with a block time
	specified, or call vTaskDelay()).  If the application makes use of the
	vTaskDelete() API function (as this demo application does) then it is also
	important that vApplicationIdleHook() is permitted to return to its calling
	function, because it is the responsibility of the idle task to clean up
	memory allocated by the kernel to any task that has since been deleted. */
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected. */
	taskDISABLE_INTERRUPTS();

	write( STDOUT_FILENO, "ERROR Stack overflow on func: ", 30 );
	write( STDOUT_FILENO, pcTaskName, strlen( pcTaskName ) );
	write( STDOUT_FILENO, "\r\n", 3 );

	if ( led0_red != NULL )
	{
		// Red light on
		metal_led_off(led0_red);
	}

	_exit(1);
}
/*-----------------------------------------------------------*/

void vApplicationTickHook( void )
{
	/* The tests in the full demo expect some interaction with interrupts. */
}
/*-----------------------------------------------------------*/

void vAssertCalled( void )
{
	taskDISABLE_INTERRUPTS();

	if ( led0_red != NULL )
	{
		// Red light on
		metal_led_off(led0_red);
	}

	_exit(1);
}
