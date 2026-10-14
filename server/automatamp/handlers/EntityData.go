package handlers

import (
	"github.com/codecat/go-enet"
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

func HandleEntityData(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	if !connection.Client.IsMasterClient {
		return
	}

	core.BroadcastPacketToAllExceptSender(server, sender, nier.PacketTypeID_ENTITY_DATA, data.DataBytes())
}
