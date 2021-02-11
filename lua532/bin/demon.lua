local t = {}
t[6] = 6;
table.insert(t, 1, 3)
for k,v in pairs(t) do
    print("key:", k, ",\tvalue:", v)
end

function mt()
    local mt = {__metatable = {},}
    local tb = {}
    setmetatable(tb, mt)
    local mt2 = {}
    setmetatable(tb, mt2)
end

mt();



print("this is lua scrpit ouput")
local t = math.floor(34.3)
print("global_lua_var:", global_lua_var)

function lua_fun(arg1, arg2 ,arg3)
    print("arg1:", arg1)
    print("arg2:", arg2)
    print("arg3:", arg3)
    return -10, -20, -30
end

local val = 5.1
print(type(val))
val = 5.0
print(type(val))
val = 5
print(type(val))

function test()
    local t = {a = 1, b = 2, c = 3, d = 4,}
    local a = 3 + 4
    local b = 3
    local c = a + b
    local d = 2.1 + 3
    local e = 1
    local f = e + d
    print("ffffff:", f)
    math.abs(-2147483648)
end

width = 3;
height = 4;


test()
print("nowSec:", os.time())
