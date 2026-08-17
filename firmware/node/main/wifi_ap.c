#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "cJSON.h"
#include "wifi_ap.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "esp_timer.h"

static const char *TAG = "wifi_ap";

static QueueHandle_t s_evt_q = NULL;
httpd_handle_t ws_server = NULL;
extern bool master_absent;

#define MAX_SESSIONS 15
typedef struct {
    int ws_fd;
    uint32_t attach_start_time;
    char phone_num[UID_BUF_SIZE];
} session_info_t;

static session_info_t s_sessions[MAX_SESSIONS];
static portMUX_TYPE s_sessions_lock = portMUX_INITIALIZER_UNLOCKED;

static const char html_page[] = 
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1, user-scalable=0\">\n"
    "  <title>radcom</title>\n"
    "  <style>\n"
    "    body { background: #121212; color: #fff; font-family: sans-serif; margin: 0; padding: 10px; display: flex; flex-direction: column; height: 100vh; box-sizing: border-box; }\n"
    "    #login, #chat { display: none; flex-direction: column; height: 100%; }\n"
    "    input, button { padding: 12px; margin: 5px 0; border-radius: 5px; border: none; font-size: 16px; }\n"
    "    input { background: #333; color: #fff; }\n"
    "    button { background: #007bff; color: #fff; cursor: pointer; }\n"
    "    #messages { flex: 1; overflow-y: auto; background: #222; padding: 10px; border-radius: 5px; margin-bottom: 10px; }\n"
    "    .msg { margin-bottom: 8px; }\n"
    "    .msg b { color: #007bff; }\n"
    "    #status { font-size: 12px; color: #aaa; text-align: right; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div id=\"login\">\n"
    "    <h2>radcom</h2>\n"
    "    <p>Enter your mobile number to connect:</p>\n"
    "    <input type=\"tel\" id=\"phone\" placeholder=\"919876543210\">\n"
    "    <button onclick=\"connect()\">Connect</button>\n"
    "  </div>\n"
    "  <div id=\"chat\">\n"
    "    <div style=\"display: flex; justify-content: space-between; align-items: center;\">\n"
    "      <h2>radcom</h2>\n"
    "      <div id=\"status\">Connecting...</div>\n"
    "    </div>\n"
    "    <div id=\"messages\"></div>\n"
    "    <input type=\"tel\" id=\"to\" placeholder=\"To (Phone Number)\">\n"
    "    <div style=\"display: flex; gap: 5px;\">\n"
    "      <input type=\"text\" id=\"text\" placeholder=\"Message\" style=\"flex: 1;\">\n"
    "      <button onclick=\"sendMsg()\">Send</button>\n"
    "    </div>\n"
    "  </div>\n"
    "  <script>\n"
    "    let ws = null;\n"
    "    let myPhone = localStorage.getItem('radcom_phone');\n"
    "    if (myPhone) {\n"
    "      document.getElementById('phone').value = myPhone;\n"
    "      connect();\n"
    "    } else {\n"
    "      document.getElementById('login').style.display = 'flex';\n"
    "    }\n"
    "    function logMsg(from, text) {\n"
    "      let m = document.getElementById('messages');\n"
    "      let div = document.createElement('div');\n"
    "      div.className = 'msg';\n"
    "      let b = document.createElement('b');\n"
    "      b.textContent = from;\n"
    "      div.appendChild(b);\n"
    "      div.appendChild(document.createTextNode(': ' + text));\n"
    "      m.appendChild(div);\n"
    "      m.scrollTop = m.scrollHeight;\n"
    "    }\n"
    "    function connect() {\n"
    "      myPhone = document.getElementById('phone').value;\n"
    "      if (!myPhone) return;\n"
    "      localStorage.setItem('radcom_phone', myPhone);\n"
    "      document.getElementById('login').style.display = 'none';\n"
    "      document.getElementById('chat').style.display = 'flex';\n"
    "      ws = new WebSocket('ws://192.168.4.1/ws');\n"
    "      ws.onopen = () => {\n"
    "        ws.send(JSON.stringify({type: 'attach', phone: myPhone}));\n"
    "      };\n"
    "      ws.onmessage = (e) => {\n"
    "        let data = JSON.parse(e.data);\n"
    "        if (data.type === 'attached') {\n"
    "          document.getElementById('status').innerText = 'Online';\n"
    "          document.getElementById('status').style.color = '#0f0';\n"
    "        } else if (data.type === 'msg') {\n"
    "          logMsg(data.from, data.text);\n"
    "        } else if (data.type === 'status') {\n"
    "          document.getElementById('status').innerText = data.online ? 'Online' : 'Master Offline';\n"
    "          document.getElementById('status').style.color = data.online ? '#0f0' : '#f00';\n"
    "        } else if (data.type === 'sent') {\n"
    "          // Sent confirmation\n"
    "        } else if (data.type === 'error') {\n"
    "          logMsg('System Error', data.msg);\n"
    "        }\n"
    "      };\n"
    "      ws.onclose = () => {\n"
    "        document.getElementById('status').innerText = 'Disconnected';\n"
    "        document.getElementById('status').style.color = '#f00';\n"
    "        setTimeout(connect, 3000);\n"
    "      };\n"
    "    }\n"
    "    function sendMsg() {\n"
    "      let to = document.getElementById('to').value;\n"
    "      let text = document.getElementById('text').value;\n"
    "      if (!to || !text || !ws) return;\n"
    "      ws.send(JSON.stringify({type: 'msg', to: to, text: text}));\n"
    "      logMsg('Me', text);\n"
    "      document.getElementById('text').value = '';\n"
    "    }\n"
    "  </script>\n"
    "</body>\n"
    "</html>";

static esp_err_t ws_send_json_sync(httpd_req_t *req, const char *json_str) {
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)json_str;
    ws_pkt.len = strlen(json_str);
    return httpd_ws_send_frame(req, &ws_pkt);
}

struct async_resp_arg {
    int fd;
    char *json_str;
};

static void ws_send_json_async_handler(void *arg) {
    struct async_resp_arg *resp_arg = arg;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    ws_pkt.payload = (uint8_t *)resp_arg->json_str;
    ws_pkt.len = strlen(resp_arg->json_str);
    
    httpd_ws_send_frame_async(ws_server, resp_arg->fd, &ws_pkt);
    free(resp_arg->json_str);
    free(resp_arg);
}

esp_err_t ws_send_json(int ws_fd, const char *json_str) {
    if (!ws_server) return ESP_FAIL;
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (!resp_arg) return ESP_ERR_NO_MEM;
    resp_arg->fd = ws_fd;
    resp_arg->json_str = strdup(json_str);
    if (!resp_arg->json_str) {
        free(resp_arg);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = httpd_queue_work(ws_server, ws_send_json_async_handler, resp_arg);
    if (ret != ESP_OK) {
        free(resp_arg->json_str);
        free(resp_arg);
    }
    return ret;
}

void ws_disconnect(int ws_fd) {
    if (!ws_server) return;
    httpd_sess_trigger_close(ws_server, ws_fd);
}

static esp_err_t catch_all_handler(httpd_req_t *req) {
    char host[32] = {0};
    
    /* 1. If Host header exists but doesn't match our AP IP, redirect to it */
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
        if (strcmp(host, AP_IP) != 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
    }

    /* 2. If URI is not root, redirect to root */
    if (strcmp(req->uri, "/") != 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" AP_IP "/");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    /* 3. Serve captive portal HTML */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        portENTER_CRITICAL(&s_sessions_lock);
        bool found = false;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].ws_fd == 0) {
                s_sessions[i].ws_fd = fd;
                s_sessions[i].attach_start_time = esp_timer_get_time() / 1000;
                s_sessions[i].phone_num[0] = '\0';
                found = true;
                break;
            }
        }
        portEXIT_CRITICAL(&s_sessions_lock);
        if (!found) {
            ESP_LOGW(TAG, "Session list full");
        }
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) return ret;

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        char phone_to_detach[UID_BUF_SIZE] = {0};
        portENTER_CRITICAL(&s_sessions_lock);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].ws_fd == fd) {
                if (s_sessions[i].phone_num[0] != '\0') {
                    strncpy(phone_to_detach, s_sessions[i].phone_num, UID_BUF_SIZE);
                }
                s_sessions[i].ws_fd = 0;
                break;
            }
        }
        portEXIT_CRITICAL(&s_sessions_lock);
        
        if (phone_to_detach[0] != '\0') {
            ws_event_t ev;
            ev.type = WS_EVT_DETACH;
            ev.ws_fd = fd;
            strncpy(ev.phone_num, phone_to_detach, UID_BUF_SIZE);
            xQueueSend(s_evt_q, &ev, 0);
        }
        return ESP_OK;
    } else if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        httpd_ws_frame_t pong_frame = { .type = HTTPD_WS_TYPE_PONG, .payload = NULL, .len = 0 };
        return httpd_ws_send_frame(req, &pong_frame);
    } else if (ws_pkt.type != HTTPD_WS_TYPE_TEXT) {
        return ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"bad_type\",\"msg\":\"Binary not supported\"}");
    }

    if (ws_pkt.len == 0) return ESP_OK;

    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        free(buf);
        return ret;
    }

    cJSON *root = cJSON_Parse((const char *)buf);
    if (!root) {
        ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"bad_json\",\"msg\":\"Invalid JSON\"}");
        free(buf);
        return ESP_OK;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"bad_json\",\"msg\":\"Missing type\"}");
        cJSON_Delete(root);
        free(buf);
        return ESP_OK;
    }

    if (strcmp(type->valuestring, "attach") == 0) {
        cJSON *phone = cJSON_GetObjectItem(root, "phone");
        if (cJSON_IsString(phone) && uid_valid(phone->valuestring)) {
            bool found = false;
            char phone_to_attach[UID_BUF_SIZE] = {0};
            
            portENTER_CRITICAL(&s_sessions_lock);
            for (int i = 0; i < MAX_SESSIONS; i++) {
                if (s_sessions[i].ws_fd == fd) {
                    strncpy(s_sessions[i].phone_num, phone->valuestring, UID_BUF_SIZE - 1);
                    s_sessions[i].phone_num[UID_BUF_SIZE - 1] = '\0';
                    strncpy(phone_to_attach, s_sessions[i].phone_num, UID_BUF_SIZE);
                    found = true;
                    break;
                }
            }
            portEXIT_CRITICAL(&s_sessions_lock);
            
            if (found) {
                ws_event_t ev;
                ev.type = WS_EVT_ATTACH;
                ev.ws_fd = fd;
                strncpy(ev.phone_num, phone_to_attach, UID_BUF_SIZE);
                if (xQueueSend(s_evt_q, &ev, 0) != pdTRUE) {
                    ESP_LOGE(TAG, "Event queue full");
                    ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"busy\",\"msg\":\"Queue full\"}");
                }
            }
        } else {
            ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"bad_uid\",\"msg\":\"Invalid phone\"}");
        }
    } else if (strcmp(type->valuestring, "msg") == 0) {
        cJSON *to = cJSON_GetObjectItem(root, "to");
        cJSON *text = cJSON_GetObjectItem(root, "text");
        
        char sender[UID_BUF_SIZE] = {0};
        portENTER_CRITICAL(&s_sessions_lock);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].ws_fd == fd) {
                strncpy(sender, s_sessions[i].phone_num, UID_BUF_SIZE);
                break;
            }
        }
        portEXIT_CRITICAL(&s_sessions_lock);
        
        if (sender[0] == '\0') {
            ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"not_attached\",\"msg\":\"Not attached\"}");
        } else if (cJSON_IsString(to) && uid_valid(to->valuestring) && cJSON_IsString(text)) {
            ws_event_t ev;
            ev.type = WS_EVT_MESSAGE;
            ev.ws_fd = fd;
            strncpy(ev.phone_num, sender, UID_BUF_SIZE);
            strncpy(ev.dst_uid, to->valuestring, UID_BUF_SIZE - 1);
            ev.dst_uid[UID_BUF_SIZE - 1] = '\0';
            
            size_t text_len = strlen(text->valuestring);
            if (text_len > PAYLOAD_MAX) {
                ESP_LOGW(TAG, "Text too long, truncating");
                text_len = PAYLOAD_MAX;
            }
            ev.text_len = text_len;
            memcpy(ev.text, text->valuestring, text_len);
            
            if (xQueueSend(s_evt_q, &ev, 0) != pdTRUE) {
                ESP_LOGE(TAG, "Event queue full");
                ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"busy\",\"msg\":\"Queue full\"}");
            }
        } else {
            ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"bad_msg\",\"msg\":\"Invalid msg\"}");
        }
    } else {
        ws_send_json_sync(req, "{\"type\":\"error\",\"code\":\"bad_type\",\"msg\":\"Unknown type\"}");
    }

    cJSON_Delete(root);
    free(buf);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

static void dns_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx_buffer[128];
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            ESP_LOGE(TAG, "DNS recvfrom failed");
            continue;
        }

        if (len > 12) {
            uint8_t tx_buffer[128];
            memcpy(tx_buffer, rx_buffer, len);
            
            tx_buffer[2] = 0x84 | (rx_buffer[2] & 0x01);
            tx_buffer[3] = 0x00;
            tx_buffer[6] = 0x00;
            tx_buffer[7] = 0x01;
            tx_buffer[8] = 0x00;
            tx_buffer[9] = 0x00;
            tx_buffer[10] = 0x00;
            tx_buffer[11] = 0x00;
            
            int tx_len = len;
            if (tx_len + 16 <= sizeof(tx_buffer)) {
                tx_buffer[tx_len++] = 0xC0;
                tx_buffer[tx_len++] = 0x0C;
                tx_buffer[tx_len++] = 0x00;
                tx_buffer[tx_len++] = 0x01;
                tx_buffer[tx_len++] = 0x00;
                tx_buffer[tx_len++] = 0x01;
                tx_buffer[tx_len++] = 0x00;
                tx_buffer[tx_len++] = 0x00;
                tx_buffer[tx_len++] = 0x00;
                tx_buffer[tx_len++] = 0x3C;
                tx_buffer[tx_len++] = 0x00;
                tx_buffer[tx_len++] = 0x04;
                tx_buffer[tx_len++] = 192;
                tx_buffer[tx_len++] = 168;
                tx_buffer[tx_len++] = 4;
                tx_buffer[tx_len++] = 1;
                
                sendto(sock, tx_buffer, tx_len, 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
            }
        }
    }
}

esp_err_t wifi_ap_init(QueueHandle_t evt_q) {
    s_evt_q = evt_q;
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = AP_CHANNEL,
            .password = AP_PASS,
            .max_connection = AP_MAX_STATIONS,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };
    if (strlen(AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    /* Throttle transmit power to 8.5 dBm (34) to prevent SuperMini LDO brownout */
    esp_wifi_set_max_tx_power(34);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 7;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    if (httpd_start(&ws_server, &config) == ESP_OK) {
        httpd_uri_t ws = {
            .uri        = WS_PATH,
            .method     = HTTP_GET,
            .handler    = ws_handler,
            .user_ctx   = NULL,
            .is_websocket = true
        };
        httpd_register_uri_handler(ws_server, &ws);

        httpd_uri_t catch_all = {
            .uri        = "/*",
            .method     = HTTP_GET,
            .handler    = catch_all_handler,
            .user_ctx   = NULL
        };
        httpd_register_uri_handler(ws_server, &catch_all);
    }

    xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL);

    return ESP_OK;
}

void wifi_task(void *arg) {
    bool last_master_status = master_absent;
    
    while (1) {
        uint32_t now = esp_timer_get_time() / 1000;
        
        portENTER_CRITICAL(&s_sessions_lock);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (s_sessions[i].ws_fd != 0 && s_sessions[i].phone_num[0] == '\0') {
                if (now - s_sessions[i].attach_start_time > ATTACH_TIMEOUT_MS) {
                    int fd_to_close = s_sessions[i].ws_fd;
                    s_sessions[i].ws_fd = 0;
                    portEXIT_CRITICAL(&s_sessions_lock);
                    
                    httpd_sess_trigger_close(ws_server, fd_to_close);
                    
                    portENTER_CRITICAL(&s_sessions_lock);
                }
            }
        }
        portEXIT_CRITICAL(&s_sessions_lock);

        if (master_absent != last_master_status) {
            last_master_status = master_absent;
            char status_json[64];
            snprintf(status_json, sizeof(status_json), "{\"type\":\"status\",\"online\":%s}", master_absent ? "false" : "true");
            
            portENTER_CRITICAL(&s_sessions_lock);
            for (int i = 0; i < MAX_SESSIONS; i++) {
                if (s_sessions[i].ws_fd != 0 && s_sessions[i].phone_num[0] != '\0') {
                    int fd = s_sessions[i].ws_fd;
                    portEXIT_CRITICAL(&s_sessions_lock);
                    ws_send_json(fd, status_json);
                    portENTER_CRITICAL(&s_sessions_lock);
                }
            }
            portEXIT_CRITICAL(&s_sessions_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
