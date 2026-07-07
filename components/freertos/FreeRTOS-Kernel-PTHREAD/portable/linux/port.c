/*
 * FreeRTOS Kernel V10.5.1 (ESP-IDF SMP modified)
 * Copyright (C) 2020 Cambridge Consultants Ltd.
 *
 * SPDX-FileCopyrightText: 2020 Cambridge Consultants Ltd
 *
 * SPDX-License-Identifier: MIT
 *
 * SPDX-FileContributor: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 * https://github.com/FreeRTOS
 *
 */

/*-----------------------------------------------------------
 * Implementation of functions defined in portable.h for the Posix port.
 *
 * Each task has a pthread which eases use of standard debuggers
 * (allowing backtraces of tasks etc). Threads for tasks that are not
 * running are blocked in sigwait().
 *
 * Task switch is done by resuming the thread for the next task by
 * signaling the condition variable and then waiting on a condition variable
 * with the current thread.
 *
 * The timer interrupt uses SIGALRM and care is taken to ensure that
 * the signal handler runs only on the thread for the current task.
 *
 * Use of part of the standard C library requires care as some
 * functions can take pthread mutexes internally which can result in
 * deadlocks as the FreeRTOS kernel can switch tasks while they're
 * holding a pthread mutex.
 *
 * stdio (printf() and friends) should be called from a single task
 * only or serialized with a FreeRTOS primitive such as a binary
 * semaphore or mutex.
 *----------------------------------------------------------*/

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>

/* Scheduler includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "utils/wait_for_event.h"
/*-----------------------------------------------------------*/

static pthread_once_t hSigSetupThread = PTHREAD_ONCE_INIT;
/*-----------------------------------------------------------*/

static volatile BaseType_t xSchedulerEnd = pdFALSE;
static volatile BaseType_t ts_init_done = pdFALSE;
static struct timespec ts_init;

/*-----------------------------------------------------------*/

static void prvSetupSignalsAndSchedulerPolicy( void );
static void prvSetupTimerInterrupt( void );
static void *prvWaitForStart( void * pvParams );
static void vPortSystemTickHandler( int sig );
/*-----------------------------------------------------------*/

static void prvFatalError( const char *pcCall, int iErrno )
{
    fprintf( stderr, "%s: %s\n", pcCall, strerror( iErrno ) );
    abort();
}

/*
 * See header file for description.
 */
TCB_t *pxPortInitialiseStack(   TaskFunction_t pxCode,
                                    void *pvParameters )
{
    TCB_t *thread;
    pthread_attr_t xThreadAttributes;
    //size_t ulStackSize;
    int iRet;

    (void)pthread_once( &hSigSetupThread, prvSetupSignalsAndSchedulerPolicy );

    /*
     * Store the additional thread data
     */
    thread = calloc(1, sizeof(TCB_t));

    thread->pxCode = pxCode;
    thread->pvParams = pvParameters;
    configASSERT( !pthread_mutex_init(&thread->lock, NULL));
    configASSERT( !pthread_cond_init(&thread->cond, NULL));

    configASSERT( !pthread_attr_init( &xThreadAttributes ));
    //pthread_attr_setstack( &xThreadAttributes, pxEndOfStack, ulStackSize );

    memset(thread->ulNotifiedValue, 0x00, sizeof(thread->ulNotifiedValue));
    memset(thread->ucNotifyState, 0x00, sizeof(thread->ucNotifyState));

    iRet = pthread_create( &thread->tid, &xThreadAttributes,
                           prvWaitForStart, thread );
    if ( iRet )
    {
        prvFatalError( "pthread_create", iRet );
    }

    return thread;
}
/*-----------------------------------------------------------*/


/*
 * See header file for description.
 */
BaseType_t xPortStartScheduler( void )
{
    clock_gettime(CLOCK_MONOTONIC, &ts_init);
    ts_init_done = pdTRUE;

    /* Start the timer that generates the tick ISR(SIGALRM).
       Interrupts are disabled here already. */
    prvSetupTimerInterrupt();

    return 0;
}
/*-----------------------------------------------------------*/

TickType_t xTaskGetTickCount( void )
{
    if( ts_init_done != pdTRUE)
    {
        return 0;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ts.tv_sec - ts_init.tv_sec) * configTICK_RATE_HZ + (int64_t)(ts.tv_nsec - ts_init.tv_nsec) * configTICK_RATE_HZ / 1000000000 + configINITIAL_TICK_COUNT;
}
/*----------------------------------------------------------*/

void vPortEndScheduler( void )
{
    struct itimerval itimer;
    struct sigaction sigtick;

    /* Stop the timer and ignore any pending SIGALRMs that would end
     * up running on the main thread when it is resumed. */
    itimer.it_value.tv_sec = 0;
    itimer.it_value.tv_usec = 0;

    itimer.it_interval.tv_sec = 0;
    itimer.it_interval.tv_usec = 0;
    (void)setitimer( ITIMER_REAL, &itimer, NULL );

    sigtick.sa_flags = 0;
    sigtick.sa_handler = SIG_IGN;
    sigemptyset( &sigtick.sa_mask );
    sigaction( SIGALRM, &sigtick, NULL );

    /* Signal the scheduler to exit its loop. */
    xSchedulerEnd = pdTRUE;
    exit(0);
}
/*-----------------------------------------------------------*/

void vPortEnterCritical( portMUX_TYPE *mux )
{
    configASSERT( pthread_mutex_lock(mux) == 0 );
}
/*-----------------------------------------------------------*/

void vPortExitCritical( portMUX_TYPE *mux )
{
    configASSERT( pthread_mutex_unlock(mux) == 0 );
}
/*-----------------------------------------------------------*/

void vPortYield( void )
{
    sched_yield();
}

BaseType_t xPortSetInterruptMask( void )
{
    /* Interrupts are always disabled inside ISRs (signals
       handlers). */
    return pdTRUE;
}
/*-----------------------------------------------------------*/

void vPortClearInterruptMask( BaseType_t xMask )
{
}
/*-----------------------------------------------------------*/

static uint64_t prvGetTimeNs(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);

    return t.tv_sec * 1000000000ull + t.tv_nsec;
}

static uint64_t prvStartTimeNs;
/* commented as part of the code below in vPortSystemTickHandler,
 * to adjust timing according to full demo requirements */
/* static uint64_t prvTickCount; */

/*
 * Setup the systick timer to generate the tick interrupts at the required
 * frequency.
 */
void prvSetupTimerInterrupt( void )
{
    struct itimerval itimer;
    int iRet;

    /* Initialise the structure with the current timer information. */
    iRet = getitimer( ITIMER_REAL, &itimer );
    if ( iRet )
    {
        prvFatalError( "getitimer", errno );
    }

    /* Set the interval between timer events. */
    itimer.it_interval.tv_sec = 0;
    itimer.it_interval.tv_usec = portTICK_RATE_MICROSECONDS;

    /* Set the current count-down. */
    itimer.it_value.tv_sec = 0;
    itimer.it_value.tv_usec = portTICK_RATE_MICROSECONDS;

    /* Set-up the timer interrupt. */
    iRet = setitimer( ITIMER_REAL, &itimer, NULL );
    if ( iRet )
    {
        prvFatalError( "setitimer", errno );
    }

    prvStartTimeNs = prvGetTimeNs();
}
/*-----------------------------------------------------------*/

static void vPortSystemTickHandler( int sig )
{
    /* Tick Increment, accounting for any lost signals or drift in
     * the timer. */
/*
 *      Comment code to adjust timing according to full demo requirements
 *      xExpectedTicks = (prvGetTimeNs() - prvStartTimeNs)
 *        / (portTICK_RATE_MICROSECONDS * 1000);
 * do { */
        xTaskIncrementTick();
/*        prvTickCount++;
 *    } while (prvTickCount < xExpectedTicks);
*/
}

/*-----------------------------------------------------------*/

void vPortCancelThread( TCB_t * pxTaskToDelete )
{
    #if ( CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK )
        /* Call the user defined task pre-deletion hook before canceling the thread */
        extern void vTaskPreDeletionHook( void * pxTCB );
        vTaskPreDeletionHook( pxTaskToDelete );
    #endif /* CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK */

    configASSERT( !pthread_cond_destroy(&pxTaskToDelete->cond));
    configASSERT( !pthread_mutex_destroy(&pxTaskToDelete->lock));

    pthread_cancel( pxTaskToDelete->tid );
    pthread_join( pxTaskToDelete->tid, NULL );
}
/*-----------------------------------------------------------*/

static void *prvWaitForStart( void * pvParams )
{
    TCB_t *pxThread = pvParams;

    /* Save off TCB_t pointer into thread-local-storage */
    configASSERT(!pthread_setspecific(task_handle_key, pvParams));

    /* Call the task's entry point. */
    pxThread->pxCode( pxThread->pvParams );

    /* A function that implements a task must not exit or attempt to return to
    * its caller as there is nothing to return to. If a task wants to exit it
    * should instead call vTaskDelete( NULL ). Artificially force an assert()
    * to be triggered if configASSERT() is defined, so application writers can
        * catch the error. */
    configASSERT( pdFALSE );

    return NULL;
}
/*-----------------------------------------------------------*/

static void prvSetupSignalsAndSchedulerPolicy( void )
{
    struct sigaction sigtick;
    int iRet;

    sigtick.sa_flags = 0;
    sigtick.sa_handler = vPortSystemTickHandler;
    sigfillset( &sigtick.sa_mask );

    iRet = sigaction( SIGALRM, &sigtick, NULL );
    if ( iRet )
    {
        prvFatalError( "sigaction", errno );
    }
}
/*-----------------------------------------------------------*/

unsigned long ulPortGetRunTime( void )
{
    struct tms xTimes;

    times( &xTimes );

    return ( unsigned long ) xTimes.tms_utime;
}
/*-----------------------------------------------------------*/

void vPortSetStackWatchpoint( void *pxStackStart )
{
    (void) pxStackStart;
}
/*-----------------------------------------------------------*/
