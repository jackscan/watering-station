#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>

typedef enum {
    HTTP_REQUEST_GET,
    HTTP_REQUEST_PUT,
} http_request_method_t;

typedef enum {
    HTTP_STATUS_OK = 200,
    HTTP_STATUS_BADREQUEST = 400,
    HTTP_STATUS_FORBIDDEN = 403,
    HTTP_STATUS_NOTFOUND = 404,
    HTTP_STATUS_INTERNALERROR = 500,
} http_status_code_t;

typedef struct {
    http_request_method_t method;
    const char *path;
    const char *body;
    int body_len;
} http_request_t;

struct netbuf;
struct netconn;

#define NETCONN_WRITE_CONST(C, S)                                              \
    netconn_write(C, S, sizeof(S) - 1, NETCONN_NOCOPY | NETCONN_MORE)

bool parse_http_request(char *buf, int len, http_request_t *request);
void http_server_send_header(struct netconn *conn, http_status_code_t status,
                             const char *content_type);
void http_server_send_file(struct netconn *conn, const char *filename);

#endif
