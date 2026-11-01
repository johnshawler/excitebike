-- MAME reference trace for NES Excitebike (Europe), PAL.  Records every CPU write to PPU/APU/controller ($2000-$2007,$4000-$4017),
-- every read of $2002/$4016/$4017 (with cpu cycle stamp) and per frame: nametable RAM, palette RAM, OAM (via $4014 DMA source)
-- Usage: mame nespal -cart exb.nes -video none -sound none -nothrottle -seconds_to_run N -autoboot_script mame_trace.lua
local out = assert(io.open(os.getenv("TRACE_OUT") or "trace.txt","w"))
local m = manager.machine
local cpu = m.devices[":maincpu"]
local mem = cpu.spaces["program"]
local scr = m.screens[":screen"]
local ppu = m.devices[":ppu"]
local frame = 0
local function cyc() return cpu.state["CURCYCLES"] and 0 or 0 end
local function now() return math.floor(m.time:as_double()*1662607+0.5) end
local function wcb(off,data,mask) out:write(string.format("W %d %d %04X %02X\n",frame,now(),off,data)) end
local function rcb(off,data,mask) out:write(string.format("R %d %d %04X %02X\n",frame,now(),off,data)) end
tp1 = mem:install_write_tap(0x2000,0x2007,"ppuw",wcb)
tp2 = mem:install_write_tap(0x4000,0x4017,"apuw",wcb)
tp3 = mem:install_read_tap(0x2002,0x2002,"ppur",rcb)
tp4 = mem:install_read_tap(0x4016,0x4017,"inr",rcb)
-- scripted input: frame ranges -> buttons
local script = {
  {0,600,{}},                       -- ~12 s attract/title (no input)
}
local function press(name,on)
  for t,p in pairs(m.ioport.ports) do for n,f in pairs(p.fields) do if n==name then f:set_value(on and 1 or 0) end end end
end
local start_f = tonumber(os.getenv("START_FRAME") or "600")
emu.register_frame_done(function()
  frame = frame + 1
  -- input
  local f = frame
  press("P1 Start", f>=start_f and f<start_f+4)
  press("P1 A", f>=start_f+10 and (f%7)~=0)      -- A: accelerate
  press("P1 Right", f>=start_f+300 and (f%40)<20)
  press("P1 Up", f>=start_f+200 and (f%97)<5)
  press("P1 Down", f>=start_f+260 and (f%89)<5)
  press("P1 B", f>=start_f+120 and (f%3)==0)
  -- dump state
  local vs = ppu.spaces["videoram"]
  if vs then
    local t = {}
    for a=0x2000,0x2fff do t[#t+1]=string.format("%02X",vs:read_u8(a)) end
    out:write(string.format("NT %d %s\n",frame,table.concat(t)))
    t={}
    for a=0x3f00,0x3f1f do t[#t+1]=string.format("%02X",vs:read_u8(a)) end
    out:write(string.format("PAL %d %s\n",frame,table.concat(t)))
  end
  local t={}
  for a=0x0200,0x02ff do t[#t+1]=string.format("%02X",mem:read_u8(a)) end
  out:write(string.format("OAMRAM %d %s\n",frame,table.concat(t)))
  t={}
  for a=0,0x7ff do t[#t+1]=string.format("%02X",mem:read_u8(a)) end
  out:write(string.format("RAM %d %s\n",frame,table.concat(t)))
end)
