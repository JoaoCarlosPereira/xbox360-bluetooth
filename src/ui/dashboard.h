#ifndef BT_DASHBOARD_H
#define BT_DASHBOARD_H

#include "../manager/bt_manager.h"

void dashboard_draw(const bt_ipc_state_t *ipc, const bt_ui_model_t *ui,
                    const bt_remap_wizard_t *wizard);

#endif
