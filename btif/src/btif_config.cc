/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
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

#define LOG_TAG "bt_btif_config"

#include "btif_config.h"

#include <base/base64.h>
#include <base/logging.h>
#include <ctype.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <private/android_filesystem_config.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <mutex>
#include <sstream>
#include <string>

#include "bt_types.h"
#include "btcore/include/module.h"
#include "btif_api.h"
#include "btif_common.h"
#include "btif_config_transcode.h"
#include "btif_keystore.h"
#include "btif_util.h"
#include "common/address_obfuscator.h"
#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "osi/include/compat.h"
#include "osi/include/config.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "osi/include/properties.h"

#define BT_CONFIG_SOURCE_TAG_NUM 1010001

#define INFO_SECTION "Info"
#define FILE_TIMESTAMP "TimeCreated"
#define FILE_SOURCE "FileSource"
#define TIME_STRING_LENGTH sizeof("YYYY-MM-DD HH:MM:SS")
#define DISABLED "disabled"
static const char* TIME_STRING_FORMAT = "%Y-%m-%d %H:%M:%S";

constexpr int kBufferSize = 400 * 10;  // initial file is ~400B

static bool btif_is_niap_mode() {
  return getuid() == AID_BLUETOOTH && is_niap_mode();
}

#define BT_CONFIG_METRICS_SECTION "Metrics"
#define BT_CONFIG_METRICS_SALT_256BIT "Salt256Bit"
using bluetooth::BtifKeystore;
using bluetooth::common::AddressObfuscator;

// TODO(armansito): Find a better way than searching by a hardcoded path.
#if defined(OS_GENERIC)
static const char* CONFIG_FILE_PATH = "bt_config.conf";
static const char* CONFIG_BACKUP_PATH = "bt_config.bak";
static const char* CONFIG_LEGACY_FILE_PATH = "bt_config.xml";
#else   // !defined(OS_GENERIC)
static const char* CONFIG_FILE_PATH = "/data/misc/bluedroid/bt_config.conf";
static const char* CONFIG_BACKUP_PATH = "/data/misc/bluedroid/bt_config.bak";
static const char* CONFIG_FILE_CHECKSUM_PATH = "/data/misc/bluedroid/bt_config.conf.encrypted-checksum";
static const char* CONFIG_BACKUP_CHECKSUM_PATH = "/data/misc/bluedroid/bt_config.bak.encrypted-checksum";
static const char* CONFIG_LEGACY_FILE_PATH =
    "/data/misc/bluedroid/bt_config.xml";
#endif  // defined(OS_GENERIC)
static const period_ms_t CONFIG_SETTLE_PERIOD_MS = 3000;

static void timer_config_save_cb(void* data);
static void btif_config_write(uint16_t event, char* p_param);
static bool is_factory_reset(void);
static void delete_config_files(void);
static void btif_config_remove_unpaired(config_t* config);
static void btif_config_remove_restricted(config_t* config);
static config_t* btif_config_open(const char* filename, const char* checksum_filename);

// Key attestation
static std::string btif_convert_to_encrypt_key(
    const std::string& unencrypt_str);
static std::string btif_convert_to_unencrypt_key(
    const std::string& encrypt_str);
static bool btif_in_encrypt_key_name_list(std::string key);
static bool btif_is_key_encrypted(int key_from_config_size,
                                  std::string key_type_string);
static std::string hash_file(const char* filename);
static std::string read_checksum_file(const char* filename);
static void write_checksum_file(const char* filename, const std::string& hash);

static const int ENCRYPT_KEY_NAME_LIST_SIZE = 7;
static const std::string encrypt_key_name_list[] = {
    "LinkKey",      "LE_KEY_PENC", "LE_KEY_PID",  "LE_KEY_LID",
    "LE_KEY_PCSRK", "LE_KEY_LENC", "LE_KEY_LCSRK"};

static enum ConfigSource {
  NOT_LOADED,
  ORIGINAL,
  BACKUP,
  LEGACY,
  NEW_FILE,
  RESET
} btif_config_source = NOT_LOADED;

static int btif_config_devices_loaded = -1;
static char btif_config_time_created[TIME_STRING_LENGTH];

// TODO(zachoverflow): Move these two functions out, because they are too
// specific for this file
// {grumpy-cat/no, monty-python/you-make-me-sad}
bool btif_get_device_type(const RawAddress& bda, int* p_device_type) {
  if (p_device_type == NULL) return false;

  std::string addrstr = bda.ToString();
  const char* bd_addr_str = addrstr.c_str();

  if (!btif_config_get_int(bd_addr_str, "DevType", p_device_type)) return false;

  LOG_DEBUG(LOG_TAG, "%s: Device [%s] type %d", __func__, bd_addr_str,
            *p_device_type);
  return true;
}

bool btif_get_address_type(const RawAddress& bda, int* p_addr_type) {
  if (p_addr_type == NULL) return false;

  std::string addrstr = bda.ToString();
  const char* bd_addr_str = addrstr.c_str();

  if (!btif_config_get_int(bd_addr_str, "AddrType", p_addr_type)) return false;

  LOG_DEBUG(LOG_TAG, "%s: Device [%s] address type %d", __func__, bd_addr_str,
            *p_addr_type);
  return true;
}

static config_t* config;
/**
 * Read metrics salt from config file, if salt is invalid or does not exist,
 * generate new one and save it to config
 */
static void read_or_set_metrics_salt() {
  AddressObfuscator::Octet32 metrics_salt = {};
  size_t metrics_salt_length = metrics_salt.size();
  if (!btif_config_get_bin(BT_CONFIG_METRICS_SECTION,
                           BT_CONFIG_METRICS_SALT_256BIT, metrics_salt.data(),
                           &metrics_salt_length)) {
    LOG(WARNING) << __func__ << ": Failed to read metrics salt from config";
    // Invalidate salt
    metrics_salt.fill(0);
  }
  if (metrics_salt_length != metrics_salt.size()) {
    LOG(ERROR) << __func__ << ": Metrics salt length incorrect, "
               << metrics_salt_length << " instead of " << metrics_salt.size();
    // Invalidate salt
    metrics_salt.fill(0);
  }
  if (!AddressObfuscator::IsSaltValid(metrics_salt)) {
    LOG(INFO) << __func__ << ": Metrics salt is not invalid, creating new one";
#if (OFF_TARGET_TEST_ENABLED == FALSE)
    //TODO need to resolve below dependency
    if (RAND_bytes(metrics_salt.data(), metrics_salt.size()) != 1) {
      LOG(FATAL) << __func__ << "Failed to generate salt for metrics";
    }
#endif
    if (!btif_config_set_bin(BT_CONFIG_METRICS_SECTION,
                             BT_CONFIG_METRICS_SALT_256BIT, metrics_salt.data(),
                             metrics_salt.size())) {
      LOG(FATAL) << __func__ << "Failed to write metrics salt to config";
    }
  }
  AddressObfuscator::GetInstance()->Initialize(metrics_salt);
}

static std::recursive_mutex config_lock;  // protects operations on |config|.
static alarm_t* config_timer;

static BtifKeystore btif_keystore(new keystore::KeystoreClientImpl);

// Module lifecycle functions

static future_t* init(void) {
  std::unique_lock<std::recursive_mutex> lock(config_lock);

  if (is_factory_reset()) delete_config_files();

  std::string file_source;

  config = btif_config_open(CONFIG_FILE_PATH, CONFIG_FILE_CHECKSUM_PATH);
  btif_config_source = ORIGINAL;
  if (!config) {
    LOG_WARN(LOG_TAG, "%s unable to load config file: %s; using backup.",
             __func__, CONFIG_FILE_PATH);
    remove(CONFIG_FILE_CHECKSUM_PATH);
    config = btif_config_open(CONFIG_BACKUP_PATH, CONFIG_BACKUP_CHECKSUM_PATH);
    btif_config_source = BACKUP;
    file_source = "Backup";
  }
  if (!config) {
    LOG_WARN(LOG_TAG,
             "%s unable to load backup; attempting to transcode legacy file.",
             __func__);
    remove(CONFIG_BACKUP_CHECKSUM_PATH);
    config = btif_config_transcode(CONFIG_LEGACY_FILE_PATH);
    btif_config_source = LEGACY;
    file_source = "Legacy";
  }
  if (!config) {
    LOG_ERROR(LOG_TAG,
              "%s unable to transcode legacy file; creating empty config.",
              __func__);
    config = config_new_empty();
    btif_config_source = NEW_FILE;
    file_source = "Empty";
  }

  if (!config) {
    LOG_ERROR(LOG_TAG, "%s unable to allocate a config object.", __func__);
    goto error;
  }

  if (!file_source.empty())
    config_set_string(config, INFO_SECTION, FILE_SOURCE, file_source.c_str());

  btif_config_remove_unpaired(config);

  // Cleanup temporary pairings if we have left guest mode
  if (!is_restricted_mode()) btif_config_remove_restricted(config);

  // Read or set config file creation timestamp
  const char* time_str;
  time_str = config_get_string(config, INFO_SECTION, FILE_TIMESTAMP, NULL);
  if (time_str != NULL) {
    strlcpy(btif_config_time_created, time_str, TIME_STRING_LENGTH);
  } else {
    time_t current_time = time(NULL);
    struct tm* time_created = localtime(&current_time);
    if (time_created) {
      if (strftime(btif_config_time_created, TIME_STRING_LENGTH,
                   TIME_STRING_FORMAT, time_created)) {
        config_set_string(config, INFO_SECTION, FILE_TIMESTAMP,
                          btif_config_time_created);
      }
    }
  }

  // Read or set metrics 256 bit hashing salt
  read_or_set_metrics_salt();

  // TODO(sharvil): use a non-wake alarm for this once we have
  // API support for it. There's no need to wake the system to
  // write back to disk.
  config_timer = alarm_new("btif.config");
  if (!config_timer) {
    LOG_ERROR(LOG_TAG, "%s unable to create alarm.", __func__);
    goto error;
  }

  LOG_EVENT_INT(BT_CONFIG_SOURCE_TAG_NUM, btif_config_source);

  return future_new_immediate(FUTURE_SUCCESS);

error:
  alarm_free(config_timer);
  config_free(config);
  config_timer = NULL;
  config = NULL;
  btif_config_source = NOT_LOADED;
  return future_new_immediate(FUTURE_FAIL);
}

static config_t* btif_config_open(const char* filename, const char* checksum_filename) {

  // START KEY ATTESTATION
  // Get hash of current file
  std::string current_hash = hash_file(filename);
  // Get stored hash
  std::string stored_hash = read_checksum_file(checksum_filename);
  if (stored_hash.empty()) {
    LOG(ERROR) << __func__ << ": stored_hash=<empty>";
    // Will encrypt once since the bt_config never encrypt.
    if (!btif_keystore.DoesKeyExist() && !current_hash.empty()) {
      write_checksum_file(checksum_filename, current_hash);
      stored_hash = read_checksum_file(checksum_filename);
    }
  }
  // Compare hashes
  if (current_hash != stored_hash) {
    return NULL;
  }
  // END KEY ATTESTATION

  config_t* config = config_new(filename);
  if (!config) return NULL;

  if (!config_has_section(config, "Adapter")) {
    LOG_ERROR(LOG_TAG, "Config is missing adapter section");
    config_free(config);
    return NULL;
  }

  return config;
}

static future_t* shut_down(void) {
  btif_config_flush();
  return future_new_immediate(FUTURE_SUCCESS);
}

static future_t* clean_up(void) {
  btif_config_flush();

  alarm_free(config_timer);
  config_timer = NULL;

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  config_free(config);
  config = NULL;
  return future_new_immediate(FUTURE_SUCCESS);
}

EXPORT_SYMBOL module_t btif_config_module = {.name = BTIF_CONFIG_MODULE,
                                             .init = init,
                                             .start_up = NULL,
                                             .shut_down = shut_down,
                                             .clean_up = clean_up};

bool btif_config_has_section(const char* section) {
  CHECK(config != NULL);
  CHECK(section != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  return config_has_section(config, section);
}

bool btif_config_exist(const char* section, const char* key) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  return config_has_key(config, section, key);
}

bool btif_config_get_int(const char* section, const char* key, int* value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);
  CHECK(value != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  bool ret = config_has_key(config, section, key);
  if (ret) *value = config_get_int(config, section, key, *value);

  return ret;
}

bool btif_config_get_uint16(const char* section, const char* key, uint16_t* value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);
  CHECK(value != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  bool ret = config_has_key(config, section, key);
  if (ret) *value = config_get_uint16(config, section, key, *value);

  return ret;
}

bool btif_config_get_uint64(const char* section, const char* key,
                            uint64_t* value) {
  CHECK(config != NULL);
  CHECK(value != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  bool ret = config_has_key(config, section, key);
  if (ret) *value = config_get_uint64(config, section, key, *value);

  return ret;
}


bool btif_config_set_int(const char* section, const char* key, int value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  config_set_int(config, section, key, value);

  return true;
}

bool btif_config_set_uint16(const char* section, const char* key, uint16_t value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  config_set_uint16(config, section, key, value);

  return true;
}

bool btif_config_set_uint64(const char* section, const char* key,
                            uint64_t value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  config_set_uint64(config, section, key, value);

  return true;
}


bool btif_config_get_str(const char* section, const char* key, char* value,
                         int* size_bytes) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);
  CHECK(value != NULL);
  CHECK(size_bytes != NULL);

  {
    std::unique_lock<std::recursive_mutex> lock(config_lock);
    const char* stored_value = config_get_string(config, section, key, NULL);
    if (!stored_value) return false;
    strlcpy(value, stored_value, *size_bytes);
  }

  *size_bytes = strlen(value) + 1;
  return true;
}

bool btif_config_set_str(const char* section, const char* key,
                         const char* value) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);
  CHECK(value != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  config_set_string(config, section, key, value);
  return true;
}

bool btif_config_get_bin(const char* section, const char* key, uint8_t* value,
                         size_t* length) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);
  CHECK(value != NULL);
  CHECK(length != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  const std::string* value_str;
  const char* value_str_from_config =  config_get_string(config, section, key, NULL);

  if (!value_str_from_config) {
    VLOG(1) << __func__ << ": cannot find string for section " << section
            << ", key " << key;
    return false;
  }

  std::string skey= key;
  std::string svalue_str_from_config1 = value_str_from_config;
  std::string* svalue_str_from_config = &svalue_str_from_config1;
  bool in_encrypt_key_name_list = btif_in_encrypt_key_name_list(skey);
  bool is_key_encrypted =
      btif_is_key_encrypted(svalue_str_from_config->size(), skey);

  if (in_encrypt_key_name_list && is_key_encrypted) {
    VLOG(2) << __func__ << " decrypt section: " << section << " key:" << key;
    std::string tmp_value_str =
        btif_convert_to_unencrypt_key(*svalue_str_from_config);
    value_str = &tmp_value_str;
  } else {
    value_str = svalue_str_from_config;
  }

  if (!value_str) return false;
  const char* cvalue_str= value_str->c_str();
  size_t value_len = strlen(cvalue_str);
  if ((value_len % 2) != 0 || *length < (value_len / 2)) return false;

  for (size_t i = 0; i < value_len; ++i)
    if (!isxdigit(cvalue_str[i])) return false;

  for (*length = 0; *cvalue_str; cvalue_str += 2, *length += 1)
    sscanf(cvalue_str, "%02hhx", &value[*length]);

  if (btif_is_niap_mode()) {
    if (in_encrypt_key_name_list && !is_key_encrypted) {
      VLOG(2) << __func__ << " encrypt section: " << section << " key:" << key;
      std::string encrypt_str =
          btif_convert_to_encrypt_key(*svalue_str_from_config);
      const char *cencrypt_str = encrypt_str.c_str();
      config_set_string(config, section, key, cencrypt_str);
    }
  } else {
    if (in_encrypt_key_name_list && is_key_encrypted) {
      config_set_string(config, section, key, cvalue_str);
    }
  }

  return true;
}

static bool btif_in_encrypt_key_name_list(std::string key) {
  return std::find(encrypt_key_name_list,
                   encrypt_key_name_list + ENCRYPT_KEY_NAME_LIST_SIZE,
                   key) != (encrypt_key_name_list + ENCRYPT_KEY_NAME_LIST_SIZE);
}

static bool btif_is_key_encrypted(int key_from_config_size,
                                  std::string key_type_string) {
  if (key_type_string.compare("LinkKey") == 0) {
    return sizeof(LinkKey) * 2 != key_from_config_size;
  } else if (key_type_string.compare("LE_KEY_PENC") == 0) {
    return sizeof(tBTM_LE_PENC_KEYS) * 2 != key_from_config_size;
  } else if (key_type_string.compare("LE_KEY_PID") == 0) {
    return sizeof(tBTM_LE_PID_KEYS) * 2 != key_from_config_size;
  } else if (key_type_string.compare("LE_KEY_LID") == 0) {
    return sizeof(tBTM_LE_PID_KEYS) * 2 != key_from_config_size;
  } else if (key_type_string.compare("LE_KEY_PCSRK") == 0) {
    return sizeof(tBTM_LE_PCSRK_KEYS) * 2 != key_from_config_size;
  } else if (key_type_string.compare("LE_KEY_LENC") == 0) {
    return sizeof(tBTM_LE_LENC_KEYS) * 2 != key_from_config_size;
  } else if (key_type_string.compare("LE_KEY_LCSRK") == 0) {
    return sizeof(tBTM_LE_LCSRK_KEYS) * 2 != key_from_config_size;
  } else {
    VLOG(2) << __func__ << ": " << key_type_string
            << " Key type is unknown, return false first";
    return false;
  }
}

static std::string btif_convert_to_unencrypt_key(
    const std::string& encrypt_str) {
  if (!encrypt_str.empty()) {
    std::string tmp_encrypt_str("");
    if (base::Base64Decode(encrypt_str, &tmp_encrypt_str)) {
      std::string unencrypt_str = btif_keystore.Decrypt(tmp_encrypt_str);
      if (!unencrypt_str.empty()) {
        return unencrypt_str;
      }
    } else {
      LOG(WARNING) << __func__
                   << ": base64string decode fail, will return empty string";
    }
  }
  return "";
}

size_t btif_config_get_bin_length(const char* section, const char* key) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  const char* value_str = config_get_string(config, section, key, NULL);
  if (!value_str) return 0;

  size_t value_len = strlen(value_str);
  return ((value_len % 2) != 0) ? 0 : (value_len / 2);
}

bool btif_config_set_bin(const char* section, const char* key,
                         const uint8_t* value, size_t length) {
  const char* lookup = "0123456789abcdef";

  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  if (length > 0) CHECK(value != NULL);

  char* str = (char*)osi_calloc(length * 2 + 1);

  for (size_t i = 0; i < length; ++i) {
    str[(i * 2) + 0] = lookup[(value[i] >> 4) & 0x0F];
    str[(i * 2) + 1] = lookup[value[i] & 0x0F];
  }

  std::string skey= key;
  std::string value_str;
  if (btif_is_niap_mode() && btif_in_encrypt_key_name_list(skey)) {
    VLOG(2) << __func__ << " encrypt section: " << section << " key:" << key;
    value_str = btif_convert_to_encrypt_key(str);
  } else {
    value_str = str;
  }
  if (value_str.empty()) return false;
  const char *enc_str = value_str.c_str();
  {
    std::unique_lock<std::recursive_mutex> lock(config_lock);
    config_set_string(config, section, key, enc_str);
  }

  osi_free(str);
  return true;
}

static std::string btif_convert_to_encrypt_key(
    const std::string& unencrypt_str) {
  if (!unencrypt_str.empty()) {
    std::string encrypt_str = btif_keystore.Encrypt(unencrypt_str, 0);
    if (!encrypt_str.empty()) {
      base::Base64Encode(encrypt_str, &encrypt_str);
      return encrypt_str;
    } else {
      LOG(ERROR) << __func__ << ": Encrypt fail, will return empty str.";
    }
  }
  return "";
}

const btif_config_section_iter_t* btif_config_section_begin(void) {
  CHECK(config != NULL);
  return (const btif_config_section_iter_t*)config_section_begin(config);
}

const btif_config_section_iter_t* btif_config_section_end(void) {
  CHECK(config != NULL);
  return (const btif_config_section_iter_t*)config_section_end(config);
}

const btif_config_section_iter_t* btif_config_section_next(
    const btif_config_section_iter_t* section) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  return (const btif_config_section_iter_t*)config_section_next(
      (const config_section_node_t*)section);
}

const char* btif_config_section_name(
    const btif_config_section_iter_t* section) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  return config_section_name((const config_section_node_t*)section);
}

bool btif_config_remove(const char* section, const char* key) {
  CHECK(config != NULL);
  CHECK(section != NULL);
  CHECK(key != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  return config_remove_key(config, section, key);
}

void btif_config_save(void) {
  CHECK(config != NULL);
  CHECK(config_timer != NULL);

  alarm_set(config_timer, CONFIG_SETTLE_PERIOD_MS, timer_config_save_cb, NULL);
}

void btif_config_flush(void) {
  CHECK(config != NULL);
  CHECK(config_timer != NULL);

  alarm_cancel(config_timer);
  btif_config_write(0, NULL);
}

bool btif_config_clear(void) {
  CHECK(config != NULL);
  CHECK(config_timer != NULL);

  alarm_cancel(config_timer);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  config_free(config);

  config = config_new_empty();
  if (config == NULL) return false;

  bool ret = config_save(config, CONFIG_FILE_PATH);
  btif_config_source = RESET;

  // Save encrypted hash
  std::string current_hash = hash_file(CONFIG_FILE_PATH);
  if (!current_hash.empty()) {
    write_checksum_file(CONFIG_FILE_CHECKSUM_PATH, current_hash);
  }

  return ret;
}

static void timer_config_save_cb(UNUSED_ATTR void* data) {
  // Moving file I/O to btif context instead of timer callback because
  // it usually takes a lot of time to be completed, introducing
  // delays during A2DP playback causing blips or choppiness.
  btif_transfer_context(btif_config_write, 0, NULL, 0, NULL);
}

static void btif_config_write(UNUSED_ATTR uint16_t event,
                              UNUSED_ATTR char* p_param) {
  CHECK(config != NULL);
  CHECK(config_timer != NULL);

  std::unique_lock<std::recursive_mutex> lock(config_lock);
  rename(CONFIG_FILE_PATH, CONFIG_BACKUP_PATH);
  rename(CONFIG_FILE_CHECKSUM_PATH, CONFIG_BACKUP_CHECKSUM_PATH);
  config_t* config_paired = config_new_clone(config);

  if (config_paired != NULL) {
    btif_config_remove_unpaired(config_paired);
    config_save(config_paired, CONFIG_FILE_PATH);
    config_free(config_paired);
  }
  // Save hash
  std::string current_hash = hash_file(CONFIG_FILE_PATH);
  if (!current_hash.empty()) {
    write_checksum_file(CONFIG_FILE_CHECKSUM_PATH, current_hash);
  }
}

static void btif_config_remove_unpaired(config_t* conf) {
  CHECK(conf != NULL);
  int paired_devices = 0;

  // The paired config used to carry information about
  // discovered devices during regular inquiry scans.
  // We remove these now and cache them in memory instead.
  const config_section_node_t* snode = config_section_begin(conf);
  while (snode != config_section_end(conf)) {
    const char* section = config_section_name(snode);
    if (RawAddress::IsValidAddress(section)) {
      if (!config_has_key(conf, section, "LinkKey") &&
          !config_has_key(conf, section, "LE_KEY_PENC") &&
          !config_has_key(conf, section, "LE_KEY_PID") &&
          !config_has_key(conf, section, "LE_KEY_PCSRK") &&
          !config_has_key(conf, section, "LE_KEY_LENC") &&
          !config_has_key(conf, section, "LE_KEY_LCSRK") &&
          !config_has_key(conf, section, "AvrcpCtVersion") &&
          !config_has_key(conf, section, "AvrcpFeatures") &&
          !config_has_key(conf, section, "TwsPlusPeerAddr") &&
          !config_has_key(conf, section, "Codecs")) {
        snode = config_section_next(snode);
        config_remove_section(conf, section);
        continue;
      }
      paired_devices++;
    }
    snode = config_section_next(snode);
  }

  // should only happen once, at initial load time
  if (btif_config_devices_loaded == -1)
    btif_config_devices_loaded = paired_devices;
}

void btif_debug_config_dump(int fd) {
  dprintf(fd, "\nBluetooth Config:\n");

  dprintf(fd, "  Config Source: ");
  switch (btif_config_source) {
    case NOT_LOADED:
      dprintf(fd, "Not loaded\n");
      break;
    case ORIGINAL:
      dprintf(fd, "Original file\n");
      break;
    case BACKUP:
      dprintf(fd, "Backup file\n");
      break;
    case LEGACY:
      dprintf(fd, "Legacy file\n");
      break;
    case NEW_FILE:
      dprintf(fd, "New file\n");
      break;
    case RESET:
      dprintf(fd, "Reset file\n");
      break;
  }

  dprintf(fd, "  Devices loaded: %d\n", btif_config_devices_loaded);
  dprintf(fd, "  File created/tagged: %s\n", btif_config_time_created);
  dprintf(fd, "  File source: %s\n",
          config_get_string(config, INFO_SECTION, FILE_SOURCE, "Original"));
}

static void btif_config_remove_restricted(config_t* config) {
  CHECK(config != NULL);

  const config_section_node_t* snode = config_section_begin(config);
  while (snode != config_section_end(config)) {
    const char* section = config_section_name(snode);
    if (RawAddress::IsValidAddress(section) &&
        config_has_key(config, section, "Restricted")) {
      BTIF_TRACE_DEBUG("%s: Removing restricted device %s", __func__, section);
      config_remove_section(config, section);
    }
    snode = config_section_next(snode);
  }
}

static bool is_factory_reset(void) {
  char factory_reset[PROPERTY_VALUE_MAX] = {0};
  osi_property_get("persist.bluetooth.factoryreset", factory_reset, "false");
  return strncmp(factory_reset, "true", 4) == 0;
}

static void delete_config_files(void) {
  remove(CONFIG_FILE_PATH);
  remove(CONFIG_BACKUP_PATH);
  remove(CONFIG_FILE_CHECKSUM_PATH);
  remove(CONFIG_BACKUP_CHECKSUM_PATH);
  osi_property_set("persist.bluetooth.factoryreset", "false");
}

static std::string hash_file(const char* filename) {
  if (!btif_is_niap_mode()) {
    LOG(INFO) << __func__ << ": Disabled for multi-user";
    return DISABLED;
  }
  FILE* fp = fopen(filename, "rb");
  if (!fp) {
    LOG(ERROR) << __func__ << ": unable to open config file: '" << filename
               << "': " << strerror(errno);
    return "";
  }
  uint8_t hash[SHA256_DIGEST_LENGTH];
  SHA256_CTX sha256;
  SHA256_Init(&sha256);
  std::array<std::byte, kBufferSize> buffer;
  int bytes_read = 0;
  while ((bytes_read = fread(buffer.data(), 1, buffer.size(), fp))) {
    SHA256_Update(&sha256, buffer.data(), bytes_read);
  }
  SHA256_Final(hash, &sha256);
  std::stringstream ss;
  for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
  }
  fclose(fp);
  return ss.str();
}

static std::string read_checksum_file(const char* checksum_filename) {
  if (!btif_is_niap_mode()) {
    LOG(INFO) << __func__ << ": Disabled for multi-user";
    return DISABLED;
  }
  std::string encrypted_hash = checksum_read(checksum_filename);
  if (encrypted_hash.empty()) {
    LOG(INFO) << __func__ << ": read empty hash.";

    return "";
  }
  return btif_keystore.Decrypt(encrypted_hash);
}

static void write_checksum_file(const char* checksum_filename,
                                const std::string& hash) {
  if (!btif_is_niap_mode()) {
    LOG(INFO) << __func__
              << ": Disabled for multi-user, since config changed removing "
                 "checksums.";
    remove(CONFIG_FILE_CHECKSUM_PATH);
    remove(CONFIG_BACKUP_CHECKSUM_PATH);
    return;
  }
  std::string encrypted_checksum = btif_keystore.Encrypt(hash, 0);
  CHECK(!encrypted_checksum.empty())
      << __func__ << ": Failed encrypting checksum";
  CHECK(checksum_save(encrypted_checksum, checksum_filename))
      << __func__ << ": Failed to save checksum!";
}
