package structs

import (
	"time"

	"github.com/codecat/go-enet"
)

// Traffic counted between two statistics lines.
type Traffic struct {
	PacketsIn  uint64
	BytesIn    uint64
	PacketsOut uint64
	BytesOut   uint64
}

type Server struct {
	Host              enet.Host
	Connections       map[enet.Peer]*Connection
	Clients           map[*Connection]*Client
	Entities          EntityList
	ConnectionCount   uint64
	HighestEntityGuid uint32
	Config            map[string]interface{}
	LastHeartbeat     time.Time

	// A session lasts from the first player joining an empty server until the last one leaves.
	SessionId      uint32
	SessionStarted time.Time

	Traffic   Traffic
	LastStats time.Time
	LastPing  time.Time
}
