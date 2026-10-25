# Native WSAPoll input correction

Exact6083a9f CI40209702182 Windows job121222389159 failed in the actual
AF_INET fixture: WSAPoll returned WSAEINVAL10022. The Linux job passed.
The owner incorrectly requested POLLERR|POLLHUP|POLLNVAL as input interests.
Microsoft documents those as returned status; unsupported input values fail:
https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-wsapoll

Only POLLRDNORM/POLLWRNORM are now requested; drive still accepts the existing
error/hangup/invalid revents. Linux and native fixture both assert the input
mask. Exact root qualification:842/873 production lines,627/695 taken arcs,
no exclusions; address/leak/undefined sanitizers pass with leak detection.
Strict normal build and Windows SDK link pass. Native rerun remains required.
The earlier native failure earns no credit and is not reported as green.
