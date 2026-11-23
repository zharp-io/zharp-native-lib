// HTTP for Linux and macOS: libcurl (the system's on macOS; libcurl4 on Linux).
#include "internal.h"

#include <curl/curl.h>
#include <mutex>

namespace zharp {

void http_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

static size_t discard(char *, size_t size, size_t count, void *) {
    return size * count;
}

int http_post(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
              const std::string &body, int timeout_ms, std::string *error) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error) *error = "curl_easy_init failed";
        return -1;
    }
    struct curl_slist *list = nullptr;
    for (const auto &header : headers) list = curl_slist_append(list, (header.first + ": " + header.second).c_str());
    list = curl_slist_append(list, "Expect:");
    char message[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeout_ms);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message);
    CURLcode code = curl_easy_perform(curl);
    long status = -1;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    } else if (error) {
        *error = message[0] ? message : curl_easy_strerror(code);
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return (int)status;
}

}  // namespace zharp
