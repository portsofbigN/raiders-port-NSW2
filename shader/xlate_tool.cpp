// Offline Switch 2 shader translator over captured NVN2 stages (work/shaders/*.code, written by host/nx2_prog.c),
// through the in-game translator (nx2_xlate.cpp). Values that only exist at draw time get neutral answers.
//
// Usage: xlate_tool <out dir> <file.code>...         each stage alone: <out dir>/<name>.spv, one line per input
//        xlate_tool --info <vertex.code> [<fragment.code>]   one program: its descriptors, linked like in game
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nx2_xlate.h"

namespace {

std::vector<unsigned char> ReadFile(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// A graphics stage starts with an SPH (type 1 or 2, shader type 1-5); anything else is a compute program.
bool IsCompute(const std::vector<unsigned char> &b) {
    if (b.size() < 4) return true;
    unsigned w;
    std::memcpy(&w, b.data(), 4);
    const unsigned sph_type = w & 0x1f, shader_type = (w >> 10) & 0xf;
    return !((sph_type == 1 || sph_type == 2) && shader_type >= 1 && shader_type <= 5);
}

const char *kKinds[] = {"cbuf", "ssbo", "texbuf", "imgbuf", "texture", "image"};

void PrintInfo(const Nx2XStage &s) {
    std::printf("stage %d: %zu words, generic loads %#x, colour stores %#x, rescaling %d, render area %d\n", s.stage,
                s.words, s.loads_generic, s.stores_frag, s.uses_rescaling, s.uses_render_area);
    for (int i = 0; i < s.ndesc; ++i) {
        const Nx2XDesc &d = s.desc[i];
        if (d.kind == NX2X_CBUF)
            std::printf("  binding %u: cbuf c%u x%u (used %#x bytes)\n", d.binding, d.index, d.count,
                        d.index < 18 ? s.cbuf_used_size[d.index] : 0);
        else
            std::printf("  binding %u: %s x%u at c%u[%#x] type %d depth %d int %d written %d secondary %d\n",
                        d.binding, kKinds[d.kind], d.count, d.cbuf_index, d.cbuf_offset, d.type, d.is_depth,
                        d.is_integer, d.is_written, d.has_secondary);
    }
}

}   // namespace

int main(int argc, char **argv) {
    if (argc >= 3 && !std::strcmp(argv[1], "--info")) {
        std::vector<std::vector<unsigned char>> files;
        std::vector<const void *> code;
        std::vector<size_t> size;
        for (int a = 2; a < argc; ++a) files.push_back(ReadFile(argv[a]));
        for (auto &f : files) code.push_back(f.data()), size.push_back(f.size());
        const int n = static_cast<int>(files.size());
        std::vector<Nx2XStage> out(n);
        Nx2XOptions opt{};
        opt.topology = 3;
        const int r = nx2_xlate_program(n, code.data(), size.data(), IsCompute(files[0]), &opt, out.data());
        if (r) {
            std::printf("FAIL stage %d: %s\n", r - 1, out[r - 1].error);
            return 1;
        }
        for (int i = 0; i < n; ++i) PrintInfo(out[i]);
        nx2_xlate_free(out.data(), n);
        return 0;
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <out dir> <file.code>...\n       %s --info <stage.code>...\n", argv[0],
                     argv[0]);
        return 2;
    }
    const std::filesystem::path outdir{argv[1]};
    std::filesystem::create_directories(outdir);
    int ok = 0, failed = 0;
    for (int a = 2; a < argc; ++a) {
        const std::filesystem::path in{argv[a]};
        auto bytes = ReadFile(argv[a]);
        const void *code = bytes.data();
        const size_t size = bytes.size();
        Nx2XStage out{};
        if (nx2_xlate_program(1, &code, &size, IsCompute(bytes), nullptr, &out)) {
            std::printf("FAIL %s: %s\n", in.filename().string().c_str(), out.error);
            ++failed;
            continue;
        }
        const auto path = outdir / (in.stem().string() + ".spv");
        std::ofstream o(path, std::ios::binary);
        o.write(reinterpret_cast<const char *>(out.spirv), static_cast<std::streamsize>(out.words * 4));
        std::printf("ok   %s stage %d -> %zu words\n", in.filename().string().c_str(), out.stage, out.words);
        nx2_xlate_free(&out, 1);
        ++ok;
    }
    std::printf("%d translated, %d failed\n", ok, failed);
    return failed ? 1 : 0;
}
