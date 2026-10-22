package handlers

import (
	"github.com/codecat/go-enet"
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

func PacketHandler(server *structs.Server, connection *structs.Connection, sender enet.Peer, data []byte) {
	packetData := nier.GetRootAsPacket(data, 0)

	switch packetData.Id() {
	case nier.PacketTypeID_HELLO:
		HandleHello(server, sender, connection, packetData)
	case nier.PacketTypeID_PING:
		HandlePing(server, sender, connection)
	case nier.PacketTypeID_PONG:
		HandlePong(server, connection)
	case nier.PacketTypeID_PLAYER_DATA:
		HandlePlayerData(server, sender, connection, packetData)
	case nier.PacketTypeID_ANIMATION_START:
		HandleAnimationStart(server, sender, connection, packetData)
	case nier.PacketTypeID_BUTTONS:
		HandleButtons(server, sender, connection, packetData)
	case nier.PacketTypeID_ENEMY_DIED:
		HandleEnemyDied(server, sender, connection, packetData)
	case nier.PacketTypeID_STORY_STATE:
		HandleStoryState(server, sender, connection, packetData)
	case nier.PacketTypeID_STORY_ACTION:
		HandleStoryAction(server, sender, connection, packetData)
	case nier.PacketTypeID_BUNKER_RING:
		HandleBunkerRing(server, sender, connection, packetData)
	case nier.PacketTypeID_LOG_CHUNK:
		HandleLogChunk(server, sender, connection, packetData)
	case nier.PacketTypeID_SPAWN_ENTITY:
		HandleSpawnEntity(server, sender, connection, packetData)
	case nier.PacketTypeID_DESTROY_ENTITY:
		HandleDestroyEntity(server, sender, connection, packetData)
	case nier.PacketTypeID_ENTITY_DATA:
		HandleEntityData(server, sender, connection, packetData)
	case nier.PacketTypeID_ENTITY_ANIMATION_START:
		HandleEntityAnimationStart(server, sender, connection, packetData)
	default:
		core.Problem(server, "Unknown packet type %d from %s", packetData.Id(), sender.GetAddress())
	}
}
