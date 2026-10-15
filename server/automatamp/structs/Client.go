package structs

import (
	"os"
	"time"

	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
)

type Client struct {
	Guid           uint64
	Model          uint32
	Name           string
	ModVersion     string
	WantsHost      bool // Joined as the host: leads the story.
	IsMasterClient bool
	JoinedAt       time.Time
	LastPlayerData *nier.PlayerData

	// Round trip time measured with the server's PING/PONG.
	PingSentAt  time.Time
	PingPending bool
	Rtt         time.Duration

	// The server's copy of the player's log (ID_LOG_CHUNK), see handlers/LogChunk.go.
	LogFile       *os.File
	LogBytes      int64
	LogCopyFailed bool
}
