#include "apps/sntp/sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"

#include "nvs_flash.h"

#include "lwip/api.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include "lwip/err.h"

#include "driver/adc.h"
#include "driver/gpio.h"
#include "driver/timer.h"

#include <assert.h>
#include <string.h>

#define ADC1_MOISTURE_CHANNEL (ADC1_GPIO35_CHANNEL) // channel 7
#define REF_GPIO (GPIO_NUM_25)
#define W_GPIO (GPIO_NUM_27)
#define B_GPIO (GPIO_NUM_0)

static const char WATERING_CONFIG_FILE[] = "/config";
static const char TAG[] = "wstation";

static const char HTTP_STATUS_OK[] = "HTTP/1.0 200 OK\r\n";
static const char HTTP_STATUS_NOTFOUND[] = "HTTP/1.0 404 File not found\r\n";
static const char HTTP_SERVER_AGENT[] = "Server: Watering Station\r\n";
static const char HTTP_CONTENT_TYPE[] = "Content-type: ";
static const char HTTP_PLAIN_TEXT[] = "text/plain";
static const char HTTP_JSON[] = "application/json";
static const char HTTP_CRLF[] = "\r\n";
// static const char HTTP_BODY_NOTFOUND[] = "\r\n<html><body><h2>404: The
// requested file cannot be found.</h2></body></html>";

static const struct {
    const char *extension;
    const char *type;
} content_types[] = {
    { "html", "text/html" },
    { "js", "application/javascript" },
    { "png", "image/png" },
    { "css", "text/css" },
};

static const int content_types_count
    = sizeof(content_types) / sizeof(content_types[0]);

#define NETCONN_WRITE_CONST(C, S)                                              \
    netconn_write(C, S, sizeof(S) - 1, NETCONN_NOCOPY | NETCONN_MORE)

#define BACKLOG_DAYS 8

typedef struct {
    int watering_hour;
    int min_water;
    int max_water;
    int min_level;
    int dst_level;
} config_t;

static struct wstation {
    xQueueHandle      evqueue;
    int16_t           mdata[24 * BACKLOG_DAYS];
    int16_t           wdata[BACKLOG_DAYS];
    int               mcount;
    int               wcount;
    int               time;
    StaticSemaphore_t sensorSem;
    SemaphoreHandle_t sensorSemHandle;
    StaticSemaphore_t dataSem;
    SemaphoreHandle_t dataSemHandle;
    config_t          config;
    ip4_addr_t        whitelist_ipaddr;
    ip4_addr_t        whitelist_netmask;
    ip4_addr_t        ipaddr;
    ip4_addr_t        netmask;
} s_station = {
    .config.watering_hour = CONFIG_WATERING_HOUR,
};

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(s_station.evqueue, &gpio_num, NULL);
}

static void set_led(bool value)
{
    gpio_set_level(CONFIG_LED_GPIO, value ? 1 : 0);
}

static void set_watering(bool value)
{
    gpio_set_level(W_GPIO, value ? 1 : 0);
}

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        s_station.ipaddr = event->event_info.got_ip.ip_info.ip;
        s_station.netmask = event->event_info.got_ip.ip_info.netmask;
        ESP_LOGI(TAG, "ip:%s\n", ip4addr_ntoa(&s_station.ipaddr));
        ESP_LOGI(TAG, "netmask:%s\n", ip4addr_ntoa(&s_station.netmask));
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    default: break;
    }
    return ESP_OK;
}

static bool read_config(void)
{
    bool  ret = false;
    FILE *file = fopen("/config", "r");
    if (file) {
        config_t config;
        if (fscanf(file, "%d,%d,%d,%d,%d", &config.watering_hour,
                   &config.min_water, &config.max_water, &config.dst_level,
                   &config.min_level)
            == 5) {
            s_station.config = config;
            ret = true;
        }

        fclose(file);
    }
    return ret;
}

// static bool save_config(void)
// {
//     bool ret = false;
//     FILE *file = fopen(WATERING_CONFIG_FILE, "w");
//     if (file)
//     {
//         if (fprintf(file, "%d,%d,%d,%d,%d", s_station.config.watering_hour,
//         s_station.config.min_water,
//                    s_station.config.max_water, s_station.config.dst_level,
//                    s_station.config.min_level) > 0)
//         {
//             ret = true;
//         }
//         fclose(file);
//     }
//     return ret;
// }

static void http_server_send_file(struct netconn *conn, const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file) {
        printf("file %s found\n", filename);
        NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
        NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);

        // content type
        {
            NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
            const char *ctype = HTTP_PLAIN_TEXT;
            const char *ext = strrchr(filename, '.');
            if (ext) {
                // move pointer beyond '.'
                ++ext;
                for (int i = 0; i < content_types_count; ++i) {
                    if (strcmp(content_types[i].extension, ext) == 0) {
                        ctype = content_types[i].type;
                        break;
                    }
                }
            }
            netconn_write(conn, ctype, strlen(ctype),
                          NETCONN_NOCOPY | NETCONN_MORE);
            NETCONN_WRITE_CONST(conn, HTTP_CRLF);
        }

        // end of header
        NETCONN_WRITE_CONST(conn, HTTP_CRLF);

        // content
        {
            char buf[256];
            int  buflen = sizeof buf;
            int  n;
            do {
                n = fread(buf, 1, buflen, file);
                if (n > 0) {
                    netconn_write(conn, buf, n, NETCONN_COPY | NETCONN_MORE);
                }
            } while (n == buflen);
        }

        fclose(file);
    } else {
        printf("file %s not found\n", filename);
        NETCONN_WRITE_CONST(conn, HTTP_STATUS_NOTFOUND);
        NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
        NETCONN_WRITE_CONST(conn, HTTP_CRLF);
        NETCONN_WRITE_CONST(conn, "<html><body><h2>");
        netconn_write(conn, filename, strlen(filename),
                      NETCONN_COPY | NETCONN_MORE);
        NETCONN_WRITE_CONST(conn, " not found</h2></body></html>\r\n");
    }
}

static int read_moisture(TickType_t maxWait)
{
    int v = -1;
    if (xSemaphoreTake(s_station.sensorSemHandle, maxWait) == pdTRUE) {
        gpio_set_level(REF_GPIO, 1);
        adc1_config_width(ADC_WIDTH_BIT_9);
        adc1_config_channel_atten(ADC1_MOISTURE_CHANNEL, ADC_ATTEN_11db);

        vTaskDelay(pdMS_TO_TICKS(1000));
        const int count = 16;
        for (int i = 0; i < count; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
            v += 512 - adc1_get_raw(ADC1_MOISTURE_CHANNEL);
            // ESP_LOGI(TAG, "v: %d", v);
        }

        v /= count / 2;

        adc_power_off();
        gpio_set_level(REF_GPIO, 0);
        xSemaphoreGive(s_station.sensorSemHandle);
    }

    // ESP_LOGI(TAG, "v: %d", v);
    return v;
}

static void http_server_send_measurement(struct netconn *conn)
{
    int v = read_moisture(pdMS_TO_TICKS(100));

    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_PLAIN_TEXT);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    char buf[16];
    int  n = snprintf(buf, sizeof(buf), "%d", v);
    netconn_write(conn, buf, n, NETCONN_COPY);
}

static void http_server_send_time(struct netconn *conn)
{
    time_t    now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[256];
    int  n = strftime(buf, sizeof(buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "time: %s", buf);

    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_PLAIN_TEXT);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    netconn_write(conn, buf, n, NETCONN_COPY);
}

static void http_server_send_ringbuf(struct netconn *conn, int16_t *buf,
                                     int len, int cur)
{
    char sbuf[16];
    int  i = cur % len;
    bool first = true;
    do {
        int v = buf[i];
        if (v >= 0 || !first) {
            int n = snprintf(sbuf, sizeof(sbuf), first ? "%d" : ",%d", v);
            netconn_write(conn, sbuf, n, NETCONN_COPY | NETCONN_MORE);
            first = false;
        }
        i = (i + 1) % len;
    } while (i != cur);
}

static void http_server_send_data(struct netconn *conn)
{
    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_JSON);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    if (xSemaphoreTake(s_station.dataSemHandle, pdMS_TO_TICKS(100)) == pdTRUE) {
        char buf[16];
        int  n;
        NETCONN_WRITE_CONST(conn, "{\"moisture\":{\"time\":");
        n = snprintf(buf, sizeof(buf), "%d", s_station.time);
        netconn_write(conn, buf, n, NETCONN_COPY | NETCONN_MORE);
        NETCONN_WRITE_CONST(conn, ",\"data\":[");
        http_server_send_ringbuf(conn, s_station.mdata, BACKLOG_DAYS * 24,
                                 s_station.mcount);
        NETCONN_WRITE_CONST(conn, "]},\"water\":{\"time\":");
        n = snprintf(buf, sizeof(buf), "%d", CONFIG_WATERING_HOUR);
        netconn_write(conn, buf, n, NETCONN_COPY | NETCONN_MORE);
        NETCONN_WRITE_CONST(conn, ",\"data\":[");
        http_server_send_ringbuf(conn, s_station.wdata, BACKLOG_DAYS,
                                 s_station.wcount);
        netconn_write(conn, "]}}", 3, NETCONN_NOCOPY);

        xSemaphoreGive(s_station.dataSemHandle);
    } else {
        NETCONN_WRITE_CONST(conn, "{\"busy\":true}");
    }
}

static void http_server_send_led(struct netconn *conn)
{
    int  l = gpio_get_level(CONFIG_LED_GPIO);
    char buf[3];
    int  n = snprintf(buf, sizeof(buf), "%d", l);

    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_PLAIN_TEXT);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    netconn_write(conn, buf, n, NETCONN_COPY);
}

static void http_server_send_addr(struct netconn *conn, ip_addr_t *fromip)
{
    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_PLAIN_TEXT);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    const char *str = ipaddr_ntoa(fromip);

    netconn_write(conn, str, strlen(str), NETCONN_COPY);
}

static void http_server_netconn_serve(struct netconn *conn)
{
    struct netbuf *inbuf;
    char *         buf;
    u16_t          buflen;
    err_t          err = netconn_recv(conn, &inbuf);

    if (err == ERR_OK) {
        netbuf_data(inbuf, (void **)&buf, &buflen);

        char req[32];
        if (sscanf(buf, "GET %31s", req) == 1) {
            ESP_LOGI(TAG, "get: %s", req);

            if (strcmp(req, "/") == 0) {
                http_server_send_file(conn, "/index.html");
            } else if (strcmp(req, "/measure") == 0) {
                http_server_send_measurement(conn);
            } else if (strcmp(req, "/time") == 0) {
                http_server_send_time(conn);
            } else if (strcmp(req, "/data") == 0) {
                http_server_send_data(conn);
            } else if (strcmp(req, "/led") == 0) {
                http_server_send_led(conn);
            } else if (strcmp(req, "/addr") == 0) {
                ip_addr_t *fromip = netbuf_fromaddr(inbuf);
                http_server_send_addr(conn, fromip);
            } else {
                http_server_send_file(conn, req);
            }
        } else if (sscanf(buf, "PUT %31s", req) == 1) {
            ip_addr_t *fromip = netbuf_fromaddr(inbuf);
            ESP_LOGI(TAG, "put: %s from %s", req, ipaddr_ntoa(fromip));
            if (IP_GET_TYPE(fromip) == IPADDR_TYPE_V4) {
                // TODO:
                // ip4_addr_t *ip = ip_2_ip4(fromip);
                // if (ip4_addr_netcmp(ip, s_station.ipaddr, s_station.netmask))
                // {

                // }
            }
        }
    }

    netconn_close(conn);
    netbuf_delete(inbuf);
}

static void http_server(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    for (;;) {
        struct netconn *conn, *newconn;
        err_t           err;
        conn = netconn_new(NETCONN_TCP);
        netconn_bind(conn, NULL, 80);
        netconn_listen(conn);
        // netconn_set_sendtimeout(conn, 2000);
        netconn_set_recvtimeout(conn, 2000);

        do {
            while ((err = netconn_accept(conn, &newconn)) == ERR_OK) {
                esp_task_wdt_reset();
                netconn_set_sendtimeout(newconn, 2000);
                netconn_set_recvtimeout(newconn, 2000);

                set_led(true);
                http_server_netconn_serve(newconn);
                netconn_delete(newconn);
                set_led(false);
            }
            esp_task_wdt_reset();
        } while (err == ERR_TIMEOUT);

        ESP_LOGE(TAG, "error at netconn_accept: %s", lwip_strerr(err));
        netconn_close(conn);
        netconn_delete(conn);
    }
}

#define ON_ESP_ERROR(E, A)                                                     \
    do {                                                                       \
        esp_err_t _err = (E);                                                  \
        if (_err != ESP_OK) {                                                  \
            ESP_LOGE(TAG, #E " failed at %d", __LINE__);                       \
            A;                                                                 \
        }                                                                      \
    } while (0)

static void watering_task(void *pvParameters)
{
    ON_ESP_ERROR(esp_task_wdt_add(NULL), abort());
    TickType_t     timeout = pdMS_TO_TICKS(CONFIG_TASK_WDT_TIMEOUT_S * 500);
    bool           state = false;
    timer_config_t tconfig = {
        .alarm_en = false,
        .counter_en = false,
        .intr_type = TIMER_INTR_LEVEL,
        .counter_dir = TIMER_COUNT_UP,
        .auto_reload = true,
        .divider = 16,
    };

    // xTaskGetTickCount

    ON_ESP_ERROR(timer_init(TIMER_GROUP_0, TIMER_0, &tconfig), abort());
    for (;;) {
        uint32_t gpio_num;
        if (xQueueReceive(s_station.evqueue, &gpio_num, timeout)) {
            bool s = gpio_get_level(B_GPIO) == 0;
            if (s != state) {
                if (s) {
                    ON_ESP_ERROR(
                        timer_set_counter_value(TIMER_GROUP_0, TIMER_0, 0),
                        break);
                    ON_ESP_ERROR(timer_start(TIMER_GROUP_0, TIMER_0), break);
                    set_led(true);
                    set_watering(true);
                } else {
                    set_watering(false);
                    set_led(false);
                    ON_ESP_ERROR(timer_pause(TIMER_GROUP_0, TIMER_0), break);
                    double t = 0;
                    ON_ESP_ERROR(
                        timer_get_counter_time_sec(TIMER_GROUP_0, TIMER_0, &t),
                        break);
                    ESP_LOGI(TAG, "manual watering: %fs", t);
                    esp_task_wdt_reset();
                    if (xSemaphoreTake(s_station.dataSemHandle, timeout)
                        == pdTRUE) {
                        s_station.wdata[(s_station.wcount + BACKLOG_DAYS - 1)
                                        % (BACKLOG_DAYS)]
                            = (int16_t)(t * 1000);
                        xSemaphoreGive(s_station.dataSemHandle);
                    }
                }
                state = s;
            }
        }
        esp_task_wdt_reset();
    }

    set_led(false);
    set_watering(false);
    abort();
}

static TickType_t ticks_till_hour(void)
{
    time_t    now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    int diff = (60 - timeinfo.tm_min) * 60 - timeinfo.tm_sec;
    if (diff <= 0) diff = 60 * 60;
    return pdMS_TO_TICKS(diff * 1000);
}

static void moisture_check(TimerHandle_t xTimer)
{
    time_t    now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[256];
    strftime(buf, sizeof(buf), "%c", &timeinfo);

    int v = read_moisture(pdMS_TO_TICKS(2000));
    // ESP_LOGI(TAG, "time: %s, moisture: %d", buf, v);

    int  hour = (timeinfo.tm_hour * 60 + timeinfo.tm_min + 30) / 60;
    bool watering = hour == s_station.config.watering_hour;
    // int  water = 0;

    if (watering) {
        // ESP_LOGI(TAG, "watering");
        // TODO: calculate amount of water
        // TODO: do watering
    }

    if (xSemaphoreTake(s_station.dataSemHandle, pdMS_TO_TICKS(5000))
        == pdTRUE) {

        if (watering) {
            // TODO:
            // s_station.wdata[s_station.wcount % (BACKLOG_DAYS)] = water;
            ++s_station.wcount;
        }

        s_station.mdata[s_station.mcount % (24 * BACKLOG_DAYS)] = (int16_t)v;
        s_station.time = timeinfo.tm_hour;
        ++s_station.mcount;
        xSemaphoreGive(s_station.dataSemHandle);
    }

    if (xTimerChangePeriod(xTimer, ticks_till_hour(), 0) != pdPASS)
        assert(false);
}

static void wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = CONFIG_WIFI_SSID,
                .password = CONFIG_WIFI_PASSWORD,
            },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "esp_wifi_stop().");
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) ESP_LOGI(TAG, "wifi not stopped: %#x", err);
    ESP_LOGI(TAG, "esp_wifi_start().");
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "esp_wifi_set_ps().");
    // esp_wifi_set_ps(WIFI_PS_MODEM);
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void setup_ps(void)
{
#if CONFIG_PM_ENABLE
    rtc_cpu_freq_t max_freq;
    rtc_clk_cpu_freq_from_mhz(CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ, &max_freq);
    esp_pm_config_esp32_t pm_config
        = { .max_cpu_freq = max_freq, .min_cpu_freq = RTC_CPU_FREQ_XTAL };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
#endif
}

void setup_spiffs(void)
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%d)", ret);
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information %d", ret);
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

static void setup_data(void)
{
    for (int i = 0; i < BACKLOG_DAYS; ++i) {
        for (int j = 0; j < 24; ++j)
            s_station.mdata[i * 24 + j] = -1;
        s_station.wdata[i] = -1;
    }
    s_station.mcount = 0;
    s_station.wcount = 0;

    s_station.sensorSemHandle
        = xSemaphoreCreateBinaryStatic(&s_station.sensorSem);
    configASSERT(s_station.sensorSemHandle);
    s_station.dataSemHandle = xSemaphoreCreateBinaryStatic(&s_station.dataSem);
    configASSERT(s_station.dataSemHandle);

    s_station.evqueue = xQueueCreate(10, sizeof(uint32_t));
    configASSERT(s_station.evqueue);
}

static void setup_sensor(void)
{
    gpio_pad_select_gpio(REF_GPIO);
    gpio_set_direction(REF_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(REF_GPIO, 0);
}

static void setup_time(void)
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // set timezone
    setenv("TZ", CONFIG_TZ, 1);
    tzset();
}

static bool time_is_valid(void)
{
    time_t    now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    // Is time set? If not, tm_year will be (1970 - 1900).
    return timeinfo.tm_year > (2016 - 1900);
}

static void setup_led(void)
{
    gpio_pad_select_gpio(CONFIG_LED_GPIO);
    gpio_set_direction(CONFIG_LED_GPIO, GPIO_MODE_OUTPUT);
    set_led(false);
}

static void setup_watering_gpio(void)
{
    gpio_pad_select_gpio(W_GPIO);
    gpio_set_direction(W_GPIO, GPIO_MODE_OUTPUT);
    set_watering(false);
}

static void setup_button(void)
{
    gpio_pad_select_gpio(B_GPIO);
    gpio_set_direction(B_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(B_GPIO, GPIO_FLOATING);
    gpio_set_intr_type(B_GPIO, GPIO_INTR_ANYEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(B_GPIO, gpio_isr_handler, NULL);
    gpio_intr_enable(B_GPIO);
}

static void wait_for_ntp(void)
{
    set_led(true);
    // wait for time
    while (!time_is_valid()) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    set_led(false);
}

void app_main(void)
{
    setup_watering_gpio();
    setup_data();

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "Erasing nvs");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    setup_led();
    setup_spiffs();
    wifi_init();
    setup_ps();
    setup_time();
    setup_sensor();
    read_config();

    wait_for_ntp();
    xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);

    xTaskCreate(&watering_task, "watering task", 2048, NULL, configMAX_PRIORITIES - 2, NULL);
    setup_button();

    TimerHandle_t timer = xTimerCreate("Init Moisture Check", ticks_till_hour(),
                                       pdFALSE, NULL, moisture_check);
    assert(timer);
    xTimerStart(timer, 0);

    xSemaphoreGive(s_station.dataSemHandle);
    xSemaphoreGive(s_station.sensorSemHandle);
}
