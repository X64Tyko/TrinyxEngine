#pragma once
#include "Types.h"

/**
 * ViewState: Camera/Projection data for rendering
 */
struct ViewState
{
	Matrix4 ViewMatrix;
	Matrix4 ProjectionMatrix;
	Vector3 CameraPosition;
};

/**
 * SceneState: Global scene environment
 */
struct SceneState
{
	Vector3 SunDirection;
	Vector3 SunColor;
};

/**
 * FramePacket: Communication protocol between Logic → Render threads
 *
 * Logic Thread produces these at FixedUpdateHz
 * Render Thread consumes via triple-buffer mailbox
 */
struct alignas(64) FramePacket
{
	ViewState View;
	SceneState Scene;

	// Timing
	double SimulationTime; // Current simulation time

	// Snapshot Metadata
	uint32_t ActiveEntityCount; // How many entities in the sparse arrays
	uint32_t FrameNumber;       // Increments each FixedUpdate, signals new data available

	void Clear()
	{
		ActiveEntityCount = 0;
		FrameNumber       = 0;
	}
};