#pragma once
#include "AssetTypes.h"
#include "EditorPanel.h"
#include <filesystem>
#include <map>
#include <string>

struct AssetDatabaseEntry;

class ContentBrowserPanel : public EditorPanel
{
public:
	ContentBrowserPanel()
		: EditorPanel("Content Browser")
	{
	}
	void Draw(EditorState& state) override;

	enum class SourceFileKind : uint8_t
	{
		Unknown,
		Construct,
		Entity,
		Component
	};

private:
	void DrawToolbar(EditorState& state);
	void DrawFolderPane(EditorState& state);
	void DrawFolderNode(const std::filesystem::path& dir, int depth);
	void DrawAssetPane(EditorState& state);
	void DrawAssetTile(EditorState& state, const AssetDatabaseEntry& entry);
	void DrawAssetList(EditorState& state);
	void DrawAssetRow(EditorState& state, const AssetDatabaseEntry& entry);

	void DrawAssetContextMenu(EditorState& state, const AssetDatabaseEntry& entry);
	void DrawBlankContextMenu(EditorState& state);

	void OpenCreatePopup(bool forFolder, AssetType type = AssetType::Invalid);
	void DrawCreatePopup(EditorState& state);
	void CommitCreate(EditorState& state);

	bool MatchesFilter(const AssetDatabaseEntry& entry) const;

	// --- Source section ---
	static SourceFileKind DetectSourceKind(const std::filesystem::path& filePath);

	void DrawSourceFolderNode(const std::filesystem::path& dir, int depth);
	void DrawSourcePane(EditorState& state);
	void DrawSourceBlankContextMenu(EditorState& state);
	void OpenSourceCreatePopup(bool forConstruct);
	void DrawSourceCreatePopup(EditorState& state);
	void CommitSourceCreate(EditorState& state);

	// Cached per Draw() call
	std::string ContentRoot;
	std::string SourceRoot;

	// Active section (Content or Source)
	enum class BrowserSection : uint8_t
	{
		Content,
		Source
	};
	BrowserSection ActiveSection = BrowserSection::Content;

	// Navigation
	std::string CurrentFolder;       // relative to ContentRoot, forward-slash separated
	std::string SourceCurrentFolder; // relative to SourceRoot, forward-slash separated

	// Source kind cache — invalidated when SourceCurrentFolder changes
	mutable std::map<std::string, SourceFileKind> SourceKindCache;
	mutable std::string SourceKindCacheFolder;

	// Selection
	AssetID SelectedAsset;

	// View
	enum class ViewMode : uint8_t
	{
		Icons,
		List
	};
	ViewMode CurrentViewMode = ViewMode::Icons;
	float ThumbnailSize      = 72.0f;

	// Filter / search
	int TypeFilter      = 0;
	char SearchBuf[128] = {};

	// Rename modal
	bool bShowRename = false;
	AssetID RenamingID;
	char RenameBuf[256] = {};

	// Content create modal
	bool bShowCreate        = false;
	bool bCreateIsFolder    = false;
	AssetType CreateType    = AssetType::Invalid;
	char CreateNameBuf[256] = {};

	// Source create modal
	bool bShowSourceCreate      = false;
	bool bSourceCreateConstruct = true; // true=Construct, false=Entity
	char SourceCreateBuf[256]   = {};
};
