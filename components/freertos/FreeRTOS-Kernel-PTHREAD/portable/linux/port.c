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

#define SIG_RESUME SIGUSR1

typedef struct THREAD
{
    pthread_t pthread;
    TaskFunction_t pxCode;
    void *pvParams;
    BaseType_t xDying;
    struct event *ev;
} Thread_t;

/*
 * The additional per-thread data is stored at the beginning of the
 * task's stack.
 */
static inline Thread_t *prvGetThreadFromTask(TaskHandle_t xTask)
{
    StackType_t *pxTopOfStack = *(StackType_t **)xTask;

    return (Thread_t *)(pxTopOfStack + 1);
}

/*-----------------------------------------------------------*/

static pthread_once_t hSigSetupThread = PTHREAD_ONCE_INIT;
//static volatile BaseType_t uxCriticalNesting;
static sigset_t xAllSignals;
static sigset_t xSchedulerOriginalSignalMask;
static pthread_t hMainThread = ( pthread_t )NULL;
/*-----------------------------------------------------------*/

static BaseType_t xSchedulerEnd = pdFALSE;
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
pthread_t *pxPortInitialiseStack( StackType_t *pxTopOfStack,
                                    StackType_t *pxEndOfStack,
                                    TaskFunction_t pxCode,
                                    void *pvParameters )
{
    Thread_t *thread;
    pthread_attr_t xThreadAttributes;
    size_t ulStackSize;
    int iRet;

    (void)pthread_once( &hSigSetupThread, prvSetupSignalsAndSchedulerPolicy );

    /*
     * Store the additional thread data at the start of the stack.
     */
    thread = (Thread_t *)(pxTopOfStack + 1) - 1;
    pxTopOfStack = (StackType_t *)thread - 1;
    ulStackSize = (pxTopOfStack + 1 - pxEndOfStack) * sizeof(*pxTopOfStack);

    thread->pxCode = pxCode;
    thread->pvParams = pvParameters;
    thread->xDying = pdFALSE;

    pthread_attr_init( &xThreadAttributes );
    pthread_attr_setstack( &xThreadAttributes, pxEndOfStack, ulStackSize );

    //rsa vPortEnterCritical();

    iRet = pthread_create( &thread->pthread, &xThreadAttributes,
                           prvWaitForStart, thread );
    if ( iRet )
    {
        prvFatalError( "pthread_create", iRet );
    }

    //rsa vPortExitCritical();

    return pxTopOfStack;
}
/*-----------------------------------------------------------*/


/*
 * See header file for description.
 */
BaseType_t xPortStartScheduler( void )
{
    int iSignal;
    sigset_t xSignals;

    clock_gettime(CLOCK_MONOTONIC, &ts_init);
    hMainThread = pthread_self();

    /* Start the timer that generates the tick ISR(SIGALRM).
       Interrupts are disabled here already. */
    prvSetupTimerInterrupt();

    /* Wait until signaled by vPortEndScheduler(). */
    sigemptyset( &xSignals );
    sigaddset( &xSignals, SIG_RESUME );

    while ( !xSchedulerEnd )
    {
        sigwait( &xSignals, &iSignal );
    }

#if ( configUSE_TIMERS == 1 )
    /* Cancel the Timer task and free its resources */
    vPortCancelThread( xTimerGetTimerDaemonTaskHandle() );
#endif /* configUSE_TIMERS */

    /* Restore original signal mask. */
    (void)pthread_sigmask( SIG_SETMASK, &xSchedulerOriginalSignalMask,  NULL );

    return 0;
}
/*-----------------------------------------------------------*/

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
    configASSERT( pthread_mutex_unlock(mux) );
}
/*-----------------------------------------------------------*/

void vPortYield( void )
{
    sched_yield();
}
/*-----------------------------------------------------------*/

/*rsa void vPortDisableInterrupts( void )
{
    pthread_sigmask( SIG_BLOCK, &xAllSignals, NULL );
}
*/
/*-----------------------------------------------------------*/
/*rsa void vPortEnableInterrupts( void )
{
    pthread_sigmask( SIG_UNBLOCK, &xAllSignals, NULL );
}*/
/*-----------------------------------------------------------*/

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
    //uxCriticalNesting++; /* Signals are blocked in this signal handler. */

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

   // uxCriticalNesting--;
}
/*-----------------------------------------------------------*/

void vPortThreadDying( void *pxTaskToDelete, volatile BaseType_t *pxPendYield )
{
    Thread_t *pxThread = prvGetThreadFromTask( pxTaskToDelete );

    pxThread->xDying = pdTRUE;
}

void vPortCancelThread( TaskHandle_t pxTaskToDelete )
{
    #if ( CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK )
        /* Call the user defined task pre-deletion hook before canceling the thread */
        extern void vTaskPreDeletionHook( void * pxTCB );
        vTaskPreDeletionHook( pxTaskToDelete );
    #endif /* CONFIG_FREERTOS_TASK_PRE_DELETION_HOOK */

    /*
     * The thread has already been suspended so it can be safely cancelled.
     */
    pthread_cancel( pxTaskToDelete );
    pthread_join( pxTaskToDelete );
}
/*-----------------------------------------------------------*/

static void *prvWaitForStart( void * pvParams )
{
    Thread_t *pxThread = pvParams;

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
    struct sigaction sigresume, sigtick;
    int iRet;

    hMainThread = pthread_self();

    /* Initialise common signal masks. */
    sigfillset( &xAllSignals );
    /* Don't block SIGINT so this can be used to break into GDB while
     * in a critical section. */
    sigdelset( &xAllSignals, SIGINT );

    /*
     * Block all signals in this thread so all new threads
     * inherits this mask.
     *
     * When a thread is resumed for the first time, all signals
     * will be unblocked.
     */
    (void)pthread_sigmask( SIG_SETMASK, &xAllSignals,
                           &xSchedulerOriginalSignalMask );

    /* SIG_RESUME is only used with sigwait() so doesn't need a
       handler. */
    sigresume.sa_flags = 0;
    sigresume.sa_handler = SIG_IGN;
    sigfillset( &sigresume.sa_mask );

    sigtick.sa_flags = 0;
    sigtick.sa_handler = vPortSystemTickHandler;
    sigfillset( &sigtick.sa_mask );

    iRet = sigaction( SIG_RESUME, &sigresume, NULL );
    if ( iRet )
    {
        prvFatalError( "sigaction", errno );
    }

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
