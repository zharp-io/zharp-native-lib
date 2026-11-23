#include <stdio.h>
#include <zharp.h>

int main(void) {
    zharp_init(NULL);
    zharp_capture_error("ConsumerError", "from C");
    zharp_shutdown();
    printf("C consumer ran (%s)\n", ZHARP_SDK_VERSION);
    return 0;
}
