#include <d3dcompiler.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
    try {
        std::ifstream input(std::filesystem::path(argv[1]), std::ios::binary);
        if (!input) throw std::runtime_error("Could not open shader source");
        const std::string source{std::istreambuf_iterator<char>(input), {}};
        std::ofstream output(std::filesystem::path(argv[2]), std::ios::binary);
        if (!output) throw std::runtime_error("Could not open shader bytecode header");
        output << "// Generated at build time; do not edit.\n#pragma once\n";
        const auto compile = [&](const char* entry, const char* profile, const char* symbol) {
            Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
            const HRESULT hr = D3DCompile(source.data(), source.size(), "capsule.hlsl", nullptr, nullptr,
                entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &code, &errors);
            if (FAILED(hr)) {
                if (errors) std::cerr.write(static_cast<const char*>(errors->GetBufferPointer()),
                    static_cast<std::streamsize>(errors->GetBufferSize()));
                throw std::runtime_error("Shader compilation failed");
            }
            output << "inline constexpr unsigned char " << symbol << "[]{\n";
            const auto* bytes = static_cast<const unsigned char*>(code->GetBufferPointer());
            for (size_t i = 0; i < code->GetBufferSize(); ++i) {
                output << static_cast<unsigned>(bytes[i]) << ',';
                if (i % 24 == 23) output << '\n';
            }
            output << "\n};\n";
        };
        compile("VSMain", "vs_5_0", "capsuleVertexShader");
        compile("PSMain", "ps_5_0", "capsulePixelShader");
        if (!output) throw std::runtime_error("Could not write shader bytecode");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
