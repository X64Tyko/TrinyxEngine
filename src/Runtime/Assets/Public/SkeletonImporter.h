#pragma once
#include <string>
#include <vector>

#include "MeshAsset.h"
#include "SkeletonAsset.h"
#include "AnimationAsset.h"
#include "TnxName.h"

struct SkeletalImportResult
{
	MeshAsset mesh; // geometry + skin weights (mesh.IsSkinned() == true on success)
	SkeletonAsset skeleton;

	std::vector<AnimationAsset> animations;
	std::vector<TnxName> animNames; // animNames[i] matches animations[i]

	bool IsValid() const { return mesh.IsValid() && mesh.IsSkinned() && skeleton.boneCount > 0; }
};

/// Imports geometry, skin weights, skeleton, and animations from a single glTF/glb file.
bool ImportSkeletalGLTF(const std::string& srcPath, SkeletalImportResult& outResult);
