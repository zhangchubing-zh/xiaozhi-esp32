#include "asp.h"
size_t receive(void *contents, size_t size, size_t count, void *user_data)
{
    Buffer *b = user_data;
    if (count && size > SIZE_MAX / count) return 0;
    size_t bytes = size * count;
    if (bytes > RESPONSE_MAX || b->size > RESPONSE_MAX - bytes) return 0;
    size_t required = b->size + bytes + 1;
    if (required > b->capacity) {
        size_t capacity = b->capacity;
        while (capacity < required) {
            if (capacity > (RESPONSE_MAX + 1U) / 2U) { capacity = RESPONSE_MAX + 1U; break; }
            capacity *= 2U;
        }
        char *data = realloc(b->data, capacity);
        if (data == NULL) return 0;
        b->data = data;
        b->capacity = capacity;
    }
    memcpy(b->data + b->size, contents, bytes);
    b->size += bytes;
    b->data[b->size] = '\0';
    return bytes;
}

void json_string(const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    putchar('"');
    for (; *p; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default: if (*p < 0x20U) printf("\\u%04x", *p); else putchar(*p);
        }
    }
    putchar('"');
}

void apply_test_tls(CURL *curl)
{
    /* 测试期：设备自签 CA + CN 与 IP 不匹配，关闭校验以连通。
     * 正式部署须恢复 VERIFYPEER=1 / VERIFYHOST=2 并锚定设备 CA。 */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
}

/* 带上 x-csrf-token 做一次 GET。cookie 由 handle 自动带。 */
int do_get(CURL *curl, const char *token, const char *url, Buffer *resp)
{
    int exit_code = HTTP_ERROR;
    struct curl_slist *headers = NULL;
    char auth[TOKEN_MAX + 32];
    long status = 0;
    CURLcode rc;

    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    if (!headers) goto done;
    if (token) {
        int n = snprintf(auth, sizeof auth, "x-csrf-token: %s", token);
        if (n <= 0 || n >= (int)sizeof auth) goto done;
        headers = curl_slist_append(headers, auth);
        if (!headers) goto done;
    }
    resp->data[0] = '\0';
    resp->size = 0;
    resp->http_status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    apply_test_tls(curl);
    /* 复用 handle: cookie 引擎已在 do_login 启用，Set-Cookie 自动随请求带上 */
    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        exit_code = NETWORK_ERROR;
        goto done;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp->http_status = status;
    exit_code = (status >= 200 && status < 300) ? OK : HTTP_ERROR;
done:
    curl_slist_free_all(headers);
    return exit_code;
}

/* POST application/x-www-form-urlencoded，body 已构造好。 */
int do_post(CURL *curl, const char *token, const char *url,
                   const char *body, const char *stage, Buffer *resp)
{
    (void)stage;
    int exit_code = HTTP_ERROR;
    struct curl_slist *headers = NULL;
    char auth[TOKEN_MAX + 32];
    long status = 0;
    CURLcode rc;

    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    if (!headers) goto done;
    headers = curl_slist_append(headers,
        "Content-Type: application/x-www-form-urlencoded");
    if (!headers) goto done;
    if (token) {
        int n = snprintf(auth, sizeof auth, "x-csrf-token: %s", token);
        if (n <= 0 || n >= (int)sizeof auth) goto done;
        headers = curl_slist_append(headers, auth);
        if (!headers) goto done;
    }
    resp->data[0] = '\0';
    resp->size = 0;
    resp->http_status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    apply_test_tls(curl);
    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        exit_code = NETWORK_ERROR;
        goto done;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp->http_status = status;
    exit_code = (status >= 200 && status < 300) ? OK : HTTP_ERROR;
done:
    curl_slist_free_all(headers);
    return exit_code;
}

int save_response(const char *path, const Buffer *resp)
{
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t written = fwrite(resp->data, 1U, resp->size, file);
    int close_rc = fclose(file);
    if (written != resp->size || close_rc != 0) return -1;
    return 0;
}
