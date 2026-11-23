/* Zharp from plain C: an error you handled, a log line and a span. */
#include <stdio.h>
#include <zharp.h>

static int parse_config(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        zharp_capture_error("ConfigError", "config file missing");
        return -1;
    }
    fclose(file);
    return 0;
}

int main(void) {
    zharp_options_t options;
    zharp_options_init(&options);
    options.sessions = 0; /* a service: no sessions or device ID */
    zharp_init(&options);

    zharp_span_t *span = zharp_start_span("startup");
    zharp_log(ZHARP_LEVEL_INFO, "starting");
    parse_config("/nonexistent/app.conf");
    zharp_end_span(span);

    zharp_shutdown();
    return 0;
}
