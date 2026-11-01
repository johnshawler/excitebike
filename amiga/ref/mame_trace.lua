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
local pxout = io.open(os.getenv("PIX_OUT") or "pix.bin","wb")
local colmap, ncol = {}, 0
local function want_pix(f) return (f>=495 and f<=530) or (f%10==0 and f>=100) end
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
local iscr = {}
do
  local fn = os.getenv("INPUT_SCRIPT")
  if fn then
    for line in io.lines(fn) do
      if line:sub(1,1) ~= "#" then
        local a,b,m,p,o = line:match("^(%d+)%s+(%d+)%s+(%x+)%s+(%d+)%s+(%d+)")
        if a then iscr[#iscr+1] = {tonumber(a),tonumber(b),tonumber(m,16),tonumber(p),tonumber(o)} end
      end
    end
  end
end
local names = {[1]="P1 A",[2]="P1 B",[4]="P1 Select",[8]="P1 Start",[16]="P1 Up",[32]="P1 Down",[64]="P1 Left",[128]="P1 Right"}
emu.register_frame_done(function()
  frame = frame + 1
  -- input
  local f = frame
  if #iscr > 0 then
    for bit,name in pairs(names) do
      local on = false
      for _,e in ipairs(iscr) do if f>=e[1] and f<e[2] and (e[4]==0 or (f % e[4]) < e[5]) and (e[3] & bit) ~= 0 then on = true end end
      press(name, on)
    end
    goto skipold
  end
  press("P1 Start", f>=start_f and f<start_f+4)
  press("P1 A", f>=start_f+10 and (f%7)~=0)      -- A: accelerate
  press("P1 Right", f>=start_f+300 and (f%40)<20)
  press("P1 Up", f>=start_f+200 and (f%97)<5)
  press("P1 Down", f>=start_f+260 and (f%89)<5)
  press("P1 B", f>=start_f+120 and (f%3)==0)
  ::skipold::
  -- pixels: one byte per pixel = index of first appearance of the RGB value; the RGB table is appended on request
  if want_pix(frame) then
    local px = scr:pixels()
    local w, h = scr.width, scr.height
    local t = {}
    for i = 1, #px, 4 do
      local key = px:sub(i,i+2)
      local id = colmap[key]
      if not id then ncol = ncol + 1; id = ncol; colmap[key] = id; out:write(string.format("COL %d %02X%02X%02X\n", id, px:byte(i+2), px:byte(i+1), px:byte(i))) end
      t[#t+1] = string.char(id)
    end
    out:write(string.format("PIX %d %d %d %d\n", frame, w, h, #px))
    pxout:write(string.char(frame % 256, frame // 256)); pxout:write(table.concat(t))
  end
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
