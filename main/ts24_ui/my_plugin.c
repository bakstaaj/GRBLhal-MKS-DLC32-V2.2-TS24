/* Optional grblHAL plugin entry point for the MKS TS24-R UI. */

#include "ts24_ui.h"

void my_plugin_init(void)
{
    mks_ts24_ui_init();
}
