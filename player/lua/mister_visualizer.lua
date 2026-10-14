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
