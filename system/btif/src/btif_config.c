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

#include <assert.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "osi/include/alarm.h"
#include "osi/include/allocator.h"
#include "btcore/include/bdaddr.h"
#include "btif_config.h"
#include "btif_config_transcode.h"
#include "btif_util.h"
#include "osi/include/compat.h"
#include "osi/include/config.h"
#include "btcore/include/module.h"
#include "osi/include/osi.h"
#include "osi/include/log.h"

#include "bt_types.h"

// TODO(armansito): Find a better way than searching by a hardcoded path.
#if defined(OS_GENERIC)
static const char *CONFIG_FILE_PATH = "bt_config.conf";
static const char *CONFIG_BACKUP_PATH = "bt_config.bak";
static const char *CONFIG_LEGACY_FILE_PATH = "bt_config.xml";
#else  // !defined(OS_GENERIC)
static const char *CONFIG_FILE_PATH = "/data/misc/bluedroid/bt_config.conf";
static const char *CONFIG_BACKUP_PATH = "/data/misc/bluedroid/bt_config.bak";
static const char *CONFIG_LEGACY_FILE_PATH = "/data/misc/bluedroid/bt_config.xml";
#endif  // defined(OS_GENERIC)
static const period_ms_t CONFIG_SETTLE_PERIOD_MS = 3000;

static void timer_config_save_cb(void *data);
static void btif_config_write(void);
static void btif_config_devcache_cleanup(void);

// TODO(zachoverflow): Move these two functions out, because they are too specific for this file
// {grumpy-cat/no, monty-python/you-make-me-sad}
bool btif_get_device_type(const BD_ADDR bd_addr, int *p_device_type)
{
    if (p_device_type == NULL)
        return FALSE;

    bt_bdaddr_t bda;
    bdcpy(bda.address, bd_addr);

    bdstr_t bd_addr_str;
    bdaddr_to_string(&bda, bd_addr_str, sizeof(bd_addr_str));

    if (!btif_config_get_int(bd_addr_str, "DevType", p_device_type))
        return FALSE;

    LOG_DEBUG("%s: Device [%s] type %d", __FUNCTION__, bd_addr_str, *p_device_type);
    return TRUE;
}

bool btif_get_address_type(const BD_ADDR bd_addr, int *p_addr_type)
{
    if (p_addr_type == NULL)
        return FALSE;

    bt_bdaddr_t bda;
    bdcpy(bda.address, bd_addr);

    bdstr_t bd_addr_str;
    bdaddr_to_string(&bda, bd_addr_str, sizeof(bd_addr_str));

    if (!btif_config_get_int(bd_addr_str, "AddrType", p_addr_type))
        return FALSE;

    LOG_DEBUG("%s: Device [%s] address type %d", __FUNCTION__, bd_addr_str, *p_addr_type);
    return TRUE;
}

static pthread_mutex_t lock;  // protects operations on |config|.
static config_t *config;
static alarm_t *alarm_timer;

// Module lifecycle functions

static future_t *init(void) {
  pthread_mutex_init(&lock, NULL);
  config = config_new(CONFIG_FILE_PATH);
  if (!config) {
    LOG_WARN("%s unable to load config file: %s; using backup.",
              __func__, CONFIG_FILE_PATH);
    config = config_new(CONFIG_BACKUP_PATH);
  }
  if (!config) {
    LOG_WARN("%s unable to load backup; attempting to transcode legacy file.", __func__);
    config = btif_config_transcode(CONFIG_LEGACY_FILE_PATH);
  }
  if (!config) {
    LOG_ERROR("%s unable to transcode legacy file; creating empty config.", __func__);
    config = config_new_empty();
  }
  if (!config) {
    LOG_ERROR("%s unable to allocate a config object.", __func__);
    goto error;
  }

  btif_config_devcache_cleanup();

  // TODO(sharvil): use a non-wake alarm for this once we have
  // API support for it. There's no need to wake the system to
  // write back to disk.
  alarm_timer = alarm_new();
  if (!alarm_timer) {
    LOG_ERROR("%s unable to create alarm.", __func__);
    goto error;
  }

  return future_new_immediate(FUTURE_SUCCESS);

error:;
  alarm_free(alarm_timer);
  config_free(config);
  pthread_mutex_destroy(&lock);
  alarm_timer = NULL;
  config = NULL;
  return future_new_immediate(FUTURE_FAIL);
}

static future_t *shut_down(void) {
  btif_config_flush();
  return future_new_immediate(FUTURE_SUCCESS);
}

static future_t *clean_up(void) {
  btif_config_flush();

  alarm_free(alarm_timer);
  config_free(config);
  pthread_mutex_destroy(&lock);
  alarm_timer = NULL;
  config = NULL;
  return future_new_immediate(FUTURE_SUCCESS);
}

const module_t btif_config_module = {
  .name = BTIF_CONFIG_MODULE,
  .init = init,
  .start_up = NULL,
  .shut_down = shut_down,
  .clean_up = clean_up,
  .dependencies = {
    NULL
  }
};

bool btif_config_has_section(const char *section) {
  assert(config != NULL);
  assert(section != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_has_section(config, section);
  pthread_mutex_unlock(&lock);

  return ret;
}

bool btif_config_exist(const char *section, const char *key) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_has_key(config, section, key);
  pthread_mutex_unlock(&lock);

  return ret;
}

bool btif_config_get_int(const char *section, const char *key, int *value) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);
  assert(value != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_has_key(config, section, key);
  if (ret)
    *value = config_get_int(config, section, key, *value);
  pthread_mutex_unlock(&lock);

  return ret;
}

bool btif_config_set_int(const char *section, const char *key, int value) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);

  pthread_mutex_lock(&lock);
  config_set_int(config, section, key, value);
  pthread_mutex_unlock(&lock);

  return true;
}

bool btif_config_get_str(const char *section, const char *key, char *value, int *size_bytes) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);
  assert(value != NULL);
  assert(size_bytes != NULL);

  pthread_mutex_lock(&lock);
  const char *stored_value = config_get_string(config, section, key, NULL);
  pthread_mutex_unlock(&lock);

  if (!stored_value)
    return false;

  strlcpy(value, stored_value, *size_bytes);
  *size_bytes = strlen(value) + 1;

  return true;
}

bool btif_config_set_str(const char *section, const char *key, const char *value) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);
  assert(value != NULL);

  pthread_mutex_lock(&lock);
  config_set_string(config, section, key, value);
  pthread_mutex_unlock(&lock);

  return true;
}

bool btif_config_get_bin(const char *section, const char *key, uint8_t *value, size_t *length) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);
  assert(value != NULL);
  assert(length != NULL);

  pthread_mutex_lock(&lock);
  const char *value_str = config_get_string(config, section, key, NULL);
  pthread_mutex_unlock(&lock);

  if (!value_str)
    return false;

  size_t value_len = strlen(value_str);
  if ((value_len % 2) != 0 || *length < (value_len / 2))
    return false;

  for (size_t i = 0; i < value_len; ++i)
    if (!isxdigit(value_str[i]))
      return false;

  for (*length = 0; *value_str; value_str += 2, *length += 1)
    sscanf(value_str, "%02hhx", &value[*length]);

  return true;
}

size_t btif_config_get_bin_length(const char *section, const char *key) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);

  pthread_mutex_lock(&lock);
  const char *value_str = config_get_string(config, section, key, NULL);
  pthread_mutex_unlock(&lock);

  if (!value_str)
    return 0;

  size_t value_len = strlen(value_str);
  return ((value_len % 2) != 0) ? 0 : (value_len / 2);
}

bool btif_config_set_bin(const char *section, const char *key, const uint8_t *value, size_t length) {
  const char *lookup = "0123456789abcdef";

  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);

  if (length > 0)
      assert(value != NULL);

  char *str = (char *)osi_calloc(length * 2 + 1);
  if (!str)
    return false;

  for (size_t i = 0; i < length; ++i) {
    str[(i * 2) + 0] = lookup[(value[i] >> 4) & 0x0F];
    str[(i * 2) + 1] = lookup[value[i] & 0x0F];
  }

  pthread_mutex_lock(&lock);
  config_set_string(config, section, key, str);
  pthread_mutex_unlock(&lock);

  osi_free(str);
  return true;
}

const btif_config_section_iter_t *btif_config_section_begin(void) {
  assert(config != NULL);
  return (const btif_config_section_iter_t *)config_section_begin(config);
}

const btif_config_section_iter_t *btif_config_section_end(void) {
  assert(config != NULL);
  return (const btif_config_section_iter_t *)config_section_end(config);
}

const btif_config_section_iter_t *btif_config_section_next(const btif_config_section_iter_t *section) {
  assert(config != NULL);
  assert(section != NULL);
  return (const btif_config_section_iter_t *)config_section_next((const config_section_node_t *)section);
}

const char *btif_config_section_name(const btif_config_section_iter_t *section) {
  assert(config != NULL);
  assert(section != NULL);
  return config_section_name((const config_section_node_t *)section);
}

bool btif_config_remove(const char *section, const char *key) {
  assert(config != NULL);
  assert(section != NULL);
  assert(key != NULL);

  pthread_mutex_lock(&lock);
  bool ret = config_remove_key(config, section, key);
  pthread_mutex_unlock(&lock);

  return ret;
}

void btif_config_save(void) {
  assert(alarm_timer != NULL);
  assert(config != NULL);

  alarm_set(alarm_timer, CONFIG_SETTLE_PERIOD_MS, timer_config_save_cb, NULL);
}

void btif_config_flush(void) {
  assert(config != NULL);
  assert(alarm_timer != NULL);

  alarm_cancel(alarm_timer);

  btif_config_write();
}

bool btif_config_clear(void){
  assert(config != NULL);
  assert(alarm_timer != NULL);

  alarm_cancel(alarm_timer);

  pthread_mutex_lock(&lock);
  config_free(config);

  config = config_new_empty();
  if (config == NULL) {
    pthread_mutex_unlock(&lock);
    return false;
  }

  bool ret = config_save(config, CONFIG_FILE_PATH);
  pthread_mutex_unlock(&lock);
  return ret;
}

static void timer_config_save_cb(UNUSED_ATTR void *data) {
  btif_config_write();
}

static void btif_config_write(void) {
  assert(config != NULL);
  assert(alarm_timer != NULL);

  btif_config_devcache_cleanup();

  pthread_mutex_lock(&lock);
  rename(CONFIG_FILE_PATH, CONFIG_BACKUP_PATH);
  sync();
  config_save(config, CONFIG_FILE_PATH);
  pthread_mutex_unlock(&lock);
}

static void btif_config_devcache_cleanup(void) {
  assert(config != NULL);

  // The config accumulates cached information about remote
  // devices during regular inquiry scans. We remove some of these
  // so the cache doesn't grow indefinitely.
  // We don't remove information about bonded devices (which have link keys).
  static const size_t ADDRS_MAX = 512;
  size_t total_addrs = 0;

  pthread_mutex_lock(&lock);
  const config_section_node_t *snode = config_section_begin(config);
  while (snode != config_section_end(config)) {
    const char *section = config_section_name(snode);
    if (string_is_bdaddr(section)) {
      ++total_addrs;

      if ((total_addrs > ADDRS_MAX) &&
          !config_has_key(config, section, "LinkKey") &&
          !config_has_key(config, section, "LE_KEY_PENC") &&
          !config_has_key(config, section, "LE_KEY_PID") &&
          !config_has_key(config, section, "LE_KEY_PCSRK") &&
          !config_has_key(config, section, "LE_KEY_LENC") &&
          !config_has_key(config, section, "LE_KEY_LCSRK")) {
        snode = config_section_next(snode);
        config_remove_section(config, section);
        continue;
      }
    }
    snode = config_section_next(snode);
  }
  pthread_mutex_unlock(&lock);
}
