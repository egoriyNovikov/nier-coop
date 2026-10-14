package handlers

import (
	flatbuffers "github.com/google/flatbuffers/go"
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

// The master client leads the story (the Host in docs/COOP_CONCEPT.md).
func GetMasterClient(server *structs.Server) *structs.Client {
	for _, client := range server.Clients {
		if client.IsMasterClient {
			return client
		}
	}

	return nil
}

func FindWantsHost(server *structs.Server) *structs.Client {
	for _, client := range server.Clients {
		if client.WantsHost {
			return client
		}
	}

	return nil
}

// Tells everyone who leads the story now, guid 0 when nobody does.
func BroadcastMasterClient(server *structs.Server, guid uint64) {
	bytes := core.BuilderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
		return nier.CreateSetMasterClient(builder, guid)
	})

	core.BroadcastPacketToAll(server, nier.PacketTypeID_SET_MASTER_CLIENT, bytes)
}
