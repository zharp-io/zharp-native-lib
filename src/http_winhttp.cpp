// HTTP for Windows: WinHTTP, part of every Windows; it honours the system's proxy settings.
#include "internal.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

namespace zharp {

void http_global_init() {}

static std::wstring widen(const std::string &text) {
    if (text.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &out[0], n);
    return out;
}

int http_post(const std::string &url, const std::vector<std::pair<std::string, std::string>> &headers,
              const std::string &body, int timeout_ms, std::string *error) {
    std::wstring wide_url = widen(url);
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    wchar_t extra[2048];
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
        if (error) *error = "invalid URL";
        return -1;
    }
    std::wstring object = std::wstring(path, parts.dwUrlPathLength) + std::wstring(extra, parts.dwExtraInfoLength);

    int status = -1;
    HINTERNET session = WinHttpOpen(L"zharp-native", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // Windows before 8.1 has no automatic proxy type.
        session = WinHttpOpen(L"zharp-native", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    HINTERNET connection = nullptr, request = nullptr;
    if (session) {
        WinHttpSetTimeouts(session, timeout_ms, 10000, timeout_ms, timeout_ms);
        connection = WinHttpConnect(session, std::wstring(host, parts.dwHostNameLength).c_str(), parts.nPort, 0);
    }
    if (connection) {
        request = WinHttpOpenRequest(connection, L"POST", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    }
    if (request) {
        std::wstring header_text;
        for (const auto &header : headers) header_text += widen(header.first + ": " + header.second + "\r\n");
        BOOL ok = WinHttpSendRequest(request, header_text.c_str(), (DWORD)-1L, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
        if (ok) ok = WinHttpReceiveResponse(request, nullptr);
        if (ok) {
            DWORD code = 0, size = sizeof code;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
                status = (int)code;
            }
        } else if (error) {
            *error = "WinHTTP error " + std::to_string(GetLastError());
        }
    } else if (error) {
        *error = "WinHTTP error " + std::to_string(GetLastError());
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    return status;
}

}  // namespace zharp
