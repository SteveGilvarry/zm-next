#include "zones.hpp"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// JSON parsing helpers
namespace {
    std::string extractJsonString(const std::string& jsonStr, const std::string& key) {
        try {
            auto j = json::parse(jsonStr);
            if (j.contains(key) && j[key].is_string()) {
                return j[key];
            }
        } catch (...) {
            // Fallback to manual parsing for malformed JSON
            auto pos = jsonStr.find("\"" + key + "\"");
            if (pos == std::string::npos) return "";
            
            pos = jsonStr.find(":", pos);
            if (pos == std::string::npos) return "";
            
            pos = jsonStr.find("\"", pos);
            if (pos == std::string::npos) return "";
            
            auto end = jsonStr.find("\"", pos + 1);
            if (end == std::string::npos) return "";
            
            return jsonStr.substr(pos + 1, end - pos - 1);
        }
        return "";
    }
    
    int extractJsonInt(const std::string& jsonStr, const std::string& key, int defaultValue = 0) {
        try {
            auto j = json::parse(jsonStr);
            if (j.contains(key) && j[key].is_number_integer()) {
                return j[key];
            }
        } catch (...) {
            // Fallback to manual parsing
            auto pos = jsonStr.find("\"" + key + "\"");
            if (pos == std::string::npos) return defaultValue;
            
            pos = jsonStr.find(":", pos);
            if (pos == std::string::npos) return defaultValue;
            
            while (pos < jsonStr.length() && (jsonStr[pos] == ':' || jsonStr[pos] == ' ' || jsonStr[pos] == '"')) pos++;
            
            return std::atoi(jsonStr.c_str() + pos);
        }
        return defaultValue;
    }
}

// ZoneMetadata Implementation
std::string ZoneMetadata::serialize() const {
    json j;
    j["monitorId"] = monitorId;
    j["frameWidth"] = frameWidth;
    j["frameHeight"] = frameHeight;
    
    json zonesJson = json::array();
    for (const auto& zone : zones) {
        json zoneJson;
        zoneJson["id"] = zone.id;
        zoneJson["name"] = zone.name;
        zoneJson["type"] = zone.type;
        zoneJson["checkMethod"] = zone.checkMethod;
        zoneJson["minPixelThreshold"] = zone.minPixelThreshold;
        zoneJson["maxPixelThreshold"] = zone.maxPixelThreshold;
        zoneJson["minAlarmPixels"] = zone.minAlarmPixels;
        zoneJson["maxAlarmPixels"] = zone.maxAlarmPixels;
        zoneJson["filterX"] = zone.filterX;
        zoneJson["filterY"] = zone.filterY;
        zoneJson["minFilterPixels"] = zone.minFilterPixels;
        zoneJson["maxFilterPixels"] = zone.maxFilterPixels;
        zoneJson["minBlobPixels"] = zone.minBlobPixels;
        zoneJson["maxBlobPixels"] = zone.maxBlobPixels;
        zoneJson["minBlobs"] = zone.minBlobs;
        zoneJson["maxBlobs"] = zone.maxBlobs;
        zoneJson["alarmRGB"] = zone.alarmRGB;
        
        // Serialize polygon coordinates
        std::string coords;
        for (const auto& point : zone.polygon.outer()) {
            if (!coords.empty()) coords += " ";
            coords += std::to_string(point.x()) + "," + std::to_string(point.y());
        }
        zoneJson["coords"] = coords;
        
        zonesJson.push_back(zoneJson);
    }
    j["zones"] = zonesJson;
    
    return j.dump();
}

ZoneMetadata ZoneMetadata::deserialize(const std::string& data) {
    ZoneMetadata metadata;
    
    try {
        auto j = json::parse(data);
        metadata.monitorId = j.value("monitorId", 0);
        metadata.frameWidth = j.value("frameWidth", 0);
        metadata.frameHeight = j.value("frameHeight", 0);
        
        if (j.contains("zones") && j["zones"].is_array()) {
            for (const auto& zoneJson : j["zones"]) {
                ZoneConfig zone;
                zone.id = zoneJson.value("id", 0);
                zone.name = zoneJson.value("name", "");
                zone.type = zoneJson.value("type", "Active");
                zone.checkMethod = zoneJson.value("checkMethod", "AlarmedPixels");
                zone.minPixelThreshold = zoneJson.value("minPixelThreshold", 25);
                zone.maxPixelThreshold = zoneJson.value("maxPixelThreshold", 255);
                zone.minAlarmPixels = zoneJson.value("minAlarmPixels", 0);
                zone.maxAlarmPixels = zoneJson.value("maxAlarmPixels", 0);
                zone.filterX = zoneJson.value("filterX", 3);
                zone.filterY = zoneJson.value("filterY", 3);
                zone.minFilterPixels = zoneJson.value("minFilterPixels", 0);
                zone.maxFilterPixels = zoneJson.value("maxFilterPixels", 0);
                zone.minBlobPixels = zoneJson.value("minBlobPixels", 0);
                zone.maxBlobPixels = zoneJson.value("maxBlobPixels", 0);
                zone.minBlobs = zoneJson.value("minBlobs", 1);
                zone.maxBlobs = zoneJson.value("maxBlobs", 0);
                zone.alarmRGB = zoneJson.value("alarmRGB", 0xFF0000);
                
                // Parse polygon coordinates
                std::string coords = zoneJson.value("coords", "");
                zone.polygon = ZoneManager::parseCoords(coords);
                
                metadata.zones.push_back(zone);
            }
        }
    } catch (const std::exception& e) {
        // If JSON parsing fails, return empty metadata
        metadata.zones.clear();
    }
    
    return metadata;
}

// ZoneManager Implementation
void ZoneManager::loadZones(const std::string& jsonConfig) {
    zones_.clear();
    spatialIndex_.clear();
    
    try {
        auto j = json::parse(jsonConfig);
        if (j.contains("zones") && j["zones"].is_array()) {
            for (const auto& zoneJson : j["zones"]) {
                ZoneConfig zone = parseZoneMinder(zoneJson.dump());
                addZone(zone);
            }
        }
    } catch (const std::exception& e) {
        // Fallback to manual parsing for ZoneMinder format
        size_t pos = 0;
        while ((pos = jsonConfig.find("{", pos)) != std::string::npos) {
            size_t end = jsonConfig.find("}", pos);
            if (end == std::string::npos) break;
            
            std::string zoneJson = jsonConfig.substr(pos, end - pos + 1);
            ZoneConfig zone = parseZoneMinder(zoneJson);
            if (zone.id > 0) {
                addZone(zone);
            }
            
            pos = end + 1;
        }
    }
}

void ZoneManager::addZone(const ZoneConfig& zone) {
    zones_.push_back(zone);
    
    // Add to spatial index
    Box boundingBox;
    bg::envelope(zone.polygon, boundingBox);
    spatialIndex_.insert(std::make_pair(boundingBox, zone.id));
}

void ZoneManager::removeZone(int zoneId) {
    auto it = std::remove_if(zones_.begin(), zones_.end(),
                            [zoneId](const ZoneConfig& z) { return z.id == zoneId; });
    if (it != zones_.end()) {
        zones_.erase(it, zones_.end());
        rebuildSpatialIndex();
    }
}

void ZoneManager::updateZone(const ZoneConfig& zone) {
    removeZone(zone.id);
    addZone(zone);
}

std::vector<int> ZoneManager::getZonesAtPoint(double x, double y) const {
    std::vector<int> result;
    Point point(x, y);
    
    std::vector<std::pair<Box, int>> candidates;
    spatialIndex_.query(bgi::intersects(point), std::back_inserter(candidates));
    
    for (const auto& candidate : candidates) {
        auto it = std::find_if(zones_.begin(), zones_.end(),
                              [&](const ZoneConfig& z) { return z.id == candidate.second; });
        if (it != zones_.end() && bg::within(point, it->polygon)) {
            result.push_back(candidate.second);
        }
    }
    
    return result;
}

ZoneConfig* ZoneManager::findZone(int zoneId) {
    auto it = std::find_if(zones_.begin(), zones_.end(),
                          [zoneId](const ZoneConfig& z) { return z.id == zoneId; });
    return (it != zones_.end()) ? &(*it) : nullptr;
}

std::vector<ZoneConfig> ZoneManager::getActiveZones() const {
    return getZonesByType("Active");
}

std::vector<ZoneConfig> ZoneManager::getZonesByType(const std::string& type) const {
    std::vector<ZoneConfig> result;
    std::copy_if(zones_.begin(), zones_.end(), std::back_inserter(result),
                [&type](const ZoneConfig& z) { return z.type == type; });
    return result;
}

ZoneConfig ZoneManager::parseZoneMinder(const std::string& jsonStr) {
    ZoneConfig zone;
    
    zone.id = extractJsonInt(jsonStr, "Id");
    zone.name = extractJsonString(jsonStr, "Name");
    zone.type = extractJsonString(jsonStr, "Type");
    zone.checkMethod = extractJsonString(jsonStr, "CheckMethod");
    
    zone.minPixelThreshold = extractJsonInt(jsonStr, "MinPixelThreshold", 25);
    zone.maxPixelThreshold = extractJsonInt(jsonStr, "MaxPixelThreshold", 255);
    zone.minAlarmPixels = extractJsonInt(jsonStr, "MinAlarmPixels", 0);
    zone.maxAlarmPixels = extractJsonInt(jsonStr, "MaxAlarmPixels", 0);
    zone.filterX = extractJsonInt(jsonStr, "FilterX", 3);
    zone.filterY = extractJsonInt(jsonStr, "FilterY", 3);
    zone.minFilterPixels = extractJsonInt(jsonStr, "MinFilterPixels", 0);
    zone.maxFilterPixels = extractJsonInt(jsonStr, "MaxFilterPixels", 0);
    zone.minBlobPixels = extractJsonInt(jsonStr, "MinBlobPixels", 0);
    zone.maxBlobPixels = extractJsonInt(jsonStr, "MaxBlobPixels", 0);
    zone.minBlobs = extractJsonInt(jsonStr, "MinBlobs", 1);
    zone.maxBlobs = extractJsonInt(jsonStr, "MaxBlobs", 0);
    zone.alarmRGB = extractJsonInt(jsonStr, "AlarmRGB", 0xFF0000);
    
    std::string coords = extractJsonString(jsonStr, "Coords");
    zone.polygon = parseCoords(coords);
    
    return zone;
}

Polygon ZoneManager::parseCoords(const std::string& coords) {
    Polygon poly;
    std::istringstream ss(coords);
    std::string token;
    
    while (std::getline(ss, token, ' ')) {
        if (token.empty()) continue;
        
        auto commaPos = token.find(',');
        if (commaPos != std::string::npos) {
            double x = std::stod(token.substr(0, commaPos));
            double y = std::stod(token.substr(commaPos + 1));
            bg::append(poly.outer(), Point(x, y));
        }
    }
    
    // Ensure polygon is closed
    if (!poly.outer().empty()) {
        const auto& first = poly.outer().front();
        const auto& last = poly.outer().back();
        if (first.x() != last.x() || first.y() != last.y()) {
            poly.outer().push_back(poly.outer().front());
        }
    }
    
    return poly;
}

void ZoneManager::rebuildSpatialIndex() {
    spatialIndex_.clear();
    for (const auto& zone : zones_) {
        Box boundingBox;
        bg::envelope(zone.polygon, boundingBox);
        spatialIndex_.insert(std::make_pair(boundingBox, zone.id));
    }
}

// Plugin context
struct ZonesCtx {
    ZoneManager zoneManager;
    int monitorId = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
};

extern "C" {

static int zones_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto* ctx = new ZonesCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    
    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->monitorId = j.value("monitor_id", 0);
            ctx->frameWidth = j.value("frame_width", 0);
            ctx->frameHeight = j.value("frame_height", 0);
            
            // Load zones configuration
            if (j.contains("zones")) {
                ctx->zoneManager.loadZones(j["zones"].dump());
            }
        } catch (const std::exception& e) {
            if (host && host->log) {
                host->log(host_ctx, ZM_LOG_ERROR, ("Failed to parse zones config: " + std::string(e.what())).c_str());
            }
        }
    }
    
    plugin->instance = ctx;
    
    if (host && host->log) {
        std::string msg = "Zones plugin started with " + std::to_string(ctx->zoneManager.getZones().size()) + " zones";
        host->log(host_ctx, ZM_LOG_INFO, msg.c_str());
    }
    
    return 0;
}

static void zones_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<ZonesCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

static void zones_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<ZonesCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) return;
    
    const zm_frame_hdr_t* header = static_cast<const zm_frame_hdr_t*>(buf);
    
    // Update frame dimensions if they've changed
    if (ctx->frameWidth == 0 || ctx->frameHeight == 0) {
        // Extract dimensions from header (adjust based on actual zm_frame_hdr_t structure)
        ctx->frameWidth = header->stream_id;   // Placeholder - adjust as needed
        ctx->frameHeight = header->flags;      // Placeholder - adjust as needed
        ctx->monitorId = header->stream_id;
    }
    
    // Create zone metadata
    ZoneMetadata metadata;
    metadata.monitorId = ctx->monitorId;
    metadata.frameWidth = ctx->frameWidth;
    metadata.frameHeight = ctx->frameHeight;
    metadata.zones = ctx->zoneManager.getZones();
    
    // Serialize metadata and attach to frame
    std::string metadataStr = metadata.serialize();
    
    // For now, just pass the frame through
    // In a more advanced implementation, we could modify the frame header
    // to include zone metadata or use a custom frame format
    
    // Forward frame to next plugin in pipeline
    if (ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
    
    // Optional: Log zone statistics
    if (ctx->host && ctx->host->log) {
        static int frameCount = 0;
        if (++frameCount % 1000 == 0) {  // Log every 1000 frames
            std::string msg = "Processed " + std::to_string(frameCount) + 
                            " frames with " + std::to_string(metadata.zones.size()) + " zones";
            ctx->host->log(ctx->hostCtx, ZM_LOG_DEBUG, msg.c_str());
        }
    }
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_PROCESS;  // Process plugin that adds metadata
    plugin->start = zones_start;
    plugin->stop = zones_stop;
    plugin->on_frame = zones_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"
