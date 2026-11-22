/*
 * mcp_server.c - MCP (Model Context Protocol) endpoint on the device.
 *
 * The MCP server used to run on the host as a stdio process (tools/scale_mcp.py,
 * deleted when the endpoint moved here). The point of this module is the other
 * direction: the MCP protocol is now served by the firmware itself, so any MCP
 * client can talk to the scale directly over the network - one POST to /mcp
 * per call, no host in the loop.
 *
 * Transport, the streaming-HTTP form of the protocol:
 *
 *   POST /mcp     body: one JSON-RPC 2.0 message
 *   reply:        application/json, the response object
 *   notification: 202 Accepted, empty body
 *
 * Methods (the same tool set the host-side server exposed):
 *
 *   initialize  -> result.serverInfo {name: "esp32-scale", version: running}
 *   tools/list  -> the tools: get_weight, tare, set_unit
 *   tools/call  -> get_weight: the fresh reading, the same fields as
 *                  /api/weight, as MCP tool content text;
 *                  tare: the POST /api/tare equivalent;
 *                  set_unit: arguments.unit, 0=g 1=kg 2=oz.
 *   ping        -> empty result.
 *
 * The JSON parser and the response builder are local: the messages are small
 * and fixed-shape, so no component was pulled in for them.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "mcp_server.h"
#include "ota.h"
#include "web_server.h"
#include "weight_task.h"

static const char *TAG = "mcp";

#define MCP_MAX_BODY  8192   /* larger messages are refused */
#define MCP_RESP_CAP  2048   /* every response we can produce fits this */

#define J_MAX_KIDS  32
#define J_STR_CAP   512

/* ----------------------------------------------------------------- JSON */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;

typedef struct jval jval_t;
struct jval {
    jtype_t type;
    char *str;                      /* J_STR value (malloc'd); NULL otherwise */
    double num;                     /* J_NUM value */
    bool b;                         /* J_BOOL value */
    int n_kids;
    jval_t *kids[J_MAX_KIDS];       /* J_ARR elements / J_OBJ values */
    char *keys[J_MAX_KIDS];         /* J_OBJ keys (malloc'd), parallel to kids */
    char strbuf[J_STR_CAP];         /* parse scratch, copied to str on J_STR */
};

static jval_t *j_new(jtype_t t)
{
    jval_t *v = calloc(1, sizeof(*v));
    if (v != NULL) {
        v->type = t;
    }
    return v;
}

static char *j_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = malloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

static void jval_free(jval_t *v)
{
    if (v == NULL) {
        return;
    }
    for (int i = 0; i < v->n_kids; i++) {
        jval_free(v->kids[i]);
        if (v->type == J_OBJ && v->keys[i] != NULL) {
            free(v->keys[i]);
        }
    }
    free(v->str);
    free(v);
}

static const char *j_skip(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

static bool j_hex4(const char **pp, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = *(*pp)++;
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

/* Parses a JSON string starting at *pp (which must point at the opening
 * quote), unescaping into out. Advances *pp past the closing quote. */
static bool j_parse_string(const char **pp, char *out, size_t cap)
{
    const char *p = *pp + 1;
    size_t n = 0;
    out[0] = '\0';
    while (*p != '\0' && *p != '"') {
        char c;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"':  c = '"';  p++; break;
            case '\\': c = '\\'; p++; break;
            case '/':  c = '/';  p++; break;
            case 'b':  c = '\b'; p++; break;
            case 'f':  c = '\f'; p++; break;
            case 'n':  c = '\n'; p++; break;
            case 'r':  c = '\r'; p++; break;
            case 't':  c = '\t'; p++; break;
            case 'u': {
                unsigned cp;
                p++;
                if (!j_hex4(&p, &cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF &&
                    p[0] == '\\' && p[1] == 'u') {
                    unsigned lo;
                    const char *q = p + 2;
                    if (j_hex4(&q, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p = q;
                    }
                }
                if (n + 4 >= cap) {
                    return false;
                }
                if (cp < 0x80) {
                    out[n++] = (char)cp;
                } else if (cp < 0x800) {
                    out[n++] = (char)(0xC0 | (cp >> 6));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out[n++] = (char)(0xE0 | (cp >> 12));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    out[n++] = (char)(0xF0 | (cp >> 18));
                    out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                    out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    out[n++] = (char)(0x80 | (cp & 0x3F));
                }
                continue;
            }
            default:
                return false;
            }
        } else {
            c = *p++;
        }
        if (n + 2 >= cap) {
            return false;
        }
        out[n++] = c;
    }
    if (*p != '"') {
        return false;              /* unterminated string */
    }
    p++;
    out[n] = '\0';
    *pp = p;
    return true;
}

static jval_t *j_parse_number(const char **pp)
{
    char *end = NULL;
    double d = strtod(*pp, &end);
    if (end == *pp) {
        return NULL;               /* not a number */
    }
    jval_t *v = j_new(J_NUM);
    if (v == NULL) {
        return NULL;
    }
    v->num = d;
    *pp = end;
    return v;
}

static jval_t *j_parse_value(const char **pp);

static jval_t *j_parse_array(const char **pp)
{
    jval_t *a = j_new(J_ARR);
    if (a == NULL) {
        return NULL;
    }
    const char *p = j_skip(*pp + 1);
    if (*p == ']') {
        *pp = p + 1;
        return a;
    }
    while (a->n_kids < J_MAX_KIDS) {
        jval_t *v = j_parse_value(&p);
        if (v == NULL) {
            goto fail;
        }
        a->kids[a->n_kids++] = v;
        p = j_skip(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            *pp = p + 1;
            return a;
        }
        goto fail;
    }
fail:
    jval_free(a);
    return NULL;
}

static jval_t *j_parse_object(const char **pp)
{
    jval_t *o = j_new(J_OBJ);
    if (o == NULL) {
        return NULL;
    }
    const char *p = j_skip(*pp + 1);
    while (o->n_kids < J_MAX_KIDS) {
        p = j_skip(p);
        if (*p == '}') {
            *pp = p + 1;
            return o;
        }
        if (*p != '"') {
            goto fail;
        }
        if (!j_parse_string(&p, o->strbuf, sizeof(o->strbuf))) {
            goto fail;
        }
        p = j_skip(p);
        if (*p != ':') {
            goto fail;
        }
        p++;
        jval_t *v = j_parse_value(&p);
        if (v == NULL) {
            goto fail;
        }
        char *kcopy = j_strdup(o->strbuf);
        if (kcopy == NULL) {
            jval_free(v);
            goto fail;
        }
        o->keys[o->n_kids] = kcopy;
        o->kids[o->n_kids] = v;
        o->n_kids++;
        p = j_skip(p);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == '}') {
            *pp = p + 1;
            return o;
        }
        goto fail;
    }
fail:
    jval_free(o);
    return NULL;
}

static jval_t *j_parse_value(const char **pp)
{
    const char *p = j_skip(*pp);
    *pp = p;
    switch (*p) {
    case '{':
        return j_parse_object(pp);
    case '[':
        return j_parse_array(pp);
    case '"': {
        jval_t *v = j_new(J_STR);
        if (v == NULL) {
            return NULL;
        }
        if (!j_parse_string(pp, v->strbuf, sizeof(v->strbuf))) {
            jval_free(v);
            return NULL;
        }
        v->str = j_strdup(v->strbuf);
        if (v->str == NULL) {
            jval_free(v);
            return NULL;
        }
        return v;
    }
    case 't':
        if (strncmp(p, "true", 4) == 0) {
            jval_t *v = j_new(J_BOOL);
            if (v != NULL) {
                v->b = true;
            }
            *pp = p + 4;
            return v;
        }
        return NULL;
    case 'f':
        if (strncmp(p, "false", 5) == 0) {
            jval_t *v = j_new(J_BOOL);
            if (v != NULL) {
                v->b = false;
            }
            *pp = p + 5;
            return v;
        }
        return NULL;
    case 'n':
        if (strncmp(p, "null", 4) == 0) {
            *pp = p + 4;
            return j_new(J_NULL);
        }
        return NULL;
    default:
        return j_parse_number(pp);
    }
}

/* Returns the value stored under key in the object o, or NULL. */
static const jval_t *j_obj_get(const jval_t *o, const char *key)
{
    if (o == NULL || o->type != J_OBJ) {
        return NULL;
    }
    for (int i = 0; i < o->n_kids; i++) {
        if (o->keys[i] != NULL && strcmp(o->keys[i], key) == 0) {
            return o->kids[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------- response builder */

/* Appends to out at pos; returns the new pos, capped so the NUL terminator
 * always fits. Every call NUL-terminates what was written. */
static size_t wr(char *out, size_t cap, size_t pos, const char *fmt, ...)
{
    if (pos + 1 >= cap) {
        return pos;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return pos;
    }
    pos += (size_t)n;
    if (pos >= cap) {
        pos = cap - 1;
    }
    return pos;
}

/* Appends the characters of s, escaped for a JSON string body, without the
 * surrounding quotes. */
static size_t wr_esc(char *out, size_t cap, size_t pos, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  pos = wr(out, cap, pos, "\\\""); break;
        case '\\': pos = wr(out, cap, pos, "\\\\"); break;
        case '\b': pos = wr(out, cap, pos, "\\b"); break;
        case '\f': pos = wr(out, cap, pos, "\\f"); break;
        case '\n': pos = wr(out, cap, pos, "\\n"); break;
        case '\r': pos = wr(out, cap, pos, "\\r"); break;
        case '\t': pos = wr(out, cap, pos, "\\t"); break;
        default:
            if (c < 0x20) {
                pos = wr(out, cap, pos, "\\u%04x", (unsigned)c);
            } else {
                pos = wr(out, cap, pos, "%c", (int)c);
            }
            break;
        }
    }
    return pos;
}

/* Appends a quoted, escaped JSON string. */
static size_t wr_json_str(char *out, size_t cap, size_t pos, const char *s)
{
    pos = wr(out, cap, pos, "\"");
    pos = wr_esc(out, cap, pos, s);
    return wr(out, cap, pos, "\"");
}

/* ------------------------------------------------------- the MCP surface */

/* The tools/list payload - the same tool set the removed host-side server
 * (tools/scale_mcp.py) exposed. */
static const char MCP_TOOLS_JSON[] =
    "{\"tools\":["
    "{\"name\":\"get_weight\","
    "\"description\":\"Current reading from the scale: value and unit, grams, sigma_mg (noise), stable/zero/tare flags, calibration constants (cal, tare_offset) and uptime. The reading is fresh on each call.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
    "{\"name\":\"tare\","
    "\"description\":\"Take a tare on the scale: the platform is assumed empty and becomes zero. Check with get_weight that the platform is actually empty before calling.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
    "{\"name\":\"set_unit\","
    "\"description\":\"Switch the scale display unit.\","
    "\"inputSchema\":{\"type\":\"object\","
    "\"properties\":{\"unit\":{\"type\":\"integer\",\"enum\":[0,1,2],\"description\":\"0=grams, 1=kilograms, 2=ounces\"}},"
    "\"required\":[\"unit\"],\"additionalProperties\":false}}"
    "]}";

/* tools/call: dispatches on params.name and appends the JSON-RPC result or
 * error to out. The response object is closed by the caller's tail brace. */
static size_t mcp_tools_call(char *out, size_t cap, size_t pos,
                             const jval_t *params)
{
    const jval_t *name = params ? j_obj_get(params, "name") : NULL;

    if (name == NULL || name->type != J_STR) {
        return wr(out, cap, pos,
                  ",\"error\":{\"code\":-32602,"
                  "\"message\":\"tools/call requires a tool name\"}");
    }

    if (strcmp(name->str, "get_weight") == 0) {
        char wj[384];
        web_weight_json(wj, sizeof(wj));
        pos = wr(out, cap, pos,
                 ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":");
        pos = wr_json_str(out, cap, pos, wj);
        return wr(out, cap, pos, "}],\"isError\":false}");
    }

    if (strcmp(name->str, "tare") == 0) {
        weight_task_tare();
        return wr(out, cap, pos,
                  ",\"result\":{\"content\":[{\"type\":\"text\","
                  "\"text\":\"{\\\"ok\\\":true}\"}],"
                  "\"isError\":false}");
    }

    if (strcmp(name->str, "set_unit") == 0) {
        const jval_t *args = params ? j_obj_get(params, "arguments") : NULL;
        const jval_t *u = args ? j_obj_get(args, "unit") : NULL;
        if (u != NULL && u->type == J_NUM &&
            u->num >= 0.0 && u->num <= 2.0) {
            weight_task_set_unit((scale_unit_t)(int)u->num);
            return wr(out, cap, pos,
                      ",\"result\":{\"content\":[{\"type\":\"text\","
                      "\"text\":\"{\\\"ok\\\":true}\"}],"
                      "\"isError\":false}");
        }
        return wr(out, cap, pos,
                  ",\"result\":{\"content\":[{\"type\":\"text\","
                  "\"text\":\"unit must be 0 (g), 1 (kg) or 2 (oz)\"}],"
                  "\"isError\":true}");
    }

    pos = wr(out, cap, pos,
             ",\"error\":{\"code\":-32602,\"message\":\"unknown tool: ");
    pos = wr_esc(out, cap, pos, name->str);
    return wr(out, cap, pos, "\"}");
}

bool mcp_process(char *body, size_t len, char *out, size_t out_cap)
{
    if (out_cap < 4) {
        return false;
    }
    out[0] = '\0';

    /* The parser reads the body as a NUL-terminated string; make sure the
     * buffer ends where the caller says it does. */
    if (len > MCP_MAX_BODY) {
        len = MCP_MAX_BODY;
    }
    body[len] = '\0';

    const char *p = body;
    jval_t *root = j_parse_value(&p);
    p = j_skip(p);
    if (root == NULL || root->type != J_OBJ || *p != '\0') {
        ESP_LOGW(TAG, "mcp: unparseable request body");
        wr(out, out_cap, 0,
           "{\"jsonrpc\":\"2.0\",\"id\":null,"
           "\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
        jval_free(root);
        return true;
    }

    const jval_t *id = j_obj_get(root, "id");
    const jval_t *method = j_obj_get(root, "method");

    /* No id (or an unusable one): notification, answer nothing. */
    bool has_id = (id != NULL && (id->type == J_NUM || id->type == J_STR));
    if (!has_id) {
        jval_free(root);
        return false;
    }

    size_t pos = wr(out, out_cap, 0, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (id->type == J_STR) {
        pos = wr_json_str(out, out_cap, pos, id->str);
    } else {
        pos = wr(out, out_cap, pos, "%lld", (long long)id->num);
    }

    if (method == NULL || method->type != J_STR) {
        pos = wr(out, out_cap, pos,
                 ",\"error\":{\"code\":-32600,\"message\":\"Invalid Request\"}");
    } else if (strncmp(method->str, "notifications/", 14) == 0) {
        jval_free(root);
        return false;
    } else if (strcmp(method->str, "ping") == 0) {
        pos = wr(out, out_cap, pos, ",\"result\":{}");
    } else if (strcmp(method->str, "initialize") == 0) {
        pos = wr(out, out_cap, pos,
                 ",\"result\":{\"protocolVersion\":\"2025-06-18\","
                 "\"capabilities\":{\"tools\":{}},"
                 "\"serverInfo\":{\"name\":\"esp32-scale\",\"version\":\"");
        pos = wr(out, out_cap, pos, "%s", ota_running_version());
        pos = wr(out, out_cap, pos, "\"}}");
    } else if (strcmp(method->str, "tools/list") == 0) {
        pos = wr(out, out_cap, pos, ",\"result\":");
        pos = wr(out, out_cap, pos, "%s", MCP_TOOLS_JSON);
    } else if (strcmp(method->str, "tools/call") == 0) {
        pos = mcp_tools_call(out, out_cap, pos, j_obj_get(root, "params"));
    } else {
        pos = wr(out, out_cap, pos,
                 ",\"error\":{\"code\":-32601,\"message\":\"method not found: ");
        pos = wr_esc(out, out_cap, pos, method->str);
        pos = wr(out, out_cap, pos, "\"}");
    }

    wr(out, out_cap, pos, "}");
    jval_free(root);
    return true;
}

/* ------------------------------------------------------------- the handler */

esp_err_t mcp_handler(httpd_req_t *req)
{
    /* +1: mcp_process NUL-terminates the body at body[len]. */
    char *body = malloc(MCP_MAX_BODY + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"out of memory\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    int remaining = (int)req->content_len;
    if (remaining < 0) {
        remaining = MCP_MAX_BODY;              /* chunked: read until EOF */
    } else if (remaining > MCP_MAX_BODY) {
        remaining = MCP_MAX_BODY;              /* refuse oversized bodies */
    }

    int n = 0;
    int timeouts = 0;
    while (n < remaining) {
        int r = httpd_req_recv(req, body + n, remaining - n);
        if (r == 0) {
            break;                              /* nothing more to read */
        }
        if (r < 0) {
            if (r != HTTPD_SOCK_ERR_TIMEOUT || ++timeouts >= 5) {
                break;
            }
            continue;
        }
        timeouts = 0;
        n += r;
    }

    char resp[MCP_RESP_CAP];
    bool send = mcp_process(body, (size_t)n, resp, sizeof(resp));
    free(body);

    httpd_resp_set_type(req, "application/json");
    if (send) {
        httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    } else {
        /* Notification: acknowledged, no body. */
        httpd_resp_set_status(req, "202 Accepted");
        httpd_resp_send(req, "", 0);
    }
    return ESP_OK;
}
