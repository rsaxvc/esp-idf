/*
 * FreeRTOS Kernel V10.5.1 (ESP-IDF SMP modified)
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * SPDX-FileCopyrightText: 2021 Amazon.com, Inc. or its affiliates
 *
 * SPDX-License-Identifier: MIT
 *
 * SPDX-FileContributor: 2023 Espressif Systems (Shanghai) CO LTD
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

/* Standard includes. */
#include <stdlib.h>
#include <pthread.h>

/* Defining MPU_WRAPPERS_INCLUDED_FROM_API_FILE prevents task.h from redefining
 * all the API functions to use the MPU wrappers.  That should only be done when
 * task.h is included from an application file. */
#define MPU_WRAPPERS_INCLUDED_FROM_API_FILE

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "event_groups.h"
/* Include private IDF API additions for critical thread safety macros */
#include "esp_private/freertos_idf_additions_priv.h"

/* Lint e961, e750 and e9021 are suppressed as a MISRA exception justified
 * because the MPU ports require MPU_WRAPPERS_INCLUDED_FROM_API_FILE to be defined
 * for the header files above, but not in this file, in order to generate the
 * correct privileged Vs unprivileged linkage and placement. */
#undef MPU_WRAPPERS_INCLUDED_FROM_API_FILE /*lint !e961 !e750 !e9021 See comment above. */

typedef struct EventGroupDef_t
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    EventBits_t uxEventBits;

    #if ( configUSE_TRACE_FACILITY == 1 )
        UBaseType_t uxEventGroupNumber;
    #endif

    #if ( ( configSUPPORT_STATIC_ALLOCATION == 1 ) && ( configSUPPORT_DYNAMIC_ALLOCATION == 1 ) )
        uint8_t ucStaticallyAllocated; /*< Set to pdTRUE if the event group is statically allocated to ensure no attempt is made to free the memory. */
    #endif
} EventGroup_t;

/*-----------------------------------------------------------*/

/*
 * Test the bits set in uxCurrentEventBits to see if the wait condition is met.
 * The wait condition is defined by xWaitForAllBits.  If xWaitForAllBits is
 * pdTRUE then the wait condition is met if all the bits set in uxBitsToWaitFor
 * are also set in uxCurrentEventBits.  If xWaitForAllBits is pdFALSE then the
 * wait condition is met if any of the bits set in uxBitsToWait for are also set
 * in uxCurrentEventBits.
 */
static BaseType_t prvTestWaitCondition( const EventBits_t uxCurrentEventBits,
                                        const EventBits_t uxBitsToWaitFor,
                                        const BaseType_t xWaitForAllBits ) PRIVILEGED_FUNCTION;

/*-----------------------------------------------------------*/

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )

    EventGroupHandle_t xEventGroupCreateStatic( StaticEventGroup_t * pxEventGroupBuffer )
    {
        EventGroup_t * pxEventBits;

        /* A StaticEventGroup_t object must be provided. */
        configASSERT( pxEventGroupBuffer );

        #if ( configASSERT_DEFINED == 1 )
        {
            /* Sanity check that the size of the structure used to declare a
             * variable of type StaticEventGroup_t equals the size of the real
             * event group structure. */
            volatile size_t xSize = sizeof( StaticEventGroup_t );
            configASSERT( xSize == sizeof( EventGroup_t ) );
            ( void ) xSize; /* Prevent unused variable warning when configASSERT() is not used. */
        } /*lint !e529 xSize is referenced if configASSERT() is defined. */
        #endif /* configASSERT_DEFINED */

        /* The user has provided a statically allocated event group - use it. */
        pxEventBits = ( EventGroup_t * ) pxEventGroupBuffer; /*lint !e740 !e9087 EventGroup_t and StaticEventGroup_t are deliberately aliased for data hiding purposes and guaranteed to have the same size and alignment requirement - checked by configASSERT(). */

        if( pxEventBits != NULL )
        {
            pxEventBits->uxEventBits = 0;

            #if ( configSUPPORT_DYNAMIC_ALLOCATION == 1 )
            {
                /* Both static and dynamic allocation can be used, so note that
                 * this event group was created statically in case the event group
                 * is later deleted. */
                pxEventBits->ucStaticallyAllocated = pdTRUE;
            }
            #endif /* configSUPPORT_DYNAMIC_ALLOCATION */

            /* Initialize the event group's spinlock. */
            pthread_mutex_init( &pxEventBits->mutex, NULL );
            pthread_cond_init( &pxEventBits->cond, NULL );

            traceEVENT_GROUP_CREATE( pxEventBits );
        }
        else
        {
            /* xEventGroupCreateStatic should only ever be called with
             * pxEventGroupBuffer pointing to a pre-allocated (compile time
             * allocated) StaticEventGroup_t variable. */
            traceEVENT_GROUP_CREATE_FAILED();
        }

        return pxEventBits;
    }

#endif /* configSUPPORT_STATIC_ALLOCATION */
/*-----------------------------------------------------------*/

#if ( configSUPPORT_DYNAMIC_ALLOCATION == 1 )

    EventGroupHandle_t xEventGroupCreate( void )
    {
        EventGroup_t * pxEventBits;

        /* Allocate the event group.  Justification for MISRA deviation as
         * follows:  pvPortMalloc() always ensures returned memory blocks are
         * aligned per the requirements of the MCU stack.  In this case
         * pvPortMalloc() must return a pointer that is guaranteed to meet the
         * alignment requirements of the EventGroup_t structure - which (if you
         * follow it through) is the alignment requirements of the TickType_t type
         * (EventBits_t being of TickType_t itself).  Therefore, whenever the
         * stack alignment requirements are greater than or equal to the
         * TickType_t alignment requirements the cast is safe.  In other cases,
         * where the natural word size of the architecture is less than
         * sizeof( TickType_t ), the TickType_t variables will be accessed in two
         * or more reads operations, and the alignment requirements is only that
         * of each individual read. */
        pxEventBits = ( EventGroup_t * ) pvPortMalloc( sizeof( EventGroup_t ) ); /*lint !e9087 !e9079 see comment above. */

        if( pxEventBits != NULL )
        {
            pxEventBits->uxEventBits = 0;

            #if ( configSUPPORT_STATIC_ALLOCATION == 1 )
            {
                /* Both static and dynamic allocation can be used, so note this
                 * event group was allocated statically in case the event group is
                 * later deleted. */
                pxEventBits->ucStaticallyAllocated = pdFALSE;
            }
            #endif /* configSUPPORT_STATIC_ALLOCATION */

            /* Initialize the event group's spinlock. */
            pthread_mutex_init( &pxEventBits->mutex, NULL );
            pthread_cond_init( &pxEventBits->cond, NULL );

            traceEVENT_GROUP_CREATE( pxEventBits );
        }
        else
        {
            traceEVENT_GROUP_CREATE_FAILED(); /*lint !e9063 Else branch only exists to allow tracing and does not generate code if trace macros are not defined. */
        }

        return pxEventBits;
    }

#endif /* configSUPPORT_DYNAMIC_ALLOCATION */
/*-----------------------------------------------------------*/

EventBits_t xEventGroupSync( EventGroupHandle_t xEventGroup,
                             const EventBits_t uxBitsToSet,
                             const EventBits_t uxBitsToWaitFor,
                             TickType_t xTicksToWait )
{
    EventBits_t uxOriginalBitValue, uxReturn;
    EventGroup_t * pxEventBits = xEventGroup;
    BaseType_t xTimeoutOccurred = pdFALSE;

    configASSERT( uxBitsToWaitFor != 0 );
    #if ( ( INCLUDE_xTaskGetSchedulerState == 1 ) || ( configUSE_TIMERS == 1 ) )
    {
        configASSERT( !( ( xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED ) && ( xTicksToWait != 0 ) ) );
    }
    #endif

    pthread_mutex_lock( &( pxEventBits->mutex ) );
    {
        uxOriginalBitValue = pxEventBits->uxEventBits;

        ( void ) xEventGroupSetBits( xEventGroup, uxBitsToSet );

        if( ( ( uxOriginalBitValue | uxBitsToSet ) & uxBitsToWaitFor ) == uxBitsToWaitFor )
        {
            /* All the rendezvous bits are now set - no need to block. */
            uxReturn = ( uxOriginalBitValue | uxBitsToSet );

            /* Rendezvous always clear the bits.  They will have been cleared
             * already unless this is the only task in the rendezvous. */
            pxEventBits->uxEventBits &= ~uxBitsToWaitFor;

            xTicksToWait = 0;
        }
        else
        {
            if( xTicksToWait != ( TickType_t ) 0 )
            {
                traceEVENT_GROUP_SYNC_BLOCK( xEventGroup, uxBitsToSet, uxBitsToWaitFor );

                /* This assignment is obsolete as uxReturn will get set after
                 * the task unblocks, but some compilers mistakenly generate a
                 * warning about uxReturn being returned without being set if the
                 * assignment is omitted. */
                uxReturn = 0;
            }
            else
            {
                /* The rendezvous bits were not set, but no block time was
                 * specified - just return the current event bit value. */
                uxReturn = pxEventBits->uxEventBits;
                xTimeoutOccurred = pdTRUE;
            }
        }
    }
    pthread_mutex_unlock( &( pxEventBits->mutex ) );

    traceEVENT_GROUP_SYNC_END( xEventGroup, uxBitsToSet, uxBitsToWaitFor, xTimeoutOccurred );

    /* Prevent compiler warnings when trace macros are not used. */
    ( void ) xTimeoutOccurred;

    return uxReturn;
}
/*-----------------------------------------------------------*/

EventBits_t xEventGroupWaitBits( EventGroupHandle_t xEventGroup,
                                 const EventBits_t uxBitsToWaitFor,
                                 const BaseType_t xClearOnExit,
                                 const BaseType_t xWaitForAllBits,
                                 TickType_t xTicksToWait )
{
    EventGroup_t * pxEventBits = xEventGroup;
    EventBits_t uxReturn;
    BaseType_t xWaitConditionMet;
    BaseType_t xTimeoutOccurred = pdFALSE;

    /* Check the user is not attempting to wait on the bits used by the kernel
     * itself, and that at least one bit is being requested. */
    configASSERT( xEventGroup );
    configASSERT( uxBitsToWaitFor != 0 );
    #if ( ( INCLUDE_xTaskGetSchedulerState == 1 ) || ( configUSE_TIMERS == 1 ) )
    {
        configASSERT( !( ( xTaskGetSchedulerState() == taskSCHEDULER_SUSPENDED ) && ( xTicksToWait != 0 ) ) );
    }
    #endif

    pthread_mutex_lock( &( pxEventBits->mutex ) );
    {
        const EventBits_t uxCurrentEventBits = pxEventBits->uxEventBits;

        /* Check to see if the wait condition is already met or not. */
        xWaitConditionMet = prvTestWaitCondition( uxCurrentEventBits, uxBitsToWaitFor, xWaitForAllBits );

        if( xWaitConditionMet != pdFALSE )
        {
            /* The wait condition has already been met so there is no need to
             * block. */
            uxReturn = uxCurrentEventBits;
            xTicksToWait = ( TickType_t ) 0;

            /* Clear the wait bits if requested to do so. */
            if( xClearOnExit != pdFALSE )
            {
                pxEventBits->uxEventBits &= ~uxBitsToWaitFor;
            }
            else
            {
                mtCOVERAGE_TEST_MARKER();
            }
        }
        else if( xTicksToWait == ( TickType_t ) 0 )
        {
            /* The wait condition has not been met, but no block time was
             * specified, so just return the current value. */
            uxReturn = uxCurrentEventBits;
            xTimeoutOccurred = pdTRUE;
        }
        else
        {
            /* This is obsolete as it will get set after the task unblocks, but
             * some compilers mistakenly generate a warning about the variable
             * being returned without being set if it is not done. */
            uxReturn = 0;

            traceEVENT_GROUP_WAIT_BITS_BLOCK( xEventGroup, uxBitsToWaitFor );
        }
    }
    pthread_mutex_unlock( &( pxEventBits->mutex ) );

    traceEVENT_GROUP_WAIT_BITS_END( xEventGroup, uxBitsToWaitFor, xTimeoutOccurred );

    /* Prevent compiler warnings when trace macros are not used. */
    ( void ) xTimeoutOccurred;

    return uxReturn;
}
/*-----------------------------------------------------------*/

EventBits_t xEventGroupClearBits( EventGroupHandle_t xEventGroup,
                                  const EventBits_t uxBitsToClear )
{
    EventGroup_t * pxEventBits = xEventGroup;
    EventBits_t uxReturn;

    /* Check the user is not attempting to clear the bits used by the kernel
     * itself. */
    configASSERT( xEventGroup );

    pthread_mutex_lock( &( pxEventBits->mutex ) );
    {
        traceEVENT_GROUP_CLEAR_BITS( xEventGroup, uxBitsToClear );

        /* The value returned is the event group value prior to the bits being
         * cleared. */
        uxReturn = pxEventBits->uxEventBits;

        /* Clear the bits. */
        pxEventBits->uxEventBits &= ~uxBitsToClear;
    }
    pthread_mutex_unlock( &( pxEventBits->mutex ) );

    return uxReturn;
}
/*-----------------------------------------------------------*/

#if ( ( configUSE_TRACE_FACILITY == 1 ) && ( INCLUDE_xTimerPendFunctionCall == 1 ) && ( configUSE_TIMERS == 1 ) )

    BaseType_t xEventGroupClearBitsFromISR( EventGroupHandle_t xEventGroup,
                                            const EventBits_t uxBitsToClear )
    {
        BaseType_t xReturn;

        traceEVENT_GROUP_CLEAR_BITS_FROM_ISR( xEventGroup, uxBitsToClear );
        xReturn = xTimerPendFunctionCallFromISR( vEventGroupClearBitsCallback, ( void * ) xEventGroup, ( uint32_t ) uxBitsToClear, NULL ); /*lint !e9087 Can't avoid cast to void* as a generic callback function not specific to this use case. Callback casts back to original type so safe. */

        return xReturn;
    }

#endif /* if ( ( configUSE_TRACE_FACILITY == 1 ) && ( INCLUDE_xTimerPendFunctionCall == 1 ) && ( configUSE_TIMERS == 1 ) ) */
/*-----------------------------------------------------------*/
#if 0 //rsa
EventBits_t xEventGroupGetBitsFromISR( EventGroupHandle_t xEventGroup )
{
    UBaseType_t uxSavedInterruptStatus;
    EventGroup_t const * const pxEventBits = xEventGroup;
    EventBits_t uxReturn;

    prvENTER_CRITICAL_OR_MASK_ISR( ( portMUX_TYPE * ) &( pxEventBits->xEventGroupLock ), uxSavedInterruptStatus );
    {
        uxReturn = pxEventBits->uxEventBits;
    }
    prvEXIT_CRITICAL_OR_UNMASK_ISR( ( portMUX_TYPE * ) &( pxEventBits->xEventGroupLock ), uxSavedInterruptStatus );

    return uxReturn;
} /*lint !e818 EventGroupHandle_t is a typedef used in other functions to so can't be pointer to const. */
#endif
/*-----------------------------------------------------------*/

EventBits_t xEventGroupSetBits( EventGroupHandle_t xEventGroup,
                                const EventBits_t uxBitsToSet )
{
    EventGroup_t * pxEventBits = xEventGroup;

    configASSERT( xEventGroup );

    pthread_mutex_lock( &( pxEventBits->mutex ) );
    traceEVENT_GROUP_SET_BITS( xEventGroup, uxBitsToSet );

    /* Set the bits. */
    pxEventBits->uxEventBits |= uxBitsToSet;

    pthread_mutex_unlock( &( pxEventBits->mutex ) );

    /* Let waiters know */
    pthread_cond_broadcast( &( pxEventBits->cond ) );

    return pxEventBits->uxEventBits;
}
/*-----------------------------------------------------------*/

void vEventGroupDelete( EventGroupHandle_t xEventGroup )
{
    EventGroup_t * pxEventBits = xEventGroup;

    configASSERT( pxEventBits );

    pthread_mutex_lock( &( pxEventBits->mutex ) );
    #if ( configNUMBER_OF_CORES > 1 )

        /* We are about to traverse a task list which is a kernel data structure.
         * Thus we need to call prvTakeKernelLock() to take the kernel lock. */
        prvTakeKernelLock();
    #endif /* configNUMBER_OF_CORES > 1 */
    {
        traceEVENT_GROUP_DELETE( xEventGroup );
    }
    #if ( configNUMBER_OF_CORES > 1 )
        /* Release the previously taken kernel lock. */
        prvReleaseKernelLock();
    #endif /* configNUMBER_OF_CORES > 1 */
    pthread_mutex_unlock( &( pxEventBits->mutex ) );

    #if ( ( configSUPPORT_DYNAMIC_ALLOCATION == 1 ) && ( configSUPPORT_STATIC_ALLOCATION == 0 ) )
    {
        /* The event group can only have been allocated dynamically - free
         * it again. */
        vPortFree( pxEventBits );
    }
    #elif ( ( configSUPPORT_DYNAMIC_ALLOCATION == 1 ) && ( configSUPPORT_STATIC_ALLOCATION == 1 ) )
    {
        /* The event group could have been allocated statically or
         * dynamically, so check before attempting to free the memory. */
        if( pxEventBits->ucStaticallyAllocated == ( uint8_t ) pdFALSE )
        {
            vPortFree( pxEventBits );
        }
        else
        {
            mtCOVERAGE_TEST_MARKER();
        }
    }
    #endif /* configSUPPORT_DYNAMIC_ALLOCATION */
}
/*-----------------------------------------------------------*/

#if ( configSUPPORT_STATIC_ALLOCATION == 1 )
    BaseType_t xEventGroupGetStaticBuffer( EventGroupHandle_t xEventGroup,
                                           StaticEventGroup_t ** ppxEventGroupBuffer )
    {
        BaseType_t xReturn;
        EventGroup_t * pxEventBits = xEventGroup;

        configASSERT( pxEventBits );
        configASSERT( ppxEventGroupBuffer );

        #if ( configSUPPORT_DYNAMIC_ALLOCATION == 1 )
        {
            /* Check if the event group was statically allocated. */
            if( pxEventBits->ucStaticallyAllocated == ( uint8_t ) pdTRUE )
            {
                *ppxEventGroupBuffer = ( StaticEventGroup_t * ) pxEventBits;
                xReturn = pdTRUE;
            }
            else
            {
                xReturn = pdFALSE;
            }
        }
        #else /* configSUPPORT_DYNAMIC_ALLOCATION */
        {
            /* Event group must have been statically allocated. */
            *ppxEventGroupBuffer = ( StaticEventGroup_t * ) pxEventBits;
            xReturn = pdTRUE;
        }
        #endif /* configSUPPORT_DYNAMIC_ALLOCATION */

        return xReturn;
    }
#endif /* configSUPPORT_STATIC_ALLOCATION */
/*-----------------------------------------------------------*/

/* For internal use only - execute a 'set bits' command that was pended from
 * an interrupt. */
void vEventGroupSetBitsCallback( void * pvEventGroup,
                                 const uint32_t ulBitsToSet )
{
    ( void ) xEventGroupSetBits( pvEventGroup, ( EventBits_t ) ulBitsToSet ); /*lint !e9079 Can't avoid cast to void* as a generic timer callback prototype. Callback casts back to original type so safe. */
}
/*-----------------------------------------------------------*/

/* For internal use only - execute a 'clear bits' command that was pended from
 * an interrupt. */
void vEventGroupClearBitsCallback( void * pvEventGroup,
                                   const uint32_t ulBitsToClear )
{
    ( void ) xEventGroupClearBits( pvEventGroup, ( EventBits_t ) ulBitsToClear ); /*lint !e9079 Can't avoid cast to void* as a generic timer callback prototype. Callback casts back to original type so safe. */
}
/*-----------------------------------------------------------*/

static BaseType_t prvTestWaitCondition( const EventBits_t uxCurrentEventBits,
                                        const EventBits_t uxBitsToWaitFor,
                                        const BaseType_t xWaitForAllBits )
{
    BaseType_t xWaitConditionMet = pdFALSE;

    if( xWaitForAllBits == pdFALSE )
    {
        /* Task only has to wait for one bit within uxBitsToWaitFor to be
         * set.  Is one already set? */
        if( ( uxCurrentEventBits & uxBitsToWaitFor ) != ( EventBits_t ) 0 )
        {
            xWaitConditionMet = pdTRUE;
        }
        else
        {
            mtCOVERAGE_TEST_MARKER();
        }
    }
    else
    {
        /* Task has to wait for all the bits in uxBitsToWaitFor to be set.
         * Are they set already? */
        if( ( uxCurrentEventBits & uxBitsToWaitFor ) == uxBitsToWaitFor )
        {
            xWaitConditionMet = pdTRUE;
        }
        else
        {
            mtCOVERAGE_TEST_MARKER();
        }
    }

    return xWaitConditionMet;
}
/*-----------------------------------------------------------*/

#if ( ( configUSE_TRACE_FACILITY == 1 ) && ( INCLUDE_xTimerPendFunctionCall == 1 ) && ( configUSE_TIMERS == 1 ) ) && 0 //rsa

    BaseType_t xEventGroupSetBitsFromISR( EventGroupHandle_t xEventGroup,
                                          const EventBits_t uxBitsToSet,
                                          BaseType_t * pxHigherPriorityTaskWoken )
    {
        BaseType_t xReturn;

        traceEVENT_GROUP_SET_BITS_FROM_ISR( xEventGroup, uxBitsToSet );
        xReturn = xTimerPendFunctionCallFromISR( vEventGroupSetBitsCallback, ( void * ) xEventGroup, ( uint32_t ) uxBitsToSet, pxHigherPriorityTaskWoken ); /*lint !e9087 Can't avoid cast to void* as a generic callback function not specific to this use case. Callback casts back to original type so safe. */

        return xReturn;
    }

#endif /* if ( ( configUSE_TRACE_FACILITY == 1 ) && ( INCLUDE_xTimerPendFunctionCall == 1 ) && ( configUSE_TIMERS == 1 ) ) */
/*-----------------------------------------------------------*/

#if ( configUSE_TRACE_FACILITY == 1 )

    UBaseType_t uxEventGroupGetNumber( void * xEventGroup )
    {
        UBaseType_t xReturn;
        EventGroup_t const * pxEventBits = ( EventGroup_t * ) xEventGroup; /*lint !e9087 !e9079 EventGroupHandle_t is a pointer to an EventGroup_t, but EventGroupHandle_t is kept opaque outside of this file for data hiding purposes. */

        if( xEventGroup == NULL )
        {
            xReturn = 0;
        }
        else
        {
            xReturn = pxEventBits->uxEventGroupNumber;
        }

        return xReturn;
    }

#endif /* configUSE_TRACE_FACILITY */
/*-----------------------------------------------------------*/

#if ( configUSE_TRACE_FACILITY == 1 )

    void vEventGroupSetNumber( void * xEventGroup,
                               UBaseType_t uxEventGroupNumber )
    {
        ( ( EventGroup_t * ) xEventGroup )->uxEventGroupNumber = uxEventGroupNumber; /*lint !e9087 !e9079 EventGroupHandle_t is a pointer to an EventGroup_t, but EventGroupHandle_t is kept opaque outside of this file for data hiding purposes. */
    }

#endif /* configUSE_TRACE_FACILITY */
/*-----------------------------------------------------------*/
