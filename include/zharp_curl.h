/*
 * Zharp for libcurl: a client span per request, with its request and response headers (secrets
 * kept as [REDACTED]) and a traceparent header so the server's trace continues this one.
 * Header-only; include it where you use libcurl, after <curl/curl.h>.
 *
 *   struct curl_slist *headers = curl_slist_append(NULL, "Accept: application/json");
 *   curl_easy_setopt(curl, CURLOPT_URL, "https://api.example.com/orders");
 *   CURLcode code = zharp_curl_perform(curl, headers);   // in place of curl_easy_perform(curl)
 *
 * Pass the same header list you would set with CURLOPT_HTTPHEADER (or NULL): libcurl can't hand
 * it back, so Zharp records it from here and adds traceparent to a copy for this request.
 * Response headers need libcurl 7.84 or later.
 */
#ifndef ZHARP_CURL_H
#define ZHARP_CURL_H

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#include "zharp.h"

#ifdef __cplusplus
extern "C" {
#endif

static void zharp_curl__header_(zharp_span_t *span, const char *side, const char *line) {
    char name[256];
    const char *colon = strchr(line, ':');
    size_t length;
    if (!colon) return;
    length = (size_t)(colon - line);
    if (length == 0 || length >= sizeof name) return;
    memcpy(name, line, length);
    name[length] = '\0';
    colon++;
    while (*colon == ' ' || *colon == '\t') colon++;
    zharp_span_set_header(span, side, name, colon);
}

/* curl_easy_perform with a span around it. */
static CURLcode zharp_curl_perform(CURL *curl, struct curl_slist *headers) {
    zharp_span_t *span = zharp_start_span("HTTP");
    struct curl_slist *sent = NULL, *item;
    char traceparent[64], line[96], name[512];
    char *url = NULL, *method = NULL;
    long status = 0;
    CURLcode code;

    zharp_span_set_kind(span, 2);
    for (item = headers; item; item = item->next) {
        zharp_curl__header_(span, "request", item->data);
        sent = curl_slist_append(sent, item->data);
    }
    if (zharp_traceparent(traceparent, sizeof traceparent)) {
        snprintf(line, sizeof line, "traceparent: %s", traceparent);
        sent = curl_slist_append(sent, line);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, sent);

    code = curl_easy_perform(curl);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_slist_free_all(sent);

    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
#if LIBCURL_VERSION_NUM >= 0x074800
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_METHOD, &method);
#endif
    if (!method) method = (char *)"GET";
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    zharp_span_set_attribute(span, "http.request.method", method);
    if (url) {
        /* The URL without its query string, which can carry tokens. */
        char full[2048];
        const char *host = strstr(url, "://");
        size_t n = strcspn(url, "?#");
        if (n >= sizeof full) n = sizeof full - 1;
        memcpy(full, url, n);
        full[n] = '\0';
        zharp_span_set_attribute(span, "url.full", full);
        if (host) {
            host += 3;
            n = strcspn(host, ":/?#");
            if (n >= sizeof name) n = sizeof name - 1;
            memcpy(name, host, n);
            name[n] = '\0';
            zharp_span_set_attribute(span, "server.address", name);
            snprintf(line, sizeof line, "%s %s", method, name);
        } else {
            snprintf(line, sizeof line, "%s", method);
        }
    }
    if (status > 0) {
        char text[16];
        snprintf(text, sizeof text, "%ld", status);
        zharp_span_set_attribute(span, "http.response.status_code", text);
    }
#if LIBCURL_VERSION_NUM >= 0x075400
    {
        struct curl_header *header = NULL;
        while ((header = curl_easy_nextheader(curl, CURLH_HEADER, -1, header)) != NULL) {
            zharp_span_set_header(span, "response", header->name, header->value);
        }
    }
#endif
    if (code != CURLE_OK) {
        zharp_span_set_error(span, curl_easy_strerror(code));
    } else if (status >= 400) {
        snprintf(name, sizeof name, "HTTP %ld", status);
        zharp_span_set_error(span, name);
    }
    /* The span's name: "GET api.example.com". */
    zharp_span_set_name(span, url ? line : method);
    zharp_end_span(span);
    return code;
}

#ifdef __cplusplus
}
#endif

#endif /* ZHARP_CURL_H */
