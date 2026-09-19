// shape_lut.cpp — see include/asciiplayer/shape_lut.h
#include "asciiplayer/shape_lut.h"

#include "asciiplayer/glyphs.h"

// The precomputed answer for every (contrast plane, brightness level, pattern)
// combination: 3 * 32 * 256 = 24576 bytes, built once and then read-only.
static const int kLevels = 32;      // brightness steps -> smooth flat regions
static const int kPlanes = 3;       // flat, soft and hard contrast targets
static char g_shapeLut[kPlanes][kLevels][256];
static bool g_shapeLutReady = false;

void buildShapeLut() {
    if (g_shapeLutReady) return;

    // Per glyph: mean ink, and how each sub-cell deviates from that mean.
    static float gMean[256], gDev[256][kSubN];
    for (int g = 0; g < kGlyphCount; ++g) {
        float m = 0.0f;
        for (int i = 0; i < kSubN; ++i) m += kGlyphs[g].ink[i];
        m /= kSubN;
        gMean[g] = m;
        for (int i = 0; i < kSubN; ++i) gDev[g][i] = kGlyphs[g].ink[i] - m;
    }

    // No ASCII glyph is a solid block - they all leave the top and bottom of the
    // cell empty - so a flat cell can never match one on shape. Judging a flat
    // cell on shape therefore rejects every dense glyph and strands the bright
    // end of the ramp. The shape term has to fade out with the cell's contrast:
    // plane 0 is pure brightness (smooth gradients), plane 2 is full shape
    // matching (edges and diagonals), plane 1 sits between them.
    const float kLevelWeight = 4.0f;
    const float kContrast[kPlanes]   = { 0.00f, 0.18f, 0.40f };
    const float kShapeWeight[kPlanes] = { 0.00f, 0.60f, 1.00f };

    for (int plane = 0; plane < kPlanes; ++plane) {
        for (int lvl = 0; lvl < kLevels; ++lvl) {
            float tm = (float)lvl / (kLevels - 1);
            for (int sig = 0; sig < 256; ++sig) {
                float tdev[kSubN];
                for (int i = 0; i < kSubN; ++i)
                    tdev[i] = (sig >> i) & 1 ? kContrast[plane] : -kContrast[plane];

                float best = 1e30f;
                char bestCh = ' ';
                for (int g = 0; g < kGlyphCount; ++g) {
                    float lv = tm - gMean[g];
                    float sum = kLevelWeight * kSubN * lv * lv;
                    if (kShapeWeight[plane] > 0.0f) {
                        float shape = 0.0f;
                        for (int i = 0; i < kSubN; ++i) {
                            float d = tdev[i] - gDev[g][i];
                            shape += d * d;
                        }
                        sum += kShapeWeight[plane] * shape;
                    }
                    if (sum < best) { best = sum; bestCh = kGlyphs[g].ch; }
                }
                g_shapeLut[plane][lvl][sig] = bestCh;
            }
        }
    }
    g_shapeLutReady = true;
}

// Picks the glyph for one cell from its 8 sub-pixel luminances.
char glyphFor(const int* lum) {
    int sum = 0;
    for (int i = 0; i < kSubN; ++i) sum += lum[i];
    int mean = sum / kSubN;

    int sig = 0, spread = 0;
    for (int i = 0; i < kSubN; ++i) {
        if (lum[i] > mean) sig |= 1 << i;
        spread += lum[i] > mean ? lum[i] - mean : mean - lum[i];
    }
    int avgSpread = spread / kSubN;
    int plane = avgSpread > 40 ? 2 : (avgSpread > 14 ? 1 : 0);
    int lvl = mean * (kLevels - 1) / 255;
    return g_shapeLut[plane][lvl][sig];
}
