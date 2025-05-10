// Stub implementation for EventBus
#include <iostream>

namespace zm {

class EventBus {
public:
    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    void publish(const std::string& msg) {
        std::cout << "EventBus publish: " << msg << std::endl;
    }

};

} // namespace zm
