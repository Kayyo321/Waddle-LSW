/** @file opencl_fls_certificate_fake.h @brief Test-only FLS API declarations. */
#ifndef WaddleOpenclFlsCertificateFakeH
#define WaddleOpenclFlsCertificateFakeH
#include "opencl_slot_certificate_fake.h"
#define FLS_OUT_OF_INDEXES UINT32_MAX
static DWORD FlsAlloc(void *);
static BOOL FlsFree(DWORD);
#endif
