package handlers

import (
	"github.com/codecat/go-enet"
	flatbuffers "github.com/google/flatbuffers/go"

	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

func HandleDestroyEntity(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	if !connection.Client.IsMasterClient {
		return
	}

	// Destroy the entity.
	entityPkt := &nier.EntityPacket{}
	flatbuffers.GetRootAs(data.DataBytes(), 0, entityPkt)

	delete(server.Entities, entityPkt.Guid())

	core.BroadcastPacketToAllExceptSender(server, sender, nier.PacketTypeID_DESTROY_ENTITY, data.DataBytes())
}
