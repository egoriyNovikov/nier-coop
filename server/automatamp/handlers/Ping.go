package handlers

import (
	"time"

	"github.com/codecat/go-enet"
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

func HandlePing(server *structs.Server, sender enet.Peer, connection *structs.Connection) {
	core.Send(server, sender, core.MakeEmptyPacketBytes(nier.PacketTypeID_PONG))
}

// Answer to the server's own PING, see SendPings.
func HandlePong(server *structs.Server, connection *structs.Connection) {
	client := connection.Client

	if client.PingPending {
		client.Rtt = time.Since(client.PingSentAt)
		client.PingPending = false
	}
}

// Measures the round trip time of every player for the statistics line.
func SendPings(server *structs.Server) {
	ping := core.MakeEmptyPacketBytes(nier.PacketTypeID_PING)

	for conn, client := range server.Clients {
		if client.PingPending {
			continue
		}

		client.PingSentAt = time.Now()
		client.PingPending = true
		core.Send(server, conn.Peer, ping)
	}
}
