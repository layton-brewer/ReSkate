#include "hom_art.h"
#include "Engine/Core/Log/logging.h"
#include <Windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

namespace dingosdk::overlay {
namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

struct Picture {
    int width{}, height{};
    std::vector<unsigned char> rgba;
    int rect{-1};
};
struct Loaded {
    std::map<std::string, Picture, std::less<>> pictures;
    ImFontAtlas *atlas{};
    bool filled{};
};
Loaded &loaded() {
    static Loaded value;
    return value;
}

bool decode(const fs::path &path, Picture &picture) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width{}, height{};
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 1024 ||
        height > 1024)
        return false;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    picture.width = static_cast<int>(width);
    picture.height = static_cast<int>(height);
    picture.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(picture.rgba.size()), picture.rgba.data()));
}

fs::path art_directory() {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return {};
    path.resize(length);
    return fs::path(path).parent_path() / L"HallOfMeat" / L"art";
}
} // namespace

std::size_t reserve_hom_art(ImFontAtlas &atlas) noexcept {
    try {
        auto &l = loaded();
        l = {};
        const auto directory = art_directory();
        std::error_code error;
        if (directory.empty() || !fs::is_directory(directory, error)) return 0;
        const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (const auto &entry : fs::directory_iterator(directory, error)) {
            if (!entry.is_regular_file() || entry.path().extension() != L".png") continue;
            Picture picture;
            if (!decode(entry.path(), picture)) continue;
            const auto name = entry.path().stem().string();
            picture.rect = atlas.AddCustomRectRegular(picture.width, picture.height);
            l.pictures.emplace(name, std::move(picture));
        }
        if (SUCCEEDED(hr)) CoUninitialize();
        l.atlas = &atlas;
        atlas.Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
        logging::log(logging::Level::info, logging::Channel::graphics, "Hall Of Meat: {} pictures reserved.", l.pictures.size());
        return l.pictures.size();
    } catch (...) {
        loaded() = {};
        return 0;
    }
}

void fill_hom_art(ImFontAtlas &atlas) noexcept {
    auto &l = loaded();
    if (l.atlas != &atlas || l.pictures.empty()) return;
    try {
        unsigned char *pixels{};
        int width{}, height{};
        atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
        if (!pixels || width <= 0 || height <= 0) { l = {}; return; }
        for (auto &[name, picture] : l.pictures) {
            const auto *rect = atlas.GetCustomRectByIndex(picture.rect);
            if (!rect || !rect->IsPacked() || rect->X + picture.width > width || rect->Y + picture.height > height) {
                picture.rect = -1;
                continue;
            }
            for (int y = 0; y < picture.height; ++y)
                std::memcpy(pixels + ((static_cast<std::size_t>(rect->Y) + y) * width + rect->X) * 4,
                            picture.rgba.data() + static_cast<std::size_t>(y) * picture.width * 4,
                            static_cast<std::size_t>(picture.width) * 4);
        }
        atlas.TexPixelsUseColors = true;
        l.filled = true;
    } catch (...) {
        l = {};
    }
}

bool hom_art(std::string_view name, HomArt &art) noexcept {
    const auto &l = loaded();
    if (!l.filled || !l.atlas || !ImGui::GetCurrentContext() || ImGui::GetIO().Fonts != l.atlas || !l.atlas->TexID) return false;
    const auto it = l.pictures.find(name);
    if (it == l.pictures.end() || it->second.rect < 0) return false;
    const auto *rect = l.atlas->GetCustomRectByIndex(it->second.rect);
    if (!rect || !rect->IsPacked()) return false;
    art.texture = l.atlas->TexID;
    l.atlas->CalcCustomRectUV(rect, &art.uv0, &art.uv1);
    art.width = static_cast<float>(it->second.width);
    art.height = static_cast<float>(it->second.height);
    return true;
}
} // namespace dingosdk::overlay
