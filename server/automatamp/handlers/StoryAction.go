package handlers

import (
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
)

// An action of a story script finished in the sender's game (the host): forwarded to the other player,
// whose game finishes the same action, and written to the server log.
func HandleStoryAction(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	action := nier.GetRootAsStoryAction(data.DataBytes(), 0)
	name := "?"
	if connection.Client != nil {
		name = connection.Client.Name
	}
	core.Event(server, "Story: %s finished action %08x of graph %08x with %08x in phase %s", name, action.Action(), action.Graph(), action.State(), string(action.Phase()))

	core.BroadcastPlayerPacketToAllExceptSender(server, sender, connection, nier.PacketTypeID_STORY_ACTION, data.DataBytes())
}
