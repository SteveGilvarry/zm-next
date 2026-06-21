// track_offline: drive the REAL zm::tracker::Tracker (plugins/tracker/tracker_core.hpp)
// over a bench_events detections.jsonl, assigning persistent track_ids.
//
// Mirrors the tracker plugin's per-event update cadence (one update() per
// detection event, per stream). Input lines:
//   {"frame":N,"event":{"stream_id":S,"detections":[{label,confidence,bbox,class_id},...]}}
// Output lines (only persons, with track_id; 0 == unconfirmed/tentative):
//   {"frame":N,"detections":[{"bbox":[x,y,w,h],"confidence":c,"track_id":K},...],
//    "confirmed_tracks":[ids...]}
//
// Usage: track_offline <in.jsonl> <out.jsonl> [iou=0.3] [max_age=30] [min_hits=3] [label=person]
#include "tracker_core.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>

using json = nlohmann::json;
using namespace zm::tracker;

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "need <in.jsonl> <out.jsonl>\n"; return 1; }
    const std::string in = argv[1], out = argv[2];
    const float iou_thr = argc > 3 ? std::stof(argv[3]) : 0.3f;
    const int max_age   = argc > 4 ? std::stoi(argv[4]) : 30;
    const int min_hits  = argc > 5 ? std::stoi(argv[5]) : 3;
    const std::string want = argc > 6 ? argv[6] : "person";

    std::map<int, Tracker> per_stream;          // stream_id -> tracker
    std::ifstream f(in);
    std::ofstream o(out);
    std::string line;
    std::set<int> all_confirmed;
    long n_in = 0, n_conf = 0;

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded()) continue;
        const int frame = j.value("frame", 0);
        const auto& ev = j["event"];
        const int sid = ev.value("stream_id", 0);

        std::vector<Det> dets;
        std::vector<json> meta;                 // parallel: bbox/conf for output
        for (const auto& d : ev["detections"]) {
            if (d.value("label", "") != want) continue;
            auto b = d["bbox"];
            Det det;
            det.x = b[0]; det.y = b[1]; det.w = b[2]; det.h = b[3];
            det.confidence = d.value("confidence", 0.f);
            det.class_id = d.value("class_id", -1);
            dets.push_back(det);
            meta.push_back(d);
            ++n_in;
        }

        auto& trk = per_stream.emplace(sid, Tracker(iou_thr, max_age, min_hits)).first->second;
        std::vector<int> ids = trk.update(dets);

        json outj;
        outj["frame"] = frame;
        json darr = json::array();
        std::set<int> confirmed_here;
        for (size_t i = 0; i < dets.size(); ++i) {
            json d;
            d["bbox"] = { dets[i].x, dets[i].y, dets[i].w, dets[i].h };
            d["confidence"] = dets[i].confidence;
            d["track_id"] = ids[i];
            darr.push_back(d);
            if (ids[i] != 0) { confirmed_here.insert(ids[i]); all_confirmed.insert(ids[i]); ++n_conf; }
        }
        outj["detections"] = darr;
        outj["confirmed_tracks"] = confirmed_here;
        o << outj.dump() << "\n";
    }
    std::cerr << "person dets in=" << n_in << "  confirmed-assignments=" << n_conf
              << "  distinct confirmed track_ids=" << all_confirmed.size()
              << "  (iou=" << iou_thr << " max_age=" << max_age << " min_hits=" << min_hits << ")\n";
    return 0;
}
