-- Memory corruptor!
-- Corrupts a random byte in memory every frame.

-- if true then return end

local LOW_MEM =  0x81000000
local HIGH_MEM = 0x817FFFFF
local MEM_RANGE = HIGH_MEM-LOW_MEM

dolphin.alert("Memory corrupter loaded!")

math.randomseed(os.clock() + os.time())

dolphin.onFrame(function()
	local addr
	repeat
		addr = math.floor(math.random()*MEM_RANGE+LOW_MEM)
	until dolphin.memory.validAddress(addr)

	local oldByte = dolphin.memory.readU8(addr)
	local newByte = (oldByte + math.floor((math.random()-0.5) * 2 * 5)) % 256
	dolphin.memory.writeU8(addr, newByte)
end)
