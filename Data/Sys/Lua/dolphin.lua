require "jit.dump".on()

local dolphin = {}
_G.dolphin = dolphin

local ffi = require "ffi"

ffi.cdef [=[
// 1 << Event
struct Dolphin_Event {
	static const uint16_t STOP = 0;
	static const uint16_t EVALUATE = 1;
	static const uint16_t FRAME = 2;
	static const uint16_t INVALID = 256;
};

void Dolphin_AddEventMask(uint16_t event);
void Dolphin_RemoveEventMask(uint16_t event);
uint16_t Dolphin_Wait(uint64_t timeout_ms);

const char *Dolphin_Evaluate_Script;

enum Message_Type {
  Information, Question, Warning, Critical
};

bool (*Dolphin_MsgAlert)(bool yes_no, int Style, const char *format, ...);

struct Log_Level {
	static const unsigned NOTICE = 1;   // VERY important information that is NOT errors. Like startup and OSReports.
	static const unsigned ERROR = 2;    // Critical errors
	static const unsigned WARNING = 3;  // Something is suspicious.
	static const unsigned INFO = 4;     // General information.
	static const unsigned DEBUG = 5;    // Detailed debugging - might make things slow.
};

void Dolphin_Log(int level, const char *text);

bool (*Dolphin_Mem_IsRamAddress)(uint32_t address);

uint8_t (*Dolphin_Mem_Read8)(uint32_t address);
uint16_t (*Dolphin_Mem_Read16)(uint32_t address);
uint32_t (*Dolphin_Mem_Read32)(uint32_t address);
uint64_t (*Dolphin_Mem_Read64)(uint32_t address);

void (*Dolphin_Mem_Write8)(uint8_t value, uint32_t address);
void (*Dolphin_Mem_Write16)(uint16_t value, uint32_t address);
void (*Dolphin_Mem_Write32)(uint32_t value, uint32_t address);
void (*Dolphin_Mem_Write64)(uint64_t value, uint32_t address);

void (*Dolphin_Mem_InvalidateICache)(uint32_t address, uint32_t size, bool forced);
]=]

local events = ffi.new("struct Dolphin_Event")

local eventHandlers = {}

local function makeEventHandler(name, eventId, eventArgsFunc)
	eventHandlers[eventId] = {
		id = eventId,
		name = name,
		listeners = {},
		eventArgsFunc = eventArgsFunc, -- Function that creates the arguments that are sent to listeners
	}
end

local function addEventMask(event)
	ffi.C.Dolphin_AddEventMask(event)
end

local function removeEventMask(event)
	ffi.C.Dolphin_RemoveEventMask(event)
end

local function addListener(eventHandler, func)
	eventHandler.listeners[#eventHandler.listeners+1] = func
	if #eventHandler.listeners == 1 then
		addEventMask(eventHandler.id)
	end
	return function()
		for i=1, #eventHandler.listeners do
			if eventHandler.listeners[i] == func then
				table.remove(eventHandler.listeners, i)
				break
			end
		end
		if #eventHandler.listeners == 0 then
			removeEventMask(eventHandler.id)
		end
	end
end

local function fireEvent(eventHandler)
	local listenersCopy = {}
	for i=1, #eventHandler.listeners do
		listenersCopy[i] = eventHandler.listeners[i]
	end
	local args
	if eventHandler.eventArgsFunc then
		args = eventHandler.eventArgsFunc()
	else
		args = {}
	end
	for i=1, #listenersCopy do
		listenersCopy[i](unpack(args))
	end
end

makeEventHandler("frame", events.FRAME)

local queue = {}

function dolphin.post(func)
	queue[#queue+1] = func
end

function dolphin.main()
	-- Load user scripts and init things
	dofile("script.lua")
	
	-- Run the main loop
	while true do
		local wait = -1 -- Normally, we want to block in Dolphin_Wait
		if queue[1] then -- If the post queue has items, run Dolphin_Wait in non-blocking(-ish) mode
			wait = 0
		end
		local event = ffi.C.Dolphin_Wait(wait)
		
		if event < events.INVALID then
			-- Valid event
			if event == events.STOP then
				break
			end
			
			if event == events.EVALUATE then
				-- Evaluate some code
				local evalCStr = ffi.C.Dolphin_Evaluate_Script -- Will be freed by the event creator during next call to Dolphin_Wait
				if evalCStr == nil then
					print("Null string in evaluate, Dolphin will now crash.")
				end
				local func, err = loadstring(ffi.string(evalCStr))
				if not func then
					dolphin.alert("%s", err)
				else
					local s, err = pcall(func)
					if not s then
						dolphin.alert("%s", err)
					end
				end
			else
				fireEvent(eventHandlers[event])
			end
		else
			-- Invalid event, run the post queue.
			-- The post queue runs while the rest of Dolphin is running.
			-- However, taking too long in the post queue can cause Dolphin to lock up for periods of time.
			-- This is because of the blocking behavior of Lua::Signal.
			-- The post queue may be changed later to allow scheduling timed events.
			if queue[1] then
				local oldQueue = queue
				queue = {}
				for i=1, #oldQueue do
					oldQueue[i]()
				end
			end
		end
	end
end

local function makeEventListenFunc(event)
	local eventHandler = eventHandlers[event]
	return function(...)
		return addListener(eventHandler, ...)
	end
end

dolphin.onFrame = makeEventListenFunc(events.FRAME)

-- Dolphin Message Alert API
do
	-- Cache the function pointer
	local Dolphin_MsgAlert = ffi.C.Dolphin_MsgAlert

	function dolphin.alert(...)
		-- Essentially PanicAlert
		Dolphin_MsgAlert(false, ffi.C.Warning, ...)
	end
end

-- Dolphin Logging API
do
	dolphin.logLevel = ffi.new("struct Dolphin_Log")

	local logLevel = dolphin.logLevel

	dolphin.rawlog = ffi.C.Dolphin_Log

	function dolphin.log(format, ...)
		ffi.C.Dolphin_Log(logLevel.NOTICE, format:format(...))
	end

	function dolphin.logi(format, ...)
		ffi.C.Dolphin_Log(logLevel.INFO, format:format(...))
	end

	function dolphin.logw(format, ...)
		ffi.C.Dolphin_Log(logLevel.WARNING, format:format(...))
	end

	function dolphin.loge(format, ...)
		ffi.C.Dolphin_Log(logLevel.ERROR, format:format(...))
	end
end

-- Print wrapper
function print(...)
	local args = {...}
	for i=1, #args do
		args[i] = tostring(args[i])
	end
	dolphin.log("%s", table.concat(args, "\t"))
end

-- Dolphin Memory API
do
	dolphin.memory = {}
	
	dolphin.memory.validAddress = ffi.C.Dolphin_Mem_IsRamAddress

	dolphin.memory.readU8 = ffi.C.Dolphin_Mem_Read8
	dolphin.memory.readU16 = ffi.C.Dolphin_Mem_Read16
	dolphin.memory.readU32 = ffi.C.Dolphin_Mem_Read32
	dolphin.memory.readU64 = ffi.C.Dolphin_Mem_Read64
	
	local writeU8 = ffi.C.Dolphin_Mem_Write8
	local writeU16 = ffi.C.Dolphin_Mem_Write16
	local writeU32 = ffi.C.Dolphin_Mem_Write32
	local writeU64 = ffi.C.Dolphin_Mem_Write64
	
	-- Dolphin's value and address parameter placement is quite weird
	
	function dolphin.memory.writeU8(address, value)
		writeU8(value, address)
	end
	
	function dolphin.memory.writeU16(address, value)
		writeU16(value, address)
	end
	
	function dolphin.memory.writeU32(address, value)
		writeU32(value, address)
	end
	
	function dolphin.memory.writeU64(address, value)
		writeU64(value, address)
	end
	
	local invalidateICache = ffi.C.Dolphin_Mem_InvalidateICache
	
	function dolphin.memory.invalidateICache(address, size, force)
		if force == nil then force = false end
		invalidateICache(address, size, force)
	end
end

if false then
	-- Game ID test code
	local gameId = {}
	for i=1, 6 do
		gameId[i] = string.char(dolphin.memory.readU8(i-1 + 0x80000000))
	end
	dolphin.alert(table.concat(gameId))
end

return dolphin
