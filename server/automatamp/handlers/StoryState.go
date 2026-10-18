package handlers

import (
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
)

// Story point of the sender's game: forwarded to the other player and written to the server log,
// so a session log shows where both games were in the story.
func HandleStoryState(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	state := nier.GetRootAsStoryState(data.DataBytes(), 0)
	name := "?"
	if connection.Client != nil {
		name = connection.Client.Name
	}
	core.Event(server, "Story: %s is in phase %s (%d), cutscene ev%04x, %d flag changes", name, string(state.Phase()), state.PhaseId(), state.Event(), state.SceneStateChanges())

	core.BroadcastPlayerPacketToAllExceptSender(server, sender, connection, nier.PacketTypeID_STORY_STATE, data.DataBytes())
}
