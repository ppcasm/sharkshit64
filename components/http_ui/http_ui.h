#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void http_ui_task(void *arg);
bool load_sta_credentials(wifi_config_t *sta_config);

#ifdef __cplusplus
}
#endif
