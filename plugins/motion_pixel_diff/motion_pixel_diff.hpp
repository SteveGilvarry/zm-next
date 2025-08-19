#pragma once

#include <vector>
#include <memory>
#include <string>
#include <xsimd/xsimd.hpp>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <zm_plugin.h>

namespace bg = boost::geometry;
using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;
using Box = bg::model::box<Point>;

// Forward declaration for zone structures (matches zones plugin)
struct ZoneConfig {
    int id;
    std::string name;
    std::string type;
    Polygon polygon;
    std::string checkMethod;
    int minPixelThreshold = 25;
    int maxPixelThreshold = 255;
    int minAlarmPixels = 0;
    int maxAlarmPixels = 0;
    int filterX = 3;
    int filterY = 3;
    int minFilterPixels = 0;
    int maxFilterPixels = 0;
    int minBlobPixels = 0;
    int maxBlobPixels = 0;
    int minBlobs = 1;
    int maxBlobs = 0;
    uint32_t alarmRGB = 0xFF0000;
};

struct ZoneMetadata {
    int monitorId;
    int frameWidth;
    int frameHeight;
    std::vector<ZoneConfig> zones;
    static ZoneMetadata deserialize(const std::string& data);
};

// Motion detection configuration
struct MotionConfig {
    int frameWidth = 1280;             // Expected frame width
    int frameHeight = 720;             // Expected frame height
    int globalThreshold = 25;          // Global motion threshold
    int minPixels = 800;               // Minimum pixels for global motion
    int downscale = 1;                 // 1=orig, 2=half, 0=custom
    int outWidth = 0, outHeight = 0;   // Custom output dimensions
    bool zoneAware = true;             // Use zone-specific thresholds
    bool enableBackgroundUpdate = true; // Update background model
    float backgroundLearningRate = 0.03125f; // 1/32
    bool enableMotionMap = false;      // Generate motion visualization
    bool enableFiltering = true;       // Apply spatial filtering
};

// Motion detection result for a zone
struct ZoneMotionResult {
    int zoneId;
    bool motionDetected;
    int pixelCount;
    int blobCount;
    std::vector<Box> blobs;
};

// Global motion detection result
struct MotionResult {
    bool hasGlobalMotion;
    int totalMotionPixels;
    std::vector<ZoneMotionResult> zoneResults;
    std::vector<uint8_t> motionMap;  // Optional visualization data
};

// SIMD-optimized pixel difference motion detector
class PixelDifferenceDetector {
private:
    int width_, height_;
    MotionConfig config_;
    alignas(64) std::vector<uint8_t> background_;
    bool backgroundReady_;
    
    // Performance counters
    size_t frameCount_;
    
public:
    PixelDifferenceDetector();
    ~PixelDifferenceDetector() = default;
    
    bool initialize(int width, int height, const MotionConfig& config);
    MotionResult detectMotion(const uint8_t* frame, const std::vector<ZoneConfig>& zones = {});
    void reset();
    
    // Configuration updates
    void updateConfig(const MotionConfig& config) { config_ = config; }
    const MotionConfig& getConfig() const { return config_; }
    
    // Statistics
    size_t getFrameCount() const { return frameCount_; }
    bool isBackgroundReady() const { return backgroundReady_; }
    
private:
    // Core motion detection
    std::vector<uint8_t> generateMotionMap(const uint8_t* frame);
    void updateBackground(const uint8_t* frame);
    
    // Zone processing
    int detectMotionInZone(const std::vector<uint8_t>& motionMap, const ZoneConfig& zone);
    std::vector<Box> findBlobs(const std::vector<uint8_t>& motionMap, const ZoneConfig& zone);
    
    // Filtering and post-processing
    void applyNoiseFilter(std::vector<uint8_t>& motionMap, int filterX, int filterY);
    
    // Utility functions
    bool isPointInZone(double x, double y, const ZoneConfig& zone) const;
    int countMotionPixelsInBounds(const std::vector<uint8_t>& motionMap, 
                                 int minX, int maxX, int minY, int maxY) const;
};
