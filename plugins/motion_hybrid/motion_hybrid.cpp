#include "motion_hybrid.hpp"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

// JSON parsing helpers (simple implementation)
namespace {
    std::string extractJsonString(const std::string& json, const std::string& key) {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        
        pos = json.find(":", pos);
        if (pos == std::string::npos) return "";
        
        pos = json.find("\"", pos);
        if (pos == std::string::npos) return "";
        
        auto end = json.find("\"", pos + 1);
        if (end == std::string::npos) return "";
        
        return json.substr(pos + 1, end - pos - 1);
    }
    
    int extractJsonInt(const std::string& json, const std::string& key, int defaultValue = 0) {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return defaultValue;
        
        pos = json.find(":", pos);
        if (pos == std::string::npos) return defaultValue;
        
        // Skip whitespace and quotes
        while (pos < json.length() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '"')) pos++;
        
        return std::atoi(json.c_str() + pos);
    }
}

// PixelDifferenceDetector Implementation
bool PixelDifferenceDetector::initialize(int width, int height, const std::string& config) {
    width_ = width;
    height_ = height;
    threshold_ = extractJsonInt(config, "threshold", 25);
    
    size_t frameSize = width_ * height_;
    background_.assign(frameSize, 0);
    backgroundReady_ = false;
    
    return true;
}

MotionResult PixelDifferenceDetector::detectMotion(const uint8_t* frame, const std::vector<ZoneConfig>& zones) {
    MotionResult result;
    result.hasMotion = false;
    result.totalPixels = 0;
    
    size_t frameSize = width_ * height_;
    std::vector<uint8_t> motionMap(frameSize, 0);
    
    if (!backgroundReady_) {
        std::memcpy(background_.data(), frame, frameSize);
        backgroundReady_ = true;
        return result;
    }
    
    // SIMD-optimized motion detection
    using batch_t = xsimd::batch<uint8_t>;
    constexpr auto VL = batch_t::size;
    
    size_t i = 0;
    int totalMotionPixels = 0;
    const uint8_t* bg = background_.data();
    batch_t threshold_vec(threshold_);
    
    // Vectorized comparison
    for (; i + VL <= frameSize; i += VL) {
        batch_t frame_vec = batch_t::load_unaligned(frame + i);
        batch_t bg_vec = batch_t::load_unaligned(bg + i);
        batch_t diff = xsimd::abs(frame_vec - bg_vec);
        auto mask = xsimd::gt(diff, threshold_vec);
        
        // Store motion map
        batch_t motion_result = xsimd::select(mask, batch_t(255), batch_t(0));
        motion_result.store_unaligned(motionMap.data() + i);
        
        // Count motion pixels
        totalMotionPixels += xsimd::reduce_add(xsimd::select(mask, batch_t(1), batch_t(0)));
    }
    
    // Handle remaining pixels
    for (; i < frameSize; ++i) {
        if (std::abs(int(frame[i]) - int(bg[i])) > threshold_) {
            motionMap[i] = 255;
            totalMotionPixels++;
        }
    }
    
    result.totalPixels = totalMotionPixels;
    result.motionMap = std::move(motionMap);
    
    // Process each zone
    for (const auto& zone : zones) {
        if (zone.type != "Active") continue;
        
        ZoneMotionResult zoneResult;
        zoneResult.zoneId = zone.id;
        zoneResult.pixelCount = detectMotionInZone(frame, result.motionMap.data(), zone);
        
        // Apply zone-specific thresholds
        bool hasMinPixels = (zone.minAlarmPixels == 0) || (zoneResult.pixelCount >= zone.minAlarmPixels);
        bool belowMaxPixels = (zone.maxAlarmPixels == 0) || (zoneResult.pixelCount <= zone.maxAlarmPixels);
        
        zoneResult.motionDetected = hasMinPixels && belowMaxPixels;
        
        if (zoneResult.motionDetected && zone.checkMethod == "Blobs") {
            zoneResult.blobs = findBlobs(result.motionMap.data(), zone);
            zoneResult.blobCount = zoneResult.blobs.size();
            
            // Apply blob constraints
            bool hasMinBlobs = (zone.minBlobs == 0) || (zoneResult.blobCount >= zone.minBlobs);
            bool belowMaxBlobs = (zone.maxBlobs == 0) || (zoneResult.blobCount <= zone.maxBlobs);
            
            zoneResult.motionDetected = hasMinBlobs && belowMaxBlobs;
        }
        
        if (zoneResult.motionDetected) {
            result.hasMotion = true;
        }
        
        result.zoneResults.push_back(zoneResult);
    }
    
    // Update background
    updateBackground(frame);
    
    return result;
}

int PixelDifferenceDetector::detectMotionInZone(const uint8_t* frame, const uint8_t* motionMap, 
                                               const ZoneConfig& zone) const {
    int count = 0;
    
    // Get bounding box of zone for efficiency
    Box boundingBox;
    bg::envelope(zone.polygon, boundingBox);
    
    int minX = std::max(0, (int)boundingBox.min_corner().x());
    int maxX = std::min(width_ - 1, (int)boundingBox.max_corner().x());
    int minY = std::max(0, (int)boundingBox.min_corner().y());
    int maxY = std::min(height_ - 1, (int)boundingBox.max_corner().y());
    
    // Check each pixel in bounding box
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            Point point(x, y);
            if (bg::within(point, zone.polygon)) {
                int idx = y * width_ + x;
                if (motionMap[idx] > 0) {
                    count++;
                }
            }
        }
    }
    
    return count;
}

std::vector<Box> PixelDifferenceDetector::findBlobs(const uint8_t* motionMap, const ZoneConfig& zone) const {
    // Simplified blob detection - in practice, you'd use connected components
    std::vector<Box> blobs;
    
    // Get zone bounding box
    Box zoneBounds;
    bg::envelope(zone.polygon, zoneBounds);
    
    int minX = std::max(0, (int)zoneBounds.min_corner().x());
    int maxX = std::min(width_ - 1, (int)zoneBounds.max_corner().x());
    int minY = std::max(0, (int)zoneBounds.min_corner().y());
    int maxY = std::min(height_ - 1, (int)zoneBounds.max_corner().y());
    
    // Simple blob detection using a grid approach
    const int gridSize = 8;
    for (int gy = minY; gy < maxY; gy += gridSize) {
        for (int gx = minX; gx < maxX; gx += gridSize) {
            int motionCount = 0;
            int blobMinX = width_, blobMaxX = 0;
            int blobMinY = height_, blobMaxY = 0;
            
            // Check grid cell
            for (int y = gy; y < std::min(gy + gridSize, maxY); ++y) {
                for (int x = gx; x < std::min(gx + gridSize, maxX); ++x) {
                    Point point(x, y);
                    if (bg::within(point, zone.polygon)) {
                        int idx = y * width_ + x;
                        if (motionMap[idx] > 0) {
                            motionCount++;
                            blobMinX = std::min(blobMinX, x);
                            blobMaxX = std::max(blobMaxX, x);
                            blobMinY = std::min(blobMinY, y);
                            blobMaxY = std::max(blobMaxY, y);
                        }
                    }
                }
            }
            
            // If enough motion pixels, create a blob
            if (motionCount >= zone.minBlobPixels && 
                (zone.maxBlobPixels == 0 || motionCount <= zone.maxBlobPixels)) {
                Box blob(Point(blobMinX, blobMinY), Point(blobMaxX, blobMaxY));
                blobs.push_back(blob);
            }
        }
    }
    
    return blobs;
}

void PixelDifferenceDetector::updateBackground(const uint8_t* frame) {
    size_t frameSize = width_ * height_;
    uint8_t* bg = background_.data();
    
    using batch_t = xsimd::batch<uint8_t>;
    constexpr auto VL = batch_t::size;
    
    size_t i = 0;
    batch_t alpha(31);
    batch_t div(32);
    
    // Vectorized background update: bg = (bg * 31 + frame) / 32
    for (; i + VL <= frameSize; i += VL) {
        batch_t frame_vec = batch_t::load_unaligned(frame + i);
        batch_t bg_vec = batch_t::load_unaligned(bg + i);
        batch_t updated = (bg_vec * alpha + frame_vec) / div;
        updated.store_unaligned(bg + i);
    }
    
    // Handle remaining pixels
    for (; i < frameSize; ++i) {
        bg[i] = (uint8_t)(((uint16_t)bg[i] * 31 + frame[i]) / 32);
    }
}

void PixelDifferenceDetector::reset() {
    backgroundReady_ = false;
    std::fill(background_.begin(), background_.end(), 0);
}

// ZoneManager Implementation
void ZoneManager::loadZones(const std::string& jsonConfig) {
    zones_.clear();
    spatialIndex_.clear();
    
    // Parse JSON array of zones (simplified parsing)
    // In production, use a proper JSON library
    size_t pos = 0;
    while ((pos = jsonConfig.find("{", pos)) != std::string::npos) {
        size_t end = jsonConfig.find("}", pos);
        if (end == std::string::npos) break;
        
        std::string zoneJson = jsonConfig.substr(pos, end - pos + 1);
        ZoneConfig zone = parseZoneMinder(zoneJson);
        addZone(zone);
        
        pos = end + 1;
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
        
        // Rebuild spatial index
        spatialIndex_.clear();
        for (const auto& zone : zones_) {
            Box boundingBox;
            bg::envelope(zone.polygon, boundingBox);
            spatialIndex_.insert(std::make_pair(boundingBox, zone.id));
        }
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

ZoneConfig ZoneManager::parseZoneMinder(const std::string& json) {
    ZoneConfig zone;
    
    zone.id = extractJsonInt(json, "Id");
    zone.name = extractJsonString(json, "Name");
    zone.type = extractJsonString(json, "Type");
    zone.checkMethod = extractJsonString(json, "CheckMethod");
    
    zone.minPixelThreshold = extractJsonInt(json, "MinPixelThreshold", 25);
    zone.maxPixelThreshold = extractJsonInt(json, "MaxPixelThreshold", 255);
    zone.minAlarmPixels = extractJsonInt(json, "MinAlarmPixels", 0);
    zone.maxAlarmPixels = extractJsonInt(json, "MaxAlarmPixels", 0);
    zone.filterX = extractJsonInt(json, "FilterX", 3);
    zone.filterY = extractJsonInt(json, "FilterY", 3);
    zone.minFilterPixels = extractJsonInt(json, "MinFilterPixels", 0);
    zone.maxFilterPixels = extractJsonInt(json, "MaxFilterPixels", 0);
    zone.minBlobPixels = extractJsonInt(json, "MinBlobPixels", 0);
    zone.maxBlobPixels = extractJsonInt(json, "MaxBlobPixels", 0);
    zone.minBlobs = extractJsonInt(json, "MinBlobs", 1);
    zone.maxBlobs = extractJsonInt(json, "MaxBlobs", 0);
    zone.alarmRGB = extractJsonInt(json, "AlarmRGB", 0xFF0000);
    
    std::string coords = extractJsonString(json, "Coords");
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

// HybridMotionDetector Implementation
HybridMotionDetector::HybridMotionDetector() 
    : host_(nullptr), hostCtx_(nullptr), width_(0), height_(0), initialized_(false) {
}

HybridMotionDetector::~HybridMotionDetector() {
    shutdown();
}

bool HybridMotionDetector::initialize(zm_host_api_t* host, void* hostCtx, const std::string& config) {
    host_ = host;
    hostCtx_ = hostCtx;
    
    // Parse config to determine algorithm type
    std::string algorithmType = extractJsonString(config, "algorithm");
    if (algorithmType.empty()) {
        algorithmType = "pixel_difference";  // Default
    }
    
    // Create algorithm instance
    if (algorithmType == "pixel_difference") {
        algorithm_ = std::make_unique<PixelDifferenceDetector>();
    } else {
        if (host_ && host_->log) {
            host_->log(hostCtx_, ZM_LOG_ERROR, ("Unknown algorithm type: " + algorithmType).c_str());
        }
        return false;
    }
    
    // Load zones
    std::string zonesConfig = extractJsonString(config, "zones");
    if (!zonesConfig.empty()) {
        zoneManager_.loadZones(zonesConfig);
    }
    
    initialized_ = true;
    return true;
}

void HybridMotionDetector::processFrame(const zm_frame_hdr_t* header, const uint8_t* frameData) {
    if (!initialized_ || !algorithm_) return;
    
    // Extract frame dimensions (this may need adjustment based on your zm_frame_hdr_t structure)
    int width = header->stream_id;  // Placeholder - adjust based on actual header structure
    int height = header->flags;     // Placeholder - adjust based on actual header structure
    
    if (width <= 0 || height <= 0) return;
    
    // Initialize algorithm if dimensions changed
    if (width != width_ || height != height_) {
        width_ = width;
        height_ = height;
        algorithm_->initialize(width_, height_, "");
    }
    
    // Detect motion
    MotionResult result = algorithm_->detectMotion(frameData, zoneManager_.getZones());
    
    // Publish events
    publishMotionEvents(result, header->stream_id);
}

void HybridMotionDetector::publishMotionEvents(const MotionResult& result, int monitorId) {
    if (!host_ || !host_->publish_evt) return;
    
    // Publish global motion event
    if (result.hasMotion) {
        std::string globalEvent = "{\"type\":\"motion\",\"monitor\":" + std::to_string(monitorId) + 
                                 ",\"pixels\":" + std::to_string(result.totalPixels) + "}";
        host_->publish_evt(hostCtx_, globalEvent.c_str());
    }
    
    // Publish zone-specific events
    for (const auto& zoneResult : result.zoneResults) {
        if (zoneResult.motionDetected) {
            std::string zoneEvent = "{\"type\":\"zone_motion\",\"monitor\":" + std::to_string(monitorId) + 
                                   ",\"zone\":" + std::to_string(zoneResult.zoneId) + 
                                   ",\"pixels\":" + std::to_string(zoneResult.pixelCount);
            
            if (!zoneResult.blobs.empty()) {
                zoneEvent += ",\"blobs\":" + std::to_string(zoneResult.blobCount);
            }
            
            zoneEvent += "}";
            host_->publish_evt(hostCtx_, zoneEvent.c_str());
        }
    }
}

void HybridMotionDetector::shutdown() {
    algorithm_.reset();
    initialized_ = false;
}

// Plugin interface implementation
struct MotionHybridCtx {
    HybridMotionDetector detector;
};

extern "C" {

static int motion_hybrid_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto* ctx = new MotionHybridCtx;
    
    std::string config = json_cfg ? json_cfg : "{}";
    if (!ctx->detector.initialize(host, host_ctx, config)) {
        delete ctx;
        return -1;
    }
    
    plugin->instance = ctx;
    return 0;
}

static void motion_hybrid_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<MotionHybridCtx*>(plugin->instance);
    if (ctx) {
        ctx->detector.shutdown();
        delete ctx;
        plugin->instance = nullptr;
    }
}

static void motion_hybrid_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<MotionHybridCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) return;
    
    const zm_frame_hdr_t* header = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* frameData = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    
    ctx->detector.processFrame(header, frameData);
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_DETECT;
    plugin->start = motion_hybrid_start;
    plugin->stop = motion_hybrid_stop;
    plugin->on_frame = motion_hybrid_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"
