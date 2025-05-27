#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>

using json = nlohmann::json;

// Test JSON configuration parsing - this tests the logic we use in the plugin
class WebRTCConfigTest : public ::testing::Test {
protected:
    // Helper function to parse WebRTC configuration like the plugin does
    bool parse_webrtc_config(const std::string& config_str, json& config) {
        try {
            config = json::parse(config_str);
            return true;
        } catch (const json::parse_error& e) {
            return false;
        }
    }
    
    // Helper to validate configuration structure
    bool validate_config(const json& config) {
        // Check for required and optional fields like the plugin does
        if (!config.contains("bind_address") || !config["bind_address"].is_string()) {
            return false;
        }
        if (!config.contains("port") || !config["port"].is_number_integer()) {
            return false;
        }
        return true;
    }
};

// Test 1: Basic configuration parsing
TEST_F(WebRTCConfigTest, ParseValidConfig) {
    std::string config_str = R"({
        "bind_address": "0.0.0.0",
        "port": 8080,
        "max_clients": 10,
        "ice_servers": [
            {"urls": ["stun:stun.l.google.com:19302"]}
        ]
    })";
    
    json config;
    EXPECT_TRUE(parse_webrtc_config(config_str, config));
    EXPECT_TRUE(validate_config(config));
    EXPECT_EQ(config["bind_address"], "0.0.0.0");
    EXPECT_EQ(config["port"], 8080);
    EXPECT_EQ(config["max_clients"], 10);
}

// Test 2: Default configuration
TEST_F(WebRTCConfigTest, DefaultConfig) {
    std::string config_str = R"({
        "bind_address": "127.0.0.1",
        "port": 8080
    })";
    
    json config;
    EXPECT_TRUE(parse_webrtc_config(config_str, config));
    EXPECT_TRUE(validate_config(config));
}

// Test 3: Stream filter configuration
TEST_F(WebRTCConfigTest, StreamFilterConfig) {
    std::string config_str = R"({
        "bind_address": "0.0.0.0",
        "port": 8080,
        "stream_filter": ["stream1", "stream2", "cam1"]
    })";
    
    json config;
    EXPECT_TRUE(parse_webrtc_config(config_str, config));
    EXPECT_TRUE(validate_config(config));
    EXPECT_TRUE(config.contains("stream_filter"));
    EXPECT_TRUE(config["stream_filter"].is_array());
    EXPECT_EQ(config["stream_filter"].size(), 3);
}

// Test 4: ICE servers configuration
TEST_F(WebRTCConfigTest, IceServersConfig) {
    std::string config_str = R"({
        "bind_address": "127.0.0.1",
        "port": 8080,
        "ice_servers": [
            {
                "urls": ["stun:stun1.l.google.com:19302", "stun:stun2.l.google.com:19302"]
            },
            {
                "urls": ["turn:my-turn-server.com:3478"],
                "username": "user",
                "credential": "pass"
            }
        ]
    })";
    
    json config;
    EXPECT_TRUE(parse_webrtc_config(config_str, config));
    EXPECT_TRUE(validate_config(config));
    EXPECT_TRUE(config.contains("ice_servers"));
    EXPECT_EQ(config["ice_servers"].size(), 2);
}

// Test 5: Invalid JSON
TEST_F(WebRTCConfigTest, InvalidJson) {
    std::string config_str = R"({
        "bind_address": "127.0.0.1",
        "port": 8080,
        "invalid": 
    })";
    
    json config;
    EXPECT_FALSE(parse_webrtc_config(config_str, config));
}

// Test 6: Missing required fields
TEST_F(WebRTCConfigTest, MissingRequiredFields) {
    std::string config_str = R"({
        "max_clients": 5
    })";
    
    json config;
    EXPECT_TRUE(parse_webrtc_config(config_str, config));
    EXPECT_FALSE(validate_config(config)); // Should fail validation
}

// Test 7: Metadata frame processing logic
TEST_F(WebRTCConfigTest, MetadataFrameProcessing) {
    // Test metadata JSON that would be received by the plugin
    std::string metadata_str = R"({
        "stream_id": "camera1",
        "codec_id": 27,
        "width": 1920,
        "height": 1080,
        "pix_fmt": 0,
        "profile": 100,
        "level": 40,
        "extradata": "Z2QAKKwFALT8BBAAABNIAAgAB2qcIGE="
    })";
    
    json metadata;
    EXPECT_TRUE(parse_webrtc_config(metadata_str, metadata));
    EXPECT_TRUE(metadata.contains("stream_id"));
    EXPECT_TRUE(metadata.contains("codec_id"));
    EXPECT_TRUE(metadata.contains("extradata"));
    EXPECT_EQ(metadata["stream_id"], "camera1");
    EXPECT_EQ(metadata["codec_id"], 27); // H.264
}

// Test 8: Stream filtering logic
TEST_F(WebRTCConfigTest, StreamFilterLogic) {
    json config = json::parse(R"({
        "bind_address": "127.0.0.1",
        "port": 8080,
        "stream_filter": ["camera1", "camera3"]
    })");
    
    // Test stream filtering logic like the plugin does
    auto should_process_stream = [&](const std::string& stream_id) -> bool {
        if (!config.contains("stream_filter")) {
            return true; // No filter means process all streams
        }
        
        auto filter = config["stream_filter"];
        if (!filter.is_array()) {
            return true;
        }
        
        for (const auto& allowed : filter) {
            if (allowed.is_string() && allowed.get<std::string>() == stream_id) {
                return true;
            }
        }
        return false;
    };
    
    EXPECT_TRUE(should_process_stream("camera1"));   // In filter
    EXPECT_FALSE(should_process_stream("camera2"));  // Not in filter
    EXPECT_TRUE(should_process_stream("camera3"));   // In filter
    EXPECT_FALSE(should_process_stream("camera4"));  // Not in filter
}
