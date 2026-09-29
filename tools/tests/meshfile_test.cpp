// Offline check of the model file reader: loads each file given, prints its materials, items, animations and bounds,
// and writes a posed copy as OBJ (the game's space) for looking at.
//   meshfile_test <model file> [animation fragment] [seconds] [out.obj]
#include <windows.h>

#include <cfloat>
#include <cstdio>
#include <string>

#include "../../src/host/meshfile.hpp"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    meshfile::Model m;
    std::string error;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    if (!m.Load(argv[1], std::wstring(temp) + L"meshfile_cache", &error)) {
        std::printf("FAIL %s\n", error.c_str());
        return 1;
    }
    for (size_t i = 0; i < m.Materials().size(); ++i) {
        const auto& mat = m.Materials()[i];
        std::printf("material %zu '%s' rgba %.3f %.3f %.3f %.2f metal %.2f rough %.2f emit %.2f %.2f %.2f texture %ls\n", i, mat.name.c_str(), mat.r, mat.g,
                    mat.b, mat.a, mat.metallic, mat.rough, mat.er, mat.eg, mat.eb, mat.textureFile.c_str());
    }
    size_t tris = 0;
    for (const auto& it : m.Items()) tris += it.indices.size() / 3;
    std::printf("items %zu triangles %zu\n", m.Items().size(), tris);
    const auto names = m.Animations();
    for (size_t i = 0; i < names.size(); ++i)
        if (i < 6) std::printf("animation %zu '%s' %.2f s\n", i, names[i].c_str(), m.Length(static_cast<int>(i)));
    std::printf("animations %zu\n", names.size());
    int anim = -1;
    double t = 0;
    if (argc > 2) {
        char frag[256];
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, frag, sizeof frag, nullptr, nullptr);
        anim = m.FindAnimation(frag);
        std::printf("posing '%s' -> %d\n", frag, anim);
    }
    if (argc > 3) t = _wtof(argv[3]);
    const auto t0 = GetTickCount64();
    const meshfile::Pose pose = m.At(anim, t);
    const auto took = GetTickCount64() - t0;
    float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& p : pose.positions)
        for (size_t v = 0; v + 2 < p.size(); v += 3)
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], p[v + static_cast<size_t>(c)]);
                hi[c] = std::max(hi[c], p[v + static_cast<size_t>(c)]);
            }
    std::printf("bounds (cm, game space) x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f; posed in %llu ms\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                static_cast<unsigned long long>(took));
    if (argc > 4) {
        // The pose as OBJ, with its UVs and an .mtl of the materials (colour, emission, texture), for rendering.
        std::wstring mtl = argv[4];
        mtl = mtl.substr(0, mtl.size() - 4) + L".mtl";
        const std::wstring mtlName = mtl.substr(mtl.find_last_of(L"\\/") + 1);
        FILE* f = _wfopen(argv[4], L"w");
        std::fprintf(f, "mtllib %ls\n", mtlName.c_str());
        size_t base = 1;
        for (size_t i = 0; i < pose.positions.size(); ++i) {
            const auto& p = pose.positions[i];
            const auto& uv = m.Items()[i].uv;
            std::fprintf(f, "g item%zu\nusemtl m%d\n", i, m.Items()[i].material);
            for (size_t v = 0; v + 2 < p.size(); v += 3) std::fprintf(f, "v %f %f %f\n", p[v], p[v + 1], p[v + 2]);
            for (size_t v = 0; v + 1 < uv.size(); v += 2) std::fprintf(f, "vt %f %f\n", uv[v], uv[v + 1]);
            const auto& idx = m.Items()[i].indices;
            for (size_t k = 0; k + 2 < idx.size(); k += 3)
                std::fprintf(f, "f %zu/%zu %zu/%zu %zu/%zu\n", idx[k] + base, idx[k] + base, idx[k + 1] + base, idx[k + 1] + base,
                             idx[k + 2] + base, idx[k + 2] + base);
            base += p.size() / 3;
        }
        std::fclose(f);
        FILE* g = _wfopen(mtl.c_str(), L"w");
        for (size_t i = 0; i < m.Materials().size(); ++i) {
            const auto& mat = m.Materials()[i];
            std::fprintf(g, "newmtl m%zu\nKd %f %f %f\nKe %f %f %f\nd %f\n", i, mat.r, mat.g, mat.b, mat.er, mat.eg, mat.eb, mat.a);
            if (!mat.textureFile.empty()) std::fprintf(g, "map_Kd %ls\n", mat.textureFile.c_str());
        }
        std::fclose(g);
    }
    return 0;
}
