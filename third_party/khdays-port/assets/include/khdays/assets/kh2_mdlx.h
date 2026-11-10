#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "khdays/assets/model_import.h"

namespace khdays::assets::kh2 {

struct MdlxMeshInfo {
    struct Weight {
        int joint;
        std::array<float, 4> position; // Weighted local xyz in metres, and weight.
    };
    std::uint32_t attribute;
    std::vector<std::uint32_t> weight_ends;
    std::vector<Weight> weights;
};

using MdlxPicture = std::function<std::optional<DecodedTexture>(int)>;

// KH2's models (obj/<name>.mdlx) imported like a mod's glTF: in metres (KH2's are
// centimetres), Y up, its bones the joints (kh2_<bone index>) at rest as the file
// places them; textures decoded from the PS2's or, where `hd_folder` has them, its
// -N.png or -N.dds (docs/KH2_MODELS.md); a file's is the folder beside it with its
// name. Throws std::runtime_error naming what it cannot read.
ImportedModel import_mdlx(const std::filesystem::path& path);
ImportedModel import_mdlx(const std::uint8_t* data, std::size_t size, const std::string& name,
                          const std::filesystem::path& hd_folder = {},
                          const MdlxPicture& remastered = {}, std::vector<MdlxMeshInfo>* mesh_info = nullptr);

// The joint name given to bone `bone`.
std::string mdlx_joint_name(int bone);

}  // namespace khdays::assets::kh2
