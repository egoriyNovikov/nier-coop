package handlers

import (
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
)

func HandleEnemyDied(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	// Each game has its own enemies, the other player kills the matching one.
	core.BroadcastPlayerPacketToAllExceptSender(server, sender, connection, nier.PacketTypeID_ENEMY_DIED, data.DataBytes())
}
