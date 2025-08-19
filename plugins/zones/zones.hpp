#pragma once

#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <boost/geometry.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>
#include <boost/geometry/index/rtree.hpp>
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

// Zone metadata structure that gets attached to frames
struct ZoneMetadata {
    int monitorId;
    int frameWidth;
    int frameHeight;
    std::vector<ZoneConfig> zones;
    
    // Serialization helpers
    std::string serialize() const;
    static ZoneMetadata deserialize(const std::string& data);
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
    ZoneConfig* findZone(int zoneId);
    
    // Query methods
    std::vector<ZoneConfig> getActiveZones() const;
    std::vector<ZoneConfig> getZonesByType(const std::string& type) const;
    
    // Migration helpers for ZoneMinder format
    static ZoneConfig parseZoneMinder(const std::string& json);
    static Polygon parseCoords(const std::string& coords);
    
private:
    void rebuildSpatialIndex();
};
