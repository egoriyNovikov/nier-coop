package handlers

import (
	"fmt"
	"time"

	"github.com/codecat/go-enet"
	flatbuffers "github.com/google/flatbuffers/go"
	core "github.com/praydog/AutomataMP/server/automatamp/core"
	nier "github.com/praydog/AutomataMP/server/automatamp/nier"
	structs "github.com/praydog/AutomataMP/server/automatamp/structs"
)

// The mod is made for exactly two players (docs/COOP_CONCEPT.md, principle 3).
const MaxPlayers = 2

// Tells the player why it can't join, then disconnects once the packet is delivered.
func reject(server *structs.Server, sender enet.Peer, name string, reason nier.RejectReason, detail string) {
	core.Event(server, "Rejected %s (%s): %s %s", name, sender.GetAddress(), nier.EnumNamesRejectReason[reason], detail)

	rejectBytes := core.BuilderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
		detailOffset := builder.CreateString(detail)
		nier.RejectStart(builder)
		nier.RejectAddReason(builder, reason)
		nier.RejectAddDetail(builder, detailOffset)
		return nier.RejectEnd(builder)
	})

	core.Send(server, sender, core.MakePacketBytes(nier.PacketTypeID_REJECT, rejectBytes))
	sender.DisconnectLater(uint32(reason))
}

func makeCreatePlayer(client *structs.Client) []uint8 {
	return core.BuilderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
		playerName := builder.CreateString(client.Name)
		nier.CreatePlayerStart(builder)
		nier.CreatePlayerAddGuid(builder, client.Guid)
		nier.CreatePlayerAddName(builder, playerName)
		nier.CreatePlayerAddModel(builder, client.Model)
		nier.CreatePlayerAddIsMasterClient(builder, client.IsMasterClient)
		return nier.CreatePlayerEnd(builder)
	})
}

func HandleHello(server *structs.Server, sender enet.Peer, connection *structs.Connection, data *nier.Packet) {
	if connection.Client != nil {
		core.Problem(server, "Second hello from %s, ignoring", connection.Client.Name)
		return
	}

	helloData := nier.GetRootAsHello(data.DataBytes(), 0)
	name := string(helloData.Name())
	modVersion := string(helloData.ModVersion())

	if helloData.Major() != uint32(nier.VersionMajorValue) {
		reject(server, sender, name, nier.RejectReasonVersionMismatch,
			fmt.Sprintf("server protocol %d, player protocol %d (mod %s)", nier.VersionMajorValue, helloData.Major(), modVersion))
		return
	}

	if password := server.Config["password"].(string); password != "" && string(helloData.Password()) != password {
		reject(server, sender, name, nier.RejectReasonWrongPassword, "")
		return
	}

	if _, ok := nier.EnumNamesModelType[nier.ModelType(helloData.Model())]; !ok {
		reject(server, sender, name, nier.RejectReasonInvalidModel, fmt.Sprintf("model %x", helloData.Model()))
		return
	}

	if len(server.Clients) >= MaxPlayers {
		reject(server, sender, name, nier.RejectReasonServerFull, fmt.Sprintf("%d players already", len(server.Clients)))
		return
	}

	if helloData.WantsHost() {
		if host := FindWantsHost(server); host != nil {
			reject(server, sender, name, nier.RejectReasonHostTaken, "the host is "+host.Name)
			return
		}
	}

	if len(server.Clients) == 0 {
		server.SessionId = uint32(time.Now().Unix())
		server.SessionStarted = time.Now()
		core.Event(server, "Session started")
	}

	server.ConnectionCount++

	client := &structs.Client{
		Guid:       server.ConnectionCount,
		Name:       core.GetFilteredPlayerName(server, helloData.Name()),
		Model:      helloData.Model(),
		ModVersion: modVersion,
		WantsHost:  helloData.WantsHost(),
		JoinedAt:   time.Now(),
	}

	// The host leads the story no matter who joined first. Without a host (nobody picked the host
	// role) the first player leads, as before. When the leader leaves, nobody else takes over:
	// the remaining player waits, and whoever joins next while nobody leads becomes the leader.
	previous := GetMasterClient(server)

	if client.WantsHost && previous != nil {
		previous.IsMasterClient = false
	}

	client.IsMasterClient = client.WantsHost || previous == nil

	connection.Client = client
	server.Clients[connection] = client

	role := "client"
	if client.IsMasterClient {
		role = "leads the story"
	}

	core.Event(server, "Joined: %s (guid %d, %s, mod %s, model %s, host role %t) -> %s",
		client.Name, client.Guid, sender.GetAddress(), modVersion,
		nier.EnumNamesModelType[nier.ModelType(client.Model)], client.WantsHost, role)

	welcomeBytes := core.BuilderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
		nier.WelcomeStart(builder)
		nier.WelcomeAddGuid(builder, client.Guid)
		nier.WelcomeAddIsMasterClient(builder, client.IsMasterClient)
		nier.WelcomeAddHighestEntityGuid(builder, server.HighestEntityGuid)
		nier.WelcomeAddSessionId(builder, server.SessionId)
		nier.WelcomeAddAcceptsLogs(builder, ClientLogsDir(server) != "")
		return nier.WelcomeEnd(builder)
	})

	core.Send(server, sender, core.MakePacketBytes(nier.PacketTypeID_WELCOME, welcomeBytes))

	if client.WantsHost && previous != nil {
		core.Event(server, "%s takes the story lead from %s", client.Name, previous.Name)
		BroadcastMasterClient(server, client.Guid)
	}

	core.BroadcastPacketToAll(server, nier.PacketTypeID_CREATE_PLAYER, makeCreatePlayer(client))

	// The players that were already here.
	for _, prevClient := range server.Clients {
		if prevClient == nil || prevClient == client {
			continue
		}

		core.Send(server, sender, core.MakePacketBytes(nier.PacketTypeID_CREATE_PLAYER, makeCreatePlayer(prevClient)))
	}

	// Enemies of the master client (only used with "Shared enemies from the host").
	for _, entity := range server.Entities {
		if entity == nil {
			continue
		}

		spawnData := core.BuilderSurround(func(builder *flatbuffers.Builder) flatbuffers.UOffsetT {
			name := builder.CreateString(string(entity.SpawnInfo.Name()))
			posdata := entity.SpawnInfo.Positional(nil)
			nier.EntitySpawnParamsStart(builder)
			nier.EntitySpawnParamsAddName(builder, name)
			nier.EntitySpawnParamsAddModel(builder, entity.SpawnInfo.Model())
			nier.EntitySpawnParamsAddModel2(builder, entity.SpawnInfo.Model2())
			packetPosData := nier.CreateEntitySpawnPositionalData(
				builder,
				posdata.Forward(nil).X(), posdata.Forward(nil).Y(), posdata.Forward(nil).Z(), posdata.Forward(nil).W(),
				posdata.Up(nil).X(), posdata.Up(nil).Y(), posdata.Up(nil).Z(), posdata.Up(nil).W(),
				posdata.Right(nil).X(), posdata.Right(nil).Y(), posdata.Right(nil).Z(), posdata.Right(nil).W(),
				posdata.W(nil).X(), posdata.W(nil).Y(), posdata.W(nil).Z(), posdata.W(nil).W(),
				posdata.Position(nil).X(), posdata.Position(nil).Y(), posdata.Position(nil).Z(), posdata.Position(nil).W(),
				posdata.Unknown(nil).X(), posdata.Unknown(nil).Y(), posdata.Unknown(nil).Z(), posdata.Unknown(nil).W(),
				posdata.Unknown2(nil).X(), posdata.Unknown2(nil).Y(), posdata.Unknown2(nil).Z(), posdata.Unknown2(nil).W(),
				posdata.Unk(), posdata.Unk2(), posdata.Unk3(), posdata.Unk4(),
				posdata.Unk5(), posdata.Unk6(), posdata.Unk7(), posdata.Unk8(),
			)
			nier.EntitySpawnParamsAddPositional(builder, packetPosData)
			return nier.EntitySpawnParamsEnd(builder)
		})

		core.Send(server, sender, core.MakeEntityPacketBytes(entity.Guid, nier.PacketTypeID_SPAWN_ENTITY, spawnData))
	}
}
