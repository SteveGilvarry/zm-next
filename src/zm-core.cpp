// zm-core.cpp - Main runner for ZoneMinder-Next plugin pipelines
#include "zm/PipelineLoader.hpp"
#include "zm/PluginManager.hpp"
#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

using namespace zm;
namespace fs = std::filesystem;

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " --pipeline <pipeline.json>\n";
    std::cout << "       or: " << prog << " --pipelines-dir <dir>\n";
}

int main(int argc, char** argv) {
    std::string pipelineFile;
    std::string pipelinesDir;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--pipeline" && i + 1 < argc) pipelineFile = argv[++i];
        else if (arg == "--pipelines-dir" && i + 1 < argc) pipelinesDir = argv[++i];
        else if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return 0; }
    }
    if (pipelineFile.empty() && pipelinesDir.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // Find pipeline file if only directory is given
    if (pipelineFile.empty() && !pipelinesDir.empty()) {
        for (const auto& entry : fs::directory_iterator(pipelinesDir)) {
            if (entry.path().extension() == ".json") {
                pipelineFile = entry.path();
                std::cout << "Using pipeline: " << pipelineFile << std::endl;
                break;
            }
        }
        if (pipelineFile.empty()) {
            std::cerr << "No pipeline JSON found in " << pipelinesDir << std::endl;
            return 2;
        }
    }

    // Determine if file is JSON or DB by extension
    bool isJson = false;
    if (pipelineFile.size() > 5 && pipelineFile.substr(pipelineFile.size()-5) == ".json")
        isJson = true;

    PipelineLoader loader(pipelineFile, isJson);
    if (!loader.load()) {
        std::cerr << "Failed to load pipeline: " << pipelineFile << std::endl;
        return 3;
    }
    loader.printProgress();

    PluginManager pm;
    // Use new API: pass vector<PluginConfig> from loader
    if (!pm.loadPipeline(loader.getPipeline())) {
        std::cerr << "Failed to load plugins for pipeline." << std::endl;
        return 4;
    }

    pm.startAll();
    std::cout << "[zm-core] Pipeline running. Press Ctrl+C to exit." << std::endl;
    // Main event loop: just sleep, plugins run in their own threads
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    pm.stopAll();
    return 0;
}
