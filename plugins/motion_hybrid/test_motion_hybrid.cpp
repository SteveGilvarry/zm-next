#include "motion_hybrid.hpp"
#include <iostream>
#include <cassert>

void testZoneParsingAndGeometry() {
    std::cout << "Testing zone parsing and geometry operations...\n";
    
    // Test ZoneMinder coordinate parsing
    std::string coords = "100,50 400,50 400,300 100,300";
    Polygon poly = ZoneManager::parseCoords(coords);
    
    assert(poly.outer().size() == 5);  // 4 points + closing point
    assert(poly.outer()[0].x() == 100);
    assert(poly.outer()[0].y() == 50);
    assert(poly.outer()[3].x() == 100);
    assert(poly.outer()[3].y() == 300);
    
    // Test point-in-polygon
    Point inside(200, 150);
    Point outside(50, 25);
    
    assert(bg::within(inside, poly));
    assert(!bg::within(outside, poly));
    
    // Test bounding box
    Box bbox;
    bg::envelope(poly, bbox);
    assert(bbox.min_corner().x() == 100);
    assert(bbox.min_corner().y() == 50);
    assert(bbox.max_corner().x() == 400);
    assert(bbox.max_corner().y() == 300);
    
    std::cout << "✓ Zone parsing and geometry tests passed\n";
}

void testZoneManager() {
    std::cout << "Testing ZoneManager...\n";
    
    ZoneManager manager;
    
    // Create test zone
    ZoneConfig zone;
    zone.id = 1;
    zone.name = "Test Zone";
    zone.type = "Active";
    zone.polygon = ZoneManager::parseCoords("0,0 100,0 100,100 0,100");
    zone.minAlarmPixels = 500;
    
    manager.addZone(zone);
    
    // Test zone retrieval
    auto zones = manager.getZones();
    assert(zones.size() == 1);
    assert(zones[0].id == 1);
    assert(zones[0].name == "Test Zone");
    
    // Test spatial queries
    auto zonesAtPoint = manager.getZonesAtPoint(50, 50);
    assert(zonesAtPoint.size() == 1);
    assert(zonesAtPoint[0] == 1);
    
    zonesAtPoint = manager.getZonesAtPoint(150, 150);
    assert(zonesAtPoint.empty());
    
    std::cout << "✓ ZoneManager tests passed\n";
}

void testJsonParsing() {
    std::cout << "Testing JSON parsing...\n";
    
    std::string zoneJson = R"({
        "Id": 5,
        "Name": "Test Zone",
        "Type": "Active", 
        "Coords": "0,0 200,0 200,150 0,150",
        "CheckMethod": "Blobs",
        "MinPixelThreshold": 25,
        "MinAlarmPixels": 1000,
        "MaxAlarmPixels": 5000,
        "FilterX": 3,
        "FilterY": 3,
        "MinBlobPixels": 100,
        "MaxBlobPixels": 2000,
        "MinBlobs": 1,
        "MaxBlobs": 5,
        "AlarmRGB": 16711680
    })";
    
    ZoneConfig zone = ZoneManager::parseZoneMinder(zoneJson);
    
    assert(zone.id == 5);
    assert(zone.name == "Test Zone");
    assert(zone.type == "Active");
    assert(zone.checkMethod == "Blobs");
    assert(zone.minPixelThreshold == 25);
    assert(zone.minAlarmPixels == 1000);
    assert(zone.maxAlarmPixels == 5000);
    assert(zone.filterX == 3);
    assert(zone.filterY == 3);
    assert(zone.minBlobPixels == 100);
    assert(zone.maxBlobPixels == 2000);
    assert(zone.minBlobs == 1);
    assert(zone.maxBlobs == 5);
    assert(zone.alarmRGB == 16711680);
    
    // Test polygon
    assert(zone.polygon.outer().size() == 5);  // 4 points + closing
    Point testPoint(100, 75);
    assert(bg::within(testPoint, zone.polygon));
    
    std::cout << "✓ JSON parsing tests passed\n";
}

void testPerformanceBenchmark() {
    std::cout << "Running performance benchmark...\n";
    
    ZoneManager manager;
    
    // Create multiple zones
    for (int i = 0; i < 10; ++i) {
        ZoneConfig zone;
        zone.id = i + 1;
        zone.name = "Zone " + std::to_string(i + 1);
        zone.type = "Active";
        
        // Create different sized zones
        int x = (i % 3) * 200;
        int y = (i / 3) * 150;
        std::string coords = std::to_string(x) + "," + std::to_string(y) + " " +
                           std::to_string(x + 150) + "," + std::to_string(y) + " " +
                           std::to_string(x + 150) + "," + std::to_string(y + 100) + " " +
                           std::to_string(x) + "," + std::to_string(y + 100);
        
        zone.polygon = ZoneManager::parseCoords(coords);
        manager.addZone(zone);
    }
    
    // Benchmark spatial queries
    auto start = std::chrono::high_resolution_clock::now();
    
    const int numQueries = 10000;
    int totalFound = 0;
    
    for (int i = 0; i < numQueries; ++i) {
        double x = (i % 640);
        double y = ((i / 640) % 480);
        auto zones = manager.getZonesAtPoint(x, y);
        totalFound += zones.size();
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "✓ Performance benchmark completed:\n";
    std::cout << "  " << numQueries << " spatial queries in " << duration.count() << " μs\n";
    std::cout << "  Average: " << (double)duration.count() / numQueries << " μs per query\n";
    std::cout << "  Total zones found: " << totalFound << "\n";
}

int main() {
    std::cout << "Motion Hybrid Plugin Test Suite\n";
    std::cout << "================================\n\n";
    
    try {
        testZoneParsingAndGeometry();
        std::cout << "\n";
        
        testZoneManager();
        std::cout << "\n";
        
        testJsonParsing();
        std::cout << "\n";
        
        testPerformanceBenchmark();
        std::cout << "\n";
        
        std::cout << "✅ All tests passed!\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cout << "❌ Test failed: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cout << "❌ Unknown test failure\n";
        return 1;
    }
}
