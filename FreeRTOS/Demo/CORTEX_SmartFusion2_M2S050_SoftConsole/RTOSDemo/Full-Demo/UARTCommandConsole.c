/*
    FreeRTOS V7.1.0 - Copyright (C) 2011 Real Time Engineers Ltd.


    ***************************************************************************
     *                                                                       *
     *    FreeRTOS tutorial books are available in pdf and paperback.        *
     *    Complete, revised, and edited pdf reference manuals are also       *
     *    available.                                                         *
     *                                                                       *
     *    Purchasing FreeRTOS documentation will not only help you, by       *
     *    ensuring you get running as quickly as possible and with an        *
     *    in-depth knowledge of how to use FreeRTOS, it will also help       *
     *    the FreeRTOS project to continue with its mission of providing     *
     *    professional grade, cross platform, de facto standard solutions    *
     *    for microcontrollers - completely free of charge!                  *
     *                                                                       *
     *    >>> See http://www.FreeRTOS.org/Documentation for details. <<<     *
     *                                                                       *
     *    Thank you for using FreeRTOS, and thank you for your support!      *
     *                                                                       *
    ***************************************************************************


    This file is part of the FreeRTOS distribution.

    FreeRTOS is free software; you can redistribute it and/or modify it under
    the terms of the GNU General Public License (version 2) as published by the
    Free Software Foundation AND MODIFIED BY the FreeRTOS exception.
    >>>NOTE<<< The modification to the GPL is included to allow you to
    distribute a combined work that includes FreeRTOS without being obliged to
    provide the source code for proprietary components outside of the FreeRTOS
    kernel.  FreeRTOS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
    more details. You should have received a copy of the GNU General Public
    License and the FreeRTOS license exception along with FreeRTOS; if not it
    can be viewed here: http://www.freertos.org/a00114.html and also obtained
    by writing to Richard Barry, contact details for whom are available on the
    FreeRTOS WEB site.

    1 tab == 4 spaces!

    http://www.FreeRTOS.org - Documentation, latest information, license and
    contact details.

    http://www.SafeRTOS.com - A version that is certified for use in safety
    critical systems.

    http://www.OpenRTOS.com - Commercial support, development, porting,
    licensing and training services.
*/

/* Standard includes. */
#include "string.h"
#include "stdio.h"

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Driver includes. */
#include "drivers/mss_uart/mss_uart.h"

/* Example includes. */
#include "FreeRTOS_CLI.h"
#include "UARTCommandConsole.h"

/* Dimensions the buffer into which input characters are placed. */
#define cmdMAX_INPUT_SIZE		50

/* The maximum time in ticks to wait for the UART access mutex. */
#define cmdMAX_MUTEX_WAIT		( 200 / portTICK_RATE_MS )

/* Characters are only ever received slowly on the CLI so it is ok to pass
received characters from the UART interrupt to the task on a queue.  This sets
the length of the queue used for that purpose. */
#define cmdRXED_CHARS_QUEUE_LENGTH			( 10 )

/*-----------------------------------------------------------*/

/*
 * The task that implements the command console processing.
 */
static void prvUARTCommandConsoleTask( void *pvParameters );

/*
 * Ensure a previous interrupt driven Tx has completed before sending the next
 * data block to the UART.
 */
static void prvSendBuffer( const uint8_t * pcBuffer, size_t xBufferLength );

/*
 * A UART is used for printf() output and CLI input and output.  Configure the
 * UART and register prvUARTRxNotificationHandler() to handle UART Rx events.
 */
static void prvConfigureUART( void );
static void prvUARTRxNotificationHandler( mss_uart_instance_t * this_uart );

/*-----------------------------------------------------------*/

/* Const messages output by the command console. */
static const uint8_t * const pcWelcomeMessage = ( uint8_t * ) "\r\n\r\nFreeRTOS command server.\r\nType Help to view a list of registered commands.\r\n\r\n>";
static const uint8_t * const pcEndOfOutputMessage = ( uint8_t * ) "\r\n[Press ENTER to execute the previous command again]\r\n>";
static const uint8_t * const pcNewLine = ( uint8_t * ) "\r\n";

/* The UART used by the CLI. */
#if configBUILD_FOR_DEVELOPMENT_KIT == 1
	static const mss_uart_instance_t * const pxUART = &g_mss_uart1;
	static const IRQn_Type xUART_IRQ = UART1_IRQn;
#else
	static const mss_uart_instance_t * const pxUART = &g_mss_uart0;
	static const IRQn_Type xUART_IRQ = UART0_IRQn;
#endif

/* Because characters are received slowly (at the speed somebody can type) then
it is ok to pass received characters from the Rx interrupt to the task on a
queue.  This is the queue used for that purpose. */
static xQueueHandle xRxedChars = NULL;

/*-----------------------------------------------------------*/

void vUARTCommandConsoleStart( uint16_t usStackSize, unsigned portBASE_TYPE uxPriority )
{
	/* A UART is used for printf() output and CLI input and output.  Note there
	is no mutual exclusion on the UART, but the demo as it stands does not
	require mutual exclusion. */
	prvConfigureUART();

	/* Create that task that handles the console itself. */
	xTaskCreate( 	prvUARTCommandConsoleTask,			/* The task that implements the command console. */
					( const int8_t * const ) "CLI",		/* Text name assigned to the task.  This is just to assist debugging.  The kernel does not use this name itself. */
					usStackSize,						/* The size of the stack allocated to the task. */
					NULL,								/* The parameter is not used, so NULL is passed. */
					uxPriority,							/* The priority allocated to the task. */
					NULL );								/* A handle is not required, so just pass NULL. */
}
/*-----------------------------------------------------------*/

static void prvUARTCommandConsoleTask( void *pvParameters )
{
int8_t cRxedChar, cInputIndex = 0, *pcOutputString;
static int8_t cInputString[ cmdMAX_INPUT_SIZE ], cLastInputString[ cmdMAX_INPUT_SIZE ];
portBASE_TYPE xReturned;

	( void ) pvParameters;

	/* Obtain the address of the output buffer.  Note there is no mutual
	exclusion on this buffer as it is assumed only one command console
	interface will be used at any one time. */
	pcOutputString = FreeRTOS_CLIGetOutputBuffer();

	/* Send the welcome message. */
	prvSendBuffer( pcWelcomeMessage, strlen( ( char * ) pcWelcomeMessage ) );

	for( ;; )
	{
		/* Wait for the next character to arrive. */
		if( xQueueReceive( xRxedChars, &cRxedChar, portMAX_DELAY ) == pdPASS )
		{
			/* Echo the character back. */
			prvSendBuffer( ( uint8_t * ) &cRxedChar, sizeof( cRxedChar ) );

			/* Was it the end of the line? */
			if( cRxedChar == '\n' || cRxedChar == '\r' )
			{
				/* Just to space the output from the input. */
				prvSendBuffer( ( uint8_t * ) pcNewLine, strlen( ( char * ) pcNewLine ) );

				/* See if the command is empty, indicating that the last command is
				to be executed again. */
				if( cInputIndex == 0 )
				{
					/* Copy the last command back into the input string. */
					strcpy( ( char * ) cInputString, ( char * ) cLastInputString );
				}

				/* Pass the received command to the command interpreter.  The
				command interpreter is called repeatedly until it returns pdFALSE
				(indicating there is no more output) as it might generate more than
				one string. */
				do
				{
					/* Get the next output string from the command interpreter. */
					xReturned = FreeRTOS_CLIProcessCommand( cInputString, pcOutputString, configCOMMAND_INT_MAX_OUTPUT_SIZE );

					/* Write the generated string to the UART. */
					prvSendBuffer( ( uint8_t * ) pcOutputString, strlen( ( char * ) pcOutputString ) );

				} while( xReturned != pdFALSE );

				/* All the strings generated by the input command have been sent.
				Clear the input	string ready to receive the next command.  Remember
				the command that was just processed first in case it is to be
				processed again. */
				strcpy( ( char * ) cLastInputString, ( char * ) cInputString );
				cInputIndex = 0;
				memset( cInputString, 0x00, cmdMAX_INPUT_SIZE );

				prvSendBuffer( ( uint8_t * ) pcEndOfOutputMessage, strlen( ( char * ) pcEndOfOutputMessage ) );
			}
			else
			{
				if( cRxedChar == '\r' )
				{
					/* Ignore the character. */
				}
				else if( cRxedChar == '\b' )
				{
					/* Backspace was pressed.  Erase the last character in the
					string - if any. */
					if( cInputIndex > 0 )
					{
						cInputIndex--;
						cInputString[ cInputIndex ] = '\0';
					}
				}
				else
				{
					/* A character was entered.  Add it to the string
					entered so far.  When a \n is entered the complete
					string will be passed to the command interpreter. */
					if( ( cRxedChar >= ' ' ) && ( cRxedChar <= '~' ) )
					{
						if( cInputIndex < cmdMAX_INPUT_SIZE )
						{
							cInputString[ cInputIndex ] = cRxedChar;
							cInputIndex++;
						}
					}
				}
			}
		}
	}
}
/*-----------------------------------------------------------*/

static void prvSendBuffer( const uint8_t * pcBuffer, size_t xBufferLength )
{
const portTickType xVeryShortDelay = 2UL;

	if( xBufferLength > 0 )
	{
		MSS_UART_irq_tx( ( mss_uart_instance_t * ) pxUART, pcBuffer, xBufferLength );

		/* Ensure any previous transmissions have completed.  The default UART
		interrupt does not provide an event based method of	signally the end of a Tx
		- this is therefore a crude poll of the Tx end status.  Replacing the
		default UART handler with one that 'gives' a semaphore when the Tx is
		complete would allow this poll loop to be replaced by a simple semaphore
		block. */
		while( MSS_UART_tx_complete( ( mss_uart_instance_t * ) pxUART ) == pdFALSE )
		{
			vTaskDelay( xVeryShortDelay );
		}
	}
}
/*-----------------------------------------------------------*/

static void prvConfigureUART( void )
{
	/* Initialise the UART which is used for printf() and CLI IO. */
	MSS_UART_init( ( mss_uart_instance_t * ) pxUART, MSS_UART_115200_BAUD, MSS_UART_DATA_8_BITS | MSS_UART_NO_PARITY | MSS_UART_ONE_STOP_BIT );

	/* Characters are only ever received slowly on the CLI so it is ok to pass
	received characters from the UART interrupt to the task on a queue.  Create
	the queue used for that purpose. */
	xRxedChars = xQueueCreate( cmdRXED_CHARS_QUEUE_LENGTH, sizeof( char ) );

	/* The interrupt handler makes use of FreeRTOS API functions, so its
	priority must be at or below the configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
	setting (the higher the numeric priority, the lower the logical priority). */
	NVIC_SetPriority( xUART_IRQ, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY );

	/* Set the UART Rx notification function. */
	MSS_UART_set_rx_handler( ( mss_uart_instance_t * ) pxUART, prvUARTRxNotificationHandler, MSS_UART_FIFO_SINGLE_BYTE );
}
/*-----------------------------------------------------------*/

static void prvUARTRxNotificationHandler( mss_uart_instance_t * pxUART )
{
uint8_t cRxed;
portBASE_TYPE xHigherPriorityTaskWoken;

	/* The command console receives data very slowly (at the speed of somebody
	typing), therefore it is ok to just handle one character at a time and use
	a queue to send the characters to the task. */
	if( MSS_UART_get_rx( pxUART, &cRxed, sizeof( cRxed ) ) == sizeof( cRxed ) )
	{
		xHigherPriorityTaskWoken = pdFALSE;
		xQueueSendFromISR( xRxedChars, &cRxed, &xHigherPriorityTaskWoken );

		/* portEND_SWITCHING_ISR() or portYIELD_FROM_ISR() can be used here. */
		portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
	}
}

