#ifndef WaddleAvGuestSetupH
#define WaddleAvGuestSetupH
/** @brief Verify OS, mapped IVSHMEM, capture adapter and active audio endpoint.
 * @return 0 ready, 1 missing/incompatible native capability with diagnostic.
 * @note Calling thread owns COM initialization/resources through return; no
 * retained handles or allocation. Does not install drivers or modify the VM.
 */
int av_guest_probe(void);
/** @brief Install the bundled signed IVSHMEM package using system pnputil.
 * @param[in] directory Nonnull borrowed UTF-8 absolute guest driver directory.
 * @return 0 installed/probe ready, 2 invalid path, 3 reboot required, 1 failure.
 * @note Requires guest administrator privileges. Owns/reaps child process, closes
 * handles; driver store/catalog validation is performed by Windows. No shell.
 */
int av_guest_setup(const char *directory);
/** @brief Verify the interactive primary display is 1920x1080 at nominal 144 Hz.
 * @return 0 selected mode ready, 1 unavailable/different with actionable diagnostic.
 * @note Caller thread, read-only Win32 display query; no parameters/allocation/
 * retained handles. Does not install or trust a driver, modify display topology,
 * claim WGC throughput or apply to a different Windows session.
 */
int av_guest_display_probe(void);
#endif
