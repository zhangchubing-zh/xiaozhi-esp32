/*
 * PLC 功能调度入口：agent 通过 -o 选择功能，main 组合调用 asp/ 中的请求。
 * 以下示例在本文件所在 scripts 目录执行；每个命令内部自动登录并复用会话。
 *
 * ./main -o login
 *   功能：验证登录，不输出密码、Cookie 或 token。
 * ./main -o get_cache_info
 *   功能：查询设备缓存/连通信息。
 * ./main -o get_active_alarm
 *   功能：查询所有设备活动告警；--equip-id 1 仅查 Logger 的告警。
 * ./main -o get_monitor_info --equip-id 4099 --equip-type-id 33036
 *   功能：查询目标设备信号，默认 para3=4，包含网络频段信号8201。
 * ./main -o get_history_alarm --equip-id 1 --page-index 1 --page-size 20
 *   功能：查询指定设备历史告警；默认时间范围最近365天。
 * ./main -o set_freq_band --equip-id 4099 --equip-type-id 33036
 *   功能：读当前频段 -> 下一Band -> 写入 -> 读回验证；每次只调整一次。
 *   Band1->2->3->4->5 对应接口值1->3->23->7->13；Band5不自动循环。
 * ./main -o set_freq_band --equip-id 4099 --equip-type-id 33036 --value 23
 *   功能：明确指定Band3，仍读取原值并在写入后读回验证。
 * 通用参数：--base-url、--user、--password（或环境变量 LITECRAB_ALARM_USER/LITECRAB_ALARM_PASSWORD）；--output 保存最终原始响应。
 *
 * 通信异常流程：agent 先调用 get_active_alarm，解释告警并确定配置设备，
 * 再调用 set_freq_band，最后重新查询 get_active_alarm 检查告警状态。
 * 告警上报设备ID不一定等于频段配置设备ID，不直接混用。
 * 当前没有 diagnose_communication 子命令或独立 diagnosis 文件。
 * 退出码：0成功，1参数错，2本地错，3网络错，4接口错，5认证错，6频段/验证失败。
 */
#include "asp/asp.h"
#include "litecrab/json.h"

static void usage(const char *program)
{
    fprintf(stderr,
        "Usage: %s -o <login|get_cache_info|get_monitor_info|get_history_alarm|get_active_alarm|set_freq_band> [opts]\n"
        "  Credentials: export LITECRAB_ALARM_USER / LITECRAB_ALARM_PASSWORD (or --user/--password)\n"
        "  --langlist zh-cn\n"
        "  --base-url https://192.168.8.10\n"
        "  --equip-id 4099 --equip-type-id 33036    (get_monitor_info / set_freq_band)\n"
        "  --para3 4 --para4 2                       (get_monitor_info)\n"
        "  --equip-id <id>                           (get_history_alarm; default 0=all)\n"
        "  --start-time YYYY-MM-DD-HH-mm-ss          (default: 365 days ago)\n"
        "  --end-time YYYY-MM-DD-HH-mm-ss            (default: now)\n"
        "  --page-index 1 --page-size 20              (get_history_alarm)\n"
        "  --alarm-level <0|1|2|3|255>                (255=all)\n"
        "  --sort-type <0|1> --sort-field <0|1>       (time/level; end/start)\n"
        "  --output <file>                            (save final raw response JSON)\n"
        "  --value <1|3|23|7|13>                     (optional; default: read current band and advance one)\n",
        program);
}

static bool valid_str(const char *s)
{
    if (s == NULL || *s == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned char c = *p;
        if (c < 0x20) return false;
        if (c == '"' || c == '\'' || c == '`' || c == ';' || c == '|' ||
            c == '<' || c == '>' || c == '\\' || c == '$') return false;
    }
    return true;
}

static bool valid_uint(const char *s)
{
    if (s == NULL || *s == '\0') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (*p < '0' || *p > '9') return false;
    return true;
}

static bool valid_url(const char *u)
{
    return u && (!strncmp(u, "https://", 8) || !strncmp(u, "http://", 7)) && valid_str(u);
}

static bool valid_op(const char *op)
{
    return op && (!strcmp(op, "login") || !strcmp(op, "get_cache_info") ||
                  !strcmp(op, "get_monitor_info") || !strcmp(op, "get_history_alarm") ||
                  !strcmp(op, "get_active_alarm") || !strcmp(op, "set_freq_band"));
}

static bool valid_history_time(const char *s)
{
    if (s == NULL) return true;
    if (strlen(s) != 19U) return false;
    for (size_t i = 0; i < 19U; ++i) {
        if (i == 4U || i == 7U || i == 10U || i == 13U || i == 16U) {
            if (s[i] != '-') return false;
        } else if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static bool value_in(const char *value, const char *const *allowed)
{
    for (size_t i = 0; allowed[i]; ++i)
        if (!strcmp(value, allowed[i])) return true;
    return false;
}

/* 频段可选值白名单，防误写 */
static bool valid_band_value(const char *v)
{
    static const char *allowed[] = {"1", "3", "23", "7", "13", NULL};
    for (int i = 0; allowed[i]; ++i)
        if (!strcmp(v, allowed[i])) return true;
    return false;
}

static bool validate(const Options *o)
{
    if (!valid_op(o->operation)) { fprintf(stderr, "Invalid -o operation\n"); return false; }
    if (!valid_str(o->user)) { fprintf(stderr, "Missing --user (set env %s)\n", ENV_USER); return false; }
    if (!valid_str(o->password)) { fprintf(stderr, "Missing --password (set env %s)\n", ENV_PASSWORD); return false; }
    if (!valid_str(o->langlist)) { fprintf(stderr, "Invalid --langlist\n"); return false; }
    if (!valid_url(o->base_url)) { fprintf(stderr, "Invalid --base-url\n"); return false; }

    if (!strcmp(o->operation, "get_monitor_info")) {
        if (!valid_uint(o->equip_id)) { fprintf(stderr, "Invalid --equip-id\n"); return false; }
        if (!valid_uint(o->equip_type_id)) { fprintf(stderr, "Invalid --equip-type-id\n"); return false; }
        if (!valid_uint(o->para3)) { fprintf(stderr, "Invalid --para3\n"); return false; }
        if (!valid_uint(o->para4)) { fprintf(stderr, "Invalid --para4\n"); return false; }
    }
    if (!strcmp(o->operation, "set_freq_band")) {
        if (!valid_uint(o->equip_id)) { fprintf(stderr, "Invalid --equip-id\n"); return false; }
        if (!valid_uint(o->equip_type_id)) { fprintf(stderr, "Invalid --equip-type-id\n"); return false; }
        if ((o->value && !valid_band_value(o->value))) {
            fprintf(stderr, "Invalid --value; allowed: 1 3 23 7 13\n");
            return false;
        }
    }
    if (!strcmp(o->operation, "get_history_alarm")) {
        static const char *const levels[] = {"0", "1", "2", "3", "255", NULL};
        static const char *const binary[] = {"0", "1", NULL};
        if (!valid_uint(o->history_equip_id)) { fprintf(stderr, "Invalid --equip-id\n"); return false; }
        if (!valid_history_time(o->start_time)) { fprintf(stderr, "Invalid --start-time\n"); return false; }
        if (!valid_history_time(o->end_time)) { fprintf(stderr, "Invalid --end-time\n"); return false; }
        if (!valid_uint(o->page_index) || !strcmp(o->page_index, "0")) {
            fprintf(stderr, "Invalid --page-index; must be >= 1\n"); return false;
        }
        if (!valid_uint(o->page_size) || !strcmp(o->page_size, "0")) {
            fprintf(stderr, "Invalid --page-size; must be >= 1\n"); return false;
        }
        if (!value_in(o->alarm_level, levels)) {
            fprintf(stderr, "Invalid --alarm-level; allowed: 0 1 2 3 255\n"); return false;
        }
        if (!value_in(o->sort_type, binary)) {
            fprintf(stderr, "Invalid --sort-type; allowed: 0 1\n"); return false;
        }
        if (!value_in(o->sort_field, binary)) {
            fprintf(stderr, "Invalid --sort-field; allowed: 0 1\n"); return false;
        }
        if (o->output && *o->output == '\0') {
            fprintf(stderr, "Invalid --output\n"); return false;
        }
    }
    if (!valid_uint(o->equip_id) || !valid_uint(o->history_equip_id)) return false;
    return true;
}

static bool parse_args(int argc, char **argv, Options *o)
{
    static const struct option opts[] = {
        {"operation",     required_argument, NULL, 'o'},
        {"user",          required_argument, NULL, 'u' + 256},
        {"password",      required_argument, NULL, 'p' + 256},
        {"langlist",      required_argument, NULL, 'l' + 256},
        {"base-url",      required_argument, NULL, 'b' + 256},
        {"equip-id",      required_argument, NULL, 'e' + 256},
        {"equip-type-id", required_argument, NULL, 'E' + 256},
        {"para3",         required_argument, NULL, 3001},
        {"para4",         required_argument, NULL, 3002},
        {"start-time",    required_argument, NULL, 3003},
        {"end-time",      required_argument, NULL, 3004},
        {"page-index",    required_argument, NULL, 3005},
        {"page-size",     required_argument, NULL, 3006},
        {"alarm-level",   required_argument, NULL, 3007},
        {"sort-type",     required_argument, NULL, 3008},
        {"sort-field",    required_argument, NULL, 3009},
        {"output",        required_argument, NULL, 3010},
        {"value",         required_argument, NULL, 'v' + 256},
        {"help",          no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    int c;
    memset(o, 0, sizeof(*o));
    o->user = getenv(ENV_USER);
    o->password = getenv(ENV_PASSWORD);
    o->langlist = DEFAULT_LANGLIST;
    o->base_url = BASE_URL_DEFAULT;
    o->equip_id = DEFAULT_EQUIP_ID;
    o->equip_type_id = DEFAULT_EQUIP_TYPE_ID;
    o->para3 = DEFAULT_PARA3;
    o->para4 = DEFAULT_PARA4;
    o->history_equip_id = DEFAULT_HISTORY_EQUIP;
    o->page_index = DEFAULT_PAGE_INDEX;
    o->page_size = DEFAULT_PAGE_SIZE;
    o->alarm_level = DEFAULT_ALARM_LEVEL;
    o->sort_type = DEFAULT_SORT_TYPE;
    o->sort_field = DEFAULT_SORT_FIELD;
    opterr = 0;
    while ((c = getopt_long(argc, argv, "o:h", opts, NULL)) != -1) {
        if (c == 'o') o->operation = optarg;
        else if (c == 'u' + 256) o->user = optarg;
        else if (c == 'p' + 256) o->password = optarg;
        else if (c == 'l' + 256) o->langlist = optarg;
        else if (c == 'b' + 256) o->base_url = optarg;
        else if (c == 'e' + 256) { o->equip_id = optarg; o->history_equip_id = optarg; }
        else if (c == 'E' + 256) o->equip_type_id = optarg;
        else if (c == 3001) o->para3 = optarg;
        else if (c == 3002) o->para4 = optarg;
        else if (c == 3003) o->start_time = optarg;
        else if (c == 3004) o->end_time = optarg;
        else if (c == 3005) o->page_index = optarg;
        else if (c == 3006) o->page_size = optarg;
        else if (c == 3007) o->alarm_level = optarg;
        else if (c == 3008) o->sort_type = optarg;
        else if (c == 3009) o->sort_field = optarg;
        else if (c == 3010) o->output = optarg;
        else if (c == 'v' + 256) o->value = optarg;
        else if (c == 'h') { usage(argv[0]); exit(OK); }
        else { fprintf(stderr, "Unknown or incomplete option\n"); return false; }
    }
    if (optind != argc) {
        fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
        return false;
    }
    return validate(o);
}

/* Function orchestration belongs here. The agent interprets active alarms and
 * selects the next operation; ASP modules only implement individual requests. */
static const char *const band_values[] = {"1", "3", "23", "7", "13"};

static int current_band(const Buffer *r)
{
    if (LjValidate(r->data, LJ_OBJECT)) return -1;
    LjToken *t = calloc(r->size + 1, sizeof *t);
    if (!t) return -1;
    LjParser parser;
    LjInit(&parser);
    int n = LjParse(&parser, r->data, r->size, t, (unsigned)(r->size + 1));
    int result = -1;
    int list = LjObjectGet(r->data, t, n, 0, "sigList");
    if (list < 0 || t[list].type != LJ_ARRAY) goto done;
    for (int i = 0; i < t[list].size; ++i) {
        int item = LjArrayGet(t, n, list, i);
        int id = LjObjectGet(r->data, t, n, item, "sigId");
        int val = LjObjectGet(r->data, t, n, item, "sigValue");
        int64_t number;
        if (id < 0 || val < 0 || LjInt64(r->data, &t[id], &number) || number != 8201) continue;
        for (int j = 0; j < 5; ++j) {
            if (LjTokenEq(r->data, &t[val], band_values[j]) ||
                (LjInt64(r->data, &t[val], &number) == 0 && number == strtoll(band_values[j], NULL, 10))) {
                result = j;
                goto done;
            }
        }
    }
done:
    free(t);
    return result;
}

static int change_freq_band(CURL *c, const char *token, const Options *o, Buffer *r,
                            FrequencyBandResult *result)
{
    memset(result, 0, sizeof(*result));
    result->before_band = result->target_band = result->after_band = -1;
    result->write_business_code = result->device_result = result->signal_result = -1;
    Options request = *o;
    request.para3 = "4";
    request.para4 = "2";
    int rc = asp_get_monitor_info(c, token, &request, r);
    result->before_http_status = r->http_status;
    if (rc != OK) {
        result->failed_stage = "read_before";
        return rc;
    }
    int before = current_band(r);
    result->before_band = before;
    if (before < 0) {
        result->failed_stage = "plan";
        return DIAGNOSIS_ERROR;
    }
    /* Band numbers advance, not the device enum values. At Band5 stop. */
    if (!request.value) {
        if (before == 4) {
            result->failed_stage = "plan";
            return DIAGNOSIS_ERROR;
        }
        request.value = band_values[before + 1];
    }
    int target = 0;
    while (target < 5 && strcmp(request.value, band_values[target])) ++target;
    result->target_band = target;
    result->target_value = request.value;
    result->write_attempted = 1;
    int write_rc = asp_set_freq_band(c, token, &request, r);
    result->write_http_status = r->http_status;
    int write_valid = write_rc == OK && !normalized_parse_set_result(r, &request, result);

    /* A write was attempted. Even if its response is unclear, perform one
     * read-only verification instead of retrying the write. */
    rc = asp_get_monitor_info(c, token, &request, r);
    result->after_http_status = r->http_status;
    if (rc != OK) {
        result->failed_stage = "readback";
        return rc;
    }
    result->after_band = current_band(r);
    result->readback_matches = result->after_band == target;
    if (write_rc != OK) {
        result->failed_stage = "write";
        return write_rc;
    }
    if (!write_valid) {
        result->failed_stage = "write";
        return HTTP_ERROR;
    }
    if (!result->readback_matches) {
        result->failed_stage = "readback";
        return DIAGNOSIS_ERROR;
    }
    return OK;
}

static int save_if_requested(const Options *options, const Buffer *response, int rc)
{
    if (rc == OK && options->output && save_response(options->output, response))
        return LOCAL_ERROR;
    return rc;
}

int main(int argc, char **argv)
{
    Options options;
    if (!parse_args(argc, argv, &options)) { usage(argv[0]); return ARG_ERROR; }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return LOCAL_ERROR;

    int exit_code = LOCAL_ERROR;
    CURL *curl = curl_easy_init();
    Buffer resp = { malloc(RESPONSE_INITIAL), 0, RESPONSE_INITIAL, 0 };
    char *token = NULL;
    if (!curl || !resp.data) {
        fprintf(stderr, "Out of memory or curl init failed\n");
        goto top_done;
    }
    resp.data[0] = '\0';

    /* 第一步：登录拿 token（cookie 引擎已启用，Set-Cookie 自动留存本 handle） */
    token = do_login(curl, &options, &resp);
    if (!token) {
        exit_code = AUTH_ERROR;
        if (!strcmp(options.operation, "login")) normalized_print_login(exit_code, resp.http_status);
        else normalized_print_error(options.operation, exit_code, resp.http_status,
                                    "login", "Authentication failed");
        goto top_done;
    }

    if (!strcmp(options.operation, "login")) {
        exit_code = OK;
        normalized_print_login(exit_code, resp.http_status);
        goto top_done;
    }

    FrequencyBandResult frequency_result;
    if (!strcmp(options.operation, "get_cache_info")) {
        exit_code = asp_get_cache_info(curl, token, &options, &resp);
        exit_code = save_if_requested(&options, &resp, exit_code);
        normalized_print_cache(&options, &resp, exit_code);
    } else if (!strcmp(options.operation, "get_monitor_info")) {
        exit_code = asp_get_monitor_info(curl, token, &options, &resp);
        exit_code = save_if_requested(&options, &resp, exit_code);
        normalized_print_monitor_info(&options, &resp, exit_code);
    } else if (!strcmp(options.operation, "get_history_alarm")) {
        exit_code = asp_get_history_alarm(curl, token, &options, &resp);
        exit_code = save_if_requested(&options, &resp, exit_code);
        normalized_print_history_alarm(&options, &resp, exit_code);
    } else if (!strcmp(options.operation, "get_active_alarm")) {
        exit_code = asp_get_active_alarm(curl, token, &options, &resp);
        exit_code = save_if_requested(&options, &resp, exit_code);
        normalized_print_active_alarm(&options, &resp, exit_code);
    } else if (!strcmp(options.operation, "set_freq_band")) {
        exit_code = change_freq_band(curl, token, &options, &resp, &frequency_result);
        exit_code = save_if_requested(&options, &resp, exit_code);
        normalized_print_frequency_band(&options, &frequency_result, exit_code);
    }
top_done:
    free(token);
    free(resp.data);
    if (curl) curl_easy_cleanup(curl);
    curl_global_cleanup();
    return exit_code;
}
