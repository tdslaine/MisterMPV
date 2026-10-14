-- Audio visualizer for audio-only files (mp3, flac, ...). Picks a lavfi-complex graph while the
-- current file has no real video (cover art does not count) and clears it again for video files.
-- Mode: --script-opts=mister-vis-mode=<name> at start, or `script-message mister-vis <name>` live.
local graphs = {
    spectrum = "[aid1]asplit[ao][a];[a]showcqt=s=640x360:fps=30:axis=0:bar_g=2:sono_g=2:count=4,format=yuv420p[vo]",
    waves = "[aid1]asplit[ao][a];[a]showwaves=s=640x360:mode=cline:rate=30:colors=0x40ff80|0xff9040,format=yuv420p[vo]",
    vectorscope = "[aid1]asplit[ao][a];[a]avectorscope=s=360x360:zoom=1.6:rate=30:rc=0:gc=220:bc=40:rf=0:gf=40:bf=0,format=yuv420p[vo]",
    spectrogram = "[aid1]asplit[ao][a];[a]showspectrum=s=640x360:slide=scroll:mode=combined:color=rainbow:scale=log:fps=30,format=yuv420p[vo]",
    bars = "[aid1]asplit[ao][a];[a]showfreqs=s=640x360:mode=bar:fscale=log:ascale=sqrt:rate=30:colors=0x40ff80|0xff9040,format=yuv420p[vo]",
}

local mode = mp.get_opt("mister-vis-mode") or "off"

local function tracks()
    local has_audio, has_video = false, false
    for _, t in ipairs(mp.get_property_native("track-list", {})) do
        if t.type == "audio" then has_audio = true end
        if t.type == "video" and not t.albumart and not t.image then has_video = true end
    end
    return has_audio, has_video
end

local function apply()
    local has_audio, has_video = tracks()
    local g = graphs[mode]
    if g and has_audio and not has_video then
        mp.set_property("lavfi-complex", g)
    elseif mp.get_property("lavfi-complex", "") ~= "" then
        mp.set_property("lavfi-complex", "")
    end
end

mp.register_event("file-loaded", apply)
mp.register_script_message("mister-vis", function(m)
    mode = m
    apply()
end)

-- ---------------------------------------------------------------------------------------------
-- Permanent artist / title overlay for music (audio-only files). Same size as the OSD text
-- (osd-font-size, real pixels). `--script-opts=mister-track=yes|no,mister-track-pos=top|bottom`
-- at start, or live: `script-message mister-track yes|no [top|bottom]`.
local track_on = (mp.get_opt("mister-track") == "yes")
local track_pos = mp.get_opt("mister-track-pos") or "bottom"
local overlay = mp.create_osd_overlay("ass-events")

local function ass_escape(s)
    return (s:gsub("\\", "\\\\"):gsub("{", "\\{"):gsub("}", "\\}"))
end

local function strip_ext(name)
    return (name:gsub("%.[%w]+$", ""))
end

local function update_track()
    local has_audio, has_video = tracks()
    if not track_on or not has_audio or has_video then
        overlay:remove()
        return
    end
    local md = mp.get_property_native("metadata", {}) or {}
    local artist = md.artist or md.ARTIST or md.Artist or md.album_artist
    local title = md.title or md.TITLE or md.Title
    if not title or title == "" then
        local fname = mp.get_property("filename", "")
        title = strip_ext(fname)
    end
    local w, h = mp.get_osd_size()
    if not w or w <= 0 then return end
    overlay.res_x, overlay.res_y = w, h
    local fs = mp.get_property_number("osd-font-size", 28)
    local an = (track_pos == "top") and 8 or 2
    local margin = math.max(4, math.floor(h * 0.04))
    local text = string.format("{\\an%d\\fs%d\\b1\\bord2.5\\shad0\\blur0\\1c&HFFFFFF&\\3c&H000000&\\pos(%d,%d)}",
        an, fs, math.floor(w / 2), (an == 8) and margin or (h - margin))
    if artist and artist ~= "" then
        text = text .. ass_escape(artist) .. "\\N" .. ass_escape(title)
    else
        text = text .. ass_escape(title)
    end
    overlay.data = text
    overlay:update()
end

mp.register_event("file-loaded", update_track)
mp.observe_property("metadata", "native", update_track)
mp.observe_property("osd-dimensions", "native", update_track)
mp.observe_property("osd-font-size", "number", update_track)
mp.register_script_message("mister-track", function(on, pos)
    track_on = (on == "yes" or on == "true")
    if pos and pos ~= "" then track_pos = pos end
    update_track()
end)
