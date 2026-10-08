#pragma once
#include <cstddef>
#include <imgui.h>
#include <string_view>

// Hall Of Meat HUD art. ReSkate ships no game data: the PNGs are read from <game>/HallOfMeat/art
// (built from the player's own Skate 3 by tools/HallOfMeat/build_assets.py) and packed into the
// ImGui font atlas, like the chat emotes. Without them the HUD falls back to drawn shapes.
namespace dingosdk::overlay {
struct HomArt {
    ImTextureID texture{};
    ImVec2 uv0{}, uv1{};
    float width{}, height{};
};
// Render thread, before the atlas is built: reads the PNGs and reserves their room.
std::size_t reserve_hom_art(ImFontAtlas &) noexcept;
// After the atlas is built: copies the pixels in.
void fill_hom_art(ImFontAtlas &) noexcept;
// The picture called `name` (file name without .png), or false when it is not there.
bool hom_art(std::string_view name, HomArt &art) noexcept;
} // namespace dingosdk::overlay
