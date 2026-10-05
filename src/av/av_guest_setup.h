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
#endif
