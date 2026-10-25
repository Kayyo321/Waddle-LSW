# Native foreign-window lifecycle gate

Exact commit48555db2a3ff4c4ea5f1b56babbbadc2b1ce0aee passed graph workflow
40210422085, Linux job121220270718 and actual Windows job121223242266.
Windows job output includes the real foreign admission/parent/title/quarantine/
generation acceptance line. Raw job log is preserved compressed alongside exact
job/step metadata. SHA256 of uncompressed log: b85eccd1b400a08a01f90e0b9ace019a55187e8e361352cc2eb0c96015d483b3

This validates this bounded real-native fixture on the Windows CI desktop.
It does not qualify an installed guest agent, signed UIAccess, capture/slot/wire
coordinator, VSOCK/IVSHMEM driver or GNOME/KDE/Sway acceptance. Native transport
workflow at6083a9f failed WSAPoll separately and is not reported as green.
No additional feature completion is assigned from this evidence alone.
