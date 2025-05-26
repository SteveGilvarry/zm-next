#include <iostream>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    // Test 1: Multi-stream configuration parsing
    std::cout << "Testing multi-stream pipeline configuration..." << std::endl;
    
    std::ifstream multi_file("/Users/stevengilvarry/Code/zm-next/pipelines/multi_rtsp_to_filesystem.json");
    if (multi_file.is_open()) {
        json multi_config;
        multi_file >> multi_config;
        
        auto outputs = multi_config["pipeline"]["output"];
        std::cout << "Found " << outputs.size() << " output plugins" << std::endl;
        
        for (size_t i = 0; i < outputs.size(); i++) {
            auto config = outputs[i]["config"];
            if (config.contains("stream_filter")) {
                auto filter = config["stream_filter"];
                std::cout << "Output " << i << " filters streams: ";
                for (auto stream_id : filter) {
                    std::cout << stream_id << " ";
                }
                std::cout << std::endl;
            } else {
                std::cout << "Output " << i << " has no stream filter (accepts all)" << std::endl;
            }
        }
    }
    
    // Test 2: Single-stream configuration parsing  
    std::cout << "\nTesting single-stream pipeline configuration..." << std::endl;
    
    std::ifstream single_file("/Users/stevengilvarry/Code/zm-next/pipelines/cap_then_store.json");
    if (single_file.is_open()) {
        json single_config;
        single_file >> single_config;
        
        auto plugins = single_config["plugins"];
        for (auto& plugin : plugins) {
            if (plugin.contains("children")) {
                for (auto& child : plugin["children"]) {
                    if (child["kind"] == "store_filesystem") {
                        auto config = child["cfg"];
                        if (config.contains("stream_filter")) {
                            std::cout << "Store plugin has stream_filter" << std::endl;
                        } else {
                            std::cout << "Store plugin has no stream_filter (backward compatible)" << std::endl;
                        }
                    }
                }
            }
        }
    }
    
    std::cout << "\nConfiguration validation complete!" << std::endl;
    return 0;
}
