/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#pragma once

#include "bt_target.h"
#include "bt_types.h"

static const char GKI_MODULE[] = "gki_module";

/* Timer list entry callback type
*/
typedef void (TIMER_CBACK)(void *p_tle);
#ifndef TIMER_PARAM_TYPE
#define TIMER_PARAM_TYPE void*
#endif
/* Define a timer list entry
*/
typedef struct _tle
{
    TIMER_CBACK  *p_cback;
    INT32         ticks;
    INT32         ticks_initial;
    TIMER_PARAM_TYPE   param;
    TIMER_PARAM_TYPE   data;
    UINT16        event;
    UINT8         in_use;
} TIMER_LIST_ENT;

/***********************************************************************
** Function prototypes
*/

/* To get and release buffers, change owner and get size
*/
void    GKI_freebuf (void *);
void   *GKI_getbuf (UINT16);
UINT16  GKI_get_buf_size (void *);
