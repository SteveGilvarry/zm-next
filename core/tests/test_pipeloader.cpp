#include <gtest/gtest.h>
#include "zm/PipelineLoader.hpp"
#include <fstream>
#include <cstdio>

using namespace zm;

TEST(PipelineLoaderTest, JsonTreePreservesChildren) {
    const std::string f = "test_pipeline_tree.json";
    {
        std::ofstream o(f);
        o << R"({"plugins":[{"kind":"capture_rtsp_multi","children":[)"
             R"({"kind":"decode_ffmpeg","children":[{"kind":"detect_onnx"},{"kind":"store"}]})"
             R"(]}]})";
    }
    PipelineLoader loader(f);
    ASSERT_TRUE(loader.load());
    const auto& p = loader.getPipeline();
    ASSERT_EQ(p.size(), 4u);  // capture, decode, detect, store (DFS order)
    ASSERT_EQ(p[0].children.size(), 1u);
    EXPECT_EQ(p[0].children[0], 1);             // capture -> decode
    ASSERT_EQ(p[1].children.size(), 2u);
    EXPECT_EQ(p[1].children[0], 2);             // decode -> detect
    EXPECT_EQ(p[1].children[1], 3);             // decode -> store (branch)
    EXPECT_TRUE(p[2].children.empty());
    EXPECT_TRUE(p[3].children.empty());
    remove(f.c_str());
}

TEST(PipelineLoaderTest, FlatArrayLinearFallback) {
    const std::string f = "test_pipeline_flat.json";
    {
        std::ofstream o(f);
        o << R"({"plugins":[{"kind":"a"},{"kind":"b"},{"kind":"c"}]})";
    }
    PipelineLoader loader(f);
    ASSERT_TRUE(loader.load());
    const auto& p = loader.getPipeline();
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0].children, (std::vector<int>{1}));
    EXPECT_EQ(p[1].children, (std::vector<int>{2}));
    EXPECT_TRUE(p[2].children.empty());
    remove(f.c_str());
}

// main omitted; use gtest_main
