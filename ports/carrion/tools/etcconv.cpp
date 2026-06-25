// etcconv: RGBA8 cru -> blocos ETC2 RGBA8 EAC (p/ embutir em XNB Rgba8Etc2=94).
// uso: etcconv <in_rgba_raw> <w> <h> <out_etc2> [effort]
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "Etc.h"
#include "EtcImage.h"
#include "EtcErrorMetric.h"

int main(int argc, char** argv)
{
    if (argc < 5) { fprintf(stderr, "uso: etcconv in.rgba w h out.etc2 [effort]\n"); return 2; }
    const char* inPath = argv[1];
    unsigned int w = (unsigned)atoi(argv[2]);
    unsigned int h = (unsigned)atoi(argv[3]);
    const char* outPath = argv[4];
    float effort = (argc >= 6) ? (float)atof(argv[5]) : 40.0f;

    size_t npix = (size_t)w * h;
    std::vector<unsigned char> raw(npix * 4);
    FILE* f = fopen(inPath, "rb");
    if (!f) { fprintf(stderr, "nao abriu %s\n", inPath); return 1; }
    if (fread(raw.data(), 1, raw.size(), f) != raw.size()) { fprintf(stderr, "leitura curta\n"); fclose(f); return 1; }
    fclose(f);

    std::vector<float> rgba(npix * 4);
    for (size_t i = 0; i < raw.size(); i++) rgba[i] = raw[i] / 255.0f;

    unsigned char* bits = nullptr;
    unsigned int bitsBytes = 0, extW = 0, extH = 0;
    int timeMs = 0;
    Etc::Encode(rgba.data(), w, h,
                Etc::Image::Format::RGBA8,
                Etc::ErrorMetric::RGBA,
                effort,
                4, 1024,
                &bits, &bitsBytes, &extW, &extH, &timeMs);
    if (!bits || bitsBytes == 0) { fprintf(stderr, "encode falhou\n"); return 1; }

    FILE* o = fopen(outPath, "wb");
    if (!o) { fprintf(stderr, "nao criou %s\n", outPath); return 1; }
    fwrite(bits, 1, bitsBytes, o);
    fclose(o);
    return 0;
}
