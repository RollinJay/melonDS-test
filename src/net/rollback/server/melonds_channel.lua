-- SPDX-License-Identifier: MIT
-- Copyright (c) 2026 The Nakama Matchmaker contributors
-- This file is a Nakama server module and contains no melonDS code, so it is under the
-- MIT license, not melonDS's GPL.

local nk = require("nakama")

-- melonDS game channels for Nakama.
--
-- A channel is an authoritative match per game, where the players of that
-- game gather: it lists who is there and whether they are looking for a
-- game, carries their chat, and pairs two players, either because one
-- challenged the other and the other accepted, or because both asked for a
-- quick match. A pair then opens a direct connection between their two
-- computers (the channel relays the P2P signals of that pair only) and
-- plays; the channel runs no emulation and sees none of the game's traffic.
--
-- RPCs
--   melonds_channels      {} -> {channels = [{match_id, game, title, members, playing}]}
--   melonds_channel_join  {game, title} -> {match_id}: the channel of a game,
--                         created when it does not exist yet
--
-- Client -> channel (match data, JSON)
--   HELLO      {name, region, ping}        who this member is
--   STATUS     {status, ping, region}      status: idle | looking | away
--   CHAT       {text}
--   CHALLENGE  {to}                        ask a member to play
--   ANSWER     {from, accept}              answer a challenge
--   CANCEL     {to}                        withdraw a challenge
--   QUEUE      {on}                        quick match: pair me with the next one
--   DONE       {seq, reason}               the pair's session ended
--   SIGNAL     P2P signal (IKEMEN-P2P/1)   relayed to its "to" member, within a pair
--
-- Channel -> client
--   STATE      {game, title, members = [{user_id, name, status, region, ping}]}
--   CHAT_MSG   {from, name, text} (or {notice = text})
--   CHALLENGED {from, name}                someone asks you to play
--   CHALLENGE_UPDATE {with, name, state}   sent | declined | cancelled | expired | busy
--   PAIRING    {seq, host, guest, host_name, guest_name, match_id, how}
--   PAIRING_END {seq, reason}               the other player of your pair is done
--   ERROR      {text}

local OP_HELLO = 0x4D440001
local OP_STATUS = 0x4D440002
local OP_CHAT = 0x4D440003
local OP_CHALLENGE = 0x4D440004
local OP_ANSWER = 0x4D440005
local OP_CANCEL = 0x4D440006
local OP_QUEUE = 0x4D440007
local OP_DONE = 0x4D440008
local OP_STATE = 0x4D440064
local OP_CHAT_MSG = 0x4D440065
local OP_CHALLENGED = 0x4D440066
local OP_CHALLENGE_UPDATE = 0x4D440067
local OP_PAIRING = 0x4D440068
local OP_PAIRING_END = 0x4D440069
local OP_ERROR = 0x4D44006A
local OP_P2P_SIGNAL = 0x494B5347   -- IKEMEN GO's signal op code

local TICK_RATE = 5
local MAX_MEMBERS = 500
local NAME_LIMIT = 24
local CHAT_LIMIT = 200
local CHAT_INTERVAL_TICKS = 3                 -- one message per 0.6 s
local CHAT_HISTORY = 30
local CHALLENGE_TIMEOUT_TICKS = 30 * TICK_RATE
local STATE_INTERVAL_TICKS = TICK_RATE        -- the member list goes out at most once a second
local EMPTY_TIMEOUT_TICKS = 60 * TICK_RATE
local PAIRING_TIMEOUT_TICKS = 6 * 60 * 60 * TICK_RATE   -- a pair that never reports back
local SIGNAL_MAX = 4096

local CHANNEL_COLLECTION = "melonds_channels"

local STATUSES = { idle = true, looking = true, away = true }

-- ---------------------------------------------------------------------------
-- Helpers

local function clean_text(text, limit)
    if type(text) ~= "string" then
        return ""
    end
    -- drop control characters (including newlines)
    text = text:gsub("[%z\1-\31\127]", "")
    if #text > limit then
        -- cut at a character boundary (UTF-8 continuation bytes are 0x80-0xBF)
        local cut = limit
        while cut > 0 and text:byte(cut + 1) ~= nil and text:byte(cut + 1) >= 0x80 and text:byte(cut + 1) < 0xC0 do
            cut = cut - 1
        end
        text = text:sub(1, cut)
    end
    return text
end

local function valid_game_id(game)
    return type(game) == "string" and #game >= 3 and #game <= 64 and game:match("^[%w%-_:%.]+$") ~= nil
end

local function decode(data)
    local ok, value = pcall(nk.json_decode, data or "")
    if ok and type(value) == "table" then
        return value
    end
    return nil
end

local function send(dispatcher, op, payload, presences)
    dispatcher.broadcast_message(op, nk.json_encode(payload), presences, nil)
end

local function member_list(state)
    local list = {}
    for _, user_id in ipairs(state.order) do
        local m = state.members[user_id]
        if m ~= nil then
            list[#list + 1] = {
                user_id = user_id,
                name = m.name,
                status = m.status,
                region = m.region,
                ping = m.ping,
            }
        end
    end
    return list
end

local function counts(state)
    local members, playing = 0, 0
    for _, m in pairs(state.members) do
        members = members + 1
        if m.status == "playing" then
            playing = playing + 1
        end
    end
    return members, playing
end

local function update_label(dispatcher, state)
    local members, playing = counts(state)
    local label = nk.json_encode({
        kind = "melonds-channel",
        game = state.game,
        title = state.title,
        members = members,
        playing = playing,
    })
    if label ~= state.label then
        state.label = label
        dispatcher.match_label_update(label)
    end
end

local function challenge_key(from, to)
    return from .. ">" .. to
end

-- Drops every challenge from or to user_id, telling the other side why.
local function drop_challenges(dispatcher, state, user_id, why)
    for key, c in pairs(state.challenges) do
        if c.from == user_id or c.to == user_id then
            state.challenges[key] = nil
            local other = (c.from == user_id) and c.to or c.from
            local om = state.members[other]
            local um = state.members[user_id]
            if om ~= nil then
                send(dispatcher, OP_CHALLENGE_UPDATE, {
                    with = user_id,
                    name = um and um.name or "",
                    state = why,
                }, { om.presence })
            end
        end
    end
end

local function remove_from_queue(state, user_id)
    for i = #state.queue, 1, -1 do
        if state.queue[i] == user_id then
            table.remove(state.queue, i)
        end
    end
end

local function pair(dispatcher, state, tick, host_id, guest_id, how)
    local host = state.members[host_id]
    local guest = state.members[guest_id]
    if host == nil or guest == nil then
        return
    end
    state.seq = state.seq + 1
    local seq = state.seq
    state.pairings[seq] = { seq = seq, host = host_id, guest = guest_id, tick = tick }
    host.status = "playing"
    guest.status = "playing"
    host.pairing = seq
    guest.pairing = seq
    drop_challenges(dispatcher, state, host_id, "busy")
    drop_challenges(dispatcher, state, guest_id, "busy")
    remove_from_queue(state, host_id)
    remove_from_queue(state, guest_id)

    local payload = {
        seq = seq,
        host = host_id,
        guest = guest_id,
        host_name = host.name,
        guest_name = guest.name,
        match_id = state.match_id,
        how = how,
    }
    send(dispatcher, OP_PAIRING, payload, { host.presence, guest.presence })
    state.dirty = true
end

local function end_pairing(state, seq)
    local p = state.pairings[seq]
    if p == nil then
        return
    end
    state.pairings[seq] = nil
    for _, user_id in ipairs({ p.host, p.guest }) do
        local m = state.members[user_id]
        if m ~= nil and m.pairing == seq then
            m.pairing = nil
            m.status = "idle"
        end
    end
    state.dirty = true
end

local function in_pairing(state, a, b)
    local ma = state.members[a]
    if ma == nil or ma.pairing == nil then
        return false
    end
    local p = state.pairings[ma.pairing]
    return p ~= nil and ((p.host == a and p.guest == b) or (p.host == b and p.guest == a))
end

-- ---------------------------------------------------------------------------
-- Match handler

local function match_init(context, params)
    params = params or {}
    local state = {
        game = params.game or "",
        title = params.title or "",
        match_id = context.match_id,
        members = {},
        order = {},
        challenges = {},
        queue = {},
        pairings = {},
        seq = 0,
        history = {},
        dirty = true,
        last_state_tick = -STATE_INTERVAL_TICKS,
        empty_ticks = 0,
        label = "",
    }
    local label = nk.json_encode({ kind = "melonds-channel", game = state.game, title = state.title, members = 0, playing = 0 })
    state.label = label
    return state, TICK_RATE, label
end

local function match_join_attempt(context, dispatcher, tick, state, presence, metadata)
    local members = counts(state)
    if members >= MAX_MEMBERS then
        return state, false, "The channel is full"
    end
    return state, true
end

local function match_join(context, dispatcher, tick, state, presences)
    for _, presence in ipairs(presences) do
        local user_id = presence.user_id
        local existing = state.members[user_id]
        if existing ~= nil then
            -- the same account again (another window): the newest presence wins
            existing.presence = presence
        else
            state.members[user_id] = {
                presence = presence,
                name = clean_text(presence.username, NAME_LIMIT),
                status = "idle",
                region = "",
                ping = -1,
                last_chat = -CHAT_INTERVAL_TICKS,
            }
            state.order[#state.order + 1] = user_id
        end
        -- the conversation so far
        for _, line in ipairs(state.history) do
            send(dispatcher, OP_CHAT_MSG, line, { presence })
        end
    end
    state.dirty = true
    return state
end

local function match_leave(context, dispatcher, tick, state, presences)
    for _, presence in ipairs(presences) do
        local user_id = presence.user_id
        local m = state.members[user_id]
        if m ~= nil and m.presence.session_id == presence.session_id then
            drop_challenges(dispatcher, state, user_id, "cancelled")
            remove_from_queue(state, user_id)
            state.members[user_id] = nil
            for i = #state.order, 1, -1 do
                if state.order[i] == user_id then
                    table.remove(state.order, i)
                end
            end
        end
    end
    state.dirty = true
    return state
end

local function handle(dispatcher, state, tick, message)
    local sender = message.sender
    if sender == nil then
        return
    end
    local user_id = sender.user_id
    local me = state.members[user_id]
    if me == nil then
        return
    end
    local op = message.op_code

    if op == OP_P2P_SIGNAL then
        -- only to the other player of one's own pair
        if #(message.data or "") > SIGNAL_MAX then
            return
        end
        local signal = decode(message.data)
        if signal == nil or type(signal.to) ~= "string" then
            return
        end
        local target = state.members[signal.to]
        if target ~= nil and in_pairing(state, user_id, signal.to) then
            dispatcher.broadcast_message(op, message.data, { target.presence }, sender)
        end
        return
    end

    local cmd = decode(message.data)
    if cmd == nil then
        return
    end

    if op == OP_HELLO or op == OP_STATUS then
        if op == OP_HELLO and cmd.name ~= nil then
            local name = clean_text(cmd.name, NAME_LIMIT)
            if name ~= "" then
                me.name = name
            end
        end
        if cmd.region ~= nil then
            me.region = clean_text(cmd.region, 8)
        end
        local ping = tonumber(cmd.ping)
        if ping ~= nil then
            me.ping = math.max(-1, math.min(9999, math.floor(ping)))
        end
        if cmd.status ~= nil and STATUSES[cmd.status] and me.status ~= "playing" then
            me.status = cmd.status
            if cmd.status ~= "looking" then
                remove_from_queue(state, user_id)
            end
        end
        state.dirty = true

    elseif op == OP_CHAT then
        if tick - me.last_chat < CHAT_INTERVAL_TICKS then
            send(dispatcher, OP_ERROR, { text = "Slow down" }, { me.presence })
            return
        end
        local text = clean_text(cmd.text, CHAT_LIMIT)
        if text == "" then
            return
        end
        me.last_chat = tick
        local line = { from = user_id, name = me.name, text = text }
        state.history[#state.history + 1] = line
        if #state.history > CHAT_HISTORY then
            table.remove(state.history, 1)
        end
        send(dispatcher, OP_CHAT_MSG, line, nil)

    elseif op == OP_CHALLENGE then
        local to = cmd.to
        local target = (type(to) == "string") and state.members[to] or nil
        if target == nil or to == user_id then
            send(dispatcher, OP_ERROR, { text = "That player is not in the channel" }, { me.presence })
            return
        end
        if me.status == "playing" or target.status == "playing" then
            send(dispatcher, OP_CHALLENGE_UPDATE, { with = to, name = target.name, state = "busy" }, { me.presence })
            return
        end
        -- both challenged each other: that is a yes
        if state.challenges[challenge_key(to, user_id)] ~= nil then
            state.challenges[challenge_key(to, user_id)] = nil
            pair(dispatcher, state, tick, to, user_id, "challenge")
            return
        end
        state.challenges[challenge_key(user_id, to)] = { from = user_id, to = to, tick = tick }
        send(dispatcher, OP_CHALLENGED, { from = user_id, name = me.name }, { target.presence })
        send(dispatcher, OP_CHALLENGE_UPDATE, { with = to, name = target.name, state = "sent" }, { me.presence })

    elseif op == OP_ANSWER then
        local from = cmd.from
        local key = (type(from) == "string") and challenge_key(from, user_id) or nil
        local c = key and state.challenges[key] or nil
        if c == nil then
            send(dispatcher, OP_ERROR, { text = "That challenge is no longer open" }, { me.presence })
            return
        end
        state.challenges[key] = nil
        local challenger = state.members[from]
        if challenger == nil then
            return
        end
        if cmd.accept == true then
            if challenger.status == "playing" or me.status == "playing" then
                send(dispatcher, OP_CHALLENGE_UPDATE, { with = from, name = challenger.name, state = "busy" }, { me.presence })
                return
            end
            -- the challenger hosts
            pair(dispatcher, state, tick, from, user_id, "challenge")
        else
            send(dispatcher, OP_CHALLENGE_UPDATE, { with = user_id, name = me.name, state = "declined" }, { challenger.presence })
        end

    elseif op == OP_CANCEL then
        local to = cmd.to
        local key = (type(to) == "string") and challenge_key(user_id, to) or nil
        if key and state.challenges[key] ~= nil then
            state.challenges[key] = nil
            local target = state.members[to]
            if target ~= nil then
                send(dispatcher, OP_CHALLENGE_UPDATE, { with = user_id, name = me.name, state = "cancelled" }, { target.presence })
            end
        end

    elseif op == OP_QUEUE then
        remove_from_queue(state, user_id)
        if cmd.on == true and me.status ~= "playing" then
            state.queue[#state.queue + 1] = user_id
            me.status = "looking"
        elseif me.status == "looking" then
            me.status = "idle"
        end
        state.dirty = true

    elseif op == OP_DONE then
        local seq = tonumber(cmd.seq)
        local p = seq and state.pairings[seq] or nil
        if p ~= nil and (p.host == user_id or p.guest == user_id) then
            -- the other player may still be connecting: tell them
            local other = state.members[(p.host == user_id) and p.guest or p.host]
            if other ~= nil then
                send(dispatcher, OP_PAIRING_END, { seq = seq, reason = clean_text(cmd.reason, 80) }, { other.presence })
            end
            end_pairing(state, seq)
        end
    end
end

local function match_loop(context, dispatcher, tick, state, messages)
    for _, message in ipairs(messages) do
        handle(dispatcher, state, tick, message)
    end

    -- quick match: first come, first paired; the first in line hosts
    while #state.queue >= 2 do
        local a = table.remove(state.queue, 1)
        local b = table.remove(state.queue, 1)
        if state.members[a] ~= nil and state.members[b] ~= nil then
            pair(dispatcher, state, tick, a, b, "quick")
        elseif state.members[b] ~= nil then
            table.insert(state.queue, 1, b)
        elseif state.members[a] ~= nil then
            table.insert(state.queue, 1, a)
        end
    end

    -- challenges and pairs that ran out
    for key, c in pairs(state.challenges) do
        if tick - c.tick > CHALLENGE_TIMEOUT_TICKS then
            state.challenges[key] = nil
            local from, to = state.members[c.from], state.members[c.to]
            if from ~= nil then
                send(dispatcher, OP_CHALLENGE_UPDATE, { with = c.to, name = to and to.name or "", state = "expired" }, { from.presence })
            end
            if to ~= nil then
                send(dispatcher, OP_CHALLENGE_UPDATE, { with = c.from, name = from and from.name or "", state = "expired" }, { to.presence })
            end
        end
    end
    for seq, p in pairs(state.pairings) do
        local gone = state.members[p.host] == nil and state.members[p.guest] == nil
        if gone or tick - p.tick > PAIRING_TIMEOUT_TICKS then
            end_pairing(state, seq)
        end
    end

    if state.dirty and tick - state.last_state_tick >= STATE_INTERVAL_TICKS then
        state.dirty = false
        state.last_state_tick = tick
        send(dispatcher, OP_STATE, { game = state.game, title = state.title, members = member_list(state) }, nil)
        update_label(dispatcher, state)
    end

    if next(state.members) == nil then
        state.empty_ticks = state.empty_ticks + 1
        if state.empty_ticks > EMPTY_TIMEOUT_TICKS then
            return nil
        end
    else
        state.empty_ticks = 0
    end
    return state
end

local function match_terminate(context, dispatcher, tick, state, grace_seconds)
    return nil
end

local function match_signal(context, dispatcher, tick, state, data)
    return state, data
end

-- ---------------------------------------------------------------------------
-- RPCs

local function read_channel(game)
    local records = nk.storage_read({ { collection = CHANNEL_COLLECTION, key = game, user_id = nil } })
    if #records == 0 then
        return nil, nil
    end
    local value = records[1].value
    return (type(value) == "table") and value.match_id or nil, records[1].version
end

local function list_channels(context, payload)
    local matches = nk.match_list(100, true, nil, 0, MAX_MEMBERS, "+label.kind:melonds-channel")
    local out = {}
    for _, m in ipairs(matches or {}) do
        local label = decode(m.label)
        if label ~= nil and label.kind == "melonds-channel" then
            out[#out + 1] = {
                match_id = m.match_id,
                game = label.game or "",
                title = label.title or "",
                members = tonumber(label.members) or m.size or 0,
                playing = tonumber(label.playing) or 0,
            }
        end
    end
    table.sort(out, function(a, b)
        if a.members ~= b.members then
            return a.members > b.members
        end
        return a.title < b.title
    end)
    return nk.json_encode({ channels = out })
end

local function join_channel(context, payload)
    local params = decode(payload) or {}
    local game = params.game
    if not valid_game_id(game) then
        error({ "That is not a game this server knows", 3 })
    end
    local title = clean_text(params.title, 64)
    if title == "" then
        title = game
    end

    local match_id, version = read_channel(game)
    if match_id ~= nil then
        local ok, match = pcall(nk.match_get, match_id)
        if ok and match ~= nil then
            return nk.json_encode({ match_id = match_id })
        end
    end

    -- none yet (or it ended): create it, unless someone else just did
    local new_id = nk.match_create("melonds_channel", { game = game, title = title })
    local ok = pcall(nk.storage_write, {
        {
            collection = CHANNEL_COLLECTION,
            key = game,
            user_id = nil,
            value = { match_id = new_id },
            version = version or "*",
            permission_read = 0,
            permission_write = 0,
        },
    })
    if not ok then
        -- lost the race: use the channel the other caller stored (ours ends,
        -- empty, after a minute)
        local other = read_channel(game)
        if other ~= nil then
            return nk.json_encode({ match_id = other })
        end
    end
    return nk.json_encode({ match_id = new_id })
end

nk.register_rpc(list_channels, "melonds_channels")
nk.register_rpc(join_channel, "melonds_channel_join")

return {
    match_init = match_init,
    match_join_attempt = match_join_attempt,
    match_join = match_join,
    match_leave = match_leave,
    match_loop = match_loop,
    match_terminate = match_terminate,
    match_signal = match_signal,
}
