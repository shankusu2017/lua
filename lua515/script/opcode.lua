local t1 = {name = "t1"}
local t2 = {name = "t2"}
setmetatable(t1, t2)
print("getmetatable(t1).name:", getmetatable(t1).name)

do
	t_gbl = "this is a t_gal"--
	--test = "i'm global variable"
	local t = require("test")
	--f2()
	t.f2()
	t.f4()
	--f2()
	--t.f3()
end

function ff(x, y)
    print("hello world in func(ff())")
end

function ff()
	print("hello in ff()")
	local str = debug.traceback("debug.traceback")
	print(str)
end

function ff2()
	ff()
end


function ff3()
	ff2()
end

for line in io.lines("tmp.txt") do 
	print(line) 
end

-- ff3()
-- local a = tonumber("3")
-- local a = 0
-- local b = 1
-- local c = a + b

-- local ret1, ret2 = math.random(100)

-- print("hello lua")
-- local tbl = {n1 = 1, n2 = 2, n3 =3,}
-- for _ in pairs(tbl) do
--     print("i'm loop tbl")
-- end

-- for n in pairs(_G) do
--     print(n)
-- end

-- a = 1
-- setfenv(1, {})

