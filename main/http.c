
#include "http.h"

#include "lwip/api.h"

#include "esp_log.h"

#include <string.h>

static const char TAG[] = "httpd";

// static const char HTTP_STATUS_OK[] = "HTTP/1.0 200 OK\r\n";
// static const char HTTP_STATUS_FORBIDDEN[] = "HTTP/1.0 403 Forbidden\r\n";
// static const char HTTP_STATUS_NOTFOUND[] = "HTTP/1.0 404 File not found\r\n";
static const char HTTP_SERVER_AGENT[] = "Server: Watering Station\r\n";
static const char HTTP_CONTENT_TYPE[] = "Content-type: ";
// static const char HTTP_PLAIN_TEXT[] = "text/plain";
// static const char HTTP_JSON[] = "application/json";
static const char HTTP_CRLF[] = "\r\n";
static const char HTTP_HEADER_END[] = "\r\n\r\n";

static const struct {
    http_status_code_t code;
    const char *str;
} status_codes[] = {
    { HTTP_STATUS_OK, "HTTP/1.0 200 OK\r\n" },
    { HTTP_STATUS_BADREQUEST, "HTTP/1.0 403 Bad Request\r\n"},
    { HTTP_STATUS_FORBIDDEN, "HTTP/1.0 403 Forbidden\r\n" },
    { HTTP_STATUS_NOTFOUND, "HTTP/1.0 404 File not found\r\n" },
    { HTTP_STATUS_INTERNALERROR, "500 Internal Server Error"},
};

static const struct {
    const char *extension;
    const char *type;
} content_types[] = {
    { "txt", "text/plain" },
    { "html", "text/html" },
    { "js", "application/javascript" },
    { "json", "application/json" },
    { "png", "image/png" },
    { "css", "text/css" },
};

static const int content_types_count
    = sizeof(content_types) / sizeof(content_types[0]);

bool parse_http_request(char *buf, int len, http_request_t *request)
{
    const char *delim = HTTP_CRLF;
    const int delim_len = strlen(delim);
    char *pend = buf + len;
    char *pbuf = strstr(buf, delim);
    if (pbuf == NULL) return false;
    *pbuf = '\0';
    pbuf += delim_len;

    // parse method and path
    {
        int method_end = 0;
        int url_start = 0;
        int url_end = 0;
        sscanf(buf, "%*s%n %n%*s%n", &method_end, &url_start, &url_end);
        if (method_end >= url_start || url_start >= url_end) return false;
        request->path = buf + url_start;
        buf[method_end] = '\0';
        buf[url_end] = '\0';
        ESP_LOGI(TAG, "%s: %s", buf, request->path);
        if (strncmp(buf, "GET", method_end) == 0) request->method = HTTP_REQUEST_GET;
        else if (strncmp(buf, "PUT", method_end) == 0) request->method = HTTP_REQUEST_PUT;
        else return false;
    }

    const char *hdr_delim = HTTP_HEADER_END;
    const int hdr_delim_len = strlen(hdr_delim);
    pbuf = strstr(pbuf, hdr_delim);
    if (pbuf == NULL)
    pbuf += hdr_delim_len;
    request->body = pbuf;
    request->body_len = pend - pbuf;

    return true;
}

static const char * get_content_type(const char *ext)
{
    const char *ctype = content_types[0].type;
    for (int i = 0; i < content_types_count; ++i) {
        if (strcmp(content_types[i].extension, ext) == 0) {
            ctype = content_types[i].type;
            break;
        }
    }
    return ctype;
}

void http_server_send_header(struct netconn *conn, http_status_code_t status,
                             const char *content_type)
{
    const char *status_str = NULL;
    for (int i = 0; i < sizeof(status_codes) / sizeof(status_codes[0]); ++i)
        if (status_codes[i].code == status) {
            status_str = status_codes[i].str;
            break;
        }
    configASSERT(status_str != NULL);
    netconn_write(conn, status_str, strlen(status_str),
                  NETCONN_NOCOPY | NETCONN_MORE);
    NETCONN_WRITE_CONST(conn, HTTP_SERVER_AGENT);
    NETCONN_WRITE_CONST(conn, HTTP_CONTENT_TYPE);
    const char *ctype = get_content_type(content_type);
    netconn_write(conn, ctype, strlen(ctype), NETCONN_NOCOPY | NETCONN_MORE);
    NETCONN_WRITE_CONST(conn, HTTP_HEADER_END);
}

void http_server_send_file(struct netconn *conn, const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file) {
        ESP_LOGI(TAG, "file %s found\n", filename);

        const char *ext = strrchr(filename, '.');
        if (ext == NULL) ext = ".txt";
        ++ext;

        http_server_send_header(conn, 200, ext);

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
        ESP_LOGW(TAG, "file %s not found\n", filename);
        http_server_send_header(conn, HTTP_STATUS_NOTFOUND, "html");
        NETCONN_WRITE_CONST(conn, "<html><body><h2>");
        netconn_write(conn, filename, strlen(filename),
                      NETCONN_COPY | NETCONN_MORE);
        NETCONN_WRITE_CONST(conn, " not found</h2></body></html>\r\n");
    }
}
