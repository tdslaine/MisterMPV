-- Winamp-style shuffle: the playlist keeps its order, only the choice of the next entry is random
-- (mpv's own --shuffle reorders the list). Every entry plays once per round; with loop-playlist a
-- new round starts when all were played.
-- Switch: --script-opts=mister-shuffle=yes at start, `script-message mister-shuffle yes|no` live.
-- Navigation: `script-message mister-next` / `mister-prev` (plain playlist-next/prev without shuffle).
local shuffle = (mp.get_opt("mister-shuffle") == "yes")
local played = {}      -- index -> true for this round
local history = {}     -- indices in play order, for "previous"
local jumping = false  -- we triggered the change ourselves

math.randomseed(os.time())

local function count() return mp.get_property_number("playlist-count", 0) end
local function cur() return mp.get_property_number("playlist-pos", 0) end

local function pick()
    local n, c = count(), cur()
    played[c] = true
    local function candidates()
        local cand = {}
        for i = 0, n - 1 do if not played[i] then cand[#cand + 1] = i end end
        return cand
    end
    local cand = candidates()
    if #cand == 0 then
        local loop = mp.get_property("loop-playlist", "no")
        if loop == "no" or n < 2 then return nil end
        played = { [c] = true }
        cand = candidates()
    end
    return cand[math.random(#cand)]
end

local function jump(t)
    jumping = true
    mp.commandv("playlist-play-index", t)
end

mp.register_event("start-file", function()
    local c = cur()
    played[c] = true
    if not jumping and history[#history] ~= c then history[#history + 1] = c end
    if jumping then history[#history + 1] = c end
    jumping = false
end)

mp.register_event("end-file", function(ev)
    if shuffle and ev.reason == "eof" then
        local t = pick()
        if t then jump(t) else mp.commandv("quit") end
    end
end)

mp.register_script_message("mister-next", function()
    if shuffle then
        local t = pick()
        if t then jump(t) end
    else
        mp.commandv("playlist-next")
    end
end)

mp.register_script_message("mister-prev", function()
    if shuffle and #history > 1 then
        table.remove(history)                 -- drop the current entry
        local t = table.remove(history)       -- previous one (start-file re-adds it)
        if t and t < count() then jump(t) end
    else
        mp.commandv("playlist-prev")
    end
end)

mp.register_script_message("mister-shuffle", function(v)
    shuffle = (v == "yes" or v == "true")
    played = { [cur()] = true }
end)
