local t1 = {}
local t2 = {}
setmetatable(t1, t2)
print("type(t1.getmetatable())", type(getmetatable(t1)))

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


ff3()
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

