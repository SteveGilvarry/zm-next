#include "motion_pixel_diff.hpp"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Simple zone metadata deserialization (matching zones plugin)
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
                if (!coords.empty()) {
                    std::istringstream ss(coords);
                    std::string token;
                    
                    while (std::getline(ss, token, ' ')) {
                        if (token.empty()) continue;
                        
                        auto commaPos = token.find(',');
                        if (commaPos != std::string::npos) {
                            double x = std::stod(token.substr(0, commaPos));
                            double y = std::stod(token.substr(commaPos + 1));
                            bg::append(zone.polygon.outer(), Point(x, y));
                        }
                    }
                    
                    // Ensure polygon is closed
                    if (!zone.polygon.outer().empty()) {
                        const auto& first = zone.polygon.outer().front();
                        const auto& last = zone.polygon.outer().back();
                        if (first.x() != last.x() || first.y() != last.y()) {
                            zone.polygon.outer().push_back(zone.polygon.outer().front());
                        }
                    }
                }
                
                metadata.zones.push_back(zone);
            }
        }
    } catch (const std::exception& e) {
        // Return empty metadata on error
        metadata.zones.clear();
    }
    
    return metadata;
}

// PixelDifferenceDetector Implementation
PixelDifferenceDetector::PixelDifferenceDetector() 
    : width_(0), height_(0), backgroundReady_(false), frameCount_(0) {
}

bool PixelDifferenceDetector::initialize(int width, int height, const MotionConfig& config) {
    width_ = width;
    height_ = height;
    config_ = config;
    frameCount_ = 0;
    
    // Handle downscaling
    int effectiveWidth = width_;
    int effectiveHeight = height_;
    
    if (config_.downscale == 2) {
        effectiveWidth = width_ / 2;
        effectiveHeight = height_ / 2;
    } else if (config_.downscale == 0 && config_.outWidth > 0 && config_.outHeight > 0) {
        effectiveWidth = config_.outWidth;
        effectiveHeight = config_.outHeight;
    }
    
    size_t frameSize = effectiveWidth * effectiveHeight;
    background_.assign(frameSize, 0);
    backgroundReady_ = false;
    
    return true;
}

MotionResult PixelDifferenceDetector::detectMotion(const uint8_t* frame, const std::vector<ZoneConfig>& zones) {
    MotionResult result;
    result.hasGlobalMotion = false;
    result.totalMotionPixels = 0;
    
    if (!frame) return result;
    
    frameCount_++;
    
    // Handle downscaling
    std::vector<uint8_t> processedFrame;
    const uint8_t* frameToProcess = frame;
    int processWidth = width_;
    int processHeight = height_;
    
    if (config_.downscale == 2) {
        processWidth = width_ / 2;
        processHeight = height_ / 2;
        processedFrame.resize(processWidth * processHeight);
        
        // Downsample by averaging 2x2 blocks
        for (int y = 0; y < processHeight; ++y) {
            for (int x = 0; x < processWidth; ++x) {
                int srcX = x * 2;
                int srcY = y * 2;
                int sum = frame[srcY * width_ + srcX] + 
                         frame[srcY * width_ + srcX + 1] + 
                         frame[(srcY + 1) * width_ + srcX] + 
                         frame[(srcY + 1) * width_ + srcX + 1];
                processedFrame[y * processWidth + x] = sum / 4;
            }
        }
        frameToProcess = processedFrame.data();
    }
    
    if (!backgroundReady_) {
        std::memcpy(background_.data(), frameToProcess, processWidth * processHeight);
        backgroundReady_ = true;
        return result;
    }
    
    // Generate motion map
    std::vector<uint8_t> motionMap = generateMotionMap(frameToProcess);
    
    // Count total motion pixels
    result.totalMotionPixels = std::count_if(motionMap.begin(), motionMap.end(),
                                            [](uint8_t val) { return val > 0; });
    
    // Check global motion threshold
    result.hasGlobalMotion = (result.totalMotionPixels >= config_.minPixels);
    
    // Process zones if provided and zone-aware mode is enabled
    if (config_.zoneAware && !zones.empty()) {
        for (const auto& zone : zones) {
            if (zone.type != "Active") continue;
            
            ZoneMotionResult zoneResult;
            zoneResult.zoneId = zone.id;
            zoneResult.pixelCount = detectMotionInZone(motionMap, zone);
            
            // Apply zone-specific thresholds
            bool hasMinPixels = (zone.minAlarmPixels == 0) || (zoneResult.pixelCount >= zone.minAlarmPixels);
            bool belowMaxPixels = (zone.maxAlarmPixels == 0) || (zoneResult.pixelCount <= zone.maxAlarmPixels);
            
            zoneResult.motionDetected = hasMinPixels && belowMaxPixels;
            
            // Blob detection for zones that use it
            if (zoneResult.motionDetected && zone.checkMethod == "Blobs") {
                zoneResult.blobs = findBlobs(motionMap, zone);
                zoneResult.blobCount = zoneResult.blobs.size();
                
                // Apply blob constraints
                bool hasMinBlobs = (zone.minBlobs == 0) || (zoneResult.blobCount >= zone.minBlobs);
                bool belowMaxBlobs = (zone.maxBlobs == 0) || (zoneResult.blobCount <= zone.maxBlobs);
                
                zoneResult.motionDetected = hasMinBlobs && belowMaxBlobs;
            }
            
            result.zoneResults.push_back(zoneResult);
        }
    }
    
    // Store motion map if requested
    if (config_.enableMotionMap) {
        result.motionMap = std::move(motionMap);
    }
    
    // Update background
    if (config_.enableBackgroundUpdate) {
        updateBackground(frameToProcess);
    }
    
    return result;
}

std::vector<uint8_t> PixelDifferenceDetector::generateMotionMap(const uint8_t* frame) {
    size_t frameSize = background_.size();
    std::vector<uint8_t> motionMap(frameSize, 0);
    
    // SIMD-optimized motion detection
    using batch_t = xsimd::batch<uint8_t>;
    constexpr auto VL = batch_t::size;
    
    const uint8_t* bg = background_.data();
    batch_t threshold_vec(config_.globalThreshold);
    
    size_t i = 0;
    
    // Vectorized comparison
    for (; i + VL <= frameSize; i += VL) {
        batch_t frame_vec = batch_t::load_unaligned(frame + i);
        batch_t bg_vec = batch_t::load_unaligned(bg + i);
        batch_t diff = xsimd::abs(frame_vec - bg_vec);
        auto mask = xsimd::gt(diff, threshold_vec);
        
        // Store motion map
        batch_t motion_result = xsimd::select(mask, batch_t(255), batch_t(0));
        motion_result.store_unaligned(motionMap.data() + i);
    }
    
    // Handle remaining pixels
    for (; i < frameSize; ++i) {
        if (std::abs(int(frame[i]) - int(bg[i])) > config_.globalThreshold) {
            motionMap[i] = 255;
        }
    }
    
    return motionMap;
}

void PixelDifferenceDetector::updateBackground(const uint8_t* frame) {
    size_t frameSize = background_.size();
    uint8_t* bg = background_.data();
    
    using batch_t = xsimd::batch<uint8_t>;
    constexpr auto VL = batch_t::size;
    
    // Convert learning rate to integer math (alpha = 1 - learningRate)
    int alpha = (int)((1.0f - config_.backgroundLearningRate) * 256);
    int beta = 256 - alpha;
    
    size_t i = 0;
    
    // Vectorized background update: bg = (bg * alpha + frame * beta) / 256
    for (; i + VL <= frameSize; i += VL) {
        // Load current values
        auto frame_vec = xsimd::load_unaligned(reinterpret_cast<const uint16_t*>(frame + i));
        auto bg_vec = xsimd::load_unaligned(reinterpret_cast<const uint16_t*>(bg + i));
        
        // For simplicity, fall back to scalar for now
        // A proper implementation would use wider types for the multiplication
        for (size_t j = 0; j < VL && i + j < frameSize; ++j) {
            bg[i + j] = (uint8_t)(((uint16_t)bg[i + j] * alpha + (uint16_t)frame[i + j] * beta) >> 8);
        }
    }
    
    // Handle remaining pixels
    for (; i < frameSize; ++i) {
        bg[i] = (uint8_t)(((uint16_t)bg[i] * alpha + (uint16_t)frame[i] * beta) >> 8);
    }
}

int PixelDifferenceDetector::detectMotionInZone(const std::vector<uint8_t>& motionMap, const ZoneConfig& zone) {
    if (zone.polygon.outer().empty()) return 0;
    
    // Get bounding box of zone for efficiency
    Box boundingBox;
    bg::envelope(zone.polygon, boundingBox);
    
    int minX = std::max(0, (int)boundingBox.min_corner().x());
    int maxX = std::min(width_ - 1, (int)boundingBox.max_corner().x());
    int minY = std::max(0, (int)boundingBox.min_corner().y());
    int maxY = std::min(height_ - 1, (int)boundingBox.max_corner().y());
    
    int count = 0;
    
    // Check each pixel in bounding box
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            Point point(x, y);
            if (bg::within(point, zone.polygon)) {
                int idx = y * width_ + x;
                if (idx < (int)motionMap.size() && motionMap[idx] > 0) {
                    count++;
                }
            }
        }
    }
    
    return count;
}

std::vector<Box> PixelDifferenceDetector::findBlobs(const std::vector<uint8_t>& motionMap, const ZoneConfig& zone) {
    std::vector<Box> blobs;
    
    if (zone.polygon.outer().empty()) return blobs;
    
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
                        if (idx < (int)motionMap.size() && motionMap[idx] > 0) {
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

void PixelDifferenceDetector::reset() {
    backgroundReady_ = false;
    frameCount_ = 0;
    std::fill(background_.begin(), background_.end(), 0);
}

bool PixelDifferenceDetector::isPointInZone(double x, double y, const ZoneConfig& zone) const {
    Point point(x, y);
    return bg::within(point, zone.polygon);
}

// Plugin context
struct MotionPixelDiffCtx {
    PixelDifferenceDetector detector;
    MotionConfig config;
    int monitorId = 0;
    zm_host_api_t* host = nullptr;
    void* hostCtx = nullptr;
};

extern "C" {

static int motion_pixel_diff_start(zm_plugin_t* plugin, zm_host_api_t* host, void* host_ctx, const char* json_cfg) {
    auto* ctx = new MotionPixelDiffCtx;
    ctx->host = host;
    ctx->hostCtx = host_ctx;
    
    // Parse configuration
    if (json_cfg) {
        try {
            auto j = json::parse(json_cfg);
            ctx->config.frameWidth = j.value("frame_width", 1280);
            ctx->config.frameHeight = j.value("frame_height", 720);
            ctx->config.globalThreshold = j.value("threshold", 25);
            ctx->config.minPixels = j.value("min_pixels", 800);
            ctx->config.downscale = j.value("downscale", 1);
            ctx->config.outWidth = j.value("out_width", 0);
            ctx->config.outHeight = j.value("out_height", 0);
            ctx->config.zoneAware = j.value("zone_aware", true);
            ctx->config.enableBackgroundUpdate = j.value("enable_background_update", true);
            ctx->config.backgroundLearningRate = j.value("background_learning_rate", 0.03125f);
            ctx->config.enableMotionMap = j.value("enable_motion_map", false);
            ctx->config.enableFiltering = j.value("enable_filtering", true);
            ctx->monitorId = j.value("monitor_id", 0);
        } catch (const std::exception& e) {
            if (host && host->log) {
                host->log(host_ctx, ZM_LOG_ERROR, ("Failed to parse motion config: " + std::string(e.what())).c_str());
            }
            // Use default configuration
        }
    }
    
    plugin->instance = ctx;
    
    if (host && host->log) {
        std::string msg = "Motion pixel diff plugin started (threshold=" + 
                         std::to_string(ctx->config.globalThreshold) + 
                         ", zone_aware=" + (ctx->config.zoneAware ? "true" : "false") + ")";
        host->log(host_ctx, ZM_LOG_INFO, msg.c_str());
    }
    
    return 0;
}

static void motion_pixel_diff_stop(zm_plugin_t* plugin) {
    auto* ctx = static_cast<MotionPixelDiffCtx*>(plugin->instance);
    if (ctx) {
        delete ctx;
        plugin->instance = nullptr;
    }
}

static void motion_pixel_diff_on_frame(zm_plugin_t* plugin, const void* buf, size_t size) {
    auto* ctx = static_cast<MotionPixelDiffCtx*>(plugin->instance);
    if (!ctx || !buf || size < sizeof(zm_frame_hdr_t)) return;
    
    const zm_frame_hdr_t* header = static_cast<const zm_frame_hdr_t*>(buf);
    const uint8_t* frameData = static_cast<const uint8_t*>(buf) + sizeof(zm_frame_hdr_t);
    
    // Only process grayscale frames from decode plugin
    if (header->hw_type != ZM_FRAME_GRAYSCALE) {
        static int skipped_frames = 0;
        skipped_frames++;
        
        if (skipped_frames % 100 == 1) {  // Log occasionally
            if (ctx->host && ctx->host->log) {
                std::string msg = "Motion plugin skipping non-grayscale frame #" + std::to_string(skipped_frames) + 
                                 " (type=" + std::to_string(header->hw_type) + ", size=" + std::to_string(header->bytes) + ")";
                ctx->host->log(ctx->hostCtx, ZM_LOG_DEBUG, msg.c_str());
            }
        }
        
        // Forward frame to next plugin without processing
        if (ctx->host && ctx->host->on_frame) {
            ctx->host->on_frame(ctx->hostCtx, buf, size);
        }
        return;
    }
    
    // Initialize detector once with configured dimensions
    static bool initialized = false;
    static int frameCount = 0;
    if (!initialized) {
        ctx->detector.initialize(ctx->config.frameWidth, ctx->config.frameHeight, ctx->config);
        initialized = true;
        
        if (ctx->host && ctx->host->log) {
            std::string msg = "Motion detector initialized with configured dimensions " + 
                             std::to_string(ctx->config.frameWidth) + "x" + std::to_string(ctx->config.frameHeight);
            ctx->host->log(ctx->hostCtx, ZM_LOG_INFO, msg.c_str());
        }
    }
    
    frameCount++;
    if (frameCount % 100 == 0) {  // Log every 100 frames
        if (ctx->host && ctx->host->log) {
            std::string msg = "Motion plugin processing grayscale frame " + std::to_string(frameCount) + 
                             " (size=" + std::to_string(header->bytes) + " bytes)";
            ctx->host->log(ctx->hostCtx, ZM_LOG_INFO, msg.c_str());
        }
    }
    
    // Get empty zones for now (zones plugin would provide this via metadata)
    std::vector<ZoneConfig> zones;
    
    // Detect motion
    MotionResult result = ctx->detector.detectMotion(frameData, zones);
    
    // Publish motion events
    if (result.hasGlobalMotion || !result.zoneResults.empty()) {
        if (ctx->host && ctx->host->publish_evt) {
            // Global motion event
            if (result.hasGlobalMotion) {
                json globalEvent;
                globalEvent["type"] = "motion";
                globalEvent["algorithm"] = "pixel_diff";
                globalEvent["monitor"] = ctx->monitorId;
                globalEvent["pixels"] = result.totalMotionPixels;
                globalEvent["frame_count"] = ctx->detector.getFrameCount();
                
                ctx->host->publish_evt(ctx->hostCtx, globalEvent.dump().c_str());
                
                if (ctx->host && ctx->host->log) {
                    std::string msg = "🚨 MOTION DETECTED! " + std::to_string(result.totalMotionPixels) + " pixels changed";
                    ctx->host->log(ctx->hostCtx, ZM_LOG_INFO, msg.c_str());
                }
            }
            
            // Zone-specific events
            for (const auto& zoneResult : result.zoneResults) {
                if (zoneResult.motionDetected) {
                    json zoneEvent;
                    zoneEvent["type"] = "zone_motion";
                    zoneEvent["algorithm"] = "pixel_diff";
                    zoneEvent["monitor"] = ctx->monitorId;
                    zoneEvent["zone"] = zoneResult.zoneId;
                    zoneEvent["pixels"] = zoneResult.pixelCount;
                    
                    if (zoneResult.blobCount > 0) {
                        zoneEvent["blobs"] = zoneResult.blobCount;
                    }
                    
                    ctx->host->publish_evt(ctx->hostCtx, zoneEvent.dump().c_str());
                }
            }
        }
    }
    
    // Forward frame to next plugin
    if (ctx->host && ctx->host->on_frame) {
        ctx->host->on_frame(ctx->hostCtx, buf, size);
    }
}

#if defined(__GNUC__) || defined(__clang__)
#define ZM_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define ZM_PLUGIN_EXPORT
#endif

ZM_PLUGIN_EXPORT void zm_plugin_init(zm_plugin_t* plugin) {
    plugin->version = 1;
    plugin->type = ZM_PLUGIN_DETECT;
    plugin->start = motion_pixel_diff_start;
    plugin->stop = motion_pixel_diff_stop;
    plugin->on_frame = motion_pixel_diff_on_frame;
    plugin->instance = nullptr;
}

} // extern "C"
