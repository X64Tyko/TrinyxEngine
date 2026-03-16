#include "EditorContext.h"
#include "EditorPanel.h"
#include "EntityBuilder.h"
#include "JoltPhysics.h"
#include "TrinyxEngine.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "Logger.h"
#include "LogicThread.h"
#include "Registry.h"
#include <cstring>

// Panel headers
#include "Panels/WorldOutlinerPanel.h"
#include "Panels/DetailsPanel.h"
#include "Panels/EngineStatsPanel.h"
#include "Panels/LogPanel.h"
#include "Panels/ContentBrowserPanel.h"

EditorContext::EditorContext()  = default;
EditorContext::~EditorContext() = default;

void EditorContext::Initialize(TrinyxEngine* engine, LogicThread* logic)
{
	EnginePtr = engine;
	LogicPtr  = logic;

	// Populate shared state pointers for panels
	State.EnginePtr   = engine;
	State.RegistryPtr = engine->GetRegistry();
	State.ConfigPtr   = engine->GetConfig();
	State.LogicPtr    = logic;
	State.EditorCtx   = this;

	// Initialize asset database from project content directory
	const EngineConfig* cfg = engine->GetConfig();
	if (cfg->ProjectDir[0] != '\0')
	{
		std::string contentDir = std::string(cfg->ProjectDir) + "/content";
		AssetDB.Initialize(contentDir.c_str());
		State.AssetDB = &AssetDB;
	}

	// Load default scene if configured
	if (cfg->DefaultScene[0] != '\0' && cfg->ProjectDir[0] != '\0')
	{
		std::string scenePath = std::string(cfg->ProjectDir) + "/content/" + cfg->DefaultScene;
		LoadScene(scenePath, false);
	}

	// Register all panels
	AddPanel<WorldOutlinerPanel>();
	AddPanel<DetailsPanel>();
	AddPanel<EngineStatsPanel>();
	AddPanel<LogPanel>();
	AddPanel<ContentBrowserPanel>();

	LOG_INFO_F("[Editor] Initialized with %zu panels", Panels.size());
}

void EditorContext::LoadScene(const std::string& path, bool bReset)
{
	EnginePtr->Spawn([path, bReset](Registry* reg)
	{
		if (bReset) reg->ResetRegistry();
		EntityBuilder::SpawnFromFile(reg, path.c_str());
	});

	State.CurrentScenePath = path;
	State.CurrentSceneName = path;
	size_t lastSlash       = State.CurrentSceneName.find_last_of('/');
	if (lastSlash != std::string::npos) State.CurrentSceneName = State.CurrentSceneName.substr(lastSlash + 1);
	size_t dot = State.CurrentSceneName.find_last_of('.');
	if (dot != std::string::npos) State.CurrentSceneName = State.CurrentSceneName.substr(0, dot);
	State.bSceneDirty = false;
	State.ClearSelection();
}

void EditorContext::BuildFrame()
{
	BuildDockspace();

	// Draw all panels
	for (auto& panel : Panels)
	{
		panel->Tick(State);
	}

	// Modals / overlays
	DrawFileDialog();
	DrawUnsavedWarning();

	// Debug windows
	if (bShowDemoWindow) ImGui::ShowDemoWindow(&bShowDemoWindow);
	if (bShowMetrics) ImGui::ShowMetricsWindow(&bShowMetrics);
}

void EditorContext::BuildDockspace()
{
	// Full-viewport dockspace with menu bar
	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar
		| ImGuiWindowFlags_NoDocking
		| ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoNavFocus
		| ImGuiWindowFlags_NoBackground;

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->WorkPos);
	ImGui::SetNextWindowSize(viewport->WorkSize);
	ImGui::SetNextWindowViewport(viewport->ID);

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

	ImGui::Begin("EditorDockspace", nullptr, windowFlags);
	ImGui::PopStyleVar(3);

	ImGuiID dockspaceID = ImGui::GetID("EditorDockspaceID");
	ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);

	// Apply default layout on first frame
	if (bFirstFrame)
	{
		ApplyDefaultLayout(dockspaceID);
		bFirstFrame = false;
	}

	BuildMenuBar();

	ImGui::End();
}

void EditorContext::ApplyDefaultLayout(unsigned int dockspaceID)
{
	ImGui::DockBuilderRemoveNode(dockspaceID);
	ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
	ImGui::DockBuilderSetNodeSize(dockspaceID, ImGui::GetMainViewport()->WorkSize);

	// Split: bottom 25% for tabbed panels, top 75% for main area
	ImGuiID top, bottom;
	ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Down, 0.25f, &bottom, &top);

	// Split top: left 15% for outliner, remainder for center+right
	ImGuiID left, centerRight;
	ImGui::DockBuilderSplitNode(top, ImGuiDir_Left, 0.15f, &left, &centerRight);

	// Split center+right: right 25% for details, center for viewport
	ImGuiID right, center;
	ImGui::DockBuilderSplitNode(centerRight, ImGuiDir_Right, 0.25f, &right, &center);

	// Dock panels
	ImGui::DockBuilderDockWindow("World Outliner", left);
	ImGui::DockBuilderDockWindow("Details", right);

	// Bottom: tabbed — Content Browser, Log, Engine Stats
	ImGui::DockBuilderDockWindow("Content Browser", bottom);
	ImGui::DockBuilderDockWindow("Log", bottom);
	ImGui::DockBuilderDockWindow("Engine Stats", bottom);

	// Mark center node as the viewport (passthru so the 3D scene shows through)
	ImGuiDockNode* centerNode = ImGui::DockBuilderGetNode(center);
	if (centerNode)
	{
		centerNode->LocalFlags |= ImGuiDockNodeFlags_CentralNode;
	}

	ImGui::DockBuilderFinish(dockspaceID);
}

void EditorContext::BuildMenuBar()
{
	if (!ImGui::BeginMenuBar()) return;

	if (ImGui::BeginMenu("File"))
	{
		if (ImGui::MenuItem("Open Scene...", "Ctrl+O"))
		{
			if (State.bSceneDirty)
			{
				bShowUnsavedWarning = true;
				PendingAction       = PendingActionType::OpenScene;
			}
			else
			{
				bShowFileDialog    = true;
				bFileDialogForSave = false;
				FileDialogPath     = State.CurrentScenePath;
			}
		}
		if (ImGui::MenuItem("Save Scene", "Ctrl+S", false, !State.CurrentScenePath.empty()))
		{
			EntityBuilder::SaveToFile(State.RegistryPtr, State.CurrentSceneName.c_str(),
									  State.CurrentScenePath.c_str());
			State.bSceneDirty = false;
		}
		if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S"))
		{
			bShowFileDialog    = true;
			bFileDialogForSave = true;
			FileDialogPath     = State.CurrentScenePath;
		}
		ImGui::Separator();
		ImGui::MenuItem("Exit", nullptr, false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Edit"))
	{
		ImGui::MenuItem("Undo", "Ctrl+Z", false, false);
		ImGui::MenuItem("Redo", "Ctrl+Y", false, false);
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("View"))
	{
		for (auto& panel : Panels)
		{
			ImGui::MenuItem(panel->Title, nullptr, &panel->bVisible);
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Reset Layout"))
		{
			bFirstFrame = true;
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Play"))
	{
		bool simPaused = !LogicPtr || LogicPtr->IsSimPaused();

		if (ImGui::MenuItem("Play", nullptr, false, simPaused))
		{
			if (LogicPtr)
			{
				if (!bHasSnapshot) SnapshotScene();
				LogicPtr->SetSimPaused(false);
			}
		}
		if (ImGui::MenuItem("Pause", nullptr, false, !simPaused))
		{
			if (LogicPtr) LogicPtr->SetSimPaused(true);
		}
		if (ImGui::MenuItem("Stop", nullptr, false, bHasSnapshot))
		{
			if (LogicPtr) LogicPtr->SetSimPaused(true);
			RestoreSnapshot();
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("Debug"))
	{
		ImGui::MenuItem("Show Demo Window", nullptr, &bShowDemoWindow);
		ImGui::MenuItem("Show ImGui Metrics", nullptr, &bShowMetrics);
		ImGui::EndMenu();
	}

	// Scene name indicator (right-aligned in menu bar)
	{
		char sceneLabel[256];
		snprintf(sceneLabel, sizeof(sceneLabel), "%s%s",
				 State.bSceneDirty ? "* " : "",
				 State.CurrentSceneName.c_str());

		float textWidth = ImGui::CalcTextSize(sceneLabel).x;
		float available = ImGui::GetContentRegionAvail().x;
		if (available > textWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - textWidth);

		ImGui::TextDisabled("%s", sceneLabel);
	}

	ImGui::EndMenuBar();
}

void EditorContext::DrawFileDialog()
{
	if (!bShowFileDialog) return;

	ImGui::OpenPopup("Scene File");

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(500, 120), ImGuiCond_Appearing);

	if (ImGui::BeginPopupModal("Scene File", &bShowFileDialog, ImGuiWindowFlags_AlwaysAutoResize))
	{
		char pathBuf[512];
		snprintf(pathBuf, sizeof(pathBuf), "%s", FileDialogPath.c_str());

		ImGui::Text(bFileDialogForSave ? "Save scene to:" : "Open scene from:");
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##path", pathBuf, sizeof(pathBuf));
		FileDialogPath = pathBuf;

		ImGui::Separator();

		if (ImGui::Button(bFileDialogForSave ? "Save" : "Open", ImVec2(120, 0)))
		{
			if (bFileDialogForSave)
			{
				// Extract scene name from path
				std::string name = FileDialogPath;
				size_t lastSlash = name.find_last_of('/');
				if (lastSlash != std::string::npos) name = name.substr(lastSlash + 1);
				size_t dot = name.find_last_of('.');
				if (dot != std::string::npos) name = name.substr(0, dot);

				EntityBuilder::SaveToFile(State.RegistryPtr, name.c_str(), FileDialogPath.c_str());
				State.CurrentScenePath = FileDialogPath;
				State.CurrentSceneName = name;
				State.bSceneDirty      = false;
			}
			else
			{
				LoadScene(FileDialogPath);
			}

			bShowFileDialog = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)))
		{
			bShowFileDialog = false;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}

void EditorContext::SnapshotScene()
{
	PlaySnapshot.clear();

	Registry* reg = State.RegistryPtr;
	if (!reg) return;

	uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
	uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();

	for (auto& [key, arch] : reg->GetArchetypes())
	{
		size_t fieldCount = arch->CachedFieldArrayLayout.size();
		if (fieldCount == 0) continue;

		ArchetypeSnapshot archSnap;
		archSnap.ArchClassID      = arch->ArchClassID;
		archSnap.TotalEntityCount = arch->TotalEntityCount;

		for (size_t ci = 0; ci < arch->Chunks.size(); ++ci)
		{
			Chunk* chunk         = arch->Chunks[ci];
			uint32_t entityCount = arch->GetChunkCount(ci);
			if (entityCount == 0) continue;

			void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
			arch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

			// Calculate total bytes needed for all decomposed fields in this chunk
			size_t totalBytes = 0;
			for (size_t i = 0; i < fieldCount; ++i)
			{
				const auto& desc = arch->CachedFieldArrayLayout[i];
				if (desc.isDecomposed && fieldArrayTable[i]) totalBytes += desc.Size * entityCount;
			}

			ArchetypeSnapshot::ChunkData chunkData;
			chunkData.Chunk       = chunk;
			chunkData.EntityCount = entityCount;
			chunkData.FieldData.resize(totalBytes);

			// Copy field data into snapshot
			size_t offset = 0;
			for (size_t i = 0; i < fieldCount; ++i)
			{
				const auto& desc = arch->CachedFieldArrayLayout[i];
				if (!desc.isDecomposed || !fieldArrayTable[i]) continue;

				size_t bytes = desc.Size * entityCount;
				std::memcpy(chunkData.FieldData.data() + offset, fieldArrayTable[i], bytes);
				offset += bytes;
			}

			archSnap.Chunks.push_back(std::move(chunkData));
		}

		if (!archSnap.Chunks.empty()) PlaySnapshot.push_back(std::move(archSnap));
	}

	bHasSnapshot = true;
	LOG_INFO("[Editor] Scene snapshot taken for Play session");
}

void EditorContext::RestoreSnapshot()
{
	if (!bHasSnapshot) return;

	EnginePtr->Spawn([this](Registry* reg)
	{
		if (!reg) return;

		// Reset all Jolt bodies — Play may have created bodies that don't exist in the snapshot.
		// FlushPendingBodies will recreate them from the restored field data on the next physics tick.
		if (EnginePtr->Physics) EnginePtr->Physics->ResetAllBodies();

		uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
		uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();

		for (auto& archSnap : PlaySnapshot)
		{
			// Find the archetype by ClassID
			Archetype* ownerArch = nullptr;
			for (auto& [key, arch] : reg->GetArchetypes())
			{
				if (arch->ArchClassID == archSnap.ArchClassID)
				{
					ownerArch = arch;
					break;
				}
			}

			if (!ownerArch) continue;

			size_t fieldCount = ownerArch->CachedFieldArrayLayout.size();

			// Restore per-chunk field data
			for (auto& chunkSnap : archSnap.Chunks)
			{
				Chunk* chunk = static_cast<Chunk*>(chunkSnap.Chunk);

				void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
				ownerArch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

				// Restore field data — only for the entity count we snapshotted.
				// If the chunk now has more entities (spawned during Play), the snapshot
				// data covers only the original ones; extras get their Active flag cleared below.
				size_t offset = 0;
				for (size_t i = 0; i < fieldCount; ++i)
				{
					const auto& desc = ownerArch->CachedFieldArrayLayout[i];
					if (!desc.isDecomposed || !fieldArrayTable[i]) continue;

					size_t bytes = desc.Size * chunkSnap.EntityCount;
					std::memcpy(fieldArrayTable[i], chunkSnap.FieldData.data() + offset, bytes);
					offset += bytes;
				}

				// Mark all snapshotted entities dirty so the GPU picks up the restore
				size_t cacheStart = chunk->Header.CacheIndexStart;
				auto* dirtyBits   = reg->DirtyBitsFrame(temporalFrame);
				for (uint32_t e = 0; e < chunkSnap.EntityCount; ++e)
				{
					size_t idx     = cacheStart + e;
					uint64_t& word = (*dirtyBits)[idx / 64];
					word           |= uint64_t(1) << (idx % 64);
				}
			}

			// Handle entities created during Play: tombstone them by clearing Active flag.
			// The snapshot restores the original field data (including Active flags for original
			// entities). Entities beyond the snapshot count need to be deactivated.
			if (ownerArch->TotalEntityCount > archSnap.TotalEntityCount)
			{
				uint32_t extraCount = ownerArch->TotalEntityCount - archSnap.TotalEntityCount;
				LOG_INFO_F("[Editor] Tombstoning %u entities created during Play in archetype %u",
						   extraCount, archSnap.ArchClassID);

				// Find the TemporalFlags field (Active flag is bit 31 of TemporalFlags::Flags).
				// Walk entities beyond the snapshot count and clear their Active flag.
				uint32_t entityIdx = archSnap.TotalEntityCount;
				while (entityIdx < ownerArch->TotalEntityCount)
				{
					uint32_t chunkIdx = entityIdx / ownerArch->EntitiesPerChunk;
					uint32_t localIdx = entityIdx % ownerArch->EntitiesPerChunk;

					if (chunkIdx < ownerArch->Chunks.size())
					{
						Chunk* chunk = ownerArch->Chunks[chunkIdx];

						// Find the Flags field in the layout and clear the Active bit
						void* fieldArrayTable[MAX_FIELDS_PER_ARCHETYPE];
						ownerArch->BuildFieldArrayTable(chunk, fieldArrayTable, temporalFrame, volatileFrame);

						for (size_t i = 0; i < fieldCount; ++i)
						{
							const auto& desc = ownerArch->CachedFieldArrayLayout[i];
							if (!desc.isDecomposed || !fieldArrayTable[i]) continue;

							// TemporalFlags::Flags is an int32 field — check by ValueType + Size
							if (desc.ValueType == FieldValueType::Int32 && desc.Size == sizeof(int32_t))
							{
								int32_t* flags  = static_cast<int32_t*>(fieldArrayTable[i]);
								flags[localIdx] &= ~static_cast<int32_t>(1u << 31); // Clear Active bit
							}
						}

						// Mark dirty for GPU
						size_t cacheStart = chunk->Header.CacheIndexStart;
						auto* dirtyBits   = reg->DirtyBitsFrame(temporalFrame);
						size_t idx        = cacheStart + localIdx;
						uint64_t& word    = (*dirtyBits)[idx / 64];
						word              |= uint64_t(1) << (idx % 64);
					}

					entityIdx++;
				}
			}
			else if (ownerArch->TotalEntityCount < archSnap.TotalEntityCount)
			{
				// Entities were deleted during Play (swap-and-pop). Field data for surviving
				// entities has been restored, but the deleted ones can't be reconstructed without
				// full entity records. This will be properly solved by PIE world duplication.
				LOG_INFO_F("[Editor] Warning: %u entities were deleted during Play in archetype %u — "
						   "deleted entities cannot be restored (PIE world duplication needed)",
						   archSnap.TotalEntityCount - ownerArch->TotalEntityCount, archSnap.ArchClassID);
			}
		}
	});

	// Clear selection since entity indices may have changed
	State.ClearSelection();

	PlaySnapshot.clear();
	bHasSnapshot = false;
	LOG_INFO("[Editor] Scene restored from snapshot");
}

void EditorContext::DrawUnsavedWarning()
{
	if (!bShowUnsavedWarning) return;

	ImGui::OpenPopup("Unsaved Changes");

	ImVec2 center = ImGui::GetMainViewport()->GetCenter();
	ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

	if (ImGui::BeginPopupModal("Unsaved Changes", &bShowUnsavedWarning, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Scene \"%s\" has unsaved changes.", State.CurrentSceneName.c_str());
		ImGui::Text("Do you want to save before continuing?");
		ImGui::Separator();

		if (ImGui::Button("Save", ImVec2(100, 0)))
		{
			if (!State.CurrentScenePath.empty())
			{
				EntityBuilder::SaveToFile(State.RegistryPtr, State.CurrentSceneName.c_str(),
										  State.CurrentScenePath.c_str());
				State.bSceneDirty = false;
			}

			// Proceed with pending action
			if (PendingAction == PendingActionType::OpenScene)
			{
				bShowFileDialog    = true;
				bFileDialogForSave = false;
				FileDialogPath     = State.CurrentScenePath;
			}

			bShowUnsavedWarning = false;
			PendingAction       = PendingActionType::None;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Discard", ImVec2(100, 0)))
		{
			State.bSceneDirty = false;

			if (PendingAction == PendingActionType::OpenScene)
			{
				bShowFileDialog    = true;
				bFileDialogForSave = false;
				FileDialogPath     = State.CurrentScenePath;
			}

			bShowUnsavedWarning = false;
			PendingAction       = PendingActionType::None;
			ImGui::CloseCurrentPopup();
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(100, 0)))
		{
			bShowUnsavedWarning = false;
			PendingAction       = PendingActionType::None;
			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}
}
