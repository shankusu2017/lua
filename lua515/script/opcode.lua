local a = 0
local b = 1
local c = a + b

local ret1, ret2 = math.random(100)

print("hello lua")
local tbl = {n1 = 1, n2 = 2, n3 =3,}
for _ in pairs(tbl) do
    print("i'm loop tbl")
end

for n in pairs(_G) do
    print(n)
end

a = 1
setfenv(1, {})

