/** @file av_guids.c @brief Emit system-SDK COM GUID definitions for the C boundary.
 * SDK declarations are linked here because MinGW libuuid omits WASAPI GUIDs.
 * No borrowed pointers, heap ownership, mutable state or threading restrictions.
 */
#include <initguid.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
