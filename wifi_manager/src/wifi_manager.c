#include "wifi_manager.h"

#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "net_events.h"
#include "ping/ping_sock.h"
#include "wifi_manager_config.h"

static const char* TAG = "WIFI_MGR";

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
  bool initialized;
  wifi_manager_state_t state;
  wifi_manager_config_t wifi_cfg; /* known_networks + AP config */
  bool reachability_started;
  uint32_t consecutive_reachability_count;
  esp_ping_handle_t reachability_ping_handle;
  esp_timer_handle_t reconnect_timer;
  esp_timer_handle_t connect_timeout_timer;
  char ip_addr[16];
} wifi_manager_ctx_t;

static wifi_manager_ctx_t s_ctx; /* zero-initialised at startup */

/* ------------------------------------------------------------------ */
/* Forward declarations for file-local helpers                          */
/* ------------------------------------------------------------------ */

static esp_err_t load_known_networks(void);
static esp_err_t save_known_networks(void);
static esp_err_t connect_to_best_network(int best_index);
static esp_err_t start_reachability_check(void);
static esp_err_t stop_reachability_check(void);
static void start_reconnect_scan(void);
static void schedule_reconnect(const char* reason);
static void connect_from_scan_results(void);
static void reconnect_timer_cb(void* arg);
static void connect_timeout_cb(void* arg);
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

esp_err_t wifi_manager_init(const wifi_manager_config_t* wifi_cfg) {
  if (s_ctx.initialized) {
    return ESP_OK;
  }

  s_ctx.wifi_cfg = *wifi_cfg;
  s_ctx.ip_addr[0] = '\0';

  ESP_LOGI(TAG, "Initializing WiFi manager...");

  esp_netif_t* sta_netif = esp_netif_create_default_wifi_sta();
  if (s_ctx.wifi_cfg.hostname[0] != '\0') {
    esp_netif_set_hostname(sta_netif, s_ctx.wifi_cfg.hostname);
  }
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  const esp_timer_create_args_t timer_args = {
      .callback = reconnect_timer_cb,
      .name = "wifi_reconnect",
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ctx.reconnect_timer));

  const esp_timer_create_args_t ct_args = {
      .callback = connect_timeout_cb,
      .name = "wifi_conn_timeout",
  };
  ESP_ERROR_CHECK(esp_timer_create(&ct_args, &s_ctx.connect_timeout_timer));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, &s_ctx));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, &s_ctx));

  load_known_networks();

  /*
   * Nothing that runs *over* the link is started here -- no SNTP, no mDNS. Those
   * subscribe to NET_EVENT and start themselves when there is something to run
   * over, which is what keeps this component buildable without them and them
   * usable without WiFi.
   */

  s_ctx.initialized = true;
  ESP_LOGI(TAG, "WiFi manager initialized successfully");
  return ESP_OK;
}

esp_err_t wifi_manager_deinit(void) {
  if (!s_ctx.initialized) {
    return ESP_OK;
  }

  wifi_manager_stop();
  esp_timer_stop(s_ctx.connect_timeout_timer);
  esp_timer_delete(s_ctx.connect_timeout_timer);
  esp_timer_stop(s_ctx.reconnect_timer);
  esp_timer_delete(s_ctx.reconnect_timer);
  esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler);
  esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler);
  esp_wifi_deinit();

  s_ctx.initialized = false;
  return ESP_OK;
}

esp_err_t wifi_manager_start_station_mode(void) {
  if (!s_ctx.initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  wifi_mode_t current_mode;
  ESP_ERROR_CHECK(esp_wifi_get_mode(&current_mode));

  if (current_mode == WIFI_MODE_AP) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
  }

  ESP_LOGI(TAG, "Starting WiFi in station mode...");
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());

  s_ctx.state = WIFI_MANAGER_STA_DISCONNECTED;
  return ESP_OK;
}

esp_err_t wifi_manager_start_ap_mode(void) {
  if (!s_ctx.initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  wifi_mode_t current_mode;
  ESP_ERROR_CHECK(esp_wifi_get_mode(&current_mode));

  if (current_mode == WIFI_MODE_STA) {
    stop_reachability_check();
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
  }

  ESP_LOGI(TAG, "Starting WiFi in AP mode...");

  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.ap.ssid, s_ctx.wifi_cfg.ap_ssid,
          sizeof(wifi_config.ap.ssid) - 1);
  strncpy((char*)wifi_config.ap.password, s_ctx.wifi_cfg.ap_password,
          sizeof(wifi_config.ap.password) - 1);
  wifi_config.ap.ssid_len = (uint8_t)strlen(s_ctx.wifi_cfg.ap_ssid);
  wifi_config.ap.channel = (uint8_t)s_ctx.wifi_cfg.ap_channel;
  wifi_config.ap.max_connection = (uint8_t)s_ctx.wifi_cfg.ap_max_connections;
  wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

  if (s_ctx.wifi_cfg.ap_password[0] == '\0') {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  s_ctx.state = WIFI_MANAGER_AP_MODE;
  ESP_LOGI(TAG, "WiFi AP started. SSID: %s", s_ctx.wifi_cfg.ap_ssid);
  return ESP_OK;
}

esp_err_t wifi_manager_stop(void) {
  if (!s_ctx.initialized) {
    return ESP_ERR_INVALID_STATE;
  }

  stop_reachability_check();

  if (s_ctx.state == WIFI_MANAGER_STA_CONNECTED) {
    ESP_ERROR_CHECK(esp_wifi_disconnect());
  }

  ESP_ERROR_CHECK(esp_wifi_stop());
  s_ctx.state = WIFI_MANAGER_POWERED_OFF;
  return ESP_OK;
}

esp_err_t wifi_manager_add_known_network(const char* ssid,
                                         const char* password) {
  wifi_manager_config_known_networks_t* nets = &s_ctx.wifi_cfg.known_networks;

  /* Update password if network already known */
  for (uint64_t i = 0; i < nets->n; i++) {
    if (strcmp(nets->items[i].ssid, ssid) == 0) {
      strncpy(nets->items[i].password, password,
              sizeof(nets->items[i].password) - 1);
      nets->items[i].password[sizeof(nets->items[i].password) - 1] = '\0';
      return save_known_networks();
    }
  }

  /* Add new entry if there is room */
  if (nets->n >= 4) {
    ESP_LOGW(TAG, "Known network list full, cannot add %s", ssid);
    return ESP_ERR_NO_MEM;
  }

  wifi_manager_config_known_networks_item_t* item = &nets->items[nets->n];
  strncpy(item->ssid, ssid, sizeof(item->ssid) - 1);
  strncpy(item->password, password, sizeof(item->password) - 1);
  item->ssid[sizeof(item->ssid) - 1] = '\0';
  item->password[sizeof(item->password) - 1] = '\0';
  item->priority = 0;
  nets->n += 1;

  ESP_LOGI(TAG, "Added known network: %s", ssid);
  return save_known_networks();
}

esp_err_t wifi_manager_remove_known_network(const char* ssid) {
  wifi_manager_config_known_networks_t* nets = &s_ctx.wifi_cfg.known_networks;

  for (uint64_t i = 0; i < nets->n; i++) {
    if (strcmp(nets->items[i].ssid, ssid) == 0) {
      /* Shift remaining entries down */
      for (uint64_t j = i; j < nets->n - 1; j++) {
        nets->items[j] = nets->items[j + 1];
      }
      nets->n -= 1;
      ESP_LOGI(TAG, "Removed known network: %s", ssid);
      return save_known_networks();
    }
  }

  return ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_manager_scan_and_connect(void) {
  if (!s_ctx.initialized || s_ctx.state == WIFI_MANAGER_STA_CONNECTED) {
    return ESP_ERR_INVALID_STATE;
  }

  ESP_LOGI(TAG, "Scanning for known networks...  Current state: %d",
           (int)s_ctx.state);

  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
  };

  /* Non-blocking: WIFI_EVENT_SCAN_DONE → connect_from_scan_results() */
  esp_err_t ret = esp_wifi_scan_start(&scan_config, false);
  if (ret != ESP_OK) {
    /* esp_wifi_start() is asynchronous, so an immediate call from app_main can
     * land before WIFI_EVENT_STA_START and fail with ESP_ERR_WIFI_NOT_STARTED.
     * Fall back to the timer so a failed first scan still gets retried. */
    ESP_LOGE(TAG, "Failed to start WiFi scan: %x - %s", ret,
             esp_err_to_name(ret));
    schedule_reconnect("initial scan start failed");
  }
  return ret;
}

wifi_manager_state_t wifi_manager_get_state(void) { return s_ctx.state; }

esp_err_t wifi_manager_get_address(char *dest, size_t length) {
  strncpy(dest, s_ctx.ip_addr, length);

  return ESP_OK;
}

esp_err_t wifi_manager_get_rssi(int8_t* rssi) {
  if (s_ctx.state != WIFI_MANAGER_STA_CONNECTED) {
    *rssi = 0;
    return ESP_OK;
  }

  wifi_ap_record_t ap_record;
  esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_record);
  if (ret == ESP_OK) {
    *rssi = ap_record.rssi;
  } else {
    *rssi = 0;
    ESP_LOGE(TAG, "Failed to retrieve station info: 0x%x %s", ret,
             esp_err_to_name(ret));
  }
  return ret;
}

bool wifi_manager_has_clients(void) {
  if (s_ctx.state != WIFI_MANAGER_AP_MODE) {
    return false;
  }

  wifi_sta_list_t sta_list;
  esp_wifi_ap_get_sta_list(&sta_list);
  return sta_list.num > 0;
}

esp_err_t wifi_manager_force_disconnect(void) {
  ESP_LOGI(TAG, "Forcing station disconnect");
  return esp_wifi_disconnect();
}

void wifi_manager_record_reachability_result(bool is_reachable) {
  if (is_reachable) {
    s_ctx.consecutive_reachability_count = 0;
  } else {
    s_ctx.consecutive_reachability_count++;
  }
}

uint32_t wifi_manager_consecutive_reachability_failures(void) {
  return s_ctx.consecutive_reachability_count;
}

/* ------------------------------------------------------------------ */
/* File-local helpers                                                   */
/* ------------------------------------------------------------------ */

/* The reconnect timer is one-shot. Every path that leaves us without a
 * connection must route through here, or reconnection stops permanently. */
static void schedule_reconnect(const char* reason) {
  ESP_LOGI(TAG, "Scheduling reconnect scan in %" PRIu64 " ms (%s)",
           s_ctx.wifi_cfg.scan_interval_ms, reason);
  esp_timer_stop(s_ctx.reconnect_timer);
  esp_timer_start_once(s_ctx.reconnect_timer,
                       s_ctx.wifi_cfg.scan_interval_ms * 1000ULL);
}

static void connect_timeout_cb(void* arg) {
  ESP_LOGW(TAG, "Connection attempt timed out, forcing disconnect");
  /* Reset the state ourselves: esp_wifi_disconnect() does not always produce a
   * STA_DISCONNECTED event, and a state stuck at STA_CONNECTING makes every
   * later reconnect attempt a silent no-op. */
  s_ctx.state = WIFI_MANAGER_STA_DISCONNECTED;
  esp_wifi_disconnect();
  schedule_reconnect("connect timeout");
}

static void start_reconnect_scan(void) {
  if (s_ctx.state == WIFI_MANAGER_STA_CONNECTING) {
    /* An attempt is still in flight; the connect timeout will resolve it. Check
     * back rather than leaving the one-shot timer disarmed. */
    schedule_reconnect("connect still in progress");
    return;
  }
  if (s_ctx.state != WIFI_MANAGER_STA_DISCONNECTED) {
    /* Connected, AP mode or powered off - nothing to reconnect. */
    return;
  }

  ESP_LOGI(TAG, "Starting reconnect scan...");
  wifi_scan_config_t scan_config = {
      .ssid = NULL,
      .bssid = NULL,
      .channel = 0,
      .show_hidden = false,
  };
  esp_err_t ret = esp_wifi_scan_start(&scan_config, false); /* non-blocking */
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start reconnect scan: 0x%x - %s", ret,
             esp_err_to_name(ret));
    schedule_reconnect("scan start failed");
  }
}

static void connect_from_scan_results(void) {
  uint16_t count = 16;
  static wifi_ap_record_t ap_info[16];

  /* This call also releases the driver's scan result buffer. */
  esp_err_t ret = esp_wifi_scan_get_ap_records(&count, ap_info);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read scan results: 0x%x - %s", ret,
             esp_err_to_name(ret));
    schedule_reconnect("scan results unavailable");
    return;
  }

  const wifi_manager_config_known_networks_t* nets =
      &s_ctx.wifi_cfg.known_networks;

  ESP_LOGI(TAG,
           "Scan found %d wifi networks.  %" PRIu64 " known networks configured",
           count, nets->n);

  int best_index = -1;
  int8_t best_rssi = -100;

  for (int i = 0; i < count; i++) {
    const char* scanned_ssid = (const char*)ap_info[i].ssid;
    bool is_known = false;
    for (uint64_t k = 0; k < nets->n; k++) {
      if (strcmp(nets->items[k].ssid, scanned_ssid) == 0) {
        is_known = true;
        ESP_LOGI(TAG, "Found known network: %s (RSSI: %d)", scanned_ssid,
                 ap_info[i].rssi);
        if (ap_info[i].rssi > best_rssi) {
          best_rssi = ap_info[i].rssi;
          best_index = (int)k;
        }
      }
    }
    if (!is_known) {
      ESP_LOGI(TAG, "Found unknown network: %s (RSSI: %d)", scanned_ssid,
               ap_info[i].rssi);
    }
  }

  if (best_index >= 0) {
    connect_to_best_network(best_index);
  } else {
    schedule_reconnect("no known networks in scan");
  }
}

static void reconnect_timer_cb(void* arg) { start_reconnect_scan(); }

static esp_err_t load_known_networks(void) {
  /* Placeholder — load from NVS */
  ESP_LOGI(TAG, "Loading known networks from NVS (placeholder)");
  return ESP_OK;
}

static esp_err_t save_known_networks(void) {
  /* Placeholder — save to NVS */
  ESP_LOGI(TAG, "Saving known networks to NVS (placeholder)");
  return ESP_OK;
}

static esp_err_t connect_to_best_network(int best_index) {
  const wifi_manager_config_known_networks_t* nets =
      &s_ctx.wifi_cfg.known_networks;

  if (nets->n == 0) {
    s_ctx.state = WIFI_MANAGER_STA_DISCONNECTED;
    schedule_reconnect("no known networks configured");
    return ESP_ERR_NOT_FOUND;
  }

  const wifi_manager_config_known_networks_item_t* network =
      &nets->items[best_index];

  ESP_LOGI(TAG, "Connecting to: %s", network->ssid);
  s_ctx.state = WIFI_MANAGER_STA_CONNECTING;

  wifi_config_t wifi_config = {};
  strncpy((char*)wifi_config.sta.ssid, network->ssid,
          sizeof(wifi_config.sta.ssid) - 1);
  strncpy((char*)wifi_config.sta.password, network->password,
          sizeof(wifi_config.sta.password) - 1);
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set station config: 0x%x - %s", ret,
             esp_err_to_name(ret));
    s_ctx.state = WIFI_MANAGER_STA_DISCONNECTED;
    schedule_reconnect("set_config failed");
    return ret;
  }

  ret = esp_wifi_connect();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_connect failed: 0x%x - %s", ret,
             esp_err_to_name(ret));
    s_ctx.state = WIFI_MANAGER_STA_DISCONNECTED;
    schedule_reconnect("connect call failed");
    return ret;
  }

  esp_timer_stop(s_ctx.connect_timeout_timer);
  esp_timer_start_once(s_ctx.connect_timeout_timer,
                       s_ctx.wifi_cfg.connection_timeout_ms * 1000ULL);

  ESP_LOGI(TAG, "Connect called");
  return ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
  wifi_manager_ctx_t* ctx = (wifi_manager_ctx_t*)arg;

  if (event_base == WIFI_EVENT) {
    switch (event_id) {
      case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "WiFi station started");
        ctx->state = WIFI_MANAGER_STA_DISCONNECTED;
        break;
      case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "WiFi station connected; waiting for IP");
        /* Associated, but state stays STA_CONNECTING until GOT_IP. Re-arm the
         * timeout so a stalled DHCP cannot wedge us there. */
        esp_timer_stop(ctx->connect_timeout_timer);
        esp_timer_start_once(ctx->connect_timeout_timer,
                             ctx->wifi_cfg.connection_timeout_ms * 1000ULL);
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t* disc =
            (const wifi_event_sta_disconnected_t*)event_data;
        ESP_LOGI(TAG, "WiFi station disconnected (reason %d)",
                 disc != NULL ? disc->reason : -1);
        ctx->state = WIFI_MANAGER_STA_DISCONNECTED;

        stop_reachability_check();

        esp_timer_stop(ctx->connect_timeout_timer);
        schedule_reconnect("station disconnected");

        net_events_post(NET_EVENT_LINK_DOWN, NULL, 0);

        break;
      }
      case WIFI_EVENT_STA_BEACON_TIMEOUT:
        ESP_LOGW(TAG, "Station beacon stop");
        break;
      case WIFI_EVENT_SCAN_DONE:
        if (ctx->state == WIFI_MANAGER_STA_DISCONNECTED) {
          connect_from_scan_results();
        } else {
          /* Results we will not use still pin driver memory until they are
           * fetched or cleared; leaving them makes every later
           * esp_wifi_scan_start() fail with ESP_ERR_WIFI_STATE. */
          esp_wifi_clear_ap_list();
        }
        break;
      case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "WiFi AP started");
        ctx->state = WIFI_MANAGER_AP_MODE;
        {
          /* Unlike a station there is no lease to wait for: the address is fixed
           * by the DHCP server we run, so the AP is reachable the moment it is up. */
          net_event_link_t info;
          esp_netif_ip_info_t ip_info;
          esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
          if (ap != NULL && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
            snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&ip_info.ip));
          } else {
            info.ip[0] = '\0';
          }
          net_events_post(NET_EVENT_AP_STARTED, &info, sizeof(info));
        }
        break;
      case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Station connected to AP");
        break;
      case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Station disconnected from AP");
        break;
      case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "AP Mode Stopped");
        net_events_post(NET_EVENT_AP_STOPPED, NULL, 0);
        break;
    }
  } else if (event_base == IP_EVENT) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP: {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;

        snprintf(ctx->ip_addr, sizeof(ctx->ip_addr), IPSTR, IP2STR(&event->ip_info.ip));

        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ctx->state = WIFI_MANAGER_STA_CONNECTED;

        {
          net_event_link_t info;
          snprintf(info.ip, sizeof(info.ip), "%s", ctx->ip_addr);
          net_events_post(NET_EVENT_LINK_UP, &info, sizeof(info));
        }

        esp_timer_stop(ctx->connect_timeout_timer);
        esp_timer_stop(ctx->reconnect_timer);

        start_reachability_check();

        break;
      }
      case IP_EVENT_STA_LOST_IP: {
        ESP_LOGW(TAG, "Lost IP: %s", ctx->ip_addr);

        ctx->ip_addr[0] = '\0';
        ctx->state = WIFI_MANAGER_STA_DISCONNECTED;

        stop_reachability_check();
        schedule_reconnect("lost IP");

        net_events_post(NET_EVENT_LINK_DOWN, NULL, 0);

        break;
      }
    }
  }
}

static void on_ping_success(esp_ping_handle_t handle, void* args) {
  ESP_LOGI(TAG, "Reachability success");
  wifi_manager_ctx_t* ctx = (wifi_manager_ctx_t*)args;
  wifi_manager_record_reachability_result(true);
  (void)ctx;
}

static void on_ping_timeout(esp_ping_handle_t handle, void* args) {
  ESP_LOGI(TAG, "Reachability timeout");
  wifi_manager_ctx_t* ctx = (wifi_manager_ctx_t*)args;
  wifi_manager_record_reachability_result(false);
  (void)ctx;
}

static void reachability_task(void* arg) {
  ip_addr_t target_addr;
  struct addrinfo hint;
  struct addrinfo* res = NULL;

  memset(&hint, 0, sizeof(hint));
  memset(&target_addr, 0, sizeof(target_addr));

  getaddrinfo(s_ctx.wifi_cfg.reachability_host, NULL, &hint, &res);

  if (res) {
    struct in_addr addr4 = ((struct sockaddr_in*)(res->ai_addr))->sin_addr;
    inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
    freeaddrinfo(res);

    /* Abort if we disconnected while the DNS lookup was in flight */
    if (s_ctx.state != WIFI_MANAGER_STA_CONNECTED) {
      ESP_LOGW(TAG, "Disconnected during reachability DNS lookup, aborting");
      vTaskDelete(NULL);
      return;
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = ESP_PING_COUNT_INFINITE;
    ping_config.interval_ms = 10000;

    esp_ping_callbacks_t ping_callbacks = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end = NULL,
        .cb_args = &s_ctx,
    };

    /* Drop any session left over from a previous connection rather than
     * overwriting the handle and leaking it. */
    if (s_ctx.reachability_ping_handle != NULL) {
      esp_ping_stop(s_ctx.reachability_ping_handle);
      esp_ping_delete_session(s_ctx.reachability_ping_handle);
      s_ctx.reachability_ping_handle = NULL;
    }

    if (esp_ping_new_session(&ping_config, &ping_callbacks,
                             &s_ctx.reachability_ping_handle) != ESP_OK) {
      ESP_LOGW(TAG, "Failed to create reachability ping session");
      s_ctx.reachability_ping_handle = NULL;
      vTaskDelete(NULL);
      return;
    }

    /* Set before starting so a disconnect racing with us still tears down. */
    s_ctx.reachability_started = true;
    esp_ping_start(s_ctx.reachability_ping_handle);

    ESP_LOGI(TAG, "Reachability ping started");
  } else {
    ESP_LOGW(TAG, "Unable to lookup reachability host");
  }

  vTaskDelete(NULL);
}

static esp_err_t start_reachability_check(void) {
  /* Optional: with no host configured there is nothing to ping, and a device on
   * an isolated network should not be reported unreachable for it. */
  if (s_ctx.wifi_cfg.reachability_host[0] == '\0') {
    return ESP_OK;
  }

  s_ctx.consecutive_reachability_count = 0;
  xTaskCreate(reachability_task, "reachability", 4096, NULL, 5, NULL);
  return ESP_OK;
}

static esp_err_t stop_reachability_check(void) {
  if (!s_ctx.reachability_started) {
    return ESP_OK;
  }

  s_ctx.reachability_started = false;

  if (s_ctx.reachability_ping_handle != NULL) {
    esp_ping_stop(s_ctx.reachability_ping_handle);
    esp_ping_delete_session(s_ctx.reachability_ping_handle);
    s_ctx.reachability_ping_handle = NULL;
  }

  ESP_LOGI(TAG, "Reachability ping stopped");
  return ESP_OK;
}
