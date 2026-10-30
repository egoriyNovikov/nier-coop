package handlers

import (
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
)

// The sender's character died in its own game: forwarded to the other player, whose game decides whether its
// character dies too (shared death, co-op concept 6 point 5), and written to the server log.
func HandlePlayerDied(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	name := "?"
	if connection.Client != nil {
		name = connection.Client.Name
	}
	core.Event(server, "Death: %s died", name)

	core.BroadcastPlayerPacketToAllExceptSender(server, sender, connection, nier.PacketTypeID_PLAYER_DIED, data.DataBytes())
}
