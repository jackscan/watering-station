#include "apps/sntp/sntp.h"

#include "freertos/timers.h"
#include "freertos/FreeRTOS.h"

#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "nvs_flash.h"

#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"

#include "driver/gpio.h"
#include "driver/adc.h"

#include <assert.h>
#include <string.h>

#define ADC1_MOISTURE_CHANNEL (ADC1_GPIO35_CHANNEL) // channel 7
#define REF_GPIO  (GPIO_NUM_25)

static const char *TAG = "wstation";

static const char HTTP_STATUS_OK[] = "HTTP/1.0 200 OK\r\n";
static const char HTTP_STATUS_NOTFOUND[] = "HTTP/1.0 404 File not found\r\n";
static const char HTTP_SERVER_AGENT[] = "Server: Watering Station\r\n";
static const char HTTP_CONTENT_TYPE[] = "Content-type: ";
static const char HTTP_PLAIN_TEXT[] = "text/plain";
static const char HTTP_JSON[] = "application/json";
static const char HTTP_CRLF[] = "\r\n";
// static const char HTTP_BODY_NOTFOUND[] = "\r\n<html><body><h2>404: The requested file cannot be found.</h2></body></html>";

static const struct
{
    const char *extension;
    const char *type;
} content_types[] = {
    {"html", "text/html"},
    {"js", "application/javascript"},
    {"png", "image/png"},
    {"css", "text/css"},
};

static const int content_types_count = sizeof(content_types) / sizeof(content_types[0]);

#define NETCONN_WRITE_CONST(C, S) \
    netconn_write(C, S, sizeof(S) - 1, NETCONN_NOCOPY | NETCONN_MORE)

#define BACKLOG_DAYS 8

static struct wstation {
    int16_t mdata[24*BACKLOG_DAYS];
    int16_t wdata[BACKLOG_DAYS];
    int mcount;
    int wcount;
    int time;
} s_station;

static void set_led(bool value)
{
    gpio_set_level(CONFIG_LED_GPIO, value ? 1 : 0);
}

esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id)
    {
    case SYSTEM_EVENT_STA_START:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        ESP_LOGI(TAG, "got ip:%s\n",
                 ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_DISCONNECTED");
        ESP_ERROR_CHECK(esp_wifi_connect());
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void
http_server_send_file(struct netconn *conn, const char *filename)
{
    FILE* file = fopen(filename, "r");
    if (file) {
        printf("file %s found\n", filename);
        NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
        NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);

        // content type
        {
            NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
            const char *ctype = HTTP_PLAIN_TEXT;
            const char * ext = strrchr(filename, '.');
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
            netconn_write(conn, ctype, strlen(ctype), NETCONN_NOCOPY | NETCONN_MORE);
            NETCONN_WRITE_CONST(conn, HTTP_CRLF);
        }

        // end of header
        NETCONN_WRITE_CONST(conn, HTTP_CRLF);

        // content
        {
            char buf[256];
            int buflen = sizeof buf;
            int n;
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
        netconn_write(conn, filename, strlen(filename), NETCONN_COPY | NETCONN_MORE);
        NETCONN_WRITE_CONST(conn, " not found</h2></body></html>\r\n");
    }
}

static int read_moisture(void)
{
    gpio_set_level(REF_GPIO, 1);
    adc1_config_width(ADC_WIDTH_BIT_9);
    adc1_config_channel_atten(ADC1_MOISTURE_CHANNEL, ADC_ATTEN_11db);

    vTaskDelay(pdMS_TO_TICKS(1000));
    int v = 0;
    const int count = 16;
    for (int i = 0; i < count; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        v += 512 - adc1_get_raw(ADC1_MOISTURE_CHANNEL);
        // ESP_LOGI(TAG, "v: %d", v);
    }

    v /= count;

    adc_power_off();
    gpio_set_level(REF_GPIO, 0);

    // ESP_LOGI(TAG, "v: %d", v);
    return v;
}

static void
http_server_send_measurement(struct netconn *conn)
{
    int v = read_moisture();

    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_PLAIN_TEXT);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%d", v);
    netconn_write(conn, buf, n, NETCONN_COPY);
}

static void
http_server_send_time(struct netconn *conn)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[256];
    int n = strftime(buf, sizeof(buf), "%c", &timeinfo);
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

static void
http_server_send_ringbuf(struct netconn *conn, int16_t *buf, int len, int cur)
{
    char sbuf[16];
    int i = cur % len;
    bool first = true;
    do
    {
        int v = buf[i];
        if (v >= 0)
        {
            int n = snprintf(sbuf, sizeof(sbuf), first ? "%d" : ",%d", v);
            netconn_write(conn, sbuf, n, NETCONN_COPY | NETCONN_MORE);
            first = false;
        }
        i = (i + 1) % len;
    }
    while (i != cur);
}

static void
http_server_send_data(struct netconn *conn)
{
    NETCONN_WRITE_CONST(conn, HTTP_STATUS_OK);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    NETCONN_WRITE_CONST(conn, HTTP_JSON);
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);
    // end of header
    NETCONN_WRITE_CONST(conn, HTTP_CRLF);

    char buf[16];
    int n;
    NETCONN_WRITE_CONST(conn, "{\"moisture\":{\"time\":");
    n = snprintf(buf, sizeof(buf), "%d", s_station.time);
    netconn_write(conn, buf, n, NETCONN_COPY | NETCONN_MORE);
    NETCONN_WRITE_CONST(conn, ",\"data\":[");
    http_server_send_ringbuf(conn, s_station.mdata, BACKLOG_DAYS * 24, s_station.mcount);
    NETCONN_WRITE_CONST(conn, "]},\"water\":{\"time\":");
    n = snprintf(buf, sizeof(buf), "%d", CONFIG_WATERING_HOUR);
    netconn_write(conn, buf, n, NETCONN_COPY | NETCONN_MORE);
    NETCONN_WRITE_CONST(conn, ",\"data\":[");
    http_server_send_ringbuf(conn, s_station.wdata, BACKLOG_DAYS, s_station.wcount);
    netconn_write(conn, "]}}", 3, NETCONN_NOCOPY);
}

static void
http_server_netconn_serve(struct netconn *conn)
{
    struct netbuf *inbuf;
    char *buf;
    u16_t buflen;
    err_t err;

    /* Read the data from the port, blocking if nothing yet there.
       We assume the request (the part we care about) is in one netbuf */
    err = netconn_recv(conn, &inbuf);

    if (err == ERR_OK)
    {
        netbuf_data(inbuf, (void **)&buf, &buflen);

        char req[32];
        if (sscanf(buf, "GET %31s", req) == 1)
        {
            printf("req: %s\n", req);

            if (strcmp(req, "/") == 0) {
                http_server_send_file(conn, "/index.html");
            } else if (strcmp(req, "/measure") == 0) {
                http_server_send_measurement(conn);
            } else if (strcmp(req, "/time") == 0) {
                http_server_send_time(conn);
            } else if (strcmp(req, "/data") == 0) {
                http_server_send_data(conn);
            } else {
                http_server_send_file(conn, req);
            }

            // netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);

            // /* Send our HTML page */
            // netconn_write(conn, http_index_hml, sizeof(http_index_hml) - 1, NETCONN_NOCOPY);
        }
    }
    /* Close the connection (server closes in HTTP) */
    netconn_close(conn);

    /* Delete the buffer (netconn_recv gives us ownership,
       so we have to make sure to deallocate the buffer) */
    netbuf_delete(inbuf);
}

static void http_server(void *pvParameters)
{
    for (;;)
    {
        struct netconn *conn, *newconn;
        err_t err;
        conn = netconn_new(NETCONN_TCP);
        netconn_bind(conn, NULL, 80);
        netconn_listen(conn);
        do
        {
            err = netconn_accept(conn, &newconn);
            if (err == ERR_OK)
            {
                set_led(true);
                http_server_netconn_serve(newconn);
                netconn_delete(newconn);
                set_led(false);
            }
        } while (err == ERR_OK);
        ESP_LOGE(TAG, "error at netconn_accept: %d", err);
        netconn_close(conn);
        netconn_delete(conn);
    }
}

static TickType_t ticks_till_hour(void)
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    int diff = (60 - timeinfo.tm_min) * 60 + 60 - timeinfo.tm_sec;
    // int diff = 60 - timeinfo.tm_sec;
    return pdMS_TO_TICKS(diff * 1000);
}

static void moisture_check(TimerHandle_t xTimer)
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    time(&now);
    localtime_r(&now, &timeinfo);
    char buf[256];
    strftime(buf, sizeof(buf), "%c", &timeinfo);

    int v = read_moisture();
    // ESP_LOGI(TAG, "time: %s, moisture: %d", buf, v);

    s_station.mdata[s_station.mcount % (24 * BACKLOG_DAYS)] = (int16_t)v;

    if (s_station.mcount > 0)
    {
        int hour = (timeinfo.tm_hour * 60 + timeinfo.tm_min + 30) / 60;
        if (hour == CONFIG_WATERING_HOUR)
        {
            // ESP_LOGI(TAG, "watering");
            s_station.wdata[s_station.wcount % (BACKLOG_DAYS)] = 0;
            ++s_station.wcount;
        }
    }
    s_station.time = timeinfo.tm_hour;
    ++s_station.mcount;

    if (xTimerChangePeriod(xTimer, ticks_till_hour(), 0) != pdPASS)
        assert(false);
}

// static void init_moisture_check(TimerHandle_t xTimer)
// {
//     time_t now = 0;
//     struct tm timeinfo = { 0 };
//     time(&now);
//     localtime_r(&now, &timeinfo);
//     char buf[256];
//     int n = strftime(buf, sizeof(buf), "%c", &timeinfo);

//     int v = read_moisture();
//     ESP_LOGI(TAG, "init time: %s, moisture: %d", buf, v);

//     s_station.mdata[0] = (int16_t)v;
//     s_station.cursor = 1;


// }

static void wifi_init(void)
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_LOGI(TAG, "esp_wifi_stop().");
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK)
        ESP_LOGI(TAG, "wifi not stopped: %#x", err);
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
    esp_pm_config_esp32_t pm_config = {
        .max_cpu_freq = max_freq,
        .min_cpu_freq = RTC_CPU_FREQ_XTAL};
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

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%d)", ret);
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("storage", &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information %d", ret);
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
}

static void setup_data(void)
{
    for (int i = 0; i < BACKLOG_DAYS; ++i)
    {
        for (int j = 0; j < 24; ++j)
            s_station.mdata[i*24 + j] = -1;
        s_station.wdata[i] = -1;
    }
    s_station.mcount = 0;
    s_station.wcount = 0;
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
    time_t now;
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
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES)
    {
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
    setup_data();

    wait_for_ntp();
    xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);

    TimerHandle_t timer = xTimerCreate("Init Moisture Check", ticks_till_hour(), pdFALSE, NULL, moisture_check);
    assert(timer);
    xTimerStart(timer, 0);
}
