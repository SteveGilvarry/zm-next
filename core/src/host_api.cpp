#include <iostream>
#include "zm_plugin.h"

extern "C" void host_log(const char* msg) {
    std::cout << msg << std::endl;
}

extern "C" void publish_event(const char* json) {
    std::cout << "Event: " << json << std::endl;
}
