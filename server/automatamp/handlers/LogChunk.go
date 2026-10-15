package handlers

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"time"

	"github.com/codecat/go-enet"
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

// A copy of a player's log never grows past this, a broken client must not fill the disk.
const MaxClientLogBytes = 64 * 1024 * 1024

// Copies of the players' logs are deleted after this.
const ClientLogMaxAge = 14 * 24 * time.Hour

var unsafeFileChars = regexp.MustCompile(`[^A-Za-z0-9_-]`)

// The folder for copies of the players' logs, empty when the server does not keep them.
func ClientLogsDir(server *structs.Server) string {
	dir, _ := server.Config["clientLogs"].(string)
	return dir
}

func DeleteOldClientLogs(server *structs.Server) {
	dir := ClientLogsDir(server)

	if dir == "" {
		return
	}

	entries, err := os.ReadDir(dir)

	if err != nil {
		return
	}

	for _, entry := range entries {
		info, err := entry.Info()

		if err == nil && !entry.IsDir() && time.Since(info.ModTime()) > ClientLogMaxAge {
			os.Remove(filepath.Join(dir, entry.Name()))
		}
	}
}

func openClientLog(server *structs.Server, client *structs.Client) (*os.File, error) {
	dir := ClientLogsDir(server)

	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}

	name := fmt.Sprintf("%s_session%d_%s_guid%d.txt",
		time.Now().Format("2006-01-02"), server.SessionId, unsafeFileChars.ReplaceAllString(client.Name, "_"), client.Guid)

	return os.OpenFile(filepath.Join(dir, name), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
}

func HandleLogChunk(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	client := connection.Client

	if ClientLogsDir(server) == "" || client.LogCopyFailed {
		return
	}

	if client.LogFile == nil {
		file, err := openClientLog(server, client)

		if err != nil {
			client.LogCopyFailed = true
			core.Problem(server, "Can't keep the log of %s: %v", client.Name, err)
			return
		}

		client.LogFile = file
		core.Event(server, "Keeping the log of %s in %s", client.Name, file.Name())
	}

	chunk := data.DataBytes()

	if client.LogBytes+int64(len(chunk)) > MaxClientLogBytes {
		client.LogFile.WriteString("\n[server] The copy of this log reached its size limit, the rest is only on the player's computer.\n")
		CloseClientLog(client)
		client.LogCopyFailed = true
		return
	}

	written, err := client.LogFile.Write(chunk)
	client.LogBytes += int64(written)

	if err != nil {
		core.Problem(server, "Can't write the log of %s: %v", client.Name, err)
		CloseClientLog(client)
		client.LogCopyFailed = true
	}
}

func CloseClientLog(client *structs.Client) {
	if client.LogFile != nil {
		client.LogFile.Close()
		client.LogFile = nil
	}
}
