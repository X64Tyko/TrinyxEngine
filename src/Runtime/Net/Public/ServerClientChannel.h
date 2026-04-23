#pragma once
#include "NetChannel.h"
#include "PlayerInputLog.h"
#include <atomic>
#include <cstdint>
#include <vector>

class NetConnectionManager;
struct ConnectionInfo;
struct PacketHeader;

// A built packet waiting to be sent on the Sentinel drain pass.
struct PendingPacket
{
	PacketHeader       Header;
	std::vector<uint8_t> Payload;
	bool               Reliable = false;
};

// Lock-free MPSC queue for PendingPacket.
// Workers push(); Sentinel drains via Drain() after all build jobs complete.
// Drain reverses the list so packets send in dispatch order.
struct PendingPacketQueue
{
	struct Node
	{
		PendingPacket Packet;
		Node*         Next = nullptr;
	};

	void Push(PendingPacket&& pkt)
	{
		Node* node  = new Node{ std::move(pkt), nullptr };
		Node* prev  = Head.load(std::memory_order_relaxed);
		do { node->Next = prev; }
		while (!Head.compare_exchange_weak(prev, node, std::memory_order_release, std::memory_order_relaxed));
	}

	// Drain all pending packets into out (in dispatch order). Caller owns the nodes after drain.
	void Drain(std::vector<PendingPacket>& out)
	{
		Node* list = Head.exchange(nullptr, std::memory_order_acquire);

		// Reverse the intrusive list to restore dispatch order.
		Node* reversed = nullptr;
		while (list)
		{
			Node* next  = list->Next;
			list->Next  = reversed;
			reversed    = list;
			list        = next;
		}

		while (reversed)
		{
			out.push_back(std::move(reversed->Packet));
			Node* next = reversed->Next;
			delete reversed;
			reversed   = next;
		}
	}

	bool IsEmpty() const { return Head.load(std::memory_order_relaxed) == nullptr; }

private:
	std::atomic<Node*> Head{ nullptr };
};

// Authority-side state for one connected Owner.
// Owns the input log, per-client entity spawn tracking, NetChannel, and outbound packet queue.
struct ServerClientChannel
{
	PlayerInputLog       InputLog;
	std::vector<bool>    Replicated;
	NetChannel           Channel;
	PendingPacketQueue   SendQueue;
	ConnectionInfo*      CI      = nullptr;
	uint8_t              OwnerID = 0;

	void Open(uint8_t ownerID, uint32_t logDepth, ConnectionInfo* ci,
	          NetConnectionManager* mgr, uint32_t entityCapacity = 0);
	void Close();

	bool IsReplicated(uint32_t index) const { return index < Replicated.size() && Replicated[index]; }

	void MarkReplicated(uint32_t index)
	{
		if (index >= Replicated.size()) Replicated.resize(index + 1, false);
		Replicated[index] = true;
	}

	void ClearReplicated(uint32_t index)
	{
		if (index < Replicated.size()) Replicated[index] = false;
	}

	void EnsureCapacity(uint32_t count)
	{
		if (Replicated.size() < count) Replicated.resize(count, false);
	}
};

