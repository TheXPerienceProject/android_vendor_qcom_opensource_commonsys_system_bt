/******************************************************************************
 *
 *  Copyright (C) 2009-2012 Broadcom Corporation
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

#ifndef BTIF_HH_H
#define BTIF_HH_H

#include <hardware/bluetooth.h>
#include <hardware/bt_hh.h>
#include <linux/version.h>
#include <pthread.h>
#include <stdint.h>
#include "bta_hh_api.h"
#include "btu.h"
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 18, 00))
#include "osi/include/fixed_queue.h"
#endif  //  (LINUX_VERSION_CODE > KERNEL_VERSION(3,18,00))

/*******************************************************************************
 *  Constants & Macros
 ******************************************************************************/

#define BTIF_HH_MAX_HID 8
#define BTIF_HH_MAX_ADDED_DEV 32

#define BTIF_HH_MAX_KEYSTATES 3
#define BTIF_HH_KEYSTATE_MASK_NUMLOCK 0x01
#define BTIF_HH_KEYSTATE_MASK_CAPSLOCK 0x02
#define BTIF_HH_KEYSTATE_MASK_SCROLLLOCK 0x04
#define BTIF_HH_OUTPUT_REPORT_SIZE       2

#define BTIF_HH_MAX_POLLING_ATTEMPTS 10
#define BTIF_HH_POLLING_SLEEP_DURATION_US 5000

/*******************************************************************************
 *  Type definitions and return values
 ******************************************************************************/

typedef enum {
  BTIF_HH_DISABLED = 0,
  BTIF_HH_ENABLED,
  BTIF_HH_DISABLING,
  BTIF_HH_DEV_UNKNOWN,
  BTIF_HH_DEV_CONNECTING,
  BTIF_HH_DEV_CONNECTED,
  BTIF_HH_DEV_DISCONNECTED
} BTIF_HH_STATUS;

typedef struct {
  bthh_connection_state_t dev_status;
  uint8_t dev_handle;
  RawAddress bd_addr;
  tBTA_HH_ATTR_MASK attr_mask;
  uint8_t sub_class;
  uint8_t app_id;
  int fd;
  bool ready_for_data;
  pthread_t hh_poll_thread_id;
  uint8_t hh_keep_polling;
  alarm_t* vup_timer;
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3, 18, 00))
  fixed_queue_t* set_rpt_id_queue;
  fixed_queue_t* get_rpt_id_queue;
#endif             //  (LINUX_VERSION_CODE > KERNEL_VERSION(3,18,00))
  bool local_vup;  // Indicated locally initiated VUP
  uint8_t last_output_rpt_data[BTIF_HH_OUTPUT_REPORT_SIZE];
} btif_hh_device_t;

/* Control block to maintain properties of devices */
typedef struct {
  uint8_t dev_handle;
  RawAddress bd_addr;
  tBTA_HH_ATTR_MASK attr_mask;
} btif_hh_added_device_t;

/**
 * BTIF-HH control block to maintain added devices and currently
 * connected hid devices
 */
typedef struct {
  BTIF_HH_STATUS status;
  btif_hh_device_t devices[BTIF_HH_MAX_HID];
  uint32_t device_num;
  btif_hh_added_device_t added_devices[BTIF_HH_MAX_ADDED_DEV];
  bool service_dereg_active;
  RawAddress pending_conn_address;
} btif_hh_cb_t;

/*******************************************************************************
 *  Functions
 ******************************************************************************/

extern btif_hh_cb_t btif_hh_cb;

extern btif_hh_device_t* btif_hh_find_connected_dev_by_handle(uint8_t handle);
extern void btif_hh_remove_device(RawAddress bd_addr);
extern bool btif_hh_add_added_dev(const RawAddress& bda,
                                  tBTA_HH_ATTR_MASK attr_mask);
extern bt_status_t btif_hh_virtual_unplug(const RawAddress* bd_addr);
extern void btif_hh_disconnect(RawAddress* bd_addr);
extern void btif_hh_service_registration(bool enable);
extern void btif_hh_setreport(btif_hh_device_t* p_dev,
                              bthh_report_type_t r_type, uint16_t size,
                              uint8_t* report);
extern void btif_hh_getreport(btif_hh_device_t* p_dev,
                              bthh_report_type_t r_type, uint8_t reportId,
                              uint16_t bufferSize);

#endif
