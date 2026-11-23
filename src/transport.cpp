// OTLP/HTTP JSON to Zharp from one background thread. Records are batched; a batch that can't be
// sent (offline, Zharp unreachable, 429/5xx) is written to the outbox folder and sent again later,
// also by the next launch.
#include "internal.h"
#include "zharp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace zharp {

namespace {

const size_t MAX_QUEUED = 5000;
const size_t MAX_BATCH = 500;
const size_t MAX_OUTBOX_FILES = 100;
const auto SEND_INTERVAL = std::chrono::seconds(2);
const auto OUTBOX_RETRY = std::chrono::seconds(60);

struct Transport {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done_changed;
    std::deque<std::string> logs;
    std::deque<std::string> spans;
    uint64_t queued = 0;     // records ever queued
    uint64_t finished = 0;   // records sent, kept in the outbox, or dropped
    bool all_sent = true;    // since the last flush began
    bool stopping = false;
    bool flushing = false;
    std::thread thread;
    Config config;
    std::string resource;
    std::string outbox;
    bool rejected_warned = false;
};

Transport *g_transport = nullptr;

std::string payload(Signal signal, const std::string &resource, const std::deque<std::string> &records, size_t count) {
    std::string scope = "{\"scope\":{\"name\":\"" ZHARP_SDK_NAME "\",\"version\":\"" ZHARP_SDK_VERSION "\"},";
    std::string body = signal == Signal::Logs ? "{\"resourceLogs\":[{\"resource\":{\"attributes\":" : "{\"resourceSpans\":[{\"resource\":{\"attributes\":";
    body += resource;
    body += signal == Signal::Logs ? "},\"scopeLogs\":[" : "},\"scopeSpans\":[";
    body += scope;
    body += signal == Signal::Logs ? "\"logRecords\":[" : "\"spans\":[";
    for (size_t i = 0; i < count; i++) {
        if (i) body += ',';
        body += records[i];
    }
    body += "]}]}]}";
    return body;
}

std::string url_for(const Config &config, Signal signal) {
    return config.endpoint + (signal == Signal::Logs ? "/v1/logs" : "/v1/traces");
}

enum class Outcome { Sent, Retry, Dropped };

Outcome post(Transport &t, Signal signal, const std::string &body) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Authorization", "Bearer " + t.config.key},
        {"Content-Type", "application/json"},
        {"User-Agent", ZHARP_SDK_NAME "/" ZHARP_SDK_VERSION},
    };
    std::string error;
    int status = http_post(url_for(t.config, signal), headers, body, 15000, &error);
    if (status >= 200 && status < 300) {
        debug_log("sent %s (%zu bytes): %d", signal == Signal::Logs ? "logs" : "spans", body.size(), status);
        return Outcome::Sent;
    }
    if (status == -1 || status == 408 || status == 429 || status >= 500) {
        debug_log("couldn't send %s (%s); kept to send later", signal == Signal::Logs ? "logs" : "spans",
                  status == -1 ? error.c_str() : std::to_string(status).c_str());
        return Outcome::Retry;
    }
    if ((status == 401 || status == 403) && !t.rejected_warned) {
        t.rejected_warned = true;
        warn("Zharp refused the key (HTTP %d): check ZHARP_KEY (Applications → your app → Setup)", status);
    } else {
        debug_log("Zharp refused %s: HTTP %d", signal == Signal::Logs ? "logs" : "spans", status);
    }
    return Outcome::Dropped;
}

void keep_in_outbox(Transport &t, Signal signal, const std::string &body) {
    if (t.outbox.empty()) return;
    std::vector<std::string> files = t.outbox.empty() ? std::vector<std::string>() : list_dir(t.outbox);
    // The oldest go first when the outbox is full.
    for (size_t i = 0; files.size() >= MAX_OUTBOX_FILES && i < files.size() - MAX_OUTBOX_FILES + 1; i++) remove_file(join_path(t.outbox, files[i]));
    char name[96];
    snprintf(name, sizeof name, "%020llu-%s.%s.json", (unsigned long long)now_nanos(), random_hex(4).c_str(), signal == Signal::Logs ? "logs" : "traces");
    write_file(join_path(t.outbox, name), body);
}

/** Sends what the outbox kept. Stops at the first batch that still can't be sent. */
void send_outbox(Transport &t) {
    if (t.outbox.empty()) return;
    for (const std::string &name : list_dir(t.outbox)) {
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        Signal signal = name.find(".logs.json") != std::string::npos ? Signal::Logs : Signal::Traces;
        std::string path = join_path(t.outbox, name);
        std::string body;
        if (!read_file(path, body)) continue;
        Outcome outcome = post(t, signal, body);
        if (outcome == Outcome::Retry) return;
        remove_file(path);
    }
}

void run(Transport *t) {
    auto last_outbox = std::chrono::steady_clock::now() - OUTBOX_RETRY;
    bool online = true;
    for (;;) {
        std::deque<std::string> logs, spans;
        bool stopping;
        {
            std::unique_lock<std::mutex> lock(t->mutex);
            t->wake.wait_for(lock, SEND_INTERVAL, [t] {
                return t->stopping || t->flushing || t->logs.size() >= MAX_BATCH || t->spans.size() >= MAX_BATCH;
            });
            stopping = t->stopping;
            size_t n = std::min(t->logs.size(), MAX_BATCH);
            logs.assign(t->logs.begin(), t->logs.begin() + (long)n);
            t->logs.erase(t->logs.begin(), t->logs.begin() + (long)n);
            n = std::min(t->spans.size(), MAX_BATCH);
            spans.assign(t->spans.begin(), t->spans.begin() + (long)n);
            t->spans.erase(t->spans.begin(), t->spans.begin() + (long)n);
        }

        bool sent_all = true;
        for (int s = 0; s < 2; s++) {
            Signal signal = s == 0 ? Signal::Logs : Signal::Traces;
            const std::deque<std::string> &records = s == 0 ? logs : spans;
            if (records.empty()) continue;
            std::string body = payload(signal, t->resource, records, records.size());
            Outcome outcome = post(*t, signal, body);
            if (outcome == Outcome::Retry) keep_in_outbox(*t, signal, body);
            if (outcome != Outcome::Sent) sent_all = false;
            online = outcome != Outcome::Retry;
        }

        auto now = std::chrono::steady_clock::now();
        if (online && !stopping && now - last_outbox >= OUTBOX_RETRY) {
            last_outbox = now;
            send_outbox(*t);
        }

        {
            std::lock_guard<std::mutex> lock(t->mutex);
            t->finished += logs.size() + spans.size();
            if (!sent_all) t->all_sent = false;
            if (t->logs.empty() && t->spans.empty()) t->flushing = false;
        }
        t->done_changed.notify_all();
        if (stopping) {
            std::lock_guard<std::mutex> lock(t->mutex);
            if (t->logs.empty() && t->spans.empty()) return;
        }
    }
}

}  // namespace

void transport_start(const Config &config, const std::string &resource) {
    if (g_transport) return;
    http_global_init();
    auto *t = new Transport();
    t->config = config;
    t->resource = resource;
    if (!config.database_path.empty()) {
        t->outbox = join_path(config.database_path, "outbox");
        make_dirs(t->outbox);
    }
    g_transport = t;
    t->thread = std::thread(run, t);
}

void transport_enqueue(Signal signal, std::string record) {
    Transport *t = g_transport;
    if (!t) return;
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(t->mutex);
        if (t->stopping) return;
        std::deque<std::string> &queue = signal == Signal::Logs ? t->logs : t->spans;
        if (queue.size() >= MAX_QUEUED) return;
        queue.push_back(std::move(record));
        t->queued++;
        wake = queue.size() >= MAX_BATCH;
    }
    if (wake) t->wake.notify_one();
}

bool transport_flush(int timeout_ms) {
    Transport *t = g_transport;
    if (!t) return true;
    std::unique_lock<std::mutex> lock(t->mutex);
    uint64_t target = t->queued;
    t->all_sent = true;
    t->flushing = true;
    t->wake.notify_one();
    bool done = t->done_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms),
                                         [t, target] { return t->finished >= target; });
    return done && t->all_sent;
}

void transport_stop(int timeout_ms) {
    Transport *t = g_transport;
    if (!t) return;
    transport_flush(timeout_ms);
    {
        std::lock_guard<std::mutex> lock(t->mutex);
        t->stopping = true;
        // What couldn't be sent in time waits in the outbox for the next launch.
        for (int s = 0; s < 2; s++) {
            Signal signal = s == 0 ? Signal::Logs : Signal::Traces;
            std::deque<std::string> &queue = s == 0 ? t->logs : t->spans;
            if (!queue.empty()) keep_in_outbox(*t, signal, payload(signal, t->resource, queue, queue.size()));
            queue.clear();
        }
    }
    t->wake.notify_one();
    if (t->thread.joinable()) t->thread.join();
    g_transport = nullptr;
    delete t;
}

}  // namespace zharp
