#ifndef MY_HTTP
#define MY_HTTP
#include <esp_check.h>

esp_err_t start_webserver(const char*);

void start_async_req_workers();

#endif