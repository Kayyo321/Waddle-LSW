# Windows controller unit qualification

Root independently reran the exact source, header, private fixture and negative
API doubles:442/445 production lines99.33%,436/482 taken arcs90.46%, no source
exclusions. The same C unit cases pass fail-fast AddressSanitizer, LeakSanitizer
(detect_leaks=1) and UndefinedBehaviorSanitizer. MinGW strict SDK syntax passes.
Inputs, commands, tool versions, raw gcov and missed sites are retained.
Normal Make unit executable and production guest executable cross-link pass.

These unit doubles intentionally isolate error/ownership transitions. They do
not establish runtime composition: their map/socket stand-ins and Ready/header
results do not replace the separate native integration fixture. That fixture
is now built/uploaded/executed by Windows CI with actual WinSock, production
opaque mapping owner, real2GiB virtual memory, actual peer and layout ABI.
No native composition or driver acceptance has yet been assigned in this commit.
