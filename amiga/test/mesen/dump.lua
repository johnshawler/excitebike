-- per-frame hashes of CPU RAM / OAM / nametable RAM / palette, driven by a pad movie (one byte per frame, bits: A B Sel Start Up Down Left Right)
local padf = io.open(os.getenv("PADFILE"), "rb"); local pad = padf:read("a"); padf:close()
local maxf = tonumber(os.getenv("MAXF"))
local out = io.open(os.getenv("OUTFILE"), "w")
local cnt = 0
local want = {}; for w in string.gmatch(os.getenv("DUMPF") or "", "%d+") do want[tonumber(w)] = true end
local function hex(mt, n) local t = {}; for i = 0, n - 1 do t[#t + 1] = string.format("%02x", emu.read(i, mt)) end; return table.concat(t) end
local function fnv(mt, n)
  local h = 2166136261
  for i = 0, n - 1 do
    local a = i
    if mt == emu.memType.nesInternalRam and i >= 256 and i < 512 then a = -1 end      -- the stack page is not compared (garbage below SP)
    if mt == emu.memType.nesPaletteRam and (i == 16 or i == 20 or i == 24 or i == 28) then a = i - 16 end    -- sprite palette mirrors of the background colours
    if a >= 0 then h = ((h ~ emu.read(a, mt)) * 16777619) & 0xFFFFFFFF end
  end
  return h
end
local st = emu.getState()
out:write("region " .. tostring(st["nes.region"]) .. "\n")
emu.addEventCallback(function()
  local b = pad:byte(cnt + 1) or 0
  emu.setInput({a = b & 1 ~= 0, b = b & 2 ~= 0, select = b & 4 ~= 0, start = b & 8 ~= 0, up = b & 16 ~= 0, down = b & 32 ~= 0, left = b & 64 ~= 0, right = b & 128 ~= 0}, 0)
end, emu.eventType.inputPolled)
local armed = false
local function dumpnow()
  if not armed then return end
  armed = false
  out:write(string.format("%d %08x %08x %08x %08x\n", cnt, fnv(emu.memType.nesInternalRam, 2048), fnv(emu.memType.nesSpriteRam, 256), fnv(emu.memType.nesNametableRam, 2048), fnv(emu.memType.nesPaletteRam, 32)))
  if want[cnt] then out:write(string.format('RAM %d %s\nPAL %d %s\n', cnt, hex(emu.memType.nesInternalRam, 2048), cnt, hex(emu.memType.nesPaletteRam, 32))) end
  if cnt >= maxf then out:close(); emu.stop(0) end
end
emu.addMemoryCallback(dumpnow, emu.callbackType.exec, tonumber(os.getenv('NMIPC')))
emu.addEventCallback(function() cnt = cnt + 1; armed = true end, emu.eventType.endFrame)
