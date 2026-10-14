package automatamp

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strconv"
	"time"

	core "github.com/praydog/AutomataMP/server/automatamp/core"
	handlers "github.com/praydog/AutomataMP/server/automatamp/handlers"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"

	"github.com/codecat/go-enet"
	"github.com/codecat/go-libs/log"
)

var currentServer *structs.Server

func handleEnetEvent(ev enet.Event) {
	switch ev.GetType() {
	case enet.EventConnect:
		handleEnetConnectEvent(ev)
	case enet.EventDisconnect:
		handleEnetDisconnectEvent(ev)
	case enet.EventReceive:
		handleEnetReceiveEvent(ev)
	}
}

func handleEnetConnectEvent(ev enet.Event) {
	core.Event(currentServer, "Connection from %s", ev.GetPeer().GetAddress())
	connection := &structs.Connection{}
	connection.Peer = ev.GetPeer()
	connection.Client = nil
	currentServer.Connections[ev.GetPeer()] = connection
}

func handleEnetDisconnectEvent(ev enet.Event) {
	connection := currentServer.Connections[ev.GetPeer()]

	if connection == nil {
		return
	}

	if connection.Client == nil {
		core.Event(currentServer, "Disconnected before joining: %s", ev.GetPeer().GetAddress())
	}

	handleDisconnect(connection, ev.GetPeer())
}

func handleDisconnect(connection *structs.Connection, peer enet.Peer) {
	client := connection.Client

	if client != nil {
		core.Event(currentServer, "Left: %s (guid %d), played %s", client.Name, client.Guid, time.Since(client.JoinedAt).Round(time.Second))

		handlers.HandleDestroyPlayer(currentServer, connection)
		delete(currentServer.Clients, connection)

		// Nobody takes over the story: the other player waits for the host to come back.
		if client.IsMasterClient && len(currentServer.Clients) > 0 {
			core.Event(currentServer, "%s led the story, nobody leads until a host joins", client.Name)
			handlers.BroadcastMasterClient(currentServer, 0)
		}

		if len(currentServer.Clients) == 0 {
			core.Event(currentServer, "Session ended after %s", time.Since(currentServer.SessionStarted).Round(time.Second))
			currentServer.Entities = make(structs.EntityList)
			currentServer.HighestEntityGuid = 0
			currentServer.SessionId = 0
		}
	}

	delete(currentServer.Connections, peer)
}

func handleEnetReceiveEvent(ev enet.Event) {
	connection := currentServer.Connections[ev.GetPeer()]

	packet := ev.GetPacket()
	defer packet.Destroy()
	packetBytes := packet.GetData()

	currentServer.Traffic.PacketsIn++
	currentServer.Traffic.BytesIn += uint64(len(packetBytes))

	// A broken or malicious packet must not take the server down for the other player.
	defer func() {
		if r := recover(); r != nil {
			core.Problem(currentServer, "Bad packet (%d bytes) from %s: %v", len(packetBytes), ev.GetPeer().GetAddress(), r)
		}
	}()

	if checkClientIsAuthorized(ev, packetBytes) {
		handlers.PacketHandler(currentServer, connection, ev.GetPeer(), packetBytes)
	}
}

func checkClientIsAuthorized(ev enet.Event, data []byte) bool {
	connection := currentServer.Connections[ev.GetPeer()]

	if connection == nil {
		core.Problem(currentServer, "Data from unknown peer %s, ignoring", ev.GetPeer().GetAddress())
		return false
	}

	packet := nier.GetRootAsPacket(data, 0)

	if !core.CheckValidPacket(packet) {
		core.Problem(currentServer, "Invalid packet (%d bytes) from %s", len(data), ev.GetPeer().GetAddress())
		return false
	}

	if connection.Client == nil && packet.Id() != nier.PacketTypeID_HELLO {
		core.Problem(currentServer, "Packet %d from %s before hello, ignoring", packet.Id(), ev.GetPeer().GetAddress())
		return false
	}

	return true
}

// One line a minute while someone plays: players, traffic and round trip times.
func logStatistics() {
	elapsed := time.Since(currentServer.LastStats).Seconds()
	traffic := currentServer.Traffic

	currentServer.LastStats = time.Now()
	currentServer.Traffic = structs.Traffic{}

	if len(currentServer.Clients) == 0 || elapsed <= 0 {
		return
	}

	players := ""

	for _, client := range currentServer.Clients {
		rtt := "?"
		if client.Rtt > 0 {
			rtt = client.Rtt.Round(time.Millisecond).String()
		}

		lead := ""
		if client.IsMasterClient {
			lead = " leads"
		}

		players += fmt.Sprintf(" | %s rtt %s%s", client.Name, rtt, lead)
	}

	core.Event(currentServer, "Stats: %d players, in %.0f pkt/s %.1f KB/s, out %.0f pkt/s %.1f KB/s%s",
		len(currentServer.Clients),
		float64(traffic.PacketsIn)/elapsed, float64(traffic.BytesIn)/elapsed/1024,
		float64(traffic.PacketsOut)/elapsed, float64(traffic.BytesOut)/elapsed/1024,
		players)
}

func periodicTasks() {
	if time.Since(currentServer.LastPing) >= 5*time.Second {
		currentServer.LastPing = time.Now()
		handlers.SendPings(currentServer)
	}

	if time.Since(currentServer.LastStats) >= time.Minute {
		logStatistics()
	}
}

func service() {
	// Wait until the next event (up to 1ms, so the loop doesn't spin a whole CPU core)
	ev := currentServer.Host.Service(1)
	evHandleCount := 0

	for ev.GetType() != enet.EventNone {
		evHandleCount++
		handleEnetEvent(ev)

		// Break out if we have handled too many events
		// so we can perform other tasks. We will handle the rest
		// next time we call Service.
		if evHandleCount >= 100 {
			break
		}

		ev = currentServer.Host.Service(0)
	}
}

func cleanup() {
	// Destroy the host when we're done with it
	if currentServer.Host != nil {
		currentServer.Host.Destroy()
	}

	// Uninitialize enet
	enet.Deinitialize()
}

func sendHeartbeatToMasterServer() {
	if !currentServer.Config["masterServerNotify"].(bool) {
		log.Info("Not sending heartbeats to master server. If you want this then set masterServerNotify to true in server.json")
		return
	}

	log.Info("Sending heartbeat to master server")
	currentServer.LastHeartbeat = time.Now()

	jsonValues := make(map[string]interface{})
	jsonValues["Port"] = currentServer.Config["port"].(string)
	jsonValues["Name"] = currentServer.Config["name"].(string)
	jsonValues["NumPlayers"] = len(currentServer.Clients)

	jsonBytes, err := json.Marshal(jsonValues)

	if err != nil {
		log.Error("Error marshalling heartbeat json: %s", err)
		return
	}

	log.Info(string(jsonBytes))

	url := currentServer.Config["masterServer"].(string) + "/heartbeat"
	log.Info("Sending heartbeat to %s", url)

	r, err := http.NewRequest("POST", url, bytes.NewBuffer(jsonBytes))

	if err != nil {
		log.Error("Error creating heartbeat request: %s", err)
		return
	}

	var zeroDialer net.Dialer

	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.DialContext = func(ctx context.Context, network, addr string) (net.Conn, error) {
		return zeroDialer.DialContext(ctx, "tcp4", addr)
	}

	client := &http.Client{}
	client.Transport = transport
	resp, err := client.Do(r)

	if err != nil {
		log.Error("Error creating heartbeat request: %s", err)
		return
	}

	if resp.StatusCode != http.StatusOK {
		log.Error("Error sending heartbeat: %s", resp.Status)
		return
	}
}

func heartbeatGoroutine() {
	if !currentServer.Config["masterServerNotify"].(bool) {
		log.Info("Not sending heartbeats to master server. If you want this then set masterServerNotify to true in server.json")
		return
	}

	for {
		// every 30 seconds, send a heartbeat to the master server
		// containing the server name and how many players are connected
		if time.Since(currentServer.LastHeartbeat) >= 30*time.Second {
			sendHeartbeatToMasterServer()
		}

		time.Sleep(time.Second)
	}
}

// server start
func Run() {
	// Initialize enet
	enet.Initialize()

	defer cleanup()

	// Convert port to int
	port, err := strconv.Atoi(currentServer.Config["port"].(string))

	if err != nil {
		log.Info("Error reading port: %s, using default (6969).", err)
		port = 6969
	}

	// Create a host listening on 0.0.0.0:6969. A few spare peer slots let extra players
	// connect and receive the reason why they can't join (the game allows 2 players).
	host, err := enet.NewHost(enet.NewListenAddress(uint16(port)), handlers.MaxPlayers+4, 1, 0, 0)
	if err != nil {
		log.Error("Couldn't create host: %s", err.Error())
		panic(err)
	}

	currentServer.Host = host

	log.Info("Created host")

	go heartbeatGoroutine()

	currentServer.LastStats = time.Now()

	// The event loop
	for {
		service()
		periodicTasks()
	}
}

func CreateServer() {
	serverJson, err := os.ReadFile("server.json")
	if err != nil {
		log.Error("Server requires a server.json file to be present")
		panic(err)
	}

	if currentServer == nil {
		currentServer = &structs.Server{}
	}

	currentServer.Connections = make(map[enet.Peer]*structs.Connection)
	currentServer.Clients = make(map[*structs.Connection]*structs.Client)
	currentServer.Entities = make(structs.EntityList)
	currentServer.Config = make(map[string]interface{})
	currentServer.ConnectionCount = 0
	currentServer.HighestEntityGuid = 0
	currentServer.Config["password"] = ""
	currentServer.Config["masterServer"] = "http://localhost"
	currentServer.Config["masterServerNotify"] = true
	currentServer.Config["name"] = "AutomataMP Server"
	currentServer.Config["port"] = "6969"

	json.Unmarshal(serverJson, &currentServer.Config)
	if currentServer.Config["password"].(string) == "" {
		log.Info("No password set, anyone can join")
	} else {
		log.Info("Password set")
	}

	Run()
}
