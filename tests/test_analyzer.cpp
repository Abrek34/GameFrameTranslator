#include <cassert>
#include <cstdio>
#include <iostream>
#include <vector>

#include "analyzer/analyzer.hpp"
#include "common/types.hpp"

using namespace gft;

static Frame makeFrame(int w, int h, uint8_t v) {
    Frame f;
    f.width = w; f.height = h; f.originX = 0; f.originY = 0;
    f.rgb.assign(static_cast<size_t>(w) * h * 3, v);
    f.capturedAtNs = nowNs();
    f.source = "test";
    return f;
}

int main() {
    AnalyzerConfig acfg;
    acfg.analysisWidth = 160;
    acfg.pixelThreshold = 10;
    FrameDiffer differ(acfg);
    Frame f1 = makeFrame(320, 180, 10);
    auto r1 = differ.compute(f1);
    assert(r1.firstFrame);
    auto r2 = differ.compute(f1);
    assert(r2.changedRatio == 0.0);

    Frame f2 = f1;
    for (int y = 20; y < 60; ++y)
        for (int x = 20; x < 100; ++x) {
            const size_t i = (static_cast<size_t>(y) * 320 + x) * 3;
            f2.rgb[i] = 240; f2.rgb[i+1]=240; f2.rgb[i+2]=240;
        }
    auto r3 = differ.compute(f2);
    assert(r3.changedRatio > 0.0);
    assert(r3.changedBbox.has_value());
    assert(r3.changedBbox->w >= 70);

    // tracker lifecycle
    AnalyzerConfig acfg2;
    acfg2.expireMs = 1000;
    acfg2.regionChange = 0.01;
    TranslateConfig tcfg;
    tcfg.retryCooldownMs = 0;
    RegionTracker tr(acfg2, tcfg);
    Frame frame = makeFrame(320, 180, 10);
    for (int y = 30; y < 50; ++y)
        for (int x = 20; x < 150; ++x) {
            const size_t i = (static_cast<size_t>(y) * 320 + x) * 3;
            frame.rgb[i] = 240; frame.rgb[i+1]=240; frame.rgb[i+2]=240;
        }
    auto diff = differ.compute(frame);
    (void)diff;
    OcrLine line{Rect{20,30,130,20}, "Continue", 90.0};
    int64_t now = 1000000000LL;
    tr.ingestFull(frame, {line}, now);
    assert(tr.size() == 1);
    auto pending = tr.pendingTranslations(now, 4);
    assert(pending.size() == 1);
    assert(tr.setTranslation(pending[0].id, "Devam", "dictionary", now + 1000000));
    auto items = tr.overlayItems(now + 2000000, 5000);
    assert(items.size() == 1);
    assert(tr.pendingTranslations(now + 2000000, 4).empty());

    std::cout << "test_analyzer OK\n";
    return 0;
}
