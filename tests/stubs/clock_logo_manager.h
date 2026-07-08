#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOCK_LOGO_RESTORE_OK = 0,
    CLOCK_LOGO_RESTORE_BUSY,
    CLOCK_LOGO_RESTORE_STORAGE_ERROR,
} clock_logo_restore_result_t;

clock_logo_restore_result_t clock_logo_restore_compiled_default(void);

#ifdef __cplusplus
}
#endif
