#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Receives each pack file by name.
using PackSink = std::function<void(const std::string& name, const void* data, size_t size)>;

// One runtime shader for the generator; a null container selects the container-less translation.
struct Cc1Shader
{
    std::string runtimeHash;
    bool isPixelShader = false;
    const uint8_t* container = nullptr;
    std::vector<uint8_t> code; // guest byte order, as the game hashes it
};

// main.cpp: null for a container the translator can parse, else the reason.
const char* validateContainer(const uint8_t* base);

// main.cpp: translates the shaders and hands the manifest and every DXIL file to the sink.
int buildCc1Shaders(std::vector<Cc1Shader>& shaders, std::string_view include, const PackSink& sink);

// cc1_pack.cpp: the --cc1-pack, --cc1-add-misses and --cc1-scan modes.
int runCc1(int argc, char** argv);
