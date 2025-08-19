#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <xsimd/xsimd.hpp>
#include <zm_plugin.h>

namespace bg = boost::geometry;
namespace bgi = boost::geometry::index;

// Geometry types
using Point = bg::model::d2::point_xy<double>;
using Polygon = bg::model::polygon<Point>;
using Box = bg::model::box<Point>;

// Zone configuration matching ZoneMinder's schema
struct ZoneConfig {
    int id;
    std::string name;
    std::string type;  // "Active", "Inactive", "Preclusive", "Inclusive", "Exclusive"
    Polygon polygon;
    
    // Detection parameters
    std::string checkMethod;  // "AlarmedPixels", "Blobs", "FilteredPixels"
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
    
    // Color for visualization
    uint32_t alarmRGB = 0xFF0000;  // Red by default
};

// Motion detection result for a zone
struct ZoneMotionResult {
    int zoneId;
    bool motionDetected;
    int pixelCount;
    int blobCount;
    std::vector<Box> blobs;  // Bounding boxes of detected blobs
};

// Global motion detection result
struct MotionResult {
    bool hasMotion;
    int totalPixels;
    std::vector<ZoneMotionResult> zoneResults;
    std::vector<uint8_t> motionMap;  // Optional: for visualization/debugging
};

// Abstract base class for motion detection algorithms
class MotionDetectionAlgorithm {
public:
    virtual ~MotionDetectionAlgorithm() = default;
    virtual bool initialize(int width, int height, const std::string& config) = 0;
    virtual MotionResult detectMotion(const uint8_t* frame, const std::vector<ZoneConfig>& zones) = 0;
    virtual void reset() = 0;
};

// SIMD-optimized pixel difference algorithm
class PixelDifferenceDetector : public MotionDetectionAlgorithm {
private:
    int width_, height_;
    int threshold_;
    alignas(64) std::vector<uint8_t> background_;
    bool backgroundReady_;
    
public:
    bool initialize(int width, int height, const std::string& config) override;
    MotionResult detectMotion(const uint8_t* frame, const std::vector<ZoneConfig>& zones) override;
    void reset() override;
    
private:
    int detectMotionInZone(const uint8_t* frame, const uint8_t* motionMap, 
                          const ZoneConfig& zone) const;
    std::vector<Box> findBlobs(const uint8_t* motionMap, const ZoneConfig& zone) const;
    void updateBackground(const uint8_t* frame);
};

// Zone manager with spatial indexing
class ZoneManager {
private:
    std::vector<ZoneConfig> zones_;
    bgi::rtree<std::pair<Box, int>, bgi::quadratic<16>> spatialIndex_;
    
public:
    void loadZones(const std::string& jsonConfig);
    void addZone(const ZoneConfig& zone);
    void removeZone(int zoneId);
    void updateZone(const ZoneConfig& zone);
    
    const std::vector<ZoneConfig>& getZones() const { return zones_; }
    std::vector<int> getZonesAtPoint(double x, double y) const;
    
    // Migration helpers for ZoneMinder format
    static ZoneConfig parseZoneMinder(const std::string& json);
    static Polygon parseCoords(const std::string& coords);
};

// Main hybrid motion detector
class HybridMotionDetector {
private:
    std::unique_ptr<MotionDetectionAlgorithm> algorithm_;
    ZoneManager zoneManager_;
    zm_host_api_t* host_;
    void* hostCtx_;
    
    int width_, height_;
    bool initialized_;
    
public:
    HybridMotionDetector();
    ~HybridMotionDetector();
    
    bool initialize(zm_host_api_t* host, void* hostCtx, const std::string& config);
    void processFrame(const zm_frame_hdr_t* header, const uint8_t* frameData);
    void shutdown();
    
private:
    void publishMotionEvents(const MotionResult& result, int monitorId);
    std::string createAlgorithm(const std::string& algorithmType);
};
