for k, v in pairs(wdye) do _G[k] = v end

-- The terminal echoes back, we don't want to read the same stuff twice.
local cat = spawn {"sh", "-c", "cat > /dev/null", environ={TERM="xterm"}}
assert(cat, "failed to spawn process")
assert(cat.term.key_left, "bad terminfo")

cat:send("Hello\r")
local m = expect(cat:exact {"Hello\r", function (p) return p[0] end})
assert(m == "Hello\r", "exact match failed, or value expansion mismatch")

local t = table.pack(expect(timeout {.5, 42}))
assert(#t == 1 and t[1] == 42, "timeout match failed, or value mismatch")

cat:send("abc123\r")
expect(cat:regex {"A(.*)3", nocase=true, function (p)
	assert(p[0] == "abc123", "wrong regex group #0")
	assert(p[1] == "bc12", "wrong regex group #1")
end})

assert(not cat:wait (true), "process reports exiting early")

-- Send EOF (^D), test method chaining.
cat:send("Closing...\r"):send("\004")
local v = expect(cat:eof {true},
	cat:default {.5, function (p) error "expected EOF, got a timeout" end})

assert(cat.pid > 0, "process has no ID")
local s1, exit, signal = cat:wait ()
assert(s1 == 0 and exit == 0 and not signal, "unexpected exit status")
assert(cat.pid < 0, "process still has an ID")
local s2 = cat:wait (true)
assert(s1 == s2, "exit status not remembered")
