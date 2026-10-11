# melonDS game channels for Nakama

`melonds_channel.lua` is the server side of melonDS's online lobby: one
channel per game, where players see who is there, chat, challenge each
other or ask for a quick match. When two players are paired, the channel
relays the few messages their computers need to find each other; the game
itself then runs directly between the two computers, so the server carries
no gameplay traffic. `docs/NETPLAY.md` describes the whole system.

The module runs in [Nakama](https://heroiclabs.com/nakama/) (open source,
Apache 2.0), a game server that needs a PostgreSQL (or CockroachDB) database.

## Setting up a server

1. Install PostgreSQL and create a database for Nakama:

   ```sh
   sudo -u postgres psql -c "ALTER USER postgres PASSWORD 'choose-one';"
   sudo -u postgres psql -c "CREATE DATABASE nakama;"
   ```

2. Download Nakama for your system from
   https://github.com/heroiclabs/nakama/releases (tested with 3.41.0) and
   prepare the database:

   ```sh
   ./nakama migrate up --database.address "postgres:choose-one@127.0.0.1:5432/nakama"
   ```

3. Put `melonds_channel.lua` in a `modules` folder, copy
   `server.example.yml` to `server.yml` and replace each `change-me-...` in
   it with a secret of its own (Nakama refuses identical keys).

4. Start Nakama:

   ```sh
   ./nakama --config server.yml --database.address "postgres:choose-one@127.0.0.1:5432/nakama"
   ```

5. Open TCP port 7350 to the players (only that: games run between the
   players' computers over UDP). Players enter the server's address, port
   7350 and the `socket.server_key` from `server.yml` in melonDS
   (System > Multiplayer > Play online (rollback)... > Settings...).

The server can host IKEMEN GO's Nakama modules at the same time: this
module's names (`melonds_channel`, `melonds_channels`,
`melonds_channel_join`, collection `melonds_channels`) are its own, and its
channels use the same peer-to-peer signal as IKEMEN GO.

## What the module does

- `melonds_channels` (RPC) lists the channels that exist, with how many
  players are in each and how many are playing.
- `melonds_channel_join` (RPC) returns a game's channel, creating it the
  first time someone opens it. A channel is a Nakama match; it ends a minute
  after its last player leaves.
- In a channel, players send their name, region and server ping; set
  themselves as looking to play; chat (at most one message every 0.6
  seconds, 200 characters); challenge a player, who accepts or declines
  within 30 seconds; or queue for a quick match, which pairs the first two
  players in the queue. The challenger, or the first in the queue, hosts.
- A paired player's P2P signals go only to the other player of the pair.
- Players' names, regions and pings are what their games report; the
  server does not check them.
