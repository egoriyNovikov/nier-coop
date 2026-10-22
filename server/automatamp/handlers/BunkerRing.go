package handlers

import (
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
)

// The angle of the Bunker's ring in the host's game: forwarded to the other player, whose game turns its ring the
// same. Sent while the ring turns, so it is not written to the server log.
func HandleBunkerRing(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	core.BroadcastPlayerPacketToAllExceptSender(server, sender, connection, nier.PacketTypeID_BUNKER_RING, data.DataBytes())
}
