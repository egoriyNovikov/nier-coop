package core

import (
	"fmt"

	"github.com/codecat/go-libs/log"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

// One line per event, prefixed with the session number the players also see in their logs.
func prefix(server *structs.Server) string {
	if server.SessionId == 0 {
		return "[no session] "
	}

	return fmt.Sprintf("[session %d] ", server.SessionId)
}

func Event(server *structs.Server, format string, args ...interface{}) {
	log.Info(prefix(server)+format, args...)
}

func Problem(server *structs.Server, format string, args ...interface{}) {
	log.Error(prefix(server)+format, args...)
}
