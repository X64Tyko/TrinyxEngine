#pragma once
#include "NetChannel.h"
#include "PlayerInputLog.h"
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

class NetConnectionManager;
struct ConnectionInfo;
struct PacketHeader;

/// @brief A fully built packet waiting for the Sentinel drain pass to send it.
struct PendingPacket
{
	PacketHeader Header;
	std::vector<uint8_t> Payload;
	bool Reliable = false;
};

/// @brief Lock-free MPSC queue for @ref PendingPacket.
///
/// Worker threads call @c Push(); Sentinel drains via @c Drain() after all
/// build jobs complete. @c Drain reverses the intrusive list to restore
/// dispatch order.
struct PendingPacketQueue
{
	struct Node
	{
		PendingPacket Packet;
		Node* Next = nullptr;
	};

	PendingPacketQueue()                                     = default;
	PendingPacketQueue(const PendingPacketQueue&)            = delete;
	PendingPacketQueue& operator=(const PendingPacketQueue&) = delete;

	/// Frees packets that were pushed but never drained (e.g. client dropped mid-frame).
	~PendingPacketQueue()
	{
		Node* list = Head.exchange(nullptr, std::memory_order_acquire);
		while (list)
		{
			Node* next = list->Next;
			delete list;
			list = next;
		}
	}

	// Ownership of each node passes to Head via the CAS; the static analyzer can't follow the
	// atomic and reports a leak. Nodes are freed by Drain() or the destructor.
	// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
	void Push(PendingPacket&& pkt)
	{
		Node* node = new Node{ std::move(pkt), nullptr };
		Node* prev = Head.load(std::memory_order_relaxed);
		do
		{
			node->Next = prev;
		} while (!Head.compare_exchange_weak(prev, node, std::memory_order_release, std::memory_order_relaxed));
	}
	// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)

	/// @brief Drain all pending packets into @p out in dispatch order.
	/// @param[out] out Receives packets in the order they were pushed.
	void Drain(std::vector<PendingPacket>& out)
	{
		Node* list = Head.exchange(nullptr, std::memory_order_acquire);

		// Reverse the intrusive list to restore dispatch order.
		Node* reversed = nullptr;
		while (list)
		{
			Node* next = list->Next;
			list->Next = reversed;
			reversed   = list;
			list       = next;
		}

		while (reversed)
		{
			out.push_back(std::move(reversed->Packet));
			Node* next = reversed->Next;
			delete reversed;
			reversed = next;
		}
	}

	bool IsEmpty() const { return Head.load(std::memory_order_relaxed) == nullptr; }

private:
	std::atomic<Node*> Head{ nullptr };
};

/// @brief Authority-side state for one connected Owner. The unit of per-client replication.
///
/// Owns the inbound @ref PlayerInputLog, per-entity @c Replicated[] bitvector,
/// @ref NetChannel typed send wrapper, and outbound @ref PendingPacketQueue.
/// Lives inside the World's @ref ReplicationSystem — PIE worlds are isolated naturally.
struct ServerClientChannel
{
	PlayerInputLog InputLog;                           ///< Inbound input frames from this Owner.
	std::vector<bool> Replicated;                      ///< Per-slab-index spawn tracking; true = EntitySpawn sent.
	std::vector<uint32_t> PendingActivations;          ///< Net handle values queued for EntityActivate; drained once Playing.
	NetChannel Channel;                                ///< Typed per-connection send wrapper.
	PendingPacketQueue SendQueue;                      ///< Worker-push / Sentinel-drain MPSC queue.
	std::optional<PendingPacket> PendingPlayerConfirm; ///< Held until spawns are in SendQueue; pushed in Flush().
	uint32_t PendingPlayerConfirmSpawnFrame = 0;       ///< Minimum LastDispatchedFrame before confirm may be pushed.
	ConnectionInfo* CI                      = nullptr; ///< GNS connection state (non-owning).
	uint8_t OwnerID                         = 0;       ///< Stable session identity (1–255; 0 = server).
	uint32_t LastAckedSimFrame              = 0;       ///< Last simulation frame this client confirmed receiving.

	/// @brief Open the channel — allocates the input log and sets @c OwnerID.
	void Open(uint8_t ownerID, uint32_t logDepth, ConnectionInfo* ci,
		NetConnectionManager* mgr, uint32_t entityCapacity = 0);

	/// @brief Close the channel and release all resources.
	void Close();

	/// @return @c true if a @c EntitySpawn has been sent for @p index.
	bool IsReplicated(uint32_t index) const { return index < Replicated.size() && Replicated[index]; }

	/// @brief Mark slab index @p index as replicated (EntitySpawn sent).
	void MarkReplicated(uint32_t index)
	{
		if (index >= Replicated.size()) Replicated.resize(index + 1, false);
		Replicated[index] = true;
	}

	/// @brief Clear the replicated flag for @p index (entity destroyed).
	void ClearReplicated(uint32_t index)
	{
		if (index < Replicated.size()) Replicated[index] = false;
	}

	/// @brief Grow the @c Replicated vector to cover at least @p count slots.
	void EnsureCapacity(uint32_t count)
	{
		if (Replicated.size() < count) Replicated.resize(count, false);
	}
};
