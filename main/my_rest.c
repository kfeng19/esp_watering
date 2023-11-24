#include "my_rest.h"
#include "esp_tls_crypto.h"
#include "protocol_examples_utils.h"
#include "var.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_vfs.h"
#include <fcntl.h>
#include "cJSON.h"
#include "my_led.h"
#include "moisture.h"

/* A simple example that demonstrates how to create GET and POST
 * handlers for the web server.
 */

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)

static const char *REST_TAG = "esp-rest";
#define REST_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                 \
    {                                                                                  \
        if (!(a))                                                                      \
        {                                                                              \
            ESP_LOGE(REST_TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                             \
        }                                                                              \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)
typedef struct rest_server_context {
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath)
{
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

#define ASYNC_WORKER_TASK_PRIORITY      5
#define ASYNC_WORKER_TASK_STACK_SIZE    2048

// Async reqeusts are queued here while they wait to
// be processed by the workers
static QueueHandle_t async_req_queue;

// Track the number of free workers at any given time
static SemaphoreHandle_t worker_ready_count;

// Each worker has its own thread
static TaskHandle_t worker_handles[CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS];

typedef esp_err_t (*httpd_req_handler_t)(httpd_req_t *req);

typedef struct {
    httpd_req_t* req;
    httpd_req_handler_t handler;
} httpd_async_req_t;


static bool is_on_async_worker_thread(void)
{
    // is our handle one of the known async handles?
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    for (int i = 0; i < CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS; i++) {
        if (worker_handles[i] == handle) {
            return true;
        }
    }
    return false;
}


// Submit an HTTP req to the async worker queue
static esp_err_t submit_async_req(httpd_req_t *req, httpd_req_handler_t handler)
{
    // must create a copy of the request that we own
    httpd_req_t* copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        return err;
    }

    httpd_async_req_t async_req = {
        .req = copy,
        .handler = handler,
    };

    // How should we handle resource exhaustion?
    // In this example, we immediately respond with an
    // http error if no workers are available.
    int ticks = 0;

    // counting semaphore: if success, we know 1 or
    // more asyncReqTaskWorkers are available.
    if (xSemaphoreTake(worker_ready_count, ticks) == false) {
        ESP_LOGE(TAG, "No workers are available");
        httpd_req_async_handler_complete(copy); // cleanup
        return ESP_FAIL;
    }

    // Since worker_ready_count > 0 the queue should already have space.
    // But lets wait up to 100ms just to be safe.
    if (xQueueSend(async_req_queue, &async_req, pdMS_TO_TICKS(100)) == false) {
        ESP_LOGE(TAG, "worker queue is full");
        httpd_req_async_handler_complete(copy); // cleanup
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void async_req_worker_task(void *p)
{
    ESP_LOGI(TAG, "starting async req task worker");

    while (true) {

        // counting semaphore - this signals that a worker
        // is ready to accept work
        xSemaphoreGive(worker_ready_count);

        // wait for a request
        httpd_async_req_t async_req;
        if (xQueueReceive(async_req_queue, &async_req, portMAX_DELAY)) {

            ESP_LOGI(TAG, "invoking %s", async_req.req->uri);

            // call the handler
            async_req.handler(async_req.req);

            // Inform the server that it can purge the socket used for
            // this request, if needed.
            if (httpd_req_async_handler_complete(async_req.req) != ESP_OK) {
                ESP_LOGE(TAG, "failed to complete async req");
            }
        }
    }

    ESP_LOGW(TAG, "worker stopped");
    vTaskDelete(NULL);
}

void start_async_req_workers(void)
{

    // counting semaphore keeps track of available workers
    worker_ready_count = xSemaphoreCreateCounting(
        CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS,  // Max Count
        0); // Initial Count
    if (worker_ready_count == NULL) {
        ESP_LOGE(TAG, "Failed to create workers counting Semaphore");
        return;
    }

    // create queue
    async_req_queue = xQueueCreate(1, sizeof(httpd_async_req_t));
    if (async_req_queue == NULL){
        ESP_LOGE(TAG, "Failed to create async_req_queue");
        vSemaphoreDelete(worker_ready_count);
        return;
    }

    // start worker tasks
    for (int i = 0; i < CONFIG_EXAMPLE_MAX_ASYNC_REQUESTS; i++) {

        bool success = xTaskCreate(async_req_worker_task, "async_req_worker",
                                    ASYNC_WORKER_TASK_STACK_SIZE, // stack size
                                    (void *)0, // argument
                                    ASYNC_WORKER_TASK_PRIORITY, // priority
                                    &worker_handles[i]);

        if (!success) {
            ESP_LOGE(TAG, "Failed to start asyncReqWorker");
            continue;
        }
    }
}



/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];

    rest_server_context_t *rest_context = (rest_server_context_t *)req->user_ctx;
    strlcpy(filepath, rest_context->base_path, sizeof(filepath));
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", sizeof(filepath));
    } else {
        strlcat(filepath, req->uri, sizeof(filepath));
    }
    struct stat st;
    if (stat(filepath, &st) == 0){
        ESP_LOGI(REST_TAG, "%s exists, size: %ld", filepath, st.st_size);
    } else {
        ESP_LOGE(REST_TAG, "%s doesn't exist.", filepath);
    }
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        ESP_LOGE(REST_TAG, "Failed to open file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char *chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(REST_TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(REST_TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGI(REST_TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}



/* Flag variable to indicate whether sensor measurement should continue */
bool continue_measurement = false;


led_strip_handle_t my_led;
adc_oneshot_unit_handle_t adc_handle;
/* Handler for POST requests, need to be asynchronous */
esp_err_t measure_start_handler(httpd_req_t *req)
{
        // This handler is first invoked on the httpd thread.
    // In order to free the httpd thread to handle other requests,
    // we must resubmit our request to be handled on an async worker thread.
    if (continue_measurement) {
        ESP_LOGE(REST_TAG, "Measurement already started!");
        return ESP_FAIL;
    }
    if (is_on_async_worker_thread() == false) {
        // submit
        if (submit_async_req(req, measure_start_handler) == ESP_OK) {
            return ESP_OK;
        } else {
            httpd_resp_set_status(req, "503 Busy");
            httpd_resp_sendstr(req, "<div> no workers available. server busy.</div>");
            return ESP_OK;
        }
    }
    // Failed in receiving request body so far
        /* Respond with success */
        continue_measurement = true;
        double level = 0;
        httpd_resp_sendstr(req, "Triggering measurement.");
        ESP_LOGI(REST_TAG, "Starting measurement.");
        /* Trigger sensor measurement indefinitely */
        while (continue_measurement) {
            level = measure_moisture(&adc_handle, ADC_CHANNEL_4);
            inv_jet(&my_led, level, 0, 100, 0.3);
            vTaskDelay(CONFIG_MEASURE_PERIOD / portTICK_PERIOD_MS);
        }
    return ESP_OK;
}

esp_err_t measure_stop_handler(httpd_req_t *req){
    continue_measurement = false;
    led_strip_clear(my_led);
    ESP_LOGI(REST_TAG, "Measurement stopped.");
    return ESP_OK;
}

esp_err_t start_webserver(const char *base_path)
{
    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t *rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_LOGI(REST_TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

#if CONFIG_IDF_TARGET_LINUX
    // Setting port as 8001 when building for Linux. Port 80 can be used only by a priviliged user in linux.
    // So when a unpriviliged user tries to run the application, it throws bind error and the server is not started.
    // Port 8001 can be used by an unpriviliged user as well. So the application will not throw bind error and the
    // server will be started.
    config.server_port = 8001;
#endif // !CONFIG_IDF_TARGET_LINUX
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);

    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");


    /* URI handler for measurement */
    httpd_uri_t measure_uri = {
        .uri       = "/api/measure/start",
        .method    = HTTP_POST,
        .handler   = measure_start_handler,
        .user_ctx  = rest_context
    };
    httpd_register_uri_handler(server, &measure_uri);

    /* URI handler for measurement */
    httpd_uri_t measure_stop_uri = {
        .uri       = "/api/measure/stop",
        .method    = HTTP_POST,
        .handler   = measure_stop_handler,
        .user_ctx  = rest_context
    };
    httpd_register_uri_handler(server, &measure_stop_uri);


    /* URI handler for getting web server files */
    httpd_uri_t common_get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    // config led and adc
    configure_led(&my_led);
    adc_handle = config_adc(ADC_CHANNEL_4);

    // return
    return ESP_OK;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}