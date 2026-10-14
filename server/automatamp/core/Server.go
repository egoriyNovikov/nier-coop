package core

import (
	"strconv"

	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
)

func GetFilteredPlayerName(server *structs.Server, input []uint8) string {
	out := ""

	if input == nil || string(input) == "" {
		out = "Client" + strconv.FormatInt(int64(len(server.Connections)), 10)
	} else {
		out = string(input)
	}

	for _, client := range server.Clients {
		if client.Name == out {
			out = out + strconv.FormatInt(int64(len(server.Connections)), 10)
			break
		}
	}

	return out
}

// Every packet the server sends goes through here, so the statistics see it.
func Send(server *structs.Server, peer enet.Peer, data []uint8) {
	server.Traffic.PacketsOut++
	server.Traffic.BytesOut += uint64(len(data))
	peer.SendBytes(data, 0, enet.PacketFlagReliable)
}

func BroadcastPacketToAll(server *structs.Server, id nier.PacketType, data []uint8) {
	broadcastData := MakePacketBytes(id, data)
	for conn := range server.Clients {
		Send(server, conn.Peer, broadcastData)
	}
}

func BroadcastPacketToAllExceptSender(server *structs.Server, sender enet.Peer, id nier.PacketType, data []uint8) {
	broadcastData := MakePacketBytes(id, data)
	for conn := range server.Clients {
		if conn.Peer == sender {
			continue
		}

		Send(server, conn.Peer, broadcastData)
	}
}

func BroadcastPlayerPacketToAll(server *structs.Server, connection *structs.Connection, id nier.PacketType, data []uint8) {
	broadcastData := MakePlayerPacketBytes(connection.Client.Guid, id, data)

	for conn := range server.Clients {
		Send(server, conn.Peer, broadcastData)
	}
}

func BroadcastPlayerPacketToAllExceptSender(server *structs.Server, sender enet.Peer, connection *structs.Connection, id nier.PacketType, data []uint8) {
	broadcastData := MakePlayerPacketBytes(connection.Client.Guid, id, data)

	for conn := range server.Clients {
		if conn.Peer == sender {
			continue
		}

		Send(server, conn.Peer, broadcastData)
	}
}
