#include "asp.h"
/* 从登录响应提取 token。受限解析："token":"<alnum>" */
static char *extract_token(const char *body)
{
    const char *key = "\"token\"";
    const char *p = body ? strstr(body, key) : NULL;
    if (!p) return NULL;
    p += strlen(key);
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != ':') return NULL;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '"') return NULL;
    ++p;
    const char *start = p;
    while (*p && *p != '"' && *p != '\\') ++p;
    if (*p != '"') return NULL;
    size_t len = (size_t)(p - start);
    if (len == 0 || len >= TOKEN_MAX) return NULL;
    char *tok = malloc(len + 1U);
    if (!tok) return NULL;
    memcpy(tok, start, len);
    tok[len] = '\0';
    for (char *q = tok; *q; ++q)
        if (!isalnum((unsigned char)*q)) { free(tok); return NULL; }
    return tok;
}

/* 登录。cookie 引擎在 curl handle 上启用，Set-Cookie 自动存并供后续请求复用。
 * 返回 malloc 的 token 或 NULL。resp 复用调用方的 buffer。 */
char *do_login(CURL *curl, const Options *o, Buffer *resp)
{
    char login_url[1024];
    snprintf(login_url, sizeof login_url, "%s%s", o->base_url, LOGIN_PATH);
    char *token = NULL;
    curl_mime *form = NULL;
    struct curl_slist *headers = NULL;
    long status = 0;
    CURLcode rc;

    /* 启用 cookie 引擎：传空串让 libcurl 在内存里管理 cookie，不落盘。
     * 登录 response 的 Set-Cookie 会被本 handle 后续请求自动带上。 */
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, "");

    form = curl_mime_init(curl);
    if (!form) goto done;
    for (int i = 0; i < 3; ++i) {
        curl_mimepart *part = curl_mime_addpart(form);
        if (!part) goto done;
        if (i == 0) { curl_mime_name(part, "usrname");  curl_mime_data(part, o->user, CURL_ZERO_TERMINATED); }
        else if (i == 1) { curl_mime_name(part, "string");   curl_mime_data(part, o->password, CURL_ZERO_TERMINATED); }
        else { curl_mime_name(part, "langlist"); curl_mime_data(part, o->langlist, CURL_ZERO_TERMINATED); }
    }
    headers = curl_slist_append(headers, "Accept: application/json, text/plain, */*");
    if (!headers) goto done;

    resp->data[0] = '\0';
    resp->size = 0;
    resp->http_status = 0;
    curl_easy_setopt(curl, CURLOPT_URL, login_url);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    apply_test_tls(curl);

    rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) goto done;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    resp->http_status = status;
    if (status < 200 || status >= 300) goto done;
    token = extract_token(resp->data);
done:
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, NULL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    if (form) curl_mime_free(form);
    curl_slist_free_all(headers);
    return token;
}
