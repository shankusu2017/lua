local modname = ...
local M = {}
_G[modname] = M
package.loaded[modname] = M
setfenv(1, M)
--local print = print


function f1()
    print("f1")
end

function f2()
    print("f2")
    f1()
end

local function f3()
    print("f3")
    f2()
end
function f4()
    print("f4")
    f3()
end

f2()
print("$$$$$$$$$$$$$$$:")
--print("###################:", t_gbl)